// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-005a on the Cortex-M4F: the audio engine skeleton driven by a fake I2S whose block clock is a timer interrupt
// (platform/qemu/fake_i2s.h). It runs the very pipeline the MK66 runs (audio/pipeline.h, audio/slot_ring.h) with the interrupt
// priorities of the ADR 0013 D1 table, and proves, with virtual time (`-icount`, so every run is exact and repeatable):
//   * the render never runs inside the DMA ISR: it runs on the render line (PendSV, exception 14) and the ISR preempts it;
//   * slot ownership over 10,000 blocks: every played block is exactly the block rendered for it, no slot is written while it
//     plays, in both phases of the RX completion;
//   * a render forced slow is an underrun the counters see, with the stale slot replayed; three in a row mute and recover;
//   * the test tone's block content against a double-precision oracle;
//   * the codec stays muted until unmute().
// QEMU has no DWT: the cycle figures are virtual-time ticks and no claim is made about them beyond "bounded".
#include <doctest/doctest.h>

#include <o3os/audio/engine.h>
#include <o3os/audio/pipeline.h>
#include <o3os/audio/test_signal.h>
#include <o3os/hal/audio.h>
#include <o3os/hal/irq.h>
#include <o3os/platform/qemu/fake_i2s.h>

#include "support/static_slot.h"

#include <cmath>
#include <cstdint>
#include <initializer_list>

namespace {

namespace q = o3os::platform::qemu;
namespace haudio = o3os::hal::audio;
using o3os::audio::StatusSnapshot;
constexpr std::uint32_t kUnmute = o3os::audio::SlotRing<2, 2>::kUnmuteAfter;  // on-time boundaries before the mute lifts

constexpr std::uint32_t kN = haudio::kTxSlots;
constexpr std::uint32_t kMaxCalls = 10'400;

std::uint32_t fnv(const std::int16_t* block) {
  std::uint32_t h = 2166136261u;
  for (std::uint32_t i = 0; i < haudio::kBlockSamples; ++i) {
    h = (h ^ static_cast<std::uint16_t>(block[i])) * 16777619u;
  }
  return h;
}

std::uint32_t zero_checksum() {
  alignas(8) std::int16_t zeros[haudio::kBlockSamples] = {};
  return fnv(zeros);
}

// Waits (idle, in WFI) until `target` boundaries have happened; bounded, so a dead block clock fails the test, not the timeout.
bool wait_boundaries(q::FakeI2s& fake, std::uint32_t target) {
  const std::uint32_t limit = 60u * target + 1000u;
  for (std::uint32_t i = 0; i < limit; ++i) {
    if (fake.pipeline().status().snapshot().boundaries >= target) {
      return true;
    }
    asm volatile("wfi");
  }
  return false;
}

// A render callback that tags its block with its call number, checks the input it was given, and can be made slow.
struct Tagger {
  std::uint32_t calls = 0;
  std::uint32_t slow_call = 0;      // 1-based call that spins, 0 = none
  std::uint32_t slow_ticks = 0;
  std::uint32_t silenced = 0;       // calls that were asked for silence
  std::uint32_t bad_ipsr = 0;       // calls that did not run on the render line (read from IPSR by the callback itself)
  std::uint32_t in_isr_calls = 0;   // calls that found the DMA ISR's flag raised: running inside it
  std::uint32_t rx_tag[kMaxCalls] = {};
  std::uint32_t seq_at[kMaxCalls] = {};
  std::uint32_t written[kMaxCalls] = {};
};
Tagger g_tag;

void tagger_render(void* user, const haudio::BlockIo& io) noexcept {
  Tagger& t = *static_cast<Tagger*>(user);
  q::FakeI2s& fake = q::fake_i2s();
  const std::uint32_t n = ++t.calls;
  std::uint32_t ipsr;
  asm volatile("mrs %0, ipsr" : "=r"(ipsr));
  if (ipsr != fake.expected_render_ipsr()) {  // the render line (PendSV or the STIR line): not thread mode, not the timer interrupt
    ++t.bad_ipsr;
  }
  if (fake.in_dma_isr()) {
    ++t.in_isr_calls;
  }
  if (n < kMaxCalls) {
    t.rx_tag[n] = q::FakeI2s::rx_tag(io.in);
    t.seq_at[n] = fake.pipeline().ring().seq();
  }
  if (n == t.slow_call) {
    q::fake_spin_ticks(t.slow_ticks);
  }
  if ((io.flags & haudio::kFlagSilence) != 0u) {
    ++t.silenced;
    for (std::uint32_t i = 0; i < haudio::kBlockSamples; ++i) {
      io.out[i] = 0;
    }
  } else {
    q::FakeI2s::fill_rx_block(io.out, n);  // the same tag layout as the fake ADC's: tag in samples 0/1, a pattern of it after
  }
  if (n < kMaxCalls) {
    t.written[n] = fnv(io.out);
  }
}

// Starts the fake with the tagger; the caller stops it.
q::FakeI2s& start_tagged(bool rx_after_tx, bool via_stir = false, std::uint32_t slow_call = 0, std::uint32_t slow_ticks = 0) {
  g_tag.calls = 0;
  g_tag.slow_call = slow_call;
  g_tag.slow_ticks = slow_ticks;
  g_tag.silenced = 0;
  g_tag.bad_ipsr = 0;
  g_tag.in_isr_calls = 0;
  q::FakeI2s& fake = q::fake_i2s();
  q::FakeI2s::Config config;
  config.rx_after_tx = rx_after_tx;
  config.render_via_stir = via_stir;
  fake.configure(config);
  const haudio::BlockCallback cb{&tagger_render, &g_tag};
  CHECK(fake.start(cb) == haudio::Device::Result::Ok);
  return fake;
}

}  // namespace

TEST_CASE("fake i2s: start() mutes first and never unmutes, unmute() opens the amplifier last") {
  static o3os::test::StaticSlot<o3os::audio::Engine> engine_slot;  // 14 KiB: not on the target's stack
  static o3os::audio::TapMem taps;                                  // the taps' memory (22 KiB), static like the image's SRAM_L area
  o3os::audio::Engine& engine = engine_slot.make(&q::fake_ticks, taps);
  q::FakeI2s& fake = q::fake_i2s();
  fake.configure(q::FakeI2s::Config{});
  CHECK(fake.start(engine.callback()) == haudio::Device::Result::Ok);
  CHECK(fake.start(engine.callback()) == haudio::Device::Result::AlreadyStarted);
  CHECK_FALSE(fake.outputs_enabled());  // start() did not unmute
  CHECK(wait_boundaries(fake, 20));
  CHECK_FALSE(fake.outputs_enabled());  // and 20 blocks of running did not either
  CHECK(fake.unmute());
  CHECK(fake.outputs_enabled());
  fake.mute();
  CHECK_FALSE(fake.outputs_enabled());
  fake.stop();

  q::FakeEvent events[16];
  const std::uint32_t count = fake.events(events, 16);
  CHECK_MESSAGE(count >= 5u, "the fake logs its calls");
  if (count < 5u) {
    return;  // no REQUIRE in a no-exceptions build
  }
  CHECK(events[0] == q::FakeEvent::AmpMuted);   // H16: muted before anything else
  CHECK(events[1] == q::FakeEvent::StartBegin);
  CHECK(events[2] == q::FakeEvent::ClockRunning);
  CHECK(events[3] == q::FakeEvent::UnmuteBegin);
  CHECK(events[4] == q::FakeEvent::AmpOpen);    // the amplifier key is the last step of the unmute
}

TEST_CASE("fake i2s: the render runs on the render line, never in the DMA ISR; the ISR is bounded") {
  for (const bool via_stir : {false, true}) {
    INFO("render line: " << (via_stir ? "IRQ 31 pended through NVIC_STIR (the MK66's path)" : "PendSV"));
    q::FakeI2s& fake = start_tagged(false, via_stir);
    CHECK(wait_boundaries(fake, 300));
    fake.stop();
    CHECK(g_tag.calls >= 299u);
    CHECK(g_tag.bad_ipsr == 0u);                 // every render ran on the render line (PendSV 14, or IRQ 31 = 47), none in thread mode or the timer ISR
    CHECK(g_tag.in_isr_calls == 0u);             // none saw the DMA ISR's flag up
    CHECK(fake.render_in_isr() == 0u);
    CHECK(fake.render_ipsr() == fake.expected_render_ipsr());
    // Priorities: the table the fake programs is the ADR's.
    CHECK(o3os::hal::irq::priority(o3os::hal::irq::Role::AudioDma) < o3os::hal::irq::priority(o3os::hal::irq::Role::Render));
    // The slot-boundary entry is small: under 2 % of the block period (virtual-time ticks), however the QEMU core is modelled.
    CHECK(fake.on_boundary_ticks_max() > 0u);
    CHECK(fake.on_boundary_ticks_max() <= fake.block_ticks() / 50u);
  }
}

TEST_CASE("fake i2s: slot ownership over 10,000 blocks, both RX phases, both render lines") {
  const std::uint32_t zero = zero_checksum();
  for (const unsigned variant : {0u, 1u, 2u, 3u}) {
    const bool rx_after_tx = (variant & 1u) != 0u;
    const bool via_stir = (variant & 2u) != 0u;
    INFO("rx_after_tx " << rx_after_tx << ", render line " << (via_stir ? "IRQ 31 via NVIC_STIR" : "PendSV"));
    q::FakeI2s& fake = start_tagged(rx_after_tx, via_stir);
    constexpr std::uint32_t kBlocks = 10'000;
    CHECK(wait_boundaries(fake, kBlocks + 20));
    fake.stop();

    const StatusSnapshot s = fake.pipeline().status().snapshot();
    CHECK(s.boundaries >= kBlocks + 20u);
    CHECK(s.underruns == 0u);
    CHECK(s.late_drops == 0u);
    CHECK(s.slot_slips == 0u);
    CHECK(s.governor_over == 0u);
    CHECK(s.mute_events == 0u);
    CHECK(fake.torn() == 0u);                  // no slot was written while it played
    CHECK(fake.render_in_isr() == 0u);
    CHECK(g_tag.bad_ipsr == 0u);
    CHECK(g_tag.in_isr_calls == 0u);
    CHECK(g_tag.calls >= kBlocks + 19u);       // one render per boundary
    CHECK(fake.played_count() >= kBlocks);

    // Every boundary played exactly the block rendered for it: boundary s plays the block of call s - N + 1, and the first N - 1
    // boundaries play the zeroed ring. A torn, repeated, skipped or foreign block would differ in tag or checksum.
    std::uint32_t bad = 0;
    std::uint32_t first_bad = 0;
    for (std::uint32_t b = 1; b <= kBlocks; ++b) {
      const q::FakePlayed& p = fake.played(b - 1);
      bool ok = p.seq == b;
      if (b < kN) {
        ok = ok && p.checksum == zero;
      } else {
        const std::uint32_t call = b - kN + 1;
        const std::uint32_t tag = static_cast<std::uint16_t>(p.head[0]) | (static_cast<std::uint32_t>(static_cast<std::uint16_t>(p.head[1])) << 16);
        ok = ok && tag == call && p.checksum == g_tag.written[call] && g_tag.seq_at[call] == call;
      }
      if (!ok && bad++ == 0) {
        first_bad = b;
      }
    }
    CHECK_MESSAGE(bad == 0u, "first bad boundary " << first_bad);

    // The render reads the newest complete RX block: the one completed at this boundary, or at the previous one when the RX
    // boundary falls just after the TX boundary. Never a half-written block, never an older one.
    std::uint32_t rx_bad = 0;
    for (std::uint32_t call = 3; call <= kBlocks; ++call) {
      const std::uint32_t want = rx_after_tx ? call - 1u : call;
      rx_bad += g_tag.rx_tag[call] == want ? 0u : 1u;
    }
    CHECK(rx_bad == 0u);
  }
}

TEST_CASE("fake i2s: a render forced to take 1.5 blocks is one underrun with two slots and none with three") {
  for (const bool via_stir : {false, true}) {
    INFO("render line: " << (via_stir ? "IRQ 31 pended through NVIC_STIR (the MK66's path)" : "PendSV"));
    constexpr std::uint32_t kSlowCall = 50;
    constexpr std::uint32_t kLate = kN == 2u ? 1u : 0u;  // its slot is due 1 block after its boundary with two slots, 2 with three
    q::FakeI2s& fake = start_tagged(false, via_stir, kSlowCall, (q::kFakeBlockTicks * 3u) / 2u);
    CHECK(wait_boundaries(fake, 200));
    fake.stop();

    const StatusSnapshot s = fake.pipeline().status().snapshot();
    CHECK(s.underruns == kLate);
    CHECK(s.late_drops == kLate);
    CHECK(s.mute_events == 0u);
    CHECK(s.slot_slips == 0u);
    CHECK(fake.torn() == 0u);
    CHECK(g_tag.silenced == 0u);
    // With three slots the slow block is written in time: every boundary plays the block of call b - kN + 1. With two, the
    // boundary whose block was dropped replays its stale slot.
    std::uint32_t wrong = 0;
    for (std::uint32_t b = kN; b <= 150; ++b) {
      const q::FakePlayed& p = fake.played(b - 1);
      const std::uint32_t tag = static_cast<std::uint16_t>(p.head[0]) | (static_cast<std::uint32_t>(static_cast<std::uint16_t>(p.head[1])) << 16);
      if (kLate == 0u || b != kSlowCall + kN - 1u) {
        wrong += tag == b - kN + 1u ? 0u : 1u;
      }
    }
    CHECK(wrong == 0u);
  }
}

TEST_CASE("fake i2s: one render forced to take kN - 1/2 blocks is kN - 1 underruns in a row, stale slots are replayed, nothing else is lost") {
  // The block of boundary s is due at boundary s + kN - 1. A render that takes (kN - 1/2) blocks misses its own slot (dropped,
  // never written) and, because the render serves the newest boundary when it gets going again, kN - 2 blocks after it are
  // never rendered either: kN - 1 underruns in a row, below the three that mute.
  static_assert(kN == 2u || kN == 3u, "the script below covers two and three slots");
  for (const bool via_stir : {false, true}) {
    INFO("render line: " << (via_stir ? "IRQ 31 pended through NVIC_STIR (the MK66's path)" : "PendSV"));
    constexpr std::uint32_t kSlowCall = 50;
    q::FakeI2s& fake = start_tagged(false, via_stir, kSlowCall, (q::kFakeBlockTicks * (2u * kN - 1u)) / 2u);
    CHECK(wait_boundaries(fake, 200));
    fake.stop();

    const StatusSnapshot s = fake.pipeline().status().snapshot();
    CHECK(s.underruns == kN - 1u);               // the blocks due while the slow render was running
    CHECK(s.max_consecutive_underruns == kN - 1u);
    CHECK(s.late_drops == 1u);                   // the slow render finished after its slot started to play: dropped, not written
    CHECK(s.mute_events == 0u);
    CHECK(s.muted == 0u);
    CHECK(s.slot_slips == 0u);
    CHECK(fake.torn() == 0u);                    // the late block was NOT written into the playing slot
    CHECK(fake.isr_preempted_render() >= 1u);    // the boundary ISR preempted the render: it is above it
    CHECK(fake.on_boundary_ticks_max() <= fake.block_ticks() / 50u);
    CHECK(g_tag.silenced == 0u);

    // Boundary b plays the block of boundary rb = b - kN + 1, which is call rb before the slow call. Boundaries kSlowCall ..
    // kSlowCall + kN - 2 were not rendered (call kSlowCall is never heard, the next kN - 2 have no call), so the kN - 1 boundaries
    // from kSlowCall + kN - 1 replay what their slot held: the block of boundary b - kTxPhysSlots (call b - kTxPhysSlots - kN + 1).
    // After them the calls are kN - 2 behind the boundaries (the skipped ones).
    std::uint32_t wrong = 0;
    std::uint32_t wrong_boundary = 0;
    for (std::uint32_t b = kN; b <= 150; ++b) {
      const q::FakePlayed& p = fake.played(b - 1);
      const std::uint32_t tag = static_cast<std::uint16_t>(p.head[0]) | (static_cast<std::uint32_t>(static_cast<std::uint16_t>(p.head[1])) << 16);
      const std::uint32_t rb = b - kN + 1u;
      std::uint32_t want = rb;
      if (rb >= kSlowCall && rb <= kSlowCall + kN - 2u) {
        want = b - haudio::kTxPhysSlots - kN + 1u;  // the stale replay
      } else if (rb > kSlowCall + kN - 2u) {
        want = rb - (kN - 2u);
      }
      if (tag != want && wrong++ == 0) {
        wrong_boundary = b;
      }
    }
    CHECK_MESSAGE(wrong == 0u, "first unexpected block at boundary " << wrong_boundary);
  }
}

TEST_CASE("fake i2s: three underruns in a row mute, the stale slot is zeroed, and the mute lifts after kUnmuteAfter on-time blocks") {
  for (const bool via_stir : {false, true}) {
    INFO("render line: " << (via_stir ? "IRQ 31 pended through NVIC_STIR (the MK66's path)" : "PendSV"));
    constexpr std::uint32_t kSlowCall = 40;
    q::FakeI2s& fake = start_tagged(false, via_stir, kSlowCall, (q::kFakeBlockTicks * 7u) / 2u);  // 3.5 blocks: three boundaries pass inside it
    CHECK(wait_boundaries(fake, 500));
    fake.stop();

    const StatusSnapshot s = fake.pipeline().status().snapshot();
    CHECK(s.underruns == 3u);
    CHECK(s.max_consecutive_underruns == 3u);
    CHECK(s.consecutive_underruns == 0u);        // the run ended when the pending render got its block in
    CHECK(s.mute_events == 1u);
    CHECK(s.muted == 0u);                        // lifted again
    CHECK(s.late_drops == 1u);
    CHECK(s.slot_slips == 0u);
    CHECK(fake.torn() == 0u);

    // The render is told to write silence for exactly kUnmuteAfter boundaries: from the one at which the mute started up to the one before
    // the mute lifted.
    CHECK(g_tag.silenced == kUnmute);
    // The boundary at which the third underrun muted zeroed its stale slot: the DAC heard digital silence there, and through the
    // silent blocks that followed, then the tone of tags resumed. Boundary b3 is the third one after the slow call's boundary.
    // The three boundaries kSlowCall + kN - 1 .. kSlowCall + kN + 1 are the underruns (the render resumes with boundary
    // kSlowCall + 3), so the third, b3, is kSlowCall + kN + 1 (3 with two slots, 4 with three). The blocks rendered before the
    // mute (boundaries up to b3 - 1) play up to b3 + kN - 2, then the silent ones.
    static_assert(kN == 2u || kN == 3u, "the script below covers two and three slots");
    const std::uint32_t b3 = kSlowCall + kN + 1u;
    const std::uint32_t zero = zero_checksum();
    CHECK_MESSAGE(fake.played(b3 - 1).checksum == zero, "boundary " << b3 << " (the ISR zeroed its stale slot) should play silence");
    for (std::uint32_t b = b3 + kN - 1u; b <= b3 + kUnmute + kN - 2u; ++b) {
      CHECK_MESSAGE(fake.played(b - 1).checksum == zero, "boundary " << b << " should play silence");
    }
    // The next boundary plays the first block rendered after the mute lifted: the call that served boundary b3 + kUnmute.
    std::uint32_t call = 0;
    for (std::uint32_t c = 1; c < kMaxCalls && c <= g_tag.calls; ++c) {
      if (g_tag.seq_at[c] == b3 + kUnmute) {
        call = c;
        break;
      }
    }
    CHECK(call != 0u);
    const q::FakePlayed& resumed = fake.played(b3 + kUnmute + kN - 1u - 1u);  // boundary b3 + kUnmute + kN - 1 plays its block
    CHECK(resumed.checksum != zero);
    CHECK(resumed.checksum == g_tag.written[call]);
    CHECK(fake.isr_preempted_render() >= 1u);
  }
}

TEST_CASE("fake i2s: the test tone against a double-precision oracle, left alone first and then the right at 1.5 kHz") {
  static o3os::test::StaticSlot<o3os::audio::Engine> engine_slot;  // 14 KiB: not on the target's stack
  static o3os::audio::TapMem taps;                                  // the taps' memory (22 KiB), static like the image's SRAM_L area
  o3os::audio::Engine& engine = engine_slot.make(&q::fake_ticks, taps);
  constexpr std::uint32_t kLeftOnly = 24;
  engine.set_left_only_blocks(kLeftOnly);
  engine.set_tone(true);
  q::FakeI2s& fake = q::fake_i2s();
  // The fake's default block is 20,000 timer ticks, a tenth of the instructions a real block holds relative to the work in it; the graph's render
  // (two test sources through level, pan, routing, the meters and the taps) is about 15,700 ticks here, which would not fit 60 % of that. A block of
  // five times the length keeps the check that the render fits the governor's budget (`budget_cycles()` follows `block_ticks`).
  q::FakeI2s::Config config;
  config.block_ticks = 5u * q::kFakeBlockTicks;
  fake.configure(config);
  CHECK(fake.start(engine.callback()) == haudio::Device::Result::Ok);
  CHECK(wait_boundaries(fake, q::kFakeFullBlocks + 4u));
  fake.stop();

  const StatusSnapshot s = fake.pipeline().status().snapshot();
  CHECK(s.underruns == 0u);
  CHECK(s.late_drops == 0u);
  CHECK(s.governor_over == 0u);
  CHECK(s.render_max > 0u);
  CHECK(s.render_max <= fake.pipeline().budget_cycles());  // inside the 60 % governor budget
  CHECK(s.stage_max[haudio::kStageRender] > 0u);
  CHECK(s.stage_max[haudio::kStageConvert] > 0u);
  CHECK(s.input_peak > 0u);                                 // the RX ring's tag pattern reached the engine's input meter

  // The independent oracle: phase increments from the rate constant by double arithmetic, gain by the ramp definition, sin() from
  // the C library, rounded half away from zero.
  const double cycles_per_sample = static_cast<double>(haudio::kCyclesPerSample.num) / static_cast<double>(haudio::kCyclesPerSample.den);
  const double inc_l_d = std::floor(4294967296.0 * 1000.0 * cycles_per_sample / static_cast<double>(haudio::kCpuHz) + 0.5);
  const double inc_r_d = std::floor(4294967296.0 * 1500.0 * cycles_per_sample / static_cast<double>(haudio::kCpuHz) + 0.5);
  CHECK(static_cast<std::uint32_t>(inc_l_d) == o3os::audio::tone_phase_inc_q32(o3os::audio::kToneHzLeft));
  CHECK(static_cast<std::uint32_t>(inc_r_d) == o3os::audio::tone_phase_inc_q32(o3os::audio::kToneHzRight));
  const std::uint64_t inc_l = static_cast<std::uint64_t>(inc_l_d);
  const std::uint64_t inc_r = static_cast<std::uint64_t>(inc_r_d);
  const double amp = static_cast<double>(o3os::audio::kToneAmplitude);
  const std::uint32_t ramp = o3os::audio::kToneRampSamples;
  const double pi2 = 6.283185307179586476925287;
  auto expect = [&](std::uint64_t n_ramp, std::uint64_t n_phase, std::uint64_t inc) {
    const std::uint64_t pos = n_ramp + 1u < ramp ? n_ramp + 1u : ramp;
    const double gain = pos >= ramp ? amp : static_cast<double>(pos) * (amp / static_cast<double>(ramp));
    const double turns = static_cast<double>((n_phase * inc) & 0xFFFFFFFFull) / 4294967296.0;
    const double v = gain * std::sin(pi2 * turns) * 32768.0;
    return static_cast<std::int32_t>(v >= 0.0 ? v + 0.5 : v - 0.5);
  };
  std::int32_t worst = 0;
  std::uint32_t compared = 0;
  std::uint32_t right_silent = 0;
  std::uint32_t right_sounds = 0;
  for (std::uint32_t b = kN; b <= q::kFakeFullBlocks; ++b) {
    const std::int16_t* block = fake.full_block(b - 1);
    const std::uint32_t j = b - kN;  // call index, 0-based: boundary b plays the block rendered for boundary b - N + 1
    for (std::uint32_t i = 0; i < haudio::kBlock; ++i) {
      const std::uint64_t n = static_cast<std::uint64_t>(j) * haudio::kBlock + i;
      const std::int32_t want_l = expect(n, n, inc_l);
      std::int32_t want_r = 0;
      if (j >= kLeftOnly) {
        want_r = expect(n - static_cast<std::uint64_t>(kLeftOnly) * haudio::kBlock, n, inc_r);
        right_sounds += block[2u * i + 1u] != 0 ? 1u : 0u;
      } else {
        right_silent += block[2u * i + 1u] == 0 ? 1u : 0u;
      }
      const std::int32_t dl = block[2u * i] - want_l;
      const std::int32_t dr = block[2u * i + 1u] - want_r;
      const std::int32_t adl = dl < 0 ? -dl : dl;
      const std::int32_t adr = dr < 0 ? -dr : dr;
      worst = adl > worst ? adl : worst;
      worst = adr > worst ? adr : worst;
      ++compared;
    }
  }
  CHECK(compared == (q::kFakeFullBlocks - kN + 1u) * haudio::kBlock);
  CHECK_MESSAGE(worst <= 1, "worst deviation from the oracle: " << worst << " LSB");
  CHECK(right_silent == (kLeftOnly) * haudio::kBlock);   // the right channel exactly silent through the left-only phase
  CHECK(right_sounds > 10u * haudio::kBlock);            // and sounding after it
  // The first N - 1 played blocks are the zeroed ring.
  for (std::uint32_t b = 1; b < kN; ++b) {
    CHECK(fake.played(b - 1).checksum == zero_checksum());
  }
}

// ---- OS-MAINGUARD: the main loop starves under a render that takes the whole block ------------------------------------------------------
namespace {

struct GuardRun {
  std::uint32_t passes;
  std::uint32_t shed;
  std::uint32_t shed_runs;
  std::uint32_t longest;
  std::uint32_t underruns;
  std::uint32_t mutes;
};

struct Burn {
  std::uint32_t ticks;
  std::uint32_t target;      // boundaries
  volatile bool done;
};
Burn g_burn;
constexpr std::uint32_t kWindow = 8;

void burn_render(void* user, const haudio::BlockIo& io) noexcept {
  Burn& b = *static_cast<Burn*>(user);
  q::fake_spin_ticks(b.ticks);
  for (std::uint32_t i = 0; i < haudio::kBlockSamples; ++i) {
    io.out[i] = 0;
  }
  q::FakeI2s& fake = q::fake_i2s();
  if (fake.pipeline().status().snapshot().boundaries >= b.target) {  // with the guard off the thread never runs again: end the run from here
    fake.stop();
    b.done = true;
  }
}

// `ticks` of main-loop CPU time: the free-running timer also counts the time a render preempted the loop, so a gap between two reads
// is not counted (a read to read step in the loop itself is a few ticks).
void thread_work(std::uint32_t ticks) {
  std::uint32_t last = q::fake_ticks();
  std::uint32_t done = 0;
  while (done < ticks) {
    const std::uint32_t now = q::fake_ticks();
    const std::uint32_t d = now - last;
    last = now;
    if (d < 200u) {
      done += d;
    }
  }
}

// One run: `burn_pct` percent of a block per render, a main pass of `pass_pct` percent of a block, `target` boundaries.
GuardRun run_guard(bool guard, std::uint32_t burn_pct, std::uint32_t pass_pct, std::uint32_t target) {
  g_burn.ticks = q::kFakeBlockTicks / 100u * burn_pct;
  g_burn.target = target;
  g_burn.done = false;
  q::FakeI2s& fake = q::fake_i2s();
  q::FakeI2s::Config config;
  config.starve_guard = guard;
  fake.configure(config);
  fake.pipeline().set_starve_blocks(kWindow);  // the test's window, not the default (64): the shares below are for 8 in 800 blocks
  const haudio::BlockCallback cb{&burn_render, &g_burn};
  CHECK(fake.start(cb) == haudio::Device::Result::Ok);
  const std::uint32_t pass_ticks = q::kFakeBlockTicks / 100u * pass_pct;
  GuardRun r{};
  while (!g_burn.done && fake.pipeline().status().snapshot().boundaries < target) {
    thread_work(pass_ticks);
    fake.pipeline().main_pass();
    ++r.passes;
  }
  fake.stop();
  const StatusSnapshot s = fake.pipeline().status().snapshot();
  r.shed = fake.pipeline().shed_blocks();
  r.shed_runs = fake.pipeline().shed_runs();
  r.longest = fake.pipeline().shed_longest();
  r.underruns = s.underruns;
  r.mutes = s.mute_events;
  return r;
}

}  // namespace

TEST_CASE("fake i2s: the main-loop guard keeps the loop alive under a render that takes the whole block, and costs nothing when the loop is fed") {
  constexpr std::uint32_t kTarget = 800;
  // The fake's ISR, the render's copies and its status take about 26 % of a block in virtual time (measured: a 30 % callback leaves the
  // loop 44 %), so a callback of 80 % is a render that takes the whole block.
  constexpr std::uint32_t kBurnWhole = 80;
  // Without the guard such a render never lets the thread run (it passes at most a handful of times).
  const GuardRun off = run_guard(false, kBurnWhole, 50, kTarget);
  MESSAGE("guard off: passes " << off.passes << " shed " << off.shed << " underruns " << off.underruns);
  CHECK(off.passes <= 5u);
  CHECK(off.shed == 0u);

  // With it the loop passes, the render sheds in runs of about one block per half-block of pass, and nothing underruns or mutes.
  // The pass cost is 0.5 and 2.5 blocks; the share is about ceil(P) / (kWindow + ceil(P)): 11 % and 27 % (measured 7 to 11 % and 26 to 28 %).
  const std::uint32_t pass_pct[2] = {50, 250};
  const std::uint32_t max_share[2] = {20, 40};  // percent of the blocks
  for (unsigned c = 0; c < 2u; ++c) {
    const GuardRun on = run_guard(true, kBurnWhole, pass_pct[c], kTarget);
    MESSAGE("guard on, pass " << pass_pct[c] << " % of a block: passes " << on.passes << " shed " << on.shed << " in " << on.shed_runs
                              << " runs, longest " << on.longest << ", underruns " << on.underruns << ", mutes " << on.mutes);
    CHECK(on.passes >= kTarget / 20u);
    CHECK(on.shed > 0u);
    CHECK(on.shed * 100u <= kTarget * max_share[c]);
    CHECK(on.underruns == 0u);
    CHECK(on.mutes == 0u);
  }

  // A light render (30 %) leaves the loop its time: the guard never sheds.
  const GuardRun light = run_guard(true, 30, 50, kTarget);
  MESSAGE("guard on, render 30 %: passes " << light.passes << " shed " << light.shed << " underruns " << light.underruns);
  CHECK(light.shed == 0u);
  CHECK(light.passes >= kTarget / 2u);
  CHECK(light.underruns == 0u);
}
