// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The register values of the DIN MIDI port and its emitter timer on the rt10xx (OS-PORT-RT10XX phase 3), with their arithmetic, so the
// compiler and the host tests check them and the driver (src/midi_din.cpp) only writes them: the format of platform/mk66 midi_config.h.
// The logic of the two interrupts is midi/din_io.h's, written against a register policy; what differs here is the chip: LPUART5 (the
// jack's UART on the Tracker+, an observed fact: Modular-Trackular FINDINGS section 12) instead of UART4, the PIT at the PERCLK root.
//
// Sources: the i.MX RT1050 Reference Manual, chapters "LPUART" (the register map and the fields), "PIT", "CCM" (CSCDR1 and the gates),
// "IOMUXC" (midi_pins.h). The baud rate: LPUART5 is clocked from the 24 MHz oscillator (CSCDR1 UART_CLK_SEL = 1, PODF /1), independent of
// PLL3's state at the hand-over from the bootloader; baud = 24,000,000 / (OSR x SBR) with OSR 16 and SBR 48 = 31,250 exactly.
// The timer: PIT channel 0 counts down at PERCLK (75 MHz, clock_config.h), so one tick is 8 CPU cycles (a shift of 3); a one-shot,
// re-armed by its interrupt (midi/din_io.h). The tick limits of din_io.h (kMinTicks 90, kNotReadyTicks 180, kNoTimeTicks 90,000) are
// written for the MK66's 90 MHz tick; here they are 1.2 us, 2.4 us and 1.2 ms: the same order, inside what the comments there allow.

#include <o3os/core/audio_clock.h>
#include <o3os/hal/audio.h>
#include <o3os/midi/din.h>
#include <o3os/midi/din_io.h>
#include <o3os/platform/rt10xx/clock_config.h>

#include <cstdint>

namespace o3os::platform::rt10xx::midi_cfg {

// The MIDI timing core's cycle counts (midi/din.h) are relative to the tree's clock; the tick arithmetic below is written from this chip's
// clock so the host checks it, and tied to the core's when the tree is ours (clock_config.h kTreeIsRt10xx).

// ---- LPUART5 (RM "LPUART", base 0x4019_4000) ----------------------------------------------------------------------------------------
namespace lpuart {
inline constexpr std::uintptr_t kBase = 0x40194000u;
inline constexpr std::uintptr_t kGlobal = kBase + 0x08u, kPincfg = kBase + 0x0Cu, kBaud = kBase + 0x10u, kStat = kBase + 0x14u,
                                kCtrl = kBase + 0x18u, kData = kBase + 0x1Cu, kModir = kBase + 0x24u, kFifo = kBase + 0x28u, kWater = kBase + 0x2Cu;
// CCM_CCGR3 CG1 (bits 3:2): lpuart5; CCM_CSCDR1: UART_CLK_SEL (bit 6) = 1 the oscillator, UART_CLK_PODF (bits 5:0) = 0 divide by 1.
inline constexpr std::uintptr_t kCcgr3 = 0x400FC074u;
inline constexpr std::uint32_t kCcgr3Lpuart5 = 0x3u << 2;
inline constexpr std::uintptr_t kCscdr1 = 0x400FC024u;
inline constexpr std::uint32_t kCscdr1UartClkSel = 1u << 6;
inline constexpr std::uint32_t kCscdr1UartPodfMask = 0x3Fu;
inline constexpr std::uint32_t kSourceHz = clock::kCrystalHz;
// GLOBAL: RST (bit 1), a software reset of the module.
inline constexpr std::uint32_t kGlobalReset = 1u << 1;
// BAUD: SBR (bits 12:0), SBNS (bit 13: one stop bit = 0), BOTHEDGE (bit 17: needed only for OSR < 8), OSR - 1 (bits 28:24).
inline constexpr std::uint32_t kOsr = 16;
inline constexpr std::uint32_t kSbr = kSourceHz / (kOsr * midi::din::kBaud);
inline constexpr std::uint32_t kBaudWord = ((kOsr - 1u) << 24) | kSbr;
static_assert(kSourceHz % (kOsr * midi::din::kBaud) == 0u, "the divisor is exact: no baud-rate error against a MIDI device");
static_assert(kSbr == 48u, "31,250 baud from the 24 MHz oscillator: OSR 16, SBR 48");
static_assert(kSbr >= 1u && kSbr < 0x2000u, "SBR is a 13-bit field and must not be zero");
static_assert(kOsr >= 4u && kOsr <= 32u, "OSR is a 5-bit field, 4..32");
constexpr std::uint32_t actual_baud(std::uint32_t source_hz, std::uint32_t osr, std::uint32_t sbr) noexcept { return source_hz / (osr * sbr); }
// CTRL: TE (bit 19), RE (bit 18), RIE (bit 21). TIE and TCIE stay off: the transmit side is the PIT's. PE, M, LOOPS and the rest zero: 8N1.
inline constexpr std::uint32_t kCtrlTe = 1u << 19;
inline constexpr std::uint32_t kCtrlRe = 1u << 18;
inline constexpr std::uint32_t kCtrlRie = 1u << 21;
inline constexpr std::uint32_t kCtrlRun = kCtrlTe | kCtrlRe | kCtrlRie;
// STAT: TDRE (bit 23), TC (22), RDRF (21), IDLE (20), OR (19), NF (18), FE (17), PF (16). OR, NF, FE and PF are write-1-to-clear: unlike the
// MK66's UART, reading the data register does not clear them, so the policy's data() clears them after the read. The bits 31:24 above TDRE
// (LBKDIF, RXEDGIF, MSBF, RXINV, RWUID, BRK13, LBKDE) and IDLE are left alone (a write of the four error bits only).
inline constexpr std::uint32_t kStatTdre = 1u << 23;
inline constexpr std::uint32_t kStatTc = 1u << 22;
inline constexpr std::uint32_t kStatRdrf = 1u << 21;
inline constexpr std::uint32_t kStatIdle = 1u << 20;
inline constexpr std::uint32_t kStatOverrun = 1u << 19;
inline constexpr std::uint32_t kStatNoise = 1u << 18;
inline constexpr std::uint32_t kStatFraming = 1u << 17;
inline constexpr std::uint32_t kStatParity = 1u << 16;
inline constexpr std::uint32_t kStatErrorClear = kStatOverrun | kStatNoise | kStatFraming | kStatParity;
// The policy of midi/din_io.h reads a UART S1 byte (the MK66's bit layout, midi::uart_s1): STAT's bits 23..16 are that byte in the same order.
constexpr std::uint8_t s1_of_stat(std::uint32_t stat) noexcept { return static_cast<std::uint8_t>((stat >> 16) & 0xFFu); }
static_assert(s1_of_stat(kStatTdre) == midi::uart_s1::kTdre && s1_of_stat(kStatTc) == midi::uart_s1::kTc && s1_of_stat(kStatRdrf) == midi::uart_s1::kRdrf &&
                  s1_of_stat(kStatIdle) == midi::uart_s1::kIdle && s1_of_stat(kStatOverrun) == midi::uart_s1::kOverrun &&
                  s1_of_stat(kStatNoise) == midi::uart_s1::kNoise && s1_of_stat(kStatFraming) == midi::uart_s1::kFraming &&
                  s1_of_stat(kStatParity) == midi::uart_s1::kParity,
              "STAT[23:16] is the S1 byte of the MK66's UART, bit for bit");
// FIFO: RXFE (bit 3) and TXFE (bit 7) zero: the FIFOs off (midi/din_io.h: the data register in front of the shifter is the whole transmit
// buffer); RXFLUSH (bit 14) and TXFLUSH (bit 15) written once at start.
inline constexpr std::uint32_t kFifoFlush = (1u << 14) | (1u << 15);
inline constexpr std::uint32_t kFifoWord = 0u;
inline constexpr std::uint32_t kDataMask = 0xFFu;
}  // namespace lpuart

// ---- PIT (RM "PIT", base 0x4008_4000) -----------------------------------------------------------------------------------------------
namespace pit {
inline constexpr std::uintptr_t kBase = 0x40084000u;
inline constexpr std::uintptr_t kMcr = kBase + 0x000u;
constexpr std::uintptr_t ldval(unsigned channel) noexcept { return kBase + 0x100u + 0x10u * channel; }
constexpr std::uintptr_t tctrl(unsigned channel) noexcept { return kBase + 0x108u + 0x10u * channel; }
constexpr std::uintptr_t tflg(unsigned channel) noexcept { return kBase + 0x10Cu + 0x10u * channel; }
inline constexpr std::uint32_t kMcrEnable = 0;  // MDIS = 0: the module's clock runs; FRZ = 0: the timers keep counting under a debugger
inline constexpr std::uint32_t kTctrlTen = 1u << 0;
inline constexpr std::uint32_t kTctrlTie = 1u << 1;
inline constexpr std::uint32_t kTflgTif = 1u << 0;  // write 1 to clear
// CCM_CCGR1 CG6 (bits 13:12): pit.
inline constexpr std::uintptr_t kCcgr1 = 0x400FC06Cu;
inline constexpr std::uint32_t kCcgr1Pit = 0x3u << 12;
inline constexpr unsigned kChannel = 0;  // resources.h: the MIDI-out emitter's

// CPU cycles per timer tick, as a shift (core clock / PERCLK = 8): the interrupt shifts, it never divides.
inline constexpr unsigned kCyclesPerTickShift = 3;
static_assert((clock::kCoreHz >> kCyclesPerTickShift) == clock::kPerclkHz && (clock::kCoreHz & ((1u << kCyclesPerTickShift) - 1u)) == 0u,
              "one PIT tick is 8 CPU cycles: the timer runs at PERCLK");
// One MIDI byte in timer ticks: 24,000 (320 us). The emitter's commit-depth-1 wake after a byte.
inline constexpr std::uint64_t kByteCyclesAtCore = std::uint64_t{clock::kCoreHz} * midi::din::kBitsPerByte / midi::din::kBaud;
inline constexpr std::uint32_t kByteTicks = static_cast<std::uint32_t>(kByteCyclesAtCore >> kCyclesPerTickShift);
static_assert(kByteTicks == 24000u, "a DIN byte is 320 us = 24,000 ticks of the 75 MHz PERCLK");
static_assert(!clock::kTreeIsRt10xx || kByteCyclesAtCore == midi::din::kByteCycles, "the timing core's byte is this chip's byte");
static_assert(midi::DinTx::kMaxTicks < 0xFFFFFFFFu, "LDVAL holds the ceiling");
}  // namespace pit

// ---- The audio timeline's lead (platform/mk66 midi_config.h explains it) ---------------------------------------------------------
inline constexpr std::uint32_t kAudioLeadCycles = (hal::audio::kTxSlots - 1u) * hal::audio::kCyclesPerBlock;

}  // namespace o3os::platform::rt10xx::midi_cfg
