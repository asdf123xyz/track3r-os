// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-SAMPFMT: AIFF / AIFF-C and IFF 8SVX as a registered sample format. The 80-bit extended sample rate (integer decode, the usual rates and the edges), every AIFF layout (8 / 16 / 24 bits,
// mono and stereo, NONE and sowt, loops from INST + MARK in either order), every 8SVX layout (plain, looped, multi-octave, Fibonacci-delta, NAME), the refusals (compression, depth, channels, chunk sizes
// past the form and the file, odd chunk padding, truncation at every offset), a source that gives one byte at a time, and a damage fuzz. The files are generated here (no samples of anyone's).
// No heap, so the same file runs as the QEMU suite `qemu_iff_sample`.
#include <doctest/doctest.h>

#include <o3os/formats/builtin.h>
#include <o3os/formats/format.h>
#include <o3os/formats/iff_sample.h>
#include <o3os/model/project.h>

#include <cstdint>
#include <cstring>

#include "support/memory_io.h"
#include "support/need.h"
#include "support/rng.h"

using namespace o3os;
using formats::FormatError;
using formats::Progress;

namespace {

model::ModelArena g_arena;
formats::BuiltinFormats g_storage;
formats::FormatRegistry g_registry;

constexpr std::size_t kMaxFile = 6000;
constexpr std::size_t kPcm = 2048;
std::uint8_t g_bytes[kMaxFile];
std::size_t g_n = 0;
std::size_t g_chunk = 0;  // where the size of the chunk being written is
alignas(8) std::int16_t g_pcm[kPcm + 16];

void u8(std::uint32_t v) { if (g_n < kMaxFile) g_bytes[g_n++] = static_cast<std::uint8_t>(v); }
void b16(std::uint32_t v) { u8(v >> 8); u8(v); }
void b32(std::uint32_t v) { b16(v >> 16); b16(v & 0xFFFFu); }
void tag(const char* t) { for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>(t[i])); }

void begin_form(const char* type) { g_n = 0; tag("FORM"); b32(0); tag(type); }
void end_form() {
  const std::uint32_t size = static_cast<std::uint32_t>(g_n - 8);
  g_bytes[4] = static_cast<std::uint8_t>(size >> 24);
  g_bytes[5] = static_cast<std::uint8_t>(size >> 16);
  g_bytes[6] = static_cast<std::uint8_t>(size >> 8);
  g_bytes[7] = static_cast<std::uint8_t>(size);
}
void begin_chunk(const char* id) { tag(id); g_chunk = g_n; b32(0); }
void end_chunk(bool pad = true) {
  const std::uint32_t size = static_cast<std::uint32_t>(g_n - g_chunk - 4);
  g_bytes[g_chunk] = static_cast<std::uint8_t>(size >> 24);
  g_bytes[g_chunk + 1] = static_cast<std::uint8_t>(size >> 16);
  g_bytes[g_chunk + 2] = static_cast<std::uint8_t>(size >> 8);
  g_bytes[g_chunk + 3] = static_cast<std::uint8_t>(size);
  if (pad && (size & 1u) != 0) u8(0);
}

// The 80-bit extended number of a whole rate, as a writer makes it (the mantissa shifted so that its top bit is set).
void ext80(std::uint32_t rate) {
  if (rate == 0) { for (int i = 0; i < 10; ++i) u8(0); return; }
  unsigned top = 31;
  while (((rate >> top) & 1u) == 0) --top;
  const std::uint64_t mant = static_cast<std::uint64_t>(rate) << (63 - top);
  b16(16383 + top);
  b32(static_cast<std::uint32_t>(mant >> 32));
  b32(static_cast<std::uint32_t>(mant));
}

std::int16_t s16(unsigned i) { return static_cast<std::int16_t>((i * 2654435761u) >> 16); }

struct Marker { unsigned id; std::uint32_t pos; unsigned name_len; };

struct Aiff {
  const char* type = "AIFF";       // "AIFF" or "AIFC"
  const char* comp = "NONE";       // AIFC compression type
  unsigned bits = 16;
  unsigned channels = 1;
  std::uint32_t frames = 100;
  std::uint32_t rate = 22050;
  std::uint32_t comm_frames = 0;   // what COMM says when it lies (0: the frames)
  std::uint32_t ssnd_offset = 0;
  unsigned loop_mode = 0;          // INST sustain loop: 0 none, 1 forward, 2 forward-backward
  unsigned loop_begin = 1, loop_end = 2;
  const Marker* marks = nullptr;
  unsigned mark_count = 0;
  bool inst_after = false;         // INST and MARK after the audio
  bool odd_before = false;         // an odd-sized unknown chunk before COMM
  bool odd_unpadded_last = false;  // an odd-sized unknown last chunk without its pad byte
  bool low_bytes = false;          // 24-bit: nonzero low bytes (dither)
  bool sowt = false;               // little-endian samples
};

void put_sample(unsigned i, const Aiff& a) {
  const std::int16_t v = s16(i);
  const std::uint32_t u = static_cast<std::uint16_t>(v);
  switch ((a.bits + 7) / 8) {
    case 1: u8(static_cast<std::uint8_t>(static_cast<std::int8_t>(v >> 8))); break;
    case 2: if (a.sowt) { u8(u); u8(u >> 8); } else { b16(u); } break;
    default: {
      const std::uint32_t low = a.low_bytes ? ((i * 29u + 7u) & 0xFFu) : 0u;
      const std::uint32_t w = (u << 8) | low;
      if (a.sowt) { u8(w); u8(w >> 8); u8(w >> 16); } else { u8(w >> 16); u8(w >> 8); u8(w); }
      break;
    }
  }
}

void put_inst_mark(const Aiff& a) {
  if (a.loop_mode != 0) {
    begin_chunk("INST");
    u8(60); u8(0); u8(0); u8(127); u8(1); u8(127); b16(0);
    b16(a.loop_mode); b16(a.loop_begin); b16(a.loop_end);  // sustain loop
    b16(0); b16(0); b16(0);                                 // release loop
    end_chunk();
  }
  if (a.mark_count != 0) {
    begin_chunk("MARK");
    b16(a.mark_count);
    for (unsigned k = 0; k < a.mark_count; ++k) {
      b16(a.marks[k].id);
      b32(a.marks[k].pos);
      u8(a.marks[k].name_len);
      for (unsigned c = 0; c < a.marks[k].name_len; ++c) u8('a' + c);
      if (((a.marks[k].name_len + 1) & 1u) != 0) u8(0);  // pascal string padded to even
    }
    end_chunk();
  }
}

void make_aiff(const Aiff& a) {
  begin_form(a.type);
  if (a.odd_before) { begin_chunk("ANNO"); u8('x'); u8('y'); u8('z'); end_chunk(); }
  if (std::strcmp(a.type, "AIFC") == 0) { begin_chunk("FVER"); b32(0xA2805140u); end_chunk(); }
  begin_chunk("COMM");
  b16(a.channels);
  b32(a.comm_frames != 0 ? a.comm_frames : a.frames);
  b16(a.bits);
  ext80(a.rate);
  if (std::strcmp(a.type, "AIFC") == 0) {
    tag(a.sowt ? "sowt" : a.comp);
    u8(4); tag("none");  // compressionName, a pascal string (5 bytes, padded)
    u8(0);
  }
  end_chunk();
  if (!a.inst_after) put_inst_mark(a);
  begin_chunk("SSND");
  b32(a.ssnd_offset); b32(0);
  for (std::uint32_t k = 0; k < a.ssnd_offset; ++k) u8(0xEE);
  for (std::uint32_t i = 0; i < a.frames * a.channels; ++i) put_sample(i, a);
  end_chunk();
  if (a.inst_after) put_inst_mark(a);
  if (a.odd_unpadded_last) { begin_chunk("ANNO"); u8('q'); end_chunk(false); }
  end_form();
}

struct Svx {
  std::uint32_t one_shot = 100, repeat = 0;
  unsigned rate = 8363;
  unsigned octaves = 1;
  unsigned comp = 0;
  const char* name = nullptr;
  unsigned chan = 0;               // CHAN chunk value, 0: none
  std::uint32_t body = 0;          // bytes in BODY (0: one_shot + repeat, or the compressed size of them)
  bool vhdr_after_body = false;
};

std::int8_t svx_sample(unsigned i) { return static_cast<std::int8_t>(s16(i) >> 8); }
const int kFib[16] = {-34, -21, -13, -8, -5, -3, -2, -1, 0, 1, 2, 3, 5, 8, 13, 21};

// The Fibonacci-delta encoding of the first `n` samples, and what a decoder must give back (the encoder follows its own state, so `back` is exact).
void fib_encode(std::uint32_t n, std::int8_t* back) {
  u8(0);  // pad
  int x = svx_sample(0);
  u8(static_cast<std::uint8_t>(x));
  for (std::uint32_t i = 0; i < n; i += 2) {
    unsigned nib[2] = {0, 0};
    for (unsigned k = 0; k < 2; ++k) {
      const std::uint32_t at = i + k;
      const int want = at < n ? svx_sample(at) : x;
      unsigned best = 0;
      int best_d = 1000;
      for (unsigned c = 0; c < 16; ++c) {
        int v = x + kFib[c];
        v = v > 127 ? 127 : (v < -128 ? -128 : v);
        const int d = v > want ? v - want : want - v;
        if (d < best_d) { best_d = d; best = c; }
      }
      x += kFib[best];
      x = x > 127 ? 127 : (x < -128 ? -128 : x);
      nib[k] = best;
      if (at < n) back[at] = static_cast<std::int8_t>(x);
    }
    u8((nib[0] << 4) | nib[1]);
  }
}

void make_svx(const Svx& s, std::int8_t* back = nullptr) {
  begin_form("8SVX");
  const auto vhdr = [&]() {
    begin_chunk("VHDR");
    b32(s.one_shot); b32(s.repeat); b32(32);
    b16(s.rate); u8(s.octaves); u8(s.comp); b32(0x10000);
    end_chunk();
  };
  if (!s.vhdr_after_body) vhdr();
  if (s.name != nullptr) { begin_chunk("NAME"); for (const char* c = s.name; *c != '\0'; ++c) u8(static_cast<std::uint8_t>(*c)); end_chunk(); }
  if (s.chan != 0) { begin_chunk("CHAN"); b32(s.chan); end_chunk(); }
  begin_chunk("BODY");
  const std::uint32_t n = s.one_shot + s.repeat;
  if (s.comp == 1) {
    std::int8_t scratch[kPcm];
    fib_encode(n, back != nullptr ? back : scratch);
  } else {
    const std::uint32_t bytes = s.body != 0 ? s.body : n;
    for (std::uint32_t i = 0; i < bytes; ++i) u8(static_cast<std::uint8_t>(svx_sample(i)));
  }
  end_chunk();
  if (s.vhdr_after_body) vhdr();
  end_form();
}

void reset() {
  g_arena.project.reset();
  g_registry = formats::FormatRegistry{};
  formats::register_builtin_formats(g_registry, g_storage);
}

// Runs the registry's reader on the file in g_bytes to the end; `cut` shortens the file, `size_hint` is what the platform knows of the size.
Progress read(std::uint16_t slot, const char* name, bool keep_stereo = false, std::size_t pcm_capacity = kPcm, std::size_t budget = 4096,
              std::size_t max_per_read = static_cast<std::size_t>(-1), std::uint32_t block_every = 0, std::uint16_t wavetable = formats::kWtAuto,
              std::uint32_t size_hint = 0) {
  const formats::FormatDesc* d = g_registry.find_id("iff-sample");
  if (d == nullptr) {
    return Progress::Failed;
  }
  test::MemorySource src(g_bytes, g_n);
  src.max_per_read = max_per_read;
  src.block_every = block_every;
  formats::ReadParams p;
  p.index = slot;
  p.name = name;
  p.pcm = g_pcm;
  p.pcm_capacity = pcm_capacity;
  p.keep_stereo = keep_stereo;
  p.wavetable = wavetable;
  p.size_hint = size_hint;
  d->reader->begin(src, g_arena, p);
  unsigned guard = 0;
  Progress pr = d->reader->progress();
  while (pr == Progress::Running && ++guard < 400000) {
    pr = d->reader->step(budget);
  }
  return pr;
}

const formats::FormatReport& rep() { return g_registry.find_id("iff-sample")->reader->report(); }

std::uint16_t pos16(std::uint32_t frame, std::uint32_t frames) {
  return static_cast<std::uint16_t>((static_cast<std::uint64_t>(frame) * 65535u + frames / 2u) / frames);
}

void ext_bytes(std::uint8_t out[10], unsigned exp, std::uint64_t mant, bool negative = false) {
  out[0] = static_cast<std::uint8_t>((negative ? 0x80u : 0u) | (exp >> 8));
  out[1] = static_cast<std::uint8_t>(exp);
  for (int i = 0; i < 8; ++i) out[2 + i] = static_cast<std::uint8_t>(mant >> (56 - 8 * i));
}

}  // namespace

TEST_CASE("iff sample: the 80-bit extended rate is decoded exactly, with integers, for the usual rates and refused at the edges") {
  std::uint32_t hz = 0;
  for (const std::uint32_t rate : {8000u, 11025u, 16000u, 22050u, 32000u, 44100u, 48000u, 88200u, 96000u, 192000u, 1u, 8363u, 16726u, 0xFFFFFFFFu}) {
    begin_form("AIFF");
    g_n = 0;
    ext80(rate);
    hz = 0;
    CHECK_MESSAGE(formats::iff::decode_extended_rate(g_bytes, hz), rate);
    CHECK(hz == rate);
  }
  // the bytes a real writer leaves for 44100 (Apple's: 40 0E AC 44 00 00 00 00 00 00)
  const std::uint8_t real[10] = {0x40, 0x0E, 0xAC, 0x44, 0, 0, 0, 0, 0, 0};
  CHECK(formats::iff::decode_extended_rate(real, hz));
  CHECK(hz == 44100u);
  std::uint8_t e[10];
  // not whole: rounded to nearest, half up (44100.5 -> 44101, 44100.25 -> 44100)
  ext_bytes(e, 16383 + 15, (44100ull << 48) | (1ull << 47));
  CHECK(formats::iff::decode_extended_rate(e, hz));
  CHECK(hz == 44101u);
  ext_bytes(e, 16383 + 15, (44100ull << 48) | (1ull << 46));
  CHECK(formats::iff::decode_extended_rate(e, hz));
  CHECK(hz == 44100u);
  // refused: zero, negative, infinity, NaN, below one half, 2^32 and over, 2^63 and over, an exponent of nothing
  hz = 777;
  ext_bytes(e, 0, 0);
  CHECK_FALSE(formats::iff::decode_extended_rate(e, hz));
  ext_bytes(e, 16383 + 15, 44100ull << 48, true);
  CHECK_FALSE(formats::iff::decode_extended_rate(e, hz));
  ext_bytes(e, 0x7FFF, 1ull << 63);
  CHECK_FALSE(formats::iff::decode_extended_rate(e, hz));
  ext_bytes(e, 0x7FFF, (1ull << 63) | 1);
  CHECK_FALSE(formats::iff::decode_extended_rate(e, hz));
  ext_bytes(e, 16383 - 2, 1ull << 63);  // 0.25
  CHECK_FALSE(formats::iff::decode_extended_rate(e, hz));
  ext_bytes(e, 16383 + 32, 1ull << 63);  // 2^32
  CHECK_FALSE(formats::iff::decode_extended_rate(e, hz));
  ext_bytes(e, 16383 + 63, 1ull << 63);
  CHECK_FALSE(formats::iff::decode_extended_rate(e, hz));
  ext_bytes(e, 16383 + 0x3000, ~0ull);
  CHECK_FALSE(formats::iff::decode_extended_rate(e, hz));
  ext_bytes(e, 5, ~0ull);  // a denormal-ish exponent
  CHECK_FALSE(formats::iff::decode_extended_rate(e, hz));
  CHECK(hz == 777u);  // untouched on a refusal
  ext_bytes(e, 16383 + 31, ~0ull);  // 2^32 - 0.5 rounds to 2^32: too big
  CHECK_FALSE(formats::iff::decode_extended_rate(e, hz));
  ext_bytes(e, 16383 - 1, 1ull << 63);  // 0.5: below one hertz
  CHECK_FALSE(formats::iff::decode_extended_rate(e, hz));
  ext_bytes(e, 16383, 1ull << 63);  // exactly 1
  CHECK(formats::iff::decode_extended_rate(e, hz));
  CHECK(hz == 1u);
}

TEST_CASE("iff sample: registered as a sample format, found by its names and by the form type, read only") {
  reset();
  CHECK(g_registry.count() == 13u);
  const formats::FormatDesc* d = g_registry.find_id("iff-sample");
  NEED(d != nullptr);
  CHECK(d->kind == formats::FormatKind::Sample);
  CHECK(d->writer == nullptr);
  CHECK(d->reader != nullptr);
  for (const char* n : {"kick.aif", "kick.AIFF", "a/b/kick.aifc", "x.8svx", "x.8SVX", "x.svx", "x.iff"}) {
    CHECK_MESSAGE(g_registry.find_name(n) == d, n);
  }
  CHECK(g_registry.find_name("kick.wav") == g_registry.find_id("wav"));
  make_aiff(Aiff{});
  CHECK(g_registry.sniff(g_bytes, 16, static_cast<std::uint32_t>(g_n)) == d);
  CHECK(g_registry.identify("kick.aif", g_bytes, 16, static_cast<std::uint32_t>(g_n)) == d);
  make_svx(Svx{});
  CHECK(g_registry.sniff(g_bytes, 16, static_cast<std::uint32_t>(g_n)) == d);
  CHECK(g_registry.sniff(g_bytes, 11, 100) == nullptr);  // (too short to show the form type: the native project is a FORM too)
  // another IFF is not claimed
  std::memcpy(g_bytes + 8, "ILBM", 4);
  CHECK(g_registry.sniff(g_bytes, 16, 100) == nullptr);
  CHECK(g_registry.identify("pic.aif", g_bytes, 16, 100) == nullptr);
  CHECK(formats::iff::sniff(reinterpret_cast<const std::uint8_t*>("RIFF\0\0\0\0WAVE"), 12, 0) == false);
}

TEST_CASE("iff sample: AIFF 16-bit mono: the audio, the rate kept, the instrument named after the file, no loop") {
  reset();
  Aiff a;
  a.frames = 300;
  a.rate = 44100;
  make_aiff(a);
  NEED(read(4, "kicks/Kick 01.aiff") == Progress::Done);
  CHECK(rep().error == FormatError::None);
  CHECK(rep().instruments == 1u);
  CHECK(rep().pcm_frames == 300u);
  CHECK(rep().pcm_channels == 1);
  CHECK(rep().sanitised == 0u);
  for (unsigned i = 0; i < 300; ++i) {
    NEED(g_pcm[i] == s16(i));
  }
  const model::Instrument* in = g_arena.project.instrument(4);
  NEED(in != nullptr);
  CHECK(std::strcmp(in->name, "Kick 01") == 0);
  CHECK(std::strcmp(in->sample_path, "samples/instr05.wav") == 0);  // our own path: the audio is stored as today
  CHECK(in->sample_rate == 44100u);
  CHECK(in->sampler.play_mode == 0);  // one shot
  CHECK(in->sampler.start == 0);
  CHECK(in->sampler.end == 65535);
  CHECK(g_arena.project.check_invariants());
}

TEST_CASE("iff sample: AIFF rates are kept as they are, not resampled") {
  for (const std::uint32_t rate : {8000u, 22050u, 44100u, 48000u, 96000u}) {
    reset();
    Aiff a;
    a.rate = rate;
    make_aiff(a);
    NEED(read(0, "r.aif") == Progress::Done);
    CHECK_MESSAGE(g_arena.project.instrument(0)->sample_rate == rate, rate);
  }
}

TEST_CASE("iff sample: AIFF 8-bit, 24-bit (exact where the low byte is zero, dithered by one step otherwise) and odd depths") {
  reset();
  Aiff a;
  a.frames = 200;
  a.bits = 8;
  make_aiff(a);
  NEED(read(0, "e.aif") == Progress::Done);
  for (unsigned i = 0; i < 200; ++i) NEED(g_pcm[i] == static_cast<std::int16_t>((s16(i) >> 8) * 256));
  a.bits = 24;
  make_aiff(a);
  NEED(read(0, "t.aif") == Progress::Done);
  for (unsigned i = 0; i < 200; ++i) NEED(g_pcm[i] == s16(i));  // nothing below the 16-bit step: exact
  a.low_bytes = true;
  make_aiff(a);
  NEED(read(0, "t.aif") == Progress::Done);
  unsigned moved = 0;
  for (unsigned i = 0; i < 200; ++i) {
    const int d = g_pcm[i] - s16(i);
    NEED((d >= -1 && d <= 2));
    moved += d != 0 ? 1u : 0u;
  }
  CHECK(moved > 0u);
  Aiff again = a;
  make_aiff(again);
  std::int16_t first[200];
  std::memcpy(first, g_pcm, sizeof first);
  NEED(read(0, "t.aif") == Progress::Done);
  CHECK(std::memcmp(first, g_pcm, sizeof first) == 0);  // a pure function of the input (fixed dither seed)
  a.low_bytes = false;
  a.bits = 12;  // 12 bits in a 16-bit container, left-justified: read as 16
  make_aiff(a);
  NEED(read(0, "w.aif") == Progress::Done);
  for (unsigned i = 0; i < 200; ++i) NEED(g_pcm[i] == s16(i));
}

TEST_CASE("iff sample: AIFF stereo is mixed to mono as the WAV reader does, or kept interleaved") {
  reset();
  Aiff a;
  a.frames = 120;
  a.channels = 2;
  make_aiff(a);
  NEED(read(0, "s.aif") == Progress::Done);
  CHECK(rep().pcm_channels == 1);
  CHECK(rep().pcm_frames == 120u);
  for (unsigned i = 0; i < 120; ++i) {
    const int l = s16(2 * i), r = s16(2 * i + 1);
    const int sum = l + r;
    const int mean_floor = sum >> 1;
    NEED((g_pcm[i] >= mean_floor - 1 && g_pcm[i] <= mean_floor + 1));
    if ((sum & 1) == 0) NEED(g_pcm[i] == mean_floor);  // a pair that has a whole 16-bit mean passes exactly
  }
  a.frames = 50;
  make_aiff(a);
  NEED(read(0, "s.aif", true) == Progress::Done);
  CHECK(rep().pcm_channels == 2);
  CHECK(rep().pcm_frames == 50u);
  for (unsigned i = 0; i < 100; ++i) NEED(g_pcm[i] == s16(i));
  // 100 samples do not fit 99
  CHECK(read(0, "s.aif", true, 99) == Progress::Failed);
  CHECK(rep().error == FormatError::LimitExceeded);
  // L = R passes bit-exact in 24 bits too
  a.frames = 10;
  a.bits = 8;
  make_aiff(a);
  NEED(read(0, "s.aif") == Progress::Done);
}

TEST_CASE("iff sample: AIFF-C NONE and sowt are read, every other compression is refused with a message") {
  reset();
  Aiff a;
  a.type = "AIFC";
  a.frames = 100;
  make_aiff(a);
  NEED(read(0, "c.aifc") == Progress::Done);
  for (unsigned i = 0; i < 100; ++i) NEED(g_pcm[i] == s16(i));
  a.sowt = true;  // little-endian 16-bit
  make_aiff(a);
  NEED(read(0, "c.aifc") == Progress::Done);
  for (unsigned i = 0; i < 100; ++i) NEED(g_pcm[i] == s16(i));
  a.sowt = false;
  for (const char* comp : {"ulaw", "alaw", "fl32", "FL64", "ima4", "MAC3", "ULAW", "in32"}) {
    a.comp = comp;
    make_aiff(a);
    reset();
    CHECK_MESSAGE(read(0, "c.aifc") == Progress::Failed, comp);
    CHECK(rep().error == FormatError::Unsupported);
    CHECK(std::strstr(rep().detail, "compression") != nullptr);
    CHECK(g_arena.project.instrument(0) == nullptr);
  }
}

TEST_CASE("iff sample: AIFF loop from INST and MARK, in either order, forward or ping-pong, as the OG's 16-bit positions") {
  reset();
  const Marker marks[] = {{7, 5, 3}, {1, 100, 0}, {2, 600, 4}, {9, 900, 1}};  // names of every length: the pad rule
  Aiff a;
  a.frames = 1000;
  a.marks = marks;
  a.mark_count = 4;
  a.loop_mode = 1;
  for (const bool after : {false, true}) {
    a.inst_after = after;
    a.loop_mode = 1;
    make_aiff(a);
    NEED(read(0, "l.aif") == Progress::Done);
    const model::Instrument* in = g_arena.project.instrument(0);
    NEED(in != nullptr);
    CHECK(in->sampler.play_mode == 1);
    CHECK(in->sampler.loop_start == pos16(100, 1000));
    CHECK(in->sampler.loop_end == pos16(600, 1000));
    CHECK(rep().sanitised == 0u);
    a.loop_mode = 2;
    make_aiff(a);
    NEED(read(0, "l.aif") == Progress::Done);
    CHECK(g_arena.project.instrument(0)->sampler.play_mode == 3);
  }
  // sustain loop mode 0: no loop
  a.loop_mode = 0;
  make_aiff(a);
  NEED(read(0, "l.aif") == Progress::Done);
  CHECK(g_arena.project.instrument(0)->sampler.play_mode == 0);
  // INST and MARK with no loop mode, or markers it does not find, or begin >= end: a one-shot, the loop counted
  a.loop_mode = 1;
  a.loop_begin = 2;
  a.loop_end = 1;  // backwards
  make_aiff(a);
  NEED(read(0, "l.aif") == Progress::Done);
  CHECK(g_arena.project.instrument(0)->sampler.play_mode == 0);
  CHECK(rep().sanitised == 1u);
  a.loop_begin = 1;
  a.loop_end = 55;  // no such marker
  make_aiff(a);
  NEED(read(0, "l.aif") == Progress::Done);
  CHECK(g_arena.project.instrument(0)->sampler.play_mode == 0);
  CHECK(rep().sanitised == 1u);
  // an end marker past the audio is held to the audio's end
  const Marker far[] = {{1, 10, 0}, {2, 5000, 0}};
  a.marks = far;
  a.mark_count = 2;
  a.loop_end = 2;
  make_aiff(a);
  NEED(read(0, "l.aif") == Progress::Done);
  CHECK(g_arena.project.instrument(0)->sampler.play_mode == 1);
  CHECK(g_arena.project.instrument(0)->sampler.loop_end == 65535);
  // an unknown loop mode (3) is dropped
  a.loop_mode = 3;
  make_aiff(a);
  NEED(read(0, "l.aif") == Progress::Done);
  CHECK(g_arena.project.instrument(0)->sampler.play_mode == 0);
  CHECK(rep().sanitised == 1u);
}

TEST_CASE("iff sample: more markers than the table keeps are counted; a loop that names a later one is dropped") {
  reset();
  Marker marks[30];
  for (unsigned k = 0; k < 30; ++k) marks[k] = {k + 1, 10 + k, k % 3};
  Aiff a;
  a.frames = 500;
  a.marks = marks;
  a.mark_count = 30;
  a.loop_mode = 1;
  a.loop_begin = 1;
  a.loop_end = 5;
  make_aiff(a);
  NEED(read(0, "m.aif") == Progress::Done);
  CHECK(g_arena.project.instrument(0)->sampler.play_mode == 1);
  CHECK(rep().sanitised == 30u - formats::iff::kMaxMarkers);
  a.loop_end = 30;
  make_aiff(a);
  NEED(read(0, "m.aif") == Progress::Done);
  CHECK(g_arena.project.instrument(0)->sampler.play_mode == 0);
  CHECK(rep().sanitised == 30u - formats::iff::kMaxMarkers + 1u);
}

TEST_CASE("iff sample: chunks of odd size are padded, and an odd last chunk may lack its pad byte") {
  reset();
  Aiff a;
  a.frames = 64;
  a.odd_before = true;
  make_aiff(a);
  NEED(read(0, "o.aif") == Progress::Done);
  for (unsigned i = 0; i < 64; ++i) NEED(g_pcm[i] == s16(i));
  CHECK(rep().sanitised == 0u);
  a.odd_before = false;
  a.odd_unpadded_last = true;
  make_aiff(a);
  NEED(read(0, "o.aif") == Progress::Done);
  CHECK(rep().sanitised == 0u);
  // with the file's size known too
  NEED(read(0, "o.aif", false, kPcm, 4096, static_cast<std::size_t>(-1), 0, formats::kWtAuto, static_cast<std::uint32_t>(g_n)) == Progress::Done);
  CHECK(rep().sanitised == 0u);
}

TEST_CASE("iff sample: the SSND offset is skipped; fewer frames than COMM says are read as far as they go; more are not read") {
  reset();
  Aiff a;
  a.frames = 80;
  a.ssnd_offset = 6;
  make_aiff(a);
  NEED(read(0, "s.aif") == Progress::Done);
  for (unsigned i = 0; i < 80; ++i) NEED(g_pcm[i] == s16(i));
  a.ssnd_offset = 0;
  a.comm_frames = 200;  // COMM promises 200, SSND holds 80
  make_aiff(a);
  NEED(read(0, "s.aif") == Progress::Done);
  CHECK(rep().pcm_frames == 80u);
  CHECK(rep().sanitised == 1u);
  a.comm_frames = 30;  // COMM says 30: the rest is not audio
  make_aiff(a);
  NEED(read(0, "s.aif") == Progress::Done);
  CHECK(rep().pcm_frames == 30u);
  CHECK(rep().sanitised == 0u);
  // an offset past the chunk
  a.comm_frames = 0;
  a.ssnd_offset = 0;
  make_aiff(a);
  const std::size_t ssnd = 12 + 8 + 18 + 0;  // FORM head + COMM chunk head + COMM = SSND chunk at 38
  CHECK(std::memcmp(g_bytes + ssnd, "SSND", 4) == 0);
  g_bytes[ssnd + 8] = 0x7F;  // offset 0x7F000000
  CHECK(read(0, "s.aif") == Progress::Failed);
  CHECK(rep().error == FormatError::Malformed);
}

TEST_CASE("iff sample: refusals leave the project and the slot as they were: depth, channels, rate, order, duplicates, no audio") {
  reset();
  const std::uint32_t generation = g_arena.project.generation();
  Aiff a;
  a.frames = 50;
  const auto check = [&](FormatError e, const char* why) {
    CHECK_MESSAGE(read(3, "x.aif") == Progress::Failed, why);
    CHECK_MESSAGE(rep().error == e, why);
    CHECK_MESSAGE(rep().detail[0] != '\0', why);
    CHECK(g_arena.project.generation() == generation);
    CHECK(g_arena.project.instrument(3) == nullptr);
  };
  a.bits = 32;
  make_aiff(a);
  check(FormatError::Unsupported, "32 bits");
  a.bits = 16;
  a.channels = 3;
  make_aiff(a);
  check(FormatError::Unsupported, "3 channels");
  a.channels = 1;
  a.rate = 0;
  make_aiff(a);
  check(FormatError::Malformed, "rate 0");
  a.rate = 22050;
  make_aiff(a);
  const std::size_t comm = 20;  // COMM chunk head at 12; its body from 20
  std::uint8_t keep[18];
  std::memcpy(keep, g_bytes + comm, 18);
  g_bytes[comm + 1] = 0;  // 0 channels
  check(FormatError::Malformed, "0 channels");
  std::memcpy(g_bytes + comm, keep, 18);
  g_bytes[comm + 7] = 0;  // 0 bits
  g_bytes[comm + 6] = 0;
  check(FormatError::Malformed, "0 bits");
  std::memcpy(g_bytes + comm, keep, 18);
  std::memset(g_bytes + comm + 2, 0, 4);  // 0 frames
  check(FormatError::Malformed, "0 frames");
  std::memcpy(g_bytes + comm, keep, 18);
  g_bytes[comm + 8] = 0xFF;  // infinity
  g_bytes[comm + 9] = 0xFF;
  check(FormatError::Malformed, "infinite rate");
  std::memcpy(g_bytes + comm, keep, 18);
  g_bytes[comm + 8] |= 0x80;  // negative rate
  check(FormatError::Malformed, "negative rate");
  // SSND before COMM
  begin_form("AIFF");
  begin_chunk("SSND"); b32(0); b32(0); b16(1); b16(2); end_chunk();
  begin_chunk("COMM"); b16(1); b32(2); b16(16); ext80(8000); end_chunk();
  end_form();
  check(FormatError::Malformed, "SSND before COMM");
  // two COMM chunks
  make_aiff(a);
  begin_form("AIFF");
  for (int k = 0; k < 2; ++k) { begin_chunk("COMM"); b16(1); b32(2); b16(16); ext80(8000); end_chunk(); }
  end_form();
  check(FormatError::Malformed, "two COMM");
  // no audio chunk, no COMM at all
  begin_form("AIFF");
  begin_chunk("COMM"); b16(1); b32(2); b16(16); ext80(8000); end_chunk();
  end_form();
  check(FormatError::Malformed, "no SSND");
  begin_form("AIFF");
  begin_chunk("ANNO"); u8('a'); u8('b'); end_chunk();
  end_form();
  check(FormatError::Malformed, "no COMM");
  // an SSND with no whole frame
  begin_form("AIFF");
  begin_chunk("COMM"); b16(2); b32(5); b16(16); ext80(8000); end_chunk();
  begin_chunk("SSND"); b32(0); b32(0); b16(1); end_chunk();
  end_form();
  check(FormatError::Malformed, "SSND shorter than one frame");
  // a COMM chunk that is too short, an AIFF-C COMM with no room for the compression type
  begin_form("AIFF");
  begin_chunk("COMM"); b16(1); b32(2); b16(16); b16(0); end_chunk();
  end_form();
  check(FormatError::Malformed, "short COMM");
  begin_form("AIFC");
  begin_chunk("COMM"); b16(1); b32(2); b16(16); ext80(8000); end_chunk();
  end_form();
  check(FormatError::Malformed, "AIFC COMM of AIFF size");
  // not an AIFF at all
  begin_form("ILBM");
  end_form();
  check(FormatError::Unsupported, "ILBM");
  make_aiff(a);
  std::memcpy(g_bytes, "RIFF", 4);
  check(FormatError::BadMagic, "RIFF");
  make_aiff(a);
  g_bytes[7] = 2;  // FORM size 2
  g_bytes[4] = g_bytes[5] = g_bytes[6] = 0;
  check(FormatError::Malformed, "FORM too small");
  CHECK(read(64, "x.aif") == Progress::Failed);  // no such slot
  CHECK(rep().error == FormatError::LimitExceeded);
  CHECK(g_arena.project.check_invariants());
}

TEST_CASE("iff sample: chunk sizes are checked against the form and the file before a byte of audio is written") {
  reset();
  Aiff a;
  a.frames = 100;
  make_aiff(a);
  const std::size_t ssnd = 12 + 8 + 18;  // the SSND chunk head
  NEED(std::memcmp(g_bytes + ssnd, "SSND", 4) == 0);
  std::uint8_t pristine[kMaxFile];
  const std::size_t total = g_n;
  std::memcpy(pristine, g_bytes, total);
  for (unsigned k = 0; k < kPcm + 16; ++k) g_pcm[k] = 0x5A5A;
  // SSND says more than the form holds
  g_bytes[ssnd + 7] = 0xFF;
  g_bytes[ssnd + 6] = 0xFF;
  CHECK(read(0, "z.aif") == Progress::Failed);
  CHECK(rep().error == FormatError::Malformed);
  CHECK(g_pcm[0] == 0x5A5A);  // nothing was written
  // SSND size 0xFFFFFFFF: no overflow
  std::memcpy(g_bytes, pristine, total);
  g_bytes[ssnd + 4] = g_bytes[ssnd + 5] = g_bytes[ssnd + 6] = g_bytes[ssnd + 7] = 0xFF;
  CHECK(read(0, "z.aif") == Progress::Failed);
  CHECK(rep().error == FormatError::Malformed);
  // with the file's size known and the form's size a lie: the file is shorter
  std::memcpy(g_bytes, pristine, total);
  g_bytes[4] = 0x7F;  // a huge FORM
  g_n = total - 10;
  CHECK(read(0, "z.aif", false, kPcm, 4096, static_cast<std::size_t>(-1), 0, formats::kWtAuto, static_cast<std::uint32_t>(g_n)) == Progress::Failed);
  CHECK(rep().error == FormatError::Truncated);
  CHECK(g_pcm[0] == 0x5A5A);  // refused by the sizes alone: no audio was written
  // an unknown chunk of size 0xFFFFFFFF before the audio
  begin_form("AIFF");
  begin_chunk("COMM"); b16(1); b32(2); b16(16); ext80(8000); end_chunk();
  tag("JUNK"); b32(0xFFFFFFFFu); b32(0);
  end_form();
  CHECK(read(0, "z.aif") == Progress::Failed);
  CHECK(rep().error == FormatError::Malformed);
  // a FORM size of -1 (a streamed file) reads to the end of the source
  std::memcpy(g_bytes, pristine, total);
  g_n = total;
  g_bytes[4] = g_bytes[5] = g_bytes[6] = g_bytes[7] = 0xFF;
  CHECK(read(0, "z.aif") == Progress::Done);
  CHECK(rep().pcm_frames == 100u);
  // a few bytes of junk after the last chunk are not read
  std::memcpy(g_bytes, pristine, total);
  g_n = total;
  u8(1); u8(2); u8(3);
  CHECK(read(0, "z.aif") == Progress::Done);
  CHECK(rep().sanitised == 0u);  // (outside the form)
  // a trailing chunk that runs past the form, after the audio: the audio stands, the trouble is counted
  make_aiff(a);
  tag("ANNO"); b32(500);
  end_form();
  CHECK(read(0, "z.aif") == Progress::Done);
  CHECK(rep().sanitised == 1u);
  CHECK(rep().pcm_frames == 100u);
}

TEST_CASE("iff sample: cut at every offset the read finishes with a whole instrument or fails Truncated, and a source of one byte, or one that blocks, agrees") {
  reset();
  const Marker marks[] = {{1, 10, 2}, {2, 40, 3}};
  Aiff a;
  a.frames = 60;
  a.channels = 2;
  a.bits = 24;
  a.marks = marks;
  a.mark_count = 2;
  a.loop_mode = 1;
  a.inst_after = true;
  make_aiff(a);
  const std::size_t whole = g_n;
  unsigned done = 0;
  for (std::size_t cut = 0; cut < whole; ++cut) {
    g_arena.project.reset();
    g_n = cut;
    const Progress p = read(5, "cut.aif");
    NEED(p != Progress::Running);
    if (p == Progress::Done) {
      ++done;
      CHECK(rep().pcm_frames == 60u);
      CHECK(g_arena.project.instrument(5) != nullptr);
    } else {
      NEED(rep().error == FormatError::Truncated);
      CHECK(g_arena.project.instrument(5) == nullptr);
    }
  }
  CHECK(done > 0u);  // the cuts inside the trailing INST / MARK chunks still make a sample
  CHECK(done < whole / 2);
  g_n = whole;
  g_arena.project.reset();
  NEED(read(5, "slow.aif", false, kPcm, 1, 1, 3) == Progress::Done);
  const std::int16_t first = g_pcm[0], last = g_pcm[59];
  const std::uint16_t le = g_arena.project.instrument(5)->sampler.loop_end;
  g_arena.project.reset();
  NEED(read(5, "fast.aif") == Progress::Done);
  CHECK(g_pcm[0] == first);
  CHECK(g_pcm[59] == last);
  CHECK(g_arena.project.instrument(5)->sampler.loop_end == le);
  // and one that fails, or lies
  CHECK(read(5, "f.aif", false, kPcm, 4096, static_cast<std::size_t>(-1), 0) == Progress::Done);
  test::MemorySource src(g_bytes, g_n);
  src.lie = test::MemorySource::Lie::TooMany;
  formats::ReadParams p;
  p.name = "lie.aif";
  p.pcm = g_pcm;
  p.pcm_capacity = kPcm;
  formats::iff::SampleReader* r = &g_storage.iff_reader;
  r->begin(src, g_arena, p);
  CHECK(r->step(1000) == Progress::Failed);
  CHECK(r->report().error == FormatError::SourceFailed);
  test::MemorySource src2(g_bytes, g_n);
  src2.fail_at = 100;
  r->begin(src2, g_arena, p);
  unsigned guard = 0;
  while (r->progress() == Progress::Running && ++guard < 1000) (void)r->step(50);
  CHECK(r->progress() == Progress::Failed);
  CHECK(r->report().error == FormatError::SourceFailed);
}

TEST_CASE("iff sample: 8SVX one-shot: the rate kept, signed 8-bit audio, a one-shot instrument") {
  reset();
  Svx s;
  s.one_shot = 400;
  s.rate = 16726;
  make_svx(s);
  NEED(read(2, "amiga/Bass.8svx") == Progress::Done);
  CHECK(rep().pcm_frames == 400u);
  CHECK(rep().pcm_channels == 1);
  CHECK(rep().sanitised == 0u);
  for (unsigned i = 0; i < 400; ++i) NEED(g_pcm[i] == svx_sample(i) * 256);
  const model::Instrument* in = g_arena.project.instrument(2);
  NEED(in != nullptr);
  CHECK(std::strcmp(in->name, "Bass") == 0);
  CHECK(in->sample_rate == 16726u);
  CHECK(in->sampler.play_mode == 0);
  CHECK(in->sampler.start == 0);
  CHECK(in->sampler.end == 65535);
  s.rate = 8363;
  make_svx(s);
  NEED(read(2, "b.svx") == Progress::Done);
  CHECK(g_arena.project.instrument(2)->sample_rate == 8363u);
}

TEST_CASE("iff sample: 8SVX one-shot and repeat parts: the repeat part loops forward") {
  reset();
  Svx s;
  s.one_shot = 100;
  s.repeat = 300;
  make_svx(s);
  NEED(read(0, "loop.8svx") == Progress::Done);
  CHECK(rep().pcm_frames == 400u);
  const model::Instrument* in = g_arena.project.instrument(0);
  NEED(in != nullptr);
  CHECK(in->sampler.play_mode == 1);
  CHECK(in->sampler.loop_start == pos16(100, 400));
  CHECK(in->sampler.loop_end == pos16(400, 400));
  s.one_shot = 0;  // all repeat
  s.repeat = 128;
  make_svx(s);
  NEED(read(0, "loop.8svx") == Progress::Done);
  CHECK(g_arena.project.instrument(0)->sampler.play_mode == 1);
  CHECK(g_arena.project.instrument(0)->sampler.loop_start == 0);
  CHECK(g_arena.project.instrument(0)->sampler.loop_end == 65535);
}

TEST_CASE("iff sample: 8SVX names the instrument, takes the first octave of several, and tells what it left out") {
  reset();
  Svx s;
  s.one_shot = 64;
  s.name = "Square wave\x01  ";
  s.octaves = 3;
  s.body = 64 + 128 + 256;  // the three octaves, each twice as long as the one before
  make_svx(s);
  NEED(read(0, "file.8svx") == Progress::Done);
  CHECK(rep().pcm_frames == 64u);
  CHECK(rep().sanitised == 1u);  // the octaves
  CHECK(std::strcmp(g_arena.project.instrument(0)->name, "Square wave?") == 0);
  for (unsigned i = 0; i < 64; ++i) NEED(g_pcm[i] == svx_sample(i) * 256);
  // CHAN: stereo, the left channel is read
  s.octaves = 1;
  s.body = 128;
  s.name = nullptr;
  s.chan = 6;
  make_svx(s);
  NEED(read(0, "st.8svx") == Progress::Done);
  CHECK(rep().pcm_frames == 64u);
  CHECK(rep().sanitised == 1u);
  CHECK(std::strcmp(g_arena.project.instrument(0)->name, "st") == 0);  // no NAME: the file's name
  // VHDR after BODY: refused (a streamed read cannot go back)
  s.chan = 0;
  s.vhdr_after_body = true;
  make_svx(s);
  CHECK(read(0, "late.8svx") == Progress::Failed);
  CHECK(rep().error == FormatError::Malformed);
}

TEST_CASE("iff sample: 8SVX Fibonacci-delta: a hand-made body, and an encoded one decoded back") {
  reset();
  // pad, initial 10, then 0x9A (+1, +2), 0xF0 (+21, -34 held to... 13 + 21 = 34, 34 - 34 = 0)
  begin_form("8SVX");
  begin_chunk("VHDR"); b32(4); b32(0); b32(16); b16(8363); u8(1); u8(1); b32(0x10000); end_chunk();
  begin_chunk("BODY"); u8(0); u8(10); u8(0x9A); u8(0xF0); end_chunk();
  end_form();
  NEED(read(0, "fib.8svx") == Progress::Done);
  CHECK(rep().pcm_frames == 4u);
  CHECK(g_pcm[0] == 11 * 256);
  CHECK(g_pcm[1] == 13 * 256);
  CHECK(g_pcm[2] == 34 * 256);
  CHECK(g_pcm[3] == 0);
  // clamped at the ends of the 8-bit range
  begin_form("8SVX");
  begin_chunk("VHDR"); b32(6); b32(0); b32(16); b16(8363); u8(1); u8(1); b32(0x10000); end_chunk();
  begin_chunk("BODY"); u8(0); u8(120); u8(0xFF); u8(0xFF); u8(0x00); end_chunk();  // +21 +21 +21 +21, -34 -34
  end_form();
  NEED(read(0, "clamp.8svx") == Progress::Done);
  CHECK(g_pcm[0] == 127 * 256);
  CHECK(g_pcm[3] == 127 * 256);
  CHECK(g_pcm[4] == 93 * 256);
  CHECK(g_pcm[5] == 59 * 256);
  // an encoded ramp of 301 samples (odd count: the last nibble pair is half used)
  Svx s;
  s.one_shot = 100;
  s.repeat = 201;
  s.comp = 1;
  std::int8_t back[kPcm];
  make_svx(s, back);
  NEED(read(0, "enc.8svx") == Progress::Done);
  CHECK(rep().pcm_frames == 301u);
  CHECK(rep().sanitised == 0u);
  for (unsigned i = 0; i < 301; ++i) NEED(g_pcm[i] == back[i] * 256);
  CHECK(g_arena.project.instrument(0)->sampler.play_mode == 1);
  // a body shorter than the header says: read as far as it goes
  begin_form("8SVX");
  begin_chunk("VHDR"); b32(100); b32(0); b32(16); b16(8363); u8(1); u8(1); b32(0x10000); end_chunk();
  begin_chunk("BODY"); u8(0); u8(10); u8(0x99); u8(0x99); end_chunk();
  end_form();
  NEED(read(0, "short.8svx") == Progress::Done);
  CHECK(rep().pcm_frames == 4u);
  CHECK(rep().sanitised == 1u);
  // a body too short to unpack
  begin_form("8SVX");
  begin_chunk("VHDR"); b32(100); b32(0); b32(16); b16(8363); u8(1); u8(1); b32(0x10000); end_chunk();
  begin_chunk("BODY"); u8(0); u8(10); end_chunk();
  end_form();
  CHECK(read(0, "tiny.8svx") == Progress::Failed);
  CHECK(rep().error == FormatError::Malformed);
}

TEST_CASE("iff sample: 8SVX refusals: other compressions, no rate, no samples, BODY first, truncation at every offset") {
  reset();
  const std::uint32_t generation = g_arena.project.generation();
  Svx s;
  s.one_shot = 50;
  const auto check = [&](FormatError e, const char* why) {
    CHECK_MESSAGE(read(1, "x.8svx") == Progress::Failed, why);
    CHECK_MESSAGE(rep().error == e, why);
    CHECK(g_arena.project.generation() == generation);
  };
  s.comp = 2;
  make_svx(s);
  check(FormatError::Unsupported, "compression 2");
  s.comp = 255;
  make_svx(s);
  check(FormatError::Unsupported, "compression 255");
  s.comp = 0;
  s.rate = 0;
  make_svx(s);
  check(FormatError::Malformed, "rate 0");
  s.rate = 8363;
  s.one_shot = 0;
  s.repeat = 0;
  s.body = 10;
  make_svx(s);
  check(FormatError::Malformed, "no samples");
  begin_form("8SVX");
  begin_chunk("BODY"); u8(1); u8(2); end_chunk();
  end_form();
  check(FormatError::Malformed, "BODY before VHDR");
  begin_form("8SVX");
  begin_chunk("VHDR"); b32(1); b32(0); end_chunk();
  end_form();
  check(FormatError::Malformed, "short VHDR");
  begin_form("8SVX");
  end_form();
  check(FormatError::Malformed, "empty form");
  s.one_shot = 3000;  // does not fit
  s.repeat = 0;
  s.body = 0;
  make_svx(s);
  CHECK(read(1, "big.8svx", false, 2000) == Progress::Failed);
  CHECK(rep().error == FormatError::LimitExceeded);
  s.one_shot = 40;
  s.repeat = 10;
  make_svx(s);
  const std::size_t whole = g_n;
  for (std::size_t cut = 0; cut < whole; ++cut) {
    g_n = cut;
    const Progress p = read(1, "cut.8svx");
    NEED(p == Progress::Failed || p == Progress::Done);
    if (p == Progress::Failed) {
      NEED(rep().error == FormatError::Truncated);
    } else {
      CHECK(cut >= whole - 1);  // (a missing final pad byte is no damage)
    }
  }
  g_n = whole;
  NEED(read(1, "ok.8svx", false, kPcm, 1, 1, 2) == Progress::Done);
}

TEST_CASE("iff sample: the load screen's wavetable choice applies as it does to a WAV") {
  reset();
  Aiff a;
  a.frames = 512;
  make_aiff(a);
  NEED(read(0, "w.aif", false, kPcm, 4096, static_cast<std::size_t>(-1), 0, formats::kWtAuto) == Progress::Done);
  CHECK(g_arena.project.instrument(0)->sampler.play_mode == 0);  // a sample
  NEED(read(0, "w.aif", false, kPcm, 4096, static_cast<std::size_t>(-1), 0, formats::kWtDetect) == Progress::Done);
  CHECK(g_arena.project.instrument(0)->sampler.play_mode == 6);  // 512 frames: windows of 256
  CHECK(g_arena.project.instrument(0)->sampler.wavetable_window_size == 256);
  NEED(read(0, "w.aif", false, kPcm, 4096, static_cast<std::size_t>(-1), 0, formats::kWtOff) == Progress::Done);
  CHECK(g_arena.project.instrument(0)->sampler.play_mode == 0);
}

TEST_CASE("iff sample: cancel abandons the read before the model is touched; a finished reader stays finished") {
  reset();
  Aiff a;
  a.frames = 1500;
  make_aiff(a);
  const formats::FormatDesc* d = g_registry.find_id("iff-sample");
  NEED(d != nullptr);
  CHECK(d->reader->can_cancel());
  test::MemorySource src(g_bytes, g_n);
  formats::ReadParams p;
  p.index = 5;
  p.name = "c.aif";
  p.pcm = g_pcm;
  p.pcm_capacity = kPcm;
  d->reader->begin(src, g_arena, p);
  CHECK(d->reader->step(300) == Progress::Running);
  CHECK(d->reader->permille() > 0u);
  CHECK(d->reader->permille() < 1000u);
  const std::uint32_t generation = g_arena.project.generation();
  d->reader->cancel();
  CHECK(d->reader->progress() == Progress::Failed);
  CHECK(rep().error == FormatError::Cancelled);
  CHECK(d->reader->step(300) == Progress::Failed);
  CHECK(g_arena.project.generation() == generation);
  CHECK(g_arena.project.instrument(5) == nullptr);
  NEED(read(5, "again.aif") == Progress::Done);
  CHECK(d->reader->permille() == 1000u);
  CHECK(d->reader->step(100) == Progress::Done);
  d->reader->cancel();  // a finished run is left as it is
  CHECK(d->reader->progress() == Progress::Done);
}

TEST_CASE("iff sample: damaged files: always Done or a typed error, never a write past the room, the project always valid") {
#if defined(__arm__)
  constexpr unsigned kRounds = 300;
#else
  constexpr unsigned kRounds = 8000;
#endif
  reset();
  static std::uint8_t pristine[3][kMaxFile];
  std::size_t sizes[3];
  const Marker marks[] = {{1, 10, 2}, {2, 40, 3}};
  Aiff a;
  a.frames = 90;
  a.channels = 2;
  a.bits = 24;
  a.marks = marks;
  a.mark_count = 2;
  a.loop_mode = 1;
  a.type = "AIFC";
  make_aiff(a);
  sizes[0] = g_n;
  std::memcpy(pristine[0], g_bytes, g_n);
  Svx s;
  s.one_shot = 60;
  s.repeat = 80;
  s.name = "fuzz";
  make_svx(s);
  sizes[1] = g_n;
  std::memcpy(pristine[1], g_bytes, g_n);
  s.comp = 1;
  make_svx(s);
  sizes[2] = g_n;
  std::memcpy(pristine[2], g_bytes, g_n);
  test::Rng rng(0xA1FF5Eu);
  unsigned done = 0;
  for (unsigned round = 0; round < kRounds; ++round) {
    const unsigned which = rng.below(3);
    const std::size_t total = sizes[which];
    std::memcpy(g_bytes, pristine[which], total);
    const unsigned edits = 1 + rng.below(4);
    for (unsigned e = 0; e < edits; ++e) {
      const std::uint32_t at = rng.below(static_cast<std::uint32_t>(total));
      if (rng.below(4) == 0) {
        g_bytes[at] = rng.below(2) != 0 ? 0xFF : 0x00;
      } else {
        g_bytes[at] = rng.byte();
      }
    }
    g_n = rng.below(6) == 0 ? rng.below(static_cast<std::uint32_t>(total)) : total;
    const std::size_t cap = 4 + rng.below(kPcm - 20);
    for (unsigned k = 0; k < 16; ++k) g_pcm[cap + k] = 0x5A5A;  // a canary after the room
    const std::uint32_t before = g_arena.project.instrument_count();
    const Progress pr = read(static_cast<std::uint16_t>(rng.below(64)), "f.aif", rng.below(2) != 0, cap, 1 + rng.below(600), rng.below(3) == 0 ? 1 + rng.below(40) : static_cast<std::size_t>(-1),
                             rng.below(5) == 0 ? 2 + rng.below(4) : 0, formats::kWtAuto, rng.below(2) != 0 ? static_cast<std::uint32_t>(g_n) : 0u);
    NEED(pr != Progress::Running);
    for (unsigned k = 0; k < 16; ++k) NEED(g_pcm[cap + k] == 0x5A5A);
    CHECK(g_arena.project.check_invariants());
    if (pr == Progress::Done) {
      ++done;
      CHECK(rep().error == FormatError::None);
      CHECK(rep().instruments == 1u);
      CHECK(rep().pcm_frames >= 1u);
      CHECK(rep().pcm_frames * rep().pcm_channels <= cap);
    } else {
      CHECK(rep().error != FormatError::None);
      CHECK(rep().detail[0] != '\0');
      CHECK(g_arena.project.instrument_count() == before);  // a failure adds nothing
    }
  }
  CHECK(done > kRounds / 10);
  for (std::uint32_t i = 0; i < model::kMaxInstruments; ++i) {
    const model::Instrument* in = g_arena.project.instrument(i);
    CHECK((in == nullptr || model::validate_instrument(*in, g_arena.project.module_count()) == nullptr));
  }
}
