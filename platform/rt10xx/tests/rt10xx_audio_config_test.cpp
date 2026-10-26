// SPDX-License-Identifier: GPL-3.0-or-later
//
// The audio path's words (audio_config.h, resources.h): PLL4 and the SAI1 root make MCLK = 256 x Fs exactly, the SAI words decoded field by
// field (the receiver the master, the transmitter synchronous), the eDMA descriptors of the two rings, the request sources, the gates.
#include <doctest/doctest.h>

#include <o3os/hal/audio.h>
#include <o3os/platform/rt10xx/audio_config.h>
#include <o3os/platform/rt10xx/audio_pins.h>
#include <o3os/platform/rt10xx/resources.h>

namespace cfg = o3os::platform::rt10xx::audio_cfg;
namespace pins = o3os::platform::rt10xx::audio_pins;
namespace res = o3os::platform::rt10xx::resources;
namespace haudio = o3os::hal::audio;
using o3os::platform::rt10xx::Irq;

namespace {
std::uint32_t field(std::uint32_t v, unsigned shift, unsigned bits) { return (v >> shift) & ((1u << bits) - 1u); }
}  // namespace

TEST_CASE("rt10xx audio: MCLK from PLL4 and the SAI1 root") {
  CHECK(cfg::pll4::fout_hz() == 1'128'960'000u);
  CHECK(cfg::sai_clock::kMclkHz == 11'289'600u);
  if (o3os::platform::rt10xx::clock::kTreeIsRt10xx) {
    CHECK(cfg::sai_clock::kMclkHz == haudio::kMclkHz);
  }
  CHECK(cfg::sai_clock::kMclkHz == 256u * 44100u);
  CHECK(field(cfg::pll4::kWord, 0, 7) == 47u);      // DIV_SELECT
  CHECK(field(cfg::pll4::kWord, 12, 1) == 0u);      // POWERDOWN clear
  CHECK(field(cfg::pll4::kWord, 13, 1) == 1u);      // ENABLE
  CHECK(field(cfg::pll4::kWord, 16, 1) == 0u);      // BYPASS clear
  CHECK(field(cfg::pll4::kWord, 19, 2) == 2u);      // POST_DIV_SELECT: divide by 1
  CHECK(cfg::pll4::kNumerator == 4u);
  CHECK(cfg::pll4::kDenominator == 100u);
  CHECK(cfg::gates::kCs1cdrSai1Value == ((3u << 6) | 24u));
  CHECK(field(cfg::gates::kCs1cdrSai1Value, 0, 6) == 24u);  // PODF: divide by 25
  CHECK(field(cfg::gates::kCs1cdrSai1Value, 6, 3) == 3u);   // PRED: divide by 4
  CHECK(cfg::sai_clock::kSel == 2u);                        // PLL4
  CHECK(cfg::sai_clock::kGpr1Sai1MclkDir == (1u << 19));
}

TEST_CASE("rt10xx audio: the SAI words") {
  // The receiver: asynchronous (the master), MCLK1, BCP, BCD, DIV 1.
  CHECK(field(cfg::sai::kRcr2Value, 30, 2) == 0u);
  CHECK(field(cfg::sai::kRcr2Value, 26, 2) == 1u);
  CHECK(field(cfg::sai::kRcr2Value, 25, 1) == 1u);
  CHECK(field(cfg::sai::kRcr2Value, 24, 1) == 1u);
  CHECK(field(cfg::sai::kRcr2Value, 0, 8) == 1u);
  // The transmitter: synchronous with the receiver, otherwise the same.
  CHECK(field(cfg::sai::kTcr2Value, 30, 2) == 1u);
  CHECK((cfg::sai::kTcr2Value & 0x3FFFFFFFu) == (cfg::sai::kRcr2Value & 0x3FFFFFFFu));
  // Frame: two 32-bit words, I2S (one bit early, active low, MSB first), the frame sync an output.
  CHECK(field(cfg::sai::kRcr4Value, 16, 5) == 1u);   // FRSZ: 2 words
  CHECK(field(cfg::sai::kRcr4Value, 8, 5) == 31u);   // SYWD: 32 bits
  CHECK(field(cfg::sai::kRcr4Value, 4, 1) == 1u);    // MF
  CHECK(field(cfg::sai::kRcr4Value, 3, 1) == 1u);    // FSE
  CHECK(field(cfg::sai::kRcr4Value, 1, 1) == 1u);    // FSP
  CHECK(field(cfg::sai::kRcr4Value, 0, 1) == 1u);    // FSD
  CHECK(cfg::sai::kTcr4Value == cfg::sai::kRcr4Value);
  CHECK(field(cfg::sai::kRcr5Value, 24, 5) == 31u);  // WNW
  CHECK(field(cfg::sai::kRcr5Value, 16, 5) == 31u);  // W0W
  CHECK(field(cfg::sai::kRcr5Value, 8, 5) == 31u);   // FBT
  CHECK(cfg::sai::kRcr3Value == (1u << 16));
  CHECK(cfg::sai::kTcr3Value == (1u << 16));
  CHECK(cfg::sai::kRcr1Value == 1u);
  CHECK(cfg::sai::kTcr1Value == 1u);
  CHECK(cfg::sai::kTxDataHalf == 0x40384022u);
  CHECK(cfg::sai::kRxDataHalf == 0x403840A2u);
  CHECK(cfg::sai::kTcsr == 0x40384008u);
  CHECK(cfg::sai::kRcsr == 0x40384088u);
}

TEST_CASE("rt10xx audio: the eDMA descriptors") {
  CHECK(cfg::dma::log2_pow2(1024) == 10u);
  CHECK(cfg::dma::log2_pow2(2) == 1u);
  CHECK(cfg::dma::log2_pow2(1) == 0u);
  const std::uint32_t tx_base = 0x20000400u;
  const cfg::dma::Tcd tx = cfg::dma::tx_tcd(tx_base, haudio::kTxRingBytes);
  CHECK(tx.saddr == tx_base);
  CHECK(tx.soff == 2);
  CHECK(field(tx.attr, 11, 5) == 10u);  // SMOD: the 1 KiB ring
  CHECK(field(tx.attr, 8, 3) == 1u);    // SSIZE 16 bits
  CHECK(field(tx.attr, 3, 5) == 0u);    // DMOD none
  CHECK(field(tx.attr, 0, 3) == 1u);    // DSIZE 16 bits
  CHECK(tx.nbytes == 2u);
  CHECK(tx.slast == 0);
  CHECK(tx.daddr == cfg::sai::kTxDataHalf);
  CHECK(tx.doff == 0);
  CHECK(tx.citer == haudio::kBlockSamples);
  CHECK(tx.biter == haudio::kBlockSamples);
  CHECK(tx.dlastsga == 0);
  CHECK(tx.csr == cfg::dma::kCsrIntMajor);
  const std::uint32_t rx_base = 0x20000800u;
  const cfg::dma::Tcd rx = cfg::dma::rx_tcd(rx_base, haudio::kRxRingBytes);
  CHECK(rx.saddr == cfg::sai::kRxDataHalf);
  CHECK(rx.soff == 0);
  CHECK(field(rx.attr, 11, 5) == 0u);
  CHECK(field(rx.attr, 8, 3) == 1u);
  CHECK(field(rx.attr, 3, 5) == 10u);  // DMOD: the 1 KiB ring
  CHECK(field(rx.attr, 0, 3) == 1u);
  CHECK(rx.daddr == rx_base);
  CHECK(rx.doff == 2);
  CHECK(rx.citer == haudio::kBlockSamples);
  CHECK(rx.csr == 0u);
  CHECK(haudio::kTxRingBytes == 1024u);
  CHECK(haudio::kRxRingBytes == 1024u);
  CHECK(haudio::kBlockSamples * 2u * haudio::kTxPhysSlots == haudio::kTxRingBytes);  // a major loop per slot
  CHECK(cfg::dma::kSourceSai1Rx == 19u);
  CHECK(cfg::dma::kSourceSai1Tx == 20u);
  CHECK(cfg::dma::kMuxEnable == 0x80000000u);
  CHECK(cfg::dma::kCrValue == 0x482u);
  CHECK((cfg::dma::kCrValue & cfg::dma::kCrHalt) == 0u);
  CHECK(sizeof(cfg::dma::Tcd) == 32u);
}

TEST_CASE("rt10xx audio: the gates and the settling times") {
  CHECK(cfg::gates::kCcgr5 == 0x400FC07Cu);
  CHECK(cfg::gates::kCcgr5Dma == (3u << 6));
  CHECK(cfg::gates::kCcgr5Sai1 == (3u << 18));
  CHECK(cfg::gates::kCcgr2Lpi2c1 == (3u << 6));
  CHECK(cfg::kMclkSettleMs == 5u);
  CHECK(cfg::kFirstBoundariesWaitMs == 25u);
  CHECK(cfg::kFirstBoundariesWaitMs * 44100u > 2u * haudio::kBlock * 1000u);  // two blocks fit the wait many times over
}

TEST_CASE("rt10xx audio: the resources and the pads") {
  CHECK(res::dma_irq(0) == Irq::DMA0_DMA16);
  CHECK(res::dma_irq(1) == Irq::DMA1_DMA17);
  CHECK(res::dma_irq(15) == Irq::DMA15_DMA31);
  CHECK(res::kDma[res::kDmaTxChannel].arbitration == 15u);
  CHECK(res::kDma[res::kDmaRxChannel].arbitration == 14u);
  CHECK(res::kDma[2].owner == res::DmaOwner::DisplayTx);
  CHECK(res::kMidiPitChannel == 0u);
  CHECK(pins::kMclk.mux_ctl == 0x401F8120u);  // the RM's address of GPIO_AD_B1_09's mux register
  CHECK(pins::kMclk.pad_ctl == 0x401F8310u);
  CHECK(pins::kBclk.select_input == 0x401F8588u);
  CHECK(pins::kLrclk.select_input == 0x401F859Cu);
  CHECK(pins::kRxData.select_input == 0x401F858Cu);
  CHECK(pins::kRxData.select_value == 2u);
  CHECK(pins::kTxData.select_input == 0u);
  CHECK(pins::kScl.mux_ctl == 0x401F80FCu);  // GPIO_AD_B1_00
  CHECK(pins::kSda.mux_ctl == 0x401F8100u);
  CHECK(pins::kCodecI2cAddress == 0x0Au);
  for (const pins::Pad* p : pins::kAudioPads) {
    CHECK(p->alt == 3u);
  }
}
