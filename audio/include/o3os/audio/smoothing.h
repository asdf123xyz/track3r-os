// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The smoothing contract and the level laws (OS-005b, OS-007i; ADR 0013 D6: "each audible parameter has a target and a per-sample linear
// ramp over one block (gain, pan, sends)"; OS-005c reuses this header for the sequencer's parameters).
//
// SMOOTHER. A Q16 gain (kernels.h: 65536 = unity) that moves toward a target. `retarget()` is called in the render context at block
// start (the ParamChange drain); `next_block()` is called once per block by the code that uses the gain and returns the pair the kernels
// take (gain at the start of the block, gain at its end). Three ramp lengths, from ParamChange::ramp_samples (core/param.h):
//     0        the engine's own smoothing: kSmoothBlocks blocks (one: 1.45 ms at 64 frames, the ADR's "ramp over one block")
//     1        instant: the gain is the target from this block's first sample. The caller asked for a step (a preset load); the engine does
//              not smooth it, and a step of audible signal is that caller's click
//     n >= 2   ceil(n / kBlock) blocks, at least one, at most kMaxRampBlocks
// A multi-block ramp moves by (target - current) / blocks_left each block, so it ends exactly on the target and each block's own ramp is
// linear. A new target while a ramp is running starts a new ramp from where the gain is now (never from its old start).
//
// The per-sample step bound is a property of the kernels (kernels.h: at most ceil(|end - start| / kBlock) per sample, 1,024 Q16 for a
// full-scale change); a multi-block ramp makes the step smaller by its length.
//
// THE LEVEL LAWS. A level 0..100 is a gain through one of two laws, both this OS's own model (level_gain_q16, below; whether the official firmware plays
// the same curves is an open research question, Q). They are NOT interchangeable and they are not chosen
// by a switch: the MIXER always uses Og1114, the INSTRUMENT volume uses Og12 or Og1114 by the origin of the project.
//   Og1114  the mixer law (the name refers to the 1.1.14 firmware; whether that firmware plays this curve is Q): 0 at level 0 and 10^((level - 100) / 50) from 1 to 100, i.e. 0.4 dB a step from
//           -39.6 dB to 0 dB, 100 is unity (applied as track volume x dry level, send x delay or reverb level, and instrument volume). It is the law of every mixer
//           level of the graph (LevelCurve: track volume, sends, the master's dry / delay / reverb / volume, the cue level, the input) AND of the
//           instrument volume of a project older than 1.2.
//   Og12    the 1.2+ law, the INSTRUMENT volume only (PAR-023: volume 0 = -inf, 1-100 = -24..+24 dB, default
//           50, as documented in the release notes, not verified; the stored u8 unchanged, so a 1.9 project's default level of 50 is 0 dB where Og1114 would play it 20 dB down).
//           Level 0 is silence, level 1 is -24.00 dB, levels 2..100 are 0.48 dB x (level - 50): 50 is exactly unity, 100 is +24.00 dB (15.85x).
//           A 101-entry level law (kept for compatibility: maintainer decision 2026-10-16, recorded in ADR 0013, PAR-023 level curve; the repo holds no capture behind its curve);
//           the curve between its anchors is INFERRED (the anchors are the documented 1.2+ volumes above; Q), so A/B listening on the unit
//           would confirm it. The voices apply it (SampleDesc::gain, computed by the loader, app/og_samples.h), never the graph.
// The mixer's 1.2+ level law is UNSOURCED and is not Og12 (cross-check): the value of every mixer level (track, reverb, delay, dry) in a v7+ OG project file is
// 86, which through Og12 would be +17.3 dB, and Og12's anchors fit the instrument default of 50. Until it is sourced (a measurement on an official unit, if ever done,
// or A/B on the unit) the mixer's levels go through Og1114 whatever the project's version; a 1.2+ project's mixer therefore plays at the Og1114
// law (an approximation; whether the official 1.2+ mixer does is Q). Whether the 1.2+ master DRY / DELAY / REVERB levels follow the mixer law is likewise unsourced.
// level_gain_q16 generates both tables' values at boot / load (no table in flash, the project rule) from exp2; LevelCurve keeps the mixer's 101
// entries in SRAM, and a fractional level (automation) interpolates linearly between two entries, which is within 0.002 dB of the exponential.
// The largest Og1114 entry is unity; the largest Og12 value, +24 dB, is 1,038,676 (15.85x).
//
// THE PAN LAW (this OS's own model; whether the official firmware's mixer uses it is an open research question, Q): a pan 0..100, 50 the centre. At the centre both channels
// are at unity (no centre attenuation); moving away from it the OPPOSITE channel is attenuated linearly to zero at the end:
//     left gain  = 1 for pan <= 50, (100 - pan) / 50 above        right gain = pan / 50 for pan <= 50, 1 above
// Pan 0 is hard left, 100 hard right on the jack: a pan above 50 attenuates the left jack (silent at 100).
//
// No heap, no libm: the level table needs exp2 once, at boot (core/fast_math.h); everything else is integer or single-precision.

#include <o3os/audio/hot.h>
#include <o3os/audio/kernels.h>
#include <o3os/hal/audio.h>

#include <cstdint>

namespace o3os::audio {

inline constexpr std::uint32_t kSmoothBlocks = 1;      // the engine's own smoothing, in blocks (ParamChange::ramp_samples == 0)
inline constexpr std::uint32_t kMaxRampBlocks = 4096;  // the longest ramp a ParamChange can ask for: about 6 s at 64 frames

using Ramp = kernel::GainRamp;  // the pair the kernels take

class Smoother {
 public:
  // Everything at once, no ramp (construction, tests).
  void set(std::int32_t gain) noexcept {
    cur_ = target_ = gain;
    blocks_ = 0;
  }
  // The gain IS its target now, no ramp: what a gain that carries no signal does (a closed route, a silent strip), so that when signal comes
  // back it does not ramp from where the gain was left (a mute, a route closed under it, the route back: nothing may leak through the old state).
  O3OS_AUDIO_INLINE void snap() noexcept {
    cur_ = target_;
    blocks_ = 0;
  }
  O3OS_AUDIO_INLINE void retarget(std::int32_t target, std::uint32_t ramp_samples) noexcept {
    target_ = target;
    if (ramp_samples == 1u || target == cur_) {
      cur_ = target;
      blocks_ = 0;
    } else {
      const std::uint32_t blocks = ramp_samples == 0u ? kSmoothBlocks : ramp_samples / hal::audio::kBlock + ((ramp_samples % hal::audio::kBlock) != 0u ? 1u : 0u);
      blocks_ = blocks == 0u ? 1u : (blocks > kMaxRampBlocks ? kMaxRampBlocks : blocks);
    }
  }
  // The gains at the start and the end of the coming block; moves the smoother on one block.
  O3OS_AUDIO_INLINE Ramp next_block() noexcept {
    const std::int32_t start = cur_;
    if (blocks_ != 0u) {
      cur_ = blocks_ == 1u ? target_ : cur_ + (target_ - cur_) / static_cast<std::int32_t>(blocks_);
      --blocks_;
    }
    return Ramp{start, cur_};
  }
  std::int32_t current() const noexcept { return cur_; }
  std::int32_t target() const noexcept { return target_; }
  bool ramping() const noexcept { return blocks_ != 0u; }
  // Closed and staying closed: the kernels may be skipped.
  O3OS_AUDIO_INLINE bool closed() const noexcept { return cur_ == 0 && target_ == 0; }
  // The same state as `o`: next_block() then gives the same pair (two gains retargeted together, as the pan law's two at the centre).
  O3OS_AUDIO_INLINE bool same_as(const Smoother& o) const noexcept { return cur_ == o.cur_ && target_ == o.target_ && blocks_ == o.blocks_; }

 private:
  std::int32_t cur_ = 0;
  std::int32_t target_ = 0;
  std::uint32_t blocks_ = 0;
};

// ---- The level laws -----------------------------------------------------------------------------------------------------------
inline constexpr unsigned kLevelSteps = 100;  // levels 0..100

enum class LevelLaw : std::uint8_t { Og1114 = 0, Og12 = 1 };

// A level 0..100 (clamped) as a Q16 gain in [0, kMaxGain] through `law`, rounded to nearest; level 0 is exactly 0, Og1114's 100 and Og12's 50 are
// exactly kernel::kUnity. Boot / load time, main thread: calls core::exp2_fast (flash), not for the render context.
std::int32_t level_gain_q16(unsigned level, LevelLaw law) noexcept;

// The MIXER's level curve: Og1114 only (see THE LEVEL LAWS: the 1.2+ mixer law is not sourced and is not Og12).
class LevelCurve {
 public:
  // Boot, main thread: fills the 101 entries (calls level_gain_q16, which lives in flash).
  void init() noexcept;
  // A level 0..100 (clamped; NaN is 0) as a Q16 gain in [0, kUnity]. Integer arithmetic after the table lookup: callable from SRAM_L.
  O3OS_AUDIO_INLINE std::int32_t q16(float level) const noexcept {
    if (!(level > 0.0f)) {
      return 0;
    }
    if (!(level < static_cast<float>(kLevelSteps))) {
      return table_[kLevelSteps];
    }
    const std::uint32_t i = static_cast<std::uint32_t>(level);
    const float f = level - static_cast<float>(i);
    const std::int32_t a = table_[i];
    const std::int32_t b = table_[i + 1u];
    return a + static_cast<std::int32_t>(static_cast<float>(b - a) * f + 0.5f);
  }
  std::int32_t entry(unsigned i) const noexcept { return table_[i <= kLevelSteps ? i : kLevelSteps]; }

 private:
  std::int32_t table_[kLevelSteps + 1u] = {};
};

// (a * b) in Q16, rounded to nearest, clamped to kMaxGain: the product of two gains (a stack of faders cannot ask a kernel for more than it is
// specified for).
O3OS_AUDIO_INLINE std::int32_t mul_q16(std::int32_t a, std::int32_t b) noexcept {
  const std::int64_t p = (static_cast<std::int64_t>(a) * b + (kernel::kUnity / 2)) >> 16;
  return p > kernel::kMaxGain ? kernel::kMaxGain : static_cast<std::int32_t>(p);
}

// ---- The pan law --------------------------------------------------------------------------------------------------------------
struct PanGains {
  std::int32_t left;
  std::int32_t right;
};
// pan 0..100 (clamped; NaN is the centre) as the two Q16 gains of the law above, rounded to nearest.
O3OS_AUDIO_INLINE PanGains pan_gains_q16(float pan) noexcept {
  if (!(pan >= 0.0f)) {
    pan = pan < 0.0f ? 0.0f : 50.0f;  // below the range: hard left; NaN: the centre
  }
  if (pan > 100.0f) {
    pan = 100.0f;
  }
  constexpr float kPerUnit = 65536.0f / 50.0f;  // one pan step away from the centre, in Q16
  if (pan <= 50.0f) {
    return PanGains{kernel::kUnity, static_cast<std::int32_t>(pan * kPerUnit + 0.5f)};
  }
  return PanGains{static_cast<std::int32_t>((100.0f - pan) * kPerUnit + 0.5f), kernel::kUnity};
}

}  // namespace o3os::audio
