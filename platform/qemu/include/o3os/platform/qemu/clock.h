// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/core/tick.h>
#include <o3os/hal/clock.h>

#include <cstdint>

namespace o3os::platform::qemu {

// hal::Clock on the Cortex-M SysTick of the `mps2-an386` model: a 1 kHz interrupt counts milliseconds, and the
// down-counter inside the current millisecond gives the microseconds, the way platform/mk66 will (it has the same
// core). Core clock of the model: 25 MHz.
//
// Shared state: the millisecond count has one writer, the SysTick handler, and is read only inside a critical
// section (PRIMASK saved and restored, so a read from an ISR is fine), because a 64-bit access is two words.
class SysTickClock final : public hal::Clock {
 public:
  static constexpr std::uint32_t kCoreHz = 25'000'000;
  static constexpr std::uint32_t kTickHz = 1000;

  // Programs SysTick (processor clock, 1 kHz, interrupt on) and starts counting from zero. Call once, early.
  static void start() noexcept;

  core::Tick now() const noexcept override;
  std::uint64_t uptime_ms() const noexcept override;
};

// The one system clock (state lives with the SysTick handler).
SysTickClock& system_clock() noexcept;

}  // namespace o3os::platform::qemu
