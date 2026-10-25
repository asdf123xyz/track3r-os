// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The master limiter (OS-LIMIT; ADR 0013 D4: the `limiter` stage of kMasterChain, BASE-10's always-on base limiter, PAR-025 / PAR-058's controls). NEW COMMUNITY OS BEHAVIOUR: the control set follows
// the limiter fields of the OG project file and the release notes (threshold, attack, release, the LIMITER MODE and the master LIMITER switch: graph_params.h `limiter`), the SOUND is ours. No value below is measured on an official
// unit ("new OS behaviour, not measured"); a measurement on an official unit, if ever done (open research question), may replace it.
//
// WHY. The unit clipped its master (hardware session, 0.4.5, t-v16, 45 s: 6,743 / 6,832 full-scale samples, 243 / 227 clipped meter windows): nothing limited the MAIN sum, and the 1.2+ instrument
// level law adds about 6 dB. The chain's int16 conversion saturates, so what reached the codec was a hard clip. This stage makes that impossible.
//
// TWO STAGES, one stereo-linked gain (the larger of |L| and |R| decides, so the image does not move):
//
//   1. THE FAST STAGE, the base limiter, ALWAYS ON, a look-ahead brickwall. Peak detection over the last kBlock = 64 samples (1.45 ms at 44,100 Hz) by a sliding maximum; the gain that
//      brings that peak to the ceiling kCeiling (-0.3 dBFS) is smoothed by a moving average of the same 64 samples (the attack: a ramp that ends as the peak leaves the delay line) and
//      released exponentially (kBrickRelease[mode]). The signal is delayed by kLatency = 63 samples, so the gain is already down when the peak arrives: |out| <= kCeiling exactly, whatever
//      the input, the controls and the history (a proof sketch is at the loop in master_limiter.cpp, and tests/audio_master_limiter_test.cpp drives +12 dB sines, steps, impulses and noise
//      against it). This is the "final hard clamp so int16 never wraps": it holds before the int16 conversion, whose saturation (kernel::float_to_q15) is the last, never needed, safety net.
//   2. THE SLOW STAGE, the three controls, ON only with the master LIMITER switch. A peak follower with the ATTACK and RELEASE times (a one-pole in each direction, no look-ahead, so a
//      transient leaks past it by design) and the gain THRESHOLD / follower when the follower is above the threshold. It sits in front of the fast stage, which sees its result and
//      catches what leaks. With the switch OFF the threshold is out of reach and this stage is the identity: the base limiter alone is left. Under the threshold the whole limiter is the identity
//      (gain 1.0 exactly: the float planes come out bit for bit, delayed by kLatency).
//
// THE VALUES (new OS behaviour, not measured):
//   THRESHOLD   1..100 % of full scale (linear), default 92.      ATTACK, RELEASE   1..1000 ms, the 10 % to 90 % time of the follower, default 10 and 100.
//   LIMITER MODE (the project's) selects a preset:
//                 slow stage's times         fast stage's release (10..90 %)
//     Subtle      x 1                        100 ms
//     Extreme     x 0.25 (four times faster) 30 ms   (tighter, more audible: a shorter release lets the fast stage pump more)
//   The ceiling kCeiling is fixed; a threshold above it (the default 92 % is below it) is a slow stage that waits for the ceiling.
//
// Real time: float only, no heap, no libm, no double, loops bounded by kBlock (one forward loop over the block and one backward over its peaks), the coefficients computed once per block and only
// when a control changed (set_param runs in the render context, from the graph's drain after the block's voices have rendered, or before start), denormals impossible (the follower is flushed to 0 below kEnvFloor, the
// gains stay in [1/64, 1], the sliding sum is an integer). State is about 1 KiB, zero until reset() (so the object is .bss). Needs finite input (the graph's planes are an int32 sum / 2^19). It runs
// from flash, called through the graph's MasterFn pointer like the send effects (the SRAM_L hot budget has no room), about 4k cycles a block by the estimate in tests/bench/audio_bench.cpp.

#include <o3os/audio/graph_params.h>
#include <o3os/audio/meters.h>
#include <o3os/core/audio_clock.h>
#include <o3os/hal/audio.h>

#include <atomic>
#include <cstdint>

namespace o3os::audio {

// What the limiter publishes for `diag mix` and a gain-reduction readout: plain words the render writes once per meter window (kMeterWindowBlocks blocks), read for display from any
// context (each word is a relaxed atomic; the four of them may be one window apart; nothing decides on them). The gain is Q16 (65536 = unity, no reduction).
struct LimiterReport {
  std::uint32_t windows;       // windows published since reset
  std::uint32_t reduced;       // ... in which the gain went below kReducedBelow
  std::uint32_t gain_now_q16;  // the lowest gain of the last window (65536 before the first)
  std::uint32_t gain_hold_q16; // the lowest gain of any window since reset
  std::uint32_t mode;          // the LIMITER MODE in force: 0 Subtle, 1 Extreme
};

class MasterLimiter {
 public:
  static constexpr std::uint32_t kBlock = hal::audio::kBlock;
  static constexpr std::uint32_t kLatency = kBlock - 1u;     // samples of delay: sample n of the output is sample n - kLatency of the input
  static constexpr float kCeiling = 0.96605088f;             // -0.3 dBFS: 10^(-0.3 / 20)
  static constexpr float kGainCeiling = kCeiling * 0.9999997f;  // the gain is made for this, 0.3 ppm lower: the three float roundings of the path (6e-8 each) can then never put a sample above kCeiling
  static constexpr float kReducedBelow = 0.98861f;           // a window whose lowest gain was under -0.1 dB counts as "reduced"
  static constexpr float kEnvFloor = 1.0e-6f;                // the follower is zero below this (a thirtieth of an LSB)
  static constexpr float kSnapGain = 0.9999f;                // the fast stage's release lands on exactly 1.0 from here (a float stuck a hair below 1 would never be transparent again)
  static constexpr float kRateHz = core::kRefRateHz;         // the engine's rate (core/audio_clock.h)
  static constexpr float kMinTimeMs = 0.25f;                 // the fastest time the Extreme preset scales to

  // The presets of the LIMITER MODE (graph_params.h limiter::Mode): the slow stage's time scale, the fast stage's release in ms. New OS behaviour, not measured.
  static constexpr float kSlowScale[2] = {1.0f, 0.25f};
  static constexpr float kBrickReleaseMs[2] = {100.0f, 30.0f};

  MasterLimiter() noexcept = default;
  MasterLimiter(const MasterLimiter&) = delete;
  MasterLimiter& operator=(const MasterLimiter&) = delete;

  // Before start (or with the render stopped): every control back at its default (limiter:: constants; switch OFF, mode Subtle), the state cleared, the report zeroed.
  void reset() noexcept;
  // OS-WAVEXP: the signal in the limiter (the look-ahead delay, the envelope, the gain) back to silence and unity, with every control as it is. With the render stopped (the export takes
  // the engine and gives it back with the limiter empty: no residue of the last block of an export at the start of the next live block, and every pass of an export from the same state).
  void flush() noexcept;

  // A control (limiter:: field numbers); false when it is not one or the value is out of range (nothing changes). Render context, or before start.
  bool set_param(unsigned field, float value) noexcept;

  // One block of exactly kBlock frames, in place, as float planes (+-1.0 = full scale; they may exceed it). Any other n is left untouched. The graph's MasterFn is process_fx.
  void process(float* left, float* right, std::uint32_t n) noexcept;
  static void process_fx(void* self, float* left, float* right, std::uint32_t n) noexcept { static_cast<MasterLimiter*>(self)->process(left, right, n); }
  static bool param_fx(void* self, unsigned field, float value) noexcept { return static_cast<MasterLimiter*>(self)->set_param(field, value); }

  // Any context (relaxed reads).
  LimiterReport report() const noexcept {
    return LimiterReport{pub_windows_.load(std::memory_order_relaxed), pub_reduced_.load(std::memory_order_relaxed), pub_now_.load(std::memory_order_relaxed),
                         pub_hold_.load(std::memory_order_relaxed), pub_mode_.load(std::memory_order_relaxed)};
  }

  // For tests: the follower, the fast stage's gain after the last sample, the controls as told, and whether every state word is a finite number.
  float envelope() const noexcept { return env_; }
  float gain() const noexcept { return gp_; }
  bool switched_on() const noexcept { return on_; }
  unsigned mode() const noexcept { return mode_; }
  bool finite() const noexcept;

 private:
  void prepare() noexcept;

  // The delay line: [channel][sample], the slow stage's result. Sample k of a block is written after the sample the output of k reads (k + 1 of the block before, or this block's 0 for the last).
  float ring_[2][kBlock] = {};
  // The peaks of the slow stage's result: this block's up to the sample being done, the suffix maxima of the block before from the one after it on; [kBlock] is a zero guard so that the last
  // sample's window needs no case. After the loop it is turned into this block's suffix maxima, which are the next block's window tails.
  float sfx_[kBlock + 1u] = {};
  std::uint32_t hist_[kBlock] = {};  // the fast stage's gain per sample, the last kBlock of them, Q24 (a moving average over them is the attack)
  std::uint32_t sum_ = 0;            // their sum: exact, so no drift and 1.0 exactly when every one is unity
  float env_ = 0.0f;                 // the slow stage's follower
  float gp_ = 0.0f;                  // the fast stage's gain in force (0 until reset(): set to 1)

  // The controls as told.
  float thr_pct_ = 0.0f;
  float att_ms_ = 0.0f;
  float rel_ms_ = 0.0f;
  std::uint8_t mode_ = 0;
  bool on_ = false;
  bool dirty_ = false;
  // Derived at the start of a block when a control changed: the slow stage's threshold and one-pole steps, the fast stage's release step.
  float ts_ = 0.0f;
  float ca_ = 0.0f;
  float cr_ = 0.0f;
  float cb_ = 0.0f;

  // The report: the window in progress (render only) and the published words.
  std::uint32_t win_blocks_ = 0;
  float win_min_ = 0.0f;
  std::uint32_t windows_ = 0;
  std::uint32_t reduced_ = 0;
  std::uint32_t hold_ = 0;
  std::atomic<std::uint32_t> pub_windows_{0};
  std::atomic<std::uint32_t> pub_reduced_{0};
  std::atomic<std::uint32_t> pub_now_{0};
  std::atomic<std::uint32_t> pub_hold_{0};
  std::atomic<std::uint32_t> pub_mode_{0};  // the mode, written by set_param (render context) for the report
};

}  // namespace o3os::audio
