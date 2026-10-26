// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The audio sample clock, as ONE constant for the whole OS (ADR 0012 amendment 2, ADR 0013 D1): MCLK is derived from the CPU
// clock, so one sample is an exactly known number of CPU cycles, written as a rational. The I2S divider, the block length in
// cycles, the USB feedback value and rate estimator, the HW-A test tone and the SGTL5000's clock checks all follow from it; it
// lives here, below both `usb` and `hal`, so neither depends on the other for it.
//
//   1800000/441 D-OS-8, exactly 44,100 Hz (the default): MCLK = 180 MHz x 196/3125 = 11.2896 MHz = 256 x 44,100 Hz
//   4080/1      the pre-1.0 clock: MCLK = 180 MHz x 16/255 = 11.294 MHz = 256 x 44,117.647 Hz (+400 ppm against 44,100)
// The build picks one: `-DO3OS_RATE_44100=OFF` selects 4080/1; the option (CMakeLists.txt, default ON) defines O3OS_RATE_44100 for every target, and this
// header is the only place that reads it, so the option changes this constant and nothing else.
//
// `kCpuHz` is the core clock the rational is relative to. It is a plain number here because `core` knows no platform: the MK66
// image static_asserts it equals its clock tree (platform/mk66 audio_config.h: clock::kCoreHz == kCpuHz), so a change of the core
// clock cannot silently change what a "cycle per sample" means. OS-PORT-RT10XX: a build for another chip defines O3OS_CPU_HZ (the
// rt10xx's 600 MHz, from the root CMakeLists.txt, for every target of that build); the 44,100 Hz rational is then kCpuHz / 100 over
// 441 (6000000/441 at 600 MHz: the same number as before at 180 MHz), and the pre-1.0 clock exists only on the MK66.

#include <cstdint>

namespace o3os::core {

struct CyclesPerSample {
  std::uint32_t num;
  std::uint32_t den;
};

#if defined(O3OS_CPU_HZ)
inline constexpr std::uint32_t kCpuHz = O3OS_CPU_HZ;
#else
inline constexpr std::uint32_t kCpuHz = 180000000u;
#endif
static_assert(kCpuHz % 100u == 0u && kCpuHz >= 100'000'000u, "the core clock is a whole number of hundreds of hertz, at least 100 MHz");

#if defined(O3OS_RATE_44100) && O3OS_RATE_44100
inline constexpr CyclesPerSample kRefCyclesPerSample{kCpuHz / 100u, 441u};
#else
static_assert(kCpuHz == 180000000u, "the pre-1.0 clock (4080 cycles per sample, 44,117.647 Hz) is the MK66's: O3OS_RATE_44100=OFF only at 180 MHz");
inline constexpr CyclesPerSample kRefCyclesPerSample{4080u, 1u};
#endif

// The engine's rate in Hz as a float constant (44,100 or 44,117.647), for DSP constants that are written as a time over the rate.
// Computed once at compile time from integers (the whole Hz, then the remainder over the cycles per sample; no double in the tree).
inline constexpr float kRefRateHz =
    static_cast<float>(static_cast<std::uint64_t>(kCpuHz) * kRefCyclesPerSample.den / kRefCyclesPerSample.num) +
    static_cast<float>(static_cast<std::uint64_t>(kCpuHz) * kRefCyclesPerSample.den % kRefCyclesPerSample.num) / static_cast<float>(kRefCyclesPerSample.num);

// Whole samples in `ms` milliseconds at the engine's rate, truncated (exact integer arithmetic; `ms` below 2^20 cannot overflow).
constexpr std::uint64_t samples_in_ms(std::uint64_t ms) noexcept {
  return ms * kCpuHz * kRefCyclesPerSample.den / (static_cast<std::uint64_t>(kRefCyclesPerSample.num) * 1000u);
}

}  // namespace o3os::core
