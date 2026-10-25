// SPDX-License-Identifier: MIT
//
// The voice of the FM module (OS-012c): a four-operator phase-modulation engine written from scratch (no third-party code: the maths is the textbook's, Chowning 1973; the algorithm
// tables are this module's own). One mono voice: four sine operators, each with a frequency ratio and detune, a level and an ADSR; sixteen algorithms say which operators modulate which and
// which are heard; operator 4 has a feedback loop.
//
//   operator:  out = sin(2 pi (phase + modulation)) * envelope * gain       phase advances by ratio * detune * the note's frequency
//   modulation of an operator = kIndex * (the sum of the outputs of the operators the algorithm routes into it)   (kIndex = 2 turns: a full-level modulator swings the phase by 4 pi)
//   output = the sum of the carriers' outputs * volume * velocity
//
// Operators are evaluated 4, 3, 2, 1 (a modulator always has a higher number than its target), per sample, in float. The sine is a 7th-order odd polynomial on a quarter wave
// (error 1.2e-6, no table: a table in the cold part would be read from SDRAM by the hot loop). Level is in 0.75 dB steps (0 = silent, 100 = full scale), an envelope is linear in amplitude
// during the attack and exponential during the decay and release (the time is that of a fall by 60 dB: a decay to a sustain level covers 99.9 % of its way in that time). An operator
// whose frequency would pass Nyquist fades out instead of folding. All state is in Voice (the instance block); no globals, no statics.
#pragma once

#include <o3module.h>

#include <stdint.h>

#include "fm_gen.h"

// The audio path is inlined into the glue's render() (the one O3M_HOT function): the hot part of the file is that loop; the stage changes and the parameters stay wherever the compiler puts them.
#define FM_INLINE __attribute__((always_inline)) inline

namespace fm {

constexpr uint32_t kOps = 4;
constexpr float kIndex = 2.0f;        // turns of phase swing for a modulator at level 100, envelope 1
constexpr float kFeedbackMax = 0.5f;  // turns at Feedback 7 (the feedback is the mean of the last two outputs of operator 4 times this)
constexpr float kDbPerLevel = 0.75f;  // Level steps
constexpr float kLog2Thousand = 9.9657843f;  // a fall by 60 dB is a factor of 1000
constexpr float kLn1000 = 6.9077553f;
constexpr float kMinAttackMs = 1.0f;
constexpr float kEnvFloor = 1.0e-4f;  // -80 dB: a release (or a decay to nothing) ends here

enum Stage : uint32_t { kOff = 0, kAttack, kDecay, kRelease };

// The sixteen algorithms (1..8 the original set, 9..16 added by OS-SNDMOD S2). mods[i]: bit j set = operator j+1 modulates operator i+1 (j > i); carriers: bit i set = operator i+1 is heard. (The names are the diagrams, 4 -> 3 meaning 4 modulates 3.)
struct Algorithm {
  uint8_t mods[kOps];
  uint8_t carriers;
};
constexpr uint32_t kAlgorithmCount = 16;
inline constexpr Algorithm kAlgorithms[kAlgorithmCount] = {
    {{0x2, 0x4, 0x8, 0x0}, 0x1},  // 1: 4 -> 3 -> 2 -> 1
    {{0x2, 0xC, 0x0, 0x0}, 0x1},  // 2: (4 + 3) -> 2 -> 1
    {{0x6, 0x0, 0x8, 0x0}, 0x1},  // 3: (4 -> 3) + 2 -> 1
    {{0x2, 0x4, 0x0, 0x0}, 0x9},  // 4: 3 -> 2 -> 1, and 4 alone
    {{0x2, 0x0, 0x8, 0x0}, 0x5},  // 5: 2 -> 1, and 4 -> 3
    {{0x0, 0x4, 0x8, 0x0}, 0x3},  // 6: 4 -> 3 -> 2, and 1 alone
    {{0x8, 0x8, 0x8, 0x0}, 0x7},  // 7: 4 -> 1, 2, 3
    {{0x0, 0x0, 0x0, 0x0}, 0xF},  // 8: all four alone
    {{0x6, 0x8, 0x8, 0x0}, 0x1},  // 9: 4 -> 3, 4 -> 2, (3 + 2) -> 1
    {{0x4, 0x4, 0x8, 0x0}, 0x3},  // 10: 4 -> 3 -> 1, 2
    {{0x4, 0x8, 0x0, 0x0}, 0x3},  // 11: 3 -> 1, and 4 -> 2
    {{0xE, 0x0, 0x0, 0x0}, 0x1},  // 12: (4 + 3 + 2) -> 1
    {{0x8, 0x4, 0x8, 0x0}, 0x3},  // 13: 4 -> 3 -> 2, and 4 -> 1
    {{0xC, 0xC, 0x0, 0x0}, 0x3},  // 14: (4 + 3) -> (2 + 1)
    {{0xA, 0x4, 0x0, 0x0}, 0x1},  // 15: (3 -> 2) + 4 -> 1
    {{0x2, 0x8, 0x0, 0x0}, 0x5},  // 16: 4 -> 2 -> 1, and 3 alone
};

// sin(2 pi x) for any float x (|x| < 2^20): the fraction, folded to a quarter wave, then the polynomial. Maximum error 1.2e-6.
FM_INLINE float sin_turns(float x) {
  x -= static_cast<float>(static_cast<int32_t>(x));  // (-1, 1)
  x -= x > 0.5f ? 1.0f : 0.0f;
  x += x < -0.5f ? 1.0f : 0.0f;  // [-0.5, 0.5]
  x = x > 0.25f ? 0.5f - x : x;  // sin(2 pi x) = sin(2 pi (0.5 - x))
  x = x < -0.25f ? -0.5f - x : x;
  const float x2 = x * x;
  return x * (6.28318051f + x2 * (-41.3392461f + x2 * (81.4080069f + x2 * -71.6076801f)));
}

struct Op {
  float phase, inc;        // turns, step a sample
  float env;               // 0..1
  float e_mul, e_add;      // the envelope's step: env = env * e_mul + e_add
  float e_lim, e_sg;       // the stage ends when (env - e_lim) * e_sg >= 0
  float gain;              // amp * nyq: what the output is scaled by
  float amp, nyq;          // the level (0..1), and the fade to 0 as the frequency nears Nyquist
  float ratio, dfac, mul;  // the frequency ratio, the detune as a factor (2^(cents / 1200)), and their product
  float a_step, kd, kr, sus, sc;  // attack step a sample, decay and release factors a sample, sustain, sc = sus * (1 - kd)
  uint32_t stage;
};

// What an instance holds (the manifest's state_bytes is the room: static_assert in fm.cpp).
struct Voice {
  const o3m_host* host;
  float rate_ms;     // samples in a millisecond
  float inc_unit;    // phase step a sample of "note 0": 440 * 2^(-69/12) / rate
  float base_inc;    // of the note being played
  float note_f;      // OS-MODPITCH: the note being played, as a float: pitch() moves base_inc to note_f + the host's offset
  float volume, velocity, vel_sens, out_gain;
  float fb, fb0, fb1;  // feedback: kFeedbackMax * 2^(F - 7) / 2 (0 for F = 0), and the last two outputs of operator 4
  float w[kOps][kOps];  // w[i][j]: kIndex when operator j+1 modulates operator i+1, else 0
  float cg[kOps];       // 1 for a carrier, else 0
  Op op[kOps];
};

namespace detail {

inline float clampf(float x, float lo, float hi) { return x >= lo ? (x <= hi ? x : hi) : lo; }  // a NaN is lo
inline float round_(float x) { return static_cast<float>(static_cast<int32_t>(x + (x >= 0.0f ? 0.5f : -0.5f))); }

}  // namespace detail

// ---- envelope stages --------------------------------------------------------------------------------------------------------------------------------------
// apply_stage() loads the constants of the operator's current stage (the stage itself is kept): it is also what a parameter change does to a running note.
inline void apply_stage(Op& o) {
  switch (o.stage) {
    case kAttack:
      o.e_mul = 1.0f;
      o.e_add = o.a_step;
      o.e_lim = 1.0f;
      o.e_sg = 1.0f;
      break;
    case kDecay:
      o.e_mul = o.kd;
      o.e_add = o.sc;
      o.e_lim = o.sus > 0.0f ? -1.0f : kEnvFloor;  // with a sustain level the decay never ends (it settles); without, it ends at the floor
      o.e_sg = -1.0f;
      break;
    case kRelease:
      o.e_mul = o.kr;
      o.e_add = 0.0f;
      o.e_lim = kEnvFloor;
      o.e_sg = -1.0f;
      break;
    default:
      o.env = 0.0f;
      o.e_mul = 0.0f;
      o.e_add = 0.0f;
      o.e_lim = 2.0f;  // never
      o.e_sg = 1.0f;
      break;
  }
}

inline void set_stage(Op& o, Stage s) {
  o.stage = s;
  apply_stage(o);
}

// The envelope of an operator reached its limit.
inline void advance(Op& o) {
  if (o.stage == kAttack) {
    o.env = 1.0f;
    set_stage(o, kDecay);
  } else {
    set_stage(o, kOff);  // the end of a release, or of a decay to nothing
  }
}

// ---- pitch ----------------------------------------------------------------------------------------------------------------------------------------------------
// The operator's step from the note's: an operator whose frequency is past 0.4 of the rate fades out, at 0.5 it is silent.
inline void refresh_pitch(Voice& v, Op& o) {
  float inc = v.base_inc * o.mul;
  inc = inc > 0.5f ? 0.5f : inc;
  o.inc = inc;
  o.nyq = inc < 0.4f ? 1.0f : (0.5f - inc) * 10.0f;
  o.gain = o.amp * o.nyq;
}

inline void derive_feedback(Voice& v, uint32_t f) {
  v.fb = f == 0u ? 0.0f : 0.5f * kFeedbackMax * static_cast<float>(1u << f) * (1.0f / 128.0f);  // kFeedbackMax * 2^(F-7), halved: the mean of two outputs
}

inline void derive_gain(Voice& v) { v.out_gain = v.volume * (1.0f - v.vel_sens * (1.0f - v.velocity)); }

// ---- parameters ---------------------------------------------------------------------------------------------------------------------------------------------
static_assert(FM_PARAM_OP2_RATIO == 8 && FM_PARAM_OP3_RATIO == 16 && FM_PARAM_OP4_RATIO == 24, "module.json: eight parameters an operator page");
static_assert(FM_PARAM_ALGORITHM == 7 && FM_PARAM_FEEDBACK == 15 && FM_PARAM_VOLUME == 23 && FM_PARAM_VEL_SENS == 31, "module.json: the global parameters close each page");

// A time in milliseconds as the factor a sample of a fall by 60 dB over that time. For the long times the factor is within 1e-5 of 1, where the host's exp2 (relative error 1e-6) would be
// off by a tenth of the step: a Taylor series is exact there and needs no call.
inline float fall_factor(const Voice& v, float ms) {
  const float y = kLn1000 / (ms * v.rate_ms);  // e^-y per sample
  if (y < 0.05f) {
    return 1.0f - y * (1.0f - y * (0.5f - y * (1.0f / 6.0f - y * (1.0f / 24.0f))));
  }
  return v.host->exp2_f(-kLog2Thousand / (ms * v.rate_ms));
}

inline void set_algorithm(Voice& v, uint32_t a) {
  const Algorithm& alg = kAlgorithms[a < kAlgorithmCount ? a : 0u];
  for (uint32_t i = 0; i < kOps; ++i) {
    v.cg[i] = ((alg.carriers >> i) & 1u) != 0u ? 1.0f : 0.0f;
    for (uint32_t j = 0; j < kOps; ++j) {
      v.w[i][j] = ((alg.mods[i] >> j) & 1u) != 0u ? kIndex : 0.0f;
    }
  }
}

// The OS does not clamp: every value is held to the manifest's range here (a stored blob can hold anything, NaN included), and whole numbers rounded.
inline void voice_set_param(Voice& v, uint32_t id, float value) {
  if (id >= FM_PARAM_COUNT) {
    return;
  }
  const o3m_param& p = fm_params[id];
  value = detail::clampf(value, p.min, p.max);
  if ((p.flags & O3M_PARAM_INT) != 0u) {
    value = detail::round_(value);
  }
  const uint32_t field = id & 7u;
  if (field == 7u) {  // the page's closing parameter: Algorithm, Feedback, Volume, Vel Sens
    switch (id >> 3) {
      case 0: set_algorithm(v, static_cast<uint32_t>(value) - 1u); break;
      case 1: derive_feedback(v, static_cast<uint32_t>(value)); break;
      case 2:
        v.volume = v.host->exp2_f(value * 0.16609640f);  // dB to a ratio: 2^(dB / 6.0206)
        derive_gain(v);
        break;
      default:
        v.vel_sens = value * 0.01f;
        derive_gain(v);
        break;
    }
    return;
  }
  Op& o = v.op[id >> 3];
  switch (field) {
    case 0:  // the ratio: 0 is 0.5, then 1, 2, 3 ... 31
      o.ratio = value < 1.0f ? 0.5f : value;
      o.mul = o.ratio * o.dfac;
      refresh_pitch(v, o);
      break;
    case 1:  // the detune, cents
      o.dfac = v.host->exp2_f(value * (1.0f / 1200.0f));
      o.mul = o.ratio * o.dfac;
      refresh_pitch(v, o);
      break;
    case 2:  // the level: 0 is silence, then 0.75 dB a step up to 100 = 1
      o.amp = value <= 0.0f ? 0.0f : v.host->exp2_f((value - 100.0f) * (kDbPerLevel * 0.16609640f));
      o.gain = o.amp * o.nyq;
      break;
    case 3:
      o.a_step = 1.0f / ((value > kMinAttackMs ? value : kMinAttackMs) * v.rate_ms);
      if (o.stage == kAttack) {
        apply_stage(o);
      }
      break;
    case 4:
      o.kd = fall_factor(v, value);
      o.sc = o.sus * (1.0f - o.kd);
      if (o.stage == kDecay) {
        apply_stage(o);
      }
      break;
    case 5:
      o.sus = value * 0.01f;
      o.sc = o.sus * (1.0f - o.kd);
      if (o.stage == kDecay) {
        apply_stage(o);  // aim at the new level
      }
      break;
    default:
      o.kr = fall_factor(v, value);
      if (o.stage == kRelease) {
        apply_stage(o);
      }
      break;
  }
}

inline void voice_init(Voice& v, const o3m_host* host) {
  v.host = host;
  const float rate = host->sample_rate;
  v.rate_ms = rate * 0.001f;
  v.inc_unit = 440.0f / rate * host->exp2_f(-69.0f / 12.0f);
  v.velocity = 1.0f;
  for (uint32_t i = 0; i < kOps; ++i) {
    v.op[i].nyq = 1.0f;
    v.op[i].dfac = 1.0f;
    v.op[i].ratio = 1.0f;
    v.op[i].mul = 1.0f;
    apply_stage(v.op[i]);  // off
  }
  for (uint32_t i = 0; i < FM_PARAM_COUNT; ++i) {
    voice_set_param(v, i, fm_params[i].def);
  }
}

// The voice sounds while the envelope of a carrier that is audible (its level is above 0) runs: a modulator alone is not heard, nor a carrier at level 0.
inline bool voice_busy(const Voice& v) {
  for (uint32_t i = 0; i < kOps; ++i) {
    if (v.cg[i] != 0.0f && v.op[i].gain != 0.0f && v.op[i].stage != kOff) {
      return true;
    }
  }
  return false;
}

// `slide`: a note the host ramps over the one that sounds (O3M_NOTE_LEGATO | O3M_NOTE_RAMP, OS-MODPITCH): the envelopes are not restarted, only the pitch moves to the note.
inline void voice_note_on(Voice& v, uint32_t note, uint32_t velocity, bool slide = false) {
  const bool fresh = !voice_busy(v);
  v.note_f = static_cast<float>(note > 127u ? 127u : note);
  v.base_inc = v.inc_unit * v.host->exp2_f(v.note_f * (1.0f / 12.0f));  // (a note puts the host's offset back to 0)
  if (slide && !fresh) {
    for (uint32_t i = 0; i < kOps; ++i) {
      refresh_pitch(v, v.op[i]);
    }
    return;
  }
  v.velocity = static_cast<float>(velocity < 1u ? 1u : (velocity > 127u ? 127u : velocity)) * (1.0f / 127.0f);
  derive_gain(v);
  if (fresh) {
    v.fb0 = 0.0f;
    v.fb1 = 0.0f;
  }
  for (uint32_t i = 0; i < kOps; ++i) {
    Op& o = v.op[i];
    refresh_pitch(v, o);
    if (fresh) {
      o.phase = 0.0f;
      o.env = 0.0f;  // nothing is heard: every envelope starts from nothing; over a sounding note they continue from where they are (no click)
    }
    set_stage(o, kAttack);
  }
}

// OS-MODPITCH (ABI 1.8): the host's Slide / Glide offset from the note, in semitones: one exp2 and the four operators' steps. `per_frame` is not followed (a staircase of one block).
inline void voice_pitch(Voice& v, float semitones, float per_frame) {
  (void)per_frame;
  v.base_inc = v.inc_unit * v.host->exp2_f((v.note_f + detail::clampf(semitones, -128.0f, 128.0f)) * (1.0f / 12.0f));
  for (uint32_t i = 0; i < kOps; ++i) {
    refresh_pitch(v, v.op[i]);
  }
}

inline void voice_note_off(Voice& v) {
  for (uint32_t i = 0; i < kOps; ++i) {
    if (v.op[i].stage != kOff) {
      set_stage(v.op[i], kRelease);
    }
  }
}

// ---- audio ----------------------------------------------------------------------------------------------------------------------------------------------------
// One operator for one sample: the phase, the envelope, the sine of the phase plus the modulation.
FM_INLINE float tick(Op& o, float m) {
  float p = o.phase + o.inc;
  p -= p >= 1.0f ? 1.0f : 0.0f;
  o.phase = p;
  float e = o.env * o.e_mul + o.e_add;
  if ((e - o.e_lim) * o.e_sg >= 0.0f) {
    o.env = e;
    advance(o);
    e = o.env;
  }
  o.env = e;
  return o.gain != 0.0f ? sin_turns(p + m) * (e * o.gain) : 0.0f;
}

// Fills `frames` samples; returns 1 while the voice may still sound.
FM_INLINE uint32_t voice_render(Voice& v, float* out, uint32_t frames) {
  if (!voice_busy(v)) {
    for (uint32_t i = 0; i < frames; ++i) {
      out[i] = 0.0f;
    }
    return 0;
  }
  const float g = v.out_gain;
  for (uint32_t i = 0; i < frames; ++i) {
    const float o4 = tick(v.op[3], v.fb * (v.fb0 + v.fb1));
    v.fb1 = v.fb0;
    v.fb0 = o4;
    const float o3 = tick(v.op[2], v.w[2][3] * o4);
    const float o2 = tick(v.op[1], v.w[1][2] * o3 + v.w[1][3] * o4);
    const float o1 = tick(v.op[0], v.w[0][1] * o2 + v.w[0][2] * o3 + v.w[0][3] * o4);
    out[i] = (v.cg[0] * o1 + v.cg[1] * o2 + v.cg[2] * o3 + v.cg[3] * o4) * g;
  }
  return voice_busy(v) ? 1u : 0u;
}

}  // namespace fm
