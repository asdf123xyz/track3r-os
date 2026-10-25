// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/formats/iff_sample.h>

#include <cstring>

namespace o3os::formats::iff {

namespace {

constexpr std::uint32_t four(char a, char b, char c, char d) noexcept {
  return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(a)) << 24) | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b)) << 16) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(c)) << 8) | static_cast<std::uint32_t>(static_cast<std::uint8_t>(d));
}

constexpr std::uint32_t kComm = four('C', 'O', 'M', 'M'), kSsnd = four('S', 'S', 'N', 'D'), kInst = four('I', 'N', 'S', 'T'), kMark = four('M', 'A', 'R', 'K');
constexpr std::uint32_t kVhdr = four('V', 'H', 'D', 'R'), kBody = four('B', 'O', 'D', 'Y'), kName = four('N', 'A', 'M', 'E'), kChan = four('C', 'H', 'A', 'N');
constexpr std::uint32_t kNone = four('N', 'O', 'N', 'E'), kSowt = four('s', 'o', 'w', 't');
constexpr std::size_t kNameMax = 32;

std::uint16_t be16(const std::uint8_t* p) noexcept { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }
std::uint32_t be32(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) | (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

// The WAV importer's quantiser (formats/wav/src/wav_importer.cpp, tpdf and quantise24): a 24-bit value to int16, exact where the low byte is zero, else rounded with TPDF dither of +-1 LSB
// of the result from one xorshift32 step.
std::int32_t tpdf(std::uint32_t& rng) noexcept {
  rng ^= rng << 13;
  rng ^= rng >> 17;
  rng ^= rng << 5;
  return static_cast<std::int32_t>((rng >> 8) & 0xFFu) + static_cast<std::int32_t>((rng >> 20) & 0xFFu) - 255;
}

std::int16_t quantise24(std::int32_t w, std::uint32_t& rng) noexcept {
  if ((w & 0xFF) == 0) {
    return static_cast<std::int16_t>(w >> 8);
  }
  const std::int32_t v = (w + 128 + tpdf(rng)) >> 8;
  return static_cast<std::int16_t>(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}

// A sample of `bytes` bytes (1..3, signed, big- or little-endian) as a 24-bit value.
std::int32_t wide(const std::uint8_t* p, unsigned bytes, bool little) noexcept {
  switch (bytes) {
    case 1:
      return static_cast<std::int32_t>(static_cast<std::int8_t>(p[0])) * 65536;
    case 2: {
      const unsigned u = little ? static_cast<unsigned>(p[0] | (p[1] << 8)) : static_cast<unsigned>((p[0] << 8) | p[1]);
      return static_cast<std::int32_t>(static_cast<std::int16_t>(u)) * 256;
    }
    default: {
      const std::uint32_t u = little ? (static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16))
                                     : ((static_cast<std::uint32_t>(p[0]) << 16) | (static_cast<std::uint32_t>(p[1]) << 8) | static_cast<std::uint32_t>(p[2]));
      return static_cast<std::int32_t>(u << 8) >> 8;  // sign-extended
    }
  }
}

// The EA reference table of the Fibonacci-delta code (8SVX sCompression 1).
const std::int8_t kFibDelta[16] = {-34, -21, -13, -8, -5, -3, -2, -1, 0, 1, 2, 3, 5, 8, 13, 21};

}  // namespace

bool sniff(const std::uint8_t* head, std::size_t n, std::uint32_t) noexcept {
  if (n < 12 || std::memcmp(head, "FORM", 4) != 0) {
    return false;  // (the native project is a FORM too: a head that cannot show the type is not claimed)
  }
  return std::memcmp(head + 8, "AIFF", 4) == 0 || std::memcmp(head + 8, "AIFC", 4) == 0 || std::memcmp(head + 8, "8SVX", 4) == 0;
}

bool decode_extended_rate(const std::uint8_t ext[10], std::uint32_t& hz) noexcept {
  if ((ext[0] & 0x80u) != 0) {
    return false;  // negative
  }
  const int exponent = ((ext[0] & 0x7F) << 8) | ext[1];
  if (exponent == 0x7FFF) {
    return false;  // infinity or NaN
  }
  std::uint64_t mant = 0;
  for (int i = 2; i < 10; ++i) {
    mant = (mant << 8) | ext[i];
  }
  // value = mant * 2^(exponent - 16383 - 63): a right shift of 63 - (exponent - 16383) places, of 1..63 for a value of 1 Hz up to 2^32.
  const int shift = 63 - (exponent - 16383);
  if (shift < 1 || shift > 63) {
    return false;  // 2^63 and over (or nothing the mantissa can reach), or a value below one half
  }
  std::uint64_t q = (mant >> shift) + ((mant >> (shift - 1)) & 1u);  // rounded to nearest, half up
  if (q == 0 || q > 0xFFFFFFFFu) {
    return false;
  }
  hz = static_cast<std::uint32_t>(q);
  return true;
}

void SampleReader::fail(FormatError e, const char* detail) noexcept {
  state_ = State::Failed;
  progress_ = formats::Progress::Failed;
  report_.error = e;
  report_.detail = detail;
  report_.offset = report_.bytes;
}

void SampleReader::begin(core::ByteSource& source, model::ModelArena& arena, const ReadParams& params) noexcept {
  report_ = FormatReport{};
  source_ = &source;
  project_ = &arena.project;
  pcm_ = params.pcm;
  capacity_ = params.pcm_capacity;
  slot_ = params.index;
  wavetable_ = params.wavetable;
  keep_stereo_ = params.keep_stereo;
  channel_ = params.channel <= 2u ? params.channel : std::uint8_t{0};
  blocked_ = false;
  p_ = Parse{};
  if (params.size_hint != 0) {
    p_.file_end = params.size_hint;
  }
  title_[0] = '\0';
  name_[0] = '\0';
  if (params.name != nullptr) {
    std::strncpy(name_, params.name, sizeof name_ - 1);  // make_sample_instrument cuts the extension, then the length
    name_[sizeof name_ - 1] = '\0';
  }
  state_ = State::FormHead;
  progress_ = formats::Progress::Running;
  if (params.index >= model::kMaxInstruments) {
    fail(FormatError::LimitExceeded, "the instrument slot does not exist");
  } else if (params.pcm == nullptr || params.pcm_capacity == 0) {
    fail(FormatError::LimitExceeded, "no room was given for the audio");
  }
}

unsigned SampleReader::permille() const noexcept {
  if (progress_ == formats::Progress::Done) {
    return 1000;
  }
  if (p_.total == 0) {
    return 0;
  }
  const unsigned v = static_cast<unsigned>((static_cast<std::uint64_t>(p_.out) * 1000u) / p_.total);
  return v > 999u ? 999u : v;
}

void SampleReader::cancel() noexcept {
  if (progress_ == formats::Progress::Running) {
    fail(FormatError::Cancelled, "cancelled");  // nothing was applied to the model
  }
}

// One read of at most `ask` bytes (and of the step's budget) into `where`. 1: `got` bytes arrived; 0: nothing now (budget spent or the source would block); -1: the read ended or failed.
int SampleReader::read_some(std::uint8_t* where, std::size_t ask, std::size_t& got) noexcept {
  got = 0;
  if (budget_ == 0) {
    return 0;
  }
  if (ask > budget_) {
    ask = budget_;
  }
  const core::IoResult r = source_->read(where, ask);
  if (r.status == core::IoStatus::WouldBlock) {
    blocked_ = true;
    return 0;
  }
  if (r.status == core::IoStatus::End) {
    on_end();
    return -1;
  }
  if (r.status != core::IoStatus::Ok || r.count == 0 || r.count > ask) {
    fail(FormatError::SourceFailed, "the source failed or broke its contract");
    return -1;
  }
  got = r.count;
  budget_ -= got;
  p_.pos += got;
  report_.bytes += static_cast<std::uint32_t>(got);
  return 1;
}

int SampleReader::take(std::size_t want) noexcept {
  while (p_.have < want) {
    std::size_t got = 0;
    const int rc = read_some(buf_ + p_.have, want - p_.have, got);
    if (rc <= 0) {
      return rc;
    }
    p_.have = static_cast<std::uint8_t>(p_.have + got);
  }
  return 1;
}

// The source ended. Once the audio is in, the rest of the file only matters for the loop: the read ends here, successfully.
void SampleReader::on_end() noexcept {
  if (!p_.data_done) {
    fail(FormatError::Truncated, state_ == State::FormHead ? "the file ends inside the header" : "the file ends before the audio is complete");
    return;
  }
  const bool boundary = (state_ == State::ChunkHead || state_ == State::Pad) && p_.have == 0 && p_.form_end == kUnknown;
  if (!boundary) {
    ++p_.sanitised;  // the form promised more than the file holds
  }
  finish();
}

void SampleReader::on_form_head() noexcept {
  if (std::memcmp(buf_, "FORM", 4) != 0) {
    fail(FormatError::BadMagic, "not an IFF FORM file");
    return;
  }
  if (std::memcmp(buf_ + 8, "AIFF", 4) == 0) {
    p_.form = Form::Aiff;
  } else if (std::memcmp(buf_ + 8, "AIFC", 4) == 0) {
    p_.form = Form::AiffC;
  } else if (std::memcmp(buf_ + 8, "8SVX", 4) == 0) {
    p_.form = Form::Svx;
  } else {
    fail(FormatError::Unsupported, "an IFF file that is not AIFF or 8SVX");
    return;
  }
  const std::uint32_t size = be32(buf_ + 4);
  if (size < 4u) {
    fail(FormatError::Malformed, "the FORM is too small to hold anything");
    return;
  }
  p_.form_end = size == 0xFFFFFFFFu ? kUnknown : std::uint64_t{8} + size;  // a streamed file may leave the size at -1
  p_.have = 0;
  state_ = State::ChunkHead;
}

// A chunk that ends past the form or past the file. After the audio it only ends the read; before it, the file is bad.
bool SampleReader::past_end(std::uint32_t size) noexcept {
  const std::uint64_t end = p_.pos + size;
  const bool file = p_.file_end != kUnknown && end > p_.file_end;
  const bool form = p_.form_end != kUnknown && end > p_.form_end;
  if (!file && !form) {
    return false;
  }
  if (p_.data_done) {
    ++p_.sanitised;
    finish();
  } else if (file) {
    fail(FormatError::Truncated, "a chunk runs past the end of the file");
  } else {
    fail(FormatError::Malformed, "a chunk runs past the end of the form");
  }
  return true;
}

void SampleReader::collect(std::size_t n) noexcept {
  p_.want = static_cast<std::uint8_t>(n);
  p_.have = 0;
  p_.chunk_left -= static_cast<std::uint32_t>(n);
  state_ = State::Collect;
}

void SampleReader::skip(std::uint32_t n, State then) noexcept {
  p_.skip_left = n;
  p_.resume = then;
  state_ = State::Skip;
}

// The chunk is done with: the rest of its payload and its pad byte are skipped, then the next chunk.
void SampleReader::end_chunk() noexcept {
  const std::uint32_t left = p_.chunk_left;
  p_.chunk_left = 0;
  skip(left, State::Pad);
}

void SampleReader::on_chunk_head() noexcept {
  const std::uint32_t id = be32(buf_);
  const std::uint32_t size = be32(buf_ + 4);
  p_.have = 0;
  p_.chunk_id = id;
  p_.chunk_left = size;
  p_.odd = (size & 1u) != 0;
  if (past_end(size)) {
    return;
  }
  if (p_.form != Form::Svx) {
    if (id == kComm) {
      const std::uint32_t need = p_.form == Form::AiffC ? 22u : 18u;
      if (p_.header_seen) {
        fail(FormatError::Malformed, "a second COMM chunk");
      } else if (size < need) {
        fail(FormatError::Malformed, "the COMM chunk is too short");
      } else {
        collect(need);
      }
    } else if (id == kSsnd) {
      if (!p_.header_seen) {
        fail(FormatError::Malformed, kSsndBeforeComm);
      } else if (p_.data_seen) {
        end_chunk();  // a second SSND is not read
      } else if (size < 8u) {
        fail(FormatError::Malformed, "the SSND chunk is too short");
      } else {
        p_.data_seen = true;
        collect(8);
      }
    } else if (id == kInst && !p_.inst_seen && size >= 20u) {
      p_.inst_seen = true;
      collect(20);
    } else if (id == kMark && !p_.mark_seen && size >= 2u) {
      p_.mark_seen = true;
      p_.mark_step = 0;
      state_ = State::Mark;
    } else {
      end_chunk();
    }
    return;
  }
  if (id == kVhdr) {
    if (p_.header_seen) {
      fail(FormatError::Malformed, "a second VHDR chunk");
    } else if (size < 20u) {
      fail(FormatError::Malformed, "the VHDR chunk is too short");
    } else {
      collect(20);
    }
  } else if (id == kBody) {
    if (!p_.header_seen) {
      fail(FormatError::Malformed, "the BODY comes before the VHDR chunk");
    } else if (p_.data_seen) {
      end_chunk();
    } else {
      p_.data_seen = true;
      state_ = State::DataBegin;
    }
  } else if (id == kName && !p_.name_seen && size != 0u) {
    p_.name_seen = true;
    collect(size < kNameMax ? size : kNameMax);
  } else if (id == kChan && !p_.chan_seen && size >= 4u) {
    p_.chan_seen = true;
    collect(4);
  } else {
    end_chunk();
  }
}

void SampleReader::on_comm() noexcept {
  const unsigned channels = be16(buf_);
  const std::uint32_t frames = be32(buf_ + 2);
  const unsigned bits = be16(buf_ + 6);
  if (channels == 0 || bits == 0 || frames == 0) {
    fail(FormatError::Malformed, "the COMM chunk has no channels, depth or frames");
    return;
  }
  if (channels > 2) {
    fail(FormatError::Unsupported, "more than two channels");
    return;
  }
  if (bits > 24) {
    fail(FormatError::Unsupported, "PCM deeper than 24 bits");
    return;
  }
  if (p_.form == Form::AiffC) {
    const std::uint32_t type = be32(buf_ + 18);
    if (type != kNone && type != kSowt) {
      fail(FormatError::Unsupported, "AIFF-C compression other than NONE and sowt");
      return;
    }
    p_.little = type == kSowt;
  }
  std::uint32_t rate = 0;
  if (!decode_extended_rate(buf_ + 8, rate)) {
    fail(FormatError::Malformed, "the sample rate is not usable");
    return;
  }
  p_.rate = rate;
  p_.comm_frames = frames;
  p_.channels = static_cast<std::uint8_t>(channels);
  p_.width = static_cast<std::uint8_t>((bits + 7u) / 8u);
  p_.unit = static_cast<std::uint8_t>(p_.width * channels);
  p_.out_channels = (channels == 2 && keep_stereo_) ? std::uint8_t{2} : std::uint8_t{1};
  p_.header_seen = true;
}

void SampleReader::on_vhdr() noexcept {
  p_.one_shot = be32(buf_);
  p_.repeat = be32(buf_ + 4);
  const std::uint32_t rate = be16(buf_ + 12);
  p_.octaves = buf_[14];
  p_.compression = buf_[15];
  if (rate == 0u) {
    fail(FormatError::Malformed, "the VHDR chunk has no sample rate");
    return;
  }
  if (p_.compression > 1u) {
    fail(FormatError::Unsupported, "8SVX compression other than none and Fibonacci-delta");
    return;
  }
  if (p_.octaves > 1u) {
    ++p_.sanitised;  // only the first octave is read
  }
  p_.rate = rate;
  p_.channels = 1;
  p_.out_channels = 1;
  p_.width = 1;
  p_.unit = 1;
  p_.header_seen = true;
}

void SampleReader::on_inst() noexcept {
  p_.loop_mode = static_cast<std::uint8_t>(be16(buf_ + 8) > 2u ? 3u : be16(buf_ + 8));
  p_.loop_begin_id = be16(buf_ + 10);
  p_.loop_end_id = be16(buf_ + 12);
}

void SampleReader::on_ssnd_head() noexcept {
  const std::uint32_t offset = be32(buf_);  // (blockSize is for writers that align the audio: not needed here)
  if (offset > p_.chunk_left) {
    fail(FormatError::Malformed, "the SSND offset runs past its chunk");
    return;
  }
  p_.chunk_left -= offset;
  if (offset != 0u) {
    skip(offset, State::DataBegin);
  } else {
    state_ = State::DataBegin;
  }
}

void SampleReader::on_collected() noexcept {
  p_.have = 0;
  switch (p_.chunk_id) {
    case kComm: on_comm(); break;
    case kVhdr: on_vhdr(); break;
    case kInst: on_inst(); break;
    case kSsnd: on_ssnd_head(); return;  // goes on to the audio by itself
    case kName: {
      std::size_t n = 0;
      for (std::size_t i = 0; i < p_.want && buf_[i] != 0; ++i) {
        title_[n++] = (buf_[i] >= 0x20 && buf_[i] < 0x7F) ? static_cast<char>(buf_[i]) : '?';
      }
      while (n != 0 && title_[n - 1] == ' ') {
        --n;
      }
      title_[n] = '\0';
      break;
    }
    case kChan:
      if (be32(buf_) == 6u) {
        ++p_.sanitised;  // stereo: the right channel follows the left in BODY and is not read
      }
      break;
    default: break;
  }
  if (state_ == State::Collect) {
    end_chunk();
  }
}

// MARK: a count, then records of a 16-bit id, a 32-bit frame position and a pascal string padded to an even length. Streamed record by record; the first kMaxMarkers are kept.
void SampleReader::step_mark() noexcept {
  if (p_.mark_step == 0u) {
    if (take(2) != 1) {
      return;
    }
    p_.have = 0;
    p_.chunk_left -= 2u;
    p_.mark_left = be16(buf_);
    p_.mark_step = 1;
    return;
  }
  if (p_.mark_left == 0u || p_.chunk_left < 7u) {
    end_chunk();  // (a count that promises more than the chunk holds is read as far as it goes)
    return;
  }
  if (take(7) != 1) {
    return;
  }
  p_.have = 0;
  p_.chunk_left -= 7u;
  --p_.mark_left;
  if (p_.mark_n < kMaxMarkers) {
    p_.mark_id[p_.mark_n] = be16(buf_);
    p_.mark_pos[p_.mark_n] = be32(buf_ + 2);
    ++p_.mark_n;
  } else {
    ++p_.sanitised;
  }
  const std::uint32_t text = static_cast<std::uint32_t>(buf_[6]) + ((buf_[6] & 1u) == 0u ? 1u : 0u);  // the text and the pad that makes count + text even
  if (text > p_.chunk_left) {
    end_chunk();
    return;
  }
  p_.chunk_left -= text;
  skip(text, State::Mark);
}

void SampleReader::data_begin() noexcept {
  std::uint64_t frames;
  if (p_.form != Form::Svx) {
    const std::uint64_t avail = p_.chunk_left / p_.unit;
    frames = p_.comm_frames < avail ? p_.comm_frames : avail;
    if (frames == 0u) {
      fail(FormatError::Malformed, "the sound data holds no whole frame");
      return;
    }
    if (p_.comm_frames > avail) {
      ++p_.sanitised;  // the COMM chunk promised more frames than the SSND chunk holds
    }
    p_.data_left = static_cast<std::uint32_t>(frames * p_.unit);
  } else {
    std::uint64_t n = std::uint64_t{p_.one_shot} + p_.repeat;
    const std::uint64_t avail = p_.chunk_left;
    if (n == 0u) {
      fail(FormatError::Malformed, "the VHDR chunk has no samples");
      return;
    }
    std::uint64_t need = n;
    if (p_.compression == 0u) {
      if (n > avail) {
        n = avail;
        ++p_.sanitised;
      }
      need = n;
      if (n == 0u) {
        fail(FormatError::Malformed, "the BODY chunk is empty");
        return;
      }
    } else {
      need = 2u + (n + 1u) / 2u;  // a pad byte, the initial value, then two samples a byte
      if (avail < 3u) {
        fail(FormatError::Malformed, "the BODY chunk is too short to unpack");
        return;
      }
      if (need > avail) {
        need = avail;
        n = 2u * (avail - 2u);
        ++p_.sanitised;
      }
      p_.fib_head = 2;
    }
    frames = n;
    p_.data_left = static_cast<std::uint32_t>(need);
  }
  if (frames * p_.out_channels > capacity_) {
    fail(FormatError::LimitExceeded, "the audio does not fit the sample buffer");
    return;
  }
  p_.frames = static_cast<std::uint32_t>(frames);
  p_.total = static_cast<std::uint32_t>(frames * p_.out_channels);
  p_.chunk_left -= p_.data_left;
  p_.have = 0;
  state_ = State::Data;
}

void SampleReader::convert_aiff(std::size_t units) noexcept {
  for (std::size_t i = 0; i < units; ++i) {
    const std::uint8_t* q = buf_ + i * p_.unit;
    if (p_.channels == 2 && p_.out_channels == 1) {
      const std::int32_t l = wide(q, p_.width, p_.little);
      const std::int32_t r = wide(q + p_.width, p_.width, p_.little);
      pcm_[p_.out++] = quantise24(channel_ == 1u ? l : (channel_ == 2u ? r : (l + r + 1) >> 1), p_.rng);  // round half up: |l + r| < 2^24; OS-LOADCH: or one channel alone
    } else {
      for (unsigned c = 0; c < p_.channels; ++c) {
        pcm_[p_.out++] = quantise24(wide(q + c * p_.width, p_.width, p_.little), p_.rng);
      }
    }
  }
}

void SampleReader::convert_svx(std::size_t bytes) noexcept {
  for (std::size_t i = 0; i < bytes; ++i) {
    const std::uint8_t b = buf_[i];
    if (p_.compression == 0u) {
      pcm_[p_.out++] = static_cast<std::int16_t>(static_cast<std::int8_t>(b) * 256);
    } else if (p_.fib_head != 0u) {
      if (p_.fib_head == 1u) {
        p_.fib = static_cast<std::int8_t>(b);  // the initial value (the byte before it is a pad)
      }
      --p_.fib_head;
    } else {
      for (unsigned k = 0; k < 2u && p_.out < p_.total; ++k) {
        int v = p_.fib + kFibDelta[k == 0u ? (b >> 4) : (b & 15u)];
        v = v > 127 ? 127 : (v < -128 ? -128 : v);
        p_.fib = static_cast<std::int16_t>(v);
        pcm_[p_.out++] = static_cast<std::int16_t>(v * 256);
      }
    }
  }
}

void SampleReader::step_data() noexcept {
  std::size_t want = sizeof buf_ - p_.have;
  if (want > p_.data_left) {
    want = p_.data_left;
  }
  std::size_t got = 0;
  if (read_some(buf_ + p_.have, want, got) != 1) {
    return;
  }
  p_.data_left -= static_cast<std::uint32_t>(got);
  const std::size_t have = p_.have + got;
  const std::size_t whole = have - have % p_.unit;
  if (p_.form == Form::Svx) {
    convert_svx(whole);
  } else {
    convert_aiff(whole / p_.unit);
  }
  const std::size_t rest = have - whole;
  for (std::size_t i = 0; i < rest; ++i) {
    buf_[i] = buf_[whole + i];  // a frame split across two reads
  }
  p_.have = static_cast<std::uint8_t>(rest);
  if (p_.data_left == 0u) {
    p_.data_done = true;
    p_.have = 0;
    end_chunk();
  }
}

formats::Progress SampleReader::step(std::size_t byte_budget) noexcept {
  if (progress_ != formats::Progress::Running) {
    return progress_;
  }
  budget_ = byte_budget == 0 ? 1 : byte_budget;
  blocked_ = false;
  while (progress_ == formats::Progress::Running && budget_ > 0 && !blocked_) {
    switch (state_) {
      case State::FormHead:
        if (take(12) == 1) {
          on_form_head();
        }
        break;
      case State::ChunkHead:
        if (p_.form_end != kUnknown && p_.form_end - p_.pos < 8u) {  // the form is over (a few bytes of junk after the last chunk are not read)
          if (p_.form_end != p_.pos) {
            ++p_.sanitised;
          }
          finish();
        } else if (take(8) == 1) {
          on_chunk_head();
        }
        break;
      case State::Collect:
        if (take(p_.want) == 1) {
          on_collected();
        }
        break;
      case State::Mark:
        step_mark();
        break;
      case State::DataBegin:
        data_begin();
        break;
      case State::Data:
        step_data();
        break;
      case State::Skip:
        if (p_.skip_left == 0u) {
          state_ = p_.resume;
        } else {
          std::size_t got = 0;
          const std::size_t ask = p_.skip_left < sizeof buf_ ? p_.skip_left : sizeof buf_;
          if (read_some(buf_, ask, got) == 1) {
            p_.skip_left -= static_cast<std::uint32_t>(got);
          }
        }
        break;
      case State::Pad:
        if (p_.odd && (p_.form_end == kUnknown || p_.pos < p_.form_end) && (p_.file_end == kUnknown || p_.pos < p_.file_end)) {
          if (take(1) == 1) {
            p_.have = 0;
            p_.odd = false;
            state_ = State::ChunkHead;
          }
        } else {
          p_.odd = false;
          state_ = State::ChunkHead;
        }
        break;
      case State::Done:
      case State::Failed:
        return progress_;
    }
  }
  return progress_;
}

void SampleReader::finish() noexcept {
  if (!p_.header_seen) {
    fail(FormatError::Malformed, p_.form == Form::Svx ? "the file has no VHDR chunk" : "the file has no COMM chunk");
    return;
  }
  if (!p_.data_done) {
    fail(FormatError::Malformed, "the file has no sound data");
    return;
  }
  wav::Info info;
  info.sample_rate = p_.rate;
  info.frames = p_.frames;
  info.channels = p_.out_channels;
  info.source_channels = p_.channels;
  info.downmixed = p_.channels == 2 && p_.out_channels == 1;
  info.bits = static_cast<std::uint8_t>(p_.width * 8u);
  info.dithered = false;
  if (p_.form == Form::Svx) {
    if (p_.repeat != 0u) {  // [one-shot part][repeat part]: the repeat part loops
      const std::uint64_t end = std::uint64_t{p_.one_shot} + p_.repeat;
      const std::uint32_t last = end < p_.frames ? static_cast<std::uint32_t>(end) : p_.frames;
      if (p_.one_shot < last) {
        info.has_loop = true;
        info.loop_type = wav::LoopType::Forward;
        info.loop_start = p_.one_shot;
        info.loop_end = last;
      } else {
        ++p_.sanitised;
      }
    }
  } else if (p_.inst_seen && p_.loop_mode != 0u) {
    bool have_b = false;
    bool have_e = false;
    std::uint32_t b = 0;
    std::uint32_t e = 0;
    for (unsigned i = 0; i < p_.mark_n; ++i) {
      if (p_.mark_id[i] == p_.loop_begin_id && !have_b) {
        b = p_.mark_pos[i];
        have_b = true;
      }
      if (p_.mark_id[i] == p_.loop_end_id && !have_e) {
        e = p_.mark_pos[i];
        have_e = true;
      }
    }
    if (e > p_.frames) {
      e = p_.frames;
    }
    if (p_.loop_mode <= 2u && have_b && have_e && b < e) {
      info.has_loop = true;
      info.loop_type = p_.loop_mode == 2u ? wav::LoopType::PingPong : wav::LoopType::Forward;
      info.loop_start = b;
      info.loop_end = e;
    } else {
      ++p_.sanitised;  // a loop the markers do not give
    }
  }
  model::Instrument inst;
  wav::make_sample_instrument(info, slot_, name_, inst, wavetable_);
  if (title_[0] != '\0') {  // 8SVX NAME
    std::memcpy(inst.name, title_, sizeof title_);
    inst.name[model::kInstrumentNameMax] = '\0';
  }
  bool unusable = false;
  const unsigned frame = wav::wavetable_frame(info.frames, 0u, wavetable_, &unusable);
  const unsigned tail = frame != 0u ? info.frames % frame : 0u;  // frames after the last whole window: the engine does not play them
  if (project_->set_instrument(slot_, inst) != model::Status::Ok) {
    fail(FormatError::Internal, "the model refused the instrument");
    return;
  }
  report_.instruments = 1;
  report_.pcm_frames = info.frames;
  report_.pcm_channels = info.channels;
  report_.sanitised = p_.sanitised + (unusable ? 1u : 0u) + (tail != 0u ? 1u : 0u) + (frame != 0u && info.has_loop ? 1u : 0u);
  report_.unverified_version = false;
  state_ = State::Done;
  progress_ = formats::Progress::Done;
}

}  // namespace o3os::formats::iff
