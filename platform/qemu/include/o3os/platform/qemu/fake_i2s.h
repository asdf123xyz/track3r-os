// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A fake I2S/DMA/codec for the QEMU `mps2-an386` target tests (OS-005a, ADR 0013 D8 "V3"): a hal::audio::Device whose block
// clock is a timer interrupt, driving the SAME render path as the MK66 (audio/pipeline.h: the slot-boundary entry in a
// high-priority ISR, the render from a lower-priority software-pended line) so that the slot protocol, the priorities, the
// underrun policy and the test signal run on the real Cortex-M4 code generation. QEMU has no I2S, no eDMA and no DWT; this
// stands in for them:
//   * CMSDK timer 0 (IRQ 8) is the block clock, its interrupt is the "DMA slot boundary" (priority kAudioDma);
//   * CMSDK timer 1 free-runs and is hal::cycles() (25 MHz virtual-time ticks, no cycle-accuracy claim);
//   * the render line is PendSV (the ADR's fallback line) or, with Config::render_via_stir, IRQ 31 pended through NVIC_STIR exactly as
//     the MK66 pends its software interrupt IRQ 64; both at priority kRender;
//   * the fake DMA owns a TX ring and an RX ring in the same geometry as the MK66's (hal/audio.h): at each boundary the "DAC"
//     records the slot that starts to play and checks the slot that just finished was not written while it played; the "ADC"
//     completes an RX block with a known tag pattern.
// Everything the test bench inspects is a plain member; all of it lives in static memory (no heap).

#include <o3os/audio/pipeline.h>
#include <o3os/hal/audio.h>

#include <cstdint>

namespace o3os::platform::qemu {

inline constexpr std::uint32_t kFakeTimerHz = 25'000'000;   // the model's CMSDK timer clock
inline constexpr std::uint32_t kFakeBlockTicks = 20'000;    // default block period: 0.8 ms of virtual time
inline constexpr std::uint32_t kFakeMaxPlayed = 12'000;     // played-block records kept
inline constexpr std::uint32_t kFakeFullBlocks = 64;        // played blocks whose samples are kept whole

// What the fake DAC heard at one boundary: the block that started to play, as a checksum plus its first four samples.
struct FakePlayed {
  std::uint32_t seq;        // the boundary number
  std::uint32_t slot;
  std::uint32_t checksum;   // FNV-1a over the block's 128 int16
  std::int16_t head[4];
};

enum class FakeEvent : std::uint8_t { AmpMuted, StartBegin, ClockRunning, UnmuteBegin, AmpOpen, MuteCalled };

class FakeI2s final : public hal::audio::Device {
 public:
  struct Config {
    std::uint32_t block_ticks = kFakeBlockTicks;
    // false: the RX block completes just before the TX boundary is taken (the render reads the block completed at this boundary);
    // true: just after (it reads the block of the previous boundary). Both are legal phases of the real hardware.
    bool rx_after_tx = false;
    // false: the render line is PendSV, pended through SCB->ICSR. true: IRQ 31, pended by writing its number to NVIC_STIR, the path
    // the MK66's IRQ 64 takes (platform/mk66/src/audio.cpp).
    bool render_via_stir = false;
    // OS-MAINGUARD: the pipeline's main-loop starvation guard (the test's thread loop calls pipeline().main_pass()).
    bool starve_guard = false;
  };

  // Before start(); resets the bench.
  void configure(const Config& config) noexcept;

  Result start(const hal::audio::BlockCallback& render) noexcept override;
  bool unmute() noexcept override;
  void mute() noexcept override;
  bool outputs_enabled() const noexcept override { return enabled_; }
  // Stops the block clock (no more boundaries). The test calls it when it has what it wants.
  void stop() noexcept;

  // ---- The bench ----------------------------------------------------------------------------------------------------------
  o3os::audio::Pipeline& pipeline() noexcept { return pipeline_; }
  std::uint32_t played_count() const noexcept { return played_count_; }
  const FakePlayed& played(std::uint32_t index) const noexcept { return played_[index]; }
  const std::int16_t* full_block(std::uint32_t index) const noexcept { return full_[index]; }
  // Boundaries at which the slot that had been playing differed from its content when it started to play: a write into the
  // playing slot (the slot-ownership violation the protocol exists to prevent).
  std::uint32_t torn() const noexcept { return torn_; }
  // The render ran while the DMA ISR was active (it must never).
  std::uint32_t render_in_isr() const noexcept { return render_in_isr_; }
  // True while the fake's boundary ISR is running: a render callback that sees it set is running inside the DMA ISR.
  bool in_dma_isr() const noexcept { return in_isr_; }
  // The render line's exception number as the render saw it (IPSR), the last render; 0 before any.
  std::uint32_t render_ipsr() const noexcept { return render_ipsr_; }
  // The exception number a render must have: 14 (PendSV), or 16 + 31 for the STIR line.
  std::uint32_t expected_render_ipsr() const noexcept { return config_.render_via_stir ? 16u + 31u : 14u; }
  // The boundary ISR fired while a render was running: it preempted it.
  std::uint32_t isr_preempted_render() const noexcept { return isr_preempted_render_; }
  // Longest time Pipeline::on_boundary took, in timer ticks.
  std::uint32_t on_boundary_ticks_max() const noexcept { return on_boundary_ticks_max_; }
  std::uint32_t block_ticks() const noexcept { return config_.block_ticks; }
  std::uint32_t events(FakeEvent* out, std::uint32_t capacity) const noexcept;
  // The tag the fake ADC wrote into RX block `index` (index = boundary number): see rx_tag().
  static void fill_rx_block(std::int16_t* block, std::uint32_t tag) noexcept;
  static std::uint32_t rx_tag(const std::int16_t* block) noexcept;

  // Called by the interrupt handlers (extern "C" in fake_i2s.cpp).
  void on_tick() noexcept;
  void on_render() noexcept;

 private:
  void log_event(FakeEvent e) noexcept;

  Config config_;
  o3os::audio::Pipeline pipeline_;
  bool started_ = false;
  bool enabled_ = false;
  FakeEvent events_[16] = {};
  std::uint32_t event_count_ = 0;

  // The fake DMA's position, ISR only.
  std::uint32_t tx_playing_ = 0;
  std::uint32_t rx_writing_ = 0;
  std::uint32_t tick_ = 0;
  std::uint32_t slot_checksum_at_start_ = 0;

  FakePlayed played_[kFakeMaxPlayed];
  std::uint32_t played_count_ = 0;
  alignas(8) std::int16_t full_[kFakeFullBlocks][hal::audio::kBlockSamples];
  std::uint32_t torn_ = 0;
  std::uint32_t render_in_isr_ = 0;
  std::uint32_t render_ipsr_ = 0;
  std::uint32_t isr_preempted_render_ = 0;
  std::uint32_t on_boundary_ticks_max_ = 0;
  volatile bool in_isr_ = false;
  volatile bool render_active_ = false;
};

// The one fake (static storage).
FakeI2s& fake_i2s() noexcept;

// The free-running tick counter (hal::cycles() on this platform), and a busy wait on it: how a test makes a render slow.
std::uint32_t fake_ticks() noexcept;
void fake_spin_ticks(std::uint32_t ticks) noexcept;

// The DMA rings of the fake, aligned like the MK66's (hal/audio.h): exposed so a test can look at them.
std::int16_t* fake_tx_ring() noexcept;
std::int16_t* fake_rx_ring() noexcept;

}  // namespace o3os::platform::qemu
