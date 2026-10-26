// SPDX-License-Identifier: GPL-3.0-or-later
//
// hal::Clock on SysTick (1 kHz from the core clock) and hal::cycles() on the DWT cycle counter: the MK66 layer's method
// (platform/mk66/src/clock.cpp) on this core's clock (clock_config.h).
#include "internal.h"
#include "regs.h"

#include <o3os/hal/cycles.h>
#include <o3os/platform/rt10xx/clock_config.h>
#include <o3os/platform/rt10xx/platform.h>

namespace o3os::platform::rt10xx {
namespace {

constexpr std::uint32_t kWrapRaceCounts = 64;
static_assert(clock::kSysTickReload + 1u == clock::kCoreHz / clock::kSysTickHz, "one reload per millisecond");
// SysTick counts at the core clock the boot settled on (boot::core_hz(): kCoreHz, or the bootloader's in the safe-boot image).
std::uint32_t reload() noexcept { return boot::core_hz() / clock::kSysTickHz; }
std::uint32_t counts_per_us() noexcept { const std::uint32_t c = boot::core_hz() / 1'000'000u; return c == 0u ? 1u : c; }

volatile std::uint64_t g_uptime_ms = 0;  // written by SysTick_Handler only; read with interrupts masked
core::Tick g_last_now = 0;               // inside now()'s critical section only

std::uint32_t irq_save() noexcept {
  std::uint32_t primask;
  asm volatile("mrs %0, primask\n\tcpsid i" : "=r"(primask)::"memory");
  return primask;
}
void irq_restore(std::uint32_t primask) noexcept { asm volatile("msr primask, %0" ::"r"(primask) : "memory"); }

}  // namespace

extern "C" void SysTick_Handler() { g_uptime_ms = g_uptime_ms + 1; }

core::Tick SysTickClock::now() const noexcept {
  const std::uint32_t primask = irq_save();
  std::uint32_t ms = static_cast<std::uint32_t>(g_uptime_ms);
  const std::uint32_t count = regs::syst_cvr();
  const bool wrapped = (regs::scb_icsr() & regs::kIcsrPendstset) != 0u;
  if (wrapped && count > kWrapRaceCounts) {
    ms += 1u;
  }
  const std::uint32_t elapsed_counts = (reload() - 1u) - count;
  const core::Tick candidate = ms * core::kTicksPerMs + elapsed_counts / counts_per_us();
  const core::Tick result = core::tick_reached(candidate, g_last_now) ? candidate : g_last_now;
  g_last_now = result;
  irq_restore(primask);
  return result;
}

std::uint64_t SysTickClock::uptime_ms() const noexcept {
  const std::uint32_t primask = irq_save();
  const std::uint64_t ms = g_uptime_ms;
  irq_restore(primask);
  return ms;
}

SysTickClock& system_clock() noexcept {
  static SysTickClock clock;
  return clock;
}

namespace boot {
std::uint32_t uptime_ms_low() noexcept { return static_cast<std::uint32_t>(g_uptime_ms); }
}  // namespace boot

}  // namespace o3os::platform::rt10xx

namespace o3os::hal {
std::uint32_t cycles() noexcept { return platform::rt10xx::regs::dwt_cyccnt(); }
}  // namespace o3os::hal
