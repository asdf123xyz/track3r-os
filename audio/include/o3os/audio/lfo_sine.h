// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-LFOSINE (docs/specs/OS-LFOSINE.md): the Sine shape of the LFO generator, ONE law for the voices (VoicePool::flfo_level) and the host tests. A pure header: float only, no libm, no table.
//
// lfo_sine_level(u) = (1 - cos 2 pi u) / 2 for the phase u in [0, 1): 0 at u = 0, 1 at u = 1/2, the Triangle's phase and amplitude convention (the generator's output 2 x level x A - A is -A cos 2 pi u).
// It is 1/2 + sin(2 pi v) / 2 for v = u - 1/4, folded to the quarter wave [-1/4, 1/4] and evaluated with the odd quintic v (c1 + c3 v^2 + c5 v^4), the minimax fit with the peak exact
// (c1 + c3/16 + c5/256 = 4): the error of sin(2 pi v) is at most 8.1e-5 (4e-5 of the level), tests/audio_lfosine_test.cpp holds the bound. About 12 float operations, no branch but the fold.

#include <o3os/audio/hot.h>

namespace o3os::audio {

inline constexpr float kSineC3 = -41.0695f;
inline constexpr float kSineC5 = 73.1833f;
inline constexpr float kSineC1 = 4.0f - kSineC3 / 16.0f - kSineC5 / 256.0f;

// The level 0..1 for a phase u in [0, 1] (a value outside is folded as it is: the result stays within a few 1e-4 of [0, 1] and the caller holds it to 0..1).
O3OS_AUDIO_INLINE float lfo_sine_level(float u) noexcept {
  float v = u - 0.25f;
  v = v > 0.25f ? 0.5f - v : v;
  const float v2 = v * v;
  const float s = 0.5f + 0.5f * v * (kSineC1 + v2 * (kSineC3 + v2 * kSineC5));
  return s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
}

}  // namespace o3os::audio
