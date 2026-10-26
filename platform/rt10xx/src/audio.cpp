// SPDX-License-Identifier: GPL-3.0-or-later
//
// The rt10xx audio path (OS-PORT-RT10XX phase 3): SAI1 as the master of a 16-bit stereo I2S link to the SGTL5000 (the receiver makes the
// clocks, the transmitter is synchronous to it: audio_config.h says why), two eDMA channels moving 64-frame blocks between the rings in the
// DTCM and the SAI's data registers, a slot-boundary ISR that only counts and pends, and the render line. The structure and the order are
// the MK66 driver's (platform/mk66 src/audio.cpp; H16: nothing is audible before the codec is configured and muted, start() never unmutes):
//
//   1. the output stage muted (this board has no known amplifier key: the codec's mutes are the stage; audio_pins.h)
//   2. the clocks: PLL4 locked, the SAI1 root, the SAI's clock gate, the eDMA's; MCLK driven out on its pad
//   3. LPI2C1, the codec's part id, the power-up plan (sgtl5000.h): outputs muted and volumes at their floor
//   4. SAI1 configured (not enabled), the eDMA channels' TCDs loaded and armed, the pipeline initialised, the two interrupt lines enabled
//   5. the pads to the SAI, the receiver (the clock master) then the transmitter enabled: the frame clocks start, the DMA feeds silence
//   6. a bounded wait for the first boundaries: if none comes the hardware is stopped and start() fails, still muted
//
// The pads are assumed (audio_pins.h) and driven by nothing but this start(): the maintainer's `audio start` is when they are first touched.
#include "internal.h"
#include "regs.h"

#include <cstring>

#include <o3os/audio/engine.h>
#include <o3os/audio/pipeline.h>
#include <o3os/hal/audio.h>
#include <o3os/hal/cycles.h>
#include <o3os/platform/mk66/sgtl5000.h>
#include <o3os/platform/rt10xx/audio.h>
#include <o3os/platform/rt10xx/audio_config.h>
#include <o3os/platform/rt10xx/audio_pins.h>
#include <o3os/platform/rt10xx/i2c.h>
#include <o3os/platform/rt10xx/irq_map.h>
#include <o3os/platform/rt10xx/mem_map.h>
#include <o3os/platform/rt10xx/resources.h>
#include <o3os/platform/rt10xx/sections.h>

extern "C" {
o3os::audio::Pipeline o3os_audio_pipeline;
const o3os::audio::AudioStatus* o3os_audio_status_ptr = nullptr;
}

namespace o3os::platform::rt10xx {
namespace {

namespace haudio = o3os::hal::audio;
namespace cfg = audio_cfg;
namespace sgtl5000 = o3os::platform::mk66::sgtl5000;
using regs::at;

constexpr std::uint8_t kTxChannel = resources::kDmaTxChannel;
constexpr std::uint8_t kRxChannel = resources::kDmaRxChannel;
constexpr unsigned kRenderIrq = static_cast<unsigned>(kRenderLine);
constexpr unsigned kTxDmaIrq = static_cast<unsigned>(resources::dma_irq(kTxChannel));
constexpr unsigned kSlotShift = cfg::dma::log2_pow2(haudio::kBlockBytes);
static_assert((1u << kSlotShift) == haudio::kBlockBytes, "a slot index is a shift of the DMA address");

// ---- The DMA rings: in the DTCM, aligned to their size (the eDMA's address modulo) ------------------------------------------------------
struct alignas(haudio::kTxRingBytes) TxRing {
  std::int16_t samples[haudio::kTxPhysSlots * haudio::kBlockSamples];
};
struct alignas(haudio::kRxRingBytes) RxRing {
  std::int16_t samples[haudio::kRxPhysSlots * haudio::kBlockSamples];
};
static_assert(haudio::kTxRingBytes <= mem::kDmaRingAlign && haudio::kRxRingBytes <= mem::kDmaRingAlign, "the rings' section is aligned to the largest ring");
O3OS_DMA_RING TxRing g_tx;
O3OS_DMA_RING RxRing g_rx;
std::int16_t* const g_tx_ring = g_tx.samples;
const std::int16_t* const g_rx_ring = g_rx.samples;

// The audio taps' memory in the ITCM (behind the hot code), the engine and the voices in the DTCM.
O3OS_TAP_MEM_L o3os::audio::TapMem g_tap_mem;
static_assert(sizeof(o3os::audio::TapMem) <= mem::kItcmDataBudget, "the audio taps' memory outgrew its ITCM budget (mem_map.h kItcmDataBudget)");
o3os::audio::Engine g_engine(&o3os::hal::cycles, g_tap_mem);
o3os::audio::VoicePool g_voices;
o3os::audio::AmigaVoices g_amiga;
std::uint8_t g_last_result = 0;
AudioClockFault g_clock_fault;

constexpr std::uint32_t kStartupBoundaries = 2;
std::uint32_t g_warmup = kStartupBoundaries;

void note_clock_fault(std::uint8_t where) noexcept {
  g_clock_fault.where = where;
  g_clock_fault.pll = at<std::uint32_t>(cfg::pll4::kReg);
  g_clock_fault.rcsr = at<std::uint32_t>(cfg::sai::kRcsr);
  g_clock_fault.tcsr = at<std::uint32_t>(cfg::sai::kTcsr);
  g_last_result = static_cast<std::uint8_t>(haudio::Device::Result::ClockFault);
}

void mux_pad(const audio_pins::Pad& pad) noexcept {
  at<std::uint32_t>(pad.mux_ctl) = pad.alt;
  at<std::uint32_t>(pad.pad_ctl) = audio_pins::kAudioPadWord;
  if (pad.select_input != 0u) {
    at<std::uint32_t>(pad.select_input) = pad.select_value;
  }
}

void load_tcd(unsigned channel, const cfg::dma::Tcd& t) noexcept {
  volatile cfg::dma::Tcd& r = regs::dma_tcd_as<cfg::dma::Tcd>(channel);
  r.csr = 0;
  r.saddr = t.saddr;
  r.soff = t.soff;
  r.attr = t.attr;
  r.nbytes = t.nbytes;
  r.slast = t.slast;
  r.daddr = t.daddr;
  r.doff = t.doff;
  r.citer = t.citer;
  r.dlastsga = t.dlastsga;
  r.biter = t.biter;
  r.csr = t.csr;
}

void quiesce_channel(unsigned channel) noexcept {
  regs::dma_cerq() = static_cast<std::uint8_t>(channel);
  regs::dma_ceei() = static_cast<std::uint8_t>(channel);
  regs::dma_cerr() = static_cast<std::uint8_t>(channel);
  regs::dma_cdne() = static_cast<std::uint8_t>(channel);
  regs::dma_cint() = static_cast<std::uint8_t>(channel);
}

void stop_hardware() noexcept {
  at<std::uint32_t>(cfg::sai::kTcsr) = 0;
  at<std::uint32_t>(cfg::sai::kRcsr) = 0;
  quiesce_channel(kTxChannel);
  quiesce_channel(kRxChannel);
  regs::nvic_icer(kTxDmaIrq / 32u) = regs::irq_bit(kTxDmaIrq);
  regs::nvic_icer(kRenderIrq / 32u) = regs::irq_bit(kRenderIrq);
}

// PLL4 at its word, locked; the SAI1 root from it; MCLK out on its pad. False when the lock does not come.
bool start_mclk() noexcept {
  at<std::uint32_t>(cfg::pll4::kNum) = cfg::pll4::kNumerator;
  at<std::uint32_t>(cfg::pll4::kDenom) = cfg::pll4::kDenominator;
  at<std::uint32_t>(cfg::pll4::kMisc2) &= ~cfg::pll4::kMisc2AudioDivMask;
  at<std::uint32_t>(cfg::pll4::kReg) = cfg::pll4::kWord | cfg::pll4::kBypass;  // bypassed while it locks
  for (std::uint32_t i = 0; i < cfg::kPllLockPolls; ++i) {
    if ((at<std::uint32_t>(cfg::pll4::kReg) & cfg::pll4::kLock) != 0u) {
      at<std::uint32_t>(cfg::pll4::kReg + 8u) = cfg::pll4::kBypass;  // CLR: the PLL's output from here
      at<std::uint32_t>(cfg::gates::kCscmr1) = (at<std::uint32_t>(cfg::gates::kCscmr1) & ~cfg::gates::kCscmr1Sai1ClkSelMask) | (cfg::sai_clock::kSel << 10);
      at<std::uint32_t>(cfg::gates::kCs1cdr) = (at<std::uint32_t>(cfg::gates::kCs1cdr) & ~cfg::gates::kCs1cdrSai1Mask) | cfg::gates::kCs1cdrSai1Value;
      at<std::uint32_t>(cfg::gates::kCcgr5) |= cfg::gates::kCcgr5Sai1;
      at<std::uint32_t>(cfg::sai_clock::kGpr1) |= cfg::sai_clock::kGpr1Sai1MclkDir;
      mux_pad(audio_pins::kMclk);
      return true;
    }
  }
  return false;
}

class Rt10xxAudio final : public haudio::Device {
 public:
  Result start(const haudio::BlockCallback& render) noexcept override;
  bool unmute() noexcept override;
  void mute() noexcept override;
  void stop() noexcept;
  bool outputs_enabled() const noexcept override { return enabled_; }
  bool running() const noexcept { return started_; }
  bool apply_input(const sgtl5000::InputPlan& plan) noexcept;

 private:
  bool codec_write(std::uint16_t reg, std::uint16_t value) noexcept { return i2c::write_reg(audio_pins::kCodecI2cAddress, reg, value) == i2c::Result::Ok; }
  bool plan_write(const sgtl5000::Step& step) noexcept {
    const std::uint16_t value = step.reg == sgtl5000::kAnaCtrl ? sgtl5000::ana_ctrl_with_adc(step.value, adc_line_) : step.value;
    return codec_write(step.reg, value);
  }
  Result bring_up_codec() noexcept;
  bool started_ = false;
  bool enabled_ = false;
  bool adc_line_ = true;
};

Rt10xxAudio::Result Rt10xxAudio::bring_up_codec() noexcept {
  if (i2c::init() != i2c::Result::Ok) {
    return Result::CodecIo;
  }
  std::uint16_t id = 0;
  if (i2c::read_reg(audio_pins::kCodecI2cAddress, sgtl5000::kChipId, id) != i2c::Result::Ok) {
    return Result::CodecAbsent;
  }
  if ((id >> 8) != sgtl5000::kPartId) {
    return Result::CodecAbsent;
  }
  for (const sgtl5000::Step& step : sgtl5000::kPowerUp) {
    if (!codec_write(step.reg, step.value)) {
      return Result::CodecIo;
    }
    if (step.wait_ms != 0u) {
      boot::delay_ms(step.wait_ms);
    }
  }
  return Result::Ok;
}

Rt10xxAudio::Result Rt10xxAudio::start(const haudio::BlockCallback& render) noexcept {
  if (started_) {
    return Result::AlreadyStarted;
  }
  g_last_result = 0;
  g_clock_fault = AudioClockFault{};
  if ((reinterpret_cast<std::uintptr_t>(g_tx_ring) & (haudio::kTxRingBytes - 1u)) != 0u || (reinterpret_cast<std::uintptr_t>(g_rx_ring) & (haudio::kRxRingBytes - 1u)) != 0u ||
      !mem::in_ram(static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(g_tx_ring)), haudio::kTxRingBytes) ||
      !mem::in_ram(static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(g_rx_ring)), haudio::kRxRingBytes)) {
    panic();  // an unaligned DMA ring is a bug in the image
  }

  // 1. Muted: there is no amplifier key known on this board; the codec comes up muted (step 3) and stays so until unmute().
  enabled_ = false;
  std::memset(g_tx.samples, 0, sizeof(g_tx.samples));
  std::memset(g_rx.samples, 0, sizeof(g_rx.samples));

  // 2. Clocks: the eDMA's global state must be the boot's; PLL4, the SAI root, MCLK out.
  if ((regs::dma_cr() & cfg::dma::kCrValue) != cfg::dma::kCrValue) {
    panic();  // start() before the reset sequence's dma_init()
  }
  if (!start_mclk()) {
    note_clock_fault(AudioClockFault::PllLock);
    return Result::ClockFault;
  }
  boot::delay_ms(cfg::kMclkSettleMs);

  // 3. The codec, muted.
  const Result codec = bring_up_codec();
  if (codec != Result::Ok) {
    g_last_result = static_cast<std::uint8_t>(codec);
    return codec;
  }

  // 4. SAI format, DMA, pipeline.
  at<std::uint32_t>(cfg::sai::kTcsr) = cfg::sai::kCsrSoftReset;
  at<std::uint32_t>(cfg::sai::kRcsr) = cfg::sai::kCsrSoftReset;
  at<std::uint32_t>(cfg::sai::kTcsr) = 0;
  at<std::uint32_t>(cfg::sai::kRcsr) = 0;
  at<std::uint32_t>(cfg::sai::kTmr) = 0;
  at<std::uint32_t>(cfg::sai::kRmr) = 0;
  at<std::uint32_t>(cfg::sai::kRcr1) = cfg::sai::kRcr1Value;
  at<std::uint32_t>(cfg::sai::kRcr2) = cfg::sai::kRcr2Value;
  at<std::uint32_t>(cfg::sai::kRcr3) = cfg::sai::kRcr3Value;
  at<std::uint32_t>(cfg::sai::kRcr4) = cfg::sai::kRcr4Value;
  at<std::uint32_t>(cfg::sai::kRcr5) = cfg::sai::kRcr5Value;
  at<std::uint32_t>(cfg::sai::kTcr1) = cfg::sai::kTcr1Value;
  at<std::uint32_t>(cfg::sai::kTcr2) = cfg::sai::kTcr2Value;
  at<std::uint32_t>(cfg::sai::kTcr3) = cfg::sai::kTcr3Value;
  at<std::uint32_t>(cfg::sai::kTcr4) = cfg::sai::kTcr4Value;
  at<std::uint32_t>(cfg::sai::kTcr5) = cfg::sai::kTcr5Value;

  quiesce_channel(kTxChannel);
  quiesce_channel(kRxChannel);
  const std::uint32_t tx_base = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(g_tx_ring));
  const std::uint32_t rx_base = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(g_rx_ring));
  load_tcd(kTxChannel, cfg::dma::tx_tcd(tx_base, haudio::kTxRingBytes));
  load_tcd(kRxChannel, cfg::dma::rx_tcd(rx_base, haudio::kRxRingBytes));
  regs::dmamux_chcfg(kTxChannel) = 0;
  regs::dmamux_chcfg(kRxChannel) = 0;
  regs::dmamux_chcfg(kTxChannel) = cfg::dma::kMuxEnable | cfg::dma::kSourceSai1Tx;
  regs::dmamux_chcfg(kRxChannel) = cfg::dma::kMuxEnable | cfg::dma::kSourceSai1Rx;

  o3os::audio::Pipeline::Config config{};
  config.tx = g_tx_ring;
  config.rx = g_rx_ring;
  config.render = render;
  config.cycles = &o3os::hal::cycles;
  config.cycles_per_block = haudio::kCyclesPerBlock;
  config.starve_guard = false;
  o3os_audio_pipeline.init(config);
  o3os_audio_status_ptr = &o3os_audio_pipeline.status();
  g_warmup = kStartupBoundaries;

  regs::nvic_icpr(kTxDmaIrq / 32u) = regs::irq_bit(kTxDmaIrq);
  regs::nvic_icpr(kRenderIrq / 32u) = regs::irq_bit(kRenderIrq);
  regs::nvic_iser(kRenderIrq / 32u) = regs::irq_bit(kRenderIrq);
  regs::nvic_iser(kTxDmaIrq / 32u) = regs::irq_bit(kTxDmaIrq);
  regs::dma_serq() = kRxChannel;
  regs::dma_serq() = kTxChannel;

  // 5. The pads, then the receiver (the clock master) and the transmitter.
  mux_pad(audio_pins::kLrclk);
  mux_pad(audio_pins::kBclk);
  mux_pad(audio_pins::kTxData);
  mux_pad(audio_pins::kRxData);
  at<std::uint32_t>(cfg::sai::kTcsr) = cfg::sai::kCsrEnable | cfg::sai::kCsrBitClockEnable | cfg::sai::kCsrFifoRequestDmaEnable | cfg::sai::kCsrFifoReset;
  at<std::uint32_t>(cfg::sai::kRcsr) = cfg::sai::kCsrEnable | cfg::sai::kCsrBitClockEnable | cfg::sai::kCsrFifoRequestDmaEnable | cfg::sai::kCsrFifoReset;

  // 6. The boundaries must come.
  bool running = false;
  for (std::uint32_t ms = 0; ms < cfg::kFirstBoundariesWaitMs && !running; ++ms) {
    boot::delay_ms(1);
    running = o3os_audio_pipeline.status().snapshot().boundaries >= 2u;
  }
  if (!running) {
    note_clock_fault(AudioClockFault::NoBoundaries);
    stop_hardware();
    return Result::ClockFault;
  }
  started_ = true;
  return Result::Ok;
}

bool Rt10xxAudio::unmute() noexcept {
  if (!started_) {
    return false;
  }
  sgtl5000::Step step{};
  for (unsigned i = 0; sgtl5000::unmute_step(i, step); ++i) {
    if (!plan_write(step)) {
      mute();
      return false;
    }
    if (step.wait_ms != 0u) {
      boot::delay_ms(step.wait_ms);
    }
  }
  enabled_ = true;
  return true;
}

void Rt10xxAudio::mute() noexcept {
  enabled_ = false;
  if (started_) {
    for (const sgtl5000::Step& step : sgtl5000::kMute) {
      plan_write(step);
    }
  }
}

void Rt10xxAudio::stop() noexcept {
  enabled_ = false;
  if (!started_) {
    return;
  }
  (void)sgtl5000::run_stop([this](const sgtl5000::Step& step) { return plan_write(step); }, [] {},
                           [](std::uint32_t ms) {
                             if (ms != 0u) {
                               boot::delay_ms(ms);
                             }
                           });
  stop_hardware();
  started_ = false;
}

bool Rt10xxAudio::apply_input(const sgtl5000::InputPlan& plan) noexcept {
  if (!started_) {
    return false;
  }
  for (unsigned i = 0; i < plan.count; ++i) {
    if (!codec_write(plan.step[i].reg, plan.step[i].value)) {
      return false;
    }
    if (plan.step[i].reg == sgtl5000::kAnaCtrl) {
      adc_line_ = plan.line;
    }
  }
  return true;
}

Rt10xxAudio g_device;

}  // namespace

hal::audio::Device& audio_device() noexcept { return g_device; }
bool audio_input_apply(const sgtl5000::InputPlan& plan) noexcept { return g_device.apply_input(plan); }
void audio_stop() noexcept { g_device.stop(); }
o3os::audio::Engine& audio_engine() noexcept { return g_engine; }
o3os::audio::VoicePool& audio_voices() noexcept { return g_voices; }
o3os::audio::AmigaVoices& audio_amiga() noexcept { return g_amiga; }
const o3os::audio::AudioStatus& audio_status() noexcept { return o3os_audio_pipeline.status(); }
void audio_cpu_reset() noexcept { o3os_audio_pipeline.reset_peaks(); }
void audio_main_pass() noexcept { o3os_audio_pipeline.main_pass(); }
const o3os::audio::Pipeline& audio_pipeline() noexcept { return o3os_audio_pipeline; }
std::uint8_t audio_last_result() noexcept { return g_last_result; }
AudioClockFault audio_clock_fault() noexcept { return g_clock_fault; }
bool audio_running() noexcept { return g_device.running(); }
const char* audio_result_name(std::uint8_t result) noexcept {
  switch (static_cast<haudio::Device::Result>(result)) {
    case haudio::Device::Result::Ok: return "ok";
    case haudio::Device::Result::AlreadyStarted: return "already started";
    case haudio::Device::Result::CodecAbsent: return "codec absent";
    case haudio::Device::Result::CodecIo: return "codec io";
    case haudio::Device::Result::ClockFault: return "clock fault";
  }
  return "?";
}

// ---- Interrupts ------------------------------------------------------------------------------------------------------------------------
// The slot-boundary ISR (eDMA channel 0's line, priority kAudioDma, runs from the ITCM): stamp first, clear the request, read the two DMA
// positions, poll the error flags, hand over to the pipeline, pend the render.
static_assert(resources::kDmaTxChannel == 0, "the handler below is named for the transmit channel's line, DMA0_DMA16");
extern "C" O3OS_RAMFUNC_L void DMA0_DMA16_IRQHandler() noexcept {
  const std::uint32_t stamp = regs::dwt_cyccnt();
  regs::dma_cint() = kTxChannel;
  asm volatile("dsb" ::: "memory");

  const std::uint32_t tx_saddr = regs::dma_tcd_as<cfg::dma::Tcd>(kTxChannel).saddr;
  const std::uint32_t rx_daddr = regs::dma_tcd_as<cfg::dma::Tcd>(kRxChannel).daddr;
  const std::uint32_t tx_base = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(g_tx_ring));
  const std::uint32_t rx_base = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(g_rx_ring));
  const std::uint32_t tx_slot = ((tx_saddr - tx_base) >> kSlotShift) & (haudio::kTxPhysSlots - 1u);
  const std::uint32_t rx_slot = ((rx_daddr - rx_base) >> kSlotShift) & (haudio::kRxPhysSlots - 1u);

  std::uint32_t faults = 0;
  const std::uint32_t tcsr = at<std::uint32_t>(cfg::sai::kTcsr);
  const std::uint32_t rcsr = at<std::uint32_t>(cfg::sai::kRcsr);
  if ((tcsr & cfg::sai::kCsrErrorFlags) != 0u) {
    at<std::uint32_t>(cfg::sai::kTcsr) = tcsr;
    ++faults;
  }
  if ((rcsr & cfg::sai::kCsrErrorFlags) != 0u) {
    at<std::uint32_t>(cfg::sai::kRcsr) = rcsr;
    ++faults;
  }
  if (regs::dma_err() != 0u) {
    regs::dma_cerr() = regs::kDmaClearAll;
    ++faults;
  }
  if (g_warmup != 0u) {
    --g_warmup;
    faults = 0;
  }
  o3os_audio_pipeline.on_boundary(tx_slot, rx_slot, stamp, faults);
  regs::nvic_stir() = kRenderIrq;
}

// The render line (the reserved line 70, priority kRender, the ITCM).
extern "C" O3OS_RAMFUNC_L void Reserved86_IRQHandler() noexcept { o3os_audio_pipeline.render(); }

}  // namespace o3os::platform::rt10xx
