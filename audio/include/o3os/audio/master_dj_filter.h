// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The master DJ filter (OS-DJ A2, docs/specs/OS-DJ.md; the `bass` stage of kMasterChain; NEW COMMUNITY OS BEHAVIOUR, the sound is ours, nothing is measured on an official unit).
//
// ONE KNOB, BIPOLAR: -100..+100 per cent. Left of 0 is a low-pass whose cutoff falls as the knob goes further (about 21 kHz, which is open, down to 60 Hz); right of 0 is a high-pass whose cutoff
// rises (10 Hz, open, up to 8 kHz). 0 is BYPASSED: the stage is off in the chain (Graph::set_master_bypass), costs nothing, and the bus is bit for bit what it was. The two halves meet at 0
// with both filters open, so the morph is continuous. One stereo state-variable filter (the topology-preserving form,
// stable at any cutoff, robust to a cutoff that moves), float only, two states a channel; k = 2 (Q 0.5, critically damped): no resonance, so no sine is lifted, and the low-pass has no overshoot
// while its cutoff is under about a quarter of the sample rate (knob -40 and beyond). A step or edge at the ceiling (-0.3 dBFS) stays at or under full scale there
// (tests/audio_dj_filter_test.cpp). Wider open (knob -1 to -30, 4 to 31 %) the bilinear filter rings by itself, up to 31 % over an edge at the ceiling (the 1.3 filter did the same, more): the stage sits AHEAD of the limiter
// (kMasterChain; spec section 9, A3c), which catches that. The price of k = 2:
// the response at +-1 is not flat to the end, -0.2 dB at 15 kHz and -2.5 dB at 20 kHz on the low-pass side, -1 dB at 30 Hz on the high-pass side (flat within 0.05 dB from 150 Hz to 10 kHz).
//
// NO CLICKS. The knob is smoothed by a linear glide in blocks (the ParamChange's ramp, at least one block of 64 samples); the coefficients are made once a block, from the position of that
// block. The state is made to match the signal when the filter starts (from bypass, or across 0): a low-pass starts with its state at the first sample (the steady state of that sample), a high-pass
// at zero, so the first output sample is the input's. When the target is 0 the glide runs down to 0 through the open end, where the filter is flat, and the stage then puts itself in bypass.
//
// Real time: float only, no heap, no libm (core::exp2_fast, once a block), loops of exactly kBlock, nothing inside the loop but the two filters. It runs from flash, called through the graph's
// MasterFn pointer like the limiter (the SRAM_L hot budget has no room). The knob and the target are written by the ParamChange drain and read by the render, which is one context (the graph's
// pass): no atomic is needed. State is 56 bytes, in the graph.

#include <o3os/hal/audio.h>

#include <cstdint>

namespace o3os::audio {

class MasterDjFilter {
 public:
  static constexpr std::uint32_t kBlock = hal::audio::kBlock;
  static constexpr float kMax = 100.0f;  // the knob's range, per cent: -kMax (low-pass, closed) .. +kMax (high-pass, closed)
  static constexpr float kK = 2.0f;      // the damping, 1 / Q: 2 is critically damped (Q 0.5), the low-pass has no overshoot at all (see the header)

  // The cutoff law, as log2 of the warped cutoff g = tan(pi fc / fs) at the knob's distance t (0..1) from 0, linear in log2: low-pass g = 12 (about 21 kHz) .. 0.00427 (60 Hz); high-pass
  // g = 0.000712 (10 Hz) .. 0.64 (8 kHz). At 44.1 kHz.
  static constexpr float kLpOpenLog2 = 3.585f;
  static constexpr float kLpSpanLog2 = -11.456f;
  static constexpr float kHpOpenLog2 = -10.457f;
  static constexpr float kHpSpanLog2 = 9.813f;

  MasterDjFilter() noexcept = default;
  MasterDjFilter(const MasterDjFilter&) = delete;
  MasterDjFilter& operator=(const MasterDjFilter&) = delete;

  // The slot's bypass flag in the graph, which the filter sets itself when it has run down to 0 (render context). The graph clears it when a target other than 0 arrives.
  void attach(bool* bypass) noexcept { bypass_ = bypass; }

  // The knob, per cent, reached in `ramp_samples` (at least one block); false when out of range or not a number (nothing changes). Render context, or before start.
  bool set(float percent, std::uint32_t ramp_samples) noexcept;
  // At once, state cleared: the export's start and end (Graph::perf_clear / perf_restore). The target 0 puts the filter away; the caller puts the slot in bypass.
  void jump(float percent) noexcept;
  // Neither a target nor a glide in progress: bypassed, no cost.
  bool idle() const noexcept { return tgt_ == 0.0f && cur_ == 0.0f; }
  float target() const noexcept { return tgt_; }
  float position() const noexcept { return cur_; }  // where the glide is (for tests)

  // One block of exactly kBlock frames, in place, as float planes. Any other n is left untouched. The graph's MasterFn is process_fx.
  void process(float* left, float* right, std::uint32_t n) noexcept;
  static void process_fx(void* self, float* left, float* right, std::uint32_t n) noexcept { static_cast<MasterDjFilter*>(self)->process(left, right, n); }

 private:
  float ic1_[2] = {};  // the filter's two integrator states, per channel
  float ic2_[2] = {};
  float a1_ = 0.0f;  // the block's coefficients
  float a2_ = 0.0f;
  float a3_ = 0.0f;
  float cur_ = 0.0f;    // the knob in force (per cent); moves to tgt_ by delta_ a block
  float tgt_ = 0.0f;
  float delta_ = 0.0f;
  std::uint32_t steps_ = 0;  // blocks left in the glide
  bool lp_ = false;          // the mode the state belongs to
  bool armed_ = false;       // the state belongs to the signal running through (false: it is made at the next block's first sample)
  bool* bypass_ = nullptr;
};

}  // namespace o3os::audio
