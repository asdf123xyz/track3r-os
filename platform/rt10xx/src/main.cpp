// SPDX-License-Identifier: GPL-3.0-or-later
//
// The probe image's main (OS-PORT-RT10XX phase 1): the reset sequence (startup.cpp) has assigned the FlexRAM, set the vector table,
// stopped the watchdogs, brought up the MPU, the caches, the clock tree and SysTick; main idles in WFI with a heartbeat counter. There
// is no display, audio, card, USB or input yet: those are the later phases of docs/specs/OS-PORT-RT10XX.md. What this image proves on the
// unit is only that the boot path completes and that the unit stays up (it does not reset, it does not fault); a crash record left by
// a fault is found by the next image that boots and can report it (the console of phase 2).
//
// O3OS_RT10XX_FAULT_DRILL: 10 s after power-up, one deliberate UsageFault (an undefined instruction), to prove the crash record round
// trip once an image can read it back. OFF in the default image.
#include "internal.h"
#include "regs.h"

#include <o3os/platform/rt10xx/mem_map.h>
#include <o3os/platform/rt10xx/platform.h>
#include <o3os/platform/rt10xx/sections.h>

#include <cstdint>

#ifndef O3OS_RT10XX_FAULT_DRILL
#define O3OS_RT10XX_FAULT_DRILL 0
#endif

namespace rt10xx = o3os::platform::rt10xx;

namespace {

// Seconds since boot, counted by the main loop; the loop is the only writer. For a debugger, and for `diag` once there is one.
volatile std::uint32_t g_heartbeat = 0;
// The boot report of this run, kept where a debugger (or the next phase's console) can find it.
volatile std::uint32_t g_reset_sources = 0;
volatile std::uint32_t g_boot_crash_count = 0;

// The module id the drill's fault carries, outside any id a loader hands out (the MK66's drill uses 0xD1 too).
constexpr std::uint16_t kDrillModule = 0xD1;
constexpr std::uint32_t kDrillAfterMs = 10'000;

// A function that runs from the ITCM: if the heartbeat keeps counting, `.ramfunc_l` was copied from flash and runs there (the first
// hardware session reads the counter through the next phase's console, or a debugger).
O3OS_RAMFUNC_L std::uint32_t count_from_itcm(std::uint32_t value) noexcept { return value + 1u; }

}  // namespace

int main() {
  g_reset_sources = rt10xx::boot_info().reset_sources;
  if (const rt10xx::crash::CrashRecord* record = rt10xx::boot_crash_record()) {
    g_boot_crash_count = record->count;
    // Kept, not cleared: nothing can report it yet. The record's count keeps growing across crashes, which is the boot counter of
    // ADR 0002; the phase-2 console clears it once it has shown it.
  }

  std::uint64_t next_second = rt10xx::system_clock().uptime_ms() + 1000u;
  for (;;) {
    asm volatile("wfi");
    const std::uint64_t now = rt10xx::system_clock().uptime_ms();
    if (now >= next_second) {
      next_second += 1000u;
      g_heartbeat = count_from_itcm(g_heartbeat);
#if O3OS_RT10XX_FAULT_DRILL
      if (now >= kDrillAfterMs) {
        rt10xx::set_module(kDrillModule);
        asm volatile("udf #0");  // UsageFault: undefined instruction
      }
#endif
    }
  }
}
