// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-002c: the WAV importer as a registered format: registration, what a WAV makes of a sample instrument (name, path, rate,
// loop, slices, play mode), what it leaves alone when it fails, the stereo choice, hostile files through the reader. The importer
// itself is wav_test.cpp. No heap, so the same file runs as the QEMU suite `qemu_wav_sample`.
#include <doctest/doctest.h>

#include <o3os/formats/builtin.h>
#include <o3os/formats/format.h>
#include <o3os/formats/wav_sample.h>
#include <o3os/model/project.h>

#include <cstdint>
#include <cstring>

#include "support/memory_io.h"
#include "support/need.h"
#include "support/rng.h"

using namespace o3os;
namespace wav = o3os::formats::wav;
using formats::FormatError;
using formats::FormatKind;
using formats::Progress;

namespace {

model::ModelArena g_arena;
formats::BuiltinFormats g_storage;
formats::FormatRegistry g_registry;

constexpr std::size_t kMaxFile = 9000;
constexpr std::size_t kPcm = 4096;
std::uint8_t g_bytes[kMaxFile];
std::size_t g_n = 0;
alignas(8) std::int16_t g_pcm[kPcm + 16];

void u8(std::uint32_t v) { if (g_n < kMaxFile) g_bytes[g_n++] = static_cast<std::uint8_t>(v); }
void u16(std::uint32_t v) { u8(v); u8(v >> 8); }
void u32(std::uint32_t v) { u16(v & 0xFFFFu); u16(v >> 16); }
void tag(const char* t) { for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>(t[i])); }

// A 16-bit file of `frames` frames (1 or 2 channels) at `rate`, with optional smpl loop (type, start, last frame) and cues.
void make_file(std::uint32_t frames, unsigned channels = 1, std::uint32_t rate = 22050, bool loop = false,
               std::uint32_t loop_type = 0, std::uint32_t loop_start = 0, std::uint32_t loop_last = 0,
               const std::uint32_t* cues = nullptr, std::uint32_t cue_count = 0) {
  g_n = 0;
  tag("RIFF");
  u32(0);
  tag("WAVE");
  tag("fmt ");
  u32(16);
  u16(1);
  u16(channels);
  u32(rate);
  u32(rate * channels * 2);
  u16(channels * 2);
  u16(16);
  if (loop) {
    tag("smpl");
    u32(60);
    for (int i = 0; i < 3; ++i) u32(0);
    u32(60);
    u32(0);
    u32(0);
    u32(0);
    u32(1);
    u32(0);
    u32(0);
    u32(loop_type);
    u32(loop_start);
    u32(loop_last);
    u32(0);
    u32(0);
  }
  tag("data");
  u32(frames * channels * 2);
  for (std::uint32_t i = 0; i < frames * channels; ++i) u16((i * 13u + 1u) & 0xFFFFu);
  if (cue_count != 0) {
    tag("cue ");
    u32(4 + 24 * cue_count);
    u32(cue_count);
    for (std::uint32_t k = 0; k < cue_count; ++k) {
      u32(k);
      u32(cues[k]);
      tag("data");
      u32(0);
      u32(0);
      u32(cues[k]);
    }
  }
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

// Runs the registry's WAV reader on the file in g_bytes to the end.
Progress read(std::uint16_t slot, const char* name, bool keep_stereo = false, std::size_t pcm_capacity = kPcm,
              std::int16_t* pcm = g_pcm, std::size_t budget = 4096) {
  const formats::FormatDesc* d = g_registry.find_id("wav");
  if (d == nullptr) {
    return Progress::Failed;
  }
  test::MemorySource src(g_bytes, g_n);
  formats::ReadParams p;
  p.index = slot;
  p.name = name;
  p.pcm = pcm;
  p.pcm_capacity = pcm_capacity;
  p.keep_stereo = keep_stereo;
  d->reader->begin(src, g_arena, p);
  unsigned guard = 0;
  Progress pr = d->reader->progress();
  while (pr == Progress::Running && ++guard < 100000) {
    pr = d->reader->step(budget);
  }
  return pr;
}

const formats::FormatReport& rep() { return g_registry.find_id("wav")->reader->report(); }

}  // namespace

TEST_CASE("wav sample: registered as a sample format, found by name and by magic, read only") {
  reset();
  CHECK(g_registry.count() == 13u);  // the seven formats of OS-002c, the device settings file of OS-002d, the OctaMED importer of OS-008b, Surge's .wt (OS-GRAN), MOD and S3M (OS-MODIMP), AIFF / 8SVX (OS-SAMPFMT)
  const formats::FormatDesc* d = g_registry.find_id("wav");
  NEED(d != nullptr);
  CHECK(d->kind == FormatKind::Sample);
  CHECK(d->reader != nullptr);
  CHECK(d->writer == nullptr);
  CHECK(g_registry.find_name("kick.wav") == d);
  CHECK(g_registry.find_name("samples/KICK.WAV") == d);
  CHECK(g_registry.find_name("kick.wav.txt") == nullptr);
  make_file(10);
  CHECK(g_registry.sniff(g_bytes, 16, 0) == d);
  CHECK(g_registry.identify("noname", g_bytes, 16, static_cast<std::uint32_t>(g_n)) == d);
  CHECK(g_registry.identify("kick.wav", g_bytes, 16, 0) == d);
  std::uint8_t head[16];
  std::memcpy(head, g_bytes, sizeof head);
  std::memcpy(head, "RIFX", 4);  // a wav by name must look like one
  CHECK(g_registry.identify("kick.wav", head, 16, 0) == nullptr);
  std::memcpy(head, g_bytes, sizeof head);
  std::memcpy(head + 8, "AVI ", 4);
  CHECK(g_registry.sniff(head, 16, 0) == nullptr);
  // The OG files still find theirs.
  CHECK(g_registry.find_name("01 kick.pti") == g_registry.find_id("og-pti"));
  CHECK(g_registry.find_name("project.mt") == g_registry.find_id("og-project"));
}

TEST_CASE("wav sample: a file makes the sample instrument: name, path, rate, positions") {
  reset();
  make_file(300, 1, 22050);
  NEED(read(4, "kicks/KICK drum.WAV") == Progress::Done);
  CHECK(rep().error == FormatError::None);
  CHECK(rep().instruments == 1u);
  CHECK(rep().pcm_frames == 300u);
  CHECK(rep().pcm_channels == 1);
  const model::Instrument* in = g_arena.project.instrument(4);
  NEED(in != nullptr);
  CHECK(in->kind == model::InstrumentKind::Sample);
  CHECK(std::strcmp(in->name, "KICK drum") == 0);  // directory and extension cut
  CHECK(std::strcmp(in->sample_path, "samples/instr05.wav") == 0);  // slot + 1, the path our OG import derives
  CHECK(in->sample_rate == 22050u);
  CHECK(in->sampler.start == 0);
  CHECK(in->sampler.end == 65535);
  CHECK(in->sampler.loop_start == 0);
  CHECK(in->sampler.loop_end == 65535);
  CHECK(in->sampler.play_mode == 0);  // no loop in the file: one-shot
  CHECK(in->sampler.slice_count == 0);
  CHECK(model::validate_instrument(*in, g_arena.project.module_count()) == nullptr);
  CHECK(g_arena.project.check_invariants());
  CHECK(g_arena.project.instrument_count() == 1u);
  CHECK(g_pcm[0] == 1);
  CHECK(g_pcm[299] == static_cast<std::int16_t>((299u * 13u + 1u) & 0xFFFFu));

  // The name: printable ASCII only, 32 characters at most; no name at all is an empty one; slot 63's path has two digits.
  make_file(50);
  NEED(read(63, "ab\x01" "cd\xE9" "efghijklmnopqrstuvwxyz0123456789ABCDEF.wav") == Progress::Done);
  in = g_arena.project.instrument(63);
  NEED(in != nullptr);
  CHECK(std::strcmp(in->name, "ab?cd?efghijklmnopqrstuvwxyz0123") == 0);
  CHECK(std::strlen(in->name) == 32u);
  CHECK(std::strcmp(in->sample_path, "samples/instr64.wav") == 0);
  NEED(read(0, nullptr) == Progress::Done);
  CHECK(g_arena.project.instrument(0)->name[0] == '\0');
  NEED(read(1, ".hidden") == Progress::Done);  // a leading dot is not an extension
  CHECK(std::strcmp(g_arena.project.instrument(1)->name, ".hidden") == 0);

  // It replaces what the slot held: the instrument is made anew.
  model::Instrument old;
  model::init_instrument(old, model::InstrumentKind::Midi);
  old.midi_channel = 5;
  CHECK(g_arena.project.set_instrument(7, old) == model::Status::Ok);
  make_file(40, 1, 48000);
  NEED(read(7, "new.wav") == Progress::Done);
  in = g_arena.project.instrument(7);
  CHECK(in->kind == model::InstrumentKind::Sample);
  CHECK((in->midi_channel == 0 && in->sample_rate == 48000u));
}

TEST_CASE("wav sample: the loop and the cue points become positions, and the loop type picks the play mode") {
  reset();
  const std::uint32_t cues[] = {500, 1000};
  struct Case {
    std::uint32_t type;
    std::uint8_t mode;
  };
  const Case types[] = {{0, 1}, {2, 2}, {1, 3}};  // forward, backward, ping-pong: play modes 1, 2, 3 of the model
  for (const Case& c : types) {
    make_file(1500, 1, 44100, true, c.type, 100, 1499, cues, 2);
    NEED(read(2, "loop.wav") == Progress::Done);
    const model::SamplerParams& s = g_arena.project.instrument(2)->sampler;
    CHECK(s.play_mode == c.mode);
    CHECK(s.loop_start == 4369);   // 100 / 1500 of 65535, rounded
    CHECK(s.loop_end == 65535);     // the file's last frame + 1 = the whole length
    CHECK(s.slice_count == 2);
    CHECK(s.slices[0] == 21845);    // 500 / 1500
    CHECK(s.slices[1] == 43690);
    CHECK(s.selected_slice == 0);
    CHECK(g_arena.project.instrument(2)->sample_rate == 44100u);
  }
  // Cue points only: slices, and the play mode is left to the user.
  make_file(1500, 1, 44100, false, 0, 0, 0, cues, 2);
  NEED(read(2, "cue.wav") == Progress::Done);
  CHECK(g_arena.project.instrument(2)->sampler.play_mode == 0);
  CHECK(g_arena.project.instrument(2)->sampler.slice_count == 2);

  // The positions are 16 bits: on a long sample two cue points one frame apart are one slice, and the same for a loop that
  // is shorter than a position. Tested on the mapping itself (no 200,000 frames of audio needed).
  wav::Info info;
  info.frames = 200000;
  info.sample_rate = 96000;
  info.channels = 1;
  info.has_loop = true;
  info.loop_type = wav::LoopType::PingPong;
  info.loop_start = 100000;
  info.loop_end = 100002;  // 2 frames: both map to 32768
  info.slice_count = 4;
  info.slices[0] = 100;
  info.slices[1] = 101;  // the same position as 100
  info.slices[2] = 5000;
  info.slices[3] = 150000;
  model::Instrument out;
  wav::make_sample_instrument(info, 9, "long.wav", out);
  CHECK(out.sampler.slice_count == 3);
  CHECK(out.sampler.slices[0] == 33);
  CHECK(out.sampler.slices[1] == 1638);
  CHECK(out.sampler.slices[2] == 49151);
  CHECK(out.sampler.play_mode == 0);  // the loop could not be told from a point: dropped
  CHECK((out.sampler.loop_start == 0 && out.sampler.loop_end == 65535));
  CHECK(out.sample_rate == 96000u);
  CHECK(model::validate_instrument(out, 0) == nullptr);
  // A frame at 0 is the start, not a cut.
  info.slice_count = 2;
  info.slices[0] = 0;
  info.slices[1] = 100000;
  info.has_loop = false;
  wav::make_sample_instrument(info, 0, "z.wav", out);
  CHECK(out.sampler.slice_count == 1);
  CHECK(out.sampler.slices[0] == 32768);
  // An empty sample has nothing to put positions on.
  info.frames = 0;
  wav::make_sample_instrument(info, 0, "z.wav", out);
  CHECK((out.sampler.slice_count == 0 && out.sampler.end == 65535));
}

TEST_CASE("wav sample: a stereo file is mixed to mono unless it is told to keep it") {
  reset();
  make_file(200, 2);
  NEED(read(0, "st.wav") == Progress::Done);
  CHECK(rep().pcm_channels == 1);
  CHECK(rep().pcm_frames == 200u);
  NEED(read(1, "st.wav", true) == Progress::Done);
  CHECK(rep().pcm_channels == 2);
  CHECK(rep().pcm_frames == 200u);
  // The room it needs follows: 200 frames mixed fit 200 samples, kept they need 400.
  NEED(read(2, "st.wav", false, 200) == Progress::Done);
  CHECK(read(3, "st.wav", true, 399) == Progress::Failed);
  CHECK(rep().error == FormatError::LimitExceeded);
  CHECK(g_arena.project.instrument(3) == nullptr);
}

TEST_CASE("wav sample: a failed or refused import leaves the project and the slot as they were") {
  reset();
  model::Instrument mine;
  model::init_instrument(mine, model::InstrumentKind::Sample);
  std::memcpy(mine.name, "mine", 5);
  NEED(g_arena.project.set_instrument(3, mine) == model::Status::Ok);
  const std::uint32_t generation = g_arena.project.generation();

  make_file(300);
  CHECK(read(3, "x.wav", false, 299) == Progress::Failed);  // does not fit
  CHECK(rep().error == FormatError::LimitExceeded);
  CHECK(read(3, "x.wav", false, 0, nullptr) == Progress::Failed);  // no room given
  CHECK(rep().error == FormatError::LimitExceeded);
  CHECK(read(64, "x.wav") == Progress::Failed);  // no such slot
  CHECK(rep().error == FormatError::LimitExceeded);
  CHECK(read(0xFFFF, "x.wav") == Progress::Failed);
  const std::size_t full = g_n;
  for (const std::size_t cut : {std::size_t{0}, std::size_t{11}, std::size_t{30}, std::size_t{100}, full - 1}) {
    g_n = cut;
    CHECK(read(3, "x.wav") == Progress::Failed);
    CHECK_MESSAGE(rep().error == FormatError::Truncated, "cut " << cut);
  }
  g_n = full;
  std::memcpy(g_bytes, "RIFX", 4);
  CHECK(read(3, "x.wav") == Progress::Failed);
  CHECK(rep().error == FormatError::Unsupported);
  std::memcpy(g_bytes, "JUNK", 4);
  CHECK(read(3, "x.wav") == Progress::Failed);
  CHECK(rep().error == FormatError::BadMagic);
  CHECK(rep().detail[0] != '\0');
  CHECK(rep().instruments == 0u);
  CHECK(g_arena.project.generation() == generation);  // not one mutation
  CHECK(std::strcmp(g_arena.project.instrument(3)->name, "mine") == 0);
  CHECK(g_arena.project.check_invariants());

  // Stepping a finished reader changes nothing, and begin() starts it again.
  const formats::FormatDesc* d = g_registry.find_id("wav");
  CHECK(d->reader->step(100) == Progress::Failed);
  make_file(10);
  CHECK(read(3, "again.wav") == Progress::Done);
  CHECK(std::strcmp(g_arena.project.instrument(3)->name, "again") == 0);
  CHECK(d->reader->step(100) == Progress::Done);
}

TEST_CASE("wav sample: cancel abandons the read before the model is touched") {
  reset();
  make_file(2000);
  const formats::FormatDesc* d = g_registry.find_id("wav");
  NEED(d != nullptr);
  CHECK(d->reader->can_cancel());
  test::MemorySource src(g_bytes, g_n);
  formats::ReadParams p;
  p.index = 5;
  p.name = "c.wav";
  p.pcm = g_pcm;
  p.pcm_capacity = kPcm;
  d->reader->begin(src, g_arena, p);
  CHECK(d->reader->step(300) == Progress::Running);
  CHECK(d->reader->permille() > 0u);
  const std::uint32_t generation = g_arena.project.generation();
  d->reader->cancel();
  CHECK(d->reader->progress() == Progress::Failed);
  CHECK(rep().error == FormatError::Cancelled);
  CHECK(d->reader->step(300) == Progress::Failed);
  CHECK(g_arena.project.generation() == generation);
  CHECK(g_arena.project.instrument(5) == nullptr);
  d->reader->cancel();  // again: nothing
  CHECK(rep().error == FormatError::Cancelled);
  // The OG readers cannot be abandoned half way (their changes are already in the model).
  CHECK_FALSE(g_registry.find_id("og-project")->reader->can_cancel());
  CHECK_FALSE(g_registry.find_id("native")->reader->can_cancel());
}

TEST_CASE("wav sample: what the file had that the instrument has no place for is counted") {
  reset();
  // A smpl chunk with three loops (two dropped), and 60 cue points (12 beyond the 48 slices).
  std::uint32_t cues[60];
  for (std::uint32_t k = 0; k < 60; ++k) cues[k] = 100 + 10 * k;
  make_file(1000, 1, 44100, true, 0, 10, 500, cues, 60);
  NEED(read(0, "many.wav") == Progress::Done);
  CHECK(rep().sanitised == 12u);  // slices dropped; the loop count is 1 (the importer reads only the first)
  CHECK(g_arena.project.instrument(0)->sampler.slice_count == 48);
}

TEST_CASE("wav sample: damaged files through the registry: always Done or a typed error, the project always valid") {
#if defined(__arm__)
  constexpr unsigned kRounds = 300;
#else
  constexpr unsigned kRounds = 6000;
#endif
  reset();
  const std::uint32_t cues[] = {50, 120, 9000};
  make_file(150, 2, 22050, true, 1, 20, 110, cues, 3);
  static std::uint8_t pristine[kMaxFile];
  const std::size_t total = g_n;
  std::memcpy(pristine, g_bytes, total);
  test::Rng rng(0x5A3F1Eu);
  unsigned done = 0;
  for (unsigned round = 0; round < kRounds; ++round) {
    std::memcpy(g_bytes, pristine, total);
    const unsigned edits = 1 + rng.below(3);
    for (unsigned e = 0; e < edits; ++e) {
      g_bytes[rng.below(static_cast<std::uint32_t>(total))] = rng.byte();
    }
    g_n = rng.below(6) == 0 ? rng.below(static_cast<std::uint32_t>(total)) : total;
    const std::uint32_t before = g_arena.project.instrument_count();
    const Progress pr = read(static_cast<std::uint16_t>(rng.below(64)), "f.wav", rng.below(2) != 0, 100 + rng.below(kPcm - 100), g_pcm,
                             1 + rng.below(500));
    CHECK(pr != Progress::Running);
    CHECK(g_arena.project.check_invariants());
    if (pr == Progress::Done) {
      ++done;
      CHECK(rep().error == FormatError::None);
      CHECK(rep().instruments == 1u);
      CHECK(rep().pcm_frames >= 1u);
    } else {
      CHECK(rep().error != FormatError::None);
      CHECK(g_arena.project.instrument_count() == before);  // a failure adds nothing
    }
  }
  CHECK(done > kRounds / 20);
  for (std::uint32_t i = 0; i < model::kMaxInstruments; ++i) {
    const model::Instrument* in = g_arena.project.instrument(i);
    CHECK((in == nullptr || model::validate_instrument(*in, g_arena.project.module_count()) == nullptr));
  }
}
