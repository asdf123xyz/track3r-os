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
