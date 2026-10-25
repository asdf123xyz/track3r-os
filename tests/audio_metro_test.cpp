// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-METRO (docs/specs/OS-METRO.md, audio/metro.h): the metronome's click, on the host and on the Cortex-M4F (`qemu_audio_graph`).
//   * the click alone: its length, its two pitches (zero crossings), the accent, the level, a decay that ends below the least bit, an offset inside the block, the continuity across blocks, saturation
//     instead of wrapping, the routing flags, a replaced and a cancelled click, the ParamChange ranges;
//   * through the graph: OUT MODE Stereo has no CUE, so Cue and Main both play on both channels (D1); MainLCueR plays Main on the left jack and Cue on the right; OFF is silent; the click is added AFTER the
//     taps, so it is in no REC take and no meter (D2); a muted render drops it; the stage report counts it in the output stage;
//   (The engine's side, a Metro event at its sample and a Step event of the count-in kept from a take, is audio_metro_engine_test.cpp: host only, it needs an Engine and a VoicePool in .bss.)
#include <doctest/doctest.h>

#include <o3os/audio/graph.h>
#include <o3os/audio/metro.h>
#include <o3os/hal/audio.h>

#include "support/static_slot.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace {

namespace ha = o3os::hal::audio;
using namespace o3os::audio;
namespace gpm = o3os::audio::graph_param::master;

constexpr std::uint32_t kB = ha::kBlock;
constexpr std::uint32_t kBS = ha::kBlockSamples;
constexpr std::uint32_t kLong = 1024;  // frames of the one-call reference

alignas(4) std::int16_t g_long[2 * kLong];

// The click alone, into a long zeroed buffer in one call (the reference the blocked runs are compared with).
void click_long(Metro& m, std::uint32_t offset, bool accent, bool left = true, bool right = true) {
  std::memset(g_long, 0, sizeof g_long);
  m.trigger(offset, accent);
  m.mix(g_long, kLong, left, right);
}

// Peak magnitude of the left channel of g_long over [from, to).
std::int32_t peak(std::uint32_t from, std::uint32_t to) {
  std::int32_t p = 0;
  for (std::uint32_t i = from; i < to; ++i) {
    const std::int32_t v = g_long[2u * i] < 0 ? -static_cast<std::int32_t>(g_long[2u * i]) : g_long[2u * i];
    p = v > p ? v : p;
  }
  return p;
}
// Sign changes of the left channel over [from, to).
unsigned crossings(std::uint32_t from, std::uint32_t to) {
  unsigned n = 0;
  for (std::uint32_t i = from + 1u; i < to; ++i) {
    n += ((g_long[2u * i] < 0) != (g_long[2u * (i - 1u)] < 0) && g_long[2u * i] != 0 && g_long[2u * (i - 1u)] != 0) ? 1u : 0u;
  }
  return n;
}

std::uint32_t g_cycles = 0;
std::uint32_t fake_cycles() noexcept { return ++g_cycles; }

Graph& make_graph() {
  static o3os::test::StaticSlot<Graph> slot;  // not on the target's stack
  static TapMem taps;
  g_cycles = 0;
  return slot.make(&fake_cycles, taps);
}

struct Rig;
void settle(Rig& rig);

struct Rig {
  Graph& g;
  alignas(8) std::int16_t in[kBS] = {};
  alignas(8) std::int16_t out[kBS] = {};
  ha::BlockReport report = {};
  explicit Rig(Graph& graph) : g(graph) {}
  void render(std::uint32_t flags = 0) {
    report = ha::BlockReport{};
    const ha::BlockIo io{in, out, flags, &report};
    g.render(io);
  }
  void master(unsigned field, float v) { CHECK(g.params().push(ParamChange{master_param(field), v, 1})); }
  std::int16_t l(unsigned i) const { return out[2u * i]; }
  std::int16_t r(unsigned i) const { return out[2u * i + 1u]; }
  bool side_silent(unsigned channel) const {
    for (std::uint32_t i = 0; i < kB; ++i) {
      if (out[2u * i + channel] != 0) return false;
    }
    return true;
  }
  bool silent() const { return side_silent(0) && side_silent(1); }
};

// Blocks enough for a click to end (662 frames are 11 blocks), so the next case starts from silence.
void settle(Rig& rig) {
  for (int k = 0; k < 12; ++k) {
    rig.render();
  }
}

}  // namespace

TEST_CASE("metro: a click is 662 frames of a decaying sine that starts at its offset and ends below the least bit") {
  Metro m;
  CHECK(m.set_mode(1.0f));
  CHECK(m.set_level(100.0f));
  CHECK_FALSE(m.busy());
  constexpr std::uint32_t kAt = 40;  // an offset is inside the block: 0..63
  click_long(m, kAt, true);
  CHECK_FALSE(m.busy());  // one call of 1024 frames played all of it
  for (std::uint32_t i = 0; i < kAt; ++i) {
    CHECK(g_long[2u * i] == 0);  // nothing before the offset
    CHECK(g_long[2u * i + 1u] == 0);
  }
  CHECK(g_long[2u * kAt] > 0);  // the click's first sample is AT the offset: r sin(w) of the peak, the second sample of the sine (a click, not a fade)
  for (std::uint32_t i = 0; i < kLong; ++i) {
    CHECK(g_long[2u * i] == g_long[2u * i + 1u]);  // both channels alike
  }
  // Accent at level 100: 0.7 of full scale (22,937) times the decay to the first quarter cycle (r^7 = 0.89): a peak of about 20,500.
  const std::int32_t p = peak(kAt, kAt + 100);
  CHECK(p > 19000);
  CHECK(p < 22937);
  // It decays by 2^-15 over its length: the last frames are inside +-1, and nothing is written past 662 frames.
  CHECK(peak(kAt + 640u, kAt + 662u) <= 1);
  for (std::uint32_t i = kAt + 662u; i < kLong; ++i) {
    CHECK(g_long[2u * i] == 0);
  }
  // A halfway point: after 331 frames the amplitude is down to 2^-7.5, about 0.5 % of the start.
  CHECK(peak(kAt + 330u, kAt + 340u) < 160);
}

TEST_CASE("metro: the plain beat is 1 kHz at 0.8 of the accent, the accent 1.5 kHz") {
  Metro m;
  CHECK(m.set_level(100.0f));
  click_long(m, 0, false);
  const std::int32_t tick_peak = peak(0, 100);
  // Zero crossings in the first 300 frames (6.8 ms; the click is still at 5 % or more): 2 x f x t = 13.6 for 1 kHz at 44,117.6 Hz.
  const unsigned tick_c = crossings(0, 300);
  CHECK(tick_c >= 12);  // (a crossing that lands on a sample of exactly 0 is not counted)
  CHECK(tick_c <= 14);
  click_long(m, 0, true);
  const std::int32_t accent_peak = peak(0, 100);
  const unsigned accent_c = crossings(0, 300);
  CHECK(accent_c >= 19);  // 20.4
  CHECK(accent_c <= 21);
  CHECK(tick_peak < accent_peak);
  const double ratio = static_cast<double>(tick_peak) / static_cast<double>(accent_peak);
  CHECK(ratio > 0.70);
  CHECK(ratio < 0.86);  // 0.8 times the difference of the decay at the two peaks (0.846 against 0.893): 0.76
}

TEST_CASE("metro: METRO VOL is a linear amplitude; level 0, mode Off and out-of-range values make no click") {
  Metro m;
  CHECK(m.set_mode(1.0f));
  CHECK(m.set_level(100.0f));
  click_long(m, 0, true);
  const std::int32_t full = peak(0, 200);
  CHECK(m.set_level(50.0f));
  click_long(m, 0, true);
  const std::int32_t half = peak(0, 200);
  CHECK(half * 2 >= full - 2);
  CHECK(half * 2 <= full + 2);
  CHECK(m.set_level(0.0f));
  m.trigger(0, true);
  CHECK_FALSE(m.busy());  // a click at level 0 is not even scheduled
  CHECK(m.set_level(50.0f));
  CHECK(m.set_mode(0.0f));
  m.trigger(0, true);
  CHECK_FALSE(m.busy());  // and none while the metronome is off
  // The ranges: nothing changes on a refusal.
  CHECK_FALSE(m.set_mode(3.0f));
  CHECK_FALSE(m.set_mode(-1.0f));
  CHECK_FALSE(m.set_mode(std::nanf("")));
  CHECK_FALSE(m.set_level(101.0f));
  CHECK_FALSE(m.set_level(-0.5f));
  CHECK_FALSE(m.set_level(std::nanf("")));
  CHECK(m.mode() == Metro::Mode::Off);
  CHECK(m.set_mode(2.0f));
  CHECK(m.mode() == Metro::Mode::Main);
  CHECK(m.set_mode(1.0f));
  CHECK(m.mode() == Metro::Mode::Cue);
}

TEST_CASE("metro: the blocks of 64 frames make the same click as one long call, and the click goes on across them") {
  Metro a;
  Metro b;
  CHECK(a.set_level(80.0f));
  CHECK(b.set_level(80.0f));
  click_long(a, 59, true);  // starts 5 frames before the end of the first block
  static std::int16_t blocked[2 * kLong];
  std::memset(blocked, 0, sizeof blocked);
  b.trigger(59, true);
  std::uint32_t calls = 0;
  for (std::uint32_t at = 0; at < kLong; at += kB) {
    if (b.busy()) {
      b.mix(blocked + 2u * at, kB, true, true);
      ++calls;
    }
  }
  CHECK(std::memcmp(blocked, g_long, sizeof blocked) == 0);
  CHECK(calls == 12u);  // 5 frames of the first block, 10 whole blocks, 17 of the last: 662 frames in 12 calls
  CHECK_FALSE(b.busy());
}

TEST_CASE("metro: the sum saturates instead of wrapping, and the routing flags choose the channel") {
  Metro m;
  CHECK(m.set_level(100.0f));
  click_long(m, 0, true);
  static std::int16_t ref[2 * kLong];
  std::memcpy(ref, g_long, sizeof ref);
  // On a full-scale block of +32767 and -32768 the click saturates at the rails.
  static std::int16_t hot[2 * kLong];
  for (std::uint32_t i = 0; i < kLong; ++i) {
    hot[2u * i] = 32767;
    hot[2u * i + 1u] = -32768;
  }
  m.trigger(0, true);
  m.mix(hot, kLong, true, true);
  for (std::uint32_t i = 0; i < kLong; ++i) {
    const std::int32_t up = 32767 + ref[2u * i];
    const std::int32_t down = -32768 + ref[2u * i];
    CHECK(hot[2u * i] == (up > 32767 ? 32767 : up));
    CHECK(hot[2u * i + 1u] == (down < -32768 ? -32768 : down));
  }
  // Left only, right only.
  click_long(m, 0, true, true, false);
  CHECK(peak(0, 100) > 1000);
  for (std::uint32_t i = 0; i < kLong; ++i) {
    CHECK(g_long[2u * i + 1u] == 0);
  }
  click_long(m, 0, true, false, true);
  for (std::uint32_t i = 0; i < kLong; ++i) {
    CHECK(g_long[2u * i] == 0);
  }
  CHECK(g_long[2u * 3u + 1u] != 0);
}

TEST_CASE("metro: a new beat replaces the click that sounds, cancel() ends it, and Off in the middle of a click cuts it") {
  Metro m;
  CHECK(m.set_level(100.0f));
  static std::int16_t blk[2 * kB];
  std::memset(blk, 0, sizeof blk);
  m.trigger(0, false);
  m.mix(blk, kB, true, true);
  CHECK(m.busy());
  // A second beat 10 frames into the next block starts the accent from its own first sample.
  m.trigger(10, true);
  std::memset(blk, 0, sizeof blk);
  m.mix(blk, kB, true, true);
  Metro fresh;
  CHECK(fresh.set_level(100.0f));
  click_long(fresh, 10, true);
  CHECK(std::memcmp(blk, g_long, sizeof blk) == 0);  // the first block of a click that starts at 10: the same samples
  // cancel() and a switch to Off.
  m.cancel();
  CHECK_FALSE(m.busy());
  m.trigger(5, true);
  CHECK(m.busy());
  CHECK(m.set_mode(0.0f));
  CHECK_FALSE(m.busy());
  std::memset(blk, 0, sizeof blk);
  if (m.busy()) {
    m.mix(blk, kB, true, true);
  }
  for (std::uint32_t i = 0; i < 2u * kB; ++i) {
    CHECK(blk[i] == 0);
  }
  // An offset past the block is the block's last frame.
  CHECK(m.set_mode(1.0f));
  m.trigger(5000, true);
  std::memset(blk, 0, sizeof blk);
  m.mix(blk, kB, true, true);
  CHECK(blk[2u * 63u] > 0);  // its first sample
  CHECK(m.busy());           // the rest of the click is for the next block
}

TEST_CASE("metro graph: OUT MODE Stereo has no CUE, so Cue and Main play on both channels; Off plays nothing") {
  Rig rig(make_graph());
  rig.master(gpm::MetroLevel, 100.0f);
  for (const float mode : {1.0f, 2.0f}) {
    rig.master(gpm::MetroMode, mode);
    settle(rig);  // the drain takes the mode; the click of the round before has ended
    rig.render();
    CHECK(rig.silent());
    rig.g.metro_beat(0, true);
    rig.render();
    bool left = false;
    bool right = false;
    for (std::uint32_t i = 0; i < kB; ++i) {
      CHECK(rig.l(i) == rig.r(i));
      left = left || rig.l(i) != 0;
      right = right || rig.r(i) != 0;
    }
    CHECK(left);
    CHECK(right);
  }
  rig.master(gpm::MetroMode, 0.0f);
  rig.render();
  rig.g.metro_beat(0, true);
  CHECK_FALSE(rig.g.metro().busy());
  rig.render();
  CHECK(rig.silent());
}

TEST_CASE("metro graph: split out (MAIN left, CUE right): Main on the left jack, Cue on the right") {
  Rig rig(make_graph());
  rig.master(gpm::MetroLevel, 100.0f);
  rig.master(gpm::OutMode, 1.0f);
  rig.master(gpm::MetroMode, 2.0f);  // Main
  rig.render();
  rig.g.metro_beat(0, true);
  rig.render();
  CHECK_FALSE(rig.side_silent(0));
  CHECK(rig.side_silent(1));
  rig.master(gpm::MetroMode, 1.0f);  // Cue
  settle(rig);
  rig.g.metro_beat(0, true);
  rig.render();
  CHECK(rig.side_silent(0));
  CHECK_FALSE(rig.side_silent(1));
  rig.master(gpm::OutMode, 0.0f);  // back to Stereo: Cue plays on MAIN
  settle(rig);
  rig.g.metro_beat(0, false);
  rig.render();
  CHECK_FALSE(rig.side_silent(0));
  CHECK_FALSE(rig.side_silent(1));
}

TEST_CASE("metro graph: the click is added after the taps: it is in no REC take and no meter, and a muted render drops it") {
  Rig rig(make_graph());
  rig.master(gpm::MetroLevel, 100.0f);
  rig.master(gpm::MetroMode, 2.0f);
  rig.render();
  alignas(4) static std::int16_t take[2 * 4 * kB];
  std::memset(take, 0x55, sizeof take);
  rig.g.rec().set_source(RecTap::Source::Master);  // a resample of what is heard: MAIN after the master level
  CHECK(rig.g.rec().arm(take, 4 * kB, 2));
  rig.g.metro_beat(0, true);
  rig.render();
  CHECK_FALSE(rig.silent());  // it is heard...
  rig.g.metro_beat(0, true);
  rig.render();
  rig.g.rec().disarm();
  rig.render();
  CHECK(rig.g.rec().frames() >= 2u * kB);
  for (std::uint32_t i = 0; i < 2u * 2u * kB; ++i) {
    CHECK(take[i] == 0);  // ... and the take is silent
  }
  // The scope shows what the taps read of the TX block, before the click: a silent graph with a click in every block leaves the scope silent.
  rig.g.set_scope_enabled(true);
  for (int blk = 0; blk < 8; ++blk) {
    rig.g.metro_beat(0, true);
    rig.render();
  }
  CHECK_FALSE(rig.silent());
  alignas(4) std::int16_t scope[2 * kScopeWindowFrames];
  std::memset(scope, 0x55, sizeof scope);
  const ScopeRing::Window sw = rig.g.scope().read_latest(scope, 2 * kScopeWindowFrames);
  CHECK(sw.whole);
  for (std::uint32_t i = 0; i < 2 * kScopeWindowFrames; ++i) {
    CHECK(scope[i] == 0);
  }
  // A muted render (the silence flag) plays nothing and forgets the click.
  rig.g.metro_beat(0, true);
  rig.render(ha::kFlagSilence);
  CHECK(rig.silent());
  CHECK_FALSE(rig.g.metro().busy());
  rig.render();
  CHECK(rig.silent());
}

TEST_CASE("metro graph: the ParamChanges are range checked, and the output stage's cycles count the click") {
  Rig rig(make_graph());
  const std::uint32_t rejected = rig.g.params_rejected();
  rig.master(gpm::MetroMode, 3.0f);
  rig.master(gpm::MetroLevel, 101.0f);
  rig.master(gpm::MetroMode, std::nanf(""));
  rig.render();
  CHECK(rig.g.params_rejected() == rejected + 3u);
  rig.master(gpm::MetroMode, 1.0f);
  rig.master(gpm::MetroLevel, 40.0f);
  rig.render();
  CHECK(rig.g.params_rejected() == rejected + 3u);
  CHECK(rig.g.metro().mode() == Metro::Mode::Cue);
  // The fake counter advances one count a call: the click's two extra reads are two counts of the Convert stage.
  rig.render();
  const std::uint32_t plain = rig.report.stage_cycles[ha::kStageConvert];
  rig.g.metro_beat(0, true);
  rig.render();
  CHECK(rig.report.stage_cycles[ha::kStageConvert] == plain + 1u);
}
