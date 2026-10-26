// SPDX-License-Identifier: GPL-3.0-or-later
//
// The clock tree's arithmetic (clock_config.h): the field values give the targets, and the SysTick reload fits.
#include <doctest/doctest.h>

#include <o3os/platform/rt10xx/clock_config.h>

namespace clock = o3os::platform::rt10xx::clock;

TEST_CASE("rt10xx clock_config: the tree") {
  CHECK(clock::kCrystalHz == 24'000'000u);
  CHECK(clock::arm_pll_hz() == 1'200'000'000u);
  CHECK(clock::pll1_divided_hz() == 600'000'000u);
  CHECK(clock::kCoreHz == 600'000'000u);
  CHECK(clock::kIpgHz == 150'000'000u);
  CHECK(clock::kPerclkHz == 75'000'000u);
  CHECK(clock::kDcdcTrgMillivolts == 1250u);
  CHECK(clock::kSysTickHz == 1000u);
  CHECK(clock::kSysTickReload == 599'999u);
  CHECK(clock::kSysTickReload <= 0xFFFFFFu);
  CHECK(clock::kBootHz == clock::kCrystalHz);
}

TEST_CASE("rt10xx clock_config: field widths (RM)") {
  CHECK(clock::kArmPllDivSelect <= 127u);
  CHECK(clock::kArmPodf <= 7u);
  CHECK(clock::kAhbPodf <= 7u);
  CHECK(clock::kIpgPodf <= 3u);
  CHECK(clock::kPerclkPodf <= 63u);
  CHECK(clock::kPrePeriphClkSel == 3u);
  CHECK(clock::kDcdcVddSocTrg == 0x12u);
}

TEST_CASE("rt10xx clock: the tree's clock") {
  // The host compiles this header at the MK66's clock; the rt10xx build sets O3OS_CPU_HZ to the core clock here.
  CHECK(clock::kTreeIsRt10xx == (o3os::core::kCpuHz == clock::kCoreHz));
#if defined(O3OS_CPU_HZ) && O3OS_CPU_HZ == 600000000u
  CHECK(clock::kTreeIsRt10xx);
#endif
}

TEST_CASE("rt10xx clock: the running clock read back from the CCM's words") {
  using clock::ClockWords;
  constexpr std::uint32_t kLocked = 1u << 31;
  // This port's tree: the ARM PLL at x100 / 2 = 1.2 GHz, ARM_PODF 1 (/2), AHB /1.
  CHECK(clock::running_core_hz(ClockWords{0, 3u << 18, 1u, kLocked | (1u << 13) | 100u, 0, 0, 0}) == 600'000'000u);
  // The same tree with the ARM PLL bypassed: the oscillator / 2.
  CHECK(clock::running_core_hz(ClockWords{0, 3u << 18, 1u, kLocked | (1u << 16) | 100u, 0, 0, 0}) == 12'000'000u);
  // periph_clk2 from the oscillator (the switch-over state of init_clocks()).
  CHECK(clock::running_core_hz(ClockWords{1u << 25, 1u << 12, 0, 0, 0, 0, 0}) == 24'000'000u);
  // periph_clk2 from PLL3, PERIPH_CLK2_PODF 3 (/4): 120 MHz.
  CHECK(clock::running_core_hz(ClockWords{(1u << 25) | (3u << 27), 0, 0, 0, 0, 0, kLocked}) == 120'000'000u);
  // pre_periph = PLL2 PFD2 at its reset fraction 24 (396 MHz), AHB /2.
  CHECK(clock::running_core_hz(ClockWords{1u << 10, 1u << 18, 0, 0, kLocked | 1u, 24u << 16, 0}) == 198'000'000u);
  // pre_periph = PLL2 (528 MHz).
  CHECK(clock::running_core_hz(ClockWords{0, 0, 0, 0, kLocked | 1u, 0, 0}) == 528'000'000u);
  CHECK(clock::pfd_hz(480'000'000u, 12) == 720'000'000u);
  CHECK(clock::pfd_hz(480'000'000u, 5) == 0u);
}
