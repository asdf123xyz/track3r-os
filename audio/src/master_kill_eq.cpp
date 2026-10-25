// SPDX-License-Identifier: GPL-3.0-or-later
//
// The master kill EQ (OS-DJ A3 / A3b): see audio/master_kill_eq.h for what it is and for the values. This file is the arithmetic.
//
// One channel, per sample, with the second-order Butterworth section in the trapezoidal state-variable form (svf_bp / svf_lp below: bp and lp, x = hp + k bp + lp, k = sqrt 2, g = tan(pi fc / fs)):
//   LP4 of the first corner: two sections in a row, the second fed with the first's lp.   lowp  = lp of section 2
//   an LR4 split's bands are lp4 and  hp4 = ap2 - lp4  with  ap2 = x - 2 k bp1  (lp4 + hp4 is the second-order all-pass of the corner, and ap2 is what section 1 gives for free)
//   rest = x - 2 k bp1 - lowp                                (the first split's high band)
//   mid  = lp of section 4 fed with lp of section 3 fed with rest;  mid + high = rest - 2 k bp3       (the second split's bands: high = that minus mid)
//   low  = lowp - 2 k bp5     with section 5 fed with lowp  (the all-pass of the second corner on the LOW band)
//   out  = cd x + cl low + cm mid + ch (mid + high)           (cd = 1 - wet, cl = wet gl, cm = wet (gm - gh), ch = wet gh: high = (mid + high) - mid)
// Three sections of 11 flops and two of 9, three differences of 2, the mix of 7. The coefficients ramp linearly per sample from the block's start values to its end values, and the sample i of a block uses the
// value (i + 1) steps in, so the last one lands on the end value (the hand-over to bypass is then exact to the float rounding of 64 additions, under 2e-6).

#include <o3os/audio/master_kill_eq.h>

namespace o3os::audio {

namespace {

constexpr float kK = 1.41421356237f;  // 1 / Q of a second-order Butterworth section
constexpr float kK2 = 2.0f * kK;

// The section's coefficients from g = tan(pi fc / fs), fs = the engine's rate (44,100 Hz, or 44,117.647 Hz with -DO3OS_RATE_44100=OFF; the two corner constants below are warped at 44,117.647 Hz and stay fixed, so at 44,100 Hz the corners sit 0.04 % low, far under anything audible): a1 = 1 / (1 + g (g + k)), a2 = g a1, a3 = g a2; and c1 = 2 a1 - 1, c2 = 2 a2 for the sections that
// do not give their band-pass out (their first integrator is then c1 s1 + c2 v3, one add fewer than going through the band-pass).
struct Co {
  float a1;
  float a2;
  float a3;
  float c1;
  float c2;
};
constexpr Co co(float g) {
  const float a1 = 1.0f / (1.0f + g * (g + kK));
  const float a2 = g * a1;
  return {a1, a2, g * a2, 2.0f * a1 - 1.0f, 2.0f * a2};
}
// A state under this is 0 (once a block, at the block's end): -400 dB, far under anything audible, and a state that decays to the denormals cannot stick there at 1 ulp (the way send_fx_util.h flushes).
constexpr float kFlushBelow = 1.0e-20f;
inline float flush(float v) noexcept { return __builtin_fabsf(v) < kFlushBelow ? 0.0f : v; }

constexpr Co kLo = co(0.0128184000f);  // MasterKillEq::kLowHz, 180 Hz
constexpr Co kHi = co(0.2020713158f);  // MasterKillEq::kHighHz, 2.8 kHz

// One section (Simper's form): v3 = x - s2, v1 = a1 s1 + a2 v3 (the band-pass), v2 = s2 + a2 s1 + a3 v3 (the low-pass), s1 = 2 v1 - s1, s2 = 2 v2 - s2. With t = a2 s1 + a3 v3: v2 = s2 + t and
// the new s2 = v2 + t.
// With the band-pass out (bp) and the low-pass out (lp):
inline void svf_bp(float x, float& s1, float& s2, const Co& c, float& bp, float& lp) noexcept {
  const float v3 = x - s2;
  bp = c.a1 * s1 + c.a2 * v3;
  const float t = c.a2 * s1 + c.a3 * v3;
  lp = s2 + t;
  s1 = (bp + bp) - s1;
  s2 = lp + t;
}
// ... and with the low-pass only (the band-pass is not wanted: s1 goes straight to its new value).
inline float svf_lp(float x, float& s1, float& s2, const Co& c) noexcept {
  const float v3 = x - s2;
  const float t = c.a2 * s1 + c.a3 * v3;
  const float lp = s2 + t;
  s1 = c.c1 * s1 + c.c2 * v3;
  s2 = lp + t;
  return lp;
}

struct Mix {
  float d;  // the input (the dry term)
  float l;  // LOW, after its all-pass
  float m;  // MID
  float h;  // MID + HIGH (the second split's sum)
};

// One channel of one block. kRamp false: the coefficients do not move and the wet weight is 1 (the dry term is 0), a kill held.
template <bool kRamp>
void run(float* x, float* st, Mix c, const Mix& step) noexcept {
  float s0 = st[0];
  float s1 = st[1];
  float s2 = st[2];
  float s3 = st[3];
  float s4 = st[4];
  float s5 = st[5];
  float s6 = st[6];
  float s7 = st[7];
  float s8 = st[8];
  float s9 = st[9];
  for (std::uint32_t i = 0; i < MasterKillEq::kBlock; ++i) {
    const float in = x[i];
    float bp1, lp1, bp3, lp3, bp5, lp5;
    svf_bp(in, s0, s1, kLo, bp1, lp1);
    const float lowp = svf_lp(lp1, s2, s3, kLo);
    const float rest = (in - kK2 * bp1) - lowp;
    svf_bp(rest, s4, s5, kHi, bp3, lp3);
    const float mid = svf_lp(lp3, s6, s7, kHi);
    const float midhigh = rest - kK2 * bp3;
    svf_bp(lowp, s8, s9, kHi, bp5, lp5);
    const float low = lowp - kK2 * bp5;
    if constexpr (kRamp) {
      c.d += step.d;
      c.l += step.l;
      c.m += step.m;
      c.h += step.h;
      x[i] = c.d * in + c.l * low + c.m * mid + c.h * midhigh;
    } else {
      x[i] = c.l * low + c.m * mid + c.h * midhigh;
    }
  }
  st[0] = flush(s0);
  st[1] = flush(s1);
  st[2] = flush(s2);
  st[3] = flush(s3);
  st[4] = flush(s4);
  st[5] = flush(s5);
  st[6] = flush(s6);
  st[7] = flush(s7);
  st[8] = flush(s8);
  st[9] = flush(s9);
}

// A kill HELD (the weights arrive and do not move): only the sections the live terms need run. Sections 1 and 2 (LOW and the rest) always run; S3 is the second split (midhigh, and the feed of
// section 4), S4 the MID band, S5 the all-pass on LOW. A term whose weight is exactly 0 is not computed, and adding a zero product changes nothing, so the output is bit for bit the full loop's.
// The sections left out are cleared (the caller marks them stale and primes them when a ramp next runs).
template <bool kS3, bool kS4, bool kS5>
void run_held(float* x, float* st, const Mix& c) noexcept {
  float s0 = st[0];
  float s1 = st[1];
  float s2 = st[2];
  float s3 = st[3];
  float s4 = st[4];
  float s5 = st[5];
  float s6 = st[6];
  float s7 = st[7];
  float s8 = st[8];
  float s9 = st[9];
  for (std::uint32_t i = 0; i < MasterKillEq::kBlock; ++i) {
    const float in = x[i];
    float bp1, lp1;
    svf_bp(in, s0, s1, kLo, bp1, lp1);
    const float lowp = svf_lp(lp1, s2, s3, kLo);
    float y = 0.0f;
    if constexpr (kS5) {
      float bp5, lp5;
      svf_bp(lowp, s8, s9, kHi, bp5, lp5);
      y = c.l * (lowp - kK2 * bp5);
    }
    if constexpr (kS3) {
      const float rest = (in - kK2 * bp1) - lowp;
      float bp3, lp3;
      svf_bp(rest, s4, s5, kHi, bp3, lp3);
      if constexpr (kS4) {
        y += c.m * svf_lp(lp3, s6, s7, kHi);
      }
      y += c.h * (rest - kK2 * bp3);
    }
    x[i] = y;
  }
  st[0] = flush(s0);
  st[1] = flush(s1);
  st[2] = flush(s2);
  st[3] = flush(s3);
  // (a section left out is cleared: it holds no signal, and a state is then exactly 0 over silence like the others)
  st[4] = kS3 ? flush(s4) : 0.0f;
  st[5] = kS3 ? flush(s5) : 0.0f;
  st[6] = kS3 && kS4 ? flush(s6) : 0.0f;
  st[7] = kS3 && kS4 ? flush(s7) : 0.0f;
  st[8] = kS5 ? flush(s8) : 0.0f;
  st[9] = kS5 ? flush(s9) : 0.0f;
}

// A value moves toward its target by kStep and stops on it.
inline float toward(float c, float t) noexcept {
  if (c < t) {
    return c + MasterKillEq::kStep < t ? c + MasterKillEq::kStep : t;
  }
  if (c > t) {
    return c - MasterKillEq::kStep > t ? c - MasterKillEq::kStep : t;
  }
  return c;
}

// The mix coefficients of the wet weight w and the band gains g (low, mid, high).
inline Mix mix_of(float w, const float* g) noexcept { return {1.0f - w, w * g[0], w * (g[1] - g[2]), w * g[2]}; }

}  // namespace

bool MasterKillEq::set(float mask) noexcept {
  if (!(mask >= 0.0f && mask <= static_cast<float>(kAll))) {  // (a NaN fails both)
    return false;
  }
  const auto m = static_cast<unsigned>(mask);
  if (static_cast<float>(m) != mask) {
    return false;
  }
  mask_ = static_cast<std::uint8_t>(m);
  return true;
}

void MasterKillEq::reset(unsigned mask) noexcept {
  mask_ = static_cast<std::uint8_t>(mask & kAll);
  for (float& g : gain_) {
    g = 1.0f;
  }
  wet_ = 0.0f;
  armed_ = false;
  stale_ = 0;
  for (auto& ch : st_) {
    for (float& s : ch) {
      s = 0.0f;
    }
  }
}

void MasterKillEq::process(float* left, float* right, std::uint32_t n) noexcept {
  if (n != kBlock) {
    return;
  }
  if (idle()) {
    // No kill and everything back at rest: the stage is bypassed from the next block (the states belong to no signal from here on).
    armed_ = false;
    if (bypass_ != nullptr) {
      *bypass_ = true;
    }
    return;
  }
  // The gains and the wet weight at the start and at the end of this block: each moves toward its target by kStep and stops on it.
  float g0[3];
  float g1[3];
  for (unsigned b = 0; b < 3u; ++b) {
    g0[b] = gain_[b];
    g1[b] = toward(g0[b], ((mask_ >> b) & 1u) != 0u ? 0.0f : 1.0f);
    gain_[b] = g1[b];
  }
  const float w0 = wet_;
  const float w1 = toward(w0, mask_ != 0u ? 1.0f : 0.0f);
  wet_ = w1;
  const Mix c0 = mix_of(w0, g0);
  const Mix c1 = mix_of(w1, g1);
  const bool held = w0 == 1.0f && w1 == 1.0f && c0.l == c1.l && c0.m == c1.m && c0.h == c1.h;
  if (!armed_) {
    // Starting (from bypass): the states of a signal that has been at its first sample for ever. The three sections that follow a low-pass hold its value (their integrator 2), the others are 0.
    armed_ = true;
    stale_ = 0;
    float* first[2] = {left, right};
    for (unsigned c = 0; c < 2u; ++c) {
      for (float& s : st_[c]) {
        s = 0.0f;
      }
      st_[c][1] = st_[c][3] = st_[c][9] = first[c][0];
    }
  }
  if (held) {
    // A kill held: nothing moves, and the input is not in the output. The weights (cl, cm, ch) = (g_low, g_mid - g_high, g_high) of the mask say which sections matter: LOW killed alone leaves
    // only the high half of the second split (sections 1 to 3), LOW + MID that and the MID band (1 to 4), LOW + HIGH the MID band alone (1 to 4), MID + HIGH the all-passed LOW alone (1, 2, 5),
    // MID, HIGH and all three run the full loop (all three is silence, and stays the full loop: its states stay alive for a release). A section left out is stale: it is primed when the next ramp runs.
    switch (mask_) {
      case kLow:
        run_held<true, false, false>(left, st_[0], c1);
        run_held<true, false, false>(right, st_[1], c1);
        stale_ = kStaleS4 | kStaleS5;
        break;
      case kLow | kMid:
      case kLow | kHigh:
        run_held<true, true, false>(left, st_[0], c1);
        run_held<true, true, false>(right, st_[1], c1);
        stale_ = kStaleS5;
        break;
      case kMid | kHigh:
        run_held<false, false, true>(left, st_[0], c1);
        run_held<false, false, true>(right, st_[1], c1);
        stale_ = kStaleS3 | kStaleS4;
        break;
      default:  // MID, HIGH, all three
        run<false>(left, st_[0], c1, Mix{});
        run<false>(right, st_[1], c1, Mix{});
        stale_ = 0;
        break;
    }
    return;
  }
  if (stale_ != 0u) {
    // A ramp after sections were left out: each one starts from the state of the signal it follows (its first integrator 0, the second the feeding section's own, which is the low-pass of that
    // signal), as an entry from bypass starts all of them. What they feed has a weight of exactly 0 at this point and ramps up from there, so the settling (a few samples of a corner at 2.8 kHz; LOW's
    // all-pass follows a signal under 180 Hz) is under 2 % of a band's level.
    for (auto& ch : st_) {
      if ((stale_ & kStaleS5) != 0u) {
        ch[8] = 0.0f;
        ch[9] = ch[3];
      }
      if ((stale_ & kStaleS3) != 0u) {
        ch[4] = 0.0f;
        ch[5] = 0.0f;
      }
      if ((stale_ & kStaleS4) != 0u) {
        ch[6] = 0.0f;
        ch[7] = ch[5];
      }
    }
    stale_ = 0;
  }
  constexpr float kInv = 1.0f / static_cast<float>(kBlock);
  const Mix step{(c1.d - c0.d) * kInv, (c1.l - c0.l) * kInv, (c1.m - c0.m) * kInv, (c1.h - c0.h) * kInv};
  run<true>(left, st_[0], c0, step);
  run<true>(right, st_[1], c0, step);
}

}  // namespace o3os::audio
