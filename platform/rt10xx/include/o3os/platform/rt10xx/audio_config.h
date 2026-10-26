// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The register values of the rt10xx audio path (OS-PORT-RT10XX phase 3), with their arithmetic, so the compiler and the host tests check
// them and the driver (src/audio.cpp) only writes them: the audio PLL (PLL4) and the SAI1 clock root that make MCLK, SAI1's transmitter and
// receiver words, the eDMA channels' transfer control descriptors and the DMAMUX sources. Sources: the i.MX RT1050 Reference Manual,
// chapters "CCM", "CCM Analog", "Synchronous Audio Interface (SAI)", "eDMA", "DMAMUX" and "IOMUXC GPR".
//
// The format is the MK66's (platform/mk66 audio_config.h): a 16-bit stereo I2S link to the SGTL5000 with the chip as the master, MCLK =
// 256 Fs, BCLK = MCLK / 4 = 64 Fs, two 32-bit words a frame with the 16-bit sample in the upper half, the DMA moving 16-bit halves. One
// difference, from the assumed wiring (audio_pins.h: the vendor's and the hobbyist ecosystem's RT1052 + SGTL5000 arrangement): the RECEIVER
// is the clock master and the transmitter is synchronous to it, because the BCLK and SYNC pads of that wiring are the receiver's. The
// other difference: MCLK is not a divider of the core clock but PLL4's output divided in the CCM, so hal::audio::kMclkRatio is unused here
// and hal::audio::kMclkHz (256 x Fs, from the one rate constant) is what the arithmetic below must reproduce exactly.

#include <o3os/core/audio_clock.h>
#include <o3os/hal/audio.h>
#include <o3os/platform/rt10xx/clock_config.h>

#include <cstdint>

namespace o3os::platform::rt10xx::audio_cfg {

// The cycle counts of the audio path (hal/audio.h kCyclesPerBlock) are relative to the tree's clock: clock_config.h kTreeIsRt10xx says it is ours.

// ---- MCLK: PLL4 (the audio PLL) and the SAI1 clock root ----------------------------------------------------------------------------
// PLL4: Fout = 24 MHz x (DIV_SELECT + NUM / DENOM), 650..1300 MHz, then POST_DIV_SELECT; the SAI1 root divides it by (PRED + 1)(PODF + 1).
// 24 x 47.04 = 1,128.96 MHz, / 4 / 25 = 11.2896 MHz = 256 x 44,100 Hz, exactly (the hobbyist ecosystem's numbers for this rate).
namespace pll4 {
inline constexpr std::uintptr_t kReg = 0x400D8070u;  // CCM_ANALOG_PLL_AUDIO (SET +4, CLR +8)
inline constexpr std::uintptr_t kNum = 0x400D8080u;
inline constexpr std::uintptr_t kDenom = 0x400D8090u;
inline constexpr std::uintptr_t kMisc2 = 0x400D8170u;  // AUDIO_DIV_LSB (bit 15), AUDIO_DIV_MSB (bit 23): the post divider's extra /2 and /4, both off
inline constexpr std::uint32_t kDivSelect = 47;
inline constexpr std::uint32_t kNumerator = 4;
inline constexpr std::uint32_t kDenominator = 100;
inline constexpr std::uint32_t kPostDivSelect = 2;  // bits 20:19: 2 = divide by 1
inline constexpr std::uint32_t kPowerDown = 1u << 12;
inline constexpr std::uint32_t kEnable = 1u << 13;
inline constexpr std::uint32_t kBypass = 1u << 16;
inline constexpr std::uint32_t kLock = 1u << 31;
inline constexpr std::uint32_t kWord = kEnable | (kPostDivSelect << 19) | kDivSelect;  // POWERDOWN and BYPASS clear
inline constexpr std::uint32_t kMisc2AudioDivMask = (1u << 15) | (1u << 23);
constexpr std::uint64_t fout_hz() noexcept {
  return (static_cast<std::uint64_t>(clock::kCrystalHz) * (kDivSelect * kDenominator + kNumerator)) / kDenominator;
}
static_assert(kDivSelect >= 27u && kDivSelect <= 54u, "PLL4 DIV_SELECT range (RM)");
static_assert(fout_hz() == 1'128'960'000u, "PLL4 at 1,128.96 MHz");
static_assert(fout_hz() >= 650'000'000u && fout_hz() <= 1'300'000'000u, "inside the PLL's range");
}  // namespace pll4

namespace sai_clock {
inline constexpr std::uint32_t kPred = 3;   // CS1CDR SAI1_CLK_PRED (bits 8:6): divide by 4
inline constexpr std::uint32_t kPodf = 24;  // CS1CDR SAI1_CLK_PODF (bits 5:0): divide by 25
inline constexpr std::uint32_t kSel = 2;    // CSCMR1 SAI1_CLK_SEL (bits 11:10): PLL4
constexpr std::uint32_t root_hz() noexcept { return static_cast<std::uint32_t>(pll4::fout_hz() / ((kPred + 1u) * (kPodf + 1u))); }
inline constexpr std::uint32_t kMclkHz = root_hz();
static_assert(kPred <= 7u && kPodf <= 63u, "field widths");
static_assert(!clock::kTreeIsRt10xx || kMclkHz == hal::audio::kMclkHz, "the SAI1 root is MCLK = 256 x Fs of the rate constant, exactly");
static_assert(kMclkHz == 11'289'600u, "11.2896 MHz");
// IOMUXC_GPR1: SAI1_MCLK_DIR (bit 19) drives MCLK out on its pad; SAI1_MCLK1_SEL (bits 2:0) = 0 is the CCM's SAI1 root.
inline constexpr std::uintptr_t kGpr1 = 0x400AC004u;
inline constexpr std::uint32_t kGpr1Sai1MclkDir = 1u << 19;
}  // namespace sai_clock

// ---- SAI1 (RM "SAI"): base 0x4038_4000; the registers from +0x08 (VERID and PARAM come first on this chip) -------------------------------
namespace sai {
inline constexpr std::uintptr_t kBase = 0x40384000u;
inline constexpr std::uintptr_t kTcsr = kBase + 0x08, kTcr1 = kBase + 0x0C, kTcr2 = kBase + 0x10, kTcr3 = kBase + 0x14, kTcr4 = kBase + 0x18,
                                kTcr5 = kBase + 0x1C, kTdr0 = kBase + 0x20, kTfr0 = kBase + 0x40, kTmr = kBase + 0x60;
inline constexpr std::uintptr_t kRcsr = kBase + 0x88, kRcr1 = kBase + 0x8C, kRcr2 = kBase + 0x90, kRcr3 = kBase + 0x94, kRcr4 = kBase + 0x98,
                                kRcr5 = kBase + 0x9C, kRdr0 = kBase + 0xA0, kRfr0 = kBase + 0xC0, kRmr = kBase + 0xE0;
// TCSR / RCSR bits (the same positions as the MK66's I2S, the same IP).
inline constexpr std::uint32_t kCsrEnable = 1u << 31;
inline constexpr std::uint32_t kCsrBitClockEnable = 1u << 28;
inline constexpr std::uint32_t kCsrFifoReset = 1u << 25;
inline constexpr std::uint32_t kCsrSoftReset = 1u << 24;
inline constexpr std::uint32_t kCsrWordStartFlag = 1u << 20;
inline constexpr std::uint32_t kCsrSyncErrorFlag = 1u << 19;
inline constexpr std::uint32_t kCsrFifoErrorFlag = 1u << 18;
inline constexpr std::uint32_t kCsrFifoRequestDmaEnable = 1u << 0;
inline constexpr std::uint32_t kCsrErrorFlags = kCsrSyncErrorFlag | kCsrFifoErrorFlag;
// The 16 bits of a sample are the upper half of the 32-bit word: the data register's address + 2.
inline constexpr std::uintptr_t kTxDataHalf = kTdr0 + 2u;
inline constexpr std::uintptr_t kRxDataHalf = kRdr0 + 2u;
inline constexpr std::uint32_t kBclkDiv = 1;    // BCLK = MCLK / ((DIV + 1) x 2) = MCLK / 4
inline constexpr std::uint32_t kWordBits = 32;
inline constexpr std::uint32_t kFrameWords = 2;
inline constexpr std::uint32_t kFifoWords = 32;  // this chip's SAI FIFOs (the MK66's are 8)
// The watermarks: the transmit FIFO requests at TFW words left, the receive FIFO at more than RFW words held. 1, as on the MK66 (the
// margin is two word times, 22 us, against the eDMA's sub-microsecond latency); the deeper FIFO here would allow more.
inline constexpr std::uint32_t kTxWatermark = 1;
inline constexpr std::uint32_t kRxWatermark = 1;
// The receiver, the clock master: RCR2 SYNC = 0 (asynchronous: it makes the clocks), MSEL = 1 (MCLK1: the SAI1 root), BCP = 1 (data changes on
// the falling edge), BCD = 1 (BCLK is an output), DIV = 1. RCR3 RCE channel 0. RCR4 FRSZ = 1 (two words), SYWD = 31, MF, FSE (one bit
// early: I2S), FSP (low = left), FSD (the frame sync is an output). RCR5 WNW = W0W = FBT = 31.
inline constexpr std::uint32_t kRcr1Value = kRxWatermark;
inline constexpr std::uint32_t kRcr2Value = (0u << 30) | (1u << 26) | (1u << 25) | (1u << 24) | kBclkDiv;
inline constexpr std::uint32_t kRcr3Value = 1u << 16;
inline constexpr std::uint32_t kRcr4Value = (1u << 16) | (31u << 8) | 0x10u | 0x08u | 0x02u | 0x01u;
inline constexpr std::uint32_t kRcr5Value = (31u << 24) | (31u << 16) | (31u << 8);
// The transmitter, synchronous to the receiver: TCR2 SYNC = 1 (bits 31:30 = 01: synchronous with the receiver), the rest as the receiver's.
inline constexpr std::uint32_t kTcr1Value = kTxWatermark;
inline constexpr std::uint32_t kTcr2Value = (1u << 30) | (1u << 26) | (1u << 25) | (1u << 24) | kBclkDiv;
inline constexpr std::uint32_t kTcr3Value = 1u << 16;
inline constexpr std::uint32_t kTcr4Value = kRcr4Value;
inline constexpr std::uint32_t kTcr5Value = kRcr5Value;
inline constexpr std::uint32_t kBclkDivide = (kBclkDiv + 1u) * 2u;
static_assert(kBclkDivide * kFrameWords * kWordBits == hal::audio::kMclkPerSample, "BCLK = MCLK / 4 and 64 BCLKs per frame make LRCLK = MCLK / 256");
static_assert(((kRcr4Value >> 16) & 0x1Fu) + 1u == kFrameWords && ((kRcr5Value >> 24) & 0x1Fu) + 1u == kWordBits, "two 32-bit words per frame");
static_assert(kTxWatermark < kFifoWords && kRxWatermark < kFifoWords, "the watermarks fit the FIFO");
}  // namespace sai

// ---- eDMA (RM "eDMA", base 0x400E_8000, 32 channels, TCDs at +0x1000) and DMAMUX (0x400E_C000, one 32-bit CHCFG a channel) --------------
namespace dma {
inline constexpr std::uintptr_t kBase = 0x400E8000u;
inline constexpr std::uintptr_t kTcdBase = kBase + 0x1000u;
inline constexpr std::uintptr_t kMuxBase = 0x400EC000u;
inline constexpr unsigned kChannels = 32;
// CR: GRP1PRI (the two groups of 16 channels must differ in priority, as on the MK66), EMLM (minor loop mapping), EDBG (keep running under a
// debugger: never halt with the amplifier open). Written by boot::dma_init() and nothing else.
inline constexpr std::uint32_t kCrValue = 0x400u | 0x80u | 0x02u;
inline constexpr std::uint32_t kCrHalt = 0x20u;
// DMAMUX request sources of this chip: SAI1 receive 19, SAI1 transmit 20. CHCFGn = ENBL | source (32-bit here).
inline constexpr std::uint32_t kSourceSai1Rx = 19;
inline constexpr std::uint32_t kSourceSai1Tx = 20;
inline constexpr std::uint32_t kMuxEnable = 1u << 31;
// The TCD, in register order: one 32-byte descriptor per channel (the MK66's eDMA, the same IP).
struct Tcd {
  std::uint32_t saddr;
  std::int16_t soff;
  std::uint16_t attr;
  std::uint32_t nbytes;
  std::int32_t slast;
  std::uint32_t daddr;
  std::int16_t doff;
  std::uint16_t citer;
  std::int32_t dlastsga;
  std::uint16_t csr;
  std::uint16_t biter;
};
static_assert(sizeof(Tcd) == 32, "a TCD is 32 bytes");
inline constexpr std::uint16_t kSize16 = 1;
inline constexpr std::uint16_t kCsrIntMajor = 0x0002;
constexpr unsigned log2_pow2(std::uint32_t v) noexcept {
  unsigned n = 0;
  while ((1u << n) < v) {
    ++n;
  }
  return n;
}
// TX: a circular read of the TX ring into the SAI's data register, one 16-bit sample per request; the major loop is one slot (one block), so
// the channel interrupts once per slot; SMOD = log2(ring size) wraps the source inside the ring (the ring sits on a multiple of its size).
constexpr Tcd tx_tcd(std::uint32_t ring_base, std::uint32_t ring_bytes) noexcept {
  Tcd t{};
  t.saddr = ring_base;
  t.soff = 2;
  t.attr = static_cast<std::uint16_t>((log2_pow2(ring_bytes) << 11) | (kSize16 << 8) | kSize16);
  t.nbytes = 2;
  t.slast = 0;
  t.daddr = static_cast<std::uint32_t>(sai::kTxDataHalf);
  t.doff = 0;
  t.citer = static_cast<std::uint16_t>(hal::audio::kBlockSamples);
  t.biter = t.citer;
  t.dlastsga = 0;
  t.csr = kCsrIntMajor;
  return t;
}
// RX: the data register into the RX ring, DMOD = log2(ring size). No interrupt: the TX channel's boundary reads its position.
constexpr Tcd rx_tcd(std::uint32_t ring_base, std::uint32_t ring_bytes) noexcept {
  Tcd t{};
  t.saddr = static_cast<std::uint32_t>(sai::kRxDataHalf);
  t.soff = 0;
  t.attr = static_cast<std::uint16_t>((kSize16 << 8) | (log2_pow2(ring_bytes) << 3) | kSize16);
  t.nbytes = 2;
  t.slast = 0;
  t.daddr = ring_base;
  t.doff = 2;
  t.citer = static_cast<std::uint16_t>(hal::audio::kBlockSamples);
  t.biter = t.citer;
  t.dlastsga = 0;
  t.csr = 0;
  return t;
}
}  // namespace dma

// ---- Clock gates (RM "CCM", the CCGR registers) ----------------------------------------------------------------------------------------
namespace gates {
inline constexpr std::uintptr_t kCcgr2 = 0x400FC070u;  // lpi2c1: CG3
inline constexpr std::uintptr_t kCcgr5 = 0x400FC07Cu;  // dma: CG3, sai1: CG9
inline constexpr std::uint32_t kCcgr2Lpi2c1 = 0x3u << 6;
inline constexpr std::uint32_t kCcgr5Dma = 0x3u << 6;
inline constexpr std::uint32_t kCcgr5Sai1 = 0x3u << 18;
inline constexpr std::uintptr_t kCscmr1 = 0x400FC01Cu;  // SAI1_CLK_SEL bits 11:10
inline constexpr std::uintptr_t kCs1cdr = 0x400FC028u;  // SAI1_CLK_PODF bits 5:0, SAI1_CLK_PRED bits 8:6
inline constexpr std::uint32_t kCscmr1Sai1ClkSelMask = 0x3u << 10;
inline constexpr std::uint32_t kCs1cdrSai1Mask = 0x1FFu;
inline constexpr std::uint32_t kCs1cdrSai1Value = (sai_clock::kPred << 6) | sai_clock::kPodf;
}  // namespace gates

// Settling times (bounded waits of start()).
inline constexpr std::uint32_t kMclkSettleMs = 5;            // MCLK before the first codec write
inline constexpr std::uint32_t kFirstBoundariesWaitMs = 25;  // two boundaries must arrive well within this (a block is 1.45 ms)
inline constexpr std::uint32_t kPllLockPolls = 1'000'000;

}  // namespace o3os::platform::rt10xx::audio_cfg
