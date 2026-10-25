// SPDX-License-Identifier: GPL-3.0-or-later
//
// The recorder's render side (OS-011c): the REC tap's take aligned to the timeline (Waiting, the step that starts it, the sample offset inside the block), the channel it
// records (mix, left, right), what it records (the REC bus, or the MAIN bus after the master level: a resample), and the input strip's feed (the codec, the USB return, or the
// sum). Through the real graph and, for the alignment, through the engine with a scripted event source. Host only (the target suites run audio_taps_test and audio_graph_test).
#include <doctest/doctest.h>

#include <o3os/audio/engine.h>
#include <o3os/audio/event_source.h>
#include <o3os/audio/graph.h>
#include <o3os/audio/taps.h>
#include <o3os/audio/voices.h>
#include <o3os/hal/audio.h>
#include <o3os/usb/stream.h>

#include "support/static_slot.h"

#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace {

namespace ha = o3os::hal::audio;
using namespace o3os::audio;
namespace gps = o3os::audio::graph_param::strip;
namespace gpm = o3os::audio::graph_param::master;

constexpr std::uint32_t kB = ha::kBlock;
constexpr std::uint32_t kBS = ha::kBlockSamples;

std::uint32_t g_cycles = 0;
std::uint32_t fake_cycles() noexcept { return ++g_cycles; }

struct Rig {
  Engine& e;
  alignas(8) std::int16_t in[kBS] = {};
  alignas(8) std::int16_t out[kBS] = {};
  ha::BlockReport report = {};
  explicit Rig(Engine& engine) : e(engine) {}
  Graph& g() { return e.graph(); }
  void render() {
    report = ha::BlockReport{};
    const ha::BlockIo io{in, out, 0, &report};
    e.render_block(io);
  }
  std::int16_t l(unsigned i) const { return out[2u * i]; }
  std::int16_t r(unsigned i) const { return out[2u * i + 1u]; }
  bool silent_out() const {
    for (std::uint32_t i = 0; i < kBS; ++i) {
      if (out[i] != 0) return false;
    }
    return true;
  }
  void strip(unsigned slot, unsigned field, float v) { CHECK(e.params().push(ParamChange{strip_param(slot, field), v, 1u})); }
  void master(unsigned field, float v) { CHECK(e.params().push(ParamChange{master_param(field), v, 1u})); }
  // The RX block of block number `block`: left = 1000 + block * 64 + i, right = -(left) / 2: distinct per sample and per channel.
  void rx(std::uint32_t block) {
    for (std::uint32_t i = 0; i < kB; ++i) {
      const int l = 1000 + static_cast<int>(block * kB + i);
      in[2u * i] = static_cast<std::int16_t>(l);
      in[2u * i + 1u] = static_cast<std::int16_t>(-l / 2);
    }
  }
};

Engine& make_engine() {
  static o3os::test::StaticSlot<Engine> slot;
  static TapMem taps;
  g_cycles = 0;
  return slot.make(&fake_cycles, taps);
}

// A scripted event source: a Step event (index `g_step_value`) at sample `g_step_offset` of block `g_step_block`, and nothing else.
std::uint32_t g_step_block = 0;
std::uint32_t g_step_offset = 0;
std::int32_t g_step_value = 0;
std::uint32_t g_step_block_now = 0;
std::uint32_t scripted(void*, std::uint64_t block_start, std::uint32_t, SeqEvent* out, std::uint32_t cap) noexcept {
  g_step_block_now = static_cast<std::uint32_t>(block_start / kB);
  if (g_step_block_now != g_step_block || cap < 1u) {
    return 0;
  }
  SeqEvent step{};
  step.kind = EventKind::Step;
  step.track = kNoTrack;
  step.offset = static_cast<std::uint16_t>(g_step_offset);
  step.value = g_step_value;
  out[0] = step;
  return 1;
}

constexpr std::int16_t rx_left(std::uint32_t block, std::uint32_t i) { return static_cast<std::int16_t>(1000 + block * kB + i); }

}  // namespace

TEST_CASE("rec tap: an aligned take waits, starts at the step's own sample, and ignores steps that are not its alignment") {
  static o3os::test::StaticSlot<RecTap> slot;
  RecTap& rec = slot.make();
  alignas(4) static std::int16_t l[kB], r[kB];
  alignas(4) static std::int16_t take[8 * kB];
  for (std::uint32_t i = 0; i < kB; ++i) {
    l[i] = static_cast<std::int16_t>(100 + i);
    r[i] = l[i];
  }
  // Align::Step: the first step starts it, 20 samples into the block.
  CHECK(rec.arm(take, 8 * kB, 1, RecTap::Align::Step));
  CHECK(rec.state() == RecTap::State::Waiting);
  rec.push(l, r, kB);
  CHECK(rec.frames() == 0u);  // waiting: nothing is written
  CHECK(rec.state() == RecTap::State::Waiting);
  rec.on_step(3, 20);
  CHECK(rec.state() == RecTap::State::Armed);
  rec.push(l, r, kB);
  CHECK(rec.frames() == kB - 20u);
  CHECK(take[0] == 120);  // the sample at the step: l[20]
  CHECK(take[kB - 21u] == l[kB - 1u]);
  rec.push(l, r, kB);  // the next block is whole
  CHECK(rec.frames() == 2u * kB - 20u);
  CHECK(take[kB - 20u] == l[0]);
  rec.on_step(4, 5);  // an Armed take is not restarted
  rec.push(l, r, kB);
  CHECK(rec.frames() == 3u * kB - 20u);
  rec.disarm();
  rec.push(l, r, kB);
  CHECK(rec.state() == RecTap::State::Done);
  rec.release();

  // Align::Bar: only a step that is a multiple of 16.
  CHECK(rec.arm(take, 8 * kB, 1, RecTap::Align::Bar));
  rec.on_step(1, 0);
  rec.on_step(15, 0);
  rec.on_step(17, 0);
  CHECK(rec.state() == RecTap::State::Waiting);
  rec.on_step(32, 63);  // the last sample of its block
  CHECK(rec.state() == RecTap::State::Armed);
  rec.push(l, r, kB);
  CHECK(rec.frames() == 1u);
  CHECK(take[0] == l[63]);
  rec.disarm();
  rec.push(l, r, kB);
  rec.release();

  // Align::Pattern: only step 0.
  CHECK(rec.arm(take, 8 * kB, 1, RecTap::Align::Pattern));
  rec.on_step(16, 0);
  CHECK(rec.state() == RecTap::State::Waiting);
  rec.on_step(0, 0);
  CHECK(rec.state() == RecTap::State::Armed);
  rec.push(l, r, kB);
  CHECK(rec.frames() == kB);
  rec.disarm();
  rec.push(l, r, kB);
  rec.release();

  // Stopped while waiting: Done with no frames, at the next block (the render is the one that makes the transition).
  CHECK(rec.arm(take, 8 * kB, 1, RecTap::Align::Bar));
  rec.disarm();
  CHECK(rec.state() == RecTap::State::Waiting);
  rec.push(l, r, kB);
  CHECK(rec.state() == RecTap::State::Done);
  CHECK(rec.frames() == 0u);
  rec.release();
  CHECK(rec.state() == RecTap::State::Idle);

  // Free is what it always was: Armed at once, a step does nothing to it.
  CHECK(rec.arm(take, 8 * kB, 1));
  CHECK(rec.state() == RecTap::State::Armed);
  rec.on_step(0, 40);
  rec.push(l, r, kB);
  CHECK(rec.frames() == kB);
}

TEST_CASE("rec tap: a mono take is the mix, the left channel or the right channel") {
  static o3os::test::StaticSlot<RecTap> slot;
  RecTap& rec = slot.make();
  alignas(4) static std::int16_t l[kB], r[kB];
  alignas(4) static std::int16_t take[kB + 4];
  for (std::uint32_t i = 0; i < kB; ++i) {
    l[i] = static_cast<std::int16_t>(1000 + 7 * i);
    r[i] = static_cast<std::int16_t>(-300 - 3 * static_cast<int>(i));
  }
  CHECK(rec.channel() == RecTap::Channel::Mix);
  for (RecTap::Channel c : {RecTap::Channel::Mix, RecTap::Channel::Left, RecTap::Channel::Right}) {
    rec.set_channel(c);
    CHECK(rec.channel() == c);
    CHECK(rec.arm(take, kB, 1));
    rec.push(l, r, kB);
    CHECK(rec.state() == RecTap::State::Done);
    for (std::uint32_t i = 0; i < kB; ++i) {
      const std::int16_t want = c == RecTap::Channel::Left ? l[i] : (c == RecTap::Channel::Right ? r[i] : static_cast<std::int16_t>((l[i] + r[i] + 1) >> 1));
      CHECK(take[i] == want);
    }
    rec.release();
  }
  // A take that starts mid-block lands unaligned in a word (take + 1 as the destination of a copy is the kernel's scalar path): the same values.
  rec.set_channel(RecTap::Channel::Left);
  CHECK(rec.arm(take, kB, 1, RecTap::Align::Step));
  rec.on_step(0, 3);
  rec.push(l, r, kB);
  for (std::uint32_t i = 0; i < kB - 3u; ++i) {
    CHECK(take[i] == l[i + 3u]);
  }
}

TEST_CASE("engine: a take armed for a bar starts at the Step event's sample, on the REC bus, with the pattern's own clock") {
  Rig rig(make_engine());
  static o3os::test::StaticSlot<VoicePool> pool_slot;
  VoicePool& pool = pool_slot.make();
  rig.e.attach_voices(&pool);
  static SeqEvent events[kMaxEventsPerBlock];
  EventSource src;
  src.step = &scripted;
  src.buffer = events;
  src.capacity = kMaxEventsPerBlock;
  rig.e.attach_events(src);
  // The input strip alone into REC (not monitored), at unity.
  rig.strip(kInputStrip, gps::Volume, 100.0f);
  rig.strip(kInputStrip, gps::Route, static_cast<float>(route::kRec));
  alignas(4) static std::int16_t take[8 * kB];
  std::memset(take, 0x55, sizeof take);
  g_step_block = 3;
  g_step_offset = 20;
  g_step_value = 32;  // a bar
  CHECK(rig.g().rec().arm(take, 5 * kB - 20u, 1, RecTap::Align::Bar));  // exactly what is left of 8 blocks from the step on: the take ends with the last one
  for (std::uint32_t b = 0; b < 8; ++b) {
    rig.rx(b);
    rig.render();
    if (b < 3u) {
      CHECK(rig.g().rec().frames() == 0u);
      CHECK(rig.g().rec().state() == RecTap::State::Waiting);
    }
  }
  // The take: the left channel at the step (block 3, sample 20) on: (L + R) / 2 of the RX, which is 3/4 of the left (R = -L / 2).
  CHECK(rig.g().rec().state() == RecTap::State::Done);
  const std::uint32_t frames = rig.g().rec().frames();
  CHECK(frames == (8u - 3u) * kB - 20u);
  for (std::uint32_t i = 0; i < frames; ++i) {
    const std::uint32_t at = 3u * kB + 20u + i;
    const std::int32_t l = rx_left(at / kB, at % kB);
    const std::int32_t r = -l / 2;
    CHECK(take[i] == static_cast<std::int16_t>((l + r + 1) >> 1));
  }
}

TEST_CASE("engine: a step that is not on the take's alignment leaves it waiting") {
  Rig rig(make_engine());
  static o3os::test::StaticSlot<VoicePool> pool_slot;
  VoicePool& pool = pool_slot.make();
  rig.e.attach_voices(&pool);
  static SeqEvent events[kMaxEventsPerBlock];
  EventSource src;
  src.step = &scripted;
  src.buffer = events;
  src.capacity = kMaxEventsPerBlock;
  rig.e.attach_events(src);
  alignas(4) static std::int16_t take[4 * kB];
  g_step_block = 1;
  g_step_offset = 10;
  g_step_value = 5;  // not a bar
  CHECK(rig.g().rec().arm(take, 4 * kB, 1, RecTap::Align::Bar));
  for (std::uint32_t b = 0; b < 4; ++b) {
    rig.rx(b);
    rig.render();
  }
  CHECK(rig.g().rec().state() == RecTap::State::Waiting);
  CHECK(rig.g().rec().frames() == 0u);
  rig.g().rec().disarm();
  rig.render();
  CHECK(rig.g().rec().state() == RecTap::State::Done);
  CHECK(rig.g().rec().frames() == 0u);
}

TEST_CASE("graph: the REC tap's source is the REC bus or MAIN after the master level (a resample), and the input strip's REC send needs no monitor") {
  Rig rig(make_engine());
  Graph& g = rig.g();
  alignas(4) static std::int16_t take[kB];
  // Input strip: REC only (not monitored, level at unity); MAIN carries nothing.
  rig.strip(kInputStrip, gps::Volume, 100.0f);
  rig.strip(kInputStrip, gps::Route, static_cast<float>(route::kRec));
  rig.rx(0);
  CHECK(g.rec().source() == RecTap::Source::Bus);
  CHECK(g.rec().arm(take, kB, 1));
  rig.render();
  CHECK(g.rec().frames() == kB);
  CHECK(take[10] == static_cast<std::int16_t>((rx_left(0, 10) + (-rx_left(0, 10) / 2) + 1) >> 1));
  bool heard = false;
  for (std::uint32_t i = 0; i < kBS; ++i) {
    heard = heard || rig.out[i] != 0;
  }
  CHECK_FALSE(heard);  // recorded but not monitored
  g.rec().release();
  // The same input as a MAIN-only route: the REC bus is silent, the master source has it (at the master level).
  rig.strip(kInputStrip, gps::Route, static_cast<float>(route::kMain));
  rig.master(gpm::Volume, 100.0f);
  rig.rx(1);
  g.rec().set_source(RecTap::Source::Bus);
  CHECK(g.rec().arm(take, kB, 1));
  rig.render();
  rig.render();  // the route change is applied at the start of the first block; the take of the second has it
  CHECK(g.rec().state() == RecTap::State::Done);
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(take[i] == 0);
  }
  g.rec().release();
  g.rec().set_source(RecTap::Source::Master);
  CHECK(g.rec().arm(take, kB, 1, RecTap::Align::Free));
  rig.rx(2);
  rig.render();
  CHECK(g.rec().state() == RecTap::State::Done);
  for (std::uint32_t i = 0; i < kB; ++i) {
    const std::int32_t l = rx_left(2, i);
    CHECK(take[i] == static_cast<std::int16_t>((l + (-l / 2) + 1) >> 1));  // MAIN's mix of the two channels
  }
  g.rec().release();
}

TEST_CASE("graph: the input strip is the codec (Sum and CodecOnly) or the USB return alone; the return has its own route to the outputs, shut while the strip carries it; pulled in every mode") {
  Rig rig(make_engine());
  Graph& g = rig.g();
  o3os::usb::Counters counters;
  static o3os::usb::ReturnRing ring;
  o3os::usb::ReturnReader reader(ring, counters);
  g.attach_return(&reader);
  rig.strip(kInputStrip, gps::Volume, 100.0f);
  rig.strip(kInputStrip, gps::Route, static_cast<float>(route::kMain));
  rig.master(gpm::Volume, 100.0f);
  auto prime = [&](std::int32_t v) {  // 288 frames of a constant stream: L = v, R = -v
    std::uint32_t chunk[48];
    for (unsigned i = 0; i < 48u; ++i) {
      chunk[i] = static_cast<std::uint16_t>(v) | (static_cast<std::uint32_t>(static_cast<std::uint16_t>(-v)) << 16);
    }
    for (unsigned c = 0; c < 6u; ++c) {
      ring.write(chunk, 48);
    }
  };
  for (std::uint32_t i = 0; i < kB; ++i) {
    rig.in[2u * i] = 2000;
    rig.in[2u * i + 1u] = -3000;
  }
  CHECK(g.input_feed() == InputFeed::Sum);

  // The host streams. Sum and CodecOnly: the strip is the codec's 2000 / -3000, and the stream joins it at MAIN by its own route (OS-006e, level 100 = unity): 2500 / -3500.
  g.set_return_enabled(true);
  prime(500);
  rig.render();
  CHECK(rig.l(10) == 2500);
  CHECK(rig.r(10) == -3500);
  g.set_input_feed(InputFeed::CodecOnly);
  prime(500);
  const std::uint32_t before = ring.size();
  rig.render();
  CHECK(rig.l(10) == 2500);
  CHECK(rig.r(10) == -3500);
  CHECK(ring.size() < before);  // pulled in every mode: the ring does not overrun
  // The stream's level is its own: shut, the strip is the codec alone (the stream is not in it), and still pulled.
  rig.master(gpm::UsbReturnLevel, 0.0f);
  prime(500);
  rig.render();
  CHECK(rig.l(10) == 2000);
  CHECK(rig.r(10) == -3000);
  rig.master(gpm::UsbReturnLevel, 100.0f);
  prime(500);
  rig.render();
  CHECK(rig.l(10) == 2500);
  // ReturnOnly: the strip is the stream alone, and the route ramps shut over a block (no step) so that the monitor does not double it: 500 / -500 once it is, not 1000.
  g.set_input_feed(InputFeed::ReturnOnly);
  prime(500);
  rig.render();
  prime(500);
  rig.render();
  CHECK(rig.l(10) == 500);
  CHECK(rig.r(10) == -500);

  // No host: the sum and the codec are the codec, the stream alone is silence.
  g.set_return_enabled(false);
  g.set_input_feed(InputFeed::Sum);
  rig.render();
  CHECK(rig.l(10) == 2000);
  g.set_input_feed(InputFeed::CodecOnly);
  rig.render();
  CHECK(rig.l(10) == 2000);
  g.set_input_feed(InputFeed::ReturnOnly);
  rig.render();
  CHECK(rig.silent_out());
  g.set_input_feed(InputFeed::Sum);
}
