// SPDX-License-Identifier: MIT
//
// The TB-303 voice: a float, block-based port of Open303 (OS-012a, ADR 0002, radar card `tb303-engine`, the dsp-port procedure).
//
//   Open303, Copyright (c) 2009 Robin Schmidt (www.rs-met.com), MIT licence: the notice and the permission text are in LICENSE-Open303.txt beside
//   this file and in THIRD_PARTY.md. Upstream commit 52f8614966cd (github.com/maddanio/open303, a mirror of the SourceForge project). This file is
//   a reimplementation of the algorithms of rosic_Open303.cpp/.h, rosic_TeeBeeFilter.h, rosic_OnePoleFilter, rosic_BiquadFilter,
//   rosic_LeakyIntegrator, rosic_DecayEnvelope and rosic_AnalogEnvelope for the Cortex-M4F; the constants (the measured cutoff mapping, the TB_303
//   filter fit, the shaping filters' corner frequencies, the accent and slide model) are Open303's.
//
// WHAT IS KEPT (tested against the original in double precision, tests/tb303_oracle_test.cpp):
//   the TB_303 branch of TeeBeeFilter (four coupled one-pole stages, the cubic soft clip in the feedback path, the feedback high-pass at 150 Hz),
//   the pre high-pass (44.486 Hz), the post high-pass (24.167 Hz), all-pass (14.008 Hz) and notch (7.5164 Hz, bandwidth 4.7 octaves), the main decay
//   envelope feeding two leaky integrators, the amplitude envelope with its accent term and de-clicker, the pitch slew (slide), the accent rules
//   (velocity >= 100, the Acc Thresh parameter since OS-SNDMOD S3: 200 ms filter decay (Acc Decay), 50 ms amp release, the accent gain), the measured cutoff mapping (envScaler / envOffset).
// WHAT IS DIFFERENT, and why (the radar card's variant B, the cost):
//   - the oscillator is a PolyBLEP saw / pulse at 2x the sample rate instead of two 12-octave mip-mapped wavetables of 2,048 floats: the tables are
//     197 KB, and 4x oversampling with a 12th-order elliptic decimator is about 1,300 cycles a sample. The waveforms are the tables' (SAW303: a
//     rising ramp from 0, the jump at half a cycle; SQUARE303: +1 for 0.4688 of a cycle with the tanh-shaped falling edge, ~5.7 % of a cycle wide),
//   - the filter chain runs at 2x, not 4x (the TB_303 fit is a function of the normalised cutoff, valid to wc = pi/4: the cutoff is held to that),
//     decimated by a 4th-order elliptic filter (0.5 dB, 40 dB, 16 kHz at 88.2 kHz, unity gain at DC) instead of the 12th-order one,
//   - the cutoff's coefficients are computed every kCtl (4) output samples from the envelopes, which run every sample; the original recomputes them
//     (a degree-12 and a degree-6 polynomial and a pow) every sample at 4x,
//   - the note list and the sequencer of Open303 are the host's: a note while another is held (O3M_NOTE_LEGATO) is a slide when the LegatoSlide
//     parameter is on, the host's note_off releases,
//   - float, with an anti-denormal bias of 1e-20 where Open303 adds FLT_MIN to a double.
//
// Contexts: a Voice is one instance; init()/set_param() derive coefficients with the host's exp2/log2/sin/cos (cold, a few dozen calls);
// render() is the audio path and calls none of them except exp2 once every kCtl samples. All the state is in the Voice (which the OS gives as the
// instance block, zeroed): no globals, no libc.
#pragma once

#include <stdint.h>

namespace tb303 {

constexpr uint32_t kCtl = 4;          // output samples between coefficient updates
constexpr uint32_t kQuietSamples = 64;  // samples of inaudible amplitude (-120 dB) after which the voice stops
constexpr float kTiny = 1e-20f;       // added to recursive filter states so that they never reach the denormals
constexpr float kPi = 3.14159265358979323846f;
constexpr float kLog2e = 1.44269504088896340736f;
constexpr float kSqrt2 = 1.41421356237309504880f;

// The host's services (o3m_host's exp2 / log2 / sin / cos) or the host tests' own.
struct Services {
  float (*exp2)(float);
  float (*log2)(float);
  float (*sin)(float);
  float (*cos)(float);
};

// Parameter ids (the module's o3m_param order) and their ranges.
// (OS-SNDMOD S3: ids 9..13 are appended, never reordered; a preset of nine values leaves them at their defaults.)
enum Param : uint32_t {
  kWaveform = 0, kCutoff, kResonance, kEnvMod, kDecay, kAccent, kSlide, kVolume, kLegato,
  kTune, kTranspose, kAccThresh, kFilterTrack, kAccDecay,
  kParamCount
};

constexpr float kTrackRefNote = 48.0f;  // the note at which Filter Track leaves the cutoff where the knob says

struct OnePole {
  float b0, b1, a1, x1, y1;
};
// A state variable filter in the topology-preserving form (Andrew Simper's / Vadim Zavalishin's): the same bilinear-transform response as the cookbook
// biquad Open303 uses for its notch and de-clicker, but its coefficients stay small (g = tan(pi f / fs)) where the direct form's sit within 1e-6 of 2
// and -1 for a corner at a few Hz and lose the filter in float (the de-clicker at 200 Hz: 76 dB against the original, the 7.5 Hz notch: 41 dB).
struct Svf {
  float a1, a2, a3, k, s1, s2;
};
struct Section {  // transposed direct form II, the decimator's
  float b0, b1, b2, a1, a2, s1, s2;
};

// The envelope section's state (Open303's mainEnv, rc2, ampEnv and the accent): what advances every output sample.
struct Env {
  float main_y, main_d;  // the main (decay) envelope: the value, the fraction it loses each sample (the note's kind)
  float rc2_y;           // the second leaky integrator (rc1 has a time constant of 0: it is the envelope itself)
  float accent_gain;     // 0 for a normal note, the Accent parameter for an accented one
  float amp_y;           // the amplitude envelope
  float amp_rel_d;       // its release factor, by the note's kind
  uint32_t trig;         // the amp envelope's first sample of a note
};

// The TB_303 ladder (TeeBeeFilter mode TB_303): its coefficients, the four stages' state; the feedback high-pass is the voice's `fbhp`.
struct Ladder {
  float b0, k, g2;  // stage coefficient, feedback gain, the output gain (2 g)
  float y1, y2, y3, y4;
};

struct Voice {
  Services sv;
  float fs;   // output rate, Hz
  float fs2;  // the filter chain's rate (2 x)

  // ---- the parameters as set
  float p_wave, p_cutoff, p_res, p_envmod, p_decay, p_accent, p_slide, p_volume;
  uint32_t p_legato;
  float p_tune;         // cents, whole numbers (S3): added to the note at note on
  float p_transpose;    // semitones, whole numbers (S3)
  float p_track;        // Filter Track as a fraction 0..1 (S3)
  float p_acc_decay;    // ms: the filter decay of an accented note (S3)
  uint32_t p_acc_thr;   // the velocity from which a note is an accent, 1..127 (S3)

  // ---- derived from them (set_param)
  float reso_skew;     // TeeBeeFilter's resonanceSkewed
  float env_scaler;    // Open303's envScaler / envOffset (the measured mapping)
  float env_offset;
  float amp_scaler;    // the master level as a factor
  float slide_d;       // the pitch slew's coefficient
  float main_d_norm;   // the main envelope's per-sample factor for a normal note
  float main_d_acc;    // and for an accented one (200 ms)

  // ---- fixed at init (the sample rate's)
  float rc2_d;          // 15 ms
  float amp_decay_d;    // 1230 ms
  float amp_rel_norm_d; // 1 ms
  float amp_rel_acc_d;  // 50 ms
  float inv_fs2;
  float cutoff_max;     // the TB_303 fit's top: wc = pi/4 at fs2
  OnePole hp1, fbhp;    // at fs2: the pre high-pass, the feedback high-pass
  OnePole hp2, ap;      // at fs: the post high-pass, the all-pass
  Svf notch, declick;
  Section dec[2];       // the decimator

  // ---- the run state
  float phase;           // the oscillator's, 0..1
  float freq;            // the slewed oscillator frequency, Hz
  float freq_target;
  float note_hz;         // OS-MODPITCH: the sounding note's own frequency (no offset); pitch() moves freq_target from it
  float track_mul;       // S3: the factor Filter Track puts on the knob cutoff NOW (1 when Filter Track is 0); render slews it to track_goal at the pitch's own rate (a slide, unit finding 2026-10-19)
  float track_goal;      // the factor for the pitch the note is heading to: the played note plus pitch()'s offset (1 when Filter Track is 0)
  float track_note;      // the played note in semitones (Tune and Transpose in), which pitch() adds its offset to; single writer: note_on / pitch / render all run on the audio thread
  Env env;
  Ladder lad;
  uint32_t ctl;          // output samples until the next coefficient update
  uint32_t gate;         // a note is held
  uint32_t idle;         // nothing will sound until the next note
  uint32_t quiet;        // consecutive output samples whose amplitude was under 1e-6 (idle at kQuietSamples)
};

namespace detail {

inline float absf(float x) { return x < 0.0f ? -x : x; }
inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline float round_whole(float x) { return static_cast<float>(static_cast<int32_t>(x + (x >= 0.0f ? 0.5f : -0.5f))); }

// 1 - exp(-1 / (fs * 0.001 * ms)): the fraction a one-pole decay of `ms` milliseconds loses each sample (1: gone at once). The decays are run as
// y -= y d, not y *= (1 - d): in float 1 - d rounds to a multiple of 6e-8, which for a 1 s decay at 44.1 kHz (d = 2.3e-5) is 0.26 % of the time constant,
// 1.5e-4 of the level after 5,000 samples; d itself keeps its 24 bits. The series is the exact 1 - e^-x for x < 0.1, which every decay here is.
inline float decay_rate(const Voice&, float fs, float ms) {
  if (!(ms > 0.0f)) {
    return 1.0f;
  }
  const float x = 1.0f / (fs * 0.001f * ms);
  if (x >= 0.1f) {
    return 1.0f;  // under 0.23 ms at 44.1 kHz: the stage is a wire
  }
  return x * (1.0f - x * (0.5f - x * ((1.0f / 6.0f) - x * ((1.0f / 24.0f) - x * (1.0f / 120.0f)))));
}

inline float exp_f(const Voice& v, float x) { return v.sv.exp2(x * kLog2e); }
inline float tan_f(const Voice& v, float x) { return v.sv.sin(x) / v.sv.cos(x); }
inline float db_to_amp(const Voice& v, float db) { return v.sv.exp2(db * 0.16609640474436813f); }  // 10^(db / 20)

// rosic_OnePoleFilter::calcCoeffs, the two modes the voice uses. `f` Hz at `rate`.
inline void one_pole_highpass(const Voice& v, OnePole& p, float f, float rate) {
  const float x = exp_f(v, -2.0f * kPi * f / rate);
  p.b0 = 0.5f * (1.0f + x);
  p.b1 = -p.b0;
  p.a1 = x;
}
inline void one_pole_allpass(const Voice& v, OnePole& p, float f, float rate) {
  const float t = tan_f(v, kPi * f / rate);
  const float x = (t - 1.0f) / (t + 1.0f);
  p.b0 = x;
  p.b1 = 1.0f;
  p.a1 = -x;
}

inline float sinh_f(const Voice& v, float x) { return 0.5f * (exp_f(v, x) - exp_f(v, -x)); }

// rosic_BiquadFilter::calcCoeffs, LOWPASS12 (Q) and BANDREJECT (bandwidth in octaves), as the same bilinear responses (k = 1 / Q; the cookbook's
// alpha = sin(w0) / (2 Q) gives k = 2 alpha / sin(w0)).
inline void svf_set(const Voice& v, Svf& f, float freq, float k, float rate) {
  const float g = tan_f(v, kPi * freq / rate);
  f.k = k;
  f.a1 = 1.0f / (1.0f + g * (g + k));
  f.a2 = g * f.a1;
  f.a3 = g * f.a2;
}
inline void svf_lowpass12(const Voice& v, Svf& f, float freq, float q, float rate) { svf_set(v, f, freq, 1.0f / q, rate); }
inline void svf_bandreject(const Voice& v, Svf& f, float freq, float bandwidth, float rate) {
  const float w = 2.0f * kPi * freq / rate;
  svf_set(v, f, freq, 2.0f * sinh_f(v, 0.5f * 0.69314718055994531f * bandwidth * w / v.sv.sin(w)), rate);
}

inline float run(OnePole& f, float in) {
  const float y = f.b0 * in + f.b1 * f.x1 + f.a1 * f.y1 + kTiny;
  f.x1 = in;
  f.y1 = y;
  return y;
}
// One sample; `out_notch`: the notch output (x - k band), else the low-pass.
template <bool Notch>
inline float run_svf(Svf& f, float in) {
  const float v3 = in - f.s2 + kTiny;
  const float v1 = f.a1 * f.s1 + f.a2 * v3;
  const float v2 = f.s2 + f.a2 * f.s1 + f.a3 * v3;
  f.s1 = 2.0f * v1 - f.s1;
  f.s2 = 2.0f * v2 - f.s2;
  return Notch ? in - f.k * v1 : v2;
}
inline float run(Section& f, float in) {
  const float y = f.b0 * in + f.s1;
  f.s1 = f.b1 * in - f.a1 * y + f.s2;
  f.s2 = f.b2 * in - f.a2 * y;
  return y;
}

// PolyBLEP residual (two samples): the correction of a unit-amplitude step at phase 0 for a phase increment dt (Valimaki, Pekonen, Nam, JASA 2012).
inline float blep(float t, float dt) {
  if (t < dt) {
    const float x = t / dt;
    return x + x - x * x - 1.0f;
  }
  if (t > 1.0f - dt) {
    const float x = (t - 1.0f) / dt;
    return x * x + x + x + 1.0f;
  }
  return 0.0f;
}

// tanh by the continued fraction of Lambert, to x^7 (error < 1e-4 for |x| <= 4); the pulse's edge uses it, a few samples of every cycle.
inline float tanh_f(float x) {
  x = clampf(x, -4.0f, 4.0f);
  const float x2 = x * x;
  return x * (135135.0f + x2 * (17325.0f + x2 * (378.0f + x2))) / (135135.0f + x2 * (62370.0f + x2 * (3150.0f + 28.0f * x2)));
}

// The oscillator mix of Open303's BlendOscillator with SAW303 and SQUARE303 at phase p (0..1), dt the phase step: (1 - blend) saw + blend 0.5 pulse.
//   saw:   2 frac(p + 0.5) - 1, the rising ramp that starts at 0 (table SAW303), the jump at p = 0.5;
//   pulse: +1 up to the tanh-shaped falling edge centred at 0.46880 (width +-0.0286, u = 140.08 (p - 0.4688)), -1 after, the rising edge a jump at p = 0.
// OS-PERF3: the two ends of the blend (the WAVEFORM knob's saw and square, the usual settings) compute only the wave they use: (1 - 0) x saw + 0 x pulse is saw and (1 - 1) x saw +
// 1 x 0.5 x pulse is 0.5 pulse, exactly (the unused wave is finite), so the output is the same bit for bit apart from the sign of a zero, and the unused wave's branches and arithmetic
// (about a fifth of a pass) are skipped.
inline float osc_saw(float p, float dt) {
  float q = p + 0.5f;
  q -= q >= 1.0f ? 1.0f : 0.0f;
  return 2.0f * q - 1.0f - blep(q, dt);  // the drop of 2 at q = 0 (p = 0.5), band-limited
}
inline float osc_pulse(float p, float dt) {
  float pulse;
  if (p < 0.44f) {
    pulse = 1.0f;
  } else if (p < 0.497f) {
    pulse = -tanh_f(140.08f * (p - 0.46880f));
  } else {
    pulse = -1.0f;
  }
  return pulse + blep(p, dt);  // the rise of 2 at p = 0
}
inline float oscillator(float p, float dt, float blend) {
  if (blend == 0.0f) {
    return osc_saw(p, dt);
  }
  if (blend == 1.0f) {
    return 0.5f * osc_pulse(p, dt);
  }
  return (1.0f - blend) * osc_saw(p, dt) + blend * 0.5f * osc_pulse(p, dt);
}

// TeeBeeFilter::calculateCoefficientsApprox4, the TB_303 branch: fx = wc / (sqrt 2 2 pi) = cutoff / (sqrt 2 fs2).
inline void filter_coefficients(Voice& v, float cutoff) {
  const float c = clampf(cutoff, 200.0f, v.cutoff_max);
  const float fx = c * (1.0f / kSqrt2) / v.fs2;
  const float r = v.reso_skew;
  v.lad.b0 = (0.00045522346f + 6.1922189f * fx) / (1.0f + 12.358354f * fx + 4.4156345f * (fx * fx));
  float k = fx * (fx * (fx * (fx * (fx * (fx + 7198.6997f) - 5837.7917f) - 476.47308f) + 614.95611f) + 213.87126f) + 16.998792f;
  float g = k * 0.058823529411764705882352941176471f;  // 1 / 17
  g = (g - 1.0f) * r + 1.0f;
  g = g * (1.0f + r);
  v.lad.k = k * r;
  v.lad.g2 = 2.0f * g;
}

// rosic_Open303::calculateEnvModScalerAndOffset, the measured mapping.
inline void env_mod_mapping(Voice& v) {
  const float c0 = 3.138152786059267e+002f;  // lowest nominal cutoff
  const float c1 = 2.394411986817546e+003f;  // highest nominal cutoff
  const float oF = 0.048292930943553f, oC = 0.294391201442418f;
  const float sLoF = 3.773996325111173f, sLoC = 0.736965594166206f;
  const float sHiF = 4.194548788411135f, sHiC = 0.864344900642434f;
  const float e = v.p_envmod * 0.01f;
  const float c = (v.sv.log2(v.p_cutoff / c0)) / (v.sv.log2(c1 / c0));
  const float sLo = sLoF * e + sLoC;
  const float sHi = sHiF * e + sHiC;
  v.env_scaler = (1.0f - c) * sLo + c * sHi;
  v.env_offset = oF * c + oC;
}

inline void derive_accent_decay(Voice& v) { v.main_d_acc = decay_rate(v, v.fs, v.p_acc_decay); }

inline void derive(Voice& v) {
  const float r = v.p_res * 0.01f;  // TeeBeeFilter::setResonance
  v.reso_skew = (1.0f - exp_f(v, -3.0f * r)) / (1.0f - exp_f(v, -3.0f));
  env_mod_mapping(v);
  v.amp_scaler = db_to_amp(v, v.p_volume);
  v.slide_d = decay_rate(v, v.fs, 0.2f * v.p_slide);
  v.main_d_norm = decay_rate(v, v.fs, v.p_decay);
  derive_accent_decay(v);
}

inline void reset_states(Voice& v) {
  v.phase = 0.0f;
  v.lad.y1 = v.lad.y2 = v.lad.y3 = v.lad.y4 = 0.0f;
  v.hp1.x1 = v.hp1.y1 = 0.0f;
  v.hp2.x1 = v.hp2.y1 = 0.0f;
  v.ap.x1 = v.ap.y1 = 0.0f;
  v.fbhp.x1 = v.fbhp.y1 = 0.0f;
  v.notch.s1 = v.notch.s2 = 0.0f;
  v.declick.s1 = v.declick.s2 = 0.0f;
  v.dec[0].s1 = v.dec[0].s2 = v.dec[1].s1 = v.dec[1].s2 = 0.0f;
}

}  // namespace detail

// One sample of the ladder: TeeBeeFilter::getSample in TB_303 mode, `s` the (pre-filtered) input. `fbhp` is the feedback high-pass.
inline float ladder_step(Ladder& l, OnePole& fbhp, float s) {
  float t = l.y4;  // shape(): x - x^3 / 6 on |x| <= sqrt 2
  t = t > kSqrt2 ? kSqrt2 : (t < -kSqrt2 ? -kSqrt2 : t);
  t = l.k * (t - (1.0f / 6.0f) * t * t * t);
  const float y0 = s - detail::run(fbhp, t);
  l.y1 += 2.0f * l.b0 * (y0 - l.y1 + l.y2);
  l.y2 += l.b0 * (l.y1 - 2.0f * l.y2 + l.y3);
  l.y3 += l.b0 * (l.y2 - 2.0f * l.y3 + l.y4);
  l.y4 += l.b0 * (l.y3 - 2.0f * l.y4);
  return l.g2 * l.y4;
}

// One output sample of the envelopes: the first half of Open303::getSample. `expo` is tmp1 + tmp2 (the cutoff's octaves above its nominal value),
// `amp` the amplitude envelope with the accent term, before the de-clicker.
inline void env_step(const Voice& v, Env& e, bool gate, float& expo, float& amp) {
  e.main_y -= e.main_y * e.main_d;
  const float in2 = e.accent_gain > 0.0f ? e.main_y : 0.0f;
  e.rc2_y += v.rc2_d * (in2 - e.rc2_y);
  expo = v.env_scaler * (e.main_y - v.env_offset) + e.accent_gain * e.rc2_y;
  if (e.trig != 0u) {
    e.amp_y = 1.0f;
    e.trig = 0;
  } else {
    e.amp_y -= e.amp_y * (gate ? v.amp_decay_d : e.amp_rel_d);
  }
  amp = e.amp_y;
  if (gate) {
    amp += (0.45f + 4.0f * e.accent_gain) * e.main_y;
  }
}

// The note as played plus Transpose and Tune (S3), held to the MIDI range; at the neutral values this is the note itself, bit for bit.
inline float played_note(const Voice& v, uint32_t note) {
  return detail::clampf(static_cast<float>(note > 127u ? 127u : note) + v.p_transpose + v.p_tune * 0.01f, 0.0f, 127.0f);
}

// Filter Track's factor for a note (in semitones): 2^((note - 48) / 12 x track); 1.0f, with no exp2, at Filter Track 0.
namespace detail {
inline float track_factor(const Voice& v, float note) {
  return v.p_track > 0.0f ? v.sv.exp2((clampf(note, 0.0f, 127.0f) - kTrackRefNote) * (1.0f / 12.0f) * v.p_track) : 1.0f;
}
}  // namespace detail

inline float note_frequency(const Voice& v, float n) {
  // 440 Hz 2^((n - 69) / 12): Open303's pitchToFreq with the master tuning at 440
  return 440.0f * v.sv.exp2((n - 69.0f) * (1.0f / 12.0f));
}

// Cold: once, with the instance block zeroed. The sample rate is the engine's.
inline void voice_init(Voice& v, const Services& sv, float sample_rate) {
  v.sv = sv;
  v.fs = sample_rate;
  v.fs2 = 2.0f * sample_rate;
  v.inv_fs2 = 1.0f / v.fs2;
  v.p_wave = 0.0f;
  v.p_cutoff = 1000.0f;
  v.p_res = 50.0f;
  v.p_envmod = 25.0f;
  v.p_decay = 1000.0f;
  v.p_accent = 50.0f;
  v.p_slide = 60.0f;
  v.p_volume = -12.0f;
  v.p_legato = 1;
  v.p_tune = 0.0f;
  v.p_transpose = 0.0f;
  v.p_track = 0.0f;
  v.p_acc_decay = 200.0f;
  v.p_acc_thr = 100u;
  v.track_mul = v.track_goal = 1.0f;
  v.track_note = kTrackRefNote;
  // the TB_303 fit holds to wc = pi/4 (see the header)
  v.cutoff_max = (kPi * 0.25f) * v.fs2 / (2.0f * kPi);
  detail::one_pole_highpass(v, v.hp1, 44.486f, v.fs2);
  detail::one_pole_highpass(v, v.fbhp, 150.0f, v.fs2);
  detail::one_pole_highpass(v, v.hp2, 24.167f, v.fs);
  detail::one_pole_allpass(v, v.ap, 14.008f, v.fs);
  detail::svf_bandreject(v, v.notch, 7.5164f, 4.7f, v.fs);
  detail::svf_lowpass12(v, v.declick, 200.0f, 0.70710678118654757f, v.fs);
  v.rc2_d = detail::decay_rate(v, v.fs, 15.0f);
  v.amp_decay_d = detail::decay_rate(v, v.fs, 1230.0f);
  v.amp_rel_norm_d = detail::decay_rate(v, v.fs, 1.0f);
  v.amp_rel_acc_d = detail::decay_rate(v, v.fs, 50.0f);
  // The decimator: scipy.signal.ellip(4, 0.5, 40, 16000 / 44100, output='sos'), the first section's numerator scaled by 1 / 0.94406 (10^(0.5 / 20)):
  // an even-order elliptic filter has its ripple minimum at DC, and the chain's gain at DC is meant to be 1.
  const float dc = 1.0592537251772889f;
  v.dec[0] = Section{0.0560333802f * dc, 0.0806011947f * dc, 0.0560333802f * dc, -0.92470149f, 0.341392156f, 0.0f, 0.0f};
  v.dec[1] = Section{1.0f, 0.21086531f, 1.0f, -0.70072837f, 0.78355269f, 0.0f, 0.0f};
  detail::derive(v);
  detail::reset_states(v);
  v.freq = v.freq_target = 440.0f;
  v.env.main_d = v.main_d_norm;
  v.env.amp_rel_d = v.amp_rel_norm_d;
  detail::filter_coefficients(v, v.p_cutoff);
  v.idle = 1;
}

// The parameters, as the module's o3m_param table describes them. The value is held to its range (the OS does not clamp).
inline void voice_set_param(Voice& v, uint32_t id, float value) {
  using detail::clampf;
  if (!(value == value)) {
    return;  // NaN
  }
  switch (id) {
    case kWaveform: v.p_wave = clampf(value, 0.0f, 1.0f); break;
    case kCutoff: v.p_cutoff = clampf(value, 314.0f, 2394.0f); break;
    case kResonance: v.p_res = clampf(value, 0.0f, 100.0f); break;
    case kEnvMod: v.p_envmod = clampf(value, 0.0f, 100.0f); break;
    case kDecay: v.p_decay = clampf(value, 200.0f, 2000.0f); break;
    case kAccent: v.p_accent = clampf(value, 0.0f, 100.0f); break;
    case kSlide: v.p_slide = clampf(value, 0.0f, 300.0f); break;
    case kVolume: v.p_volume = clampf(value, -48.0f, 6.0f); break;
    case kLegato: v.p_legato = value >= 0.5f ? 1u : 0u; break;
    // S3: none of these touches the filter or the envelopes' coefficients, so none runs derive() (a CC on Tune is not a full derive a block)
    case kTune: v.p_tune = detail::round_whole(clampf(value, -100.0f, 100.0f)); return;
    case kTranspose: v.p_transpose = detail::round_whole(clampf(value, -24.0f, 24.0f)); return;
    case kAccThresh: v.p_acc_thr = static_cast<uint32_t>(detail::round_whole(clampf(value, 1.0f, 127.0f))); return;
    case kFilterTrack: v.p_track = clampf(value, 0.0f, 100.0f) * 0.01f; return;
    case kAccDecay:
      v.p_acc_decay = clampf(value, 30.0f, 2000.0f);
      detail::derive_accent_decay(v);
      return;
    default: return;
  }
  detail::derive(v);
  v.ctl = 0;  // the next sample recomputes the filter's coefficients
}

// Open303::triggerNote / slideToNote. `velocity` >= 100 is an accent. `legato`: a note was held (the host's flag). `ramp` (OS-MODPITCH, O3M_NOTE_RAMP): the host drives this note's pitch (a Glide or Slide
// of the pattern): with `legato` the envelope is kept whatever the Legato Slide parameter says (the pattern chose the slide), the base pitch jumps to the note at once and the module's own slew
// stays out of it, so the pitch never slides twice; the Slide and Legato Slide parameters are for a live legato key.
inline void voice_note_on(Voice& v, uint32_t note, uint32_t velocity, bool legato, bool ramp = false) {
  const bool accent = velocity >= v.p_acc_thr;
  const bool slide = legato && (v.p_legato != 0u || ramp) && !v.idle;
  if (!slide && v.idle) {  // retrigger the oscillator and reset the filters only when nothing sounds (no click)
    detail::reset_states(v);
  }
  if (accent) {
    v.env.accent_gain = v.p_accent * 0.01f;
    v.env.main_d = v.main_d_acc;
    v.env.amp_rel_d = v.amp_rel_acc_d;
  } else {
    v.env.accent_gain = 0.0f;
    v.env.main_d = v.main_d_norm;
    v.env.amp_rel_d = v.amp_rel_norm_d;
  }
  const float played = played_note(v, note);
  v.freq_target = note_frequency(v, played);
  v.note_hz = v.freq_target;
  // Filter Track (S3): the cutoff's factor for this note: one octave per octave of the note above the reference at 100 %. It is the GOAL: a trigger takes it at once, a slide lets render slew the
  // factor to it with the pitch's own time constant (the cutoff then glides with the pitch instead of stepping at the glide start); the cost is one exp2 here, none at Filter Track 0.
  v.track_note = played;
  v.track_goal = detail::track_factor(v, played);
  if (!slide) {
    v.track_mul = v.track_goal;
  }
  if (ramp && slide) {
    v.freq = v.freq_target;  // the host moves the pitch (pitch()): the base jumps, no slew of our own
  }
  if (!slide) {
    v.freq = v.freq_target;  // the slew limiter's state: no glide on a trigger
    v.env.main_y = 1.0f / (1.0f - v.env.main_d);  // DecayEnvelope::trigger: yInit = 1 / c, so that the next sample, y * c, is 1
    v.env.trig = 1;
  }
  v.gate = 1;
  v.idle = 0;
  v.quiet = 0;
  v.ctl = 0;
}

inline void voice_note_off(Voice& v) { v.gate = 0; }

// OS-MODPITCH (ABI 1.8): the host's Slide / Glide offset from the note, in semitones. The frequency goes straight to note x 2^(offset / 12), with one exp2, past the module's own slew (the host's
// ramp is the only slide on a pattern note). `per_frame` is not followed: the pitch is a staircase of one block (64 samples, 0.05 semitone of a 7-semitone glide in 200 ms).
inline void voice_pitch(Voice& v, float semitones, float per_frame) {
  (void)per_frame;
  const float hz = semitones == 0.0f ? v.note_hz : v.note_hz * v.sv.exp2(detail::clampf(semitones, -128.0f, 128.0f) * (1.0f / 12.0f));
  v.freq_target = hz;
  v.freq = hz;
  v.track_goal = detail::track_factor(v, v.track_note + (semitones == 0.0f ? 0.0f : detail::clampf(semitones, -128.0f, 128.0f)));  // the cutoff follows the pitch (render slews to it)
}

// The audio path: `n` samples (any count; no state depends on how a block is cut) into `out`. Returns 1 while the voice may still sound, 0 once it is
// silent and idle (the output is then exact zeros).
//
// OS-303PERF (M1): TWO LEVELS. The outer loop runs once for every kCtl samples (the grid `ctl` counts, which a block boundary does not move) and holds the coefficient update, the only call out of
// the loop (the host's exp2); the inner loop is the sample, with no test of the grid. All the state the sample changes is in locals (`lad` already was; the filters of the chain whose state the
// 32 S registers could not keep in a loop that had a call in it are the other half: hp1, fbhp, the de-clicker and the decimator), written back once at the end. hp2, ap and the notch stay in the voice:
// measured, in locals they cost instructions (more spills than they save). A sample is bit for bit what it was: the update uses the envelope value of ITS sample (the first of the segment), which the
// outer loop gets by running env_step on a copy of the envelope (the same operations on the same values); tests/tb303_refactor_test.cpp holds this to the old loop sample by sample and state by state.
// Not done, because it cannot be bit-exact: the idle test (`quiet`) on the grid would end the voice up to kCtl - 1 samples later, which is a different output and a different carried `rc2_y`
// (the per-sample test is not where the time goes: dropping the trig half of it changed the instructions of a block by none); and `inv_fs2` in filter_coefficients (x * (1 / fs2) is not x / fs2: one ulp
// in the coefficients, a divide in four samples).
__attribute__((always_inline)) inline uint32_t voice_render(Voice& v, float* __restrict out, uint32_t n) {  // (restrict, OS-PERF3: the stores to `out` cannot change the voice's coefficients, so they stay in registers across the loop)
  if (v.idle) {
    for (uint32_t i = 0; i < n; ++i) {
      out[i] = 0.0f;
    }
    return 0;
  }
  const float blend = v.p_wave;
  const float amp_scaler = v.amp_scaler;
  const float inv_fs2 = v.inv_fs2;
  const float slide_d = v.slide_d;
  const float knob = v.p_cutoff;
  const float track_goal = v.track_goal;
  const float track_a = kCtl * slide_d < 1.0f ? kCtl * slide_d : 1.0f;  // the pitch slew's fraction per sample, for one update's kCtl samples
  float tm = v.track_mul;                                                // (S3: at Filter Track 0 goal and tm are 1.0f, the step adds 0 and x 1.0f is the knob itself, bit for bit)
  const float freq_target = v.freq_target;
  const bool gate = v.gate != 0u;
  float phase = v.phase;
  float freq = v.freq;
  uint32_t ctl = v.ctl;
  Env env = v.env;
  Ladder lad = v.lad;
  OnePole hp1 = v.hp1;
  OnePole fbhp = v.fbhp;
  Svf declick = v.declick;
  Section dec0 = v.dec[0];
  Section dec1 = v.dec[1];
  uint32_t quiet = v.quiet;
  uint32_t done = n;  // samples produced before the voice went idle (the rest of the block is zeros)
  bool went_idle = false;

  for (uint32_t i = 0; i < n && !went_idle;) {
    if (ctl == 0) {
      // the update of the sample that starts the segment, from its own envelope value: env_step on a copy, the sample itself then runs it for real
      Env peek = env;
      float expo, unused;
      env_step(v, peek, gate, expo, unused);
      tm += track_a * (track_goal - tm);
      detail::filter_coefficients(v, (knob * tm) * v.sv.exp2(expo));
      lad.b0 = v.lad.b0;
      lad.k = v.lad.k;
      lad.g2 = v.lad.g2;
      ctl = kCtl;
    }
    const uint32_t seg = ctl < n - i ? ctl : n - i;  // the samples to the next update, or to the end of the block
    ctl -= seg;
    for (uint32_t j = 0; j < seg; ++j, ++i) {
      // pitch slew: LeakyIntegrator on the frequency (the glide of a slide), held inside the oscillator's range
      freq += slide_d * (freq_target - freq);
      const float f = freq < 20000.0f ? (freq > 0.0f ? freq : 0.0f) : 20000.0f;
      const float dt = f * inv_fs2;

      float expo, amp_out;
      env_step(v, env, gate, expo, amp_out);  // (expo is the update's, above: dead here)
      (void)expo;
      amp_out = detail::run_svf<false>(declick, amp_out);

      // 2x oversampled: oscillator (inverted), pre high-pass, the TB_303 ladder, the decimator
      float x = 0.0f;
      for (int pass = 0; pass < 2; ++pass) {
        float s = -detail::oscillator(phase, dt, blend);
        phase += dt;
        phase -= phase >= 1.0f ? 1.0f : 0.0f;
        s = detail::run(hp1, s);
        x = detail::run(dec1, detail::run(dec0, ladder_step(lad, fbhp, s)));
      }

      // 1x: all-pass, post high-pass, notch, the amplitude, the level
      x = detail::run(v.ap, x);
      x = detail::run(v.hp2, x);
      x = detail::run_svf<true>(v.notch, x);
      x *= amp_out * amp_scaler;
      out[i] = x;
      // Idle: the amplitude (envelope plus accent term, after the de-clicker) stayed under 1e-6 for kQuietSamples in a row, which nothing but a note can
      // end: the voice stops (its output was inaudible, -120 dB). Counted per sample, so where a block is cut changes nothing.
      if (detail::absf(amp_out) < 1e-6f && env.trig == 0u) {
        if (++quiet >= kQuietSamples) {
          done = i + 1u;
          went_idle = true;
          ctl += seg - (j + 1u);  // (the grid counts the samples that ran: the same state as the loop that tested each sample)
          break;
        }
      } else {
        quiet = 0;
      }
    }
  }

  v.phase = phase;
  v.freq = freq;
  v.track_mul = tm;
  v.ctl = ctl;
  v.env = env;
  v.lad.y1 = lad.y1;
  v.lad.y2 = lad.y2;
  v.lad.y3 = lad.y3;
  v.lad.y4 = lad.y4;
  v.hp1 = hp1;
  v.fbhp = fbhp;
  v.declick = declick;
  v.dec[0] = dec0;
  v.dec[1] = dec1;
  v.quiet = quiet;
  if (went_idle) {
    for (uint32_t i = done; i < n; ++i) {
      out[i] = 0.0f;
    }
    v.idle = 1;
    return 0;
  }
  return 1;
}

}  // namespace tb303
