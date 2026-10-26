// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the rt10xx platform layer offers the layers above it once the reset sequence (src/startup.cpp) has run (OS-PORT-RT10XX): the
// hal::Clock on SysTick, the boot report (why the chip reset, the crash record the last run left) and the stage and module slots a
// fault handler stamps into a crash record. The same contract as platform/mk66's platform.h, on the other chip; implemented only in the
// rt10xx image build, the declarations compile everywhere so the header check passes on every platform.
//
// The crash record's format and its pure functions (encode, validate, snapshot) are the MK66's (platform/mk66/crash_record.h: Cortex-M
// fault registers and a stage word, nothing of the K66 in them) and are reused as they are; moving them to a chip-neutral directory is a
// backlog row (OS-PORT-RT10XX phase 1 note), not a change of this port.

#include <o3os/core/tick.h>
#include <o3os/hal/clock.h>
#include <o3os/platform/mk66/crash_record.h>

#include <cstdint>

namespace o3os::platform::rt10xx {

namespace crash = o3os::platform::mk66::crash;

// hal::Clock on the Cortex-M SysTick at 1 kHz from the core clock (clock_config.h kCoreHz): a millisecond count in the SysTick handler
// and, from the down-counter inside the current millisecond, microsecond resolution. The MK66 layer's method.
class SysTickClock final : public hal::Clock {
 public:
  core::Tick now() const noexcept override;
  std::uint64_t uptime_ms() const noexcept override;
};

// The one system clock; valid once the reset sequence has started SysTick.
SysTickClock& system_clock() noexcept;

// Reset sources, as the System Reset Controller reports them for the reset that started this run (SRC_SRSR, read then cleared at boot).
// Bits (RT1050 RM "SRC"): 0 power-on (ipp_reset_b), 1 CPU lockup or SYSRESETREQ, 2 CSU, 3 the POR_B pin (ipp_user_reset_b), 4 WDOG1,
// 5 JTAG reset, 6 JTAG software reset, 7 WDOG3 (RTWDOG), 8 the temperature sensor's panic reset.
struct BootInfo {
  std::uint32_t reset_sources;
  bool crash_record_found;
};
inline constexpr std::uint32_t kResetPower = 1u << 0;
inline constexpr std::uint32_t kResetLockupOrSoftware = 1u << 1;
inline constexpr std::uint32_t kResetPin = 1u << 3;
inline constexpr std::uint32_t kResetWatchdog = 1u << 4;
inline constexpr std::uint32_t kResetRtWatchdog = 1u << 7;
inline constexpr std::uint32_t kResetTemperature = 1u << 8;

const BootInfo& boot_info() noexcept;

// The crash record that was waiting at reset, or nullptr (a copy taken before anything could overwrite it; valid for the whole run).
const crash::CrashRecord* boot_crash_record() noexcept;
void clear_crash_record() noexcept;

// Where the OS is, for the next crash record (a word and its complement in the noinit block).
void set_stage(crash::Stage stage) noexcept;
void set_module(std::uint16_t module) noexcept;

// A software panic: records a crash with exception = crash::kPanicException and the caller's context, then resets. Never returns.
[[noreturn]] void panic() noexcept;

}  // namespace o3os::platform::rt10xx
