// SPDX-License-Identifier: MIT
//
// "Chords" (OS-H3B, new Community OS behaviour, ABI 1.5: capabilities read_project and edit_project): a chord progression into the Pattern's block. It uses only the step entries of ABI 1.5, so it declares 1.5
// and also runs on an OS of 1.5; where the OS is 1.6 it asks instrument_info once, to choose the default MODE.
//
//   TARGET   the block's first track and rows, or with no block the cursor's track from the cursor row to the end of the track. SPREAD uses the first 3 (4 with SEVENTHS) tracks of the block. The app starts there; page 2
//            (Right) holds the five target rows (PATTERN, TRACK, ROW, ROWS, TRACKS; o3m_edit.h, OS-APPUX S5) that move it inside the app. They are not kept (a relaunch from TOOLS starts at the cursor again), Delete puts the launch value back.
//   VALUES   KEY C..B, SCALE MAJOR / MINOR, PROG one of eight progressions (a list of scale degrees), RATE rows per chord (default: the rows divided by the number of chords), OCTAVE 2..6, SEVENTHS,
//            INSTR (default the context's), MODE.
//   CHORD FX at each chord row: the root note, the instrument and Chord FX 14 (FX2, or FX1 when FX2 is taken); the other rows of the target get an empty note. Only a MIDI instrument plays the whole chord:
//            the sequencer keeps one voice a track, so a sample or module instrument plays the root alone (seq/src/sequencer.cpp pfx_note, `++stats_.fx_ignored`).
//   SPREAD   one chord tone per track (root, third, fifth, seventh), the same rows, no FX: it sounds on every instrument.
//   DEFAULT  MODE is CHORD FX when the instrument is a MIDI instrument and SPREAD otherwise (and on an OS that cannot say what the instrument is). Moving MODE yourself stops the default following INSTR.
//   KEYS     the grammar of o3m_edit.h (Up / Down the row, Left / Right the page, the encoder the value, Shift + the encoder in coarse steps, Delete the row's default; on RATE and MODE that is AUTO). F1 APPLY writes the target as ONE edit group (one undo unit). F8 leaves.
//
// No OS parameters, no clock, no audio work, no heap. The module has no libc: every loop is written out.
#include <o3module.h>

#include "chords_gen.h"
#include "chords_math.h"
#include "o3m_edit.h"

namespace {

using namespace o3edit;
using namespace chords;

enum Row : uint32_t { kKey, kScale, kProg, kRate, kOctave, kSevenths, kInstr, kMode, kRows };
constexpr uint32_t kModeFx = 0, kModeSpread = 1;
constexpr uint32_t kPages = 2;  // the eight settings, then the target rows

struct State {
  const o3m_read_api* read;
  const o3m_edit_api* edit;
  const o3m_io_api* io;  // set_own_param: the eight rows are kept with the project (OS-APPUX S3)
  uint32_t instr_auto;   // INSTR follows the context's instrument
  uint32_t have_edit;
  uint32_t have_kind;  // instrument_info exists (an OS of 1.6)
  Target tgt;
  Setup u;             // rate is the effective one (see rate_auto)
  uint32_t rate_auto;  // RATE follows the default
  uint32_t mode;
  uint32_t mode_auto;  // MODE follows the instrument's kind
  uint32_t page, row;
  uint32_t msg;
  int32_t msg_code;
  uint32_t msg_a, msg_b;  // numbers behind the message
};
static_assert(sizeof(State) <= CHORDS_STATE_BYTES, "module.json: state_bytes is too small for State");

constexpr uint32_t kMsgNone = 0, kMsgApplied = 1, kMsgFailed = 2, kMsgNoPattern = 3, kMsgNoContext = 4, kMsgNoEdit = 5, kMsgNothing = 6;

// ---- state -------------------------------------------------------------------------------------------------------------------------------------

uint32_t target_rows(const State* s) { return track_rows(&s->tgt, 0); }

void sync_rate(State* s) {
  if (s->rate_auto != 0u) {
    const uint32_t rows = target_rows(s);
    s->u.rate = default_rate(s->u, rows != 0u ? rows : 1u);
  }
}

// The default MODE for the instrument: CHORD FX for a MIDI instrument, SPREAD for any other and when the OS cannot say.
uint32_t default_mode(const State* s) {
  if (s->have_kind != 0u) {
    o3m_instrument_info info;
    info.size = sizeof info;
    info.kind = 0;
    if (s->read->instrument_info(static_cast<uint8_t>(s->u.instr), &info) == O3M_EDIT_OK && info.kind == O3M_INSTRUMENT_MIDI) {
      return kModeFx;
    }
  }
  return kModeSpread;
}

int init(void* raw, const o3m_host* host) {
  State* s = static_cast<State*>(raw);
  if (host->draw == nullptr || !O3M_HOST_HAS(host, read) || host->read == nullptr || !O3M_TABLE_HAS(host->read, get_steps) || host->read->get_steps == nullptr || host->read->pattern_info == nullptr ||
      host->read->launch_ctx == nullptr) {
    return 1;  // an OS below 1.5, or an app without read_project: nothing to write into
  }
  s->read = host->read;
  s->io = O3M_HOST_HAS(host, io) ? host->io : nullptr;
  s->have_kind = O3M_TABLE_HAS(host->read, instrument_info) && host->read->instrument_info != nullptr ? 1u : 0u;
  s->edit = O3M_HOST_HAS(host, edit) && host->edit != nullptr && O3M_TABLE_HAS(host->edit, set_steps) && host->edit->edit_begin != nullptr && host->edit->edit_end != nullptr &&
                    host->edit->set_steps != nullptr
                ? host->edit
                : nullptr;
  s->have_edit = s->edit != nullptr ? 1u : 0u;
  return 0;
}

// The eight declared rows come back from the OS before enter() (set_param: the project's kept values, else the manifest's defaults). RATE, INSTR and MODE have an AUTO that follows the target and the
// instrument, written 0: RATE 1..128 is a value, INSTR 1..64 an instrument, MODE 1 is CHORD FX and 2 SPREAD.
void set_param(void* raw, uint32_t index, float v) {
  State* s = static_cast<State*>(raw);
  const uint32_t n = v > 0.0f ? static_cast<uint32_t>(v + 0.5f) : 0u;
  switch (index) {
    case kKey: s->u.key = n < 11u ? n : 11u; break;
    case kScale: s->u.minor = n != 0u ? 1u : 0u; break;
    case kProg: s->u.prog = n < kProgCount ? n : kProgCount - 1u; break;
    case kRate:
      s->rate_auto = n == 0u ? 1u : 0u;
      if (n != 0u) {
        s->u.rate = n < O3M_PATTERN_ROWS_MAX ? n : O3M_PATTERN_ROWS_MAX;
      }
      break;
    case kOctave: s->u.octave = n < kMinOctave ? kMinOctave : (n > kMaxOctave ? kMaxOctave : n); break;
    case kSevenths: s->u.sevenths = n != 0u ? 1u : 0u; break;
    case kInstr:
      s->instr_auto = n == 0u ? 1u : 0u;
      if (n != 0u) {
        s->u.instr = n < O3M_INSTR_MAX ? n : O3M_INSTR_MAX;
      }
      break;
    case kMode:
      s->mode_auto = n == 0u ? 1u : 0u;
      if (n != 0u) {
        s->mode = n == 1u ? kModeFx : kModeSpread;
      }
      break;
    default: break;
  }
}

// A value the user changed or reset is kept with the project (the AUTOs as 0).
void keep_row(State* s) {
  if (s->page != 0u) {
    return;  // (the target rows are not kept)
  }
  float v = 0.0f;
  switch (s->row) {
    case kKey: v = static_cast<float>(s->u.key); break;
    case kScale: v = static_cast<float>(s->u.minor); break;
    case kProg: v = static_cast<float>(s->u.prog); break;
    case kRate: v = s->rate_auto != 0u ? 0.0f : static_cast<float>(s->u.rate); break;
    case kOctave: v = static_cast<float>(s->u.octave); break;
    case kSevenths: v = static_cast<float>(s->u.sevenths); break;
    case kInstr: v = s->instr_auto != 0u ? 0.0f : static_cast<float>(s->u.instr); break;
    default: v = s->mode_auto != 0u ? 0.0f : (s->mode == kModeFx ? 1.0f : 2.0f); break;
  }
  own_set(s->io, s->row, v);
}

void enter(void* raw) {
  State* s = static_cast<State*>(raw);
  s->page = s->row = 0;
  s->msg = kMsgNone;
  s->msg_code = 0;
  s->msg_a = s->msg_b = 0;
  if (!read_target(s->read, &s->tgt, 0)) {
    s->msg = s->tgt.from_tools == 0u ? kMsgNoContext : kMsgNoPattern;
  }
  if (s->instr_auto != 0u) {
    s->u.instr = s->tgt.instrument;
  }
  if (s->mode_auto != 0u) {
    s->mode = default_mode(s);
  }
  sync_rate(s);
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
  const uint32_t rows = target_rows(s);
  if (rows == 0u) {
    s->msg = kMsgNothing;
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
  const uint16_t pattern = static_cast<uint16_t>(s->tgt.pattern);
  const uint8_t first = static_cast<uint8_t>(s->tgt.first_row);
  uint32_t written = 0, no_slot = 0, left_out = 0;
  if (s->mode == kModeFx) {
    const uint8_t track = static_cast<uint8_t>(s->tgt.first_track);
    if (s->read->get_steps(pattern, track, first, cur, static_cast<uint8_t>(rows)) == O3M_EDIT_OK) {
      written = chordfx_steps<o3m_step>(s->u, cur, out, rows, &no_slot);
      (void)s->edit->set_steps(pattern, track, first, out, static_cast<uint8_t>(rows), O3M_STEP_NOTE | O3M_STEP_INSTR | O3M_STEP_FX1 | O3M_STEP_FX2);
    }
  } else {
    const uint32_t want = s->u.sevenths != 0u ? 4u : 3u;
    const uint32_t tracks = want < s->tgt.tracks ? want : s->tgt.tracks;
    left_out = want - tracks;
    for (uint32_t t = 0; t < tracks; ++t) {
      const uint8_t track = static_cast<uint8_t>(s->tgt.first_track + t);
      const uint32_t n = track_rows(&s->tgt, t);
      if (n == 0u || s->read->get_steps(pattern, track, first, cur, static_cast<uint8_t>(n)) != O3M_EDIT_OK) {
        continue;
      }
      const uint32_t w = spread_steps<o3m_step>(s->u, t, cur, out, n);
      written = w > written ? w : written;
      (void)s->edit->set_steps(pattern, track, first, out, static_cast<uint8_t>(n), O3M_STEP_NOTE | O3M_STEP_INSTR);
    }
  }
  const int32_t e = s->edit->edit_end();
  s->msg_code = e;
  s->msg_a = written;
  s->msg_b = s->mode == kModeFx ? no_slot : left_out;
  s->msg = e != O3M_EDIT_OK ? kMsgFailed : kMsgApplied;
}

// ---- editing -----------------------------------------------------------------------------------------------------------------------------------

uint32_t page_rows(const State* s) { return s->page == 0u ? static_cast<uint32_t>(kRows) : static_cast<uint32_t>(kTargetRows); }

void edit_value(State* s, int32_t d, bool reset) {
  Setup& u = s->u;
  if (s->page != 0u) {
    edit_target(s->read, &s->tgt, s->row, d, reset);
    sync_rate(s);  // (RATE follows the rows)
    return;
  }
  switch (s->row) {
    case kKey:
      u.key = reset ? 0u : static_cast<uint32_t>((static_cast<int32_t>(u.key) + d % 12 + 12) % 12);
      break;
    case kScale:
      u.minor = reset ? 0u : (d != 0 ? (d > 0 ? 1u : 0u) : u.minor);
      break;
    case kProg:
      u.prog = reset ? 0u : static_cast<uint32_t>(clamp_i(static_cast<int32_t>(u.prog) + d, 0, static_cast<int32_t>(kProgCount) - 1));
      break;
    case kRate:
      if (reset) {
        s->rate_auto = 1;
      } else {
        s->rate_auto = 0;
        u.rate = static_cast<uint32_t>(clamp_i(static_cast<int32_t>(u.rate) + d, 1, static_cast<int32_t>(O3M_PATTERN_ROWS_MAX)));
      }
      break;
    case kOctave:
      u.octave = reset ? kDefaultOctave : static_cast<uint32_t>(clamp_i(static_cast<int32_t>(u.octave) + d, static_cast<int32_t>(kMinOctave), static_cast<int32_t>(kMaxOctave)));
      break;
    case kSevenths:
      u.sevenths = reset ? 0u : (d != 0 ? (d > 0 ? 1u : 0u) : u.sevenths);
      break;
    case kInstr:
      s->instr_auto = reset ? 1u : 0u;
      u.instr = reset ? s->tgt.instrument : static_cast<uint32_t>(clamp_i(static_cast<int32_t>(u.instr) + d, 1, static_cast<int32_t>(O3M_INSTR_MAX)));
      if (s->mode_auto != 0u) {
        s->mode = default_mode(s);
      }
      break;
    default:
      if (reset) {
        s->mode_auto = 1;
        s->mode = default_mode(s);
      } else if (d != 0) {
        s->mode_auto = 0;
        s->mode = d > 0 ? kModeSpread : kModeFx;
      }
      break;
  }
  sync_rate(s);
}

// The coarse step of each row under Shift + the encoder (OS-APPUX S2): RATE by four rows, INSTR by eight; the rest are lists or a few values.
constexpr int32_t kCoarse[kRows] = {1, 1, 1, 4, 1, 1, 8, 1};

void input(void* raw, const o3m_ui_event* ev) {
  State* s = static_cast<State*>(raw);
  const Input in = grammar(ev, &s->row, page_rows(s), &s->page, kPages);
  if (in.move == kMoveValue) {
    edit_value(s, steps(in, s->page == 0u ? kCoarse[s->row] : target_coarse(s->row)), false);
    keep_row(s);
    s->msg = kMsgNone;
    return;
  }
  if (ev->kind != O3M_EV_BUTTON || ev->value == 0) {
    return;
  }
  s->msg = kMsgNone;
  if (in.move == kMovePage) {
    s->row = s->row < page_rows(s) ? s->row : page_rows(s) - 1u;  // (the new page has fewer rows)
    return;
  }
  if (in.move == kMoveDefault) {
    edit_value(s, 0, true);
    keep_row(s);
    return;
  }
  if (ev->id == O3M_BTN_F1) {
    apply(s);
  }
}

// ---- the page ----------------------------------------------------------------------------------------------------------------------------------

uint32_t put_chord_name(char* out, uint32_t at, const Chord& c) {
  static const char* const kPc[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  static const char* const kSuffix[kQualityCount] = {"", "m", "dim", "maj7", "m7", "7", "m7b5"};
  at = put_text(out, at, kPc[(c.root - 1u) % 12u]);
  return put_text(out, at, kSuffix[c.quality]);
}

void view_draw(void* raw, uint32_t, o3m_canvas* c) {
  State* s = static_cast<State*>(raw);
  const Setup& u = s->u;
  char facts[64];
  facts[0] = '\0';
  if (s->tgt.from_tools != 0u && s->tgt.have_pattern != 0u) {
    uint32_t n = put_text(facts, 0, s->tgt.has_block != 0u ? "BLOCK  TRACK " : "AT THE CURSOR  TRACK ");  // (the launch's word; the track and rows are what exists of the target now)
    n = put_uint(facts, n, s->tgt.first_track + 1u, 1);
    if (s->mode == kModeSpread && s->tgt.tracks > 1u) {
      n = put_text(facts, n, "-");
      n = put_uint(facts, n, s->tgt.first_track + s->tgt.tracks, 1);
    }
    n = put_text(facts, n, "  ROWS ");
    const uint32_t cnt = target_rows(s);
    n = put_uint(facts, n, s->tgt.first_row + 1u, 1);
    n = put_text(facts, n, "-");
    n = put_uint(facts, n, s->tgt.first_row + (cnt != 0u ? cnt : 1u), 1);
    facts[n] = '\0';
  }
  static const char* const kNames[kRows] = {"KEY", "SCALE", "PROG", "RATE", "OCTAVE", "SEVENTHS", "INSTR", "MODE"};
  static const char* const kKeys[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  static const char* const kProgNames[kProgCount] = {"I-V-vi-IV", "I-vi-IV-V", "ii-V-I", "I-IV-V-IV", "vi-IV-I-V", "i-VI-III-VII", "i-iv-v-i", "12-BAR BLUES"};
  PageRow rows[kRows];
  for (uint32_t r = 0; s->page != 0u && r < kTargetRows; ++r) {
    fill_target_row(&rows[r], &s->tgt, r);
  }
  for (uint32_t r = 0; s->page == 0u && r < kRows; ++r) {
    rows[r].name = kNames[r];
    rows[r].dim = false;
    uint32_t n = 0;
    switch (r) {
      case kKey: n = put_text(rows[r].value, 0, kKeys[u.key % 12u]); break;
      case kScale: n = put_text(rows[r].value, 0, u.minor != 0u ? "MINOR" : "MAJOR"); break;
      case kProg: n = put_text(rows[r].value, 0, kProgNames[u.prog % kProgCount]); break;
      case kRate:
        n = put_uint(rows[r].value, 0, u.rate, 1);
        n = put_text(rows[r].value, n, s->rate_auto != 0u ? " AUTO" : "");
        break;
      case kOctave: n = put_uint(rows[r].value, 0, u.octave, 1); break;
      case kSevenths: n = put_text(rows[r].value, 0, u.sevenths != 0u ? "ON" : "OFF"); break;
      case kInstr: n = put_uint(rows[r].value, 0, u.instr, 2); break;
      default:
        n = put_text(rows[r].value, 0, s->mode == kModeFx ? "CHORD FX" : "SPREAD");
        n = put_text(rows[r].value, n, s->mode_auto != 0u ? " AUTO" : "");
        break;
    }
    rows[r].value[n] = '\0';
  }
  // the chords as they will be written, on the right
  char extra[64];
  uint32_t x = 0;
  const uint32_t fit = chords_fitting(u, target_rows(s) != 0u ? target_rows(s) : 1u);
  for (uint32_t k = 0; k < fit && x + 12u < sizeof extra; ++k) {
    x = put_chord_name(extra, x, chord_of(u, k));
    x = put_text(extra, x, " ");
  }
  extra[x] = '\0';
  char answer[72];
  uint32_t n = 0;
  uint32_t color = O3M_COLOR_ACCENT;
  switch (s->msg) {
    case kMsgApplied:
      n = put_uint(answer, n, s->msg_a, 1);
      n = put_text(answer, n, s->mode == kModeFx ? " CHORDS WRITTEN" : " ROWS WRITTEN");
      if (s->msg_b != 0u) {
        n = put_text(answer, n, "  (");
        n = put_uint(answer, n, s->msg_b, 1);
        n = put_text(answer, n, s->mode == kModeFx ? " WITHOUT FX: NO FREE SLOT)" : " TONES LEFT OUT: TOO FEW TRACKS)");
      } else {
        n = put_text(answer, n, "  (UNDO TO TAKE BACK)");
      }
      color = O3M_COLOR_PLAY;
      break;
    case kMsgNothing: n = put_text(answer, n, "NOTHING TO WRITE: THE TARGET IS PAST THE TRACK END"); break;
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
  draw_page(c, s->page == 0u ? "CHORDS  1/2" : "CHORDS  TARGET  2/2", facts, rows, page_rows(s), s->row, extra, answer, color);
}

void leave(void*) {}
void shutdown(void*) {}

// OS-KEYROWDLG S5 (ABI 1.9): what the keys do, for the page's key row (labels only: the OS delivers the keys to input() whatever they say; F8 is the OS's BACK).
const o3m_keys kKeys = {{"APPLY"}, {"APPLY"}};  // (Shift + Fn runs Fn: no app reads `mods`, so the Shift row says the same)

const o3m_keys* ui_keys(void*) { return &kKeys; }

const o3m_ui kUi = {O3M_UI_SIZE(CHORDS_ABI_MINOR), enter, input, view_draw, leave, ui_keys};

}  // namespace

extern "C" O3M_EXPORT const o3m_module o3m_entry __attribute__((section(".o3m_entry"))) = {
    CHORDS_ENTRY_HEAD, sizeof(State), CHORDS_ENTRY_PARAMS, init, nullptr, set_param, nullptr, nullptr, shutdown, CHORDS_ENTRY_VIEWS(&kUi),
};
