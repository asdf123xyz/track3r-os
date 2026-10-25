// SPDX-License-Identifier: GPL-3.0-or-later
//
// The ownership protocol of Engine::offline_* (engine.h, OFFLINE RENDERING) against a real second thread that plays the render line: it calls the engine's callback as fast as it can while the
// main thread asks for the engine, renders offline, and gives it back. The claims, under the sanitizers (ASan + UBSan by default, TSan with -DO3OS_TSAN=ON):
//   * once offline_ready(true) is seen, the render thread has stood down: it renders nothing (the live source is not asked, the clock does not move) and writes only silence, while the
//     main thread uses the engine - and what the main thread renders is the blocks a single-threaded engine makes;
//   * offline_leave() gives the engine back: the render thread's blocks go on from the clock the live render had.
// Host only (std::thread).
#include <doctest/doctest.h>

#include <o3os/audio/engine.h>
#include <o3os/audio/voices.h>
#include <o3os/hal/audio.h>

#include "support/need.h"
#include "support/static_slot.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

namespace haudio = o3os::hal::audio;
using namespace o3os::audio;

constexpr std::uint32_t kBlk = haudio::kBlock;

std::uint32_t cycles() noexcept { return 0; }

struct Script {
  std::atomic<std::uint32_t> calls{0};
  // The block start of the first call after `armed` was set (the render thread is parked when the test sets it, and takes the engine back after the release that follows): where the live
  // timeline resumed. The test cannot read the engine's clock while the render thread runs (it is the render's own variable).
  std::atomic<bool> armed{false};
  std::atomic<std::uint64_t> first_start{~std::uint64_t{0}};
};

std::uint32_t script_step(void* self, std::uint64_t block_start, std::uint32_t n, SeqEvent* out, std::uint32_t cap) noexcept {
  Script& s = *static_cast<Script*>(self);
  s.calls.fetch_add(1, std::memory_order_relaxed);
  if (s.armed.load(std::memory_order_relaxed)) {
    std::uint64_t none = ~std::uint64_t{0};
    (void)s.first_start.compare_exchange_strong(none, block_start, std::memory_order_relaxed);
  }
  const std::uint32_t block = static_cast<std::uint32_t>(block_start / n);
  if (block % 8u == 1u && cap > 0u) {
    SeqEvent e{};
    e.offset = 5;
    e.kind = EventKind::NoteOn;
    e.track = 0;
    e.a = 60;
    e.b = 0;
    e.c = 100;
    out[0] = e;
    return 1;
  }
  return 0;
}

struct Rig {
  Rig() : engine(&cycles, taps) {
    for (std::int16_t& s : pcm) {
      s = 7000;
    }
    pool.set_sample(0, SampleDesc{pcm, 4000, 0, 0, 65536});
    engine.attach_voices(&pool);
    engine.attach_live(&live_box);  // the export nulls the engine's pointer to it for its blocks (the live queue is not read): the render thread reads it when it runs, and TSan checks that the park protocol covers it
  }
  void source(Script& s) {
    EventSource e;
    e.self = &s;
    e.step = &script_step;
    e.buffer = events;
    e.capacity = kMaxEventsPerBlock;
    engine.attach_events(e);
  }
  EventSource source_of(Script& s) {
    EventSource e;
    e.self = &s;
    e.step = &script_step;
    e.buffer = export_events;
    e.capacity = kMaxEventsPerBlock;
    return e;
  }
  TapMem taps;
  Engine engine;
  VoicePool pool;
  LiveBox live_box;
  std::int16_t pcm[4000];
  SeqEvent events[kMaxEventsPerBlock];
  SeqEvent export_events[kMaxEventsPerBlock];
};

}  // namespace

TEST_CASE("offline engine threaded: the render thread stands down on the request, the main thread renders offline meanwhile, and the live render takes the engine back") {
  static o3os::test::StaticSlot<Rig> reference_slot;
  static o3os::test::StaticSlot<Rig> slot;
  // The reference: a single thread renders the script's blocks offline.
  std::vector<std::int16_t> reference;
  {
    Rig& r = reference_slot.make();
    Script script;
    r.engine.offline_request();
    r.engine.offline_enter(r.source_of(script));
    alignas(8) std::int16_t in[haudio::kBlockSamples] = {};
    alignas(8) std::int16_t out[haudio::kBlockSamples] = {};
    haudio::BlockReport report;
    for (int b = 0; b < 200; ++b) {
      const haudio::BlockIo io{in, out, 0, &report};
      r.engine.render_offline(io);
      reference.insert(reference.end(), out, out + haudio::kBlockSamples);
    }
    r.engine.offline_leave();
  }

  Rig& r = slot.make();
  Script live;
  r.source(live);
  std::atomic<bool> stop{false};
  std::atomic<std::uint32_t> nonzero_blocks_while_parked{0};
  std::atomic<std::uint32_t> blocks{0};
  std::atomic<bool> parked_view{false};
  std::thread render([&] {
    alignas(8) std::int16_t in[haudio::kBlockSamples] = {};
    alignas(8) std::int16_t out[haudio::kBlockSamples];
    haudio::BlockReport report;
    while (!stop.load(std::memory_order_relaxed)) {
      for (std::int16_t& s : out) {
        s = 0x5555;
      }
      const haudio::BlockIo io{in, out, 0, &report};
      const haudio::BlockCallback cb = r.engine.callback();
      cb.fn(cb.user, io);
      if (parked_view.load(std::memory_order_acquire)) {
        std::uint32_t nz = 0;
        for (std::int16_t s : out) {
          nz += s != 0 ? 1u : 0u;
        }
        nonzero_blocks_while_parked.fetch_add(nz, std::memory_order_relaxed);
      }
      blocks.fetch_add(1, std::memory_order_relaxed);
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
  });

  // Let the live render run, then ask for the engine.
  while (blocks.load() < 20u) {
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  CHECK(live.calls.load() >= 20u);
  r.engine.offline_request();
  bool ready = false;
  for (int i = 0; i < 20000 && !ready; ++i) {
    ready = r.engine.offline_ready(true);
    if (!ready) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  }
  NEED(ready);
  const std::uint32_t live_calls = live.calls.load();
  const std::uint64_t clock = r.engine.sample_clock();
  parked_view.store(true, std::memory_order_release);
  // Wait until the thread has gone round a number of times since the park, so that "it did nothing" is a statement about many blocks.
  const std::uint32_t seen = blocks.load();
  while (blocks.load() < seen + 50u) {
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  CHECK(live.calls.load() == live_calls);
  CHECK(r.engine.sample_clock() == clock);

  Script export_script;
  r.engine.offline_enter(r.source_of(export_script));
  std::vector<std::int16_t> off;
  alignas(8) std::int16_t in[haudio::kBlockSamples] = {};
  alignas(8) std::int16_t out[haudio::kBlockSamples] = {};
  haudio::BlockReport report;
  for (int b = 0; b < 200; ++b) {
    const haudio::BlockIo io{in, out, 0, &report};
    r.engine.render_offline(io);
    off.insert(off.end(), out, out + haudio::kBlockSamples);
    if (b % 20 == 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(200));  // the render thread goes round while the main thread is in the middle of its work
    }
  }
  // (the first block holds the residual of the live render's last voice, which the take-over stopped: it fades within that block; from the second on the blocks are the reference's)
  NEED(off.size() == reference.size());
  CHECK(std::equal(off.begin() + haudio::kBlockSamples, off.end(), reference.begin() + haudio::kBlockSamples));
  CHECK(live.calls.load() == live_calls);
  CHECK(export_script.calls.load() == 200u);

  live.armed.store(true, std::memory_order_relaxed);  // (the render thread is parked: it takes this, and the engine, back with offline_leave's release)
  r.engine.offline_leave();
  parked_view.store(false, std::memory_order_release);
  // The render thread takes the engine back: its source is asked again, from the clock it had.
  bool resumed = false;
  for (int i = 0; i < 20000 && !resumed; ++i) {
    resumed = live.calls.load() > live_calls;
    if (!resumed) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  }
  CHECK(resumed);
  stop.store(true);
  render.join();
  CHECK(live.first_start.load() == clock);  // the live timeline resumed where it stopped (offline_leave gave the clock back)
  CHECK(nonzero_blocks_while_parked.load() == 0u);  // every block it wrote while parked was silence
  CHECK(r.engine.sample_clock() > clock);
}
