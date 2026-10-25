// SPDX-License-Identifier: GPL-3.0-or-later
//
// "Suena un patron" (OS-005d lite), end to end and offline: the demo kit and the demo pattern the device starts with (model/demo.h, audio/demo_kit.h),
// the sequencer as the engine's event source (seq/engine_link.h), the sampler voices and the graph, rendered block by block for two bars at 120 BPM
// and then stopped. The same wiring as the MK66's composition root (platform/mk66/src/app_main.cpp), minus the hardware. Host only (the project is
// 1.9 MB and the run is a few thousand blocks; the pure parts have their own QEMU suites).
#include <doctest/doctest.h>

#include <o3os/audio/demo_kit.h>
#include <o3os/audio/engine.h>
#include <o3os/audio/voices.h>
#include <o3os/core/audio_clock.h>
#include <o3os/hal/audio.h>
#include <o3os/model/demo.h>
#include <o3os/model/project.h>
#include <o3os/seq/engine_link.h>
#include <o3os/seq/sequencer.h>

#include "support/need.h"
#include "support/static_slot.h"

#include <cstdint>

namespace {

namespace haudio = o3os::hal::audio;
using namespace o3os;

constexpr std::uint32_t kBlk = haudio::kBlock;
// Two bars of 16 steps at 120 BPM are 4 s of the engine's clock (core/audio_clock.h): 176,470.6 samples at 44,117.6 Hz, 176,400 at 44,100 Hz. Bar 3 starts in
// the block of that sample, not at its first sample (23 of block 2757, 16 of block 2756); the run stops before that block.
constexpr std::uint32_t kTwoBarSamples = static_cast<std::uint32_t>(4ull * core::kCpuHz * core::kRefCyclesPerSample.den / core::kRefCyclesPerSample.num);
static_assert(kTwoBarSamples % haudio::kBlock != 0u, "bar 3 starts inside a block, so the blocks before it are the floor");
constexpr std::uint32_t kTwoBarBlocks = kTwoBarSamples / haudio::kBlock;
constexpr std::uint32_t kTail = 40;  // blocks after the stop: the release is 256 samples, its residual 64

std::uint32_t fake_cycles_counter = 0;
std::uint32_t fake_cycles() noexcept { return fake_cycles_counter += 11u; }

struct Result {
  std::uint64_t hash = 0;             // FNV-1a over every output sample of every block
  std::int32_t track_peak[16] = {};   // the largest |sample| each track's voice block held
  std::uint32_t track_blocks[16] = {};  // the blocks in which the track had a voice block at all
  std::int32_t out_peak = 0;
  std::uint32_t clipped = 0;
  std::uint32_t tail_nonzero = 0;     // output samples that are not zero in the last 10 blocks after the stop
  std::uint32_t note_ons = 0, steals = 0, ignored = 0;
  std::uint32_t queue_overflow = 0, deferred = 0, overrun = 0, torn = 0, missing = 0;
  bool playing_after_stop = true;
  std::uint32_t kit_samples = 0;
};

Result run_demo() {
  static model::ModelArena arena;
  model::Project& project = arena.project;
  project.reset();
  (void)project.create_pattern(1, model::kDefaultPatternLength);
  (void)project.append_order(1);
  CHECK(model::demo::pattern_is_empty(project, 1));
  CHECK(model::demo::add_instruments(project) == model::Status::Ok);
  CHECK(model::demo::fill_pattern(project, 1) == model::Status::Ok);
  CHECK_FALSE(model::demo::pattern_is_empty(project, 1));
  CHECK(project.tempo_x100() == 12000u);

  static std::int16_t kit_pcm[40000];
  static audio::DemoKit kit;
  Result r;
  if (!audio::generate_demo_kit(kit_pcm, 40000, kit)) {
    CHECK_MESSAGE(false, "the demo kit does not fit its buffer");
    return r;
  }
  r.kit_samples = kit.samples;

  static test::StaticSlot<audio::VoicePool> pool_slot;
  static test::StaticSlot<audio::Engine> engine_slot;
  static test::StaticSlot<seq::Sequencer> seq_slot;
  static audio::TapMem taps;
  static audio::SeqEvent events[audio::kMaxEventsPerBlock];
  audio::VoicePool& pool = pool_slot.make();
  audio::Engine& engine = engine_slot.make(&fake_cycles, taps);
  seq::Sequencer& sequencer = seq_slot.make(project);
  for (unsigned i = 0; i < audio::kDemoKitSounds; ++i) {
    pool.set_sample(i, kit.sample[i]);
  }
  engine.attach_voices(&pool);
  engine.attach_events(seq::event_source(sequencer, events, audio::kMaxEventsPerBlock));

  alignas(8) std::int16_t in[haudio::kBlockSamples] = {};
  alignas(8) std::int16_t out[haudio::kBlockSamples] = {};
  haudio::BlockReport report;
  r.hash = 0xCBF29CE484222325ull;
  const auto block = [&](bool tail_window) {
    report = haudio::BlockReport{};
    const haudio::BlockIo io{in, out, 0, &report};
    engine.render_block(io);
    for (std::uint32_t i = 0; i < haudio::kBlockSamples; ++i) {
      r.hash = (r.hash ^ static_cast<std::uint16_t>(out[i])) * 0x100000001B3ull;
      const std::int32_t a = out[i] < 0 ? -static_cast<std::int32_t>(out[i]) : static_cast<std::int32_t>(out[i]);
      r.out_peak = a > r.out_peak ? a : r.out_peak;
      r.clipped += a >= 32767 ? 1u : 0u;
      r.tail_nonzero += (tail_window && out[i] != 0) ? 1u : 0u;
    }
    for (unsigned t = 0; t < 16; ++t) {
      const std::int16_t* f = pool.feed(t);
      if (f == nullptr) {
        continue;
      }
      ++r.track_blocks[t];
      for (std::uint32_t i = 0; i < kBlk; ++i) {
        const std::int32_t a = f[i] < 0 ? -static_cast<std::int32_t>(f[i]) : static_cast<std::int32_t>(f[i]);
        r.track_peak[t] = a > r.track_peak[t] ? a : r.track_peak[t];
      }
    }
  };

  // Play pattern 1 from its first step, as the Play button does (a command on the sequencer's ring, taken at the next block).
  CHECK(sequencer.commands().push(seq::Command{seq::Command::Kind::PlayPattern, 1, 0, 0}));
  for (std::uint32_t b = 0; b < kTwoBarBlocks; ++b) {
    block(false);
  }
  CHECK(sequencer.playing());
  CHECK(sequencer.commands().push(seq::Command{seq::Command::Kind::Stop, 0, 0, 0}));
  for (std::uint32_t b = 0; b < kTail; ++b) {
    block(b + 10u >= kTail);
  }
  r.playing_after_stop = sequencer.playing();
  r.note_ons = pool.stats().note_ons;
  r.steals = pool.stats().steals;
  r.ignored = pool.stats().ignored;
  r.queue_overflow = sequencer.stats().queue_overflow;
  r.deferred = sequencer.stats().deferred;
  r.overrun = sequencer.stats().microstep_overrun;
  r.torn = sequencer.stats().rows_torn;
  r.missing = sequencer.stats().rows_missing;
  return r;
}

}  // namespace

TEST_CASE("demo: the demo pattern plays two bars through the sequencer, the voices and the graph: sound on tracks 1 to 4, nothing elsewhere, silent after the stop, the same every run") {
  const Result a = run_demo();
  const Result b = run_demo();

  CHECK(a.kit_samples > 25000u);  // the four sounds: about 30,300 samples
  CHECK(a.kit_samples < 40000u);
  CHECK(a.hash == b.hash);        // deterministic: the same bytes both times
  CHECK(a.hash != 0xCBF29CE484222325ull);

  // Tracks 1 to 4 (indices 0..3) sound; the others never had a voice. Peaks: the kick is 0.40 of full scale, the snare 0.28, the hat 0.15 at its
  // loud steps (and 60/100 of that at the others), the bass 0.20.
  CHECK(a.track_peak[0] > 10000);  // kick
  CHECK(a.track_peak[1] > 7000);   // snare
  CHECK(a.track_peak[2] > 2000);   // hat
  CHECK(a.track_peak[3] > 3500);   // bass
  for (unsigned t = 0; t < 4; ++t) {
    CHECK_MESSAGE(a.track_blocks[t] > 100u, "track " << t + 1);
    CHECK(a.track_peak[t] == b.track_peak[t]);
  }
  for (unsigned t = 4; t < 16; ++t) {
    CHECK_MESSAGE(a.track_blocks[t] == 0u, "track " << t + 1);
    CHECK(a.track_peak[t] == 0);
  }
  CHECK(a.out_peak > 8000);  // the mix is audible ...
  CHECK(a.clipped == 0u);    // ... and never clips: the kit's peaks add up to less than full scale

  // Every note of two bars reached a voice: per bar 4 kicks, 2 snares, 8 hats and 6 bass notes, and 2 ghost kicks and 1 ghost snare in all.
  CHECK(a.note_ons == 2u * (4u + 2u + 8u + 6u) + 3u);
  CHECK(a.ignored == 0u);  // no note on an empty slot
  CHECK(a.steals == 0u);   // four tracks never need more than the eight voices
  CHECK(a.queue_overflow == 0u);
  CHECK(a.deferred == 0u);
  CHECK(a.overrun == 0u);
  CHECK(a.torn == 0u);
  CHECK(a.missing == 0u);

  // The stop: the notes release and the residuals fade, so the last 10 blocks are exact silence and the transport is stopped.
  CHECK_FALSE(a.playing_after_stop);
  CHECK(a.tail_nonzero == 0u);
}
