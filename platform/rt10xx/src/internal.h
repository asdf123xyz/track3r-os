// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Declarations shared between the translation units of the rt10xx image (OS-PORT-RT10XX). Private to src/.

#include <o3os/platform/rt10xx/platform.h>

#include <cstdint>

namespace o3os::platform::rt10xx::boot {

// ---- boot.cpp: the reset sequence's steps, in the order startup.cpp calls them ------------------------------------------------------
void disable_watchdogs() noexcept;      // WDOG1, WDOG2 and the RTWDOG off (the official firmware's SystemInit does the same, FINDINGS 8)
void enable_dwt() noexcept;             // cycle counter running (hal::cycles(), the delays below)
void init_memory_protection() noexcept; // the MPU table of mpu_config.h, then the instruction cache and (O3OS_RT10XX_DCACHE) the data cache
void init_clocks() noexcept;            // VDD_SOC 1.25 V, ARM PLL 1.2 GHz, core 600 MHz, IPG 150, PERCLK 75 (clock_config.h). Fails visibly, never hangs
void init_interrupts() noexcept;        // VTOR, NVIC priorities from hal/irq.h, fault handlers enabled, divide-by-zero traps
void start_systick() noexcept;          // 1 kHz tick; the clock reads valid from here
void capture_reset_cause() noexcept;    // SRC_SRSR, read and cleared

// Busy-wait delays on the cycle counter, bounded whatever the counter does. Sized for clock_config.h kBootHz until init_clocks() has
// finished, for kCoreHz after.
void delay_us(std::uint32_t us) noexcept;
void delay_ms(std::uint32_t ms) noexcept;

// A failure at boot: a panic record with `module` as its module id (crash_record.h: module 0 is "none"), then a halt in WFI with
// interrupts off, never a reset loop. The unit then needs a power cycle; the next image to boot finds the record.
[[noreturn]] void fail(std::uint16_t module) noexcept;

// ---- crash_store.cpp --------------------------------------------------------------------------------------------------------------
void capture_boot_record() noexcept;               // copies a valid noinit record aside, at boot
crash::CrashRecord load_stored_record() noexcept;  // raw copy of the noinit block's record (may be garbage)
void store_record(const crash::CrashRecord& r) noexcept;
std::uint32_t load_context() noexcept;             // crash::kUnknownContext if the stage words are not trustworthy
void reset_context() noexcept;                     // stage Reset, no module
[[noreturn]] void reset_chip() noexcept;           // SYSRESETREQ
[[noreturn]] void halt() noexcept;                 // interrupts off, WFI for ever
std::uint32_t uptime_ms_low() noexcept;            // clock.cpp: SysTick milliseconds since boot, no lock (fault path)

}  // namespace o3os::platform::rt10xx::boot
