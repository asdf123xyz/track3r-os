// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The display's link on the rt10xx (OS-PORT-RT10XX phase 4): the LPSPI that carries the FT81x's SPI, its clock root and divider words, the
// pads, as constants whose arithmetic the compiler checks and platform/rt10xx/tests/rt10xx_display_config_test.cpp pins.
//
// Evidence. That the display is an EVE controller (FT81x / BT81x family) on an LPSPI is an observed fact of the Tracker+ (Modular-Trackular
// FINDINGS section 4); WHICH LPSPI and which pads is not known. The assumption is LPSPI4 on GPIO_B0_01 (SDI), B0_02 (SDO), B0_03 (SCK),
// ALT3, with chip select on GPIO_B0_00 driven as a GPIO (GPIO2_IO00), the RT1052's main SPI pad group in the vendor's and the hobbyist
// ecosystem's boards. The PD# (power-down) line is not driven: no pad is known, and the FT812 driver's soft reset (InitConfig::soft_reset,
// the host command RST_PULSE) covers a PD# held high by the board. If `display start` finds no chip, this header is the one to change.
//
// Sources: the i.MX RT1050 Reference Manual, chapters "LPSPI", "CCM" (CBCMR LPSPI_CLK_SEL / LPSPI_PODF, CCGR1) and "IOMUXC".
// The rates: the FT81x takes at most 11 MHz until its clock is up and at most 30 MHz after; the project's spec asks for 20 MHz at most
// (platform/mk66 display/dspi_config.h, the same limits): init 1.1 MHz, data 18.86 MHz here.

#include <o3os/platform/rt10xx/audio_pins.h>

#include <cstdint>

namespace o3os::platform::rt10xx::display_cfg {

// ---- The clock root: PLL2 (528 MHz) / (LPSPI_PODF + 1) = 132 MHz ------------------------------------------------------------------
inline constexpr std::uintptr_t kCbcmr = 0x400FC018u;
inline constexpr std::uint32_t kCbcmrLpspiSelShift = 4;   // bits 5:4: 2 = PLL2
inline constexpr std::uint32_t kCbcmrLpspiPodfShift = 26; // bits 28:26
inline constexpr std::uint32_t kCbcmrLpspiMask = (0x3u << kCbcmrLpspiSelShift) | (0x7u << kCbcmrLpspiPodfShift);
inline constexpr std::uint32_t kLpspiSel = 2;
inline constexpr std::uint32_t kLpspiPodf = 3;
inline constexpr std::uint32_t kCbcmrLpspiValue = (kLpspiSel << kCbcmrLpspiSelShift) | (kLpspiPodf << kCbcmrLpspiPodfShift);
inline constexpr std::uint32_t kPll2Hz = 528'000'000u;
inline constexpr std::uint32_t kRootHz = kPll2Hz / (kLpspiPodf + 1u);
static_assert(kRootHz == 132'000'000u, "the LPSPI root at 132 MHz");
inline constexpr std::uintptr_t kPllSys = 0x400D8030u;  // the root is valid only while PLL2 is locked and not bypassed (the driver checks)
inline constexpr std::uintptr_t kCcgr1 = 0x400FC06Cu;
inline constexpr std::uint32_t kCcgr1Lpspi4 = 0x3u << 6;  // CG3
inline constexpr std::uintptr_t kCcgr0 = 0x400FC068u;
inline constexpr std::uint32_t kCcgr0Gpio2 = 0x3u << 30;  // CG15

// ---- LPSPI4 (0x403A_0000) ------------------------------------------------------------------------------------------------------------
inline constexpr std::uintptr_t kBase = 0x403A0000u;
inline constexpr std::uintptr_t kCr = kBase + 0x10u, kSr = kBase + 0x14u, kIer = kBase + 0x18u, kDer = kBase + 0x1Cu, kCfgr0 = kBase + 0x20u,
                                kCfgr1 = kBase + 0x24u, kCcr = kBase + 0x40u, kFcr = kBase + 0x58u, kFsr = kBase + 0x5Cu, kTcr = kBase + 0x60u,
                                kTdr = kBase + 0x64u, kRsr = kBase + 0x70u, kRdr = kBase + 0x74u;
inline constexpr std::uint32_t kCrMen = 1u << 0;
inline constexpr std::uint32_t kCrRst = 1u << 1;
inline constexpr std::uint32_t kCrRtf = 1u << 8;
inline constexpr std::uint32_t kCrRrf = 1u << 9;
inline constexpr std::uint32_t kSrClearAll = 0x3F00u;   // WCF FCF TCF TEF REF DMF: write one to clear
inline constexpr std::uint32_t kCfgr1Master = 1u << 0;  // master; PINCFG 0 (SIN in, SOUT out); the PCS pins are not used (chip select is a GPIO)
inline constexpr std::uint32_t kRsrRxEmpty = 1u << 1;
inline constexpr std::uint32_t kFsrTxCountMask = 0x1Fu;
// TCR: FRAMESZ 11:0 = bits - 1, PRESCALE 29:27, CPOL 31 = 0, CPHA 30 = 0 (mode 0, the FT81x's), MSB first.
inline constexpr std::uint32_t kTcrByte = 7u;
// SCK = root / (2^PRESCALE x (SCKDIV + 2)); CCR also holds the delays (DBT, PCSSCK, SCKPCS), irrelevant with a GPIO chip select.
struct Rate {
  std::uint32_t prescale;
  std::uint32_t sckdiv;
};
constexpr std::uint32_t sck_hz(const Rate& r) noexcept { return kRootHz / ((1u << r.prescale) * (r.sckdiv + 2u)); }
inline constexpr Rate kInitRate = {0, 118};  // 132 / 120 = 1.1 MHz: the 1 MHz class of the bring-up
inline constexpr Rate kDataRate = {0, 5};    // 132 / 7 = 18.86 MHz: the largest not above 20 MHz
inline constexpr std::uint32_t kMaxInitHz = 11'000'000u;
inline constexpr std::uint32_t kMaxDataHz = 20'000'000u;
static_assert(sck_hz(kInitRate) <= kMaxInitHz && sck_hz(kInitRate) >= 1'000'000u, "the init rate");
static_assert(sck_hz(kDataRate) <= kMaxDataHz && sck_hz(Rate{0, kDataRate.sckdiv - 1u}) > kMaxDataHz, "the data rate is the largest not above 20 MHz");
static_assert(kInitRate.sckdiv <= 255u && kDataRate.sckdiv <= 255u, "SCKDIV is 8 bits");
constexpr std::uint32_t tcr(const Rate& r) noexcept { return (r.prescale << 27) | kTcrByte; }
constexpr std::uint32_t ccr(const Rate& r) noexcept { return r.sckdiv; }
// A byte at the init rate takes 7.3 us; the bound is in polls of RSR (each a few core cycles): far more than any byte needs.
inline constexpr std::uint32_t kBytePolls = 100'000;

// ---- Pads (assumed) -----------------------------------------------------------------------------------------------------------------
inline constexpr unsigned kB0First = 42 + 32;  // GPIO_B0_00's pad index (audio_pins.h)
inline constexpr audio_pins::Pad kSdi = {"GPIO_B0_01 LPSPI4_SDI", audio_pins::mux_of(kB0First + 1), audio_pins::pad_of(kB0First + 1), 3,
                                         audio_pins::kIomuxc + 0x518u, 0};
inline constexpr audio_pins::Pad kSdo = {"GPIO_B0_02 LPSPI4_SDO", audio_pins::mux_of(kB0First + 2), audio_pins::pad_of(kB0First + 2), 3,
                                         audio_pins::kIomuxc + 0x51Cu, 0};
inline constexpr audio_pins::Pad kSck = {"GPIO_B0_03 LPSPI4_SCK", audio_pins::mux_of(kB0First + 3), audio_pins::pad_of(kB0First + 3), 3,
                                         audio_pins::kIomuxc + 0x520u, 0};
inline constexpr audio_pins::Pad kCs = {"GPIO_B0_00 GPIO2_IO00 (chip select)", audio_pins::mux_of(kB0First + 0), audio_pins::pad_of(kB0First + 0), 5, 0, 0};
inline constexpr std::uintptr_t kGpio2 = 0x401BC000u;  // DR +0, GDIR +4; DR_SET +0x84, DR_CLEAR +0x88
inline constexpr std::uintptr_t kGpio2Gdir = kGpio2 + 0x04u;
inline constexpr std::uintptr_t kGpio2DrSet = kGpio2 + 0x84u;
inline constexpr std::uintptr_t kGpio2DrClear = kGpio2 + 0x88u;
inline constexpr std::uint32_t kCsBit = 1u << 0;
// SW_PAD_CTL: fast slew, DSE 6, speed 2 (the SPI lines); SDI with the keeper.
inline constexpr std::uint32_t kPadWord = (1u << 0) | (6u << 3) | (2u << 6) | (1u << 12);
static_assert(kSdi.mux_ctl == 0x401F8140u && kCs.mux_ctl == 0x401F813Cu, "GPIO_B0_00 and _01's mux registers");

}  // namespace o3os::platform::rt10xx::display_cfg
