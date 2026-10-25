// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/formats/wt_surge.h>

#include <cstring>

namespace o3os::formats::wt {

namespace {

constexpr std::size_t kHeader = 12;
constexpr std::uint16_t kFlagIsSample = 1, kFlagLoop = 2, kFlagInt16 = 4, kFlagInt16Is16 = 8, kFlagUserModified = 0x20;

std::uint32_t le32(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint16_t le16(const std::uint8_t* p) noexcept { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }

std::int16_t from_float(const std::uint8_t* p) noexcept {
  const std::uint32_t bits = le32(p);
  float x;
  std::memcpy(&x, &bits, sizeof x);
  if (!(x == x)) {
    return 0;  // NaN
  }
  float v = x * 32767.0f;
  v = v > 32767.0f ? 32767.0f : (v < -32767.0f ? -32767.0f : v);
  return static_cast<std::int16_t>(v >= 0.0f ? v + 0.5f : v - 0.5f);
}

}  // namespace

void SurgeReader::fail(FormatError e, const char* detail) noexcept {
  state_ = State::Failed;
  progress_ = formats::Progress::Failed;
  report_.error = e;
  report_.detail = detail;
  report_.offset = report_.bytes;
}

void SurgeReader::begin(core::ByteSource& source, model::ModelArena& arena, const ReadParams& params) noexcept {
  report_ = FormatReport{};
  source_ = &source;
  project_ = &arena.project;
  slot_ = params.index;
  pcm_ = params.pcm;
  capacity_ = params.pcm_capacity;
  have_ = 0;
  out_ = 0;
  total_ = 0;
  name_[0] = '\0';
  if (params.name != nullptr) {
    std::strncpy(name_, params.name, sizeof name_ - 1);
    name_[sizeof name_ - 1] = '\0';
  }
  state_ = State::Header;
  progress_ = formats::Progress::Running;
  if (params.index >= model::kMaxInstruments) {
    fail(FormatError::LimitExceeded, "the instrument slot does not exist");
  } else if (params.pcm == nullptr || params.pcm_capacity == 0) {
    fail(FormatError::LimitExceeded, "no room was given for the audio");
  }
}

unsigned SurgeReader::permille() const noexcept {
  if (progress_ == formats::Progress::Done) {
    return 1000;
  }
  if (state_ != State::Data || total_ == 0) {
    return 0;
  }
  const unsigned p = static_cast<unsigned>((static_cast<std::uint64_t>(out_) * 1000u) / total_);
  return p > 999u ? 999u : p;
}

void SurgeReader::cancel() noexcept {
  if (progress_ == formats::Progress::Running) {
    fail(FormatError::Cancelled, "cancelled");  // nothing was applied to the model
  }
}

void SurgeReader::on_header() noexcept {
  if (std::memcmp(buf_, "vawt", 4) != 0) {
    fail(FormatError::BadMagic, "not a Surge wavetable (no 'vawt' tag)");
    return;
  }
  n_samples_ = le32(buf_ + 4);
  n_tables_ = le16(buf_ + 8);
  flags_ = le16(buf_ + 10);
  if (n_samples_ == 0u || n_tables_ == 0u) {
    fail(FormatError::Malformed, "a wavetable with no samples or no tables");
    return;
  }
  if (n_samples_ < 32u || n_samples_ > 2048u || (n_samples_ & (n_samples_ - 1u)) != 0u) {
    fail(FormatError::Unsupported, "a frame size the engine has no pitch table for (32..2048, a power of two)");
    return;
  }
  total_ = n_samples_ * n_tables_;  // at most 2048 x 65535: fits 32 bits
  if (total_ > capacity_) {
    fail(FormatError::LimitExceeded, "the audio does not fit the sample buffer");
    return;
  }
  width_ = (flags_ & kFlagInt16) != 0u ? 2 : 4;
  have_ = 0;
  state_ = State::Data;
}

// `bytes` bytes of buf_ are whole samples: convert them to PCM.
void SurgeReader::convert(std::size_t bytes) noexcept {
  const bool is16 = (flags_ & kFlagInt16Is16) != 0u;
  for (std::size_t i = 0; i + width_ <= bytes && out_ < total_; i += width_) {
    std::int16_t v;
    if (width_ == 4) {
      v = from_float(buf_ + i);
    } else {
      v = static_cast<std::int16_t>(le16(buf_ + i));
      if (!is16) {
        const int d = static_cast<int>(v) * 2;  // the 15-bit form: full scale at +-16384 (ASSUMED, see the header)
        v = static_cast<std::int16_t>(d > 32767 ? 32767 : (d < -32768 ? -32768 : d));
      }
    }
    pcm_[out_++] = v;
  }
}

formats::Progress SurgeReader::step(std::size_t byte_budget) noexcept {
  if (progress_ != formats::Progress::Running) {
    return progress_;
  }
  std::size_t budget = byte_budget == 0 ? 1 : byte_budget;
  while (budget > 0 && state_ != State::Done && state_ != State::Failed) {
    std::size_t want;
    std::uint8_t* where;
    if (state_ == State::Header) {
      want = kHeader - have_;
      where = buf_ + have_;
    } else {
      const std::uint64_t left_bytes = static_cast<std::uint64_t>(total_ - out_) * width_ - have_;
      want = left_bytes > sizeof buf_ - have_ ? sizeof buf_ - have_ : static_cast<std::size_t>(left_bytes);
      where = buf_ + have_;
    }
    want = want > budget ? budget : want;
    const core::IoResult r = source_->read(where, want);
    if (r.status == core::IoStatus::WouldBlock) {
      return progress_;
    }
    if (r.status == core::IoStatus::End) {
      fail(FormatError::Truncated, state_ == State::Header ? "the file ends inside the header" : "the audio is cut short");
      return progress_;
    }
    if (r.status == core::IoStatus::Failed || r.count == 0 || r.count > want) {
      fail(FormatError::SourceFailed, "the source failed or broke its contract");
      return progress_;
    }
    budget -= r.count;
    report_.bytes += static_cast<std::uint32_t>(r.count);
    have_ = static_cast<std::uint8_t>(have_ + r.count);
    if (state_ == State::Header) {
      if (have_ == kHeader) {
        on_header();
      }
    } else {
      const std::size_t whole = (have_ / width_) * width_;
      convert(whole);
      const std::size_t rest = have_ - whole;
      for (std::size_t i = 0; i < rest; ++i) {
        buf_[i] = buf_[whole + i];  // a sample split across two reads
      }
      have_ = static_cast<std::uint8_t>(rest);
      if (out_ == total_) {
        finish();
      }
    }
  }
  return progress_;
}

void SurgeReader::finish() noexcept {
  wav::Info info;
  info.sample_rate = 44100;
  info.frames = total_;
  info.channels = 1;
  info.source_channels = 1;
  info.bits = 16;
  info.wt_frame = static_cast<std::uint16_t>(n_samples_);
  model::Instrument inst;
  wav::make_sample_instrument(info, slot_, name_, inst, static_cast<std::uint16_t>(n_samples_));  // an explicit size: the file's own
  if (project_->set_instrument(slot_, inst) != model::Status::Ok) {
    fail(FormatError::Internal, "the model refused the instrument");
    return;
  }
  report_.instruments = 1;
  report_.pcm_frames = total_;
  report_.pcm_channels = 1;
  report_.sanitised = ((flags_ & kFlagIsSample) != 0u ? 1u : 0u) + ((flags_ & kFlagLoop) != 0u ? 1u : 0u) + ((flags_ & kFlagUserModified) != 0u ? 1u : 0u);
  state_ = State::Done;
  progress_ = formats::Progress::Done;
}

}  // namespace o3os::formats::wt
