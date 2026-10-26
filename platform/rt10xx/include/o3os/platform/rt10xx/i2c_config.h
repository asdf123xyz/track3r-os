// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The codec's control bus on the rt10xx (OS-PORT-RT10XX phase 3): LPI2C1 as a polled master at about 375 kHz, with the register facts and
// the timing words from the i.MX RT1050 Reference Manual ("Low Power Inter-Integrated Circuit (LPI2C)"). The pads are audio_pins.h's
// assumption. The bus runs from the 24 MHz oscillator (CCM_CSCDR2 LPI2C_CLK_SEL = 1, podf /1), prescaler 1: the timing fields are in
// 24 MHz cycles. SCL's period is CLKLO + CLKHI + 2 + the pad's rise time: 32 + 24 + 2 = 58 cycles = 2.42 us (413 kHz before the rise
// time, about 375 kHz with it), under the SGTL5000's 400 kHz.

#include <cstdint>

namespace o3os::platform::rt10xx::i2c_cfg {

inline constexpr std::uintptr_t kLpi2c1 = 0x403F0000u;
inline constexpr std::uintptr_t kMcr = kLpi2c1 + 0x10u;
inline constexpr std::uintptr_t kMsr = kLpi2c1 + 0x14u;
inline constexpr std::uintptr_t kMier = kLpi2c1 + 0x18u;
inline constexpr std::uintptr_t kMcfgr0 = kLpi2c1 + 0x20u;
inline constexpr std::uintptr_t kMcfgr1 = kLpi2c1 + 0x24u;
inline constexpr std::uintptr_t kMcfgr2 = kLpi2c1 + 0x28u;
inline constexpr std::uintptr_t kMcfgr3 = kLpi2c1 + 0x2Cu;
inline constexpr std::uintptr_t kMccr0 = kLpi2c1 + 0x48u;
inline constexpr std::uintptr_t kMfcr = kLpi2c1 + 0x58u;
inline constexpr std::uintptr_t kMfsr = kLpi2c1 + 0x5Cu;
inline constexpr std::uintptr_t kMtdr = kLpi2c1 + 0x60u;
inline constexpr std::uintptr_t kMrdr = kLpi2c1 + 0x70u;
// MCR
inline constexpr std::uint32_t kMcrEnable = 1u << 0;
inline constexpr std::uint32_t kMcrReset = 1u << 1;
inline constexpr std::uint32_t kMcrResetTxFifo = 1u << 8;
inline constexpr std::uint32_t kMcrResetRxFifo = 1u << 9;
// MSR (write 1 to clear the flags from EPF up)
inline constexpr std::uint32_t kMsrTxReady = 1u << 0;
inline constexpr std::uint32_t kMsrRxReady = 1u << 1;
inline constexpr std::uint32_t kMsrEndPacket = 1u << 8;
inline constexpr std::uint32_t kMsrStopDetect = 1u << 9;
inline constexpr std::uint32_t kMsrNackDetect = 1u << 10;
inline constexpr std::uint32_t kMsrArbitrationLost = 1u << 11;
inline constexpr std::uint32_t kMsrFifoError = 1u << 12;
inline constexpr std::uint32_t kMsrPinLowTimeout = 1u << 13;
inline constexpr std::uint32_t kMsrMasterBusy = 1u << 24;
inline constexpr std::uint32_t kMsrBusBusy = 1u << 25;
inline constexpr std::uint32_t kMsrErrors = kMsrNackDetect | kMsrArbitrationLost | kMsrFifoError | kMsrPinLowTimeout;
inline constexpr std::uint32_t kMsrClearAll = kMsrEndPacket | kMsrStopDetect | kMsrErrors;
// MTDR commands (bits 10:8)
inline constexpr std::uint32_t kCmdTransmit = 0u << 8;
inline constexpr std::uint32_t kCmdReceive = 1u << 8;  // the low byte: bytes to receive minus one
inline constexpr std::uint32_t kCmdStop = 2u << 8;
inline constexpr std::uint32_t kCmdStart = 4u << 8;    // the low byte: the address with the R/W bit
// MRDR
inline constexpr std::uint32_t kMrdrEmpty = 1u << 14;
// MCFGR1: PRESCALE (bits 2:0) = 0 (divide by 1), PINCFG (bits 26:24) = 0 (two-pin open drain)
inline constexpr std::uint32_t kMcfgr1Word = 0u;
// MCFGR2: BUSIDLE (bits 11:0) = 0 (off), FILTSCL (bits 19:16) and FILTSDA (bits 27:24) = 2 cycles of glitch filter
inline constexpr std::uint32_t kMcfgr2Word = (2u << 16) | (2u << 24);
// MCCR0: CLKLO (bits 5:0), CLKHI (bits 13:8), SETHOLD (bits 21:16), DATAVD (bits 29:24), in prescaled cycles minus one.
inline constexpr std::uint32_t kClkLo = 32;
inline constexpr std::uint32_t kClkHi = 24;
inline constexpr std::uint32_t kSetHold = 15;  // 0.65 us: above the 0.6 us setup/hold of fast mode
inline constexpr std::uint32_t kDataValid = 7;
inline constexpr std::uint32_t kMccr0Word = ((kClkLo - 1u) << 0) | ((kClkHi - 1u) << 8) | ((kSetHold - 1u) << 16) | ((kDataValid - 1u) << 24);
inline constexpr std::uint32_t kSourceHz = 24'000'000u;
constexpr std::uint32_t scl_hz_before_rise() noexcept { return kSourceHz / (kClkLo + kClkHi + 2u); }
static_assert(scl_hz_before_rise() <= 420'000u && scl_hz_before_rise() >= 350'000u, "about 400 kHz before the rise time");
static_assert(kClkLo <= 64u && kClkHi <= 64u && kSetHold <= 64u && kDataValid <= 64u, "6-bit fields");
// MFCR: the watermarks: 0 (TXWATER: request when empty; RXWATER: a flag at one word)
inline constexpr std::uint32_t kMfcrWord = 0u;
// CCM_CSCDR2: LPI2C_CLK_SEL (bit 18) = 1: the oscillator; LPI2C_CLK_PODF (bits 24:19) = 0: divide by 1
inline constexpr std::uintptr_t kCscdr2 = 0x400FC038u;
inline constexpr std::uint32_t kCscdr2Lpi2cClkSel = 1u << 18;
inline constexpr std::uint32_t kCscdr2Lpi2cPodfMask = 0x3Fu << 19;
// Bounded waits, in cycles of the core clock (polls of the status register).
inline constexpr std::uint32_t kByteTimeoutPolls = 200'000;   // a byte is about 25 us; this is milliseconds of polling
inline constexpr std::uint32_t kStopTimeoutPolls = 200'000;

}  // namespace o3os::platform::rt10xx::i2c_cfg
