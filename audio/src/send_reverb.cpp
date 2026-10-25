// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/audio/send_reverb.h>

#include <o3os/audio/hot.h>
#include <o3os/audio/send_fx_util.h>

#include <cstring>

namespace o3os::audio {

namespace {

using sendfx_util::flush;
using sendfx_util::trunc16;

enum : unsigned { kPd, kIn0, kIn1, kIn2, kIn3, kAl1, kDl1, kAl2, kDl2, kAr1, kDr1, kAr2, kDr2 };
static_assert(kDr2 + 1u == SendReverb::kLines, "one row of kRingFull per line");

// The line ids of the two branches: modulated allpass, delay, allpass, delay.
constexpr unsigned kAp1Id[2] = {kAl1, kAr1};
constexpr unsigned kD1Id[2] = {kDl1, kDr1};
constexpr unsigned kAp2Id[2] = {kAl2, kAr2};
constexpr unsigned kD2Id[2] = {kDl2, kDr2};

// The paper's modulated allpasses' nominal delays (samples at the FULL rate) and the LFO's excursion: the ring of each is nominal + excursion + 2.
constexpr std::uint32_t kModNom[2] = {672u, 908u};
constexpr std::uint32_t kExcFull = 16u;
static_assert(SendReverb::kRingFull[kAl1] == kModNom[0] + kExcFull + 2u && SendReverb::kRingFull[kAr1] == kModNom[1] + kExcFull + 2u, "the modulated allpasses' rings");
constexpr std::uint32_t kWinMax = SendReverb::kBlock + 2u * kExcFull + 2u;

// The output taps (the paper's output table): seven for each channel, a line, the offset in samples behind its write position at the FULL rate and a sign. The two channels have the same
// pattern of signs, + + - + - - -, which the output loop below is written for.
struct Tap {
  std::uint8_t line;
  std::uint16_t k;
  std::int8_t sign;
};
constexpr unsigned kTapsPerChannel = 7;
constexpr Tap kTaps[2][kTapsPerChannel] = {
    {{kDr1, 266, 1}, {kDr1, 2974, 1}, {kAr2, 1913, -1}, {kDr2, 1996, 1}, {kDl1, 1990, -1}, {kAl2, 187, -1}, {kDl2, 1066, -1}},
    {{kDl1, 353, 1}, {kDl1, 3627, 1}, {kAl2, 1228, -1}, {kDl2, 2673, 1}, {kDr1, 2111, -1}, {kAr2, 335, -1}, {kDr2, 121, -1}},
};
constexpr bool taps_fit() {
  constexpr std::int8_t pattern[kTapsPerChannel] = {1, 1, -1, 1, -1, -1, -1};
  for (unsigned c = 0; c < 2u; ++c) {
    for (unsigned i = 0; i < kTapsPerChannel; ++i) {
      if (kTaps[c][i].sign != pattern[i] || kTaps[c][i].k < SendReverb::kBlock || kTaps[c][i].k + SendReverb::kBlock > SendReverb::kRingFull[kTaps[c][i].line]) {
        return false;
      }
    }
  }
  return true;
}
static_assert(taps_fit(), "every tap reads at least a block behind the write position and a block short of the ring's end (it is read after the block's writes), and the signs are the pattern the output loop sums");

// The start of each line in the memory.
// (each line starts on a 4-byte boundary: the staging copies move two samples a word)
constexpr std::uint32_t offset_of(unsigned line) {
  std::uint32_t o = 0;
  for (unsigned i = 0; i < line; ++i) {
    o += (static_cast<std::uint32_t>(SendReverb::kRingFull[i]) + 1u) & ~1u;
  }
  return o;
}
static_assert(offset_of(SendReverb::kLines) == SendReverb::kMemSamples, "the lines fill the memory exactly");

// Levels. The send is scaled down by kIn before it enters the plate (headroom in the int16 lines), the taps' sum is scaled by kOut and by sqrt(1 - decay^2), which keeps the return's level
// near constant while SIZE moves the decay (a longer tail holds more energy).
constexpr float kIn = 0.25f;
constexpr float kOut = 1.3f;

constexpr float kFs = static_cast<float>(hal::audio::sample_rate_mhz()) * 1.0e-3f;
constexpr float kBlockSec = static_cast<float>(hal::audio::kBlock) / kFs;
constexpr float kLfoInc[2] = {0.8f * kBlockSec, 1.1f * kBlockSec};  // the LFOs' phase step a block
constexpr float kSlewK = 0.1f;                                      // the per-block slew of the controls (about 14 ms)

// The 23-tap half-band filter (Kaiser window, beta 5; the centre tap is 0.5, the even ones are zero, the odd ones sum to 0.25 a side so the gain at DC is 1): flat to 7.7 kHz, -55 dB from 14.3 kHz.
constexpr float kHb[6] = {0.312388033f, -0.089587838f, 0.039210421f, -0.016676371f, 0.005727762f, -0.001062007f};  // taps +-1, +-3, +-5, +-7, +-9, +-11

// A parabola through 0 at the ends of the period and +-1 at the quarters: a sine to a few percent, for a modulation that nobody listens to as a tone.
O3OS_AUDIO_INLINE float lfo_wave(float p) noexcept {
  const float q = p < 0.5f ? 2.0f * p : 2.0f * p - 2.0f;  // -1 .. 1
  const float a = q < 0.0f ? -q : q;
  return 4.0f * q * (1.0f - a);
}

}  // namespace

// ---- Setup -------------------------------------------------------------------------------------------------------------------------------------------

// The lines, the offsets and the derived constants for the rate in force; leaves the memory as it is.
void SendReverb::configure() noexcept {
  const std::uint32_t sh = shift_;
  const std::uint32_t half = (1u << sh) >> 1;  // rounding: x / 2^sh to nearest, ties up
  nb_ = kBlock >> sh;
  exc_ = kExcFull >> sh;
  em_ = static_cast<float>(exc_ - 1u);
  for (unsigned s = 0; s < 2u; ++s) {
    nom_[s] = (kModNom[s] + half) >> sh;
  }
  std::uint32_t longest = 0;
  for (unsigned i = 0; i < kLines; ++i) {
    Line& ln = line_[i];
    ln.base = mem_ != nullptr ? mem_ + offset_of(i) : nullptr;
    ln.len = (i == kAl1 || i == kAr1) ? nom_[i == kAl1 ? 0 : 1] + exc_ + 2u : (static_cast<std::uint32_t>(kRingFull[i]) + half) >> sh;
    ln.pos = 0;
    longest = ln.len > longest ? ln.len : longest;
  }
  for (unsigned c = 0; c < 2u; ++c) {
    for (unsigned i = 0; i < kTapsPerChannel; ++i) {
      tap_k_[c][i] = (static_cast<std::uint32_t>(kTaps[c][i].k) + half) >> sh;
    }
  }
  idle_after_ = longest << sh;  // frames: a ring's worth at the internal rate
  lp_[0] = lp_[1] = 0.0f;
  lfo_m_[0] = lfo_m_[1] = 0.0f;
  for (float& v : dec_hist_) v = 0.0f;
  for (auto& h : int_hist_) {
    for (float& v : h) v = 0.0f;
  }
  pd_cur_ = pd_tgt_ = 0;  // the first block takes its target at once
  dirty_ = true;
  zero_run_ = 0;
}

void SendReverb::attach(std::int16_t* mem, std::uint32_t samples) noexcept {
  mem_ = samples >= kMemSamples ? mem : nullptr;
  for (std::uint32_t i = 0; mem_ != nullptr && i < kMemSamples; ++i) {
    mem_[i] = 0;
  }
  size_ = damp_ = predelay_ = sendfx::kReverbDefault;
  diffusion_ = sendfx::kReverbDiffusionDefault;
  quality_ = 0;
  lfo_phase_[0] = 0.0f;
  lfo_phase_[1] = 0.5f;  // the two LFOs start half a turn apart
  shift_ = 1;            // HALF
  clearing_ = false;
  configure();
  decay_ = 0.0f;  // the first block's controls() sets every control at once
  idle_ = mem_ != nullptr;  // the memory is all zero: nothing to do until the first sound
  zero_run_ = idle_after_;
}

// Start clearing the memory a slice a block (run() does it); the layout is configured at once for the rate in force, so that what follows starts from an empty plate.
void SendReverb::begin_clear() noexcept {
  if (mem_ == nullptr) {
    return;
  }
  configure();
  lfo_phase_[0] = 0.0f;  // the state is as after attach() (send_reverb.h): the modulation starts from its beginning, so that a render that begins from a cleared plate (the export,
  lfo_phase_[1] = 0.5f;  // OS-WAVEXP) is the same whatever the plate was doing before
  decay_ = 0.0f;
  if (idle_ && !clearing_) {  // a plate that sleeps holds nothing (the lines are all zero already), but its modulation stopped wherever it was: it wakes from the beginning too
    zero_run_ = idle_after_;
    return;
  }
  clearing_ = true;
  clear_pos_ = 0;
  idle_ = false;
}

bool SendReverb::set_param(unsigned field, float v) noexcept {
  switch (field) {
    case sendfx::kClearField:
      if (v != 1.0f) return false;
      begin_clear();
      return true;
    case sendfx::reverb::Size:
      if (!(v >= 0.0f && v <= 100.0f)) return false;
      size_ = v;
      break;
    case sendfx::reverb::Damp:
      if (!(v >= 0.0f && v <= 100.0f)) return false;
      damp_ = v;
      break;
    case sendfx::reverb::Predelay:
      if (!(v >= 0.0f && v <= 100.0f)) return false;
      predelay_ = v;
      break;
    case sendfx::reverb::Diffusion:
      if (!(v >= 0.0f && v <= 100.0f)) return false;
      diffusion_ = v;
      break;
    case sendfx::reverb::Quality:
      if (v != 0.0f && v != 1.0f) return false;
      quality_ = v != 0.0f ? 1u : 0u;
      break;
    default:
      return false;
  }
  dirty_ = true;
  return true;
}

bool SendReverb::finite() const noexcept {
  bool ok = __builtin_isfinite(decay_) && __builtin_isfinite(damp_q_) && __builtin_isfinite(out_gain_) && __builtin_isfinite(g_t1_);
  for (unsigned s = 0; s < 2u; ++s) {
    ok = ok && __builtin_isfinite(lp_[s]) && __builtin_isfinite(lfo_m_[s]) && __builtin_isfinite(lfo_phase_[s]);
  }
  for (const float v : dec_hist_) ok = ok && __builtin_isfinite(v);
  for (const auto& h : int_hist_) {
    for (const float v : h) ok = ok && __builtin_isfinite(v);
  }
  return ok;
}

// The controls for this block: targets from the parameters, one slew step towards them (all of them at once on the first block).
void SendReverb::controls() noexcept {
  const float u_size = size_ * 0.01f;
  const float u_damp = damp_ * 0.01f;
  const float decay_t = 0.25f + 0.66f * u_size;
  float pole = 0.0005f + 0.85f * u_damp * u_damp;
  pole = shift_ != 0u ? pole * pole : pole;  // the same colour in hertz at half the rate
  const float q_t = 1.0f - pole;
  const float k = diffusion_ * 0.0125f;  // 1 at the default 0.68 (diffusion 80)
  const bool first = decay_ == 0.0f;
  if (first) {
    decay_ = decay_t;
    damp_q_ = q_t;
    g_in1_ = 0.75f * k;
    g_in2_ = 0.625f * k;
    g_t1_ = 0.70f * k;
  } else {
    decay_ = sendfx_util::slew(decay_, decay_t, kSlewK);
    damp_q_ = sendfx_util::slew(damp_q_, q_t, kSlewK);
    g_in1_ = sendfx_util::slew(g_in1_, 0.75f * k, kSlewK);
    g_in2_ = sendfx_util::slew(g_in2_, 0.625f * k, kSlewK);
    g_t1_ = sendfx_util::slew(g_t1_, 0.70f * k, kSlewK);
  }
  const float g2 = decay_ + 0.15f;
  g_t2_ = g2 < 0.25f ? 0.25f : (g2 > 0.5f ? 0.5f : g2);
  out_gain_ = kOut * __builtin_sqrtf(1.0f - decay_ * decay_);
  if (dirty_) {
    const Line& pd = line_[kPd];
    const float span = static_cast<float>(pd.len - nb_);
    pd_tgt_ = nb_ + static_cast<std::uint32_t>(predelay_ * 0.01f * span + 0.5f);
    pd_tgt_ = pd_tgt_ > pd.len ? pd.len : pd_tgt_;
    if (pd_cur_ == 0u) {
      pd_cur_ = pd_tgt_;
    }
    dirty_ = false;
  }
}

// ---- Ring access -------------------------------------------------------------------------------------------------------------------------------------

// n slots of a line from slot `start` (wrapping) into dst, and where the first of them is: dst + (start & 1), so that the ring address and the scratch address are both on a word
// boundary together and two slots move a word (the scratch has two spare samples). n is at most the line's length.
const std::int16_t* SendReverb::copy_out(const Line& ln, std::uint32_t start, std::uint32_t n, std::int16_t* dst) noexcept {
  std::int16_t* const p = dst + (start & 1u);
  const std::uint32_t head = ln.len - start;
  const std::uint32_t first = n < head ? n : head;
  const std::int16_t* const src = ln.base + start;
  std::uint32_t i = 0;
  if ((start & 1u) != 0u && first != 0u) {
    p[0] = src[0];
    i = 1;
  }
#pragma GCC unroll 4
  for (; i + 2u <= first; i += 2u) {
    std::uint32_t w;
    std::memcpy(&w, src + i, sizeof w);
    std::memcpy(p + i, &w, sizeof w);
  }
  if (i < first) {
    p[i] = src[i];
  }
  for (std::uint32_t j = first; j < n; ++j) {  // the part after the ring's end, from its start
    p[j] = ln.base[j - first];
  }
  return p;
}

// n new slots, src[0..n) (which sits at the parity of the line's write position, see wbuf), into a line at its write position (wrapping), which then moves on by n. Returns the OR of the
// words written (zero: all zero).
std::uint32_t SendReverb::copy_in(Line& ln, const std::int16_t* src, std::uint32_t n) noexcept {
  const std::uint32_t pos = ln.pos;
  const std::uint32_t head = ln.len - pos;
  const std::uint32_t first = n < head ? n : head;
  std::int16_t* const dst = ln.base + pos;
  std::uint32_t any = 0;
  std::uint32_t i = 0;
  if ((pos & 1u) != 0u && first != 0u) {
    dst[0] = src[0];
    any |= static_cast<std::uint16_t>(src[0]);
    i = 1;
  }
#pragma GCC unroll 4
  for (; i + 2u <= first; i += 2u) {
    std::uint32_t w;
    std::memcpy(&w, src + i, sizeof w);
    std::memcpy(dst + i, &w, sizeof w);
    any |= w;
  }
  if (i < first) {
    dst[i] = src[i];
    any |= static_cast<std::uint16_t>(src[i]);
  }
  for (std::uint32_t j = first; j < n; ++j) {
    ln.base[j - first] = src[j];
    any |= static_cast<std::uint16_t>(src[j]);
  }
  ln.pos = pos + n >= ln.len ? pos + n - ln.len : pos + n;
  return any;
}

// ---- The fused blocks ----------------------------------------------------------------------------------------------------------------------------------
// Each is one loop over the block with the signal in registers from stage to stage (the float working arrays and the conversions between stages cost more than the arithmetic did), all the
// lines' old content staged first and the new content copied back after. A Schroeder allpass whose delay is the line's length: z = the slot's old content (the delay's output), w = x + g z
// is stored, y = z - g w is the output. A plain delay: the slot's old content is the output, the input takes its place.

// The input: the pre-delay's output (cross-faded between two delays when the pre-delay moved) scaled down, through the four allpasses; the result is d_.
void SendReverb::input_block(const std::int16_t* zp, const std::int16_t* zq, bool fade, std::uint32_t n) noexcept {
  Line& l0 = line_[kIn0];
  Line& l1 = line_[kIn1];
  Line& l2 = line_[kIn2];
  Line& l3 = line_[kIn3];
  const std::int16_t* const z0 = view(l0, l0.pos, n, z_[1]);
  const std::int16_t* const z1 = view(l1, l1.pos, n, z_[2]);
  const std::int16_t* const z2 = view(l2, l2.pos, n, z_[3]);
  const std::int16_t* const z3 = view(l3, l3.pos, n, z_[4]);
  std::int16_t* const w0 = wbuf(0, l0);
  std::int16_t* const w1 = wbuf(1, l1);
  std::int16_t* const w2 = wbuf(2, l2);
  std::int16_t* const w3 = wbuf(3, l3);
  const float ga = g_in1_;
  const float gb = g_in2_;
  const float step = 1.0f / static_cast<float>(n);
  float f = step;
  float* const d = d_;
  for (std::uint32_t i = 0; i < n; ++i) {
    float x;
    if (fade) {
      x = kIn * ((1.0f - f) * static_cast<float>(zp[i]) + f * static_cast<float>(zq[i]));
      f += step;
    } else {
      x = kIn * static_cast<float>(zp[i]);
    }
    float z = static_cast<float>(z0[i]);
    float w = x + ga * z;
    x = z - ga * w;
    w0[i] = static_cast<std::int16_t>(trunc16(w));
    z = static_cast<float>(z1[i]);
    w = x + ga * z;
    x = z - ga * w;
    w1[i] = static_cast<std::int16_t>(trunc16(w));
    z = static_cast<float>(z2[i]);
    w = x + gb * z;
    x = z - gb * w;
    w2[i] = static_cast<std::int16_t>(trunc16(w));
    z = static_cast<float>(z3[i]);
    w = x + gb * z;
    x = z - gb * w;
    w3[i] = static_cast<std::int16_t>(trunc16(w));
    d[i] = x;
  }
  nz_ |= copy_in(l0, w0, n);
  nz_ |= copy_in(l1, w1, n);
  nz_ |= copy_in(l2, w2, n);
  nz_ |= copy_in(l3, w3, n);
}

// One branch of the tank: the input and the other branch's last delay's output, through the modulated allpass (gain -g_t1: its delay read at nominal + m(t), m a slow LFO of at most exc - 1
// samples, linearly interpolated from a window of n + 2 exc + 2 slots copied in once, m moving linearly across the block between its values at the block's two ends), the first delay, the
// damping low-pass and the decay, the second allpass and the last delay. `cross` is the other branch's last delay's old content (staged before either branch wrote).
void SendReverb::tank_block(unsigned side, const std::int16_t* cross, std::uint32_t n) noexcept {
  Line& lm = line_[kAp1Id[side]];
  Line& l1 = line_[kD1Id[side]];
  Line& l2 = line_[kAp2Id[side]];
  Line& l3 = line_[kD2Id[side]];
  const std::uint32_t e = exc_;
  const std::int16_t* const win = view(lm, start_of(lm, nom_[side] + e + 1u), n + 2u * e + 2u, win_);
  const std::int16_t* const zd1 = view(l1, l1.pos, n, z_[5]);
  const std::int16_t* const za2 = view(l2, l2.pos, n, z_[6]);
  std::int16_t* const wam = wbuf(0, lm);
  std::int16_t* const wd1 = wbuf(1, l1);
  std::int16_t* const wa2 = wbuf(2, l2);
  std::int16_t* const wd2 = wbuf(3, l3);
  lfo_phase_[side] += kLfoInc[side];
  lfo_phase_[side] = lfo_phase_[side] >= 1.0f ? lfo_phase_[side] - 1.0f : lfo_phase_[side];
  const float m1 = em_ * lfo_wave(lfo_phase_[side]);
  float m = lfo_m_[side];
  const float dm = (m1 - m) * (1.0f / static_cast<float>(n));
  const float gm = -g_t1_;
  const float g2 = g_t2_;
  const float q = damp_q_;
  const float decay = decay_;
  float lp = lp_[side];
  const float* const d = d_;
  for (std::uint32_t i = 0; i < n; ++i) {
    // the sum, and the modulated allpass
    float x = d[i] + static_cast<float>(cross[i]);
    const float u = static_cast<float>(i + e + 1u) - m;  // the read position in the window, > 0
    const std::uint32_t j = static_cast<std::uint32_t>(u);
    const float fr = u - static_cast<float>(j);
    const float c0 = static_cast<float>(win[j]);
    const float c1 = static_cast<float>(win[j + 1u]);
    float z = c0 + fr * (c1 - c0);
    float w = x + gm * z;
    x = z - gm * w;
    wam[i] = static_cast<std::int16_t>(trunc16(w));
    m += dm;
    // the first delay (its input is the allpass's output), the damping low-pass and the decay
    wd1[i] = static_cast<std::int16_t>(trunc16(x));
    lp += q * (static_cast<float>(zd1[i]) - lp);
    x = decay * lp;
    // the second allpass, and the last delay's input
    z = static_cast<float>(za2[i]);
    w = x + g2 * z;
    x = z - g2 * w;
    wa2[i] = static_cast<std::int16_t>(trunc16(w));
    wd2[i] = static_cast<std::int16_t>(trunc16(x));
  }
  lfo_m_[side] = m1;
  lp_[side] = flush(lp);
  nz_ |= copy_in(lm, wam, n);
  nz_ |= copy_in(l1, wd1, n);
  nz_ |= copy_in(l2, wa2, n);
  nz_ |= copy_in(l3, wd2, n);
}

// ---- The half-band converters --------------------------------------------------------------------------------------------------------------------------

// 64 input samples to 32 at half the rate: the half-band low-pass and every second output (polyphase: the centre tap and the odd pairs only). The output is in d_.
void SendReverb::decimate(const std::int16_t* in) noexcept {
  for (unsigned i = 0; i < 22u; ++i) {
    ext_[i] = dec_hist_[i];
  }
  for (std::uint32_t j = 0; j < kBlock; ++j) {
    ext_[22u + j] = static_cast<float>(in[j]);
  }
  for (std::uint32_t m = 0; m < kBlock / 2u; ++m) {
    const float* c = ext_ + 2u * m + 11u;
    float y = 0.5f * c[0];
    y += kHb[0] * (c[-1] + c[1]);
    y += kHb[1] * (c[-3] + c[3]);
    y += kHb[2] * (c[-5] + c[5]);
    y += kHb[3] * (c[-7] + c[7]);
    y += kHb[4] * (c[-9] + c[9]);
    y += kHb[5] * (c[-11] + c[11]);
    d_[m] = y;
  }
  for (unsigned i = 0; i < 22u; ++i) {
    dec_hist_[i] = ext_[kBlock + i];
  }
}

// 32 samples at half the rate to 64: the even outputs are the (delayed) input, the odd ones the half-band's interpolation (gain 2 on the odd pairs). `hist` holds the last 11 inputs.
void SendReverb::interpolate(const float* y, std::int16_t* out, float* hist) noexcept {
  float* e = ext_;
  for (unsigned i = 0; i < 11u; ++i) {
    e[i] = hist[i];
  }
  for (std::uint32_t j = 0; j < kBlock / 2u; ++j) {
    e[11u + j] = y[j];
  }
  for (std::uint32_t m = 0; m < kBlock / 2u; ++m) {
    const float* c = e + m + 5u;
    float odd = kHb[0] * (c[0] + c[1]);
    odd += kHb[1] * (c[-1] + c[2]);
    odd += kHb[2] * (c[-2] + c[3]);
    odd += kHb[3] * (c[-3] + c[4]);
    odd += kHb[4] * (c[-4] + c[5]);
    odd += kHb[5] * (c[-5] + c[6]);
    out[2u * m] = static_cast<std::int16_t>(trunc16(c[0]));
    out[2u * m + 1u] = static_cast<std::int16_t>(trunc16(2.0f * odd));
  }
  for (unsigned i = 0; i < 11u; ++i) {
    hist[i] = e[kBlock / 2u + i];
  }
}

// ---- The block -----------------------------------------------------------------------------------------------------------------------------------------

void SendReverb::process(const std::int16_t* in, std::int16_t* out_l, std::int16_t* out_r, std::uint32_t n) noexcept {
  n = n < kBlock ? n : kBlock;
  if (mem_ == nullptr) {
    for (std::uint32_t i = 0; i < n; ++i) {
      out_l[i] = 0;
      out_r[i] = 0;
    }
    return;
  }
  if (n == kBlock) {
    run(in, out_l, out_r);
    return;
  }
  std::int16_t pad_in[kBlock] = {};
  std::int16_t pad_l[kBlock];
  std::int16_t pad_r[kBlock];
  for (std::uint32_t i = 0; i < n; ++i) {
    pad_in[i] = in[i];
  }
  run(pad_in, pad_l, pad_r);
  for (std::uint32_t i = 0; i < n; ++i) {
    out_l[i] = pad_l[i];
    out_r[i] = pad_r[i];
  }
}

void SendReverb::run(const std::int16_t* in, std::int16_t* out_l, std::int16_t* out_r) noexcept {
  if (static_cast<std::uint32_t>(quality_ != 0u ? 0u : 1u) != shift_) {  // a change of rate: the lines are repartitioned, the tail is cleared (a slice a block, below)
    const bool empty = idle_ && !clearing_;  // a sleeping plate holds nothing: only the layout changes, at once
    shift_ = quality_ != 0u ? 0u : 1u;
    if (empty) {
      configure();
      decay_ = 0.0f;
      zero_run_ = idle_after_;
    } else {
      begin_clear();
    }
  }
  if (clearing_) {  // the memory is cleared a slice a block, the return silent, the dry untouched
    const std::uint32_t end = clear_pos_ + kClearSamples < kMemSamples ? clear_pos_ + kClearSamples : kMemSamples;
    for (std::uint32_t i = clear_pos_; i < end; ++i) {
      mem_[i] = 0;
    }
    clear_pos_ = end;
    if (clear_pos_ >= kMemSamples) {
      clearing_ = false;
      idle_ = true;  // an empty plate: nothing to do until the first sound
      zero_run_ = idle_after_;
    }
    for (std::uint32_t i = 0; i < kBlock; ++i) {
      out_l[i] = 0;
      out_r[i] = 0;
    }
    return;
  }
  if (idle_) {
    std::int32_t any = 0;
    for (std::uint32_t i = 0; i < kBlock; ++i) {
      any |= in[i];
    }
    if (any == 0) {
      for (std::uint32_t i = 0; i < kBlock; ++i) {
        out_l[i] = 0;
        out_r[i] = 0;
      }
      return;
    }
    idle_ = false;
    zero_run_ = 0;
  }
  controls();
  nz_ = 0;
  const std::uint32_t n = nb_;

  // ---- the input: at the internal rate, delayed, diffused ----------------------------------------------------------------------------------------------
  Line& pd = line_[kPd];
  std::int16_t* const pw = wbuf(0, pd);
  if (shift_ == 0u) {
    for (std::uint32_t i = 0; i < n; ++i) {
      pw[i] = in[i];
    }
  } else {
    decimate(in);
    for (std::uint32_t i = 0; i < n; ++i) {
      pw[i] = static_cast<std::int16_t>(trunc16(d_[i]));
    }
  }
  const std::int16_t* const zp = copy_out(pd, start_of(pd, pd_cur_), n, z_[0]);
  const bool fade = pd_cur_ != pd_tgt_;
  const std::int16_t* const zq = fade ? copy_out(pd, start_of(pd, pd_tgt_), n, tapbuf_[0]) : zp;
  nz_ |= copy_in(pd, pw, n);
  pd_cur_ = pd_tgt_;
  input_block(zp, zq, fade, n);

  // ---- the tank ---------------------------------------------------------------------------------------------------------------------------------------
  // The cross feeds are the branches' last delays' outputs, which are old content (the delays are at least a block): both are read before either branch writes.
  const std::int16_t* const cross0 = copy_out(line_[kD2Id[0]], line_[kD2Id[0]].pos, n, z_[1]);
  const std::int16_t* const cross1 = copy_out(line_[kD2Id[1]], line_[kD2Id[1]].pos, n, z_[2]);
  tank_block(0, cross1, n);  // the left branch takes the right's last delay
  tank_block(1, cross0, n);

  // ---- the returns ------------------------------------------------------------------------------------------------------------------------------------
  // The output taps were staged before anything was written (what each line held when the block began): seven a channel, summed with the paper's signs, scaled, and for HALF interpolated.
  const float og = out_gain_;
  for (unsigned c = 0; c < 2u; ++c) {
    const std::int16_t* tp[kTapsPerChannel];
    for (unsigned t = 0; t < kTapsPerChannel; ++t) {
      // (the lines were written above: this block's n new slots are behind the write position now, so the offset is k + n from it)
      const Line& ln = line_[kTaps[c][t].line];
      tp[t] = view(ln, start_of(ln, tap_k_[c][t] + n), n, tapbuf_[t]);
    }
    std::int16_t* const out = c == 0u ? out_l : out_r;
    for (std::uint32_t i = 0; i < n; ++i) {
      const std::int32_t sum = tp[0][i] + tp[1][i] - tp[2][i] + tp[3][i] - tp[4][i] - tp[5][i] - tp[6][i];
      const float y = static_cast<float>(sum) * og;
      if (shift_ == 0u) {
        out[i] = static_cast<std::int16_t>(trunc16(y));
      } else {
        y_[c][i] = y;
      }
    }
    if (shift_ != 0u) {
      interpolate(y_[c], out, int_hist_[c]);
    }
  }

  // ---- bookkeeping: the quiet state --------------------------------------------------------------------------------------------------------------------
  if (nz_ == 0u) {
    zero_run_ = zero_run_ + kBlock < idle_after_ ? zero_run_ + kBlock : idle_after_;
    if (zero_run_ >= idle_after_) {
      idle_ = true;
      lp_[0] = lp_[1] = 0.0f;
      for (float& v : dec_hist_) v = 0.0f;
      for (auto& h : int_hist_) {
        for (float& v : h) v = 0.0f;
      }
    }
  } else {
    zero_run_ = 0;
  }
}

}  // namespace o3os::audio
