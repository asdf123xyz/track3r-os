// SPDX-License-Identifier: GPL-3.0-or-later
//
// The master DJ filter (OS-DJ A2): see audio/master_dj_filter.h for what it is and for the values. This file is the arithmetic.
//
// The filter is the trapezoidal state-variable form: with g = tan(pi fc / fs) and k = 1 / Q, a1 = 1 / (1 + g (g + k)), a2 = g a1, a3 = g a2, and for each sample
//   v3 = x - ic2;  v1 = a1 ic1 + a2 v3;  v2 = ic2 + a2 ic1 + a3 v3;  ic1 = 2 v1 - ic1;  ic2 = 2 v2 - ic2;
// the low-pass is v2 and the high-pass x - k v1 - v2. It is stable for every g > 0 (the integrators are the physical states), so the open ends (g = 12 and g = 0.0007) need no care.

#include <o3os/audio/master_dj_filter.h>

#include <o3os/core/fast_math.h>

namespace o3os::audio {

namespace {

// Both channels in one loop (the coefficients shared), the mode a template parameter so that the loop has no branch. `ic` holds the states of the left and of the right.
template <bool Lp>
void run(float* left, float* right, float* ic1, float* ic2, float a1, float a2, float a3) noexcept {
  constexpr float k = MasterDjFilter::kK;
  float c1l = ic1[0];
  float c1r = ic1[1];
  float c2l = ic2[0];
  float c2r = ic2[1];
  for (std::uint32_t i = 0; i < MasterDjFilter::kBlock; ++i) {
    const float xl = left[i];
    const float xr = right[i];
    const float v3l = xl - c2l;
    const float v3r = xr - c2r;
    const float v1l = a1 * c1l + a2 * v3l;
    const float v1r = a1 * c1r + a2 * v3r;
    const float v2l = c2l + a2 * c1l + a3 * v3l;
    const float v2r = c2r + a2 * c1r + a3 * v3r;
    c1l = 2.0f * v1l - c1l;
    c1r = 2.0f * v1r - c1r;
    c2l = 2.0f * v2l - c2l;
    c2r = 2.0f * v2r - c2r;
    left[i] = Lp ? v2l : xl - k * v1l - v2l;
    right[i] = Lp ? v2r : xr - k * v1r - v2r;
  }
  ic1[0] = c1l;
  ic1[1] = c1r;
  ic2[0] = c2l;
  ic2[1] = c2r;
}

}  // namespace

bool MasterDjFilter::set(float percent, std::uint32_t ramp_samples) noexcept {
  if (!(percent >= -kMax && percent <= kMax)) {  // (a NaN fails both)
    return false;
  }
  const std::uint32_t blocks = ramp_samples <= kBlock ? 1u : (ramp_samples + kBlock - 1u) / kBlock;
  tgt_ = percent;
  steps_ = blocks;
  delta_ = (percent - cur_) / static_cast<float>(blocks);
  return true;
}

void MasterDjFilter::jump(float percent) noexcept {
  const float p = percent >= -kMax && percent <= kMax ? percent : 0.0f;
  cur_ = p;
  tgt_ = p;
  delta_ = 0.0f;
  steps_ = 0;
  armed_ = false;
  for (unsigned c = 0; c < 2u; ++c) {
    ic1_[c] = 0.0f;
    ic2_[c] = 0.0f;
  }
}

void MasterDjFilter::process(float* left, float* right, std::uint32_t n) noexcept {
  if (n != kBlock) {
    return;
  }
  if (steps_ != 0u) {
    --steps_;
    cur_ = steps_ == 0u ? tgt_ : cur_ + delta_;
  }
  if (cur_ == 0.0f) {
    // Open at both ends and meeting here: nothing to do, and the state belongs to no signal. Arrived for good (no glide left, target 0): the stage is bypassed from the next block.
    armed_ = false;
    if (steps_ == 0u && tgt_ == 0.0f && bypass_ != nullptr) {
      *bypass_ = true;
    }
    return;
  }
  const bool lp = cur_ < 0.0f;
  const float t = (lp ? -cur_ : cur_) * (1.0f / kMax);
  const float g = core::exp2_fast(lp ? kLpOpenLog2 + kLpSpanLog2 * t : kHpOpenLog2 + kHpSpanLog2 * t);
  a1_ = 1.0f / (1.0f + g * (g + kK));
  a2_ = g * a1_;
  a3_ = g * a2_;
  if (!armed_ || lp != lp_) {
    // Starting (from bypass, or across 0 with the other mode): the state of a signal that has been at its first sample for ever, so that the first output sample is the input's. A low-pass
    // follows its input: ic2 is that sample. A high-pass has taken the level out: ic2 is 0.
    armed_ = true;
    lp_ = lp;
    ic1_[0] = 0.0f;
    ic1_[1] = 0.0f;
    ic2_[0] = lp ? left[0] : 0.0f;
    ic2_[1] = lp ? right[0] : 0.0f;
  }
  if (lp) {
    run<true>(left, right, ic1_, ic2_, a1_, a2_, a3_);
  } else {
    run<false>(left, right, ic1_, ic2_, a1_, a2_, a3_);
  }
}

}  // namespace o3os::audio
