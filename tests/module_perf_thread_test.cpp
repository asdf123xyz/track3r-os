// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-303PERF: ModuleVoices::perf() under a real concurrent writer (host only: std::thread; also the TSan run, `module_perf_tsan`). The render writes an instance's perf cell once a block (a seqlock;
// add() has the render for its one writer, which is what is tested here; clear() has a second, bind(): see PerfCell) and `diag mod` reads it from the main loop, so a read that is returned is one writer's whole publish: every block here costs the same cycles in every bucket, so a read that mixed
// two publishes shows as a sum that is not the block count times the cost.
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/static_slot.h"

#include <o3os/audio/module_voices.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>

namespace {

using namespace o3os;
using audio::ModuleVoices;

constexpr std::uint32_t kBlk = hal::audio::kBlock;
constexpr std::uint32_t kStep = 2;      // a counter read
constexpr std::uint32_t kRenderCost = 100;
// A segment reads the counter at tz, t0, t1, t2, t3, t4 (module_voices.cpp): loop = (t4 - t0) + zero-fill = (kStep + kRenderCost + kStep + kStep + kStep) + kStep, render = kRenderCost + kStep, out = 2 kStep.
constexpr std::uint32_t kLoop = 5u * kStep + kRenderCost;
constexpr std::uint32_t kRender = kRenderCost + kStep;
constexpr std::uint32_t kOut = 2u * kStep;

std::uint32_t g_clock = 0;  // the writer thread's alone: the module's render() and the counter reads are both in the render
std::uint32_t read_clock() noexcept { return g_clock += kStep; }

struct alignas(16) State {
  std::uint32_t gate;
};
int init(void*, const o3m_host*) { return 0; }
std::uint32_t render(void*, const float*, float* out, std::uint32_t n) {
  for (std::uint32_t i = 0; i < n; ++i) {
    out[i] = 0.1f;
  }
  g_clock += kRenderCost;
  return 1;
}
void set_param(void*, std::uint32_t, float) {}
void note_on(void* s, std::uint32_t, std::uint32_t, std::uint32_t) { static_cast<State*>(s)->gate = 1; }
void note_off(void* s, std::uint32_t) { static_cast<State*>(s)->gate = 0; }
void shutdown(void*) {}
const o3m_module kMod = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_INSTRUMENT, sizeof(o3m_module), "perf", sizeof(State), 0, nullptr,
                         init, render, set_param, note_on, note_off, shutdown, nullptr, 0};

}  // namespace

TEST_CASE("module perf thread: a read is one whole publish of the render, never a mixture of two") {
  static o3os::test::StaticSlot<ModuleVoices> slot;
  ModuleVoices& mv = slot.make();
  alignas(16) static std::uint8_t scratch[ModuleVoices::kScratchBytes];
  alignas(16) static State state;
  std::memset(scratch, 0, sizeof scratch);
  std::memset(&state, 0, sizeof state);
  g_clock = 1000;
  NEED(mv.attach_scratch(scratch, sizeof scratch));
  mv.set_cycles(&read_clock);
  NEED(mv.bind(0, kMod, 1, &state, nullptr, 0) == ModuleVoices::Bind::Ok);
  mv.reset_perf();  // (the split is off from boot)

  constexpr std::uint32_t kBlocks = 40000;
  std::atomic<bool> done{false};
  std::thread writer([&] {
    mv.begin_block();
    (void)mv.note_on(0, 60, 0, 100);
    mv.render(0, kBlk);
    mv.end_block();
    for (std::uint32_t b = 1; b < kBlocks; ++b) {
      mv.begin_block();
      mv.render(0, kBlk);
      mv.end_block();
      if (b == kBlocks / 2) {
        mv.reset_perf();  // a reset in the middle: the reader sees the count fall and the figures stay whole
      }
    }
    done.store(true, std::memory_order_release);
  });
  std::uint64_t reads = 0;
  std::uint64_t torn = 0;
  std::uint64_t falls = 0;
  std::uint32_t last = 0;
  while (!done.load(std::memory_order_acquire)) {
    audio::ModulePerf p;
    if (!mv.perf(0, p)) {
      continue;  // the writer was inside a publish four times running: a diagnostic tool tries again
    }
    ++reads;
    const std::uint64_t b = p.blocks;
    const bool whole = p.loop_sum == b * kLoop && p.render_sum == b * kRender && p.out_sum == b * kOut &&
                       (b == 0 || (p.loop_min == kLoop && p.loop_max == kLoop && p.render_min == kRender && p.render_max == kRender && p.out_min == kOut && p.out_max == kOut));
    torn += whole ? 0u : 1u;
    falls += p.blocks < last ? 1u : 0u;
    last = p.blocks;
  }
  writer.join();
  CHECK(reads > 1000u);
  CHECK(torn == 0u);
  CHECK(falls <= 1u);  // the one reset (the count may also read 0 while the render has not served it yet, which is a fall too: never more than the reset itself)
  audio::ModulePerf p;
  NEED(mv.perf(0, p));
  CHECK(p.blocks > 0u);
  CHECK(p.blocks <= kBlocks / 2u);
  CHECK(p.loop_sum == static_cast<std::uint64_t>(p.blocks) * kLoop);
  mv.retire_all();
  mv.release_all();
}
