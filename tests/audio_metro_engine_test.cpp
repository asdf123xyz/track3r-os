// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-METRO through the engine (audio/engine.h), host only (an Engine and a VoicePool are too big for the QEMU suite that holds audio_metro_test.cpp): a Metro event of the event source reaches
// the click at its sample inside the block, and a Step event flagged as a count-in step is not offered to a take that waits for a step, the first plain one is.
#include <doctest/doctest.h>

#include <o3os/audio/engine.h>
#include <o3os/audio/voices.h>
#include <o3os/hal/audio.h>

#include "support/static_slot.h"

#include <cstdint>

namespace {

namespace ha = o3os::hal::audio;
using namespace o3os::audio;
namespace gpm = o3os::audio::graph_param::master;

constexpr std::uint32_t kB = ha::kBlock;
constexpr std::uint32_t kBS = ha::kBlockSamples;

std::uint32_t g_cycles = 0;
std::uint32_t fake_cycles() noexcept { return ++g_cycles; }

SeqEvent g_script[4];
std::uint32_t g_script_n = 0;

std::uint32_t metro_script_step(void*, std::uint64_t, std::uint32_t, SeqEvent* out, std::uint32_t cap) noexcept {
  std::uint32_t n = 0;
  for (; n < g_script_n && n < cap; ++n) {
    out[n] = g_script[n];
  }
  g_script_n = 0;  // each script is for one block
  return n;
}

SeqEvent ev(std::uint16_t offset, EventKind kind, std::uint8_t a = 0, std::uint8_t flags = 0, std::int32_t value = 0) {
  SeqEvent e{};
  e.offset = offset;
  e.kind = kind;
  e.track = kNoTrack;
  e.a = a;
  e.flags = flags;
  e.value = value;
  return e;
}

}  // namespace

TEST_CASE("metro engine: a Metro event clicks at its sample, and a Step event of the count-in is kept from a take that waits for a step") {
  static o3os::test::StaticSlot<Engine> engine_slot;
  static o3os::test::StaticSlot<VoicePool> pool_slot;
  static TapMem taps;
  static SeqEvent events[kMaxEventsPerBlock];
  g_cycles = 0;
  Engine& e = engine_slot.make(&fake_cycles, taps);
  e.attach_voices(&pool_slot.make());
  EventSource src;
  src.step = &metro_script_step;
  src.buffer = events;
  src.capacity = kMaxEventsPerBlock;
  e.attach_events(src);
  CHECK(e.params().push(ParamChange{master_param(gpm::MetroLevel), 100.0f, 1}));
  CHECK(e.params().push(ParamChange{master_param(gpm::MetroMode), 2.0f, 1}));
  alignas(8) std::int16_t in[kBS] = {};
  alignas(8) std::int16_t out[kBS] = {};
  ha::BlockReport report{};
  const auto render = [&] {
    report = ha::BlockReport{};
    e.render_block(ha::BlockIo{in, out, 0, &report});
  };
  render();  // the drain takes the two changes (the events of a block come before its drain: the first block has none)
  // The beat at sample 17 of the next block: silent up to it, the click from it.
  g_script[0] = ev(17, EventKind::Metro, 1, 0, 0);
  g_script_n = 1;
  render();
  for (std::uint32_t i = 0; i < 17; ++i) {
    CHECK(out[2u * i] == 0);
    CHECK(out[2u * i + 1u] == 0);
  }
  CHECK(out[2u * 17u] > 0);
  CHECK(out[2u * 17u] == out[2u * 17u + 1u]);
  // An armed take that waits for a step starts at the first Step event that is not a count-in's.
  alignas(4) static std::int16_t take[2 * 4 * kB];
  CHECK(e.graph().rec().arm(take, 4 * kB, 2, RecTap::Align::Step));
  CHECK(e.graph().rec().state() == RecTap::State::Waiting);
  g_script[0] = ev(5, EventKind::Step, 1, kEvCountIn, 48);
  g_script_n = 1;
  render();
  CHECK(e.graph().rec().state() == RecTap::State::Waiting);  // a step of the count-in: the take goes on waiting
  g_script[0] = ev(9, EventKind::Step, 1, 0, 0);
  g_script_n = 1;
  render();
  CHECK(e.graph().rec().state() != RecTap::State::Waiting);  // the start point: the take begins (Armed, or already written to)
  e.graph().rec().disarm();
  render();
}
