// SPDX-License-Identifier: MIT
//
// TEMPLATE of an effect module (SDK guide: module/sdk/README.md). `o3m_gen.py new --kind effect --id <id>` copies this directory under your id; the descriptor header
// is generated from module.json. An effect has one mono input and one mono output; render(in, out, frames) may be called with in == out. note_on and note_off do not apply to
// an effect, and the OS never calls them (the descriptor still carries them: an effect that wants to follow the notes of its track is a later ABI minor).
#include <o3module.h>

#include "example_effect_gen.h"

namespace {

struct State {
  const o3m_host* host;
  float rate;
  float a;       // the one-pole coefficient
  float y;       // the filter's memory
  float mix;     // 0..1
  uint32_t bypass;
  float cutoff;
};
static_assert(sizeof(State) <= EXAMPLE_EFFECT_STATE_BYTES, "module.json: state_bytes is too small for State");

inline float clampf(float x, float lo, float hi) { return x >= lo ? (x <= hi ? x : hi) : lo; }  // a NaN is not >= lo: it becomes lo, where the usual x < lo ? lo : ... would pass it on

void derive(State* s) {
  const float fc = clampf(s->cutoff, 1.0f, s->rate * 0.45f);
  s->a = 1.0f - s->host->exp2_f(-6.2831853f * fc / s->rate * 1.442695f);  // 1 - e^(-2 pi fc / fs)
}

int init(void* raw, const o3m_host* host) {
  State* s = static_cast<State*>(raw);
  s->host = host;
  s->rate = host->sample_rate;
  s->cutoff = example_effect_params[EXAMPLE_EFFECT_PARAM_CUTOFF].def;
  s->mix = example_effect_params[EXAMPLE_EFFECT_PARAM_MIX].def * 0.01f;
  s->bypass = 0;
  derive(s);
  return 0;
}

void set_param(void* raw, uint32_t id, float value) {
  State* s = static_cast<State*>(raw);
  switch (id) {
    case EXAMPLE_EFFECT_PARAM_CUTOFF: s->cutoff = clampf(value, 20.0f, 20000.0f); derive(s); break;
    case EXAMPLE_EFFECT_PARAM_MIX: s->mix = clampf(value, 0.0f, 100.0f) * 0.01f; break;
    case EXAMPLE_EFFECT_PARAM_BYPASS: s->bypass = value >= 0.5f ? 1u : 0u; break;
    default: break;
  }
}

void note_on(void*, uint32_t, uint32_t, uint32_t) {}
void note_off(void*, uint32_t) {}
void shutdown(void*) {}

O3M_HOT uint32_t render(void* raw, const float* in, float* out, uint32_t frames) {
  State* s = static_cast<State*>(raw);
  if (s->bypass != 0u) {
    for (uint32_t i = 0; i < frames; ++i) {
      out[i] = in[i];
    }
    return 1;
  }
  float y = s->y;
  const float a = s->a;
  const float mix = s->mix;
  for (uint32_t i = 0; i < frames; ++i) {
    const float x = in[i];
    y += a * (x - y);
    out[i] = x + mix * (y - x);
  }
  s->y = y;
  return 1;  // an effect with a tail returns 0 when it is silent and idle; this one is cheap enough to run always
}

// ---- the view: the magnitude response in dB, from the Cutoff and Mix values the canvas carries --------------------------------------------------
float flog2(float x) {  // log2 for x > 0: the exponent and a polynomial of the mantissa (ln on [1, 2), error 1e-4, then times log2 e)
  const uint32_t b = __builtin_bit_cast(uint32_t, x);
  const float e = static_cast<float>(static_cast<int32_t>((b >> 23) & 255u) - 127);
  const float m = __builtin_bit_cast(float, (b & 0x007FFFFFu) | 0x3F800000u);
  return e + 1.442695f * (-1.7417939f + (2.8212026f + (-1.4699568f + (0.44717955f - 0.056570851f * m) * m) * m) * m);
}

float fexp2(float x) {
  x = x < -60.0f ? -60.0f : (x > 60.0f ? 60.0f : x);
  float whole = static_cast<float>(static_cast<int32_t>(x));
  whole = whole > x ? whole - 1.0f : whole;
  const float f = x - whole;
  const float p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * (0.0096181f + f * 0.0013333f))));
  return p * __builtin_bit_cast(float, static_cast<uint32_t>(static_cast<int32_t>(whole) + 127) << 23);
}

void view_draw(void*, uint32_t view, o3m_canvas* c) {
  if (view != EXAMPLE_EFFECT_VIEW_RESPONSE || c->param_count < EXAMPLE_EFFECT_PARAM_COUNT) {
    return;
  }
  const float fc = clampf(c->params[EXAMPLE_EFFECT_PARAM_CUTOFF], 20.0f, 20000.0f);
  const float mix = clampf(c->params[EXAMPLE_EFFECT_PARAM_MIX], 0.0f, 100.0f) * 0.01f;
  const bool bypass = c->params[EXAMPLE_EFFECT_PARAM_BYPASS] >= 0.5f;
  float f[40];
  float db[40];
  const float step = flog2(20000.0f / 20.0f) / 39.0f;
  for (uint32_t i = 0; i < 40; ++i) {
    f[i] = 20.0f * fexp2(step * static_cast<float>(i));
    const float x = f[i] / fc;
    const float lp = 1.0f / (1.0f + x * x);                       // |H|^2 of the one-pole
    const float wet = bypass ? 1.0f : (1.0f - mix) + mix * (lp > 0.0f ? lp : 0.0f);  // a rough sum of powers: an illustration, not the exact mix of two phases
    db[i] = clampf(-3.0103f * flog2(1.0f / wet), -48.0f, 12.0f);
  }
  o3m_curve curve;
  o3m_curve_set(&curve, f, db, 40, O3M_AXIS_LOG_FREQ, O3M_AXIS_DB, 20.0f, 20000.0f, -48.0f, 12.0f, O3M_CURVE_AXES | O3M_CURVE_FILL, O3M_COLOR_FX);
  c->draw->curve(c, &curve);
}

const o3m_ui kUi = {O3M_UI_SIZE(EXAMPLE_EFFECT_ABI_MINOR), nullptr, nullptr, view_draw, nullptr, nullptr};

}  // namespace

extern "C" O3M_EXPORT const o3m_module o3m_entry __attribute__((section(".o3m_entry"))) = {
    EXAMPLE_EFFECT_ENTRY_HEAD, sizeof(State), EXAMPLE_EFFECT_ENTRY_PARAMS, init, render, set_param, note_on, note_off, shutdown, EXAMPLE_EFFECT_ENTRY_VIEWS(&kUi),
};
