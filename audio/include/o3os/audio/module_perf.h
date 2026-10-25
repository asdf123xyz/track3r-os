// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-303PERF: what the render measured of one module instance, block by block (ModuleVoices::perf), as plain numbers. The measure is off from boot, `diag cpu reset` turns it on and `diag cpu perf off` turns it off again. Cycles are the engine's counter (the DWT on the unit). A sample is one
// FULL block of the instance (kBlock frames of it, in however many segments): a block that began or ended mid-way says too little about the module and is left out, so `min` is the floor of a
// sustained note and `max` the worst block, preemption and cold caches included (the counter is the wall clock).
//
//   loop    the instance's whole iteration in the render: the zero-fill of its block, enter(), the module's render(), leave(), the conversion to Q15 (the governor's figure plus the zero-fill; while the measure
//           is on, the governor's figure itself includes the three extra counter reads of each segment, which it does not with the measure off)
//   render  the module's render() call alone (what `diag bench tb303` times on a private instance, here on the live one, in the song)
//   out     the output side of the loop: the zero-fill and the conversion
//   hooks   what is left of the loop: enter() and leave(), the crash-context stamps (derived: loop - render - out)
// loop - render is what the wrapper costs; render at the same load is what `diag bench` should read, and the difference between them is the interrupts and the cold caches.

#include <cstdint>

namespace o3os::audio {

struct ModulePerf {
  std::uint32_t blocks = 0;  // full blocks measured since the bind or the last reset (0: nothing to report)
  std::uint32_t loop_min = 0, loop_max = 0;
  std::uint32_t render_min = 0, render_max = 0;
  std::uint32_t out_min = 0, out_max = 0;
  std::uint64_t loop_sum = 0, render_sum = 0, out_sum = 0;

  std::uint32_t loop_mean() const noexcept { return mean(loop_sum); }
  std::uint32_t render_mean() const noexcept { return mean(render_sum); }
  std::uint32_t out_mean() const noexcept { return mean(out_sum); }
  // enter() + leave(): the loop less the render and the output side (each mean rounded, so it may be off by one cycle; never negative)
  std::uint32_t hooks_mean() const noexcept {
    const std::uint32_t rest = render_mean() + out_mean();
    return loop_mean() > rest ? loop_mean() - rest : 0u;
  }

 private:
  std::uint32_t mean(std::uint64_t sum) const noexcept { return blocks != 0u ? static_cast<std::uint32_t>((sum + blocks / 2u) / blocks) : 0u; }
};

}  // namespace o3os::audio
