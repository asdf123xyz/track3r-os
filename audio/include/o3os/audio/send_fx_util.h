// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the two send effects (send_delay.h, send_reverb.h) share: the float to int16 write, the stereo word, and the flush of a decayed filter state. Internal to the audio library.

#include <o3os/audio/hot.h>
#include <o3os/audio/simd.h>

#include <cstdint>
#include <cstring>

namespace o3os::audio::sendfx_util {

// A float held to the int16 range and CUT TOWARDS ZERO (the hardware's VCVT rounds that way), as an int32. The cut is deliberate: a tail that recirculates through int16 words and rounds to
// nearest can sit on +-1 for ever (a limit cycle: 0.6 x 1 rounds back to 1), one that is cut towards zero strictly shrinks and ends at exactly 0, so silence in gives silence out.
// On the Cortex-M4F: VCVT.S32.F32 saturates and turns NaN into 0 (written as the instruction, so no float-to-int overflow is ever executed as C++), then SSAT; elsewhere the clamp is
// written out (NaN becomes +32767).
O3OS_AUDIO_INLINE std::int32_t trunc16(float v) noexcept {
#if defined(__ARM_FEATURE_SIMD32) && defined(__ARM_FP)
  __asm__("vcvt.s32.f32 %0, %0" : "+t"(v));
  std::int32_t i;
  std::memcpy(&i, &v, sizeof i);
  return simd::M4Ops::ssat16(i);
#else
  v = v < 32767.0f ? v : 32767.0f;
  v = v > -32768.0f ? v : -32768.0f;
  return static_cast<std::int32_t>(v);
#endif
}

// Two int16 values in one 32-bit word, `l` in the low half.
O3OS_AUDIO_INLINE std::uint32_t pack(std::int32_t l, std::int32_t r) noexcept {
  return (static_cast<std::uint32_t>(l) & 0xFFFFu) | (static_cast<std::uint32_t>(r) << 16);
}
O3OS_AUDIO_INLINE float lane_l(std::uint32_t w) noexcept { return static_cast<float>(static_cast<std::int16_t>(w & 0xFFFFu)); }
O3OS_AUDIO_INLINE float lane_r(std::uint32_t w) noexcept { return static_cast<float>(static_cast<std::int16_t>(w >> 16)); }

// A filter state below a thousandth of an LSB (the states are in int16 units) is zero: no denormals, and an exact silence after a tail.
inline constexpr float kFlushBelow = 1.0e-3f;
O3OS_AUDIO_INLINE float flush(float v) noexcept { return __builtin_fabsf(v) < kFlushBelow ? 0.0f : v; }

// One step of a slew towards a target (the per-block smoothing of a control), snapping when it is close.
O3OS_AUDIO_INLINE float slew(float now, float target, float k) noexcept {
  const float next = now + (target - now) * k;
  return __builtin_fabsf(target - next) < 1.0e-4f ? target : next;
}

}  // namespace o3os::audio::sendfx_util
