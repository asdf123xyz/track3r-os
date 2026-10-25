// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/audio/metro.h>
#include <o3os/audio/simd.h>

namespace o3os::audio {
namespace {

// The resonator of the header: a2 = -r^2 and a1 = 2 r cos(w) for r = 2^(-15 / 662), w = 2 pi f / 44,117.647. The start state is y[0] = 0 and y[-1] = -sin(w) / r (times the peak), which makes the
// first sample of the click r sin(w) and the click A r^n sin(w n) (computed offline; these are the only numbers of the sound).
constexpr float kA2 = -0.969076712f;
constexpr float kTickA1 = 1.948900547f;     // 1000 Hz
constexpr float kTickY1 = -0.144184745f;
constexpr float kAccentA1 = 1.924078592f;   // 1500 Hz
constexpr float kAccentY1 = -0.215363127f;

using simd::NativeOps;  // SSAT #16 on the M4F, its portable model elsewhere (audio/simd.h)

}  // namespace

void Metro::trigger(std::uint32_t offset, bool accent) noexcept {
  if (mode_ == Mode::Off || !(level_ > 0.0f)) {
    return;
  }
  pending_ = true;
  accent_ = accent;
  start_ = offset < hal::audio::kBlock ? offset : hal::audio::kBlock - 1u;
}

void Metro::mix_out(std::int16_t* out, std::uint32_t frames, bool silence, bool split) noexcept {
  if (silence) {
    cancel();
    return;
  }
  const bool cue = split && mode_ == Mode::Cue;
  mix(out, frames, !cue, !split || cue);
}

void Metro::mix(std::int16_t* out, std::uint32_t frames, bool left, bool right) noexcept {
  std::uint32_t i = 0;
  if (pending_) {
    pending_ = false;
    const float peak = 32767.0f * kAccentPeak * level_ * (accent_ ? 1.0f : kTickRatio);
    a1_ = accent_ ? kAccentA1 : kTickA1;
    p1_ = 0.0f;
    p2_ = (accent_ ? kAccentY1 : kTickY1) * peak;
    left_ = kClickFrames;
    i = start_ < frames ? start_ : frames;
  }
  std::uint32_t n = frames - i;
  n = n < left_ ? n : left_;
  left_ -= n;
  const float a1 = a1_;
  float p1 = p1_;
  float p2 = p2_;
  std::int16_t* o = out + 2u * i;
  for (; n != 0u; --n, o += 2) {
    const float y = a1 * p1 + kA2 * p2;
    p2 = p1;
    p1 = y;
    const std::int32_t v = static_cast<std::int32_t>(y);  // truncated: the click is at most 0.7 of full scale, and its tail ends below the least bit
    if (left) {
      o[0] = static_cast<std::int16_t>(NativeOps::ssat16(o[0] + v));
    }
    if (right) {
      o[1] = static_cast<std::int16_t>(NativeOps::ssat16(o[1] + v));
    }
  }
  p1_ = p1;
  p2_ = p2;
}

}  // namespace o3os::audio
