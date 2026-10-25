// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-METRO end to end on the host: the device settings through app::MetroSync and the ParamChange ring into a real Engine, a real Sequencer as its event source (seq/engine_link.h), a Play with a
// count-in, and the codec's TX block observed block by block. The pattern is empty, so what comes out is the click and nothing else: its onsets must be on the samples of the beats the sequencer
// made (the count-in's four, the first accented, then the start point's), the click must be in both channels (OUT MODE Stereo) and a metronome that is off must be silent, count-in or not.
#include <doctest/doctest.h>

#include <o3os/app/metro_sync.h>
#include <o3os/audio/engine.h>
#include <o3os/audio/voices.h>
#include <o3os/model/params.h>
#include <o3os/model/settings.h>
#include <o3os/seq/engine_link.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "support/need.h"
#include "support/seq_fixture.h"
#include "support/static_slot.h"

using namespace o3os;
using namespace o3os::test;
namespace ha = o3os::hal::audio;
namespace ds = o3os::model::device_setting;

namespace {

std::uint32_t g_cycles = 0;
std::uint32_t fake_cycles() noexcept { return ++g_cycles; }

std::uint64_t exact_sample(std::uint64_t ticks, std::uint32_t bpm_x100) {
  const std::uint64_t n = ticks * core::kCpuHz * core::kRefCyclesPerSample.den * 125u;
  const std::uint64_t d = 4u * core::kRefCyclesPerSample.num * static_cast<std::uint64_t>(bpm_x100);
  return (2u * n + d) / (2u * d);
}

struct Run {
  std::vector<std::uint64_t> onsets;  // absolute samples where the left channel goes from 0 to non-zero
  std::uint32_t right_differs = 0;    // frames where the two channels differ (OUT MODE Stereo: none)
  std::uint32_t max_count_in = 0;     // the largest count-in the published position had
  std::uint32_t count_in_after = 0;   // and its last value
};

// Renders `blocks` blocks after one block that only takes the parameters; the transport starts at the second.
Run run(float metro_mode, unsigned count_in_bars, std::uint32_t blocks) {
  fresh_project(4, 4);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(12000) == model::Status::Ok);
  CHECK(p.create_pattern(1, 64) == model::Status::Ok);
  static StaticSlot<audio::Engine> engine_slot;
  static StaticSlot<audio::VoicePool> pool_slot;
  static audio::TapMem taps;
  static audio::SeqEvent events[audio::kMaxEventsPerBlock];
  g_cycles = 0;
  audio::Engine& e = engine_slot.make(&fake_cycles, taps);
  e.attach_voices(&pool_slot.make());
  seq::Sequencer& s = seq_slot().make(seq_project());
  e.attach_events(seq::event_source(s, events, audio::kMaxEventsPerBlock));

  model::Settings d;
  std::memset(&d, 0, sizeof d);
  d.init(core::ParamKind::Device);
  CHECK(d.set(ds::MetroMode, metro_mode) == model::Status::Ok);
  CHECK(d.set(ds::MetronomeVolume, 50.0f) == model::Status::Ok);
  app::MetroSync sync;
  CHECK(sync.step(d, e.params()));

  alignas(8) std::int16_t in[ha::kBlockSamples] = {};
  alignas(8) std::int16_t out[ha::kBlockSamples] = {};
  ha::BlockReport report{};
  const auto render = [&] {
    report = ha::BlockReport{};
    e.render_block(ha::BlockIo{in, out, 0, &report});
  };
  render();  // block 0: the drain takes the three ParamChanges (the sequencer's switch included)
  s.play_pattern(1, 0, count_in_bars);
  Run r;
  std::uint32_t quiet = 1000;  // frames of silence before this one: an onset is a sound after at least 200 of them (a sample inside a click can be exactly 0 at a zero crossing)
  for (std::uint32_t b = 1; b <= blocks; ++b) {
    render();
    for (std::uint32_t i = 0; i < ha::kBlock; ++i) {
      if (out[2u * i] != 0) {
        if (quiet >= 200u) {
          r.onsets.push_back(static_cast<std::uint64_t>(b) * ha::kBlock + i);
        }
        quiet = 0;
      } else {
        ++quiet;
      }
      r.right_differs += out[2u * i] != out[2u * i + 1u] ? 1u : 0u;
    }
    const std::uint32_t c = s.play_position().read().count_in;
    r.max_count_in = c > r.max_count_in ? c : r.max_count_in;
    r.count_in_after = c;
  }
  return r;
}

}  // namespace

TEST_CASE("metro end to end: a count-in of one bar clicks its four beats and the start point's on the sequencer's samples, on both channels") {
  const std::uint32_t steps = 21;  // the 16 steps of the count-in, then 5 of the pattern
  const std::uint32_t blocks = static_cast<std::uint32_t>(exact_sample(48u * steps, 12000) / ha::kBlock) + 2u;
  const Run r = run(1.0f, 1, blocks);
  // The run starts at block 1 (sample 64); beats at steps 0, 4, 8, 12 of the count-in and at the start point (step 16 of the run), whose next beat is at 20.
  NEED(r.onsets.size() >= 6u);
  const std::uint32_t beat_steps[] = {0, 4, 8, 12, 16, 20};
  for (std::uint32_t b = 0; b < 6; ++b) {
    CHECK_MESSAGE(r.onsets[b] == 64u + exact_sample(48u * beat_steps[b], 12000), "beat " << b);
  }
  CHECK(r.right_differs == 0u);  // OUT MODE Stereo: the click is on both channels
  CHECK(r.max_count_in == 16u);
  CHECK(r.count_in_after == 0u);
}

TEST_CASE("metro end to end: with the metronome off a count-in is silent, and a plain play clicks on the beats of the pattern") {
  const Run silent = run(0.0f, 2, 3000);
  CHECK(silent.onsets.empty());
  CHECK(silent.max_count_in == 32u);  // still counted: the strip and the pads do not need the click
  const std::uint32_t blocks = static_cast<std::uint32_t>(exact_sample(48u * 9u, 12000) / ha::kBlock) + 2u;
  const Run plain = run(2.0f, 0, blocks);  // Main, no count-in: step 0 at the first block, beats at 0, 4, 8
  NEED(plain.onsets.size() >= 3u);
  CHECK(plain.onsets[0] == 64u);
  CHECK(plain.onsets[1] == 64u + exact_sample(48u * 4u, 12000));
  CHECK(plain.onsets[2] == 64u + exact_sample(48u * 8u, 12000));
  CHECK(plain.max_count_in == 0u);
}
