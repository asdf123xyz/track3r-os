// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-CPU10 slice 2 (B1): the graph's output under a stream of pan edits, pinned. A strip whose only pending change is its pan (a panbrello, a pan FX, a note's instrument pan) is retargeted by a
// shorter path than the full recompute (Graph::recompute_pan); that path must give the very samples of the full one. This test drives four strips fed with noise through a scripted, seeded mix of
// pan edits (the panbrello each block, the instrument's pan at a note start, the tracker FX's absolute pan, the Perform layer's PANNING) and of every other kind of edit that shares a block with them
// or lands in the middle of a ramp the pan edit started (volume ramps of one to many blocks, mutes, the instrument's sends, the channel gain, the routing, the master DRY level), hashes every output
// block (FNV-1a) and compares the hash with the value the code had BEFORE the slice (dev a8abd929, the full recompute for every dirty strip). Host and Cortex-M4F: the two give the same hash.
#include <doctest/doctest.h>

#include <o3os/audio/graph.h>
#include <o3os/hal/audio.h>

#include "support/static_slot.h"

#include <cstdint>
#include <cstdio>

namespace {

namespace ha = o3os::hal::audio;
using namespace o3os::audio;
namespace gps = o3os::audio::graph_param::strip;
namespace gpm = o3os::audio::graph_param::master;

constexpr std::uint32_t kB = ha::kBlock;
constexpr std::uint32_t kBS = ha::kBlockSamples;
constexpr unsigned kRunTracks = 4;

std::uint32_t g_cycles = 0;
std::uint32_t fake_cycles() noexcept { return ++g_cycles; }

struct Lcg {
  std::uint32_t s;
  std::uint32_t next() {
    s = s * 1664525u + 1013904223u;
    return s >> 8;
  }
  std::uint32_t below(std::uint32_t n) { return next() % n; }
};

struct Fnv {
  std::uint32_t h = 2166136261u;
  void add(std::uint32_t v) {
    for (unsigned k = 0; k < 4u; ++k) {
      h = (h ^ ((v >> (8u * k)) & 0xFFu)) * 16777619u;
    }
  }
};

constexpr std::uint32_t kRamps[] = {0u, 1u, 64u, 130u, 400u};

std::uint32_t run(std::uint32_t seed, unsigned pan_percent) {
  static o3os::test::StaticSlot<Graph> slot;
  static TapMem taps;
  g_cycles = 0;
  Graph& g = slot.make(&fake_cycles, taps);
  alignas(8) static std::int16_t in[kBS];
  alignas(8) static std::int16_t out[kBS];
  alignas(8) static std::int16_t feed[kRunTracks][kB];
  Lcg r{seed};
  for (unsigned i = 0; i < kBS; ++i) {
    in[i] = 0;
  }
  for (unsigned t = 0; t < kRunTracks; ++t) {
    for (std::uint32_t i = 0; i < kB; ++i) {
      feed[t][i] = static_cast<std::int16_t>(static_cast<std::int32_t>(r.below(24001u)) - 12000);
    }
    CHECK(g.params().push(ParamChange{strip_param(t, gps::Source), 3.0f, 1u}));
    g.feed(t, feed[t]);
    CHECK(g.params().push(ParamChange{strip_param(t, gps::Route), static_cast<float>(route::kMain | route::kSndA | route::kSndB), 1u}));
  }
  Fnv f;
  for (unsigned blk = 0; blk < 360u; ++blk) {
    for (unsigned t = 0; t < kRunTracks; ++t) {
      const std::uint32_t ramp = kRamps[r.below(5u)];
      if (r.below(100u) < pan_percent) {
        switch (r.below(5u)) {
          case 0:
          case 1: g.set_track_pan_mod(t, static_cast<float>(static_cast<int>(r.below(201u)) - 100), ramp); break;
          case 2: g.set_voice_pan(t, r.below(101u), ramp); break;
          case 3: g.set_track_pan(t, static_cast<float>(r.below(101u)), ramp); break;
          default: g.set_perf_pan(t, static_cast<int>(r.below(61u)) - 30, ramp); break;
        }
      }
      if (r.below(100u) < 10u) {
        switch (r.below(8u)) {
          case 0: CHECK(g.params().push(ParamChange{strip_param(t, gps::Volume), static_cast<float>(r.below(101u)), ramp})); break;
          case 1: g.set_strip_mute(t, r.below(2u) != 0u); break;
          case 2: g.set_voice_sends(t, r.below(101u), r.below(101u), ramp); break;
          case 3: g.set_track_gain(t, static_cast<float>(r.below(101u)) * 0.01f, ramp); break;
          case 4: CHECK(g.params().push(ParamChange{strip_param(t, gps::Route), static_cast<float>(r.below(64u)), ramp})); break;
          case 5: CHECK(g.params().push(ParamChange{master_param(gpm::Dry), static_cast<float>(60u + r.below(41u)), ramp})); break;
          case 6:  // a note's start: its sends and its pan at once (ramp 1), then the panbrello's smoothed update of the same block
            g.set_voice_sends(t, r.below(101u), r.below(101u), 1u);
            g.set_voice_pan(t, r.below(101u), 1u);
            g.set_track_pan_mod(t, static_cast<float>(static_cast<int>(r.below(201u)) - 100), 0u);
            break;
          default: g.set_perf_gain(t, -static_cast<int>(r.below(60u)), ramp); break;
        }
      }
    }
    ha::BlockReport report{};
    const ha::BlockIo io{in, out, 0u, &report};
    g.render(io);
    for (unsigned i = 0; i < kBS; i += 2u) {
      f.add(static_cast<std::uint32_t>(static_cast<std::uint16_t>(out[i])) | (static_cast<std::uint32_t>(static_cast<std::uint16_t>(out[i + 1u])) << 16));
    }
  }
  return f.h;
}

void report(const char* group, std::uint32_t got, std::uint32_t pinned) {
  CHECK_MESSAGE(got == pinned, group << ": the digest changed");
  if (got != pinned) {
    std::printf("DIGEST %s 0x%08x\n", group, static_cast<unsigned>(got));
  }
}

}  // namespace

TEST_CASE("graph pan digest: pan edits alone, mixed with every other edit, and ramps in flight") {
  report("pan_heavy", run(7u, 70u), 0x799a9a54u);
  report("pan_mixed", run(1234u, 25u), 0x19ab4894u);
  report("pan_rare", run(99u, 5u), 0x74c603fdu);
}
