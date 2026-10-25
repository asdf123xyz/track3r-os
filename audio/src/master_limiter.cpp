// SPDX-License-Identifier: GPL-3.0-or-later
//
// The master limiter (OS-LIMIT): see audio/master_limiter.h for what it is and for the values. This file is the arithmetic.
//
// THE CEILING PROOF (what the loop in process() relies on). Let x'[t] be the slow stage's result at input time t, p'[t] = max(|L'|, |R'|) its peak, W = kBlock = 64, K = kGainCeiling.
//   pm[j]  = max p'[j-W+1 .. j]                       the sliding maximum: the larger of the previous block's suffix maximum from j's position + 1 (sfx_) and this block's running maximum (run)
//   m[j]   = floor(Q24 * min(1, K / pm[j]))           the gain that holds that window to the ceiling, rounded DOWN
//   G[n]   = (m[n-W+1] + ... + m[n]) / 2^(24 + 6)     the moving average of the last W of them (hist_, sum_: integers, so exact)
//   gp[n]  = min(G[n], gp[n-1] + cb * (1 - gp[n-1]))  the release: never above G, so never above what the ceiling needs
//   y[n]   = x'[n - (W-1)] * gp[n]                    the output, W - 1 = kLatency samples behind the input
// For t = n - (W-1) every m[j] with j in [t, t + W - 1] has t inside its window (j - W + 1 <= t <= j), so m[j] <= K / p'[t] when p'[t] > K, and the average G[n] of exactly those W values is
// <= K / p'[t]. Then |y[n]| <= p'[t] * G[n] <= K; and when p'[t] <= K, G <= 1 gives |y| <= p'[t] <= K. Nothing in it depends on the controls, the release or what came before: the history only
// enters through values <= 1, and a window that includes samples from before the start sees the zeros of reset(). The float roundings on the way (the division, the conversion of the sum to
// float, the final product: 6e-8 relative each) are absorbed by K being 3e-7 under kCeiling, so even a float comparison finds nothing above kCeiling. With every m at unity, G = 2^30 / 2^30 is
// exactly 1.0f, and so is gp, so a signal under the ceiling (and under the slow stage's threshold) comes out bit for bit.

#include <o3os/audio/master_limiter.h>

namespace o3os::audio {

namespace {

constexpr float kUnityQ = 16777216.0f;              // 2^24: a gain of 1.0 in the per-sample history
constexpr std::uint32_t kUnityQi = 1u << 24;
constexpr float kSumScale = 1.0f / 1073741824.0f;   // 2^-30: the sum of kBlock (2^6) Q24 gains is Q30
constexpr float kRise = 2200.0f / MasterLimiter::kRateHz;  // the one-pole's step for a 10 % to 90 % time of one millisecond (2.2 time constants)
constexpr float kOffThreshold = 1.0e6f;             // the slow stage's threshold with the switch OFF: out of reach of any plane the graph can make

// The one-pole step for a 10 % to 90 % time of `ms`: 1 - exp(-x) with x = 2.2 / (ms * fs), as the bilinear x / (1 + x / 2), which is within 0.3 % at the 0.25 ms of the fastest preset and
// better above (no libm in the render context).
float coef(float ms) noexcept {
  const float x = kRise / (ms < MasterLimiter::kMinTimeMs ? MasterLimiter::kMinTimeMs : ms);
  return x / (1.0f + 0.5f * x);
}

}  // namespace

void MasterLimiter::reset() noexcept {
  for (std::uint32_t i = 0; i < kBlock; ++i) {
    ring_[0][i] = 0.0f;
    ring_[1][i] = 0.0f;
    sfx_[i] = 0.0f;
    hist_[i] = kUnityQi;
  }
  sfx_[kBlock] = 0.0f;
  sum_ = kBlock * kUnityQi;
  env_ = 0.0f;
  gp_ = 1.0f;
  thr_pct_ = limiter::kThresholdDefault;
  att_ms_ = limiter::kAttackDefault;
  rel_ms_ = limiter::kReleaseDefault;
  mode_ = 0;
  on_ = false;
  win_blocks_ = 0;
  win_min_ = 1.0f;
  windows_ = 0;
  reduced_ = 0;
  hold_ = 65536u;
  pub_windows_.store(0u, std::memory_order_relaxed);
  pub_reduced_.store(0u, std::memory_order_relaxed);
  pub_now_.store(65536u, std::memory_order_relaxed);
  pub_hold_.store(65536u, std::memory_order_relaxed);
  pub_mode_.store(0u, std::memory_order_relaxed);
  prepare();
}

void MasterLimiter::flush() noexcept {
  for (std::uint32_t i = 0; i < kBlock; ++i) {
    ring_[0][i] = 0.0f;
    ring_[1][i] = 0.0f;
    sfx_[i] = 0.0f;
    hist_[i] = kUnityQi;
  }
  sfx_[kBlock] = 0.0f;
  sum_ = kBlock * kUnityQi;
  env_ = 0.0f;
  gp_ = 1.0f;
}

bool MasterLimiter::set_param(unsigned field, float value) noexcept {
  switch (field) {
    case limiter::Switch:
    case limiter::Mode:
      if (value != 0.0f && value != 1.0f) {
        return false;
      }
      if (field == limiter::Switch) {
        on_ = value != 0.0f;
      } else {
        mode_ = value != 0.0f ? 1u : 0u;
        pub_mode_.store(mode_, std::memory_order_relaxed);
      }
      break;
    case limiter::Threshold:
      if (!(value >= limiter::kThresholdMin && value <= limiter::kThresholdMax)) {  // (also refuses a NaN)
        return false;
      }
      thr_pct_ = value;
      break;
    case limiter::Attack:
    case limiter::Release:
      if (!(value >= limiter::kTimeMin && value <= limiter::kTimeMax)) {
        return false;
      }
      (field == limiter::Attack ? att_ms_ : rel_ms_) = value;
      break;
    default:
      return false;
  }
  dirty_ = true;
  return true;
}

void MasterLimiter::prepare() noexcept {
  ts_ = on_ ? thr_pct_ * 0.01f : kOffThreshold;
  ca_ = coef(att_ms_ * kSlowScale[mode_]);
  cr_ = coef(rel_ms_ * kSlowScale[mode_]);
  cb_ = coef(kBrickReleaseMs[mode_]);
  dirty_ = false;
}

void MasterLimiter::process(float* left, float* right, std::uint32_t n) noexcept {
  if (n != kBlock) {
    return;
  }
  if (dirty_) {
    prepare();
  }
  float* const ring_l = ring_[0];
  float* const ring_r = ring_[1];
  const float ts = ts_;
  const float ca = ca_;
  const float cr = cr_;
  const bool slow = on_;
  float env = slow ? env_ : 0.0f;  // the follower runs only while the switch is ON: it starts from nothing when the switch goes on

  // The stereo-linked peak of every sample, and the block's.
  float pk[kBlock];
  float pmax = 0.0f;
  for (std::uint32_t k = 0; k < kBlock; ++k) {
    const float al = __builtin_fabsf(left[k]);
    const float ar = __builtin_fabsf(right[k]);
    const float p = al > ar ? al : ar;
    pk[k] = p;
    pmax = p > pmax ? p : pmax;
  }

  float gmin = 1.0f;  // the lowest gain this block (a bound: the two stages' lowest multiplied), for the report
  // THE QUIET BLOCK: nothing is being reduced and nothing in this block, nor in the one before, reaches the ceiling or the slow stage's threshold, so both stages are the identity (every gain is
  // 1.0 exactly, every window's maximum is under the ceiling): the block is the delay line, shifted. Exactly what the loop below would compute, at a third of its cost.
  const bool quiet = sum_ == kBlock * kUnityQi && gp_ == 1.0f && sfx_[0] <= kCeiling && pmax <= kCeiling && (!slow || (pmax <= ts && env <= ts));
  if (quiet) {
    if (slow) {
      for (std::uint32_t k = 0; k < kBlock; ++k) {
        env += (pk[k] > env ? ca : cr) * (pk[k] - env);
      }
    }
    for (std::uint32_t k = 0; k < kBlock; ++k) {
      const std::uint32_t j = (k + 1u) & (kBlock - 1u);
      const float xl = left[k];
      const float xr = right[k];
      left[k] = ring_l[j];
      right[k] = ring_r[j];
      ring_l[k] = xl;
      ring_r[k] = xr;
      sfx_[k] = pmax;  // an upper bound of the suffix maxima that is under the ceiling: the next block's windows decide the same
    }
  } else {
    const float cb = cb_;
    float gp = gp_;
    std::uint32_t sum = sum_;
    float run = 0.0f;  // the running maximum of this block's peaks up to the sample being done
    float min_s = 1.0f;
    float min_b = gp;
    // OS-PERF3: the window maximum `pm` changes only when a larger peak arrives or the old one leaves, so the gain it asks for (a division, a float to integer conversion) is kept from the last
    // sample until pm differs: the same value, computed less often (bit for bit: the arithmetic is the same expression on the same input).
    float last_pm = -1.0f;
    std::uint32_t last_mq = kUnityQi;
    for (std::uint32_t k = 0; k < kBlock; ++k) {
      // The slow stage: the threshold / attack / release on the stereo-linked peak, no look-ahead (what it lets through is the fast stage's).
      const float l = left[k];
      const float r = right[k];
      const float p = pk[k];
      float xl = l;
      float xr = r;
      float pp = p;
      if (slow) {
        env += (p > env ? ca : cr) * (p - env);
        if (env > ts) {
          const float g = ts / env;  // ts >= 0.01 here, so g is a finite number in (0, 1)
          xl = l * g;
          xr = r * g;
          pp = p * g;  // = max(|xl|, |xr|): the product is monotonic
          min_s = g < min_s ? g : min_s;
        }
      }

      // The fast stage: the peak of the last kBlock samples of the slow stage's result (this block's so far, the last block's from the next sample on), the gain that holds it to the
      // ceiling, Q24, floored; the moving average of the last kBlock of those is the attack; the release can only be slower than it.
      run = pp > run ? pp : run;
      const float tail = sfx_[k + 1u];
      sfx_[k] = pp;
      const float pm = tail > run ? tail : run;
      if (pm != last_pm) {
        last_pm = pm;
        last_mq = pm > kCeiling ? static_cast<std::uint32_t>((kGainCeiling / pm) * kUnityQ) : kUnityQi;
      }
      const std::uint32_t mq = last_mq;
      sum += mq;
      sum -= hist_[k];
      hist_[k] = mq;
      const float avg = static_cast<float>(sum) * kSumScale;
      const float rel = gp + cb * (1.0f - gp);
      gp = avg < rel ? avg : rel;
      if (mq != kUnityQi) {  // (the gain only goes down on a sample whose window needed a reduction: that is where its lowest is)
        min_b = gp < min_b ? gp : min_b;
      }

      // The output: the sample kLatency behind (k + 1 of the block before, or this block's first for the last), read before this block's own sample k takes its place.
      const std::uint32_t j = (k + 1u) & (kBlock - 1u);
      const float dl = ring_l[j];
      const float dr = ring_r[j];
      ring_l[k] = xl;
      ring_r[k] = xr;
      left[k] = dl * gp;
      right[k] = dr * gp;
    }

    // This block's peaks become the suffix maxima the next block's windows start from.
    float s = 0.0f;
    for (std::uint32_t k = kBlock; k-- > 0u;) {
      s = sfx_[k] > s ? sfx_[k] : s;
      sfx_[k] = s;
    }
    gp_ = gp > kSnapGain ? 1.0f : gp;  // the release lands on exactly 1.0 (a float stuck a hair below it would never be transparent again); G is min'ed in again at the next sample
    sum_ = sum;
    gmin = min_s * min_b;
  }
  env_ = env < kEnvFloor ? 0.0f : env;  // exact silence, and no denormal (a block of the follower's decay never gets near one)

  // The report: the lowest gain of the window, published at its end.
  win_min_ = gmin < win_min_ ? gmin : win_min_;
  if (++win_blocks_ >= kMeterWindowBlocks) {
    const std::uint32_t q = static_cast<std::uint32_t>(win_min_ * 65536.0f + 0.5f);
    hold_ = q < hold_ ? q : hold_;
    ++windows_;
    reduced_ += win_min_ < kReducedBelow ? 1u : 0u;
    pub_now_.store(q, std::memory_order_relaxed);
    pub_hold_.store(hold_, std::memory_order_relaxed);
    pub_reduced_.store(reduced_, std::memory_order_relaxed);
    pub_windows_.store(windows_, std::memory_order_relaxed);
    win_blocks_ = 0;
    win_min_ = 1.0f;
  }
}

bool MasterLimiter::finite() const noexcept {
  bool ok = __builtin_isfinite(env_) && __builtin_isfinite(gp_);
  for (std::uint32_t i = 0; i < kBlock; ++i) {
    ok = ok && __builtin_isfinite(ring_[0][i]) && __builtin_isfinite(ring_[1][i]) && __builtin_isfinite(sfx_[i]);
  }
  return ok && __builtin_isfinite(sfx_[kBlock]);
}

}  // namespace o3os::audio
