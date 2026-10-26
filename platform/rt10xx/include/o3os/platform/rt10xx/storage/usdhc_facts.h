// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the rt10xx's uSDHC1 (the SD host controller) and the Tracker+'s SD slot need from the platform (OS-PORT-RT10XX phase 4): register
// addresses and bit values, the pads, the clock root and divider arithmetic, and the transfer words of each command, as constants whose
// arithmetic the compiler checks and platform/rt10xx/tests/rt10xx_usdhc_test.cpp pins. The code that writes them is the image's
// (src/usdhc_bus.cpp); the protocol on top of it (platform/mk66 storage/sd_card.h, chip-neutral) sees none of this.
//
// SOURCES: the i.MX RT1050 Reference Manual, chapters "Ultra Secured Digital Host Controller (uSDHC)", "CCM", "CCM Analog" and "IOMUXC".
// The uSDHC is a descendant of the K66's SDHC (platform/mk66 storage/sdhc_facts.h): the same register map for the command, response,
// buffer, present-state, protocol, system and interrupt registers, with one difference the driver must follow: the data-transfer bits
// the K66 keeps in XFERTYP (DMA, block count, auto CMD12, direction, multiple blocks) are in MIX_CTRL here, written before CMD_XFR_TYP.
// The bits 3:0 of SYS_CTRL are reserved (no SDCLKEN): the card clock runs when the divider is stable.
//
// Board facts. That the card is in a slot the bootloader reads (`Firmware/`) is an observed fact; WHICH controller and pads is an
// assumption: uSDHC1 on GPIO_SD_B0_00..05 (CMD, CLK, DATA0..3, ALT0), the only 4-bit SD pad group of the RT1052 that is not shared with
// the SDRAM, used by the vendor's boards and the hobbyist ecosystem. No card-detect contact is known: the slot is taken as full and a card
// that does not answer is "no card" (the card logic's NoResponse). No write-protect contact either.

#include <o3os/platform/mk66/storage/sd_protocol.h>
#include <o3os/platform/rt10xx/audio_pins.h>

#include <cstdint>

namespace o3os::platform::rt10xx::storage::usdhc {

namespace sd = o3os::platform::mk66::storage::sd;

// ---- Pads (assumed) -------------------------------------------------------------------------------------------------------------------
inline constexpr unsigned kSdB0First = 42 + 64;  // GPIO_SD_B0_00's index in the IOMUXC's pad order (audio_pins.h)
struct SdPad {
  const char* name;
  std::uintptr_t mux_ctl;
  std::uintptr_t pad_ctl;
};
constexpr SdPad sd_pad(const char* name, unsigned n) noexcept { return SdPad{name, audio_pins::mux_of(kSdB0First + n), audio_pins::pad_of(kSdB0First + n)}; }
inline constexpr SdPad kCmd = sd_pad("GPIO_SD_B0_00 USDHC1_CMD", 0);
inline constexpr SdPad kClk = sd_pad("GPIO_SD_B0_01 USDHC1_CLK", 1);
inline constexpr SdPad kData0 = sd_pad("GPIO_SD_B0_02 USDHC1_DATA0", 2);
inline constexpr SdPad kData1 = sd_pad("GPIO_SD_B0_03 USDHC1_DATA1", 3);
inline constexpr SdPad kData2 = sd_pad("GPIO_SD_B0_04 USDHC1_DATA2", 4);
inline constexpr SdPad kData3 = sd_pad("GPIO_SD_B0_05 USDHC1_DATA3", 5);
inline constexpr std::uint32_t kPadAlt = 0;
// SW_PAD_CTL: the vendor's SD words. CMD and data: hysteresis (bit 16), 47 kOhm pull-up (PUS 1, bits 15:14), PUE, PKE, speed 2, DSE 1,
// fast slew. CLK: the same without the pull.
inline constexpr std::uint32_t kPadCmdData = (1u << 16) | (1u << 14) | (1u << 13) | (1u << 12) | (2u << 6) | (1u << 3) | (1u << 0);
inline constexpr std::uint32_t kPadClk = (1u << 16) | (2u << 6) | (1u << 3) | (1u << 0);
static_assert(kPadCmdData == 0x17089u && kPadClk == 0x10089u, "the vendor's SD pad words");
static_assert(kCmd.mux_ctl == 0x401F81BCu && kCmd.pad_ctl == 0x401F83ACu, "GPIO_SD_B0_00's registers");

// ---- The clock root: PLL2 PFD2 / (USDHC1_PODF + 1) ------------------------------------------------------------------------------------
// The driver selects PFD2 and PODF itself and READS PFD2's fraction to know the root: PLL2 and its PFDs may feed the FlexSPI the code runs
// from, so they are never reprogrammed here. A PLL2 that is not locked, or a PFD2 that is gated, is a root of 0 Hz: power_up() says so.
inline constexpr std::uintptr_t kPllSys = 0x400D8030u;   // CCM_ANALOG_PLL_SYS: LOCK 31, BYPASS 16, ENABLE 13, POWERDOWN 12, DIV_SELECT 0 (1 = x22)
inline constexpr std::uintptr_t kPfd528 = 0x400D8100u;   // CCM_ANALOG_PFD_528: PFD2_CLKGATE 23, PFD2_FRAC 21:16
inline constexpr std::uint32_t kPllLock = 1u << 31;
inline constexpr std::uint32_t kPllBypass = 1u << 16;
inline constexpr std::uint32_t kPllPowerDown = 1u << 12;
inline constexpr std::uint32_t kPfd2Gate = 1u << 23;
inline constexpr std::uint32_t kPfd2FracShift = 16;
inline constexpr std::uint32_t kPll2Hz = 528'000'000u;  // 24 MHz x 22: the PLL's only rate (DIV_SELECT 1)
inline constexpr std::uintptr_t kCscmr1 = 0x400FC01Cu;   // USDHC1_CLK_SEL bit 16: 0 = PLL2 PFD2
inline constexpr std::uint32_t kCscmr1Usdhc1Sel = 1u << 16;
inline constexpr std::uintptr_t kCscdr1 = 0x400FC024u;   // USDHC1_PODF bits 13:11 (USDHC2_PODF is 18:16; the vendor SDK and the Teensy core agree)
inline constexpr std::uint32_t kCscdr1PodfShift = 11;
inline constexpr std::uint32_t kCscdr1PodfMask = 0x7u << kCscdr1PodfShift;
inline constexpr std::uint32_t kPodf = 1;                // divide by 2: 396 MHz -> 198 MHz (the RM's limit for the root is 198 MHz)
inline constexpr std::uintptr_t kCcgr6 = 0x400FC080u;    // usdhc1: CG1, bits 3:2
inline constexpr std::uint32_t kCcgr6Usdhc1 = 0x3u << 2;
// The root from the two analog words as read (0: not running).
constexpr std::uint32_t root_hz(std::uint32_t pll_sys, std::uint32_t pfd528, std::uint32_t podf) noexcept {
  if ((pll_sys & kPllLock) == 0u || (pll_sys & (kPllPowerDown | kPllBypass)) != 0u || (pfd528 & kPfd2Gate) != 0u) {
    return 0;
  }
  const std::uint32_t frac = (pfd528 >> kPfd2FracShift) & 0x3Fu;
  if (frac < 12u || frac > 35u) {
    return 0;  // outside the PFD's range: a word this driver does not understand
  }
  return static_cast<std::uint32_t>(static_cast<std::uint64_t>(kPll2Hz) * 18u / frac / (podf + 1u));
}
static_assert(root_hz(kPllLock | 1u, 24u << kPfd2FracShift, kPodf) == 198'000'000u, "PFD2 at its reset fraction 24 (396 MHz), divided by 2");
static_assert(root_hz(1u, 24u << kPfd2FracShift, kPodf) == 0u && root_hz(kPllLock, kPfd2Gate | (24u << kPfd2FracShift), kPodf) == 0u, "not running: 0");

// ---- Registers (uSDHC1 at 0x402C_0000) ------------------------------------------------------------------------------------------------
inline constexpr std::uintptr_t kBase = 0x402C0000u;
inline constexpr std::uint32_t kOffBlkAtt = 0x04;
inline constexpr std::uint32_t kOffCmdArg = 0x08;
inline constexpr std::uint32_t kOffCmdXfrTyp = 0x0C;
inline constexpr std::uint32_t kOffCmdRsp0 = 0x10;  // CMD_RSP0..3 at +4 each: bits 127:8 of an R2, bits 39:8 of a 48-bit response in RSP0
inline constexpr std::uint32_t kOffDataPort = 0x20;
inline constexpr std::uint32_t kOffPresState = 0x24;
inline constexpr std::uint32_t kOffProtCtrl = 0x28;
inline constexpr std::uint32_t kOffSysCtrl = 0x2C;
inline constexpr std::uint32_t kOffIntStatus = 0x30;
inline constexpr std::uint32_t kOffIntStatusEn = 0x34;
inline constexpr std::uint32_t kOffIntSignalEn = 0x38;
inline constexpr std::uint32_t kOffWtmkLvl = 0x44;
inline constexpr std::uint32_t kOffMixCtrl = 0x48;
inline constexpr std::uint32_t kOffVendSpec = 0xC0;

constexpr std::uint32_t blk_att(std::uint32_t block_size, std::uint32_t block_count) noexcept { return (block_size & 0x1FFFu) | (block_count << 16); }

// CMD_XFR_TYP: CMDINX 29:24, CMDTYP 23:22, DPSEL 21, CICEN 20, CCCEN 19, RSPTYP 17:16.
inline constexpr std::uint32_t kXferRsptypShift = 16;
inline constexpr std::uint32_t kXferCccen = 1u << 19;
inline constexpr std::uint32_t kXferCicen = 1u << 20;
inline constexpr std::uint32_t kXferDpsel = 1u << 21;
inline constexpr std::uint32_t kXferCmdtypShift = 22;
inline constexpr std::uint32_t kXferCmdinxShift = 24;
inline constexpr std::uint32_t kRsptyp136 = 1, kRsptyp48 = 2, kRsptyp48Busy = 3, kCmdtypAbort = 3;
// MIX_CTRL: DMAEN 0, BCEN 1, AC12EN 2, DDR_EN 3, DTDSEL 4 (1 = read), MSBSEL 5; the bits above are the tuning's and are kept.
inline constexpr std::uint32_t kMixBcen = 1u << 1;
inline constexpr std::uint32_t kMixAc12en = 1u << 2;
inline constexpr std::uint32_t kMixDtdsel = 1u << 4;
inline constexpr std::uint32_t kMixMsbsel = 1u << 5;
inline constexpr std::uint32_t kMixMask = 0x3Fu;

// The command word of each command (the table of the MK66's xfertyp(), without the data bits).
constexpr std::uint32_t xfr_typ(sd::Cmd cmd) noexcept {
  std::uint32_t x = static_cast<std::uint32_t>(sd::wire_index(cmd)) << kXferCmdinxShift;
  switch (sd::response_of(cmd)) {
    case sd::Resp::None: break;
    case sd::Resp::R1:
    case sd::Resp::R6:
    case sd::Resp::R7: x |= (kRsptyp48 << kXferRsptypShift) | kXferCccen | kXferCicen; break;
    case sd::Resp::R1b: x |= (kRsptyp48Busy << kXferRsptypShift) | kXferCccen | kXferCicen; break;
    case sd::Resp::R2: x |= (kRsptyp136 << kXferRsptypShift) | kXferCccen; break;
    case sd::Resp::R3: x |= kRsptyp48 << kXferRsptypShift; break;
  }
  if (sd::reads_data(cmd) || sd::writes_data(cmd)) {
    x |= kXferDpsel;
  }
  if (cmd == sd::Cmd::StopTransmission) {
    x |= kCmdtypAbort << kXferCmdtypShift;
  }
  return x;
}
// The data bits of each command, for MIX_CTRL: direction, and for CMD18 the block count, the controller's own CMD12 and multiple blocks.
constexpr std::uint32_t mix_ctrl(sd::Cmd cmd) noexcept {
  std::uint32_t m = 0;
  if (sd::reads_data(cmd)) {
    m |= kMixDtdsel;
    if (cmd == sd::Cmd::ReadMultipleBlock) {
      m |= kMixMsbsel | kMixBcen | kMixAc12en;
    }
  }
  return m;
}

// PRES_STATE.
inline constexpr std::uint32_t kPrsCihb = 1u << 0;
inline constexpr std::uint32_t kPrsCdihb = 1u << 1;
inline constexpr std::uint32_t kPrsSdstb = 1u << 3;
inline constexpr std::uint32_t kPrsBwen = 1u << 10;
inline constexpr std::uint32_t kPrsBren = 1u << 11;
inline constexpr std::uint32_t kPrsDat0 = 1u << 24;
// PROT_CTRL: DTW 2:1, EMODE 5:4 (2 = little endian).
constexpr std::uint32_t prot_bus_width(std::uint32_t prot, unsigned bits) noexcept {
  return (prot & ~((0x3u << 1) | (0x3u << 4))) | ((bits == 4u ? 1u : 0u) << 1) | (2u << 4);
}
// SYS_CTRL: DVS 7:4, SDCLKFS 15:8, DTOCV 19:16, RSTA 24, RSTC 25, RSTD 26, INITA 27 (bits 3:0 reserved: kept as read).
inline constexpr std::uint32_t kSysDvsShift = 4;
inline constexpr std::uint32_t kSysSdclkfsShift = 8;
inline constexpr std::uint32_t kSysDtocvShift = 16;
inline constexpr std::uint32_t kSysClockMask = (0xFu << kSysDvsShift) | (0xFFu << kSysSdclkfsShift) | (0xFu << kSysDtocvShift);
inline constexpr std::uint32_t kSysRsta = 1u << 24;
inline constexpr std::uint32_t kSysRstc = 1u << 25;
inline constexpr std::uint32_t kSysRstd = 1u << 26;
inline constexpr std::uint32_t kSysInita = 1u << 27;
inline constexpr std::uint32_t kDataTimeoutExponent = 0xE;
// INT_STATUS (write one to clear) and INT_STATUS_EN: the K66's positions.
inline constexpr std::uint32_t kIntCc = 1u << 0;
inline constexpr std::uint32_t kIntTc = 1u << 1;
inline constexpr std::uint32_t kIntBwr = 1u << 4;
inline constexpr std::uint32_t kIntBrr = 1u << 5;
inline constexpr std::uint32_t kIntCtoe = 1u << 16;
inline constexpr std::uint32_t kIntCce = 1u << 17;
inline constexpr std::uint32_t kIntCebe = 1u << 18;
inline constexpr std::uint32_t kIntCie = 1u << 19;
inline constexpr std::uint32_t kIntDtoe = 1u << 20;
inline constexpr std::uint32_t kIntDce = 1u << 21;
inline constexpr std::uint32_t kIntDebe = 1u << 22;
inline constexpr std::uint32_t kIntAc12e = 1u << 24;
inline constexpr std::uint32_t kIntCommandErrors = kIntCtoe | kIntCce | kIntCebe | kIntCie;
inline constexpr std::uint32_t kIntDataErrors = kIntDtoe | kIntDce | kIntDebe | kIntAc12e;
inline constexpr std::uint32_t kIntEnabled = kIntCc | kIntTc | kIntBwr | kIntBrr | kIntCommandErrors | kIntDataErrors;
inline constexpr std::uint32_t kIntAll = 0xFFFFFFFFu;
// WTMK_LVL: RD_WML 7:0, WR_WML 23:16 (words); the burst lengths (12:8, 28:24) are the DMA's and stay at their reset value.
inline constexpr std::uint32_t kReadWatermarkWords = 16;
inline constexpr std::uint32_t kWriteWatermarkWords = 16;
inline constexpr std::uint32_t kWtmkMask = 0xFFu | (0xFFu << 16);
constexpr std::uint32_t wtmk(std::uint32_t was) noexcept { return (was & ~kWtmkMask) | kReadWatermarkWords | (kWriteWatermarkWords << 16); }
static_assert(sd::kBlockSize / 4u % kReadWatermarkWords == 0u && sd::kBlockSize / 4u % kWriteWatermarkWords == 0u, "whole watermark groups a block");

// ---- The divider: SDCLK = root / (prescaler x divisor), SDCLKFS = prescaler / 2 (0 for 1), DVS = divisor - 1 (the K66's arithmetic) -----
struct ClockPlan {
  bool ok;
  std::uint8_t sdclkfs;
  std::uint8_t dvs;
  std::uint32_t hz;
};
constexpr ClockPlan plan_clock(std::uint32_t root, std::uint32_t max_hz) noexcept {
  ClockPlan best{false, 0, 0, 0};
  if (root == 0u || max_hz == 0u) {
    return best;
  }
  std::uint32_t best_total = 0;
  for (std::uint32_t prescaler = 1; prescaler <= 256u; prescaler <<= 1) {
    for (std::uint32_t divisor = 1; divisor <= 16u; ++divisor) {
      const std::uint32_t total = prescaler * divisor;
      if (root / total <= max_hz && (!best.ok || total < best_total)) {
        best = ClockPlan{true, static_cast<std::uint8_t>(prescaler >> 1), static_cast<std::uint8_t>(divisor - 1u), root / total};
        best_total = total;
      }
    }
  }
  return best;
}
constexpr std::uint32_t sys_clock_fields(const ClockPlan& p) noexcept {
  return (static_cast<std::uint32_t>(p.dvs) << kSysDvsShift) | (static_cast<std::uint32_t>(p.sdclkfs) << kSysSdclkfsShift) | (kDataTimeoutExponent << kSysDtocvShift);
}
inline constexpr std::uint32_t kIdentifyMaxHz = 400'000;
inline constexpr std::uint32_t kRootHz = 198'000'000u;  // the expected root (root_hz at the reset fraction)
static_assert(plan_clock(kRootHz, kIdentifyMaxHz).ok && plan_clock(kRootHz, kIdentifyMaxHz).hz <= kIdentifyMaxHz, "400 kHz is reachable");
static_assert(plan_clock(kRootHz, 50'000'000u).hz == 49'500'000u, "high speed: 198 / 4");
static_assert(plan_clock(kRootHz, 25'000'000u).hz == 24'750'000u, "default speed: 198 / 8");

}  // namespace o3os::platform::rt10xx::storage::usdhc
