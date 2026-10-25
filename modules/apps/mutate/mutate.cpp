// SPDX-License-Identifier: MIT
//
// "Mutate" (OS-H3B, new Community OS behaviour, ABI 1.6: capabilities read_project and edit_project): random variations of an instrument's sound. It uses the instrument-parameter entries of ABI 1.6 (instrument_info,
// get_param_desc, get_instrument_param, set_instrument_param).
//
//   TARGET   the instrument the app was launched with (the Pattern's current instrument): a SAMPLE or a MODULE instrument. Anything else says SAMPLE OR MODULE INSTRUMENTS ONLY. The INSTRUMENT row (OS-APPUX S5)
//            moves it inside the app (1..64; Delete puts the launch instrument back); it is not kept, a relaunch from TOOLS starts at the Pattern's instrument again.
//   VALUES   AMOUNT 0..100 % of each parameter's range, GROUP (ALL, or one of the groups that hold a writable parameter: VOICE, FILTER, ENV, SENDS, MOD for a sample; PAGE 1.. for a module), SEED 0..9999.
//   THE RULE for every parameter the OS marks writable in the group: new = old + (r - 0.5) x AMOUNT x (max - min), clamped; r is drawn from SEED and the parameter's id, so the same SEED always gives the
//            same result, whichever GROUP is chosen. A STEPPED or ENUM parameter moves by the delta rounded. Parameters the OS does not mark writable (the sample, the play mode, ...) are never touched.
//   KEYS     the grammar of o3m_edit.h (Up / Down the row, the encoder the value, Shift + the encoder in coarse steps, Delete the row's default). F1 APPLY writes the changes as ONE edit group (one undo unit, UNDO restores every value exactly); F2 VARY is SEED + 1, then
//            APPLY. AMOUNT 0 changes nothing, so APPLY then does nothing at all. F8 leaves.
//
// No OS parameters, no clock, no audio work, no heap. The module has no libc: every loop is written out.
#include <o3module.h>

#include "mutate_gen.h"
#include "mutate_math.h"
#include "o3m_edit.h"

namespace {

using namespace o3edit;
using namespace mutate;

enum Row : uint32_t { kAmount, kGroup, kSeed, kInstrument, kRows };  // (the rows before INSTRUMENT are declared and kept)
constexpr uint32_t kDefaultAmount = 20;
constexpr uint32_t kMaxGroups = 8;

struct State {
  const o3m_read_api* read;
  const o3m_edit_api* edit;
  const o3m_io_api* io;  // set_own_param: AMOUNT, GROUP and SEED are kept with the project (OS-APPUX S3)
  uint32_t ok;           // the OS can read and write instrument parameters
  uint32_t instrument;   // 1..64
  uint32_t home_instr;   // the launch instrument (what Delete puts back on INSTRUMENT); 0 = there is no launch context
  uint32_t kind;         // O3M_INSTRUMENT_*
  uint32_t count;        // writable parameters
  uint32_t groups;       // groups that hold a writable parameter
  uint32_t amount, group_sel, seed;  // group_sel 0 = ALL, else groups[group_sel - 1]
  uint32_t row;
  uint32_t msg;
  int32_t msg_code;
  uint32_t msg_changed;
  uint8_t group_id[kMaxGroups];
  PD pd[kMaxPD];
};
static_assert(sizeof(State) <= MUTATE_STATE_BYTES, "module.json: state_bytes is too small for State");

constexpr uint32_t kMsgNone = 0, kMsgApplied = 1, kMsgFailed = 2, kMsgNoContext = 3, kMsgNoEdit = 4, kMsgNothing = 5, kMsgKind = 6, kMsgUnchanged = 7;

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

// GROUP is kept as the index the user chose; the list it indexes follows the instrument, which the OS has not shown the app yet when it hands the value back (set_param, before enter()): a value past
// the list reads as ALL.
uint32_t sel_of(const State* s) { return s->group_sel <= s->groups ? s->group_sel : 0u; }
uint32_t group_of_sel(const State* s) { return sel_of(s) == 0u ? 0xFFFFu : s->group_id[sel_of(s) - 1u]; }

// The rows are the OS's to hand back (set_param before enter()) and the project's to keep (set_own_param).
void set_param(void* raw, uint32_t index, float v) {
  State* s = static_cast<State*>(raw);
  const uint32_t n = v > 0.0f ? static_cast<uint32_t>(v + 0.5f) : 0u;
  switch (index) {
    case kAmount: s->amount = n < kMaxAmount ? n : kMaxAmount; break;
    case kGroup: s->group_sel = n < kMaxGroups ? n : kMaxGroups; break;
    case kSeed: s->seed = n < kMaxSeed ? n : kMaxSeed; break;
    default: break;
  }
}

uint32_t row_value(const State* s, uint32_t row) { return row == kAmount ? s->amount : (row == kGroup ? s->group_sel : s->seed); }

uint32_t params_in_group(const State* s) {
  uint32_t n = 0;
  const uint32_t g = group_of_sel(s);
  for (uint32_t i = 0; i < s->count; ++i) {
    n += g == 0xFFFFu || s->pd[i].group == g ? 1u : 0u;
  }
  return n;
}

// The instrument's kind, writable parameters and groups (the launch, or an INSTRUMENT row the user moved).
void retarget(State* s) {
  s->count = s->groups = 0;
  s->kind = O3M_INSTRUMENT_EMPTY;
  s->msg = kMsgNone;
  uint32_t engine = 0;
  s->count = load_writable(s->read, s->instrument, s->pd, kMaxPD, &s->kind, &engine);
  if (s->kind != O3M_INSTRUMENT_SAMPLE && s->kind != O3M_INSTRUMENT_MODULE) {
    s->msg = kMsgKind;
    return;
  }
  s->groups = list_groups(s->pd, s->count, s->group_id, kMaxGroups);
}

void enter(void* raw) {
  State* s = static_cast<State*>(raw);
  s->row = 0;
  s->msg = kMsgNone;
  s->msg_code = 0;
  s->msg_changed = 0;
  s->count = s->groups = 0;
  s->kind = O3M_INSTRUMENT_EMPTY;
  s->home_instr = 0;
  o3m_launch_ctx lc;
  lc.size = sizeof lc;
  lc.flags = lc.pattern = lc.track = lc.instrument = lc.block_start = lc.block_length = 0;
  lc.block_tracks = 0;
  lc.reserved[0] = lc.reserved[1] = lc.reserved[2] = 0;
  if (s->read->launch_ctx(&lc) == 0u) {
    s->msg = kMsgNoContext;
    return;
  }
  s->instrument = s->home_instr = lc.instrument + 1u <= O3M_INSTR_MAX ? lc.instrument + 1u : O3M_INSTR_MAX;
  retarget(s);
}

// ---- APPLY -------------------------------------------------------------------------------------------------------------------------------------

void apply(State* s) {
  if (s->msg == kMsgNoContext || s->msg == kMsgKind) {
    return;  // (the answer stays: there is nothing to apply to)
  }
  if (s->ok == 0u) {
    s->msg = kMsgNoEdit;
    return;
  }
  if (s->amount == 0u || s->count == 0u) {
    s->msg = kMsgNothing;  // AMOUNT 0 changes nothing: no group, so no undo unit
    return;
  }
  const int32_t b = s->edit->edit_begin();
  if (b != O3M_EDIT_OK) {
    s->msg = kMsgFailed;
    s->msg_code = b;
    return;
  }
  const uint32_t g = group_of_sel(s);
  uint32_t changed = 0;
  for (uint32_t i = 0; i < s->count; ++i) {
    const PD& p = s->pd[i];
    float old = 0.0f;
    if ((g != 0xFFFFu && p.group != g) || s->read->get_instrument_param(static_cast<uint8_t>(s->instrument), p.id, &old) != O3M_EDIT_OK) {
      continue;
    }
    const float v = mutate_value(old, p.min, p.max, p.whole != 0u, s->amount, unit_of(s->seed, p.id));
    if (v != old && s->edit->set_instrument_param(static_cast<uint8_t>(s->instrument), p.id, v) == O3M_EDIT_OK) {
      ++changed;
    }
  }
  const int32_t e = s->edit->edit_end();
  s->msg_code = e;
  s->msg_changed = changed;
  s->msg = e != O3M_EDIT_OK ? kMsgFailed : (changed != 0u ? kMsgApplied : kMsgUnchanged);
}

// ---- editing -----------------------------------------------------------------------------------------------------------------------------------

void edit_value(State* s, int32_t d, bool reset) {
  switch (s->row) {
    case kAmount:
      s->amount = reset ? kDefaultAmount : static_cast<uint32_t>(clamp_i(static_cast<int32_t>(s->amount) + 5 * d, 0, static_cast<int32_t>(kMaxAmount)));
      break;
    case kGroup:
      s->group_sel = reset ? 0u : static_cast<uint32_t>(clamp_i(static_cast<int32_t>(sel_of(s)) + d, 0, static_cast<int32_t>(s->groups)));
      break;
    case kSeed:
      s->seed = reset ? 0u : static_cast<uint32_t>(clamp_i(static_cast<int32_t>(s->seed) + d, 0, static_cast<int32_t>(kMaxSeed)));
      break;
    default:  // INSTRUMENT: not kept, and nothing to move with no launch context
      if (s->home_instr != 0u) {
        s->instrument = step_instrument(s->instrument, s->home_instr, d, reset);
        retarget(s);
      }
      break;
  }
}

// The coarse step of each row under Shift + the encoder (OS-APPUX S2): AMOUNT by four of its steps of 5, SEED by hundreds, INSTRUMENT by eight.
constexpr int32_t kCoarse[kRows] = {4, 1, 100, 8};

// A key clears the answer to the last APPLY; the two that describe the target stay (retarget() sets the instrument's).
void settle(State* s) {
  if (s->msg != kMsgNoContext && s->msg != kMsgKind) {
    s->msg = kMsgNone;
  }
}

// A declared value is kept with the project; INSTRUMENT is the target and is not.
void keep_row(State* s) {
  if (s->row < kInstrument) {
    own_set(s->io, s->row, static_cast<float>(row_value(s, s->row)));
  }
}

void input(void* raw, const o3m_ui_event* ev) {
  State* s = static_cast<State*>(raw);
  const Input in = grammar(ev, &s->row, kRows, nullptr, 1);
  if (in.move == kMoveValue) {
    edit_value(s, steps(in, kCoarse[s->row]), false);
    keep_row(s);
    settle(s);
    return;
  }
  if (ev->kind != O3M_EV_BUTTON || ev->value == 0) {
    return;
  }
  settle(s);
  if (in.move == kMoveDefault) {
    edit_value(s, 0, true);
    keep_row(s);
    settle(s);
    return;
  }
  switch (ev->id) {
    case O3M_BTN_F1:
      apply(s);
      break;
    case O3M_BTN_F2:
      s->seed = s->seed < kMaxSeed ? s->seed + 1u : 0u;
      own_set(s->io, kSeed, static_cast<float>(s->seed));
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
  uint32_t n = 0;
  if (s->kind == O3M_INSTRUMENT_SAMPLE || s->kind == O3M_INSTRUMENT_MODULE) {
    n = put_text(facts, n, "INSTRUMENT ");
    n = put_uint(facts, n, s->instrument, 2);
    n = put_text(facts, n, s->kind == O3M_INSTRUMENT_SAMPLE ? "  SAMPLE  " : "  MODULE  ");
    n = put_uint(facts, n, s->count, 1);
    n = put_text(facts, n, " WRITABLE");
  }
  facts[n] = '\0';
  static const char* const kNames[kRows] = {"AMOUNT", "GROUP", "SEED", "INSTRUMENT"};
  PageRow rows[kRows];
  for (uint32_t r = 0; r < kRows; ++r) {
    rows[r].name = kNames[r];
    rows[r].dim = false;
    uint32_t k = 0;
    switch (r) {
      case kAmount:
        k = put_uint(rows[r].value, 0, s->amount, 1);
        k = put_text(rows[r].value, k, " %");
        break;
      case kGroup: k = sel_of(s) == 0u ? put_text(rows[r].value, 0, "ALL") : put_group_name(rows[r].value, 0, s->kind, s->group_id[sel_of(s) - 1u]); break;
      case kSeed: k = put_uint(rows[r].value, 0, s->seed, 4); break;
      default:  // INSTRUMENT: dim and "-" with no launch context
        rows[r].dim = s->home_instr == 0u;
        k = s->home_instr == 0u ? put_text(rows[r].value, 0, "-") : put_uint(rows[r].value, 0, s->instrument, 2);
        break;
    }
    rows[r].value[k] = '\0';
  }
  char extra[40];
  uint32_t x = 0;
  if (s->count != 0u) {
    x = put_uint(extra, x, params_in_group(s), 1);
    x = put_text(extra, x, " PARAMETERS IN THE GROUP");
  }
  extra[x] = '\0';
  char answer[72];
  n = 0;
  uint32_t color = O3M_COLOR_ACCENT;
  switch (s->msg) {
    case kMsgApplied:
      n = put_uint(answer, n, s->msg_changed, 1);
      n = put_text(answer, n, s->msg_changed == 1u ? " PARAMETER CHANGED  (UNDO TO TAKE BACK)" : " PARAMETERS CHANGED  (UNDO TO TAKE BACK)");
      color = O3M_COLOR_PLAY;
      break;
    case kMsgUnchanged: n = put_text(answer, n, "NO PARAMETER MOVED: TRY A LARGER AMOUNT OR ANOTHER SEED"); break;
    case kMsgNothing: n = put_text(answer, n, s->amount == 0u ? "AMOUNT IS 0: NOTHING TO DO" : "NO WRITABLE PARAMETER"); break;
    case kMsgKind: n = put_text(answer, n, "SAMPLE OR MODULE INSTRUMENTS ONLY"); break;
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
  draw_page(c, "MUTATE", facts, rows, kRows, s->row, extra, answer, color);
}

void leave(void*) {}
void shutdown(void*) {}

// OS-KEYROWDLG S5 (ABI 1.9): what the keys do, for the page's key row (labels only: the OS delivers the keys to input() whatever they say; F8 is the OS's BACK).
const o3m_keys kKeys = {{"APPLY", "VARY"}, {"APPLY", "VARY"}};  // (Shift + Fn runs Fn: no app reads `mods`, so the Shift row says the same)

const o3m_keys* ui_keys(void*) { return &kKeys; }

const o3m_ui kUi = {O3M_UI_SIZE(MUTATE_ABI_MINOR), enter, input, view_draw, leave, ui_keys};

}  // namespace

extern "C" O3M_EXPORT const o3m_module o3m_entry __attribute__((section(".o3m_entry"))) = {
    MUTATE_ENTRY_HEAD, sizeof(State), MUTATE_ENTRY_PARAMS, init, nullptr, set_param, nullptr, nullptr, shutdown, MUTATE_ENTRY_VIEWS(&kUi),
};
