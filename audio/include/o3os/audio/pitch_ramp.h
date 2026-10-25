// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The pitch ramp of the Slide and Glide FX (OS-PFX, OS-IFX1, OS-MODPITCH): ONE law for the sampler voices (VoicePool::mod_block) and the module instruments (ModuleVoices, ABI 1.8 pitch()).
// A ramp is an offset from the note in 1/65536 semitone (Q16), a target and the samples left; it is walked once a block, linear: what is left of the distance over what is left of the time, and it
// lands on the target with the last evaluation (the pitch of the block that follows has the target). A pure header, no state of its own: the numbers the two sides share are here so that they
// cannot drift (tests/module_pitch_test.cpp pins the module side against the sampler's audible ramp).

#include <o3os/hal/audio.h>

#include <cstdint>

namespace o3os::audio {

struct PitchRamp {
  std::int32_t q16 = 0;    // the offset from the note now, 1/65536 semitone
  std::int32_t tgt = 0;    // ... and where it goes
  std::uint32_t left = 0;  // samples until it is there; 0: it is (the ramp is idle)
};

// The most a ramp's offset may be, either way (ABI 1.8 pitch(): |semitones| <= 128). With it a step of the walk (|distance| <= 2 x this, times a block of 64) stays inside an int32.
inline constexpr std::int32_t kRampLimitQ16 = 128 * 65536;

// The sequencer's ticks a step (the voices' clock too), and the tick rate in Q24 ticks a sample for a tempo in BPM x 100 (384 x 2^24 over the sample rate in mHz: exact in float).
inline constexpr std::uint32_t kTicksPerStep = 576;
inline constexpr float kTickRateQ24PerX100 = 6442450944.0f / static_cast<float>(hal::audio::sample_rate_mhz());
inline std::uint32_t tick_rate_q24(std::uint32_t x100) noexcept { return static_cast<std::uint32_t>(static_cast<float>(x100 > 100000u ? 100000u : x100) * kTickRateQ24PerX100); }

// A Slide FX's ramp lasts one step of the tempo in force (NEW Community OS behaviour, voices.h SLIDE FX), in samples, 1 at least.
inline std::uint32_t slide_step_samples(std::uint32_t tick_rate_q) noexcept {
  const std::uint64_t per = (static_cast<std::uint64_t>(kTicksPerStep) << 24) / (tick_rate_q != 0u ? tick_rate_q : 1u);
  return per < 1u ? 1u : (per > 0xFFFFFFu ? 0xFFFFFFu : static_cast<std::uint32_t>(per));
}
// A Slide FX of `sixteenths` of a semitone (-255..255) as the ramp's target.
constexpr std::int32_t slide_target_q16(int sixteenths) noexcept { return sixteenths * 4096; }
// A Glide's time in samples (`ms` x 44.1), 1 at least.
constexpr std::uint32_t glide_samples(std::uint32_t ms) noexcept {
  const std::uint32_t samples = ms * 441u / 10u;
  return samples < 1u ? 1u : samples;
}

// The ramp moves on `ahead` samples (the rest of the block, at most a block): the offset is where the walk puts it, and a ramp that is left with no more time than that lands on its target. Returns
// false once the ramp has landed (left is 0); the caller takes the target as the pitch of the block that follows.
inline bool ramp_advance(PitchRamp& r, std::uint32_t ahead) noexcept {
  if (r.left == 0u) {
    return false;
  }
  if (r.left <= ahead) {
    r.q16 = r.tgt;
    r.left = 0;
    return false;
  }
  r.q16 += (r.tgt - r.q16) * static_cast<std::int32_t>(ahead) / static_cast<std::int32_t>(r.left);  // 1/65536 semitone: a 15 s glide of a semitone still moves 6 units a block
  r.left -= ahead;
  return true;
}

}  // namespace o3os::audio
