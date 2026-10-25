// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The master kill EQ (OS-DJ A3 and A3b, docs/specs/OS-DJ.md D2 as amended; the `eq` stage of kMasterChain; NEW COMMUNITY OS BEHAVIOUR, the sound is ours, nothing is measured on an official unit).
//
// THREE BANDS FROM TWO LINKWITZ-RILEY CROSSOVERS. LR4 (fourth order, 24 dB an octave; two second-order Butterworth sections one after the other) at kLowHz splits the signal into LOW (below) and
// the rest; the rest is split the same way at kHighHz into MID (below) and HIGH (above). Both bands of an LR4 split are -6 dB at the corner and in phase, and their sum is a second-order all-pass.
// So the LOW band goes through a second-order all-pass at kHighHz (the phase the MID and HIGH bands got from their crossover), and low + mid + high is a flat-magnitude all-pass whatever the
// corners: no band lifts another. Each band has a gain that is 1 (live) or 0 (killed), moved with a 5 ms linear ramp.
//
// WHAT A KILL IS HERE. A killed band is taken away at 24 dB an octave inside the others' ranges, to -40 dB or more at the centre of the band and -6 dB at its corner (maintainer, 2026-10-19:
// kill depth A, replacing the first-order split of A3): killed LOW is -45 dB at 50 Hz and -62 dB at 30 Hz, killed MID -42 dB at 700 Hz (the worst point: the middle of the band), killed HIGH
// -50 dB at 10 kHz. The corners were moved from 200 Hz and 2.5 kHz to 180 Hz and 2.8 kHz because at 200 Hz and 2.5 kHz the MID kill is only -38 dB at 700 Hz (the leaks of the neighbours add
// in phase, (corner / f)^4 each); nothing else depends on them. No kill lifts any frequency (every response is at or under 0 dB, so the HIGH kill needs no compensation as it did with the
// first-order split) and the three bands add up to the input's magnitude exactly. What is proven is the steady response to sines (tests/audio_kill_eq_test.cpp). PEAKS ARE NOT: an all-pass
// turns the phases, so the peak of a limited mix grows while the stage is in (analytically +2 dB on average and up to +4 dB for a synthetic mix with flat tops at the limiter's ceiling, up to
// +6.6 dB for a ceiling-level square at 400 Hz; a kick burst changes by +0.1 to -3.2 dB), and the converter would saturate what passes full scale. So the stage sits AHEAD of the limiter (kMasterChain; docs/specs/OS-DJ.md
// A3c), which catches the peaks.
//
// ENTRY AND EXIT BY CROSSFADE. The band sum is an all-pass, not the identity, so switching between the bypassed stage (the input) and the stage would change the phase at once. Instead a wet weight
// moves 0 -> 1 on a kill and 1 -> 0 on its release with the same 5 ms ramp as the gains, and the output is (1 - wet) input + wet (bands with their gains): a crossfade between two continuous signals.
// A kill that starts from bypass runs the filters from the DC state of the first sample (the first output sample is the input's to one step of the ramp, under 0.45 %), the crossfade hides the
// settling; a release ends in the input itself (wet and gains arrive together) and the stage puts itself in bypass, bit for bit the input again. The filters keep running while anything is on or ramping, so a band that
// comes back is already in phase. Every coefficient is ramped per sample and the last sample of a block lands exactly on the block's end value.
//
// Real time: float only, no heap, no libm, loops of exactly kBlock, nothing inside the loop but the five filter sections of a channel and the mix; the ten states of a channel are flushed to 0 below
// 1e-20 once a block (a kill held over silence cannot leave them denormal or stuck at 1 ulp). It runs from flash (-O3), called through the
// graph's MasterFn pointer like the limiter and the DJ filter (the SRAM_L hot budget has no room). A block whose coefficients do not move (a kill held) takes a loop without the dry term and the
// ramp steps, and only the sections its live bands need (OS-DJ rt: LOW alone sections 1 to 3, LOW + MID and LOW + HIGH 1 to 4, MID + HIGH 1, 2 and 5, the rest all five; the ones left out are cleared and primed when a ramp next runs). The mask, the gains and the wet weight are written by the ParamChange drain and read by the render, which is one context (the graph's pass): no atomic is needed. The state (two
// channels of ten floats, the gains, the wet weight) is in the graph.
//
// CPU [est]: counted from the compiled loop (arm-none-eabi-objdump, -O3, -ffp-contract=off as the audio library is built): a channel's sample is 69 instructions, about 75 cycles of the M4F
// (FP add / mul 1 cycle, loads and stores 2, the loop branch 3), 150 a frame, 3.7 % of the 180 MHz core at 44.1 kHz while a kill is held; the 5 ms ramps (a loop that also steps four
// coefficients, 91 cycles) cost 4.5 %; 0 at rest. To be measured on the unit (cpu-budget §5).

#include <o3os/hal/audio.h>

#include <cstdint>

namespace o3os::audio {

class MasterKillEq {
 public:
  static constexpr std::uint32_t kBlock = hal::audio::kBlock;
  // The sections 3 (the second split), 4 (MID) and 5 (the all-pass on LOW) as the bits of stale_.
  static constexpr unsigned kStaleS3 = 1u;
  static constexpr unsigned kStaleS4 = 2u;
  static constexpr unsigned kStaleS5 = 4u;
  // The bands, as the bits of the kill mask.
  static constexpr unsigned kLow = 1u;
  static constexpr unsigned kMid = 2u;
  static constexpr unsigned kHigh = 4u;
  static constexpr unsigned kAll = kLow | kMid | kHigh;

  // The corners of the two LR4 crossovers, at the engine's rate (44,100 Hz by default). The warped constants in the .cpp are those of 44,117.647 Hz (the pre-1.0 clock), so at 44,100 Hz the corners sit 0.04 % low.
  static constexpr float kLowHz = 180.0f;
  static constexpr float kHighHz = 2800.0f;
  // A gain, and the wet weight, move by this much a block of kBlock samples: the whole travel (1 <-> 0) takes 5 ms (221 samples), the last block of it shorter.
  static constexpr float kStep = 64.0f / 221.0f;

  MasterKillEq() noexcept = default;
  MasterKillEq(const MasterKillEq&) = delete;
  MasterKillEq& operator=(const MasterKillEq&) = delete;

  // The slot's bypass flag in the graph, which the stage sets itself once no kill is on and everything is back at rest (render context). The graph clears it when a mask other than 0 arrives.
  void attach(bool* bypass) noexcept { bypass_ = bypass; }

  // The kill mask (a sum of kLow, kMid, kHigh) as the float the ParamChange carries; false when it is not a whole number 0..kAll (nothing changes). Render context, or before start.
  bool set(float mask) noexcept;
  // The export's start and end (Graph::perf_clear / perf_restore): the state cleared and every gain at 1 and the wet weight at 0 (the input), the mask as the target, so a kill that comes back
  // is crossfaded in from the first block.
  void reset(unsigned mask) noexcept;
  // Neither a kill nor anything still on its way: bypassed, no cost.
  bool idle() const noexcept { return mask_ == 0u && wet_ == 0.0f && gain_[0] == 1.0f && gain_[1] == 1.0f && gain_[2] == 1.0f; }
  unsigned mask() const noexcept { return mask_; }
  float gain(unsigned band) const noexcept { return band < 3u ? gain_[band] : 0.0f; }  // 0 low, 1 mid, 2 high: where the ramp is (for tests)
  float wet() const noexcept { return wet_; }                                           // 0 the input, 1 the bands: where the crossfade is (for tests)
  float state(unsigned ch, unsigned i) const noexcept { return ch < 2u && i < 10u ? st_[ch][i] : 0.0f; }  // an integrator of a channel (for tests)

  // One block of exactly kBlock frames, in place, as float planes. Any other n is left untouched. The graph's MasterFn is process_fx.
  void process(float* left, float* right, std::uint32_t n) noexcept;
  static void process_fx(void* self, float* left, float* right, std::uint32_t n) noexcept { static_cast<MasterKillEq*>(self)->process(left, right, n); }

 private:
  // Per channel the (ic1eq, ic2eq) integrators of the five TPT state-variable sections: LOW crossover (2), the rest's crossover (2), the all-pass on LOW (1).
  float st_[2][10] = {};
  float gain_[3] = {1.0f, 1.0f, 1.0f};  // low, mid, high: where the ramp is, at the end of the last block
  float wet_ = 0.0f;                    // the crossfade input -> bands: where it is, at the end of the last block
  std::uint8_t mask_ = 0;               // the target: the bands that are killed
  bool armed_ = false;                  // the states belong to the signal running through (false: they are made at the next block's first sample)
  std::uint8_t stale_ = 0;              // sections a held kill left out (kStale*): their states are old, and are primed when a ramp next runs. Render context
  bool* bypass_ = nullptr;
};

}  // namespace o3os::audio
