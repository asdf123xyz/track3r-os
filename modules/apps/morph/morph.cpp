// SPDX-License-Identifier: MIT
//
// "Morph" (OS-H3B, new Community OS behaviour, ABI 1.6: capabilities read_project and edit_project): a blend of the current instrument (A) towards another (B). It uses the instrument-parameter entries of ABI 1.6.
//
//   TARGET   the instrument the app was launched with: a SAMPLE or a MODULE instrument. When the page opens the app copies A's writable parameters into its own state, A0. The INSTRUMENT A row (OS-APPUX S5) moves
//            A inside the app (1..64; Delete puts the launch instrument back) and copies A0 again; it is not kept, a relaunch from TOOLS starts at the Pattern's instrument again.
//   VALUES   SOURCE = B, only the instruments of the same kind AND the same engine (a sample's play engine, or the same module) are listed; none gives NO MATCHING INSTRUMENT. POS 0..100 %.
//   THE RULE for every parameter the OS marks writable: A = A0 + (B - A0) x POS for a continuous one; a STEPPED or ENUM parameter takes B's value from POS 50. POS 0 is A0 and POS 100 is B, exactly.
//   KEYS     the grammar of o3m_edit.h (Up / Down the row, the encoder the value, Shift + the encoder in coarse steps, Delete the row's default). F1 APPLY writes A as ONE edit group (one undo unit, UNDO restores every value exactly). F3 RESET is APPLY at POS 0: A goes back to A0. F8 leaves.
//            There is no live sweep: every apply is an undo unit, and sweeping would flood the history.
//
// No OS parameters, no clock, no audio work, no heap. The module has no libc: every loop is written out.
#include <o3module.h>

#include "morph_gen.h"
#include "morph_math.h"
#include "o3m_edit.h"

namespace {

using namespace o3edit;
using namespace morph;

enum Row : uint32_t { kSource, kPos, kInstrument, kRows };  // (the rows before INSTRUMENT are declared and kept)

struct State {
  const o3m_read_api* read;
  const o3m_edit_api* edit;
  const o3m_io_api* io;  // set_own_param: SOURCE and POS are kept with the project (OS-APPUX S3)
  uint32_t src;          // SOURCE as kept: the instrument 1..64 the user chose, 0 = the first candidate (the default)
  uint32_t ok;          // the OS can read and write instrument parameters
  uint32_t a;           // A: 1..64
  uint32_t home_a;      // the launch instrument (what Delete puts back on INSTRUMENT A); 0 = there is no launch context
  uint32_t kind, engine;
  uint32_t count;       // writable parameters (A0 holds that many)
  uint32_t cands;       // instruments that match
  uint32_t sel;         // the SOURCE, an index into cand[]
  uint32_t pos;
  uint32_t row;
  uint32_t msg;
  int32_t msg_code;
  uint32_t msg_changed;
  uint8_t cand[O3M_INSTR_MAX];
  PD pd[kMaxPD];
  float a0[kMaxPD];
};
static_assert(sizeof(State) <= MORPH_STATE_BYTES, "module.json: state_bytes is too small for State");

constexpr uint32_t kMsgNone = 0, kMsgApplied = 1, kMsgFailed = 2, kMsgNoContext = 3, kMsgNoEdit = 4, kMsgUnchanged = 5, kMsgKind = 6, kMsgNoMatch = 7, kMsgReset = 8;

int init(void* raw, const o3m_host* host) {
  State* s = static_cast<State*>(raw);
  if (host->draw == nullptr || !O3M_HOST_HAS(host, read) || host->read == nullptr || !have_params(host->read) || host->read->launch_ctx == nullptr) {
    return 1;  // an OS below 1.6, or an app without read_project: no instrument parameters to read
  }
  s->read = host->read;
  s->io = O3M_HOST_HAS(host, io) ? host->io : nullptr;
  s->edit = O3M_HOST_HAS(host, edit) && host->edit != nullptr && O3M_TABLE_HAS(host->edit, set_instrument_param) && host->edit->edit_begin != nullptr && host->edit->edit_end != nullptr &&
                    host->edit->set_instrument_param != nullptr
                ? host->edit
                : nullptr;
  s->ok = s->edit != nullptr ? 1u : 0u;
  return 0;
}

// The rows are the OS's to hand back (set_param before enter()) and the project's to keep (set_own_param). SOURCE is kept as the instrument number, which stays true when the instruments move; the
// index into the candidates is found again in enter().
void set_param(void* raw, uint32_t index, float v) {
  State* s = static_cast<State*>(raw);
  const uint32_t n = v > 0.0f ? static_cast<uint32_t>(v + 0.5f) : 0u;
  if (index == kSource) {
    s->src = n < O3M_INSTR_MAX ? n : O3M_INSTR_MAX;
  } else if (index == kPos) {
    s->pos = n < kMaxPos ? n : kMaxPos;
  }
}

void keep_source(State* s) { own_set(s->io, kSource, static_cast<float>(s->cands != 0u && s->sel != 0u && s->sel < s->cands ? s->cand[s->sel] : 0u)); }  // (the first candidate is the default: 0)

// A's kind and writable parameters (A0), and the candidates for B (the launch, or an INSTRUMENT row the user moved).
void retarget(State* s) {
  s->sel = 0;
  s->msg = kMsgNone;
  s->msg_code = 0;
  s->msg_changed = 0;
  s->count = s->cands = 0;
  s->kind = O3M_INSTRUMENT_EMPTY;
  s->count = load_writable(s->read, s->a, s->pd, kMaxPD, &s->kind, &s->engine);
  if (s->kind != O3M_INSTRUMENT_SAMPLE && s->kind != O3M_INSTRUMENT_MODULE) {
    s->msg = kMsgKind;
    return;
  }
  for (uint32_t i = 0; i < s->count; ++i) {  // A0: A's writable parameters as they are now
    s->a0[i] = 0.0f;
    if (s->read->get_instrument_param(static_cast<uint8_t>(s->a), s->pd[i].id, &s->a0[i]) != O3M_EDIT_OK) {
      s->a0[i] = s->pd[i].min;
    }
  }
  for (uint32_t n = 1; n <= O3M_INSTR_MAX; ++n) {  // B: the other instruments of the same kind and engine
    o3m_instrument_info info;
    info.size = sizeof info;
    if (n != s->a && s->read->instrument_info(static_cast<uint8_t>(n), &info) == O3M_EDIT_OK && info.kind == s->kind && info.engine == s->engine && info.param_count != 0u) {
      s->cand[s->cands++] = static_cast<uint8_t>(n);
    }
  }
  for (uint32_t i = 0; i < s->cands; ++i) {  // the SOURCE the project kept, when it is still a candidate
    if (s->src != 0u && s->cand[i] == s->src) {
      s->sel = i;
    }
  }
  if (s->cands == 0u) {
    s->msg = kMsgNoMatch;
  }
}

void enter(void* raw) {
  State* s = static_cast<State*>(raw);
  s->sel = 0;
  s->row = 0;
  s->msg = kMsgNone;
  s->msg_code = 0;
  s->msg_changed = 0;
  s->count = s->cands = 0;
  s->kind = O3M_INSTRUMENT_EMPTY;
  s->home_a = 0;
  o3m_launch_ctx lc;
  lc.size = sizeof lc;
  lc.flags = lc.pattern = lc.track = lc.instrument = lc.block_start = lc.block_length = 0;
  lc.block_tracks = 0;
  lc.reserved[0] = lc.reserved[1] = lc.reserved[2] = 0;
  if (s->read->launch_ctx(&lc) == 0u) {
    s->msg = kMsgNoContext;
    return;
  }
  s->a = s->home_a = lc.instrument + 1u <= O3M_INSTR_MAX ? lc.instrument + 1u : O3M_INSTR_MAX;
  retarget(s);
}

// ---- APPLY -------------------------------------------------------------------------------------------------------------------------------------

void apply(State* s, uint32_t pos, bool is_reset) {
  if (s->msg == kMsgNoContext || s->msg == kMsgKind || s->msg == kMsgNoMatch) {
    return;  // (the answer stays: there is nothing to blend)
  }
  if (s->ok == 0u) {
    s->msg = kMsgNoEdit;
    return;
  }
  const uint32_t b = s->cand[s->sel < s->cands ? s->sel : 0u];
  const int32_t begin = s->edit->edit_begin();
  if (begin != O3M_EDIT_OK) {
    s->msg = kMsgFailed;
    s->msg_code = begin;
    return;
  }
  uint32_t changed = 0;
  for (uint32_t i = 0; i < s->count; ++i) {
    const PD& p = s->pd[i];
    float bv = 0.0f, cur = 0.0f;
    if (s->read->get_instrument_param(static_cast<uint8_t>(b), p.id, &bv) != O3M_EDIT_OK || s->read->get_instrument_param(static_cast<uint8_t>(s->a), p.id, &cur) != O3M_EDIT_OK) {
      continue;
    }
    const float v = morph_value(s->a0[i], bv, p.min, p.max, p.whole != 0u, pos);
    if (v != cur && s->edit->set_instrument_param(static_cast<uint8_t>(s->a), p.id, v) == O3M_EDIT_OK) {
      ++changed;
    }
  }
  const int32_t e = s->edit->edit_end();
  s->msg_code = e;
  s->msg_changed = changed;
  s->msg = e != O3M_EDIT_OK ? kMsgFailed : (changed != 0u ? (is_reset ? kMsgReset : kMsgApplied) : kMsgUnchanged);
}

// ---- editing -----------------------------------------------------------------------------------------------------------------------------------

bool sticky(const State* s) { return s->msg == kMsgNoContext || s->msg == kMsgKind || s->msg == kMsgNoMatch; }

void edit_value(State* s, int32_t d, bool reset) {
  if (s->row == kInstrument) {
    if (s->home_a != 0u) {  // (nothing to move with no launch context)
      const uint32_t was = s->a;
      s->a = step_instrument(s->a, s->home_a, d, reset);
      if (s->a != was) {  // (A0 is A as it is now: for the same A it would be an APPLY's blend and RESET could no longer put the first sound back; SOURCE stays, found again among the new candidates)
        retarget(s);
      }
    }
    return;
  }
  if (s->row == kSource) {
    s->sel = reset ? 0u : static_cast<uint32_t>(clamp_i(static_cast<int32_t>(s->sel) + d, 0, s->cands != 0u ? static_cast<int32_t>(s->cands) - 1 : 0));
    if (s->cands != 0u) {  // (with no candidate there is nothing to choose and the earlier choice stays)
      s->src = s->sel != 0u && s->sel < s->cands ? s->cand[s->sel] : 0u;  // the SOURCE the user chose (what keep_source() keeps); it survives an A that is B for a while
    }
  } else {
    s->pos = reset ? 50u : static_cast<uint32_t>(clamp_i(static_cast<int32_t>(s->pos) + 5 * d, 0, static_cast<int32_t>(kMaxPos)));
  }
}

// The coarse step of each row under Shift + the encoder (OS-APPUX S2): SOURCE by four candidates, POS by four of its steps of 5, INSTRUMENT A by eight.
constexpr int32_t kCoarse[kRows] = {4, 4, 8};

void keep_row(State* s) {
  if (s->row == kSource) {
    keep_source(s);
  } else if (s->row == kPos) {
    own_set(s->io, kPos, static_cast<float>(s->pos));  // (INSTRUMENT A is the target and is not kept)
  }
}

void input(void* raw, const o3m_ui_event* ev) {
  State* s = static_cast<State*>(raw);
  const Input in = grammar(ev, &s->row, kRows, nullptr, 1);
  if (in.move == kMoveValue) {
    edit_value(s, steps(in, kCoarse[s->row]), false);
    keep_row(s);
    s->msg = sticky(s) ? s->msg : kMsgNone;
    return;
  }
  if (ev->kind != O3M_EV_BUTTON || ev->value == 0) {
    return;
  }
  s->msg = sticky(s) ? s->msg : kMsgNone;
  if (in.move == kMoveDefault) {
    edit_value(s, 0, true);
    keep_row(s);
    return;
  }
  switch (ev->id) {
    case O3M_BTN_F1:
      apply(s, s->pos, false);
      break;
    case O3M_BTN_F3:
      s->pos = 0;
      own_set(s->io, kPos, 0.0f);
      apply(s, 0, true);
      break;
    default:
      break;
  }
}

// ---- the page ----------------------------------------------------------------------------------------------------------------------------------

void view_draw(void* raw, uint32_t, o3m_canvas* c) {
  State* s = static_cast<State*>(raw);
  char facts[64];
  uint32_t n = 0;
  if (s->kind == O3M_INSTRUMENT_SAMPLE || s->kind == O3M_INSTRUMENT_MODULE) {
    n = put_text(facts, n, "A = INSTRUMENT ");
    n = put_uint(facts, n, s->a, 2);
    n = put_text(facts, n, s->kind == O3M_INSTRUMENT_SAMPLE ? "  SAMPLE  " : "  MODULE  ");
    n = put_uint(facts, n, s->count, 1);
    n = put_text(facts, n, " WRITABLE");
  }
  facts[n] = '\0';
  PageRow rows[kRows];
  rows[kSource].name = "SOURCE B";
  rows[kSource].dim = s->cands == 0u;
  n = s->cands == 0u ? put_text(rows[kSource].value, 0, "NONE") : put_uint(rows[kSource].value, 0, s->cand[s->sel < s->cands ? s->sel : 0u], 2);
  rows[kSource].value[n] = '\0';
  rows[kPos].name = "POS";
  rows[kPos].dim = false;
  n = put_uint(rows[kPos].value, 0, s->pos, 1);
  n = put_text(rows[kPos].value, n, " %");
  rows[kPos].value[n] = '\0';
  rows[kInstrument].name = "INSTRUMENT A";
  rows[kInstrument].dim = s->home_a == 0u;  // "-" with no launch context
  n = s->home_a == 0u ? put_text(rows[kInstrument].value, 0, "-") : put_uint(rows[kInstrument].value, 0, s->a, 2);
  rows[kInstrument].value[n] = '\0';
  char extra[40];
  uint32_t x = 0;
  if (s->cands != 0u) {
    x = put_uint(extra, x, s->cands, 1);
    x = put_text(extra, x, s->cands == 1u ? " MATCHING INSTRUMENT" : " MATCHING INSTRUMENTS");
  }
  extra[x] = '\0';
  char answer[72];
  n = 0;
  uint32_t color = O3M_COLOR_ACCENT;
  switch (s->msg) {
    case kMsgApplied:
    case kMsgReset:
      n = put_uint(answer, n, s->msg_changed, 1);
      n = put_text(answer, n, s->msg == kMsgReset ? " PARAMETERS PUT BACK  (UNDO TO TAKE BACK)" : " PARAMETERS CHANGED  (UNDO TO TAKE BACK)");
      color = O3M_COLOR_PLAY;
      break;
    case kMsgUnchanged: n = put_text(answer, n, "NOTHING MOVED: A IS ALREADY THERE"); break;
    case kMsgKind: n = put_text(answer, n, "SAMPLE OR MODULE INSTRUMENTS ONLY"); break;
    case kMsgNoMatch: n = put_text(answer, n, "NO MATCHING INSTRUMENT"); break;
    case kMsgFailed:
      n = put_text(answer, n, "NOT APPLIED  (CODE ");
      n = put_uint(answer, n, static_cast<uint32_t>(s->msg_code), 1);
      n = put_text(answer, n, ")");
      break;
    case kMsgNoContext: n = put_text(answer, n, "OPEN IT FROM PATTERN F7 TOOLS"); break;
    case kMsgNoEdit: n = put_text(answer, n, "THIS HOST CANNOT EDIT"); break;
    default: break;
  }
  answer[n] = '\0';
  draw_page(c, "MORPH", facts, rows, kRows, s->row, extra, answer, color);
}

void leave(void*) {}
void shutdown(void*) {}

// OS-KEYROWDLG S5 (ABI 1.9): what the keys do, for the page's key row (labels only: the OS delivers the keys to input() whatever they say; F8 is the OS's BACK).
const o3m_keys kKeys = {{"APPLY", nullptr, "RESET"}, {"APPLY", nullptr, "RESET"}};  // (Shift + Fn runs Fn: no app reads `mods`, so the Shift row says the same)

const o3m_keys* ui_keys(void*) { return &kKeys; }

const o3m_ui kUi = {O3M_UI_SIZE(MORPH_ABI_MINOR), enter, input, view_draw, leave, ui_keys};

}  // namespace

extern "C" O3M_EXPORT const o3m_module o3m_entry __attribute__((section(".o3m_entry"))) = {
    MORPH_ENTRY_HEAD, sizeof(State), MORPH_ENTRY_PARAMS, init, nullptr, set_param, nullptr, nullptr, shutdown, MORPH_ENTRY_VIEWS(&kUi),
};
