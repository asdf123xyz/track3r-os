// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-GRAN, BASE-04: wavetable formats on import. (1) Serum: a WAV with a 'clm ' chunk ("<!>2048 ...") is a wavetable of that frame size; the chunk read by the WAV importer (valid, before and after
// the audio, odd size and its pad byte, malformed, duplicate, truncated at every offset). (2) The rule for which WAVs are wavetables (wavetable_frame: marker, kWtDetect and the length rule
// 2048 / 1024 / 256, an explicit size, kWtOff, sizes the engine has no table for) and the instrument it makes (sample type 1, play mode 6, window size, window 0; a sample otherwise, exactly as
// before). (3) Surge .wt: the 12-byte header, float32 and int16 data (the 15-bit assumption), refusals (tag, sizes, counts, room), truncation at every offset, a source that gives one byte or blocks,
// trailing metadata, the registry's sniffing. Crafted files in memory, no heap: the same file runs as the QEMU suite `qemu_wavetable_import`.
#include <doctest/doctest.h>

#include <o3os/formats/builtin.h>
#include <o3os/formats/format.h>
#include <o3os/formats/wav_sample.h>
#include <o3os/formats/wt_surge.h>
#include <o3os/model/project.h>

#include <cstdint>
#include <cstring>

#include "support/memory_io.h"
#include "support/need.h"

using namespace o3os;
namespace wav = o3os::formats::wav;
using formats::FormatError;
using formats::Progress;

namespace {

model::ModelArena g_arena;
formats::BuiltinFormats g_storage;
formats::FormatRegistry g_registry;

constexpr std::size_t kMaxFile = 18000;
constexpr std::size_t kPcm = 9000;
std::uint8_t g_bytes[kMaxFile];
std::size_t g_n = 0;
alignas(8) std::int16_t g_pcm[kPcm + 16];

void u8(std::uint32_t v) { if (g_n < kMaxFile) g_bytes[g_n++] = static_cast<std::uint8_t>(v); }
void u16(std::uint32_t v) { u8(v); u8(v >> 8); }
void u32(std::uint32_t v) { u16(v & 0xFFFFu); u16(v >> 16); }
void tag(const char* t) { for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>(t[i])); }

// A 16-bit mono WAV of `frames` frames with an optional 'clm ' chunk (text `clm`, or none) before or after the audio.
void make_wav(std::uint32_t frames, const char* clm = nullptr, bool clm_after = false, std::uint32_t clm_size = 0) {
  g_n = 0;
  tag("RIFF");
  u32(0);
  tag("WAVE");
  tag("fmt ");
  u32(16);
  u16(1);
  u16(1);
  u32(44100);
  u32(88200);
  u16(2);
  u16(16);
  const auto put_clm = [&]() {
    if (clm == nullptr) {
      return;
    }
    const std::uint32_t len = clm_size != 0 ? clm_size : static_cast<std::uint32_t>(std::strlen(clm));
    tag("clm ");
    u32(len);
    for (std::uint32_t i = 0; i < len; ++i) u8(static_cast<std::uint8_t>(i < std::strlen(clm) ? clm[i] : ' '));
    if ((len & 1u) != 0u) u8(0);  // the RIFF pad byte
  };
  if (!clm_after) put_clm();
  tag("data");
  u32(frames * 2);
  for (std::uint32_t i = 0; i < frames; ++i) u16((i * 7u + 3u) & 0x7FFFu);
  if (clm_after) put_clm();
  const std::uint32_t size = static_cast<std::uint32_t>(g_n - 8);
  g_bytes[4] = static_cast<std::uint8_t>(size);
  g_bytes[5] = static_cast<std::uint8_t>(size >> 8);
  g_bytes[6] = static_cast<std::uint8_t>(size >> 16);
  g_bytes[7] = static_cast<std::uint8_t>(size >> 24);
}

void reset() {
  g_arena.project.reset();
  g_registry = formats::FormatRegistry{};
  formats::register_builtin_formats(g_registry, g_storage);
}

Progress run(const char* id, std::uint16_t slot, const char* name, std::uint16_t wavetable = formats::kWtAuto, std::size_t capacity = kPcm, std::size_t budget = 4096, std::size_t max_read = static_cast<std::size_t>(-1),
             std::size_t cut = static_cast<std::size_t>(-1), std::uint32_t block_every = 0) {
  const formats::FormatDesc* d = g_registry.find_id(id);
  if (d == nullptr) {
    return Progress::Failed;
  }
  test::MemorySource src(g_bytes, cut < g_n ? cut : g_n);
  src.max_per_read = max_read;
  src.block_every = block_every;
  formats::ReadParams p;
  p.index = slot;
  p.name = name;
  p.pcm = g_pcm;
  p.pcm_capacity = capacity;
  p.wavetable = wavetable;
  d->reader->begin(src, g_arena, p);
  unsigned guard = 0;
  Progress pr = d->reader->progress();
  while (pr == Progress::Running && ++guard < 200000) {
    pr = d->reader->step(budget);
  }
  return pr;
}

const formats::FormatReport& rep(const char* id) { return g_registry.find_id(id)->reader->report(); }

const model::SamplerParams& sampler(std::uint16_t slot) { return g_arena.project.instrument(slot)->sampler; }

// A Surge .wt: `tables` tables of `n` samples; float32 (value i of table t = (t + 1) / 8 - 0.5 x sin-free ramp: a plain function), or int16 (flag 4, optionally 8).
void make_wt(std::uint32_t n, std::uint32_t tables, std::uint32_t flags, const char* tagv = "vawt") {
  g_n = 0;
  tag(tagv);
  u32(n);
  u16(tables);
  u16(flags);
  for (std::uint32_t t = 0; t < tables; ++t) {
    for (std::uint32_t i = 0; i < n; ++i) {
      const std::int32_t v = static_cast<std::int32_t>((t * 977u + i * 31u) % 2001u) - 1000;  // -1000..1000
      if ((flags & 4u) != 0u) {
        u16(static_cast<std::uint32_t>(v) & 0xFFFFu);
      } else {
        const float f = static_cast<float>(v) / 1000.0f;
        std::uint32_t bits;
        std::memcpy(&bits, &f, sizeof bits);
        u32(bits);
      }
    }
  }
}

}  // namespace

// ---- Serum: the 'clm ' chunk through the WAV importer and the reader ------------------------------------------------------------------------------------------------------------------
TEST_CASE("clm: the importer reads the frame size of a Serum wavetable's chunk, before or after the audio, with its pad byte") {
  reset();
  const char* const text = "<!>2048 20000000 wavetable (www.xferrecords.com)";  // 48 bytes
  for (const bool after : {false, true}) {
    make_wav(4096, text, after);
    NEED(run("wav", 0, "serum.wav") == Progress::Done);
    CHECK(sampler(0).play_mode == 6);
    CHECK(sampler(0).sample_type == 1);
    CHECK(sampler(0).wavetable_window_size == 2048);
    CHECK(sampler(0).wavetable_window == 0);
    CHECK(rep("wav").pcm_frames == 4096u);
    CHECK(sampler(0).slice_count == 0);
    CHECK(sampler(0).start == 0);
    CHECK(sampler(0).end == 65535);
  }
  make_wav(4096, "<!>1024 x", false);  // an odd size: the pad byte
  NEED(run("wav", 1, "odd.wav") == Progress::Done);
  CHECK(sampler(1).wavetable_window_size == 1024);
  make_wav(4096, "<!>2048", false);  // the number alone, a chunk of 7 bytes
  NEED(run("wav", 2, "bare.wav") == Progress::Done);
  CHECK(sampler(2).wavetable_window_size == 2048);
}

TEST_CASE("clm: a chunk that does not say a usable size is dropped and reported, the file stays a sample; a second chunk is ignored") {
  reset();
  const char* const bad[] = {"clm", "<!>", "<!>0", "<!>abc", "!!!2048", "<!>99999999", "<!>65536 "};
  for (const char* text : bad) {
    make_wav(2048, text);
    NEED(run("wav", 0, "bad.wav") == Progress::Done);
    CHECK_MESSAGE(sampler(0).play_mode == 0, text);
    CHECK_MESSAGE(sampler(0).sample_type == 0, text);
    CHECK_MESSAGE(rep("wav").sanitised >= 1u, text);
  }
  make_wav(2048, "<!>7 ", false, 2);  // a chunk smaller than 4 bytes: skipped whole
  NEED(run("wav", 0, "tiny.wav") == Progress::Done);
  CHECK(sampler(0).play_mode == 0);
  // 4096 is a number the engine has no table for: the file stays a sample and says so
  make_wav(8192, "<!>4096 ");
  NEED(run("wav", 0, "big.wav") == Progress::Done);
  CHECK(sampler(0).play_mode == 0);
  CHECK(rep("wav").sanitised == 1u);
  // 1000 is not a power of two
  make_wav(4000, "<!>1000 ");
  NEED(run("wav", 0, "odd.wav") == Progress::Done);
  CHECK(sampler(0).play_mode == 0);
  CHECK(rep("wav").sanitised == 1u);
}

TEST_CASE("clm: cut at every offset the reader either finishes with the right instrument or fails typed, and never leaves a half instrument; a source of one byte and a blocking one agree") {
  reset();
  make_wav(2100, "<!>1024 20000000 wavetable");
  const std::size_t whole = g_n;
  unsigned done = 0, failed = 0;
  for (std::size_t cut = 0; cut <= whole; cut += 3) {
    g_arena.project.reset();
    const Progress p = run("wav", 3, "cut.wav", formats::kWtAuto, kPcm, 4096, static_cast<std::size_t>(-1), cut);
    if (p == Progress::Done) {
      ++done;
      CHECK(sampler(3).wavetable_window_size == 1024);
    } else {
      ++failed;
      CHECK(rep("wav").error != FormatError::None);
      CHECK(g_arena.project.instrument(3) == nullptr);  // the project is as it was
    }
  }
  CHECK(done >= 1u);
  CHECK(failed >= 1u);
  g_arena.project.reset();
  NEED(run("wav", 3, "slow.wav", formats::kWtAuto, kPcm, 1, 1, static_cast<std::size_t>(-1), 3) == Progress::Done);
  CHECK(sampler(3).wavetable_window_size == 1024);
  CHECK(sampler(3).play_mode == 6);
}

// ---- The rule ------------------------------------------------------------------------------------------------------------------------------------------------------------------------
TEST_CASE("wavetable rule: a marker makes a wavetable, a plain WAV is a sample unless the user asks, and the length rule is 2048, then 1024, then 256") {
  using wav::wavetable_frame;
  using formats::kWtAuto;
  using formats::kWtDetect;
  using formats::kWtOff;
  bool unusable = true;
  CHECK(wavetable_frame(4096, 0, kWtAuto, &unusable) == 0u);  // a plain WAV: a sample, whatever its length
  CHECK_FALSE(unusable);
  CHECK(wavetable_frame(4096, 2048, kWtAuto) == 2048u);
  CHECK(wavetable_frame(4096, 1024, kWtAuto) == 1024u);
  CHECK(wavetable_frame(2048, 2048, kWtAuto) == 2048u);  // one window: a single cycle
  CHECK(wavetable_frame(1000, 2048, kWtAuto, &unusable) == 0u);  // not one whole window
  CHECK(unusable);
  CHECK(wavetable_frame(8192, 4096, kWtAuto, &unusable) == 0u);
  CHECK(unusable);
  CHECK(wavetable_frame(8000, 1000, kWtAuto, &unusable) == 0u);
  CHECK(unusable);
  CHECK(wavetable_frame(4096, 2048, kWtOff) == 0u);  // never
  // "load as wavetable": the largest of 2048, 1024, 256 that divides the length
  CHECK(wavetable_frame(2048 * 8, 0, kWtDetect) == 2048u);
  CHECK(wavetable_frame(1024 * 3, 0, kWtDetect) == 1024u);
  CHECK(wavetable_frame(256 * 7, 0, kWtDetect) == 256u);
  CHECK(wavetable_frame(256 * 7 + 1, 0, kWtDetect, &unusable) == 0u);  // fits none: a sample
  CHECK_FALSE(unusable);
  CHECK(wavetable_frame(2048 * 2, 1024, kWtDetect) == 1024u);  // the file's own marker first
  CHECK(wavetable_frame(2048 * 2, 4096, kWtDetect) == 2048u);  // ... unless it is no size the engine has
  CHECK(wavetable_frame(0, 0, kWtDetect) == 0u);
  // an explicit size over the marker's
  CHECK(wavetable_frame(2048, 0, 512) == 512u);
  CHECK(wavetable_frame(4096, 2048, 256) == 256u);
  CHECK(wavetable_frame(4096, 0, 300, &unusable) == 0u);
  CHECK(unusable);
  CHECK(wavetable_frame(100, 0, 512, &unusable) == 0u);  // less than one window
  CHECK(unusable);
}

TEST_CASE("wavetable rule: through the reader: auto leaves a plain WAV a sample, kWtDetect and an explicit size make the wavetable, the instrument is otherwise what it was") {
  reset();
  make_wav(4096);
  NEED(run("wav", 0, "plain.wav") == Progress::Done);
  CHECK(sampler(0).play_mode == 0);
  CHECK(sampler(0).sample_type == 0);
  CHECK(sampler(0).wavetable_window_size == 0);
  NEED(run("wav", 1, "plain.wav", formats::kWtDetect) == Progress::Done);
  CHECK(sampler(1).play_mode == 6);
  CHECK(sampler(1).sample_type == 1);
  CHECK(sampler(1).wavetable_window_size == 2048);
  make_wav(3 * 1024);
  NEED(run("wav", 2, "p.wav", formats::kWtDetect) == Progress::Done);
  CHECK(sampler(2).wavetable_window_size == 1024);
  make_wav(5 * 256);
  NEED(run("wav", 3, "p.wav", formats::kWtDetect) == Progress::Done);
  CHECK(sampler(3).wavetable_window_size == 256);
  make_wav(5 * 256 + 17);
  NEED(run("wav", 4, "p.wav", formats::kWtDetect) == Progress::Done);
  CHECK(sampler(4).play_mode == 0);  // a length that is no multiple of 256: a sample
  make_wav(4096);
  NEED(run("wav", 5, "p.wav", 512) == Progress::Done);
  CHECK(sampler(5).wavetable_window_size == 512);
  // never: a marked file as a plain sample
  make_wav(4096, "<!>2048 ");
  NEED(run("wav", 6, "p.wav", formats::kWtOff) == Progress::Done);
  CHECK(sampler(6).play_mode == 0);
  // a marker over a length that is not whole windows: the windows are the whole ones, the tail is counted
  make_wav(2048 * 3 + 100, "<!>2048 ");
  NEED(run("wav", 7, "p.wav") == Progress::Done);
  CHECK(sampler(7).play_mode == 6);
  CHECK(rep("wav").sanitised == 1u);
}

// ---- Surge ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
TEST_CASE("surge wt: float32 tables become 16-bit PCM and a wavetable instrument") {
  reset();
  make_wt(256, 4, 0);
  NEED(run("surge-wt", 0, "pad.wt") == Progress::Done);
  const formats::FormatReport& r = rep("surge-wt");
  CHECK(r.pcm_frames == 1024u);
  CHECK(r.pcm_channels == 1);
  CHECK(r.instruments == 1u);
  CHECK(r.sanitised == 0u);
  CHECK(sampler(0).play_mode == 6);
  CHECK(sampler(0).sample_type == 1);
  CHECK(sampler(0).wavetable_window_size == 256);
  CHECK(sampler(0).wavetable_window == 0);
  CHECK(std::strcmp(g_arena.project.instrument(0)->name, "pad") == 0);
  // table t, sample i is ((t x 977 + i x 31) % 2001 - 1000) / 1000 as float: x 32767, rounded
  unsigned bad = 0;
  for (std::uint32_t t = 0; t < 4; ++t) {
    for (std::uint32_t i = 0; i < 256; ++i) {
      const std::int32_t v = static_cast<std::int32_t>((t * 977u + i * 31u) % 2001u) - 1000;
      const float want = static_cast<float>(v) / 1000.0f * 32767.0f;
      bad += (static_cast<float>(g_pcm[t * 256 + i]) - want > 0.51f || want - static_cast<float>(g_pcm[t * 256 + i]) > 0.51f) ? 1u : 0u;
    }
  }
  CHECK(bad == 0u);
}

TEST_CASE("surge wt: int16 tables: with flag 8 as they are, without it the 15-bit form doubled and saturated (assumption in wt_surge.h)") {
  reset();
  make_wt(64, 3, 4 | 8);
  NEED(run("surge-wt", 0, "a.wt") == Progress::Done);
  for (std::uint32_t i = 0; i < 64; ++i) {
    CHECK(g_pcm[i] == static_cast<std::int16_t>(static_cast<std::int32_t>(i * 31u % 2001u) - 1000));
  }
  make_wt(64, 3, 4);
  NEED(run("surge-wt", 1, "b.wt") == Progress::Done);
  for (std::uint32_t i = 0; i < 64; ++i) {
    CHECK(g_pcm[i] == static_cast<std::int16_t>(2 * (static_cast<std::int32_t>(i * 31u % 2001u) - 1000)));
  }
  // saturation: 20000 x 2 holds at 32767, -20000 x 2 at -32768
  g_n = 0;
  tag("vawt");
  u32(32);
  u16(1);
  u16(4);
  for (int i = 0; i < 32; ++i) u16(i % 2 == 0 ? 20000u : static_cast<std::uint32_t>(-20000) & 0xFFFFu);
  NEED(run("surge-wt", 2, "c.wt") == Progress::Done);
  CHECK(g_pcm[0] == 32767);
  CHECK(g_pcm[1] == -32768);
}

TEST_CASE("surge wt: flags the engine does not use are counted, a NaN is silence, a float past full scale clips, trailing metadata is not read") {
  reset();
  g_n = 0;
  tag("vawt");
  u32(32);
  u16(1);
  u16(1 | 2 | 0x10 | 0x20);  // is_sample, loop_sample, has_metadata, user_modified
  for (int i = 0; i < 32; ++i) {
    std::uint32_t bits = i == 0 ? 0x7FC00000u : (i == 1 ? 0x40000000u /* 2.0 */ : (i == 2 ? 0xC0000000u /* -2.0 */ : 0u));
    u32(bits);
  }
  const char* xml = "<wtmeta name=\"x\"/>";
  for (const char* c = xml; *c != '\0'; ++c) u8(static_cast<std::uint8_t>(*c));
  u8(0);
  NEED(run("surge-wt", 0, "m.wt") == Progress::Done);
  CHECK(g_pcm[0] == 0);
  CHECK(g_pcm[1] == 32767);
  CHECK(g_pcm[2] == -32767);
  CHECK(rep("surge-wt").sanitised == 3u);  // is_sample, loop_sample, user_modified (the metadata is neither used nor a loss)
}

TEST_CASE("surge wt: refusals leave the project and the slot as they were: tag, frame sizes, counts, room, truncation at every offset") {
  reset();
  make_wt(256, 2, 0, "vawx");
  CHECK(run("surge-wt", 0, "t.wt") == Progress::Failed);
  CHECK(rep("surge-wt").error == FormatError::BadMagic);
  const std::uint32_t sizes[] = {0, 4, 31, 100, 3000, 4096, 65536};
  for (const std::uint32_t n : sizes) {
    make_wt(n > 512 ? 32 : (n == 0 ? 32 : 32), 1, 0);  // (a valid body: the header is then overwritten)
    g_bytes[4] = static_cast<std::uint8_t>(n);
    g_bytes[5] = static_cast<std::uint8_t>(n >> 8);
    g_bytes[6] = static_cast<std::uint8_t>(n >> 16);
    g_bytes[7] = static_cast<std::uint8_t>(n >> 24);
    CHECK_MESSAGE(run("surge-wt", 0, "t.wt") == Progress::Failed, n);
    CHECK_MESSAGE((rep("surge-wt").error == FormatError::Unsupported || rep("surge-wt").error == FormatError::Malformed), n);
  }
  make_wt(64, 0, 0);  // no tables
  CHECK(run("surge-wt", 0, "t.wt") == Progress::Failed);
  CHECK(rep("surge-wt").error == FormatError::Malformed);
  make_wt(2048, 5, 0);  // 10240 samples: does not fit 9000
  CHECK(run("surge-wt", 0, "t.wt") == Progress::Failed);
  CHECK(rep("surge-wt").error == FormatError::LimitExceeded);
  CHECK(run("surge-wt", 64, "t.wt") == Progress::Failed);  // the slot does not exist
  make_wt(64, 2, 0);
  const std::size_t whole = g_n;
  for (std::size_t cut = 0; cut < whole; ++cut) {
    g_arena.project.reset();
    const Progress p = run("surge-wt", 5, "cut.wt", formats::kWtAuto, kPcm, 4096, static_cast<std::size_t>(-1), cut);
    CHECK_MESSAGE(p == Progress::Failed, cut);
    CHECK(rep("surge-wt").error == FormatError::Truncated);
    CHECK(g_arena.project.instrument(5) == nullptr);
  }
  // and a source that gives one byte at a time and blocks now and then reads the same file
  g_arena.project.reset();
  NEED(run("surge-wt", 5, "slow.wt", formats::kWtAuto, kPcm, 1, 1, static_cast<std::size_t>(-1), 3) == Progress::Done);
  CHECK(rep("surge-wt").pcm_frames == 128u);
  const std::int16_t first = g_pcm[0];
  g_arena.project.reset();
  NEED(run("surge-wt", 5, "fast.wt") == Progress::Done);
  CHECK(g_pcm[0] == first);
}

TEST_CASE("surge wt: found by its name and by its tag, and read through the registry like any sample format") {
  reset();
  const formats::FormatDesc* d = g_registry.find_id("surge-wt");
  NEED(d != nullptr);
  CHECK(d->kind == formats::FormatKind::Sample);
  CHECK(d->writer == nullptr);
  CHECK(g_registry.find_name("Basic Shapes.WT") == d);
  make_wt(64, 1, 0);
  const std::uint8_t* head = g_bytes;
  CHECK(g_registry.sniff(head, 16, static_cast<std::uint32_t>(g_n)) == d);
  CHECK(g_registry.sniff(reinterpret_cast<const std::uint8_t*>("RIFF\0\0\0\0WAVEfmt "), 16, 100) == g_registry.find_id("wav"));
  CHECK(g_registry.identify("x.wt", head, 16, static_cast<std::uint32_t>(g_n)) == d);
}
