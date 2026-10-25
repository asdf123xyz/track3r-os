// SPDX-License-Identifier: GPL-3.0-or-later
//
// The source of an exported WAV (app/export_source.h).
#include <o3os/app/export_source.h>

#include <o3os/app/save_service.h>
#include <o3os/core/audio_clock.h>

#include <cstring>

namespace o3os::app {

namespace {

constexpr std::uint64_t gcd64(std::uint64_t a, std::uint64_t b) noexcept {
  while (b != 0u) {
    const std::uint64_t t = a % b;
    a = b;
    b = t;
  }
  return a;
}

// One output frame of the file lasts kOutCps CPU cycles (180 MHz / 44,100 Hz as a rational), one frame of the engine kRefCyclesPerSample: an output frame advances the input's time line by
// kOutCps / kRefCyclesPerSample input frames. As a fraction in lowest terms: 1 / 1 for the default clock (1800000/441 cycles a frame), 2500 / 2499 with -DO3OS_RATE_44100=OFF (4080 cycles a frame).
static_assert(core::kCpuHz % 100u == 0u && RateConverter::kOutRate % 100u == 0u, "the file's rate as cycles per frame is a rational of small numbers");
constexpr std::uint64_t kOutNum = core::kCpuHz / 100u;  // cycles per output frame = kOutNum / kOutDen = 1800000 / 441, the D-OS-8 rate of core/audio_clock.h
constexpr std::uint64_t kOutDen = RateConverter::kOutRate / 100u;
constexpr std::uint64_t kAdvNum0 = kOutNum * core::kRefCyclesPerSample.den;
constexpr std::uint64_t kAdvDen0 = kOutDen * core::kRefCyclesPerSample.num;
constexpr std::uint64_t kAdvGcd = gcd64(kAdvNum0, kAdvDen0);
constexpr std::uint64_t kAdvNum = kAdvNum0 / kAdvGcd;
constexpr std::uint64_t kAdvDen = kAdvDen0 / kAdvGcd;
static_assert(kAdvNum >= kAdvDen, "the engine's rate is not below the file's: an output frame never advances less than an input frame (the converter does not catch up)");
static_assert(kAdvDen < (std::uint64_t{1} << 24), "the fraction of an input frame is an exact float");

constexpr bool kIdentity = kAdvNum == kAdvDen;

std::int16_t to_q15(float v) noexcept {
  v += v >= 0.0f ? 0.5f : -0.5f;
  return v >= 32767.0f ? std::int16_t{32767} : (v <= -32768.0f ? std::int16_t{-32768} : static_cast<std::int16_t>(v));
}

// The Catmull-Rom cubic through x1 (t = 0) and x2 (t = 1), with x0 and x3 as its neighbours.
float cubic(float x0, float x1, float x2, float x3, float t) noexcept {
  return x1 + 0.5f * t * (x2 - x0 + t * (2.0f * x0 - 5.0f * x1 + 4.0f * x2 - x3 + t * (3.0f * (x1 - x2) + x3 - x0)));
}

void put16(std::uint8_t* p, std::uint32_t v) noexcept {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
}
void put32(std::uint8_t* p, std::uint32_t v) noexcept {
  put16(p, v);
  put16(p + 2, v >> 16);
}

}  // namespace

// ---- The rate converter -------------------------------------------------------------------------------------------------------------------------

bool RateConverter::identity() noexcept { return kIdentity; }

void RateConverter::reset() noexcept {
  std::memset(h_, 0, sizeof h_);
  fed_ = 0;
  pos_ = 0;
}

std::uint32_t RateConverter::feed(const std::int16_t* in, std::uint32_t n, std::int16_t* out) noexcept {
  if constexpr (kIdentity) {
    if (out != nullptr) {
      std::memcpy(out, in, static_cast<std::size_t>(n) * 2u * sizeof(std::int16_t));
    }
    fed_ += n;
    return n;
  }
  std::uint32_t made = 0;
  for (std::uint32_t k = 0; k < n; ++k) {
    for (unsigned j = 0; j < 3; ++j) {  // the history shifts down one frame, the new one comes in at the top
      h_[j][0] = h_[j + 1][0];
      h_[j][1] = h_[j + 1][1];
    }
    h_[3][0] = in[2u * k];
    h_[3][1] = in[2u * k + 1u];
    ++fed_;  // the newest frame is x[fed_ - 1]
    // Output frame m sits at input position pos_ / kAdvDen; it needs x[i - 1] .. x[i + 2] (i the whole part), which are the four held when i + 2 is the newest.
    while (pos_ / kAdvDen + 3u <= fed_) {
      const std::uint64_t i = pos_ / kAdvDen;
      if (i + 3u == fed_) {
        if (out != nullptr) {
          const float t = static_cast<float>(pos_ % kAdvDen) / static_cast<float>(kAdvDen);
          for (unsigned c = 0; c < 2; ++c) {
            out[2u * made + c] = to_q15(cubic(h_[0][c], h_[1][c], h_[2][c], h_[3][c], t));
          }
        }
        ++made;
      }  // (an output the history has already moved past is not made: the ratio is not below one, so it does not arise)
      pos_ += kAdvNum;
    }
  }
  return made;
}

// ---- The source ---------------------------------------------------------------------------------------------------------------------------------

bool ExportSource::set(const audio::OfflineRequest& request) noexcept {
  if (!attached()) {
    return false;
  }
  request_ = request;
  measure_rq_ = request;
  known_frames_ = 0;
  unit_base_ = 0;
  units_ = 2;
  frames_known_ = false;
  length_frames_ = 0;
  phase_ = Phase::Idle;
  failure_ = "";
  return true;
}

bool ExportSource::set_stem(const audio::OfflineRequest& write, const audio::OfflineRequest& measure, std::uint32_t known_frames, unsigned unit_base, unsigned units) noexcept {
  if (!set(write)) {
    return false;
  }
  measure_rq_ = measure;
  known_frames_ = known_frames;
  unit_base_ = unit_base;
  units_ = units < 2u ? 2u : units;
  return true;
}

void ExportSource::fail(const char* why) noexcept {
  failure_ = why;
  phase_ = Phase::Failed;
  end();
}

void ExportSource::end() noexcept {
  if (opened_) {
    opened_ = false;
    w_.renderer->close();
  }
}

bool ExportSource::open_pass(const audio::OfflineRequest& request) noexcept {
  if (!w_.renderer->open(request)) {
    failure_ = w_.renderer->failure();
    return false;
  }
  opened_ = true;
  conv_.reset();
  blocks_ = 0;
  out_frames_ = 0;
  waits_ = 0;
  return true;
}

void ExportSource::begin(core::ByteSink& sink) noexcept {
  sink_ = &sink;
  frames_known_ = false;
  length_frames_ = 0;
  written_frames_ = 0;
  header_sent_ = 0;
  head_ = tail_ = used_ = 0;
  bytes_out_ = 0;
  end_frames_ = loud_frames_ = quiet_blocks_ = tail_blocks_ = 0;
  tail_seen_ = false;
  measure_pm_ = 0;
  failure_ = "";
  phase_ = Phase::Measure;
  bool opened = attached();
  if (opened && known_frames_ != 0u) {  // a stem after the first: the length is the set's, the file is written at once
    length_frames_ = known_frames_;
    frames_known_ = true;
    make_header();
    phase_ = Phase::Write;
    opened = open_pass(request_);
  } else if (opened) {
    opened = open_pass(measure_rq_);
  }
  if (!opened) {
    phase_ = Phase::Failed;
    if (failure_[0] == '\0') {
      failure_ = "EXPORT IS NOT AVAILABLE";
    }
  }
}

// The file's 44-byte header for length_frames_.
void ExportSource::make_header() noexcept {
  std::uint8_t* h = header_;
  const std::uint32_t data = length_frames_ * 4u;
  std::memcpy(h, "RIFF", 4);
  put32(h + 4, 36u + data);
  std::memcpy(h + 8, "WAVEfmt ", 8);
  put32(h + 16, 16);
  put16(h + 20, 1);  // PCM
  put16(h + 22, 2);
  put32(h + 24, RateConverter::kOutRate);
  put32(h + 28, RateConverter::kOutRate * 4u);
  put16(h + 32, 4);
  put16(h + 34, 16);
  std::memcpy(h + 36, "data", 4);
  put32(h + 40, data);
}

int ExportSource::progress_permille() const noexcept {
  switch (phase_) {
    case Phase::Idle:  // (a file of a set that has not begun: the share of the bar the files before it filled)
      return static_cast<int>(unit_base_ * 1000u / units_);
    case Phase::Failed:
      return 0;
    case Phase::Measure:  // the pass that measures: its share of the bar (the first half of a plain export's)
      return static_cast<int>(unit_base_ * 1000u / units_ + measure_pm_ / units_);
    case Phase::Write:
    case Phase::Drain: {
      const unsigned unit = unit_base_ + (known_frames_ != 0u ? 0u : 1u);  // (a stem after the first has no measuring pass)
      return static_cast<int>(unit * 1000u / units_ + (length_frames_ != 0u ? static_cast<unsigned>(static_cast<std::uint64_t>(bytes_out_ / 4u) * 1000u / (static_cast<std::uint64_t>(units_) * length_frames_)) : 1000u / units_));
    }
    case Phase::Done:  // the end of this job's passes (1000 for a plain export and for the last file of a set)
      return static_cast<int>((unit_base_ + (known_frames_ != 0u ? 1u : 2u)) * 1000u / units_);
  }
  return 0;
}

// One block of the render has come. In the measuring pass it is counted and judged (is it a sound? is the arrangement over? has it been quiet long enough?); in the writing pass it goes
// through the converter into the ring, as many of its frames as the file still wants.
void ExportSource::take_block(bool write) noexcept {
  ++blocks_;
  if (!write) {
    const std::uint32_t made = conv_.feed(block_, kBlock, nullptr);
    out_frames_ += made;
    std::int32_t peak = 0;
    for (std::uint32_t i = 0; i < hal::audio::kBlockSamples; ++i) {
      const std::int32_t a = block_[i] < 0 ? -static_cast<std::int32_t>(block_[i]) : static_cast<std::int32_t>(block_[i]);
      peak = a > peak ? a : peak;
    }
    if (peak > kSilenceLsb) {
      loud_frames_ = out_frames_;
      quiet_blocks_ = 0;
    } else {
      ++quiet_blocks_;
    }
    if (!tail_seen_ && w_.renderer->in_tail()) {
      tail_seen_ = true;
      end_frames_ = out_frames_;  // the block the arrangement ended in is the file's, whole
    }
    if (tail_seen_) {
      ++tail_blocks_;
    }
    measure_pm_ = w_.renderer->progress();
    if (out_frames_ > kMaxFrames) {
      fail("TOO LONG FOR A WAV (100 MIN)");
    } else if (tail_seen_ && (quiet_blocks_ >= (measure_rq_.trim_tail ? 1u : kHoldBlocks) || tail_blocks_ >= kMaxTailBlocks)) {  // (trim_tail: the first silent block ends it, no hold)
      length_frames_ = end_frames_ > loud_frames_ ? end_frames_ : loud_frames_;
      frames_known_ = true;
    }
    return;
  }
  std::int16_t* const out = out_block_;
  const std::uint32_t made = conv_.feed(block_, kBlock, out);
  const std::uint32_t want = length_frames_ - written_frames_;
  const std::uint32_t take = made < want ? made : want;
  const std::uint8_t* src = reinterpret_cast<const std::uint8_t*>(out);
  std::size_t bytes = static_cast<std::size_t>(take) * 4u;
  while (bytes != 0u) {  // into the ring, in at most two pieces
    const std::size_t room = w_.ring_bytes - head_;
    const std::size_t n = bytes < room ? bytes : room;
    std::memcpy(w_.ring + head_, src, n);
    head_ = (head_ + n) % w_.ring_bytes;
    used_ += n;
    src += n;
    bytes -= n;
  }
  written_frames_ += take;
}

// Renders for about one slice. The measuring pass ends when the length is known (and the second pass is opened); the writing pass ends when the file's last frame is in the ring.
// False: the render failed (fail() has said why).
bool ExportSource::render_slice(bool write) noexcept {
  const std::uint32_t t0 = w_.cycles != nullptr ? w_.cycles() : 0u;
  for (std::uint32_t n = 0; n < kMaxBlocksPerStep; ++n) {
    if (write && (ring_free() < kMaxBlockOutBytes || written_frames_ >= length_frames_)) {
      break;  // the ring is full (the card's turn), or the file is complete
    }
    const audio::OfflineRenderer::Step s = w_.renderer->next(block_);
    if (s == audio::OfflineRenderer::Step::Failed) {
      fail(w_.renderer->failure()[0] != '\0' ? w_.renderer->failure() : "THE RENDER FAILED");
      return false;
    }
    if (s == audio::OfflineRenderer::Step::Wait) {
      if (++waits_ > kMaxWaits) {
        fail("THE AUDIO DID NOT STAND DOWN");
        return false;
      }
    } else {
      waits_ = 0;
      take_block(write);
      if (phase_ == Phase::Failed || (!write && frames_known_)) {
        return phase_ != Phase::Failed;
      }
    }
    if (w_.cycles != nullptr && w_.slice_cycles != 0u && static_cast<std::uint32_t>(w_.cycles() - t0) >= w_.slice_cycles) {
      break;
    }
  }
  return true;
}

// Gives the sink what the ring holds, the header first, at most `byte_budget` bytes. False: the sink refused.
bool ExportSource::drain(std::size_t byte_budget) noexcept {
  while (byte_budget != 0u) {
    const std::uint8_t* src;
    std::size_t n;
    if (header_sent_ < kHeaderBytes) {
      src = header_ + header_sent_;
      n = kHeaderBytes - header_sent_;
    } else if (used_ != 0u) {
      src = w_.ring + tail_;
      n = used_ < w_.ring_bytes - tail_ ? used_ : w_.ring_bytes - tail_;
    } else {
      break;
    }
    n = n < byte_budget ? n : byte_budget;
    const core::IoResult r = sink_->write(src, n);
    if (r.status != core::IoStatus::Ok || r.count == 0u || r.count > n) {
      return false;
    }
    if (header_sent_ < kHeaderBytes) {
      header_sent_ += r.count;
    } else {
      tail_ = (tail_ + r.count) % w_.ring_bytes;
      used_ -= r.count;
      bytes_out_ += static_cast<std::uint32_t>(r.count);
    }
    byte_budget -= r.count;
  }
  return true;
}

formats::Progress ExportSource::step(std::size_t byte_budget) noexcept {
  switch (phase_) {
    case Phase::Idle:
    case Phase::Failed:
      return formats::Progress::Failed;
    case Phase::Done:
      return formats::Progress::Done;
    case Phase::Measure: {
      if (!render_slice(false)) {
        return formats::Progress::Failed;
      }
      if (!frames_known_) {
        return formats::Progress::Running;
      }
      // The length is known: the file's header, and the render starts over, from silence, with the engine still ours.
      if (length_frames_ == 0u || static_cast<std::uint64_t>(length_frames_) * 4u + kHeaderBytes > 0xFFFFFFFFull) {
        fail("NOTHING TO WRITE");
        return formats::Progress::Failed;
      }
      make_header();
      if (!open_pass(request_)) {
        fail(failure_[0] != '\0' ? failure_ : "THE RENDER FAILED");
        return formats::Progress::Failed;
      }
      phase_ = Phase::Write;
      return formats::Progress::Running;
    }
    case Phase::Write:
    case Phase::Drain: {
      if (phase_ == Phase::Write && !render_slice(true)) {
        return formats::Progress::Failed;
      }
      if (phase_ == Phase::Write && written_frames_ >= length_frames_) {
        phase_ = Phase::Drain;
        end();  // the last frame is in the ring: the engine goes back to the live render before the card has finished
      }
      if (!drain(byte_budget)) {
        failure_ = "";  // the card said no: AtomicSave reads the sink's status for the reason
        phase_ = Phase::Failed;
        end();
        return formats::Progress::Failed;
      }
      if (phase_ == Phase::Drain && used_ == 0u && header_sent_ >= kHeaderBytes) {
        phase_ = Phase::Done;
        return formats::Progress::Done;
      }
      return formats::Progress::Running;
    }
  }
  return formats::Progress::Failed;
}

}  // namespace o3os::app
