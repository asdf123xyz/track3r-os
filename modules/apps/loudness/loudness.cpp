// SPDX-License-Identifier: MIT
//
// "Loudness VU" (OS-H2a, new Community OS behaviour, ABI 1.4 until OS-APPUX S3, now 1.10, capability audio_tap): level meters for what the Tracker plays (MASTER) or what comes into the line input (INPUT). It reads the
// OS's audio tap through the io table's tap_select and tap_levels: the render measures the peak and the sum of squares of every block it writes, and this app only asks, once a frame, for what
// happened since its last question. It makes no sound and does no audio work: nothing here runs in the render.
//
//   PEAK   the loudest sample of the last frame, falling 24 dB a second, with a marker that holds the highest peak for HOLD seconds (parameter, 1..10)
//   RMS    the root mean square over about 300 ms (a one-pole average of the frames' mean squares: the classic VU time), in dBFS
//   CLIP   the lamp lights for 2 s when a block had a sample at full scale (|x| >= 32767); CLIPS counts the frames that had one since the last clear
//   HISTORY the RMS level every 250 ms for the last 24 s, as a line
//   F1 clears the hold, the maximum, the clip count and the history (it was Delete: Delete is the row's default now).   Up / Down, the encoder, Shift + the encoder, Delete (the grammar of o3m_edit.h,
//   OS-APPUX S3): the two rows at the bottom, kept with the project: SOURCE 1 = INPUT, 2 = MASTER; HOLD seconds.
//
// All figures are dBFS: 0.0 is a sample at full scale, 20 log10 of the linear level; the bars run from -60 to 0. The math comes from the host (log2_f, exp2_f), the module has no libc.
#include <o3module.h>

#include "loudness_gen.h"
#include "o3m_edit.h"

namespace {

constexpr float kFloorDb = -60.0f;    // the left edge of the bars and the history
constexpr float kMinDb = -96.0f;      // below this a level is silence (-INF)
constexpr float kPeakFallDb = 24.0f;  // per second
constexpr float kRmsTauMs = 300.0f;
constexpr uint32_t kClipMs = 2000;
constexpr uint32_t kHistory = 96;
constexpr uint32_t kHistoryStepMs = 250;

struct State {
  const o3m_host* host;  // the math
  const o3m_io_api* io;  // NULL: the host lent no io table
  uint32_t source;       // O3M_TAP_INPUT or O3M_TAP_MASTER, as the parameter says
  uint32_t selected;     // what the tap was last told (0: nothing yet)
  uint32_t hold_s;       // HOLD, seconds
  uint32_t row;          // the row of the two the cursor is on
  uint32_t started;      // 1 after the first poll
  uint32_t now;          // io->now_ms() at the last poll
  float peak_db;         // the falling peak
  float hold_db;
  uint32_t hold_at;
  float max_db;          // the highest peak since the last clear
  float ms;              // the smoothed mean square, linear amplitude squared
  float rms_db;
  uint32_t clip_at;
  uint32_t clip_seen;    // 1 once a clip was heard since the last clear
  uint32_t clips;        // frames with a clip since the last clear
  uint32_t hist_at;
  uint32_t hist_n;       // points held, 0..kHistory
  uint32_t hist_head;    // the next slot to write
  float hist[kHistory];
};
static_assert(sizeof(State) <= LOUDNESS_STATE_BYTES, "module.json: state_bytes is too small for State");

// A level from the host is a linear amplitude, 0..1 and a little over (a clip); anything else (NaN, a negative, an absurd one) counts as silence, so one bad answer cannot poison the averages.
float sane(float v) { return v >= 0.0f && v <= 8.0f ? v : 0.0f; }

float to_db(const State* s, float linear) {
  const float x = linear > 3.0e-6f ? linear : 3.0e-6f;  // -110 dBFS: the floor of the log
  const float db = 6.0205999f * s->host->log2_f(x);
  return db > kMinDb ? db : kMinDb;
}

void clear(State* s) {
  s->hold_db = kMinDb;
  s->max_db = kMinDb;
  s->clip_seen = 0;
  s->clips = 0;
  s->hist_n = 0;
  s->hist_head = 0;
}

int init(void* raw, const o3m_host* host) {
  State* s = static_cast<State*>(raw);
  s->host = host;
  s->io = O3M_HOST_HAS(host, io) ? host->io : nullptr;
  s->source = O3M_TAP_MASTER;
  s->hold_s = 3;
  s->peak_db = kMinDb;
  s->rms_db = kMinDb;
  clear(s);
  return host->draw != nullptr && host->log2_f != nullptr && host->exp2_f != nullptr ? 0 : 1;
}

uint32_t clamp_int(float v, float lo, float hi) {
  const float x = v >= lo ? (v <= hi ? v : hi) : lo;  // a NaN falls to the lower bound
  return static_cast<uint32_t>(x + 0.5f);
}

void set_param(void* raw, uint32_t id, float value) {
  State* s = static_cast<State*>(raw);
  if (id == LOUDNESS_PARAM_SOURCE) {
    s->source = clamp_int(value, 1.0f, 2.0f);
  } else if (id == LOUDNESS_PARAM_HOLD) {
    s->hold_s = clamp_int(value, 1.0f, 10.0f);
  }
}

void enter(void* raw) {
  State* s = static_cast<State*>(raw);
  s->row = 0;
  s->started = 0;
  s->selected = 0;
  s->peak_db = kMinDb;
  s->rms_db = kMinDb;
  s->ms = 0.0f;
  s->clip_at = 0;
  clear(s);
}

enum Row : uint32_t { kSource, kHold, kRowCount };
constexpr int32_t kCoarse[kRowCount] = {1, 5};  // Shift + the encoder: HOLD by five seconds a detent; SOURCE is two choices

void input(void* raw, const o3m_ui_event* ev) {
  State* s = static_cast<State*>(raw);
  const o3edit::Input in = o3edit::grammar(ev, &s->row, kRowCount, nullptr, 1);
  if (in.move == o3edit::kMoveValue || in.move == o3edit::kMoveDefault) {
    const int32_t d = in.move == o3edit::kMoveValue ? o3edit::steps(in, kCoarse[s->row]) : 0;
    const bool reset = in.move == o3edit::kMoveDefault;
    if (s->row == kSource) {
      s->source = reset ? O3M_TAP_MASTER : static_cast<uint32_t>(o3edit::clamp_i(static_cast<int32_t>(s->source) + d, 1, 2));
    } else {
      s->hold_s = reset ? 3u : static_cast<uint32_t>(o3edit::clamp_i(static_cast<int32_t>(s->hold_s) + d, 1, 10));
    }
    o3edit::own_set(s->io, s->row, static_cast<float>(s->row == kSource ? s->source : s->hold_s));  // kept with the project
    return;
  }
  if (ev->kind == O3M_EV_BUTTON && ev->value != 0 && ev->id == O3M_BTN_F1) {  // CLEAR
    clear(s);
  }
}

bool has_tap(const State* s) {
  return s->io != nullptr && s->io->now_ms != nullptr && s->io->size >= __builtin_offsetof(o3m_io_api, tap_levels) + sizeof(s->io->tap_levels) && s->io->tap_select != nullptr &&
         s->io->tap_levels != nullptr;
}

// Once a frame (view_draw): the clock, the tap's point, and what the render measured since the last frame.
void poll(State* s) {
  if (!has_tap(s)) {
    return;
  }
  const o3m_io_api* io = s->io;
  const uint32_t now = io->now_ms();
  uint32_t dt = s->started != 0u ? now - s->now : 0u;  // unsigned: the clock wraps
  dt = dt < 250u ? dt : 250u;                          // a long stall is one step, not a long fall
  s->now = now;
  if (s->started == 0u) {
    s->started = 1;
    s->hold_at = now;
    s->hist_at = now;
  }
  if (s->selected != s->source) {
    if (io->tap_select(s->source) == 0) {
      s->selected = s->source;
    }
  }
  o3m_tap_levels lv;
  lv.size = sizeof lv;
  lv.frames = 0;
  lv.lost_frames = 0;
  lv.clip_blocks = 0;
  lv.peak = 0.0f;
  lv.rms = 0.0f;
  io->tap_levels(&lv);
  float inst_db = kMinDb;
  float ms_new = 0.0f;
  if (lv.frames != 0u) {
    const float peak = sane(lv.peak);
    const float rms = sane(lv.rms);
    inst_db = to_db(s, peak);
    ms_new = rms * rms;
    if (lv.clip_blocks != 0u) {
      s->clip_at = now;
      s->clip_seen = 1;
      ++s->clips;
    }
  }
  // A frame with nothing measured (frames 0: no new block yet) is not silence: the figures stay as they are.
  if (lv.frames != 0u) {
    // the falling peak, the hold marker and the maximum
    const float fall = s->peak_db - kPeakFallDb * static_cast<float>(dt) * 0.001f;
    s->peak_db = inst_db > fall ? inst_db : fall;
    if (inst_db >= s->hold_db) {
      s->hold_db = inst_db;
      s->hold_at = now;
    } else if (now - s->hold_at >= s->hold_s * 1000u) {
      s->hold_db = s->peak_db;
      s->hold_at = now;
    }
    s->max_db = inst_db > s->max_db ? inst_db : s->max_db;
    // the RMS over about 300 ms: a one-pole average of the mean square, with the step's own weight
    const float alpha = 1.0f - s->host->exp2_f(-static_cast<float>(dt) * (1.442695f / kRmsTauMs));
    s->ms += alpha * (ms_new - s->ms);
    const float rms_db = 3.0102999f * s->host->log2_f(s->ms > 1.0e-12f ? s->ms : 1.0e-12f);  // 10 log10 of the mean square
    s->rms_db = rms_db > kMinDb ? rms_db : kMinDb;
  }
  if (now - s->hist_at >= kHistoryStepMs) {
    s->hist_at = now;
    s->hist[s->hist_head] = s->rms_db;
    s->hist_head = (s->hist_head + 1u) % kHistory;
    s->hist_n = s->hist_n < kHistory ? s->hist_n + 1u : kHistory;
  }
}

// ---- text, with no libc -----------------------------------------------------------------------------------------------------------------

uint32_t put_text(char* out, uint32_t at, const char* text) {
  while (*text != '\0') {
    out[at++] = *text++;
  }
  return at;
}

uint32_t put_num(char* out, uint32_t at, uint32_t v) {
  char digits[10];
  uint32_t n = 0;
  do {
    digits[n++] = static_cast<char>('0' + v % 10u);
    v /= 10u;
  } while (v != 0u && n < 10u);
  while (n != 0u) {
    out[at++] = digits[--n];
  }
  return at;
}

// A level as "-12.3", "  0.0" or "-INF", right-aligned in 6 columns.
uint32_t put_db(char* out, uint32_t at, float db) {
  char tmp[8];
  uint32_t n = 0;
  if (db <= -90.0f) {
    n = put_text(tmp, 0, "-INF");
  } else {
    const bool neg = db < 0.0f;
    const uint32_t tenths = static_cast<uint32_t>((neg ? -db : db) * 10.0f + 0.5f);
    if (neg && tenths != 0u) {
      tmp[n++] = '-';
    }
    n = put_num(tmp, n, tenths / 10u);
    tmp[n++] = '.';
    tmp[n++] = static_cast<char>('0' + tenths % 10u);
  }
  for (uint32_t i = n; i < 6u; ++i) {
    out[at++] = ' ';
  }
  for (uint32_t i = 0; i < n; ++i) {
    out[at++] = tmp[i];
  }
  return at;
}

// ---- drawing ------------------------------------------------------------------------------------------------------------------------------

// Where a level falls on a scale of `width` pixels: 0 at kFloorDb, width at 0 dBFS.
int32_t scale(float db, int32_t width) {
  const float f = (db - kFloorDb) * (1.0f / -kFloorDb);
  const float c = f > 0.0f ? (f < 1.0f ? f : 1.0f) : 0.0f;
  return static_cast<int32_t>(c * static_cast<float>(width));
}

void bar(o3m_canvas* c, int32_t x, int32_t y, int32_t w, int32_t h, float db) {
  const o3m_draw_api* api = c->draw;
  api->rect(c, x, y, w, h, O3M_COLOR_EMPTY);
  const int32_t full = scale(db, w);
  const int32_t warn = scale(-6.0f, w);  // the top 6 dB are orange
  if (full > 0) {
    api->rect(c, x, y, full < warn ? full : warn, h, O3M_COLOR_PLAY);
  }
  if (full > warn) {
    api->rect(c, x + warn, y, full - warn, h, O3M_COLOR_ACCENT);
  }
}

void view_draw(void* raw, uint32_t, o3m_canvas* c) {
  State* s = static_cast<State*>(raw);
  poll(s);
  const o3m_draw_api* api = c->draw;
  const int32_t w = c->width;
  const int32_t h = c->height;
  api->rect(c, 0, 0, w, h, O3M_COLOR_PANE);
  api->text(c, 8, 4, "LOUDNESS VU", O3M_FACE_DISPLAY, O3M_COLOR_TEXT);
  char text[64];
  uint32_t n = 0;
  // the two rows at the bottom (the grammar of o3m_edit.h): SOURCE and HOLD
  o3edit::PageRow rows[kRowCount];
  rows[kSource].name = "SOURCE";
  rows[kSource].dim = false;
  n = put_text(rows[kSource].value, 0, s->source == O3M_TAP_INPUT ? "INPUT" : "MASTER");
  rows[kSource].value[n] = '\0';
  rows[kHold].name = "HOLD";
  rows[kHold].dim = false;
  n = put_num(rows[kHold].value, 0, s->hold_s);
  n = put_text(rows[kHold].value, n, " S");
  rows[kHold].value[n] = '\0';
  o3edit::draw_rows(c, 0, h - 2 * o3edit::kRowPitch - 4, w / 2, rows, kRowCount, s->row);
  if (!has_tap(s)) {
    api->text(c, 8, 60, "NO AUDIO TAP FOR THIS APP", O3M_FACE_DATA, O3M_COLOR_ACCENT);
    return;
  }

  const int32_t x0 = 72;
  const int32_t rx = w - 8 - 60;  // the readouts, 6 columns of the Data face
  const int32_t x1 = rx - 12;
  const int32_t bw = x1 - x0;
  const int32_t bh = 24;
  api->text(c, 8, 48, "PEAK", O3M_FACE_LABEL, O3M_COLOR_TEXT_DIM);
  bar(c, x0, 44, bw, bh, s->peak_db);
  const int32_t hm = x0 + scale(s->hold_db, bw);
  api->rect(c, hm - 2 > x0 ? hm - 2 : x0, 44, 3, bh, O3M_COLOR_TEXT);
  put_db(text, 0, s->peak_db);
  text[6] = '\0';
  api->text(c, rx, 46, text, O3M_FACE_DATA, O3M_COLOR_TEXT);
  api->text(c, 8, 84, "RMS", O3M_FACE_LABEL, O3M_COLOR_TEXT_DIM);
  bar(c, x0, 80, bw, bh, s->rms_db);
  put_db(text, 0, s->rms_db);
  text[6] = '\0';
  api->text(c, rx, 82, text, O3M_FACE_DATA, O3M_COLOR_TEXT);

  // the scale under the bars
  static const int16_t kTicks[7] = {-60, -48, -36, -24, -12, -6, 0};
  for (uint32_t i = 0; i < 7u; ++i) {
    const int32_t tx = x0 + scale(static_cast<float>(kTicks[i]), bw);
    api->rect(c, tx, 106, 1, 5, O3M_COLOR_TEXT_DIM);
    n = 0;
    if (kTicks[i] < 0) {
      text[n++] = '-';
      n = put_num(text, n, static_cast<uint32_t>(-kTicks[i]));
    } else {
      n = put_num(text, n, 0u);
    }
    text[n] = '\0';
    const int32_t tw = api->text_width(c, text, O3M_FACE_MICRO);
    int32_t lx = tx - tw / 2;
    lx = lx + tw > x1 + 20 ? x1 + 20 - tw : lx;
    api->text(c, lx < 0 ? 0 : lx, 112, text, O3M_FACE_MICRO, O3M_COLOR_TEXT_DIM);
  }

  // hold, maximum, clips
  n = put_text(text, 0, "HOLD");
  n = put_db(text, n, s->hold_db);
  n = put_text(text, n, "   MAX");
  n = put_db(text, n, s->max_db);
  text[n] = '\0';
  api->text(c, 8, 134, text, O3M_FACE_DATA, O3M_COLOR_TEXT);
  const bool lit = s->clip_at != 0u && s->now - s->clip_at < kClipMs;
  n = put_text(text, 0, "CLIPS ");
  n = put_num(text, n, s->clips);
  text[n] = '\0';
  const int32_t cw = api->text_width(c, text, O3M_FACE_DATA);
  api->text(c, w - 8 - cw, 134, text, O3M_FACE_DATA, s->clip_seen != 0u ? O3M_COLOR_ACCENT : O3M_COLOR_TEXT_DIM);
  api->rect(c, w - 8 - cw - 28, 136, 18, 18, lit ? O3M_COLOR_ACCENT : O3M_COLOR_EMPTY);

  // the history of the RMS level, newest at the right
  const int32_t hy = 170;
  const int32_t hh = h - hy - 2 * o3edit::kRowPitch - 12;
  if (hh >= 40) {
    api->text(c, 8, hy - 14, "RMS HISTORY 24 S", O3M_FACE_MICRO, O3M_COLOR_TEXT_DIM);
    const int32_t aw = w - 16;
    api->rect(c, 8, hy, aw, hh, O3M_COLOR_EMPTY);
    const int32_t step = (aw - 1) / static_cast<int32_t>(kHistory - 1u);
    if (s->hist_n >= 2u) {
      int16_t xy[2u * kHistory];
      for (uint32_t i = 0; i < s->hist_n; ++i) {
        const float db = s->hist[(s->hist_head + kHistory - s->hist_n + i) % kHistory];
        xy[2u * i] = static_cast<int16_t>(8 + aw - 1 - static_cast<int32_t>(s->hist_n - 1u - i) * step);
        xy[2u * i + 1u] = static_cast<int16_t>(hy + hh - 1 - scale(db, hh - 2));
      }
      api->polyline(c, xy, s->hist_n, O3M_COLOR_PLAY);
    }
  }
}

void leave(void*) {}
void shutdown(void*) {}

// OS-KEYROWDLG S5 (ABI 1.9): what the keys do, for the page's key row (labels only: the OS delivers the keys to input() whatever they say; F8 is the OS's BACK).
const o3m_keys kKeys = {{"CLEAR"}, {"CLEAR"}};  // (Shift + Fn runs Fn: no app reads `mods`, so the Shift row says the same)

const o3m_keys* ui_keys(void*) { return &kKeys; }

const o3m_ui kUi = {O3M_UI_SIZE(LOUDNESS_ABI_MINOR), enter, input, view_draw, leave, ui_keys};

}  // namespace

extern "C" O3M_EXPORT const o3m_module o3m_entry __attribute__((section(".o3m_entry"))) = {
    LOUDNESS_ENTRY_HEAD, sizeof(State), LOUDNESS_ENTRY_PARAMS, init, nullptr, set_param, nullptr, nullptr, shutdown, LOUDNESS_ENTRY_VIEWS(&kUi),
};
