// SPDX-License-Identifier: GPL-3.0-or-later
//
// The render loop skeleton (audio/engine.h, OS-005a) called directly: the HW-A tone script that tells the channels apart (left alone,
// then the right one at another frequency), the fade, the silence flag, the bounded ParamChange drain, the input meter and the
// single-shot stall hook. Host and Cortex-M4F (`qemu_audio`).
#include <doctest/doctest.h>

#include <o3os/audio/engine.h>
#include <o3os/audio/test_signal.h>
#include <o3os/hal/audio.h>

#include "support/static_slot.h"

#include <cstdint>

namespace {

namespace haudio = o3os::hal::audio;
using o3os::audio::Engine;

// A cycle counter that only moves when read (by `step` per read).
std::uint32_t g_counter = 0;
std::uint32_t g_step = 1;
std::uint32_t fake_cycles() noexcept {
  g_counter += g_step;
  return g_counter;
}

// The engine is 14 KiB and its taps' memory 22 KiB: built in static storage, so that the same cases run on the target's small stack.
Engine& make_engine() {
  static o3os::test::StaticSlot<Engine> slot;
  static o3os::audio::TapMem taps;
  return slot.make(&fake_cycles, taps);
}

struct Block {
  alignas(8) std::int16_t in[haudio::kBlockSamples] = {};
  alignas(8) std::int16_t out[haudio::kBlockSamples] = {};
  haudio::BlockReport report = {};
};

void render(Engine& e, Block& b, std::uint32_t flags = 0) {
  b.report = haudio::BlockReport{};
  const haudio::BlockIo io{b.in, b.out, flags, &b.report};
  e.render_block(io);
}

bool channel_silent(const Block& b, unsigned ch) {
  for (std::uint32_t i = 0; i < haudio::kBlock; ++i) {
    if (b.out[2u * i + ch] != 0) {
      return false;
    }
  }
  return true;
}

}  // namespace

TEST_CASE("engine: tone off is digital silence and costs no tone") {
  g_counter = 0;
  g_step = 1;
  Engine& e = make_engine();
  Block b;
  for (int k = 0; k < 20; ++k) {
    render(e, b);
    CHECK(channel_silent(b, 0));
    CHECK(channel_silent(b, 1));
  }
  CHECK(e.tone_left().ramp_pos() == 0u);
  CHECK(e.tone_right().ramp_pos() == 0u);
  CHECK_FALSE(e.tone_requested());
}

TEST_CASE("engine: the tone script plays the left channel alone, then the right at 1.5 kHz, so a channel swap is visible") {
  g_counter = 0;
  g_step = 1;
  Engine& e = make_engine();
  constexpr std::uint32_t kLeftOnly = 30;
  e.set_left_only_blocks(kLeftOnly);
  e.set_tone(true);
  Block b;
  std::uint32_t left_blocks_sounding = 0;
  for (std::uint32_t k = 0; k < kLeftOnly + 40u; ++k) {
    render(e, b);
    const bool right_silent = channel_silent(b, 1);
    const bool left_silent = channel_silent(b, 0);
    left_blocks_sounding += left_silent ? 0u : 1u;
    if (k < kLeftOnly) {
      CHECK(right_silent);           // exactly silent through the whole left-only phase
    }
    if (k == kLeftOnly + 39u) {
      CHECK_FALSE(right_silent);     // the right channel has joined
      CHECK_FALSE(left_silent);      // and the left goes on
    }
  }
  CHECK(left_blocks_sounding >= kLeftOnly + 39u);
  CHECK(e.tone_right().ramp_pos() > 0u);
}

TEST_CASE("engine: each channel has its own frequency, 1 kHz left and 1.5 kHz right, by zero crossings") {
  g_counter = 0;
  g_step = 1;
  Engine& e = make_engine();
  e.set_left_only_blocks(8);
  e.set_tone(true);
  Block b;
  // Past the fades (2,205 samples = 35 blocks), then two seconds' worth of samples at the rate constant.
  for (std::uint32_t k = 0; k < 8u + 40u; ++k) {
    render(e, b);
  }
  const std::uint64_t samples_per_second = (static_cast<std::uint64_t>(haudio::kCpuHz) * haudio::kCyclesPerSample.den) / haudio::kCyclesPerSample.num;
  const std::uint32_t blocks = static_cast<std::uint32_t>(2u * samples_per_second / haudio::kBlock);
  std::uint32_t crossings[2] = {0, 0};
  std::int16_t previous[2] = {0, 0};
  for (std::uint32_t k = 0; k < blocks; ++k) {
    render(e, b);
    for (std::uint32_t i = 0; i < haudio::kBlock; ++i) {
      for (unsigned ch = 0; ch < 2; ++ch) {
        const std::int16_t v = b.out[2u * i + ch];
        crossings[ch] += (previous[ch] < 0 && v >= 0) ? 1u : 0u;
        previous[ch] = v;
      }
    }
  }
  const std::uint64_t samples = static_cast<std::uint64_t>(blocks) * haudio::kBlock;
  const double seconds = static_cast<double>(samples) * static_cast<double>(haudio::kCyclesPerSample.num) / static_cast<double>(haudio::kCyclesPerSample.den) / static_cast<double>(haudio::kCpuHz);
  const double left_hz = static_cast<double>(crossings[0]) / seconds;
  const double right_hz = static_cast<double>(crossings[1]) / seconds;
  CHECK(left_hz > 999.0);
  CHECK(left_hz < 1001.0);
  CHECK(right_hz > 1499.0);
  CHECK(right_hz < 1501.0);
}

TEST_CASE("engine: switching the tone off fades both channels to exact silence and rewinds the script") {
  g_counter = 0;
  g_step = 1;
  Engine& e = make_engine();
  constexpr std::uint32_t kLeftOnly = 6;
  e.set_left_only_blocks(kLeftOnly);
  e.set_tone(true);
  Block b;
  for (std::uint32_t k = 0; k < kLeftOnly + 45u; ++k) {
    render(e, b);
  }
  CHECK(e.tone_left().ramp_pos() == o3os::audio::kToneRampSamples);
  CHECK(e.tone_right().ramp_pos() == o3os::audio::kToneRampSamples);
  e.set_tone(false);
  for (std::uint32_t k = 0; k < 40u; ++k) {
    render(e, b);
  }
  CHECK(channel_silent(b, 0));
  CHECK(channel_silent(b, 1));
  CHECK(e.tone_left().ramp_pos() == 0u);
  CHECK(e.tone_right().ramp_pos() == 0u);
  // Switched on again: the script starts over, the right channel silent for the left-only phase again.
  e.set_tone(true);
  for (std::uint32_t k = 0; k < kLeftOnly; ++k) {
    render(e, b);
    CHECK(channel_silent(b, 1));
  }
  CHECK_FALSE(channel_silent(b, 0));
}

TEST_CASE("engine: the silence flag writes silence, resets the fades and keeps the script on its clock") {
  g_counter = 0;
  g_step = 1;
  Engine& e = make_engine();
  constexpr std::uint32_t kLeftOnly = 12;
  e.set_left_only_blocks(kLeftOnly);
  e.set_tone(true);
  Block b;
  for (std::uint32_t k = 0; k < 5u; ++k) {
    render(e, b);
  }
  CHECK_FALSE(channel_silent(b, 0));
  for (std::uint32_t k = 5; k < 9u; ++k) {   // four silenced blocks: the underrun mute
    render(e, b, haudio::kFlagSilence);
    CHECK(channel_silent(b, 0));
    CHECK(channel_silent(b, 1));
  }
  CHECK(e.tone_left().ramp_pos() == 0u);     // the fade starts again afterwards
  // Block index 9, 10, 11: still the left-only phase; block 12 is the first with the right channel, silenced time included.
  for (std::uint32_t k = 9; k < kLeftOnly; ++k) {
    render(e, b);
    CHECK(channel_silent(b, 1));
    CHECK_FALSE(channel_silent(b, 0));
  }
  render(e, b);                              // block index 12
  CHECK_FALSE(channel_silent(b, 1));
}

TEST_CASE("engine: the ParamChange drain is bounded per block, and the input is metered") {
  g_counter = 0;
  g_step = 1;
  Engine& e = make_engine();
  Block b;
  for (std::uint32_t i = 0; i < o3os::audio::kParamRingSize; ++i) {
    CHECK(e.params().push(o3os::audio::ParamChange{static_cast<o3os::core::ParamId>(i), 0.5f, 0u}));
  }
  CHECK_FALSE(e.params().push(o3os::audio::ParamChange{static_cast<o3os::core::ParamId>(999u), 0.0f, 0u}));  // full: the producer is told, never blocked
  render(e, b);
  CHECK(e.params_applied() == o3os::audio::kMaxParamsPerBlock);           // one block drains at most 64
  for (int k = 0; k < 3; ++k) {
    render(e, b);
  }
  CHECK(e.params_applied() == o3os::audio::kParamRingSize);               // the rest over the next blocks
  render(e, b);
  CHECK(e.params_applied() == o3os::audio::kParamRingSize);

  b.in[17] = -32768;
  b.in[50] = 1234;
  render(e, b);
  CHECK(b.report.input_peak == 32768u);                                   // |-32768| counts as 32768
  b.in[17] = 0;
  render(e, b);
  CHECK(b.report.input_peak == 1234u);
  for (unsigned s = 0; s < haudio::kStageCount - 1u; ++s) {
    if (s == haudio::kStageRender) {
      CHECK(b.report.stage_cycles[s] == 0u);                              // no source made a block: the sources' stage is nothing
    } else {
      CHECK(b.report.stage_cycles[s] > 0u);                               // the graph's other stages are timed (the commit stage is the pipeline's)
    }
  }
  CHECK(b.report.stage_cycles[haudio::kStageCommit] == 0u);
}

TEST_CASE("engine: the test stall is single shot, burns the cycles asked for, is capped, and cannot hang") {
  g_counter = 0;
  g_step = 1;
  Engine& e = make_engine();
  Block b;
  render(e, b);
  const std::uint32_t quiet = g_counter;
  g_counter = 0;
  render(e, b);
  const std::uint32_t normal_cycles = g_counter;
  CHECK(normal_cycles <= quiet);

  e.request_test_stall(5000);
  g_counter = 0;
  render(e, b);
  CHECK(g_counter >= 5000u);                 // it burned at least what was asked
  CHECK(g_counter < 5000u + 4u * normal_cycles + 100u);
  g_counter = 0;
  render(e, b);
  CHECK(g_counter == normal_cycles);         // single shot: the next render is normal again

  // Capped at four block periods, whatever is asked.
  e.request_test_stall(0xFFFFFFFFu);
  g_step = 1000;                              // the counter runs fast so the test is short
  g_counter = 0;
  render(e, b);
  CHECK(g_counter >= Engine::kMaxTestStallCycles);
  CHECK(g_counter < Engine::kMaxTestStallCycles + 20000u);

  // A cycle counter that never moves cannot hang the render: the loop is bounded by the request.
  g_step = 0;
  e.request_test_stall(100000);
  g_counter = 0;
  render(e, b);
  CHECK(g_counter == 0u);                     // returned, the iteration cap ended it
}
