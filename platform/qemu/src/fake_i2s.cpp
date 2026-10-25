// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/platform/qemu/fake_i2s.h>

#include <o3os/hal/cycles.h>
#include <o3os/hal/irq.h>

namespace o3os::platform::qemu {
namespace {

namespace haudio = o3os::hal::audio;

volatile std::uint32_t& reg(std::uintptr_t address) { return *reinterpret_cast<volatile std::uint32_t*>(address); }

// CMSDK APB timer of the mps2 board: CTRL (enable, interrupt enable), VALUE (counts down), RELOAD, INTSTATUS / INTCLEAR.
constexpr std::uintptr_t kTimer0 = 0x40000000;
constexpr std::uintptr_t kTimer1 = 0x40001000;
constexpr std::uint32_t kCtrlEnable = 1u << 0;
constexpr std::uint32_t kCtrlIrqEnable = 1u << 3;
constexpr unsigned kTimer0Irq = 8;
constexpr unsigned kStirIrq = 31;                      // the second render line, pended through NVIC_STIR
constexpr std::uintptr_t kNvicStir = 0xE000EF00;

// Cortex-M system registers (ARMv7-M ARM B3.2, B3.4).
constexpr std::uintptr_t kScbIcsr = 0xE000ED04;
constexpr std::uintptr_t kScbShpr3 = 0xE000ED20;       // bytes: PendSV at +2, SysTick at +3
constexpr std::uintptr_t kNvicIser = 0xE000E100;
constexpr std::uintptr_t kNvicIcer = 0xE000E180;
constexpr std::uintptr_t kNvicIcpr = 0xE000E280;
constexpr std::uintptr_t kNvicIpr = 0xE000E400;
constexpr std::uint32_t kIcsrPendSvSet = 1u << 28;
constexpr std::uint32_t kIcsrPendSvClear = 1u << 27;

volatile std::uint8_t& byte_reg(std::uintptr_t address) { return *reinterpret_cast<volatile std::uint8_t*>(address); }

// The fake DMA rings, in the MK66's geometry and with its declared alignment (the QEMU layer does not need the alignment, the
// pipeline's geometry does not change with it, and a test that checks placement should see the same declaration).
struct alignas(haudio::kTxRingBytes) TxRing {
  std::int16_t samples[haudio::kTxPhysSlots * haudio::kBlockSamples];
};
struct alignas(haudio::kRxRingBytes) RxRing {
  std::int16_t samples[haudio::kRxPhysSlots * haudio::kBlockSamples];
};
TxRing g_tx;
RxRing g_rx;

FakeI2s g_fake;

std::uint32_t fnv(const std::int16_t* block) {
  std::uint32_t h = 2166136261u;
  for (std::uint32_t i = 0; i < haudio::kBlockSamples; ++i) {
    h = (h ^ static_cast<std::uint16_t>(block[i])) * 16777619u;
  }
  return h;
}

std::uint32_t ipsr() {
  std::uint32_t v;
  asm volatile("mrs %0, ipsr" : "=r"(v));
  return v;
}

}  // namespace

// ---- hal::cycles(): timer 1, a free-running down counter inverted into an up counter ---------------------------------------
std::uint32_t fake_ticks() noexcept { return ~reg(kTimer1 + 0x04); }
void fake_spin_ticks(std::uint32_t ticks) noexcept {
  const std::uint32_t start = fake_ticks();
  while (static_cast<std::uint32_t>(fake_ticks() - start) < ticks) {
  }
}
std::int16_t* fake_tx_ring() noexcept { return g_tx.samples; }
std::int16_t* fake_rx_ring() noexcept { return g_rx.samples; }
FakeI2s& fake_i2s() noexcept { return g_fake; }

// A block of the fake ADC: sample 0 and 1 carry the tag (low and high half), the rest a pattern of the tag, so a block that is
// torn or mixed up is not mistaken for another.
void FakeI2s::fill_rx_block(std::int16_t* block, std::uint32_t tag) noexcept {
  block[0] = static_cast<std::int16_t>(tag & 0xFFFFu);
  block[1] = static_cast<std::int16_t>(tag >> 16);
  for (std::uint32_t i = 2; i < haudio::kBlockSamples; ++i) {
    block[i] = static_cast<std::int16_t>((tag * 31u + i * 7u) & 0x7FFFu);
  }
}
std::uint32_t FakeI2s::rx_tag(const std::int16_t* block) noexcept {
  return static_cast<std::uint16_t>(block[0]) | (static_cast<std::uint32_t>(static_cast<std::uint16_t>(block[1])) << 16);
}

void FakeI2s::configure(const Config& config) noexcept {
  config_ = config;
  played_count_ = 0;
  torn_ = 0;
  render_in_isr_ = 0;
  render_ipsr_ = 0;
  isr_preempted_render_ = 0;
  on_boundary_ticks_max_ = 0;
  event_count_ = 0;
}

void FakeI2s::log_event(FakeEvent e) noexcept {
  if (event_count_ < sizeof(events_) / sizeof(events_[0])) {
    events_[event_count_++] = e;
  }
}
std::uint32_t FakeI2s::events(FakeEvent* out, std::uint32_t capacity) const noexcept {
  const std::uint32_t n = event_count_ < capacity ? event_count_ : capacity;
  for (std::uint32_t i = 0; i < n; ++i) {
    out[i] = events_[i];
  }
  return n;
}

FakeI2s::Result FakeI2s::start(const haudio::BlockCallback& render) noexcept {
  if (started_) {
    return Result::AlreadyStarted;
  }
  // H16: the output stage is muted first, whatever it was.
  enabled_ = false;
  log_event(FakeEvent::AmpMuted);
  log_event(FakeEvent::StartBegin);

  // The fake DMA rings are silence; the RX ring holds tag 0.
  for (std::int16_t& s : g_tx.samples) {
    s = 0;
  }
  for (std::int16_t& s : g_rx.samples) {
    s = 0;
  }
  tx_playing_ = 0;
  rx_writing_ = 0;
  tick_ = 0;
  slot_checksum_at_start_ = fnv(g_tx.samples);
  played_count_ = 0;

  // The free-running counter first: the pipeline's stamps need it.
  reg(kTimer1 + 0x08) = 0xFFFFFFFFu;
  reg(kTimer1 + 0x00) = kCtrlEnable;

  haudio::BlockCallback cb = render;
  o3os::audio::Pipeline::Config pc{};
  pc.tx = g_tx.samples;
  pc.rx = g_rx.samples;
  pc.render = cb;
  pc.cycles = &o3os::hal::cycles;
  pc.cycles_per_block = config_.block_ticks;
  pc.starve_guard = config_.starve_guard;
  pipeline_.init(pc);

  // The ADR 0013 D1 priorities, from the one table: the block clock is the DMA ISR, PendSV the render, SysTick as on the MK66.
  byte_reg(kNvicIpr + kTimer0Irq) = hal::irq::priority(hal::irq::Role::AudioDma);
  byte_reg(kNvicIpr + kStirIrq) = hal::irq::priority(hal::irq::Role::Render);
  byte_reg(kScbShpr3 + 2) = hal::irq::priority(hal::irq::Role::Render);
  byte_reg(kScbShpr3 + 3) = hal::irq::priority(hal::irq::Role::SysTick);
  reg(kNvicIcpr) = (1u << kTimer0Irq) | (1u << kStirIrq);
  reg(kNvicIser) = (1u << kTimer0Irq) | (config_.render_via_stir ? (1u << kStirIrq) : 0u);
  asm volatile("dsb\n\tisb" ::: "memory");

  reg(kTimer0 + 0x0C) = 1;  // clear any stale interrupt
  reg(kTimer0 + 0x08) = config_.block_ticks;
  reg(kTimer0 + 0x00) = kCtrlEnable | kCtrlIrqEnable;
  started_ = true;
  log_event(FakeEvent::ClockRunning);
  return Result::Ok;
}

bool FakeI2s::unmute() noexcept {
  if (!started_) {
    return false;
  }
  log_event(FakeEvent::UnmuteBegin);
  enabled_ = true;
  log_event(FakeEvent::AmpOpen);  // the last step
  return true;
}

void FakeI2s::mute() noexcept {
  log_event(FakeEvent::MuteCalled);
  enabled_ = false;
}

void FakeI2s::stop() noexcept {
  reg(kTimer0 + 0x00) = 0;
  reg(kNvicIcer) = (1u << kTimer0Irq) | (1u << kStirIrq);
  // Thread mode only: a pending render runs the moment the barriers below let it, so none is left to touch a bench that is
  // about to go out of scope; a stale pend bit is cleared.
  asm volatile("dsb\n\tisb" ::: "memory");
  asm volatile("dsb\n\tisb" ::: "memory");
  reg(kScbIcsr) = kIcsrPendSvClear;
  reg(kNvicIcpr) = (1u << kTimer0Irq) | (1u << kStirIrq);
  started_ = false;
}

// The fake DMA slot boundary (the timer interrupt, priority kAudioDma).
void FakeI2s::on_tick() noexcept {
  const std::uint32_t stamp = fake_ticks();
  reg(kTimer0 + 0x0C) = 1;  // clear the interrupt
  in_isr_ = true;
  if (render_active_) {
    ++isr_preempted_render_;
  }

  // The fake DAC: the slot that has just finished playing must hold what it held when it started.
  if (played_count_ != 0 || tick_ != 0) {
    const std::uint32_t now = fnv(g_tx.samples + tx_playing_ * haudio::kBlockSamples);
    if (now != slot_checksum_at_start_) {
      ++torn_;
    }
  }
  ++tick_;
  tx_playing_ = (tx_playing_ + 1u) & (haudio::kTxPhysSlots - 1u);

  // The fake ADC completes RX block `tick_` (slot tick_ mod 4); the pipeline is told the slot being written next.
  const std::uint32_t completed_slot = tick_ & (haudio::kRxPhysSlots - 1u);
  const std::uint32_t writing_after = (tick_ + 1u) & (haudio::kRxPhysSlots - 1u);
  if (!config_.rx_after_tx) {
    fill_rx_block(g_rx.samples + completed_slot * haudio::kBlockSamples, tick_);
  }

  const std::uint32_t before = fake_ticks();
  pipeline_.on_boundary(tx_playing_, config_.rx_after_tx ? completed_slot : writing_after, stamp, 0);
  const std::uint32_t took = fake_ticks() - before;
  if (took > on_boundary_ticks_max_) {
    on_boundary_ticks_max_ = took;
  }
  if (config_.rx_after_tx) {
    fill_rx_block(g_rx.samples + completed_slot * haudio::kBlockSamples, tick_);
  }

  // The fake DAC records the slot that starts to play (after the ISR may have zeroed it).
  const std::int16_t* block = g_tx.samples + tx_playing_ * haudio::kBlockSamples;
  slot_checksum_at_start_ = fnv(block);
  if (played_count_ < kFakeMaxPlayed) {
    FakePlayed& p = played_[played_count_];
    p.seq = tick_;
    p.slot = tx_playing_;
    p.checksum = slot_checksum_at_start_;
    for (unsigned i = 0; i < 4; ++i) {
      p.head[i] = block[i];
    }
    if (played_count_ < kFakeFullBlocks) {
      for (std::uint32_t i = 0; i < haudio::kBlockSamples; ++i) {
        full_[played_count_][i] = block[i];
      }
    }
    ++played_count_;
  }

  // Pend the render line last: PendSV through the system control block, or IRQ 31 through NVIC_STIR (the MK66's path).
  in_isr_ = false;
  if (config_.render_via_stir) {
    reg(kNvicStir) = kStirIrq;
  } else {
    reg(kScbIcsr) = kIcsrPendSvSet;
  }
}

void FakeI2s::on_render() noexcept {
  if (in_isr_) {
    ++render_in_isr_;
  }
  render_ipsr_ = ipsr();
  render_active_ = true;
  pipeline_.render();
  render_active_ = false;
}

}  // namespace o3os::platform::qemu

namespace o3os::hal {
std::uint32_t cycles() noexcept { return platform::qemu::fake_ticks(); }
}  // namespace o3os::hal

extern "C" void TIMER0_IRQHandler() { o3os::platform::qemu::fake_i2s().on_tick(); }
extern "C" void PendSV_Handler() { o3os::platform::qemu::fake_i2s().on_render(); }
extern "C" void STIR_IRQHandler() { o3os::platform::qemu::fake_i2s().on_render(); }
