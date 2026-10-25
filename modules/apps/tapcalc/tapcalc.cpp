// SPDX-License-Identifier: MIT
//
// "Tap Calc" (OS-TAPCALC, new Community OS behaviour, ABI 1.3 until OS-APPUX S3, now 1.10: capabilities read_project and edit_project): tap a pad N times, read the BPM, read what that tempo means as note lengths (the
// delay time that echoes in time) and as LFO rates, and put it in the project. Everything is in the state block the OS lends; input() only records, view_draw() only draws; nothing is
// computed per audio block (an app has no render at all), so it costs the audio engine nothing.
//
//   Tap        any pad, or F1.  The BPM is the mean of the last AVERAGE intervals (4..8); a pause of more than 2 s starts a new series; 20.0..300.0, one decimal.
//   Set        F2 sets the project's tempo to the BPM shown (host->edit->set_tempo: one undoable unit; the project takes 40.0..800.0, so a tapped 20.0..39.9 is refused and says so).
//   Rows       the grammar of o3m_edit.h (OS-APPUX S3), the two rows at the bottom, kept with the project: Up / Down choose BPM or AVERAGE; the encoder moves BPM by 0.1 (Shift + the encoder by 1.0) or
//              AVERAGE by 1; Delete is the row's default: AVERAGE 8, BPM 120.0 and the taps forgotten. Tapping (a pad, F1) is unchanged; the taps are lost on exit, the BPM is kept.
//   Pages      Left / Right: note lengths in ms, or the same notes as LFO rates in Hz. 1/1..1/32, straight, dotted (x 1.5) and triplet (x 2/3).
//
// The clock is the host's now_ms() (ABI 1.3, io table), read in input() where the tap is: a tap is timed by the event that carries it, not by a frame count. The OS calls input() from the
// main loop, so a tap carries 16..33 ms of jitter at worst. The mean over N intervals is (last tap - first tap) / N, so only the two END taps count and they keep their jitter: the
// tempo is good to about +-33 ms over the span, i.e. +-2 BPM at 120 BPM over 4 intervals and about +-1 BPM over the default 8 (the jitter of the taps in between cancels). The project's tempo is
// read with host->read->get_tempo() for the PROJECT line. A host that lends no clock cannot run the app: init() says so.
#include <o3module.h>

#include "o3m_edit.h"
#include "tapcalc_gen.h"
#include "tapcalc_math.h"

namespace {

using namespace tapcalc;

constexpr uint32_t kMsgNone = 0, kMsgSet = 1, kMsgRange = 2, kMsgNoProject = 3, kMsgNoEdit = 4;
constexpr uint32_t kMsgMs = 2500;  // how long the answer to F2 stays on the page

struct State {
  const o3m_io_api* io;      // the clock (required)
  const o3m_read_api* read;  // the project's tempo (NULL: no PROJECT line)
  const o3m_edit_api* edit;  // set_tempo (NULL: F2 says it cannot)
  Tapper tapper;
  uint32_t last_tap;     // now_ms() at the last tap
  uint32_t bpm10;        // the tempo shown, x 10
  uint32_t window;       // AVERAGE
  uint32_t page;         // 0 ms, 1 Hz
  uint32_t row;          // the cursor's row: kRowBpm or kRowAverage
  uint32_t from_tap;     // 1 when bpm10 is what the taps say, 0 when it was set by hand
  uint32_t msg;          // the answer to the last F2 (kMsg*)
  uint32_t msg_until;    // now_ms() at which it goes
  uint32_t msg_bpm10;    // the tempo that F2 sent
};
static_assert(sizeof(State) <= TAPCALC_STATE_BYTES, "module.json: state_bytes is too small for State");

uint32_t now_ms(const State* s) { return s->io->now_ms(); }

int init(void* raw, const o3m_host* host) {
  State* s = static_cast<State*>(raw);
  if (host->draw == nullptr || !O3M_HOST_HAS(host, io) || host->io == nullptr || host->io->now_ms == nullptr) {
    return 1;  // an OS of ABI 1.0 lends no draw API, one below 1.3 no clock
  }
  s->io = host->io;
  s->read = O3M_HOST_HAS(host, read) && host->read != nullptr && host->read->get_tempo != nullptr ? host->read : nullptr;
  s->edit = O3M_HOST_HAS(host, edit) && host->edit != nullptr && host->edit->set_tempo != nullptr ? host->edit : nullptr;
  s->bpm10 = 1200;
  s->window = 8;  // the widest window: the most accurate (the end taps keep their jitter)
  return 0;
}

// The two declared rows, in the order the manifest has them (AVERAGE was the first: a project that kept it keeps it); the page shows BPM first.
enum Row : uint32_t { kRowBpm, kRowAverage, kRowCount };
constexpr uint32_t kBpmDefault = 1200;  // 120.0
constexpr uint32_t kAverageDefault = 8;

void set_param(void* raw, uint32_t id, float value) {
  State* s = static_cast<State*>(raw);
  if (id == TAPCALC_PARAM_BPM) {
    const float x = value >= static_cast<float>(kMinBpm10) ? (value <= static_cast<float>(kMaxBpm10) ? value : static_cast<float>(kMaxBpm10)) : static_cast<float>(kMinBpm10);
    s->bpm10 = static_cast<uint32_t>(x + 0.5f);  // (the BPM the project kept: shown as a manual one until a tap says otherwise)
  }
  if (id == TAPCALC_PARAM_AVERAGE) {
    const float x = value >= static_cast<float>(kMinIntervals) ? (value <= static_cast<float>(kMaxIntervals) ? value : static_cast<float>(kMaxIntervals)) : static_cast<float>(kMinIntervals);
    s->window = static_cast<uint32_t>(x + 0.5f);  // (a NaN falls to the lower bound)
    if (s->from_tap != 0u && s->tapper.intervals() != 0u) {
      s->bpm10 = s->tapper.bpm10(s->window);
    }
  }
}

// The BPM and AVERAGE come back before enter() (set_param: the project's kept values or the manifest's defaults); the taps and the answer to F2 are transient.
void enter(void* raw) {
  State* s = static_cast<State*>(raw);
  s->tapper.clear();
  s->from_tap = 0;
  s->msg = kMsgNone;
  s->row = kRowBpm;
}

void keep(const State* s, uint32_t row) {
  o3edit::own_set(s->io, row == kRowBpm ? TAPCALC_PARAM_BPM : TAPCALC_PARAM_AVERAGE, static_cast<float>(row == kRowBpm ? s->bpm10 : s->window));
}

void nudge(State* s, int32_t tenths) {
  int32_t v = static_cast<int32_t>(s->bpm10) + tenths;
  v = v < static_cast<int32_t>(kMinBpm10) ? static_cast<int32_t>(kMinBpm10) : (v > static_cast<int32_t>(kMaxBpm10) ? static_cast<int32_t>(kMaxBpm10) : v);
  s->bpm10 = static_cast<uint32_t>(v);
  s->from_tap = 0;
  s->tapper.clear();
}

// F2: the BPM shown becomes the project's tempo (BPM x 100). The host queues it and applies it as one undoable edit.
void apply(State* s) {
  s->msg_until = now_ms(s) + kMsgMs;
  s->msg_bpm10 = s->bpm10;
  if (s->edit == nullptr) {
    s->msg = kMsgNoEdit;
    return;
  }
  const int r = s->edit->set_tempo(s->bpm10 * 10u);
  s->msg = r == O3M_EDIT_OK ? kMsgSet : (r == O3M_EDIT_RANGE ? kMsgRange : kMsgNoProject);
}

constexpr int32_t kCoarse[kRowCount] = {10, 1};  // Shift + the encoder: BPM by 1.0 (ten steps of 0.1) a detent; AVERAGE has no coarse step

void input(void* raw, const o3m_ui_event* ev) {
  State* s = static_cast<State*>(raw);
  const o3edit::Input in = o3edit::grammar(ev, &s->row, kRowCount, &s->page, 2);
  if (in.move == o3edit::kMoveValue) {
    const int32_t d = o3edit::steps(in, kCoarse[s->row]);
    if (s->row == kRowBpm) {
      nudge(s, d);
    } else {
      s->window = static_cast<uint32_t>(o3edit::clamp_i(static_cast<int32_t>(s->window) + d, static_cast<int32_t>(kMinIntervals), static_cast<int32_t>(kMaxIntervals)));
    }
    keep(s, s->row);
    return;
  }
  if (in.move == o3edit::kMoveDefault) {
    if (s->row == kRowBpm) {
      s->bpm10 = kBpmDefault;  // the row's default, and the taps forgotten (what Delete did)
      s->tapper.clear();
      s->from_tap = 0;
    } else {
      s->window = kAverageDefault;
    }
    keep(s, s->row);
    return;
  }
  if (in.move != o3edit::kMoveNone) {
    return;  // a row or a page moved
  }
  if ((ev->kind == O3M_EV_PAD && ev->value > 0) || (ev->kind == O3M_EV_BUTTON && ev->value != 0 && ev->id == O3M_BTN_F1)) {
    const uint32_t now = now_ms(s);
    s->tapper.tap(now);
    s->last_tap = now;
    if (s->tapper.intervals() != 0u) {
      s->bpm10 = s->tapper.bpm10(s->window);
      s->from_tap = 1;
      keep(s, kRowBpm);  // (the tapped tempo is the BPM the project remembers)
    }
  } else if (ev->kind == O3M_EV_BUTTON && ev->value != 0 && ev->id == O3M_BTN_F2) {
    apply(s);
  }
}

uint32_t put_text(char* out, uint32_t at, const char* text) {
  while (*text != '\0') {
    out[at++] = *text++;
  }
  return at;
}

uint32_t put_uint(char* out, uint32_t at, uint32_t v) {
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

void view_draw(void* raw, uint32_t, o3m_canvas* c) {
  State* s = static_cast<State*>(raw);
  const o3m_draw_api* api = c->draw;
  const uint32_t now = now_ms(s);
  const int32_t w = c->width;
  const int32_t rows_y = c->height - 2 * o3edit::kRowPitch - 4;  // the two rows (BPM, AVERAGE) at the bottom, the project's tempo line above them
  const int32_t proj_y = rows_y - 20;
  const int32_t pitch = (proj_y - 4) / 8;  // eight rows: the tempo, the column heads, the six note values
  const bool live = s->from_tap != 0u && now - s->last_tap <= kResetMs;
  api->rect(c, 0, 0, w, c->height, O3M_COLOR_PANE);

  char text[48];
  uint32_t n = put_fixed(text, 0, s->bpm10, 1);
  n = put_text(text, n, " BPM");
  text[n] = '\0';
  api->text(c, 8, 2, text, pitch >= 36 ? O3M_FACE_DISPLAY : O3M_FACE_DATA, live ? O3M_COLOR_ACCENT : O3M_COLOR_TEXT);
  n = put_text(text, 0, "TAPS ");
  n = put_uint(text, n, s->tapper.n);
  n = put_text(text, n, s->from_tap != 0u ? "  TAP  " : "  MANUAL  ");
  n = put_uint(text, n, s->page + 1u);
  n = put_text(text, n, "/2");
  text[n] = '\0';
  api->text(c, w - 8 - api->text_width(c, text, O3M_FACE_DATA), 2 + pitch / 4, text, O3M_FACE_DATA, O3M_COLOR_TEXT_DIM);

  // the table: a label column and three value columns, each right-aligned to its edge
  static const char* const kLabels[kDivisions] = {"1/1", "1/2", "1/4", "1/8", "1/16", "1/32"};
  static const char* const kHeads[kVariants] = {"STRAIGHT", "DOTTED", "TRIPLET"};
  const int32_t label_w = api->text_width(c, "1/16 ", O3M_FACE_DATA);
  const int32_t col_w = (w - 16 - label_w) / 3;
  api->rect(c, 8, pitch + pitch - 2, w - 16, 1, O3M_COLOR_EMPTY);
  api->text(c, 8, pitch + 2, s->page == 0u ? "MS" : "HZ", O3M_FACE_DATA, O3M_COLOR_TEXT_DIM);
  for (uint32_t v = 0; v < kVariants; ++v) {
    const int32_t right = 8 + label_w + static_cast<int32_t>(v + 1u) * col_w;
    api->text(c, right - api->text_width(c, kHeads[v], O3M_FACE_DATA), pitch + 2, kHeads[v], O3M_FACE_DATA, O3M_COLOR_TEXT_DIM);
  }
  for (uint32_t d = 0; d < kDivisions; ++d) {
    const int32_t y = (static_cast<int32_t>(d) + 2) * pitch + 2;
    api->text(c, 8, y, kLabels[d], O3M_FACE_DATA, O3M_COLOR_INSTR);
    for (uint32_t v = 0; v < kVariants; ++v) {
      n = s->page == 0u ? put_fixed(text, 0, note_tenths(s->bpm10, d, v), 1) : put_fixed(text, 0, hz_hundredths(s->bpm10, d, v), 2);
      text[n] = '\0';
      api->text(c, 8 + label_w + static_cast<int32_t>(v + 1u) * col_w - api->text_width(c, text, O3M_FACE_DATA), y, text, O3M_FACE_DATA, v == 0u ? O3M_COLOR_TEXT : O3M_COLOR_FX);
    }
  }

  // the project's tempo, and the answer to F2 while it is news
  const uint32_t project = s->read != nullptr ? s->read->get_tempo() : 0u;
  if (project != 0u) {
    n = put_text(text, 0, "PROJECT ");
    n = put_fixed(text, n, project, 2);
    n = put_text(text, n, " BPM");
  } else {
    n = put_text(text, 0, s->read != nullptr ? "NO PROJECT" : "PROJECT TEMPO UNKNOWN");
  }
  text[n] = '\0';
  api->text(c, 8, proj_y, text, O3M_FACE_LABEL, O3M_COLOR_TEXT);
  if (s->msg != kMsgNone && static_cast<int32_t>(s->msg_until - now) > 0) {
    n = 0;
    switch (s->msg) {
      case kMsgSet:
        n = put_text(text, n, "SET ");
        n = put_fixed(text, n, s->msg_bpm10, 1);
        n = put_text(text, n, " BPM  (UNDO TO TAKE BACK)");
        break;
      case kMsgRange:
        n = put_text(text, n, "NOT SET: THE PROJECT TAKES 40 - 800 BPM");
        break;
      case kMsgNoProject:
        n = put_text(text, n, "NOT SET: NO PROJECT");
        break;
      default:
        n = put_text(text, n, "NOT SET: THIS APP CANNOT EDIT");
        break;
    }
    text[n] = '\0';
    api->text(c, w - 8 - api->text_width(c, text, O3M_FACE_LABEL), proj_y, text, O3M_FACE_LABEL, s->msg == kMsgSet ? O3M_COLOR_PLAY : O3M_COLOR_ACCENT);
  }
  o3edit::PageRow rows[kRowCount];
  rows[kRowBpm].name = "BPM";
  rows[kRowBpm].dim = false;
  n = put_fixed(rows[kRowBpm].value, 0, s->bpm10, 1);
  rows[kRowBpm].value[n] = '\0';
  rows[kRowAverage].name = "AVERAGE";
  rows[kRowAverage].dim = false;
  n = put_uint(rows[kRowAverage].value, 0, s->window);
  rows[kRowAverage].value[n] = '\0';
  o3edit::draw_rows(c, 0, rows_y, w / 2, rows, kRowCount, s->row);
}

void leave(void*) {}
void shutdown(void*) {}

// OS-KEYROWDLG S5 (ABI 1.9): what the keys do, for the page's key row (labels only: the OS delivers the keys to input() whatever they say; F8 is the OS's BACK).
const o3m_keys kKeys = {{"TAP", "SET"}, {"TAP", "SET"}};  // (Shift + Fn runs Fn: no app reads `mods`, so the Shift row says the same)

const o3m_keys* ui_keys(void*) { return &kKeys; }

const o3m_ui kUi = {O3M_UI_SIZE(TAPCALC_ABI_MINOR), enter, input, view_draw, leave, ui_keys};

}  // namespace

extern "C" O3M_EXPORT const o3m_module o3m_entry __attribute__((section(".o3m_entry"))) = {
    TAPCALC_ENTRY_HEAD, sizeof(State), TAPCALC_ENTRY_PARAMS, init, nullptr, set_param, nullptr, nullptr, shutdown, TAPCALC_ENTRY_VIEWS(&kUi),
};
