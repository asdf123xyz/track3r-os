// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Peripheral registers of the i.MX RT10xx and the Cortex-M7 core that the rt10xx platform layer touches, with their field positions
// (OS-PORT-RT10XX). Private to src/: nothing outside this directory sees a register. Written from the i.MX RT1050 Reference Manual
// (the chapter is named at each block) and the ARMv7-M Architecture Reference Manual; NXP's device header was used only as a fact
// table to cross-check addresses and field positions.

#include <cstdint>

namespace o3os::platform::rt10xx::regs {

template <typename T>
inline volatile T& at(std::uintptr_t address) noexcept {
  return *reinterpret_cast<volatile T*>(address);
}

// ---- IOMUXC_GPR (RM "IOMUXC GPR"), 0x400A_C000: the FlexRAM configuration --------------------------------------------------------
inline constexpr std::uintptr_t kIomuxcGpr = 0x400AC000u;
inline constexpr std::uintptr_t kGpr16Address = kIomuxcGpr + 0x40u;  // FLEXRAM_BANK_CFG_SEL (bit 2); startup.cpp names these two in asm
inline constexpr std::uintptr_t kGpr17Address = kIomuxcGpr + 0x44u;  // FLEXRAM_BANK_CFG
inline volatile std::uint32_t& gpr16() noexcept { return at<std::uint32_t>(kGpr16Address); }
inline volatile std::uint32_t& gpr17() noexcept { return at<std::uint32_t>(kGpr17Address); }

// ---- CCM (RM "Clock Controller Module"), 0x400F_C000 ----------------------------------------------------------------------------
inline constexpr std::uintptr_t kCcm = 0x400FC000u;
inline volatile std::uint32_t& ccm_cacrr() noexcept { return at<std::uint32_t>(kCcm + 0x10u); }   // ARM_PODF [2:0]
inline volatile std::uint32_t& ccm_cbcdr() noexcept { return at<std::uint32_t>(kCcm + 0x14u); }   // IPG_PODF [9:8], AHB_PODF [12:10], PERIPH_CLK_SEL [25]
inline volatile std::uint32_t& ccm_cbcmr() noexcept { return at<std::uint32_t>(kCcm + 0x18u); }   // PERIPH_CLK2_SEL [13:12], PRE_PERIPH_CLK_SEL [19:18]
inline volatile std::uint32_t& ccm_cscmr1() noexcept { return at<std::uint32_t>(kCcm + 0x1Cu); }  // PERCLK_PODF [5:0], PERCLK_CLK_SEL [6]
inline volatile std::uint32_t& ccm_cdhipr() noexcept { return at<std::uint32_t>(kCcm + 0x48u); }  // the handshake busy bits
inline volatile std::uint32_t& ccm_ccgr3() noexcept { return at<std::uint32_t>(kCcm + 0x74u); }   // clock gates: semc = CG2 (bits 5:4)
inline volatile std::uint32_t& ccm_ccgr4() noexcept { return at<std::uint32_t>(kCcm + 0x78u); }   // iomuxc = CG1 (bits 3:2)
inline volatile std::uint32_t& ccm_ccgr6() noexcept { return at<std::uint32_t>(kCcm + 0x80u); }   // usboh3 = CG0 (bits 1:0)
inline constexpr std::uint32_t kCcgr3Semc = 0x3u << 4;
inline constexpr std::uint32_t kCcgr4Iomuxc = 0x3u << 2;
inline constexpr std::uint32_t kCcgr6Usboh3 = 0x3u << 0;
inline constexpr std::uint32_t kCbcdrSemcClkSel = 1u << 6;     // 0: the periph clock, 1: the alternate (PLL2 PFD2 or PLL3 PFD1)
inline constexpr std::uint32_t kCbcdrSemcAltClkSel = 1u << 7;
inline constexpr std::uint32_t kCbcdrSemcPodfShift = 16;
inline constexpr std::uint32_t kCbcdrSemcPodfMask = 0x7u << kCbcdrSemcPodfShift;
inline constexpr std::uint32_t kCacrrArmPodfMask = 0x7u;
inline constexpr std::uint32_t kCbcdrIpgPodfShift = 8;
inline constexpr std::uint32_t kCbcdrIpgPodfMask = 0x3u << kCbcdrIpgPodfShift;
inline constexpr std::uint32_t kCbcdrAhbPodfShift = 10;
inline constexpr std::uint32_t kCbcdrAhbPodfMask = 0x7u << kCbcdrAhbPodfShift;
inline constexpr std::uint32_t kCbcdrPeriphClkSel = 1u << 25;  // 1: periph_clk2 (the bypass path), 0: pre_periph (the PLLs)
inline constexpr std::uint32_t kCbcmrPeriphClk2SelShift = 12;
inline constexpr std::uint32_t kCbcmrPeriphClk2SelMask = 0x3u << kCbcmrPeriphClk2SelShift;  // 1: the 24 MHz oscillator
inline constexpr std::uint32_t kCbcmrPeriphClk2SelOsc = 1u << kCbcmrPeriphClk2SelShift;
inline constexpr std::uint32_t kCbcmrPrePeriphClkSelShift = 18;
inline constexpr std::uint32_t kCbcmrPrePeriphClkSelMask = 0x3u << kCbcmrPrePeriphClkSelShift;
inline constexpr std::uint32_t kCscmr1PerclkPodfMask = 0x3Fu;
inline constexpr std::uint32_t kCscmr1PerclkClkSel = 1u << 6;
inline constexpr std::uint32_t kCdhiprSemcPodfBusy = 1u << 0;
inline constexpr std::uint32_t kCdhiprAhbPodfBusy = 1u << 1;
inline constexpr std::uint32_t kCdhiprPeriph2ClkSelBusy = 1u << 3;
inline constexpr std::uint32_t kCdhiprPeriphClkSelBusy = 1u << 5;
inline constexpr std::uint32_t kCdhiprArmPodfBusy = 1u << 16;

// ---- CCM_ANALOG (RM "CCM Analog"), 0x400D_8000: the PLLs; each register has SET (+4), CLR (+8) and TOG (+C) aliases ---------------
inline constexpr std::uintptr_t kCcmAnalog = 0x400D8000u;
inline volatile std::uint32_t& pll_arm() noexcept { return at<std::uint32_t>(kCcmAnalog + 0x00u); }
inline volatile std::uint32_t& pll_arm_set() noexcept { return at<std::uint32_t>(kCcmAnalog + 0x04u); }
inline volatile std::uint32_t& pll_arm_clr() noexcept { return at<std::uint32_t>(kCcmAnalog + 0x08u); }
inline constexpr std::uint32_t kPllArmDivSelectMask = 0x7Fu;
inline constexpr std::uint32_t kPllPowerDown = 1u << 12;
inline constexpr std::uint32_t kPllEnable = 1u << 13;
inline constexpr std::uint32_t kPllBypassClkSrcMask = 0x3u << 14;  // 0: the 24 MHz oscillator
inline constexpr std::uint32_t kPllBypass = 1u << 16;
inline constexpr std::uint32_t kPllLock = 1u << 31;

// ---- DCDC (RM "DCDC Converter"), 0x4008_0000 --------------------------------------------------------------------------------------
inline constexpr std::uintptr_t kDcdc = 0x40080000u;
inline volatile std::uint32_t& dcdc_reg0() noexcept { return at<std::uint32_t>(kDcdc + 0x00u); }
inline volatile std::uint32_t& dcdc_reg3() noexcept { return at<std::uint32_t>(kDcdc + 0x0Cu); }
inline constexpr std::uint32_t kDcdcReg0StsDcOk = 1u << 31;
inline constexpr std::uint32_t kDcdcReg3TrgMask = 0x1Fu;

// ---- Watchdogs (RM "WDOG1/WDOG2", "RTWDOG") -----------------------------------------------------------------------------------------
// WDOG1 and WDOG2: a 16-bit control register; WDE (bit 2) can only be cleared while the watchdog has never been enabled since reset.
inline volatile std::uint16_t& wdog1_wcr() noexcept { return at<std::uint16_t>(0x400B8000u); }
inline volatile std::uint16_t& wdog2_wcr() noexcept { return at<std::uint16_t>(0x400D0000u); }
inline constexpr std::uint16_t kWdogWcrWde = 1u << 2;
// RTWDOG: unlock through CNT (the 32-bit key when CS[CMD32EN] is set, else the two 16-bit halves), then CS with EN clear and UPDATE set.
inline constexpr std::uintptr_t kRtwdog = 0x400BC000u;
inline volatile std::uint32_t& rtwdog_cs() noexcept { return at<std::uint32_t>(kRtwdog + 0x0u); }
inline volatile std::uint32_t& rtwdog_cnt() noexcept { return at<std::uint32_t>(kRtwdog + 0x4u); }
inline volatile std::uint32_t& rtwdog_toval() noexcept { return at<std::uint32_t>(kRtwdog + 0x8u); }
inline constexpr std::uint32_t kRtwdogUnlock = 0xD928C520u;
inline constexpr std::uint32_t kRtwdogUnlockLow = 0xC520u;
inline constexpr std::uint32_t kRtwdogUnlockHigh = 0xD928u;
inline constexpr std::uint32_t kRtwdogCsUpdate = 1u << 5;
inline constexpr std::uint32_t kRtwdogCsEn = 1u << 7;
inline constexpr std::uint32_t kRtwdogCsCmd32En = 1u << 13;

// ---- SRC (RM "System Reset Controller"), 0x400F_8000 ------------------------------------------------------------------------------
inline volatile std::uint32_t& src_srsr() noexcept { return at<std::uint32_t>(0x400F8008u); }  // write 1 to clear a bit

// ---- Cortex-M7 System Control Block (ARMv7-M ARM B3.2) ----------------------------------------------------------------------------
inline volatile std::uint32_t& scb_icsr() noexcept { return at<std::uint32_t>(0xE000ED04u); }
inline volatile std::uint32_t& scb_vtor() noexcept { return at<std::uint32_t>(0xE000ED08u); }
inline volatile std::uint32_t& scb_aircr() noexcept { return at<std::uint32_t>(0xE000ED0Cu); }
inline volatile std::uint32_t& scb_ccr() noexcept { return at<std::uint32_t>(0xE000ED14u); }
inline volatile std::uint8_t& scb_shp(unsigned exception) noexcept { return at<std::uint8_t>(0xE000ED18u + (exception - 4u)); }  // SHPR1..3, exception 4..15
inline volatile std::uint32_t& scb_shcsr() noexcept { return at<std::uint32_t>(0xE000ED24u); }
inline volatile std::uint32_t& scb_cfsr() noexcept { return at<std::uint32_t>(0xE000ED28u); }
inline volatile std::uint32_t& scb_hfsr() noexcept { return at<std::uint32_t>(0xE000ED2Cu); }
inline volatile std::uint32_t& scb_mmfar() noexcept { return at<std::uint32_t>(0xE000ED34u); }
inline volatile std::uint32_t& scb_bfar() noexcept { return at<std::uint32_t>(0xE000ED38u); }
inline volatile std::uint32_t& scb_cpacr() noexcept { return at<std::uint32_t>(0xE000ED88u); }
inline volatile std::uint32_t& scb_ccsidr() noexcept { return at<std::uint32_t>(0xE000ED80u); }
inline volatile std::uint32_t& scb_csselr() noexcept { return at<std::uint32_t>(0xE000ED84u); }
inline volatile std::uint32_t& scb_iciallu() noexcept { return at<std::uint32_t>(0xE000EF50u); }
inline volatile std::uint32_t& scb_dcisw() noexcept { return at<std::uint32_t>(0xE000EF60u); }
inline volatile std::uint32_t& scb_dccisw() noexcept { return at<std::uint32_t>(0xE000EF74u); }
inline constexpr std::uint32_t kIcsrPendstset = 1u << 26;
inline constexpr std::uint32_t kAircrVectkey = 0x05FA0000u;
inline constexpr std::uint32_t kAircrSysresetreq = 1u << 2;
inline constexpr std::uint32_t kCcrDiv0Trp = 1u << 4;
inline constexpr std::uint32_t kCcrDc = 1u << 16;
inline constexpr std::uint32_t kCcrIc = 1u << 17;
inline constexpr std::uint32_t kShcsrFaultsEnable = (1u << 16) | (1u << 17) | (1u << 18);  // MEMFAULTENA, BUSFAULTENA, USGFAULTENA
inline constexpr std::uint32_t kCpacrFullFpu = (0x3u << 20) | (0x3u << 22);  // CP10, CP11 full access

// ---- MPU (ARMv7-M ARM B3.5) --------------------------------------------------------------------------------------------------------
inline volatile std::uint32_t& mpu_type() noexcept { return at<std::uint32_t>(0xE000ED90u); }
inline volatile std::uint32_t& mpu_ctrl() noexcept { return at<std::uint32_t>(0xE000ED94u); }
inline volatile std::uint32_t& mpu_rnr() noexcept { return at<std::uint32_t>(0xE000ED98u); }
inline volatile std::uint32_t& mpu_rbar() noexcept { return at<std::uint32_t>(0xE000ED9Cu); }
inline volatile std::uint32_t& mpu_rasr() noexcept { return at<std::uint32_t>(0xE000EDA0u); }
inline constexpr std::uint32_t kMpuTypeDregionShift = 8;

// ---- NVIC (ARMv7-M ARM B3.4) -------------------------------------------------------------------------------------------------------
inline volatile std::uint32_t& nvic_iser(unsigned word) noexcept { return at<std::uint32_t>(0xE000E100u + 4u * word); }
inline volatile std::uint32_t& nvic_icer(unsigned word) noexcept { return at<std::uint32_t>(0xE000E180u + 4u * word); }
inline volatile std::uint32_t& nvic_ispr(unsigned word) noexcept { return at<std::uint32_t>(0xE000E200u + 4u * word); }
inline volatile std::uint8_t& nvic_ipr(unsigned line) noexcept { return at<std::uint8_t>(0xE000E400u + line); }

inline volatile std::uint32_t& nvic_icpr(unsigned word) noexcept { return at<std::uint32_t>(0xE000E280u + 4u * word); }
inline volatile std::uint32_t& nvic_stir() noexcept { return at<std::uint32_t>(0xE000EF00u); }  // the software trigger: the line number
constexpr std::uint32_t irq_bit(unsigned line) noexcept { return 1u << (line % 32u); }

// ---- eDMA (RM "eDMA", 0x400E_8000) and DMAMUX (0x400E_C000): the audio channels' registers; the TCDs are audio_config.h's struct -------------
inline constexpr std::uintptr_t kDma = 0x400E8000u;
inline volatile std::uint32_t& dma_cr() noexcept { return at<std::uint32_t>(kDma + 0x00u); }
inline volatile std::uint32_t& dma_es() noexcept { return at<std::uint32_t>(kDma + 0x04u); }
inline volatile std::uint8_t& dma_ceei() noexcept { return at<std::uint8_t>(kDma + 0x18u); }
inline volatile std::uint8_t& dma_cerq() noexcept { return at<std::uint8_t>(kDma + 0x1Au); }
inline volatile std::uint8_t& dma_serq() noexcept { return at<std::uint8_t>(kDma + 0x1Bu); }
inline volatile std::uint8_t& dma_cdne() noexcept { return at<std::uint8_t>(kDma + 0x1Cu); }
inline volatile std::uint8_t& dma_cerr() noexcept { return at<std::uint8_t>(kDma + 0x1Eu); }
inline volatile std::uint8_t& dma_cint() noexcept { return at<std::uint8_t>(kDma + 0x1Fu); }
inline volatile std::uint32_t& dma_err() noexcept { return at<std::uint32_t>(kDma + 0x2Cu); }
inline volatile std::uint8_t& dma_dchpri(unsigned channel) noexcept { return at<std::uint8_t>(kDma + 0x100u + (channel ^ 3u)); }  // DCHPRIn: byte (3 - n % 4) of word n / 4
inline constexpr std::uint8_t kDmaClearAll = 0x40;  // the CAxx bit of the one-byte clear registers
template <typename Tcd>
inline volatile Tcd& dma_tcd_as(unsigned channel) noexcept { return *reinterpret_cast<volatile Tcd*>(kDma + 0x1000u + 32u * channel); }
inline volatile std::uint32_t& dmamux_chcfg(unsigned channel) noexcept { return at<std::uint32_t>(0x400EC000u + 4u * channel); }

// ---- SysTick (ARMv7-M ARM B3.3) ----------------------------------------------------------------------------------------------------
inline volatile std::uint32_t& syst_csr() noexcept { return at<std::uint32_t>(0xE000E010u); }
inline volatile std::uint32_t& syst_rvr() noexcept { return at<std::uint32_t>(0xE000E014u); }
inline volatile std::uint32_t& syst_cvr() noexcept { return at<std::uint32_t>(0xE000E018u); }
inline constexpr std::uint32_t kSystCsrEnable = 1u << 0;
inline constexpr std::uint32_t kSystCsrTickint = 1u << 1;
inline constexpr std::uint32_t kSystCsrClksource = 1u << 2;

// ---- DWT and the debug enable (ARMv7-M ARM C1.8, C1.6); the Cortex-M7's DWT is locked until LAR takes the key -----------------------
inline volatile std::uint32_t& demcr() noexcept { return at<std::uint32_t>(0xE000EDFCu); }
inline volatile std::uint32_t& dwt_ctrl() noexcept { return at<std::uint32_t>(0xE0001000u); }
inline volatile std::uint32_t& dwt_cyccnt() noexcept { return at<std::uint32_t>(0xE0001004u); }
inline volatile std::uint32_t& dwt_lar() noexcept { return at<std::uint32_t>(0xE0001FB0u); }
inline constexpr std::uint32_t kDemcrTrcena = 1u << 24;
inline constexpr std::uint32_t kDwtCyccntena = 1u << 0;
inline constexpr std::uint32_t kDwtLarKey = 0xC5ACCE55u;

}  // namespace o3os::platform::rt10xx::regs
