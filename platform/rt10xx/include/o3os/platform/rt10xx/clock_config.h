// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The clock tree of the rt10xx image (OS-PORT-RT10XX), as register field values with their arithmetic, so the numbers are checked by the
// compiler and the host tests and the boot code only writes them. Source: the i.MX RT1050 Reference Manual, chapters "Clock Controller
// Module (CCM)", "CCM Analog" and "DCDC". The targets are the part's rated speed, the standard i.MX RT1052 configuration of the vendor's
// examples (600 MHz core, 150 MHz IPG, 75 MHz PERCLK), written from the manual, not copied from a board file. NOT VERIFIED ON THE UNIT:
// the official firmware's core clock has not been measured (docs/specs/OS-PORT-RT10XX.md, open research questions). A wrong clock makes
// the heartbeat run at the wrong rate, which the first hardware session measures before anything is built on it.
//
//   24 MHz crystal (the one every RT10xx boots from; the FlexSPI boot ROM has already set it up)
//     -> DCDC: VDD_SOC to 1.25 V (the manual's requirement for 600 MHz; the reset default is lower)
//     -> ARM PLL (PLL1): Fout = 24 MHz x DIV_SELECT / 2 = 1,200 MHz (DIV_SELECT 100)
//     -> ARM_PODF /2 (CACRR): 600 MHz pll1 divided clock
//     -> PRE_PERIPH_CLK_SEL = 3 (the divided PLL1), PERIPH_CLK_SEL = 0 (CBCDR): 600 MHz periph_clk
//     -> AHB_PODF /1: AHB = core = 600 MHz; IPG_PODF /4: 150 MHz IPG; PERCLK_PODF /2 (CSCMR1): 75 MHz PERCLK (PIT, GPT)
//   SYS PLL (PLL2, 528 MHz) and USB1 PLL (PLL3, 480 MHz) are left as the boot ROM set them in this phase: the FlexSPI the image runs from
//   takes its clock from PLL3's PFD0, and PLL2 feeds nothing yet (the SEMC's clock is phase 2, with its own switch under the same rule:
//   never stop a PLL the code is executing through).

#include <cstdint>

namespace o3os::platform::rt10xx::clock {

inline constexpr std::uint32_t kCrystalHz = 24'000'000u;

// CCM_ANALOG_PLL_ARM[DIV_SELECT], 7 bits, 54..108: Fout = Fref x DIV_SELECT / 2.
inline constexpr std::uint32_t kArmPllDivSelect = 100u;
// CCM_CACRR[ARM_PODF], 3 bits: divide by the value + 1.
inline constexpr std::uint32_t kArmPodf = 1u;
// CCM_CBCDR: AHB_PODF (3 bits, bits 12:10), IPG_PODF (2 bits, bits 9:8), PERIPH_CLK_SEL (bit 25); CCM_CBCMR: PRE_PERIPH_CLK_SEL (bits 19:18).
inline constexpr std::uint32_t kAhbPodf = 0u;
inline constexpr std::uint32_t kIpgPodf = 3u;
inline constexpr std::uint32_t kPrePeriphClkSel = 3u;  // the divided PLL1
// CCM_CSCMR1[PERCLK_PODF], 6 bits (bits 5:0): divide the IPG clock by the value + 1; PERCLK_CLK_SEL (bit 6) = 0: from IPG.
inline constexpr std::uint32_t kPerclkPodf = 1u;
// DCDC_REG3[TRG], 5 bits: VDD_SOC = 0.8 V + TRG x 25 mV. 0x12 = 1.25 V.
inline constexpr std::uint32_t kDcdcVddSocTrg = 0x12u;
inline constexpr std::uint32_t kDcdcTrgMillivolts = 800u + kDcdcVddSocTrg * 25u;

constexpr std::uint32_t arm_pll_hz() noexcept { return kCrystalHz * kArmPllDivSelect / 2u; }
constexpr std::uint32_t pll1_divided_hz() noexcept { return arm_pll_hz() / (kArmPodf + 1u); }
constexpr std::uint32_t ahb_hz() noexcept { return pll1_divided_hz() / (kAhbPodf + 1u); }
constexpr std::uint32_t ipg_hz() noexcept { return ahb_hz() / (kIpgPodf + 1u); }
constexpr std::uint32_t perclk_hz() noexcept { return ipg_hz() / (kPerclkPodf + 1u); }

inline constexpr std::uint32_t kCoreHz = ahb_hz();
inline constexpr std::uint32_t kIpgHz = ipg_hz();
inline constexpr std::uint32_t kPerclkHz = perclk_hz();

// The core clock the boot ROM hands over at (the clock the delays are sized for until init_clocks() has run): the ROM leaves the ARM PLL
// off and the core on the 24 MHz oscillator through periph_clk2 in the configurations seen on this family (open research question for
// this unit's bootloader; a wrong guess only mis-sizes the first delays, which are bounded).
inline constexpr std::uint32_t kBootHz = kCrystalHz;

// SysTick: 1 kHz from the core clock (CLKSOURCE = 1), a 24-bit down-counter.
inline constexpr std::uint32_t kSysTickHz = 1000u;
inline constexpr std::uint32_t kSysTickReload = kCoreHz / kSysTickHz - 1u;

static_assert(kArmPllDivSelect >= 54u && kArmPllDivSelect <= 108u, "PLL_ARM DIV_SELECT range (RM)");
static_assert(kArmPodf <= 7u && kAhbPodf <= 7u && kIpgPodf <= 3u && kPerclkPodf <= 63u && kPrePeriphClkSel <= 3u, "field widths");
static_assert(kDcdcVddSocTrg <= 31u && kDcdcTrgMillivolts == 1250u, "VDD_SOC 1.25 V");
static_assert(arm_pll_hz() == 1'200'000'000u, "ARM PLL 1.2 GHz");
static_assert(kCoreHz == 600'000'000u, "core (AHB) 600 MHz");
static_assert(kIpgHz == 150'000'000u, "IPG 150 MHz (the RM's maximum)");
static_assert(kPerclkHz == 75'000'000u, "PERCLK 75 MHz");
static_assert(kSysTickReload <= 0x00FFFFFFu, "the SysTick reload fits 24 bits");
static_assert(kCoreHz % 1'000'000u == 0u, "whole counts per microsecond (clock.cpp)");

}  // namespace o3os::platform::rt10xx::clock
