// SPDX-License-Identifier: MIT
//
// "Scope Spectrum" (OS-H2b, new Community OS behaviour, ABI 1.4 until OS-APPUX S3, now 1.10, capability audio_tap): an oscilloscope and a spectrum analyser for what the Tracker plays (MASTER, the default) or what comes
// into the line input (INPUT), one app with a VIEW parameter. It reads the newest frames of the OS's audio tap with tap_latest; it makes no sound and does no audio work: everything here runs in
// the 33 ms app frame (view_draw), never in the render.
//
//   SCOPE      the newest 2,048 frames are searched for a rising zero crossing (after the signal has been below -1/8 of its peak: noise around zero does not fire it); the trace starts an
//              eighth of the span before it, so the edge sits at 1/8 of the width. No crossing (silence, DC, noise, a very low note) = FREE RUN: the newest span as it is. TIMEBASE 1..5 =
//              128, 256, 512, 1024, 1536 frames across the area (2.9, 5.8, 11.6, 23.2, 34.8 ms at 44.1 kHz). The trace is peak-decimated to at most 248 points (a long span keeps its peaks).
//   SPECTRUM   the newest 1,024 frames, the mean removed (no DC), a Hann window, a 1,024-point complex FFT (../common/o3m_fft.h, written from scratch), the power of each of 48 bars of
//              1/5.35 octave from 40 Hz to 20 kHz (the loudest bin in the bar, or the interpolation between the two bins around its centre where a bar is narrower than a bin), in dBFS
//              (a full-scale sine is 0 dB), 60 dB of range. The bars fall 60 dB/s, a peak-hold line falls 12 dB/s. PEAK shows the loudest bin (its centre frequency: 43 Hz resolution).
//   Up / Down, the encoder, Shift + the encoder, Delete (the grammar of o3m_edit.h, OS-APPUX S3): the three rows at the bottom, kept with the project: VIEW 1 = SCOPE, 2 = SPECTRUM; SOURCE 1 = INPUT,
//   2 = MASTER; TIMEBASE 1..5 (SCOPE only).
//
// COST per 33 ms frame (ESTIMATE from the compiled loops and the operation counts, owed to the unit: QA-MOD-033; the app's state is in SDRAM, which the core does not cache, about 28 cycles a
// load). SCOPE: copy 2,048 frames from the ring (about 30 kcycles), trigger scan and peak decimation of the window in the state (a few 2,048-element passes, up to 0.2 ms each from SDRAM),
// the draw calls: 0.5 to 1.2 ms. SPECTRUM: a 1,024-point transform is 28 chunks (the load and mean, two window passes, the bit reversal and 10 stages in chunks of 256 butterflies, the 48 bars;
// a butterfly is 40 to 50 cycles in cache, up to 350 from SDRAM), at most 12 chunks AND 2 ms of the clock a frame, the clock checked BEFORE every chunk: the transform takes 3 frames at the
// nominal speed (10 a second) and view_draw stays under its 4 ms (O3M_VIEW_MAX_US) if the estimates hold; if they do not, the guard cuts the work instead (slower bars, never a quarantine).
// The last bars stay on the screen while the next transform runs.
#include <o3module.h>

#include "o3m_edit.h"
#include "o3m_fft.h"
#include "o3m_text.h"
#include "scope_gen.h"
#include "scope_math.h"

namespace {

constexpr uint32_t kWinMax = 2048;       // O3M_TAP_LATEST_MAX
constexpr uint32_t kN = 1024;            // spectrum points
constexpr uint32_t kBars = 48;
constexpr float kFirstHz = 40.0f;
constexpr float kOctaves = 8.9657843f;   // log2(20000 / 40)
constexpr float kBarFallDb = 60.0f;      // per second
constexpr float kPeakFallDb = 12.0f;
constexpr float kFloorDb = -60.0f;
constexpr float kNormPower = 1.4210855e-14f;  // 1 / (32768 * N / 4)^2: a full-scale sine under the Hann window is 0 dB
constexpr uint32_t kChunksPerFrame = 12;  // chunks of the transform a frame at most (256 butterflies or 512 array elements each)...
constexpr uint32_t kGuardMs = 2;          // ...and none once the clock has moved this much since the frame began (checked BEFORE every chunk, the first too)
constexpr uint32_t kFftChunk = 256;
constexpr uint32_t kPassChunk = 512;
constexpr uint32_t kMaxPoints = 248;     // two polylines of 125 and 124 points, both under O3M_VIEW_MAX_POLY_POINTS

const uint16_t kSpan[5] = {128, 256, 512, 1024, 1536};

struct State {
  const o3m_host* host;
  const o3m_io_api* io;
  uint32_t view;       // 1 = SCOPE, 2 = SPECTRUM
  uint32_t source;     // O3M_TAP_INPUT or O3M_TAP_MASTER
  uint32_t timebase;   // 1..5
  uint32_t row;        // the row of the three the cursor is on
  uint32_t selected;   // what the tap was last told (0: nothing yet)
  uint32_t shown_view; // the view the work arrays are for
  // scope
  uint32_t got;        // real frames of the last tap_latest (0: none yet)
  int32_t trig;        // trigger index in win, -1: free run
  // spectrum
  o3fft::Job job;
  o3fft::Hann hann;
  uint32_t phase;      // 0 idle, 1 window pass, 2 FFT, 3 bars
  uint32_t idx;        // progress through the window pass
  float mean;
  uint32_t fresh;      // 1 after the first finished transform
  uint32_t upd;        // now_ms of the last finished transform
  uint32_t peak_bin;   // the loudest bin of the last transform
  float peak_db;
  float bar[kBars];    // dBFS, falling
  float hold[kBars];   // dBFS, the peak-hold line
  float edge[kBars + 1u];  // the bars' edges in bins (fractions)
  alignas(4) int16_t win[kWinMax];
  float re[kN];
  float im[kN];
  float tw_c[kN / 2];
  float tw_s[kN / 2];
};
static_assert(sizeof(State) <= SCOPE_STATE_BYTES, "module.json: state_bytes is too small for State");

bool has_tap(const State* s) {
  return s->io != nullptr && s->io->now_ms != nullptr && s->io->size >= __builtin_offsetof(o3m_io_api, tap_latest) + sizeof(s->io->tap_latest) && s->io->tap_select != nullptr &&
         s->io->tap_latest != nullptr;
}

void reset_spectrum(State* s) {
  s->phase = 0;
  s->fresh = 0;
  s->peak_bin = 0;
  s->peak_db = -120.0f;
  for (uint32_t b = 0; b < kBars; ++b) {
    s->bar[b] = -120.0f;
    s->hold[b] = -120.0f;
  }
}

int init(void* raw, const o3m_host* host) {
  State* s = static_cast<State*>(raw);
  s->host = host;
  s->io = O3M_HOST_HAS(host, io) ? host->io : nullptr;
  s->view = 1;
  s->source = O3M_TAP_MASTER;
  s->timebase = 3;
  s->selected = 0;
  s->shown_view = 0;
  s->got = 0;
  s->trig = -1;
  s->upd = 0;
  reset_spectrum(s);
  if (host->draw == nullptr || host->log2_f == nullptr || host->exp2_f == nullptr || host->cos_f == nullptr || host->sin_f == nullptr) {
    return 1;
  }
  o3fft::init_twiddle(host, s->tw_c, s->tw_s, kN);
  const float bins_per_hz = static_cast<float>(kN) / host->sample_rate;
  for (uint32_t b = 0; b <= kBars; ++b) {
    s->edge[b] = kFirstHz * host->exp2_f(kOctaves * static_cast<float>(b) * (1.0f / static_cast<float>(kBars))) * bins_per_hz;
  }
  return 0;
}

uint32_t clamp_int(float v, float lo, float hi) {
  const float x = v >= lo ? (v <= hi ? v : hi) : lo;  // a NaN falls to the lower bound
  return static_cast<uint32_t>(x + 0.5f);
}

void set_param(void* raw, uint32_t id, float value) {
  State* s = static_cast<State*>(raw);
  if (id == SCOPE_PARAM_VIEW) {
    s->view = clamp_int(value, 1.0f, 2.0f);
  } else if (id == SCOPE_PARAM_SOURCE) {
    s->source = clamp_int(value, 1.0f, 2.0f);
  } else if (id == SCOPE_PARAM_TIMEBASE) {
    s->timebase = clamp_int(value, 1.0f, 5.0f);
  }
}

enum Row : uint32_t { kRowView, kRowSource, kRowTimebase, kRowCount };
constexpr int32_t kCoarse[kRowCount] = {1, 1, 2};  // Shift + the encoder: TIMEBASE by two a detent; VIEW and SOURCE are two choices
constexpr int32_t kRowsH = static_cast<int32_t>(kRowCount) * o3edit::kRowPitch + 4;  // the rows at the bottom of the page

// VIEW, SOURCE and TIMEBASE come back before enter() (set_param: the project's kept values or the manifest's defaults); enter() leaves them alone.
void enter(void* raw) {
  State* s = static_cast<State*>(raw);
  s->row = 0;
  s->selected = 0;
  s->shown_view = 0;
  s->got = 0;
  s->trig = -1;
  reset_spectrum(s);
}

void input(void* raw, const o3m_ui_event* ev) {
  State* s = static_cast<State*>(raw);
  const o3edit::Input in = o3edit::grammar(ev, &s->row, kRowCount, nullptr, 1);
  if (in.move == o3edit::kMoveValue || in.move == o3edit::kMoveDefault) {
    const int32_t d = in.move == o3edit::kMoveValue ? o3edit::steps(in, kCoarse[s->row]) : 0;
    const bool reset = in.move == o3edit::kMoveDefault;
    uint32_t* field = s->row == kRowView ? &s->view : (s->row == kRowSource ? &s->source : &s->timebase);
    const uint32_t def = s->row == kRowView ? 1u : (s->row == kRowSource ? static_cast<uint32_t>(O3M_TAP_MASTER) : 3u);
    const int32_t hi = s->row == kRowTimebase ? 5 : 2;
    *field = reset ? def : static_cast<uint32_t>(o3edit::clamp_i(static_cast<int32_t>(*field) + d, 1, hi));
    o3edit::own_set(s->io, s->row, static_cast<float>(*field));  // kept with the project
  }
}

// ---- the spectrum ---------------------------------------------------------------------------------------------------------------------

float power(const State* s, uint32_t k) { return s->re[k] * s->re[k] + s->im[k] * s->im[k]; }

float to_db(const State* s, float p) {
  const float x = p * kNormPower;
  return 3.0103f * s->host->log2_f(x > 1.0e-12f ? x : 1.0e-12f);  // 10 log10; the floor is -120 dB
}

// Chunk 1 of a transform: the newest 1,024 frames and their mean.
void begin_transform(State* s) {
  uint32_t got = s->io->tap_latest(s->win, kN);
  got = got <= kN ? got : 0u;  // (a host that answers more than it was given is not believed)
  if (got == 0u) {
    return;
  }
  int32_t sum = 0;
  for (uint32_t j = 0; j < kN; ++j) {
    sum += s->win[j];
  }
  s->mean = static_cast<float>(sum) * (1.0f / static_cast<float>(kN));
  o3fft::hann_begin(s->host, &s->hann, kN);
  s->idx = 0;
  s->phase = 1;
}

// A chunk of the window pass: no DC (it would leak into the lowest bars), the Hann window, the imaginary part cleared.
void window_chunk(State* s) {
  const uint32_t end = s->idx + kPassChunk < kN ? s->idx + kPassChunk : kN;
  for (uint32_t j = s->idx; j < end; ++j) {
    s->re[j] = (static_cast<float>(s->win[j]) - s->mean) * o3fft::hann_next(&s->hann);
    s->im[j] = 0.0f;
  }
  s->idx = end;
  if (end == kN) {
    o3fft::begin(&s->job, kN);
    s->phase = 2;
  }
}

// The transform is done: the 48 bars, the falls, the loudest bin.
void finish_transform(State* s, uint32_t now) {
  uint32_t dt = s->fresh != 0u ? now - s->upd : 0u;
  dt = dt < 250u ? dt : 250u;
  s->upd = now;
  const float sec = static_cast<float>(dt) * 0.001f;
  for (uint32_t b = 0; b < kBars; ++b) {
    const float lo = s->edge[b], hi = s->edge[b + 1u];
    uint32_t i0 = static_cast<uint32_t>(lo);
    i0 = static_cast<float>(i0) < lo ? i0 + 1u : i0;  // ceil
    uint32_t i1 = static_cast<uint32_t>(hi);          // floor
    i1 = i1 > kN / 2u - 1u ? kN / 2u - 1u : i1;
    float p = 0.0f;
    if (i1 >= i0) {
      for (uint32_t k = i0; k <= i1; ++k) {
        const float v = power(s, k);
        p = v > p ? v : p;
      }
    } else {  // narrower than a bin: the two around its centre
      const float c = 0.5f * (lo + hi);
      uint32_t k = static_cast<uint32_t>(c);
      k = k > kN / 2u - 2u ? kN / 2u - 2u : k;
      const float fr = c - static_cast<float>(k);
      p = power(s, k) * (1.0f - fr) + power(s, k + 1u) * fr;
    }
    const float db = to_db(s, p);
    const float fall_bar = s->bar[b] - kBarFallDb * sec;
    const float fall_hold = s->hold[b] - kPeakFallDb * sec;
    s->bar[b] = db > fall_bar ? db : fall_bar;
    s->hold[b] = db > fall_hold ? db : fall_hold;
  }
  uint32_t pk = 1;
  float pp = 0.0f;
  for (uint32_t k = 1; k < kN / 2u; ++k) {
    const float v = power(s, k);
    if (v > pp) {
      pp = v;
      pk = k;
    }
  }
  s->peak_bin = pk;
  s->peak_db = to_db(s, pp);
  s->fresh = 1;
  s->phase = 0;
}

// ---- once a frame -----------------------------------------------------------------------------------------------------------------------

void poll(State* s) {
  if (!has_tap(s)) {
    return;
  }
  const o3m_io_api* io = s->io;
  const uint32_t t0 = io->now_ms();
  if (s->selected != s->source) {
    if (io->tap_select(s->source) != 0) {
      return;
    }
    s->selected = s->source;
    reset_spectrum(s);
    s->got = 0;
  }
  if (s->shown_view != s->view) {
    s->shown_view = s->view;
    reset_spectrum(s);
    s->got = 0;
    s->trig = -1;
  }
  if (s->view == 1u) {
    uint32_t got = io->tap_latest(s->win, kWinMax);
    got = got <= kWinMax ? got : 0u;
    s->got = got;
    if (got == 0u) {
      s->trig = -1;
      return;
    }
    const uint32_t span = kSpan[s->timebase - 1u];
    const uint32_t pre = span / 8u;
    s->trig = scope_math::find_trigger(s->win, kWinMax, pre, kWinMax - span + pre);
    return;
  }
  // The transform, a chunk at a time: at most kChunksPerFrame chunks, none once the clock has moved kGuardMs (checked before EVERY chunk, the first too), and one finished transform at most
  // (the next starts in the next frame; the bars of the last one stay on the screen).
  for (uint32_t chunks = 0; chunks < kChunksPerFrame && io->now_ms() - t0 < kGuardMs; ++chunks) {
    if (s->phase == 0u) {
      begin_transform(s);
      if (s->phase == 0u) {
        break;  // nothing to transform yet
      }
    } else if (s->phase == 1u) {
      window_chunk(s);
    } else if (s->phase == 2u) {
      if (o3fft::step(&s->job, s->re, s->im, s->tw_c, s->tw_s, kFftChunk)) {
        s->phase = 3;
      }
    } else {
      finish_transform(s, t0);
      break;
    }
  }
}

// ---- drawing ----------------------------------------------------------------------------------------------------------------------------

void draw_header(const State* s, o3m_canvas* c, const char* title) {
  const o3m_draw_api* api = c->draw;
  api->rect(c, 0, 0, c->width, c->height, O3M_COLOR_PANE);
  api->text(c, 8, 4, title, O3M_FACE_DISPLAY, O3M_COLOR_TEXT);
  // the three rows at the bottom (the grammar of o3m_edit.h): VIEW, SOURCE, TIMEBASE (dim in the spectrum: it only sets the scope's span)
  o3edit::PageRow rows[kRowCount];
  rows[kRowView].name = "VIEW";
  rows[kRowView].dim = false;
  uint32_t n = o3text::put_text(rows[kRowView].value, 0, s->view == 1u ? "SCOPE" : "SPECTRUM");
  rows[kRowView].value[n] = '\0';
  rows[kRowSource].name = "SOURCE";
  rows[kRowSource].dim = false;
  n = o3text::put_text(rows[kRowSource].value, 0, s->source == O3M_TAP_INPUT ? "INPUT" : "MASTER");
  rows[kRowSource].value[n] = '\0';
  rows[kRowTimebase].name = "TIMEBASE";
  rows[kRowTimebase].dim = s->view != 1u;
  n = o3text::put_num(rows[kRowTimebase].value, 0, s->timebase);
  rows[kRowTimebase].value[n] = '\0';
  o3edit::draw_rows(c, 0, c->height - kRowsH, c->width / 2, rows, kRowCount, s->row);
}

void draw_scope(const State* s, o3m_canvas* c) {
  const o3m_draw_api* api = c->draw;
  const int32_t w = c->width;
  const int32_t h = c->height;
  const int32_t px = 8, py = 44;
  const int32_t pw = w - 16;
  const int32_t ph = h - py - kRowsH - 26;
  api->rect(c, px, py, pw, ph, O3M_COLOR_EMPTY);
  const int32_t cy = py + ph / 2;
  const uint32_t span = kSpan[s->timebase - 1u];
  for (int32_t i = 0; i <= 8; ++i) {  // eight divisions in time
    api->rect(c, px + (pw - 1) * i / 8, py, 1, ph, O3M_COLOR_PANE);
  }
  api->rect(c, px, py + ph / 4, pw, 1, O3M_COLOR_PANE);
  api->rect(c, px, py + ph * 3 / 4, pw, 1, O3M_COLOR_PANE);
  api->rect(c, px, cy, pw, 1, O3M_COLOR_TEXT_DIM);
  if (s->got != 0u) {
    const uint32_t pre = span / 8u;
    const uint32_t start = s->trig >= 0 ? static_cast<uint32_t>(s->trig) - pre : kWinMax - span;
    const uint32_t pts = span < kMaxPoints ? span : kMaxPoints;
    int16_t xy[2u * kMaxPoints];
    const int32_t half = ph / 2 - 2;
    for (uint32_t p = 0; p < pts; ++p) {
      const uint32_t lo = start + p * span / pts;
      uint32_t hi = start + (p + 1u) * span / pts;
      hi = hi > lo ? hi : lo + 1u;
      const int32_t v = scope_math::peak_of(s->win, lo, hi);
      xy[2u * p] = static_cast<int16_t>(px + static_cast<int32_t>(p) * (pw - 1) / static_cast<int32_t>(pts - 1u));
      xy[2u * p + 1u] = static_cast<int16_t>(cy - ((v * half) >> 15));
    }
    const uint32_t first = pts / 2u + 1u;  // the second polyline starts at the last point of the first: two lines under the limit of 128 points each
    api->polyline(c, xy, first, O3M_COLOR_PLAY);
    api->polyline(c, xy + 2u * (first - 1u), pts - first + 1u, O3M_COLOR_PLAY);
  }
  char text[64];
  uint32_t n = o3text::put_text(text, 0, "SPAN ");
  const uint32_t tenths = static_cast<uint32_t>(static_cast<float>(span) * 10000.0f / s->host->sample_rate + 0.5f);
  n = o3text::put_num(text, n, tenths / 10u);
  text[n++] = '.';
  text[n++] = static_cast<char>('0' + tenths % 10u);
  n = o3text::put_text(text, n, " MS   ");
  n = o3text::put_text(text, n, s->got == 0u ? "WAITING FOR AUDIO" : (s->trig >= 0 ? "TRIGGER RISING 0" : "FREE RUN"));
  text[n] = '\0';
  api->text(c, 8, h - kRowsH - 22, text, O3M_FACE_DATA, O3M_COLOR_TEXT);
}

int32_t scale_db(float db, int32_t height) {
  const float f = (db - kFloorDb) * (1.0f / -kFloorDb);
  const float c = f > 0.0f ? (f < 1.0f ? f : 1.0f) : 0.0f;
  return static_cast<int32_t>(c * static_cast<float>(height));
}

void draw_spectrum(const State* s, o3m_canvas* c) {
  const o3m_draw_api* api = c->draw;
  const int32_t w = c->width;
  const int32_t h = c->height;
  const int32_t px = 44, py = 44;
  const int32_t pw = w - px - 8;
  const int32_t ph = h - py - kRowsH - 44;
  api->rect(c, px, py, pw, ph, O3M_COLOR_EMPTY);
  char text[32];
  for (int32_t i = 0; i <= 5; ++i) {  // 0, -12, ... -60 dB
    const int32_t gy = py + ph * i / 5;
    api->rect(c, px, i == 5 ? gy - 1 : gy, pw, 1, O3M_COLOR_PANE);
    uint32_t n = 0;
    if (i != 0) {
      text[n++] = '-';
    }
    n = o3text::put_num(text, n, static_cast<uint32_t>(12 * i));
    text[n] = '\0';
    const int32_t ty = gy - 6;
    api->text(c, px - 4 - api->text_width(c, text, O3M_FACE_MICRO), ty < 0 ? 0 : ty, text, O3M_FACE_MICRO, O3M_COLOR_TEXT_DIM);
  }
  if (s->fresh != 0u) {
    int16_t xy[2u * kBars];
    for (uint32_t b = 0; b < kBars; ++b) {
      const int32_t bx = px + static_cast<int32_t>(b) * pw / static_cast<int32_t>(kBars);
      const int32_t bx2 = px + static_cast<int32_t>(b + 1u) * pw / static_cast<int32_t>(kBars);
      const int32_t bh = scale_db(s->bar[b], ph);
      if (bh > 0) {
        api->rect(c, bx, py + ph - bh, bx2 - bx - 1 > 1 ? bx2 - bx - 1 : 1, bh, s->bar[b] >= -6.0f ? O3M_COLOR_ACCENT : O3M_COLOR_PLAY);
      }
      xy[2u * b] = static_cast<int16_t>((bx + bx2) / 2);
      xy[2u * b + 1u] = static_cast<int16_t>(py + ph - 1 - scale_db(s->hold[b], ph - 1));
    }
    api->polyline(c, xy, kBars, O3M_COLOR_TEXT);
  }
  // the frequency scale: 100 Hz, 1 kHz, 10 kHz and the 20 kHz end
  static const uint16_t kTicks[4] = {100, 1000, 10000, 20000};
  for (uint32_t i = 0; i < 4u; ++i) {
    const float fx = s->host->log2_f(static_cast<float>(kTicks[i]) * (1.0f / kFirstHz)) * (1.0f / kOctaves);
    const int32_t tx = px + static_cast<int32_t>(fx * static_cast<float>(pw - 1));
    api->rect(c, tx, py + ph, 1, 5, O3M_COLOR_TEXT_DIM);
    uint32_t n = o3text::put_num(text, 0, kTicks[i] >= 1000u ? kTicks[i] / 1000u : kTicks[i]);
    if (kTicks[i] >= 1000u) {
      text[n++] = 'K';
    }
    text[n] = '\0';
    const int32_t tw = api->text_width(c, text, O3M_FACE_MICRO);
    int32_t lx = tx - tw / 2;
    lx = lx + tw > w ? w - tw : lx;
    api->text(c, lx, py + ph + 8, text, O3M_FACE_MICRO, O3M_COLOR_TEXT_DIM);
  }
  char line[64];
  uint32_t n = o3text::put_text(line, 0, "PEAK ");
  if (s->fresh != 0u && s->peak_db > -60.0f) {
    const uint32_t hz = static_cast<uint32_t>(static_cast<float>(s->peak_bin) * s->host->sample_rate / static_cast<float>(kN) + 0.5f);
    n = o3text::put_num(line, n, hz);
    n = o3text::put_text(line, n, " HZ  ");
    const int32_t db = static_cast<int32_t>(s->peak_db - 0.5f);  // (rounded down: the dB of a peak is never claimed higher than it is)
    n = db < 0 ? o3text::put_signed(line, n, db) : o3text::put_num(line, n, 0u);
    n = o3text::put_text(line, n, " DB");
  } else {
    n = o3text::put_text(line, n, "--");
  }
  line[n] = '\0';
  api->text(c, 8, h - kRowsH - 22, line, O3M_FACE_DATA, O3M_COLOR_TEXT);
}

void view_draw(void* raw, uint32_t, o3m_canvas* c) {
  State* s = static_cast<State*>(raw);
  poll(s);
  draw_header(s, c, s->view == 1u ? "SCOPE" : "SPECTRUM");
  if (!has_tap(s)) {
    c->draw->text(c, 8, 60, "NO AUDIO TAP FOR THIS APP", O3M_FACE_DATA, O3M_COLOR_ACCENT);
    return;
  }
  if (s->view == 1u) {
    draw_scope(s, c);
  } else {
    draw_spectrum(s, c);
  }
}

void leave(void*) {}
void shutdown(void*) {}

const o3m_ui kUi = {O3M_UI_SIZE(SCOPE_ABI_MINOR), enter, input, view_draw, leave, nullptr};

}  // namespace

extern "C" O3M_EXPORT const o3m_module o3m_entry __attribute__((section(".o3m_entry"))) = {
    SCOPE_ENTRY_HEAD, sizeof(State), SCOPE_ENTRY_PARAMS, init, nullptr, set_param, nullptr, nullptr, shutdown, SCOPE_ENTRY_VIEWS(&kUi),
};
