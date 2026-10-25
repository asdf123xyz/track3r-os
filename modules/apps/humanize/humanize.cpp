// SPDX-License-Identifier: MIT
//
// "Humanize" (OS-H3B, new Community OS behaviour, ABI 1.5 until OS-APPUX S3, now 1.10: capabilities read_project and edit_project): microtiming and volume jitter on the Pattern's block. Its three rows are declared
// (TIME, VOL, SEED) and kept with the project (set_own_param).
//
//   TARGET   the block (every track of it, its rows), or with no block the cursor's track for 16 rows from the cursor row. Only steps with a note are touched. The app starts there and the five target rows
//            (PATTERN, TRACK, ROW, ROWS, TRACKS; o3m_edit.h, OS-APPUX S5) move it inside the app; they are not kept (a relaunch from TOOLS starts at the cursor again), Delete puts the launch value back.
//   TIME     0..100: the largest delay, written as FX 2 Micro-move (it delays only, as the FX does). SEED 0..9999. VOL 0..64: the volume of a step that has one moves by up to that much either way; an unset volume is left unset (it plays at the instrument's volume).
//   FX SLOT  a step that already has Micro-move gets that slot's value replaced; otherwise the delay goes in an empty slot, FX2 first. A step with both slots taken keeps its timing and is counted.
//   KEYS     the grammar of o3m_edit.h (Up / Down the row, the encoder the value, Shift + the encoder in coarse steps, Delete the row's default). F1 APPLY writes the target as ONE edit group (one undo unit); F2 VARY is SEED + 1, then APPLY. F8 leaves.
//            TIME 0 and VOL 0 change nothing, so APPLY then does nothing at all (no group, no undo unit).
//
// Same SEED, same steps (humanize_math.h). The rows are the app's own drawing (no OS column), kept with the project: the OS gives them back through set_param before enter(). No clock, no audio work, no heap. The module has no libc: every loop is written out.
#include <o3module.h>

#include "humanize_gen.h"
#include "humanize_math.h"
#include "o3m_edit.h"

namespace {

using namespace o3edit;
using namespace humanize;

enum Row : uint32_t { kTime, kVol, kSeed, kDeclared, kRows = kDeclared + kTargetRows };  // (the target rows follow the three declared ones)
constexpr uint32_t kDefaultTime = 20;
constexpr uint32_t kDefaultVol = 8;

struct State {
  const o3m_read_api* read;
  const o3m_edit_api* edit;
  const o3m_io_api* io;  // set_own_param: the three rows are kept with the project (OS-APPUX S3)
  uint32_t have_edit;  // the edit table can write steps
  Target tgt;
  uint32_t time, vol, seed;
  uint32_t row;
  uint32_t msg;       // kMsg*: the answer to the last APPLY, kept until the next key
  int32_t msg_code;   // the edit table's code behind it
  uint32_t msg_steps;  // steps humanized
  uint32_t msg_skipped;
};
static_assert(sizeof(State) <= HUMANIZE_STATE_BYTES, "module.json: state_bytes is too small for State");

constexpr uint32_t kMsgNone = 0, kMsgApplied = 1, kMsgFailed = 2, kMsgNoPattern = 3, kMsgNoContext = 4, kMsgNoEdit = 5, kMsgNothing = 6, kMsgNoNotes = 7;

int init(void* raw, const o3m_host* host) {
  State* s = static_cast<State*>(raw);
  if (host->draw == nullptr || !O3M_HOST_HAS(host, read) || host->read == nullptr || !O3M_TABLE_HAS(host->read, get_steps) || host->read->get_steps == nullptr || host->read->pattern_info == nullptr ||
      host->read->launch_ctx == nullptr) {
    return 1;  // an OS below 1.5, or an app without read_project: nothing to humanize
  }
  s->read = host->read;
  s->io = O3M_HOST_HAS(host, io) ? host->io : nullptr;
  s->edit = O3M_HOST_HAS(host, edit) && host->edit != nullptr && O3M_TABLE_HAS(host->edit, set_steps) && host->edit->edit_begin != nullptr && host->edit->edit_end != nullptr &&
                    host->edit->set_steps != nullptr
                ? host->edit
                : nullptr;
  s->have_edit = s->edit != nullptr ? 1u : 0u;
  return 0;
}

// The OS hands the three declared rows back before enter() (set_param, the project's remembered values or the manifest's defaults): enter() leaves them alone.
void set_param(void* raw, uint32_t index, float v) {
  State* s = static_cast<State*>(raw);
  const uint32_t n = v > 0.0f ? static_cast<uint32_t>(v + 0.5f) : 0u;
  switch (index) {
    case kTime: s->time = n < kMaxTime ? n : kMaxTime; break;
    case kVol: s->vol = n < kMaxVol ? n : kMaxVol; break;
    case kSeed: s->seed = n < kMaxSeed ? n : kMaxSeed; break;
    default: break;
  }
}

uint32_t row_value(const State* s, uint32_t row) { return row == kTime ? s->time : (row == kVol ? s->vol : s->seed); }

// A value the user changed is kept with the project (the target rows are not: a relaunch starts at the cursor).
void keep(State* s, uint32_t row) {
  if (row < kDeclared) {
    own_set(s->io, row, static_cast<float>(row_value(s, row)));
  }
}

void enter(void* raw) {
  State* s = static_cast<State*>(raw);
  s->row = 0;
  s->msg = kMsgNone;
  s->msg_code = 0;
  s->msg_steps = s->msg_skipped = 0;
  if (!read_target(s->read, &s->tgt, kNoBlockRows)) {
    s->msg = s->tgt.from_tools == 0u ? kMsgNoContext : kMsgNoPattern;
  }
}

// ---- APPLY -------------------------------------------------------------------------------------------------------------------------------------

void apply(State* s) {
  if (s->tgt.from_tools == 0u) {
    s->msg = kMsgNoContext;
    return;
  }
  if (s->tgt.have_pattern == 0u) {
    s->msg = kMsgNoPattern;
    return;
  }
  if (s->have_edit == 0u) {
    s->msg = kMsgNoEdit;
    return;
  }
  if (s->time == 0u && s->vol == 0u) {
    s->msg = kMsgNothing;  // TIME 0 and VOL 0 change nothing: no group, so no undo unit
    return;
  }
  o3m_step cur[O3M_PATTERN_ROWS_MAX];
  o3m_step out[O3M_PATTERN_ROWS_MAX];
  const int32_t b = s->edit->edit_begin();
  if (b != O3M_EDIT_OK) {
    s->msg = kMsgFailed;
    s->msg_code = b;
    return;
  }
  uint32_t touched = 0, skipped = 0;
  for (uint32_t l = 0; l < s->tgt.tracks; ++l) {
    const uint32_t n = track_rows(&s->tgt, l);
    const uint8_t track = static_cast<uint8_t>(s->tgt.first_track + l);
    if (n == 0u || s->read->get_steps(static_cast<uint16_t>(s->tgt.pattern), track, static_cast<uint8_t>(s->tgt.first_row), cur, static_cast<uint8_t>(n)) != O3M_EDIT_OK) {
      continue;
    }
    Setup u;
    u.time = s->time;
    u.vol = s->vol;
    u.seed = s->seed;
    u.stream = l;
    const Result r = humanize_steps<o3m_step, Draw>(u, cur, out, n);
    touched += r.touched;
    skipped += r.skipped;
    if (r.touched != 0u) {
      (void)s->edit->set_steps(static_cast<uint16_t>(s->tgt.pattern), track, static_cast<uint8_t>(s->tgt.first_row), out, static_cast<uint8_t>(n), O3M_STEP_VOL | O3M_STEP_FX1 | O3M_STEP_FX2);
    }
  }
  const int32_t e = s->edit->edit_end();
  s->msg_code = e;
  s->msg_steps = touched;
  s->msg_skipped = skipped;
  s->msg = e != O3M_EDIT_OK ? kMsgFailed : (touched != 0u ? kMsgApplied : kMsgNoNotes);
}

// ---- editing -----------------------------------------------------------------------------------------------------------------------------------

void edit_value(State* s, int32_t d, bool reset) {
  if (s->row >= kDeclared) {
    edit_target(s->read, &s->tgt, s->row - kDeclared, d, reset);
    return;
  }
  switch (s->row) {
    case kTime:
      s->time = reset ? kDefaultTime : static_cast<uint32_t>(clamp_i(static_cast<int32_t>(s->time) + d, 0, static_cast<int32_t>(kMaxTime)));
      break;
    case kVol:
      s->vol = reset ? kDefaultVol : static_cast<uint32_t>(clamp_i(static_cast<int32_t>(s->vol) + d, 0, static_cast<int32_t>(kMaxVol)));
      break;
    default:
      s->seed = reset ? 0u : static_cast<uint32_t>(clamp_i(static_cast<int32_t>(s->seed) + d, 0, static_cast<int32_t>(kMaxSeed)));
      break;
  }
}

// The coarse step of each row under Shift + the encoder (OS-APPUX S2): TIME and VOL by tens and eights, SEED by hundreds, the target rows as o3m_edit.h says.
int32_t coarse_of(uint32_t row) { return row == kTime ? 10 : (row == kVol ? 8 : (row == kSeed ? 100 : target_coarse(row - kDeclared))); }

void input(void* raw, const o3m_ui_event* ev) {
  State* s = static_cast<State*>(raw);
  const Input in = grammar(ev, &s->row, kRows, nullptr, 1);
  if (in.move == kMoveValue) {
    edit_value(s, steps(in, coarse_of(s->row)), false);
    keep(s, s->row);
    s->msg = kMsgNone;
    return;
  }
  if (ev->kind != O3M_EV_BUTTON || ev->value == 0) {
    return;
  }
  s->msg = kMsgNone;
  if (in.move == kMoveDefault) {
    edit_value(s, 0, true);
    keep(s, s->row);
    return;
  }
  switch (ev->id) {
    case O3M_BTN_F1:
      apply(s);
      break;
    case O3M_BTN_F2:
      s->seed = s->seed < kMaxSeed ? s->seed + 1u : 0u;
      keep(s, kSeed);
      apply(s);
      break;
    default:
      break;
  }
}

// ---- the page ----------------------------------------------------------------------------------------------------------------------------------

void view_draw(void* raw, uint32_t, o3m_canvas* c) {
  State* s = static_cast<State*>(raw);
  char facts[64];
  facts[0] = '\0';
  if (s->tgt.from_tools != 0u && s->tgt.have_pattern != 0u) {
    uint32_t n = put_text(facts, 0, s->tgt.has_block != 0u ? "BLOCK  TRACK " : "AT THE CURSOR  TRACK ");  // (the launch's word; the track and rows are what exists of the target now)
    n = put_uint(facts, n, s->tgt.first_track + 1u, 1);
    if (s->tgt.tracks > 1u) {
      n = put_text(facts, n, "-");
      n = put_uint(facts, n, s->tgt.first_track + s->tgt.tracks, 1);
    }
    n = put_text(facts, n, "  ROWS ");
    const uint32_t cnt = track_rows(&s->tgt, 0);
    n = put_uint(facts, n, s->tgt.first_row + 1u, 1);  // (the Pattern counts steps from 1)
    n = put_text(facts, n, "-");
    n = put_uint(facts, n, s->tgt.first_row + (cnt != 0u ? cnt : 1u), 1);
    facts[n] = '\0';
  }
  PageRow rows[kRows];
  static const char* const kNames[kDeclared] = {"TIME", "VOL", "SEED"};
  for (uint32_t r = kDeclared; r < kRows; ++r) {
    fill_target_row(&rows[r], &s->tgt, r - kDeclared);
  }
  for (uint32_t r = 0; r < kDeclared; ++r) {
    rows[r].name = kNames[r];
    rows[r].dim = false;
    uint32_t n = 0;
    switch (r) {
      case kTime: n = put_uint(rows[r].value, 0, s->time, 1); break;
      case kVol:
        n = put_text(rows[r].value, 0, "+/- ");
        n = put_uint(rows[r].value, n, s->vol, 1);
        break;
      default: n = put_uint(rows[r].value, 0, s->seed, 4); break;
    }
    rows[r].value[n] = '\0';
  }
  char answer[64], extra[48];
  uint32_t n = 0, x = 0;
  uint32_t color = O3M_COLOR_ACCENT;
  switch (s->msg) {
    case kMsgApplied:
      n = put_uint(answer, n, s->msg_steps, 1);
      n = put_text(answer, n, s->msg_steps == 1u ? " STEP HUMANIZED  (UNDO TO TAKE BACK)" : " STEPS HUMANIZED  (UNDO TO TAKE BACK)");
      color = O3M_COLOR_PLAY;
      if (s->msg_skipped != 0u) {
        x = put_uint(extra, x, s->msg_skipped, 1);
        x = put_text(extra, x, s->msg_skipped == 1u ? " STEP: NO FREE FX SLOT" : " STEPS: NO FREE FX SLOT");
      }
      break;
    case kMsgNoNotes: n = put_text(answer, n, "NO STEP WITH A NOTE IN THE TARGET"); break;
    case kMsgNothing: n = put_text(answer, n, "TIME AND VOL ARE 0: NOTHING TO DO"); break;
    case kMsgFailed:
      n = put_text(answer, n, "NOT APPLIED  (CODE ");
      n = put_uint(answer, n, static_cast<uint32_t>(s->msg_code), 1);
      n = put_text(answer, n, ")");
      break;
    case kMsgNoPattern: n = put_text(answer, n, "THE PATTERN DOES NOT EXIST YET: ENTER A NOTE FIRST"); break;
    case kMsgNoContext: n = put_text(answer, n, "OPEN IT FROM PATTERN F7 TOOLS"); break;
    case kMsgNoEdit: n = put_text(answer, n, "THIS HOST CANNOT EDIT"); break;
    default: break;
  }
  answer[n] = '\0';
  extra[x] = '\0';
  draw_page(c, "HUMANIZE", facts, rows, kRows, s->row, extra, answer, color);
}

void leave(void*) {}
void shutdown(void*) {}

// OS-KEYROWDLG S5 (ABI 1.9): what the keys do, for the page's key row (labels only: the OS delivers the keys to input() whatever they say; F8 is the OS's BACK).
const o3m_keys kKeys = {{"APPLY", "VARY"}, {"APPLY", "VARY"}};  // (Shift + Fn runs Fn: no app reads `mods`, so the Shift row says the same)

const o3m_keys* ui_keys(void*) { return &kKeys; }

const o3m_ui kUi = {O3M_UI_SIZE(HUMANIZE_ABI_MINOR), enter, input, view_draw, leave, ui_keys};

}  // namespace

extern "C" O3M_EXPORT const o3m_module o3m_entry __attribute__((section(".o3m_entry"))) = {
    HUMANIZE_ENTRY_HEAD, sizeof(State), HUMANIZE_ENTRY_PARAMS, init, nullptr, set_param, nullptr, nullptr, shutdown, HUMANIZE_ENTRY_VIEWS(&kUi),
};
