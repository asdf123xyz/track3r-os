// SPDX-License-Identifier: MIT
//
// TEMPLATE of an instrument module (SDK guide: module/sdk/README.md). `o3m_gen.py new --kind instrument --id <id>` copies this directory under your id; every
// place that says what the module is lives in module.json, from which the descriptor header (example_instrument_gen.h) is generated: edit the manifest, regenerate,
// implement. What is yours: the State, the functions below, the view.
//
// The rules the packer and the OS hold you to (o3module.h): float only (no double, no libm); no globals and no statics (all state is in State, which the OS gives zeroed);
// no heap, no I/O, no blocking and bounded loops in render, set_param, note_on and note_off (they run in the audio interrupt); only render and what it calls are O3M_HOT.
#include <o3module.h>

#include "example_instrument_gen.h"

namespace {

// All the state of one instance. The manifest's `state_bytes` is the room the OS gives it: the static_assert holds the two together.
struct State {
  const o3m_host* host;
  float rate;        // Hz
  float phase;       // 0..1
  float inc;         // phase step a sample (of the note's pitch)
  float env;         // 1 at the note's start, falls by `decay` a sample
  float decay;       // the per-sample factor of the envelope
  float gain;        // linear
  float level_db;
  float decay_ms;
  float pitch;       // semitones
  float note;        // the note being played
  uint32_t sounding;
};
static_assert(sizeof(State) <= EXAMPLE_INSTRUMENT_STATE_BYTES, "module.json: state_bytes is too small for State");

inline float clampf(float x, float lo, float hi) { return x >= lo ? (x <= hi ? x : hi) : lo; }  // a NaN is not >= lo: it becomes lo, where the usual x < lo ? lo : ... would pass it on

// Control-rate maths, with the host's services (init, set_param and note_on may call them: they are cold).
void derive(State* s) {
  s->gain = s->host->exp2_f(s->level_db * 0.16609640f);  // dB to a ratio: 2^(dB / 6.0206)
  s->decay = s->host->exp2_f(-1.442695f / (s->decay_ms * 0.001f * s->rate));
  s->inc = 440.0f * s->host->exp2_f((s->note + s->pitch - 69.0f) * (1.0f / 12.0f)) / s->rate;
}

int init(void* raw, const o3m_host* host) {
  State* s = static_cast<State*>(raw);
  s->host = host;
  s->rate = host->sample_rate;
  s->level_db = example_instrument_params[EXAMPLE_INSTRUMENT_PARAM_LEVEL].def;
  s->pitch = example_instrument_params[EXAMPLE_INSTRUMENT_PARAM_PITCH].def;
  s->decay_ms = example_instrument_params[EXAMPLE_INSTRUMENT_PARAM_DECAY].def;
  s->note = 69.0f;
  derive(s);
  return 0;
}

// The OS does not clamp: the module must (a stored parameter blob can hold any finite number, and a damaged one a NaN: clampf sends that to the lower bound).
void set_param(void* raw, uint32_t id, float value) {
  State* s = static_cast<State*>(raw);
  const o3m_param& p = example_instrument_params[id < EXAMPLE_INSTRUMENT_PARAM_COUNT ? id : 0u];
  value = clampf(value, p.min, p.max);
  switch (id) {
    case EXAMPLE_INSTRUMENT_PARAM_LEVEL: s->level_db = value; break;
    case EXAMPLE_INSTRUMENT_PARAM_PITCH: s->pitch = value; break;
    case EXAMPLE_INSTRUMENT_PARAM_DECAY: s->decay_ms = value; break;
    default: return;
  }
  derive(s);
}

void note_on(void* raw, uint32_t note, uint32_t velocity, uint32_t flags) {
  State* s = static_cast<State*>(raw);
  (void)flags;
  s->note = static_cast<float>(note > 127u ? 127u : note);
  s->env = static_cast<float>(velocity > 127u ? 127u : velocity) * (1.0f / 127.0f);
  s->sounding = 1;
  derive(s);
}

void note_off(void* raw, uint32_t) { (void)raw; }  // a plucked sound: the envelope decides when it ends

void shutdown(void*) {}

// sin(2 pi p) for p in [0, 1): the parabola 4x(1 - |x|) of x = 2p - 1 (negated), refined by y = 0.775 y + 0.225 y |y|: an error of 1e-3, no table, no libm.
O3M_HOT uint32_t render(void* raw, const float*, float* out, uint32_t frames) {
  State* s = static_cast<State*>(raw);
  float phase = s->phase;
  float env = s->env;
  for (uint32_t i = 0; i < frames; ++i) {
    const float x = 2.0f * phase - 1.0f;
    float y = -4.0f * x * (1.0f - (x < 0.0f ? -x : x));
    y = 0.775f * y + 0.225f * y * (y < 0.0f ? -y : y);
    out[i] = y * env * s->gain;
    env *= s->decay;
    phase += s->inc;
    phase -= phase >= 1.0f ? 1.0f : 0.0f;
  }
  s->phase = phase;
  s->env = env;
  if (env < 1.0e-4f) {
    s->sounding = 0;
  }
  return s->sounding;  // 0 lets the OS skip the instance until the next note
}

// ---- the view ------------------------------------------------------------------------------------------------------------------------
// view_draw() paints through canvas->draw (rectangles, text, polylines, curves; colours are the theme's roles) from the parameter values in canvas->params, not from the
// state (the audio thread may be changing it). Budgets per frame: O3M_VIEW_MAX_CALLS calls, O3M_VIEW_MAX_POINTS points, O3M_VIEW_MAX_TEXT characters, 4 ms; over one, the view is
// blanked and the module quarantined.
float fexp2(float x) {  // 2^x for the plot: a polynomial of the fraction (error 1e-5), no libm
  x = x < -60.0f ? -60.0f : (x > 60.0f ? 60.0f : x);
  float whole = static_cast<float>(static_cast<int32_t>(x));
  whole = whole > x ? whole - 1.0f : whole;
  const float f = x - whole;
  const float p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * (0.0096181f + f * 0.0013333f))));
  return p * __builtin_bit_cast(float, static_cast<uint32_t>(static_cast<int32_t>(whole) + 127) << 23);
}

void view_draw(void*, uint32_t view, o3m_canvas* c) {
  if (view != EXAMPLE_INSTRUMENT_VIEW_ENVELOPE || c->param_count < EXAMPLE_INSTRUMENT_PARAM_COUNT) {
    return;
  }
  const float decay_ms = c->params[EXAMPLE_INSTRUMENT_PARAM_DECAY];
  float y[32];
  for (uint32_t i = 0; i < 32; ++i) {  // three decays: the envelope's e-fold time is `decay`
    y[i] = fexp2(-3.0f * 1.442695f * static_cast<float>(i) / 31.0f);
  }
  o3m_curve curve;  // (o3m_curve_set, not an initialiser list: a list with zeros becomes a memset, and a module has no libc)
  o3m_curve_set(&curve, nullptr, y, 32, O3M_AXIS_LINEAR, O3M_AXIS_LINEAR, 0.0f, decay_ms * 0.003f, 0.0f, 1.0f, O3M_CURVE_AXES | O3M_CURVE_FILL, O3M_COLOR_INSTR);
  c->draw->curve(c, &curve);
}

const o3m_ui kUi = {O3M_UI_SIZE(EXAMPLE_INSTRUMENT_ABI_MINOR), nullptr, nullptr, view_draw, nullptr, nullptr};

}  // namespace

extern "C" O3M_EXPORT const o3m_module o3m_entry __attribute__((section(".o3m_entry"))) = {
    EXAMPLE_INSTRUMENT_ENTRY_HEAD, sizeof(State), EXAMPLE_INSTRUMENT_ENTRY_PARAMS, init, render, set_param, note_on, note_off, shutdown, EXAMPLE_INSTRUMENT_ENTRY_VIEWS(&kUi),
};
