// SPDX-License-Identifier: GPL-3.0-or-later
//
// The reset sequence's steps of the rt10xx image (OS-PORT-RT10XX), called by startup.cpp in order: watchdogs, cycle counter, the MPU
// and the caches, the clock tree, the NVIC, SysTick. Every wait is bounded and a step that does not complete ends in fail(), never in a
// hang. The register facts are regs.h's; the values are clock_config.h's and mpu_config.h's.
#include "internal.h"
#include "regs.h"

#include <o3os/hal/irq.h>
#include <o3os/platform/rt10xx/clock_config.h>
#include <o3os/platform/rt10xx/irq_map.h>
#include <o3os/platform/rt10xx/mem_map.h>
#include <o3os/platform/rt10xx/mpu_config.h>

#include <cstdint>

#ifndef O3OS_RT10XX_DCACHE
#define O3OS_RT10XX_DCACHE 1
#endif

namespace o3os::platform::rt10xx::boot {
namespace {

// The module ids a boot failure's panic record carries (outside any id a module loader hands out: real ids start at 1, these are above
// 0xD0 like the MK66's drill ids).
constexpr std::uint16_t kFailDcdc = 0xE1;
constexpr std::uint16_t kFailPeriphSwitch = 0xE2;
constexpr std::uint16_t kFailArmPll = 0xE3;
constexpr std::uint16_t kFailDividers = 0xE4;
constexpr std::uint16_t kFailMpu = 0xE5;

// The core clock the delays are sized for: the boot ROM's until init_clocks() finishes.
std::uint32_t g_core_hz = clock::kBootHz;

// A bounded poll: `done()` is tried until it is true or `polls` attempts have gone by.
template <typename Predicate>
bool wait_until(Predicate done, std::uint32_t polls = 1'000'000u) noexcept {
  for (std::uint32_t i = 0; i < polls; ++i) {
    if (done()) {
      return true;
    }
  }
  return false;
}

// A CCM handshake: the busy bit clears when the divider or the mux has switched.
bool wait_ccm(std::uint32_t busy_bit) noexcept {
  return wait_until([busy_bit] { return (regs::ccm_cdhipr() & busy_bit) == 0u; });
}

}  // namespace

// ---- Watchdogs -------------------------------------------------------------------------------------------------------------------
void disable_watchdogs() noexcept {
  using namespace regs;
  // WDOG1 and WDOG2: WDE can be cleared only while the watchdog has not been enabled since reset; the boot ROM leaves them so.
  wdog1_wcr() = static_cast<std::uint16_t>(wdog1_wcr() & ~kWdogWcrWde);
  wdog2_wcr() = static_cast<std::uint16_t>(wdog2_wcr() & ~kWdogWcrWde);
  // RTWDOG: the unlock key within its window, then EN off with UPDATE kept so a later slice may enable it deliberately.
  if ((rtwdog_cs() & kRtwdogCsCmd32En) != 0u) {
    rtwdog_cnt() = kRtwdogUnlock;
  } else {
    rtwdog_cnt() = kRtwdogUnlockLow;
    rtwdog_cnt() = kRtwdogUnlockHigh;
  }
  rtwdog_toval() = 0xFFFFu;
  rtwdog_cs() = (rtwdog_cs() & ~kRtwdogCsEn) | kRtwdogCsUpdate;
}

// ---- Cycle counter and delays ----------------------------------------------------------------------------------------------------
void enable_dwt() noexcept {
  regs::demcr() = regs::demcr() | regs::kDemcrTrcena;
  regs::dwt_lar() = regs::kDwtLarKey;  // the Cortex-M7's DWT is locked until the key is written
  regs::dwt_cyccnt() = 0;
  regs::dwt_ctrl() = regs::dwt_ctrl() | regs::kDwtCyccntena;
}

void delay_us(std::uint32_t us) noexcept {
  const std::uint32_t wait = us * (g_core_hz / 1'000'000u);
  const std::uint32_t start = regs::dwt_cyccnt();
  for (std::uint32_t spin = 0; spin < wait; ++spin) {  // bounded even if the counter is not running
    if (static_cast<std::uint32_t>(regs::dwt_cyccnt() - start) >= wait) {
      return;
    }
  }
}

void delay_ms(std::uint32_t ms) noexcept {
  for (std::uint32_t i = 0; i < ms; ++i) {
    delay_us(1000);
  }
}

// ---- MPU and caches --------------------------------------------------------------------------------------------------------------
namespace {

// Invalidate the whole data cache by set and way (ARMv7-M ARM B2.2.6: CCSIDR gives sets and ways; DCISW takes set << 5 | way << (32 - log2 ways)).
void invalidate_dcache() noexcept {
  regs::scb_csselr() = 0;  // level 1, data
  asm volatile("dsb" ::: "memory");
  const std::uint32_t ccsidr = regs::scb_ccsidr();
  const std::uint32_t sets = ((ccsidr >> 13) & 0x7FFFu) + 1u;
  const std::uint32_t ways = ((ccsidr >> 3) & 0x3FFu) + 1u;
  unsigned way_shift = 32;
  for (std::uint32_t w = ways - 1u; w != 0u; w >>= 1u) {
    --way_shift;
  }
  for (std::uint32_t set = 0; set < sets; ++set) {
    for (std::uint32_t way = 0; way < ways; ++way) {
      regs::scb_dcisw() = (set << 5) | (way << way_shift);
    }
  }
  asm volatile("dsb\n\tisb" ::: "memory");
}

}  // namespace

void init_memory_protection() noexcept {
  using namespace regs;
  const unsigned regions = (mpu_type() >> kMpuTypeDregionShift) & 0xFFu;
  if (regions < mpu::kRegionCount) {
    fail(kFailMpu);  // not the Cortex-M7 this port expects
  }
  asm volatile("dmb" ::: "memory");
  mpu_ctrl() = 0;
  for (unsigned i = 0; i < regions; ++i) {  // every region written: the ones not in the table are disabled
    mpu_rnr() = i;
    if (i < mpu::kRegionCount) {
      mpu_rbar() = mpu::rbar_of(mpu::kRegions[i]);
      mpu_rasr() = mpu::rasr_of(mpu::kRegions[i]);
    } else {
      mpu_rbar() = (1u << 4) | i;
      mpu_rasr() = 0;
    }
  }
  mpu_ctrl() = mpu::kCtrlEnable;
  asm volatile("dsb\n\tisb" ::: "memory");

  // Instruction cache: invalidate, enable.
  scb_iciallu() = 0;
  asm volatile("dsb\n\tisb" ::: "memory");
  scb_ccr() = scb_ccr() | kCcrIc;
  asm volatile("dsb\n\tisb" ::: "memory");
#if O3OS_RT10XX_DCACHE
  // Data cache: invalidate every line (the contents are undefined after reset), enable. Sound because no bus master touches a cacheable
  // region (mpu_config.h): the SDRAM is the core's alone, the TCMs and the OCRAM are not cached.
  invalidate_dcache();
  scb_ccr() = scb_ccr() | kCcrDc;
  asm volatile("dsb\n\tisb" ::: "memory");
#endif
}

// ---- Clocks ----------------------------------------------------------------------------------------------------------------------
// The sequence (clock_config.h has the targets and the arithmetic):
//   VDD_SOC up -> the periph clock onto the 24 MHz oscillator (so the ARM PLL can be reprogrammed under it) -> ARM PLL bypassed,
//   enabled, locked, bypass off -> ARM_PODF -> AHB and IPG dividers -> pre_periph = the divided PLL1 -> periph clock back onto it ->
//   PERCLK. The FlexSPI the code runs from keeps its clock (PLL3 PFD0) untouched throughout.
void init_clocks() noexcept {
  using namespace regs;

  // VDD_SOC to 1.25 V, waiting for the converter to settle.
  dcdc_reg3() = (dcdc_reg3() & ~kDcdcReg3TrgMask) | clock::kDcdcVddSocTrg;
  if (!wait_until([] { return (dcdc_reg0() & kDcdcReg0StsDcOk) != 0u; })) {
    fail(kFailDcdc);
  }

  // periph_clk2 from the oscillator, and the periph clock onto periph_clk2.
  ccm_cbcmr() = (ccm_cbcmr() & ~kCbcmrPeriphClk2SelMask) | kCbcmrPeriphClk2SelOsc;
  if (!wait_ccm(kCdhiprPeriph2ClkSelBusy)) {
    fail(kFailPeriphSwitch);
  }
  ccm_cbcdr() = ccm_cbcdr() | kCbcdrPeriphClkSel;
  if (!wait_ccm(kCdhiprPeriphClkSelBusy)) {
    fail(kFailPeriphSwitch);
  }
  g_core_hz = clock::kCrystalHz;

  // The ARM PLL: bypassed (its output is the oscillator meanwhile), powered, enabled with the multiplier, locked, then the bypass off.
  pll_arm() = (pll_arm() & ~kPllBypassClkSrcMask) | kPllBypass;
  pll_arm() = kPllBypass | kPllEnable | (clock::kArmPllDivSelect & kPllArmDivSelectMask);  // POWERDOWN clear
  if (!wait_until([] { return (pll_arm() & kPllLock) != 0u; })) {
    fail(kFailArmPll);
  }
  pll_arm_clr() = kPllBypass;

  // ARM_PODF, then the AHB and IPG dividers, each with its handshake.
  ccm_cacrr() = (ccm_cacrr() & ~kCacrrArmPodfMask) | clock::kArmPodf;
  if (!wait_ccm(kCdhiprArmPodfBusy)) {
    fail(kFailDividers);
  }
  ccm_cbcdr() = (ccm_cbcdr() & ~(kCbcdrAhbPodfMask | kCbcdrIpgPodfMask)) | (clock::kAhbPodf << kCbcdrAhbPodfShift) |
                (clock::kIpgPodf << kCbcdrIpgPodfShift);
  if (!wait_ccm(kCdhiprAhbPodfBusy)) {
    fail(kFailDividers);
  }

  // pre_periph = the divided PLL1, and the periph clock back onto pre_periph: the core runs at kCoreHz from here.
  ccm_cbcmr() = (ccm_cbcmr() & ~kCbcmrPrePeriphClkSelMask) | (clock::kPrePeriphClkSel << kCbcmrPrePeriphClkSelShift);
  ccm_cbcdr() = ccm_cbcdr() & ~kCbcdrPeriphClkSel;
  if (!wait_ccm(kCdhiprPeriphClkSelBusy)) {
    fail(kFailPeriphSwitch);
  }
  g_core_hz = clock::kCoreHz;

  // PERCLK (PIT, GPT) from the IPG clock.
  ccm_cscmr1() = (ccm_cscmr1() & ~(kCscmr1PerclkPodfMask | kCscmr1PerclkClkSel)) | clock::kPerclkPodf;
}

// ---- Interrupts ------------------------------------------------------------------------------------------------------------------
void init_interrupts() noexcept {
  using namespace regs;
  scb_vtor() = mem::kImageBase;  // set in step 1 already; stated again where the NVIC is set up

  // Priority grouping 0: all four implemented bits are preemption priority; ADR 0013 D1 depends on a plain 16-level order.
  scb_aircr() = kAircrVectkey;

  for (unsigned word = 0; word < (kIrqCount + 31u) / 32u; ++word) {
    nvic_icer(word) = 0xFFFFFFFFu;  // nothing enabled: the bootloader may have left lines on
  }
  for (unsigned line = 0; line < kIrqCount; ++line) {
    nvic_ipr(line) = hal::irq::priority(role_of(static_cast<Irq>(line)));
  }
  scb_shp(4) = hal::irq::priority(kMemManageRole);
  scb_shp(5) = hal::irq::priority(kBusFaultRole);
  scb_shp(6) = hal::irq::priority(kUsageFaultRole);
  scb_shp(11) = hal::irq::priority(kSvCallRole);
  scb_shp(14) = hal::irq::priority(kPendSvRole);
  scb_shp(15) = hal::irq::priority(kSysTickRole);

  scb_shcsr() = scb_shcsr() | kShcsrFaultsEnable;
  scb_ccr() = scb_ccr() | kCcrDiv0Trp;
  asm volatile("dsb\n\tisb" ::: "memory");
}

void start_systick() noexcept {
  using namespace regs;
  syst_rvr() = clock::kSysTickReload;
  syst_cvr() = 0;
  syst_csr() = kSystCsrEnable | kSystCsrTickint | kSystCsrClksource;
}

// ---- Failure ---------------------------------------------------------------------------------------------------------------------
[[noreturn]] void fail(std::uint16_t module) noexcept {
  set_module(module);
  panic();  // crash_store.cpp: a boot-stage panic halts instead of resetting (no reset loop on a unit with no output yet)
}

}  // namespace o3os::platform::rt10xx::boot
