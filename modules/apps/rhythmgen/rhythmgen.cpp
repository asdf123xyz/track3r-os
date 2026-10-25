// SPDX-License-Identifier: MIT
//
// "Rhythm Gen" (OS-H3A, new Community OS behaviour, ABI 1.5: capabilities read_project and edit_project): a polyrhythm generator for the Pattern's block, the first app that edits the project.
// Opened from Pattern F7 TOOLS, it is given the block (pattern, first track, tracks, first row, rows) in the launch context and writes notes into it.
//
//   LANES    one per track of the block, up to four (LANES 4 OF n says the rest are left alone). With no block there is one lane: the cursor's track, 16 rows from the cursor row (or to the end of the track).
//   PER LANE HITS 0..LENGTH, LENGTH 1..64 (the cycle repeats over the block), ROTATE 0..LENGTH-1, NOTE, PROB 0..100 %. Different LENGTHs on different lanes are the polyrhythm.
//   SHARED   INSTR, VOL (unset or 0..127), MODE (REPLACE empties the note on every row that is not a hit, ADD keeps the notes there), SEED 0..9999; then the four target rows PATTERN, TRACK, ROW, ROWS (o3m_edit.h,
//            OS-APPUX S5): the app starts at the Pattern cursor and these move the block inside the app (LANES stay the block's tracks, so there is no TRACKS row). They are not kept (a relaunch from TOOLS starts at the cursor again), Delete puts the launch value back.
//   THE RULE row i of the cycle is a hit when (i x HITS) mod LENGTH < HITS: the Euclid rule of the Fill dialog, so both give the same pattern; then the cycle is rotated by ROTATE. Each hit is kept with
//            probability PROB, drawn from xorshift32 seeded by SEED and the lane: the same settings always give the same steps. Notes, instrument and volume are written; FX are never touched.
//   KEYS     the grammar of o3m_edit.h: Left / Right the page (1..4 the lanes, 5 the shared values), Up / Down the row, the encoder the value, Shift + the encoder in coarse steps, Delete the row's default. F1 APPLY writes the lanes as ONE edit group (one undo unit,
//            UNDO takes it back); F2 VARY is SEED + 1 and then APPLY (a fresh variation, a fresh undo unit). F8 leaves. There is no preview: you hear it in Pattern.
//
// No OS parameters: the pages are the app's own (the OS lists at most eight parameter rows). No clock, no audio work, no heap. The module has no libc: every loop is written out.
#include <o3module.h>

#include "o3m_edit.h"
#include "rhythmgen_gen.h"
#include "rhythmgen_math.h"

namespace {

using namespace rhythmgen;
using o3edit::grammar;  // OS-APPUX S2: the keys every app page answers to (its own text helpers and drawing stay: the lane pages are the app's)
using o3edit::Input;
using o3edit::steps;
using o3edit::own_set;
using o3edit::edit_target;
using o3edit::fill_target_row;
using o3edit::read_target;
using o3edit::target_coarse;
using o3edit::track_rows;

constexpr uint32_t kPages = 5;  // lanes 1..4, then the shared values
constexpr uint32_t kLaneRows = 5;
enum LaneRow : uint32_t { kHits, kLength, kRotate, kNote, kProb };
enum SharedRow : uint32_t { kInstr, kVol, kMode, kSeed, kSharedDeclared };  // (the four target rows follow the four declared ones: PATTERN, TRACK, ROW, ROWS)
constexpr uint32_t kSharedRows = kSharedDeclared + 4;  // no TRACKS row: the lanes are the block's tracks (the shared page holds eight rows)

// `autos`: the values that follow the block instead of being a choice (HITS from LENGTH, LENGTH from the block's rows, NOTE from the first note in it): kept as 0 with the project (OS-APPUX S3).
constexpr uint8_t kAutoHits = 1, kAutoLength = 2, kAutoNote = 4;
struct Lane {
  uint8_t hits, length, rotate, note, prob, autos;
};

struct State {
  const o3m_read_api* read;
  const o3m_edit_api* edit;
  const o3m_io_api* io;   // set_own_param (the 24 rows are kept with the project), now_ms and play_note (the audition)
  uint32_t instr_auto;    // INSTR follows the context's instrument
  uint32_t ctx_instr;     // that instrument, 1..64 (1 with no context)
  uint32_t aud_on, aud_lane, aud_pos, aud_next, aud_instr, aud_held;  // the audition of a lane's hits (OS-APPUX S3): the step it is at, when the next is due, the instrument whose note is sounding
  uint32_t have_edit;     // the edit table can write steps (an OS of ABI 1.5 with edit_project)
  o3edit::Target tgt;     // the block (pattern, first track, first row, rows; kNoBlockRows rows with no block), moved by the four target rows (OS-APPUX S5); `tracks` is the block's, not a row
  uint32_t total_lanes;   // tracks the block covers (LANES x OF n); 0 with no context
  uint32_t lanes;         // min(total_lanes, 4)
  uint32_t instr;         // 1..64
  uint32_t vol_ui;        // 0 = unset, 1..128 = volume 0..127
  uint32_t replace;       // MODE
  uint32_t seed;          // 0..9999
  uint32_t page;          // 0..4
  uint32_t row;           // the cursor row of the page
  uint32_t msg;           // kMsg*: the answer to the last APPLY, kept until the next key
  int32_t msg_code;       // the edit table's code behind it
  uint32_t msg_lanes;     // lanes written
  Lane lane[kMaxLanes];
  uint8_t lane_default_note[kMaxLanes];
};
static_assert(sizeof(State) <= RHYTHMGEN_STATE_BYTES, "module.json: state_bytes is too small for State");

constexpr uint32_t kMsgNone = 0, kMsgApplied = 1, kMsgFailed = 2, kMsgNoPattern = 3, kMsgNoContext = 4, kMsgNoEdit = 5, kMsgNothing = 6;

// ---- the block ---------------------------------------------------------------------------------------------------------------------------------

// The rows of lane `l` that exist: the block's rows cut at the track's own length (0: the track is not in the pattern or the block lies past its end).
uint32_t lane_rows(const State* s, uint32_t l, uint32_t* first) {
  *first = s->tgt.first_row;
  return l < s->lanes ? track_rows(&s->tgt, l) : 0u;
}

uint32_t default_length(const State* s) { return s->tgt.rows < kMaxLength ? s->tgt.rows : kMaxLength; }

uint32_t default_hits(uint32_t length) {
  const uint32_t h = length / 4u;
  return h != 0u ? h : 1u;
}

// The note a lane starts with: the first note already in its rows, else C-4.
uint8_t first_note_of(State* s, uint32_t l) {
  uint32_t first = 0;
  const uint32_t n = lane_rows(s, l, &first);
  o3m_step cur[O3M_PATTERN_ROWS_MAX];
  if (n != 0u && n <= O3M_PATTERN_ROWS_MAX && s->read->get_steps(static_cast<uint16_t>(s->tgt.pattern), static_cast<uint8_t>(s->tgt.first_track + l), static_cast<uint8_t>(first), cur,
                                                                  static_cast<uint8_t>(n)) == O3M_EDIT_OK) {
    for (uint32_t j = 0; j < n; ++j) {
      if (cur[j].note >= O3M_NOTE_FIRST && cur[j].note <= O3M_NOTE_LAST) {
        return cur[j].note;
      }
    }
  }
  return static_cast<uint8_t>(kDefaultNote);
}

// A lane's AUTO values, from the block (the rows and the first note in it) as it is known now.
void resolve_lane(State* s, uint32_t l) {
  Lane& a = s->lane[l];
  if ((a.autos & kAutoLength) != 0u) {
    a.length = static_cast<uint8_t>(default_length(s));
  }
  if (a.length < 1u) {
    a.length = 1;
  }
  if (a.length > kMaxLength) {
    a.length = static_cast<uint8_t>(kMaxLength);
  }
  if ((a.autos & kAutoHits) != 0u) {
    a.hits = static_cast<uint8_t>(default_hits(a.length));
  }
  if ((a.autos & kAutoNote) != 0u) {
    a.note = s->lane_default_note[l];
  }
  if (a.note < O3M_NOTE_FIRST || a.note > O3M_NOTE_LAST) {
    a.note = static_cast<uint8_t>(kDefaultNote);
  }
  if (a.hits > a.length) {
    a.hits = a.length;
  }
  if (a.rotate >= a.length) {
    a.rotate = static_cast<uint8_t>(a.length - 1u);
  }
  if (a.prob > 100u) {
    a.prob = 100;
  }
}

// The 24 declared rows (lane l row r at 5 l + r; INSTR, VOL, MODE, SEED at 20..23) come back from the OS before enter(): the project's kept values, else the manifest's defaults. HITS, LENGTH, NOTE and
// INSTR have an AUTO, written 0 (HITS is kept as hits + 1, so that 0 hits and AUTO differ).
void set_param(void* raw, uint32_t index, float v) {
  State* s = static_cast<State*>(raw);
  const uint32_t n = v > 0.0f ? static_cast<uint32_t>(v + 0.5f) : 0u;
  if (index < kMaxLanes * kLaneRows) {
    Lane& a = s->lane[index / kLaneRows];
    switch (index % kLaneRows) {
      case kHits:
        a.autos = n == 0u ? static_cast<uint8_t>(a.autos | kAutoHits) : static_cast<uint8_t>(a.autos & ~kAutoHits);
        a.hits = static_cast<uint8_t>(n == 0u ? 0u : (n - 1u < kMaxLength ? n - 1u : kMaxLength));
        break;
      case kLength:
        a.autos = n == 0u ? static_cast<uint8_t>(a.autos | kAutoLength) : static_cast<uint8_t>(a.autos & ~kAutoLength);
        a.length = static_cast<uint8_t>(n == 0u ? 1u : (n < kMaxLength ? n : kMaxLength));
        break;
      case kRotate: a.rotate = static_cast<uint8_t>(n < kMaxLength - 1u ? n : kMaxLength - 1u); break;
      case kNote:
        a.autos = n == 0u ? static_cast<uint8_t>(a.autos | kAutoNote) : static_cast<uint8_t>(a.autos & ~kAutoNote);
        a.note = static_cast<uint8_t>(n == 0u ? kDefaultNote : (n < O3M_NOTE_FIRST ? O3M_NOTE_FIRST : (n > O3M_NOTE_LAST ? O3M_NOTE_LAST : n)));
        break;
      default: a.prob = static_cast<uint8_t>(n < 100u ? n : 100u); break;
    }
    return;
  }
  switch (index - kMaxLanes * kLaneRows) {
    case kInstr:
      s->instr_auto = n == 0u ? 1u : 0u;
      s->instr = n == 0u ? 1u : (n < O3M_INSTR_MAX ? n : O3M_INSTR_MAX);
      break;
    case kVol: s->vol_ui = n < 128u ? n : 128u; break;
    case kMode: s->replace = n != 0u ? 1u : 0u; break;
    case kSeed: s->seed = n < kMaxSeed ? n : kMaxSeed; break;
    default: break;
  }
}

// The declared value of the row under the cursor, as it is kept.
float row_to_keep(const State* s) {
  if (s->page < kMaxLanes) {
    const Lane& a = s->lane[s->page];
    switch (s->row) {
      case kHits: return (a.autos & kAutoHits) != 0u ? 0.0f : static_cast<float>(a.hits + 1u);
      case kLength: return (a.autos & kAutoLength) != 0u ? 0.0f : static_cast<float>(a.length);
      case kRotate: return static_cast<float>(a.rotate);
      case kNote: return (a.autos & kAutoNote) != 0u ? 0.0f : static_cast<float>(a.note);
      default: return static_cast<float>(a.prob);
    }
  }
  switch (s->row) {
    case kInstr: return s->instr_auto != 0u ? 0.0f : static_cast<float>(s->instr);
    case kVol: return static_cast<float>(s->vol_ui);
    case kMode: return static_cast<float>(s->replace);
    default: return static_cast<float>(s->seed);
  }
}

void keep_row(State* s) {
  if (s->page >= kMaxLanes && s->row >= kSharedDeclared) {
    return;  // (the target rows are not kept: a relaunch starts at the cursor)
  }
  const uint32_t index = s->page < kMaxLanes ? s->page * kLaneRows + s->row : kMaxLanes * kLaneRows + s->row;
  own_set(s->io, index, row_to_keep(s));
}

int init(void* raw, const o3m_host* host) {
  State* s = static_cast<State*>(raw);
  if (host->draw == nullptr || !O3M_HOST_HAS(host, read) || host->read == nullptr || !O3M_TABLE_HAS(host->read, get_steps) || host->read->get_steps == nullptr ||
      host->read->pattern_info == nullptr || host->read->launch_ctx == nullptr) {
    return 1;  // an OS below 1.5, or an app without read_project: nothing to generate into
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

// The lanes and their AUTO values from the target as it is now (the launch, or a target row the user moved): LENGTH follows the rows, NOTE the first note in the lane's rows.
void retarget(State* s) {
  s->total_lanes = s->tgt.from_tools != 0u ? s->tgt.tracks : 0u;
  s->lanes = s->total_lanes < kMaxLanes ? s->total_lanes : kMaxLanes;
  for (uint32_t l = 0; l < kMaxLanes; ++l) {
    s->lane_default_note[l] = l < s->lanes && s->tgt.have_pattern != 0u ? first_note_of(s, l) : static_cast<uint8_t>(kDefaultNote);
    resolve_lane(s, l);
  }
}

void enter(void* raw) {
  State* s = static_cast<State*>(raw);
  (void)read_target(s->read, &s->tgt, kNoBlockRows);
  s->page = s->row = 0;
  s->msg = s->tgt.from_tools == 0u ? kMsgNoContext : (s->tgt.have_pattern == 0u ? kMsgNoPattern : kMsgNone);
  s->aud_on = s->aud_held = 0;
  s->ctx_instr = s->tgt.instrument;
  if (s->instr_auto != 0u) {
    s->instr = s->ctx_instr;
  }
  retarget(s);
}

// ---- APPLY -------------------------------------------------------------------------------------------------------------------------------------

// One lane's steps, from what is there. Returns the rows written (0: the lane has none).
uint32_t write_lane(State* s, uint32_t l, o3m_step* cur, o3m_step* out) {
  uint32_t first = 0;
  const uint32_t n = lane_rows(s, l, &first);
  const uint8_t track = static_cast<uint8_t>(s->tgt.first_track + l);
  if (n == 0u || s->read->get_steps(static_cast<uint16_t>(s->tgt.pattern), track, static_cast<uint8_t>(first), cur, static_cast<uint8_t>(n)) != O3M_EDIT_OK) {
    return 0;
  }
  LaneSetup setup;
  setup.hits = s->lane[l].hits;
  setup.length = s->lane[l].length;
  setup.rotate = s->lane[l].rotate;
  setup.note = s->lane[l].note;
  setup.prob = s->lane[l].prob;
  setup.instr = s->instr;
  setup.vol = s->vol_ui != 0u ? s->vol_ui - 1u : 255u;
  setup.replace = s->replace != 0u;
  setup.seed = s->seed;
  setup.lane = l;
  (void)lane_steps(setup, cur, out, n);
  if (s->edit->set_steps(static_cast<uint16_t>(s->tgt.pattern), track, static_cast<uint8_t>(first), out, static_cast<uint8_t>(n), O3M_STEP_NOTE | O3M_STEP_INSTR | O3M_STEP_VOL) != O3M_EDIT_OK) {
    return 0;
  }
  return n;
}

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
  o3m_step cur[O3M_PATTERN_ROWS_MAX];
  o3m_step out[O3M_PATTERN_ROWS_MAX];
  const int32_t b = s->edit->edit_begin();
  if (b != O3M_EDIT_OK) {
    s->msg = kMsgFailed;
    s->msg_code = b;
    return;
  }
  uint32_t written = 0;
  for (uint32_t l = 0; l < s->lanes; ++l) {
    written += write_lane(s, l, cur, out) != 0u ? 1u : 0u;
  }
  const int32_t e = s->edit->edit_end();
  s->msg_code = e;
  s->msg_lanes = written;
  s->msg = e != O3M_EDIT_OK ? kMsgFailed : (written != 0u ? kMsgApplied : kMsgNothing);
}

// ---- editing -----------------------------------------------------------------------------------------------------------------------------------

uint32_t page_rows(const State* s) { return s->page < kMaxLanes ? kLaneRows : kSharedRows; }

int32_t clamp_i(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

void fix_lane(Lane& a) {
  if (a.length < 1u) {
    a.length = 1;
  }
  if (a.length > kMaxLength) {
    a.length = static_cast<uint8_t>(kMaxLength);
  }
  if (a.hits > a.length) {
    a.hits = a.length;
  }
  if (a.rotate >= a.length) {
    a.rotate = static_cast<uint8_t>(a.length - 1u);
  }
}

// The row under the cursor moves by `d` detents (d == 0 with `reset`: its default).
void edit_value(State* s, int32_t d, bool reset) {
  if (s->page < kMaxLanes) {
    if (s->page >= s->lanes) {
      return;  // a lane with no track
    }
    Lane& a = s->lane[s->page];
    switch (s->row) {
      case kHits:
        a.autos = reset ? static_cast<uint8_t>(a.autos | kAutoHits) : static_cast<uint8_t>(a.autos & ~kAutoHits);
        a.hits = reset ? static_cast<uint8_t>(default_hits(a.length)) : static_cast<uint8_t>(clamp_i(static_cast<int32_t>(a.hits) + d, 0, a.length));
        break;
      case kLength:
        a.autos = reset ? static_cast<uint8_t>(a.autos | kAutoLength) : static_cast<uint8_t>(a.autos & ~kAutoLength);
        a.length = reset ? static_cast<uint8_t>(default_length(s)) : static_cast<uint8_t>(clamp_i(static_cast<int32_t>(a.length) + d, 1, static_cast<int32_t>(kMaxLength)));
        break;
      case kRotate:
        a.rotate = reset ? 0u : static_cast<uint8_t>(clamp_i(static_cast<int32_t>(a.rotate) + d, 0, a.length - 1));
        break;
      case kNote:
        a.autos = reset ? static_cast<uint8_t>(a.autos | kAutoNote) : static_cast<uint8_t>(a.autos & ~kAutoNote);
        a.note = reset ? s->lane_default_note[s->page] : static_cast<uint8_t>(clamp_i(static_cast<int32_t>(a.note) + d, O3M_NOTE_FIRST, O3M_NOTE_LAST));
        break;
      default:
        a.prob = reset ? 100u : static_cast<uint8_t>(clamp_i(static_cast<int32_t>(a.prob) + 5 * d, 0, 100));
        break;
    }
    fix_lane(a);
    return;
  }
  if (s->row >= kSharedDeclared) {
    edit_target(s->read, &s->tgt, s->row - kSharedDeclared, d, reset);
    retarget(s);
    return;
  }
  switch (s->row) {
    case kInstr:
      s->instr_auto = reset ? 1u : 0u;
      s->instr = reset ? s->ctx_instr : static_cast<uint32_t>(clamp_i(static_cast<int32_t>(s->instr) + d, 1, static_cast<int32_t>(O3M_INSTR_MAX)));
      break;
    case kVol:
      s->vol_ui = reset ? 0u : static_cast<uint32_t>(clamp_i(static_cast<int32_t>(s->vol_ui) + d, 0, 128));
      break;
    case kMode:
      s->replace = reset ? 1u : (d != 0 ? (d > 0 ? 1u : 0u) : s->replace);
      break;
    default:
      s->seed = reset ? 0u : static_cast<uint32_t>(clamp_i(static_cast<int32_t>(s->seed) + d, 0, static_cast<int32_t>(kMaxSeed)));
      break;
  }
}

// ---- the audition (OS-APPUX S3, play_note) ----------------------------------------------------------------------------------------------------------

// Turning the encoder on a lane plays that lane's hits once through, the first 16 steps at eight a second, on the instrument INSTR names: what the lane would write, to hear. The page is the app's clock (it is
// drawn 30 times a second): each frame sounds the next step when it is due, one note at a time. Without play_notes (an OS or a file that does not grant it) there is no sound and nothing else changes.
constexpr uint32_t kAuditionSteps = 16, kAuditionStepMs = 125, kAuditionVelocity = 100;

bool can_play(const State* s) { return s->io != nullptr && O3M_TABLE_HAS(s->io, stop_note) && s->io->play_note != nullptr && s->io->stop_note != nullptr && s->io->now_ms != nullptr; }

void audition_hush(State* s) {
  if (s->aud_held != 0u && can_play(s)) {
    (void)s->io->stop_note(s->aud_instr);
  }
  s->aud_held = 0;
}

void audition_start(State* s) {
  if (!can_play(s) || s->page >= s->lanes || s->tgt.have_pattern == 0u) {
    return;
  }
  audition_hush(s);
  s->aud_on = 1;
  s->aud_lane = s->page;
  s->aud_pos = 0;
  s->aud_next = s->io->now_ms();
}

void audition_step(State* s) {
  if (s->aud_on == 0u || !can_play(s)) {
    return;
  }
  const uint32_t now = s->io->now_ms();
  if (static_cast<int32_t>(now - s->aud_next) < 0) {
    return;
  }
  audition_hush(s);
  const Lane& a = s->lane[s->aud_lane];
  if (s->aud_pos >= a.length || s->aud_pos >= kAuditionSteps) {
    s->aud_on = 0;
    return;
  }
  if (cycle_hit(a.hits, a.length, a.rotate, s->aud_pos) && s->io->play_note(s->instr, a.note - 1u, kAuditionVelocity) == O3M_PLAY_OK) {
    s->aud_held = 1;
    s->aud_instr = s->instr;
  }
  ++s->aud_pos;
  s->aud_next = now + kAuditionStepMs;
}

// The coarse step of each row under Shift + the encoder (OS-APPUX S2): a lane's HITS, LENGTH and ROTATE by four, NOTE by an octave, PROB by four of its steps of 5; INSTR by eight, VOL by sixteen, SEED by hundreds.
constexpr int32_t kLaneCoarse[kLaneRows] = {4, 4, 4, 12, 4};
constexpr int32_t kSharedCoarse[kSharedDeclared] = {8, 16, 1, 100};  // (the target rows: target_coarse)

void input(void* raw, const o3m_ui_event* ev) {
  State* s = static_cast<State*>(raw);
  const Input in = grammar(ev, &s->row, page_rows(s), &s->page, kPages);
  if (in.move == o3edit::kMoveValue) {
    edit_value(s, steps(in, s->page < kMaxLanes ? kLaneCoarse[s->row] : (s->row < kSharedDeclared ? kSharedCoarse[s->row] : target_coarse(s->row - kSharedDeclared))), false);
    keep_row(s);
    audition_start(s);
    s->msg = kMsgNone;
    return;
  }
  if (ev->kind != O3M_EV_BUTTON || ev->value == 0) {
    return;
  }
  s->msg = kMsgNone;
  if (in.move == o3edit::kMovePage) {
    s->row = s->row < page_rows(s) ? s->row : page_rows(s) - 1u;  // (the new page may have fewer rows)
    return;
  }
  if (in.move == o3edit::kMoveDefault) {
    edit_value(s, 0, true);
    keep_row(s);
    audition_start(s);
    return;
  }
  switch (ev->id) {
    case O3M_BTN_F1:
      apply(s);
      break;
    case O3M_BTN_F2:
      s->seed = s->seed < kMaxSeed ? s->seed + 1u : 0u;
      own_set(s->io, kMaxLanes * kLaneRows + kSeed, static_cast<float>(s->seed));
      apply(s);
      break;
    default:
      break;
  }
}

// ---- the page ----------------------------------------------------------------------------------------------------------------------------------

uint32_t put_text(char* out, uint32_t at, const char* text) {
  while (*text != '\0') {
    out[at++] = *text++;
  }
  return at;
}

uint32_t put_uint(char* out, uint32_t at, uint32_t v, uint32_t min_digits) {
  char digits[10];
  uint32_t n = 0;
  do {
    digits[n++] = static_cast<char>('0' + v % 10u);
    v /= 10u;
  } while (v != 0u && n < 10u);
  while (n < min_digits) {
    digits[n++] = '0';
  }
  while (n != 0u) {
    out[at++] = digits[--n];
  }
  return at;
}

uint32_t put_note(char* out, uint32_t at, uint32_t step_note) {
  static const char* const kNames[12] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};
  const uint32_t n = step_note - 1u;  // MIDI note 0..127; C-0 = 0 as the Pattern's tile names it (the MIDDLE C setting is ignored)
  at = put_text(out, at, kNames[n % 12u]);
  return put_uint(out, at, n / 12u, 1);
}

const char* const kLaneNames[kLaneRows] = {"HITS", "LENGTH", "ROTATE", "NOTE", "PROB"};
const char* const kSharedNames[kSharedDeclared] = {"INSTR", "VOL", "MODE", "SEED"};

// The text of one row's value.
void value_text(const State* s, uint32_t row, char* out) {
  uint32_t n = 0;
  if (s->page < kMaxLanes) {
    const Lane& a = s->lane[s->page];
    switch (row) {
      case kHits: n = put_uint(out, 0, a.hits, 1); break;
      case kLength: n = put_uint(out, 0, a.length, 1); break;
      case kRotate: n = put_uint(out, 0, a.rotate, 1); break;
      case kNote: n = put_note(out, 0, a.note); break;
      default:
        n = put_uint(out, 0, a.prob, 1);
        n = put_text(out, n, " %");
        break;
    }
  } else {
    switch (row) {
      case kInstr: n = put_uint(out, 0, s->instr, 2); break;
      case kVol:
        if (s->vol_ui == 0u) {
          n = put_text(out, 0, "UNSET");
        } else {
          n = put_uint(out, 0, s->vol_ui - 1u, 1);
        }
        break;
      case kMode: n = put_text(out, 0, s->replace != 0u ? "REPLACE" : "ADD"); break;
      default: n = put_uint(out, 0, s->seed, 4); break;
    }
  }
  out[n] = '\0';
}

void view_draw(void* raw, uint32_t, o3m_canvas* c) {
  State* s = static_cast<State*>(raw);
  audition_step(s);
  const o3m_draw_api* api = c->draw;
  const int32_t w = c->width;
  int32_t pitch = c->height / 12;
  pitch = pitch < 20 ? 20 : pitch;
  char text[72];
  api->rect(c, 0, 0, w, c->height, O3M_COLOR_PANE);

  // title and what the block is
  uint32_t n = put_text(text, 0, "RHYTHM GEN");
  if (s->tgt.from_tools != 0u && s->tgt.have_pattern != 0u) {
    n = put_text(text, n, "  PAT ");
    n = put_uint(text, n, s->tgt.pattern, 2);
    n = put_text(text, n, "  LANES ");
    n = put_uint(text, n, s->lanes, 1);
    if (s->total_lanes > s->lanes) {
      n = put_text(text, n, " OF ");
      n = put_uint(text, n, s->total_lanes, 1);
    }
  }
  text[n] = '\0';
  api->text(c, 8, 2, text, O3M_FACE_DATA, O3M_COLOR_TEXT);
  if (s->tgt.from_tools != 0u && s->tgt.have_pattern != 0u) {
    n = put_text(text, 0, s->tgt.has_block != 0u ? "BLOCK  TRACK " : "AT THE CURSOR  TRACK ");  // (the launch's word; the track and rows are what exists of the target now)
    n = put_uint(text, n, s->tgt.first_track + 1u, 1);
    if (s->lanes > 1u) {
      n = put_text(text, n, "-");
      n = put_uint(text, n, s->tgt.first_track + s->lanes, 1);
    }
    n = put_text(text, n, "  ROWS ");
    uint32_t first = 0;
    const uint32_t cnt = lane_rows(s, 0, &first);  // (cut at the first lane's track end)
    n = put_uint(text, n, first + 1u, 1);  // (the Pattern counts steps from 1)
    n = put_text(text, n, "-");
    n = put_uint(text, n, first + (cnt != 0u ? cnt : 1u), 1);
    text[n] = '\0';
    api->text(c, 8, pitch + 2, text, O3M_FACE_DATA, O3M_COLOR_TEXT_DIM);
  }

  // the pages: 1 2 3 4 SHARED, the current one in the accent colour (a lane with no track is dim)
  const int32_t cell = api->text_width(c, "0", O3M_FACE_DATA);
  for (uint32_t p = 0; p < kPages; ++p) {
    const char* label = "SHARED";
    char one[2] = {static_cast<char>('1' + p), '\0'};
    if (p < kMaxLanes) {
      label = one;
    }
    const int32_t x = 8 + static_cast<int32_t>(p < kMaxLanes ? p * 3u : 12u) * cell;
    api->text(c, x, 2 * pitch + 2, label, O3M_FACE_DATA, p == s->page ? O3M_COLOR_ACCENT : (p < kMaxLanes && p >= s->lanes ? O3M_COLOR_EMPTY : O3M_COLOR_TEXT));
  }

  // the rows of the page
  const uint32_t rows = page_rows(s);
  const bool dead = s->page < kMaxLanes && s->page >= s->lanes;
  for (uint32_t r = 0; r < rows; ++r) {
    const int32_t y = static_cast<int32_t>(3u + r) * pitch;
    const bool sel = r == s->row;
    if (sel) {
      api->rect(c, 4, y, w / 2, pitch - 2, O3M_COLOR_SEL);
    }
    o3edit::PageRow tr;  // a target row of the shared page (o3m_edit.h): its name, value and dim come from the helper
    const bool target = s->page >= kMaxLanes && r >= kSharedDeclared;
    if (target) {
      fill_target_row(&tr, &s->tgt, r - kSharedDeclared);
    }
    const bool dim = dead || (target && tr.dim);
    const uint32_t ink = sel ? O3M_COLOR_SEL_TEXT : (dim ? O3M_COLOR_EMPTY : O3M_COLOR_TEXT);
    api->text(c, 8, y + 2, target ? tr.name : (s->page < kMaxLanes ? kLaneNames[r] : kSharedNames[r]), O3M_FACE_DATA, ink);
    if (!target) {
      value_text(s, r, text);
    }
    const char* value = target ? tr.value : text;
    api->text(c, w / 2 - 4 - api->text_width(c, value, O3M_FACE_DATA), y + 2, value, O3M_FACE_DATA, sel ? O3M_COLOR_SEL_TEXT : (dim ? O3M_COLOR_EMPTY : O3M_COLOR_INSTR));
  }

  // the cycle of the lane on view, as it would hit (before the probability), and on the shared page one line for each lane
  if (s->page < kMaxLanes && !dead) {
    const Lane& a = s->lane[s->page];
    uint32_t len = a.length;
    for (uint32_t i = 0; i < len; ++i) {
      text[i] = cycle_hit(a.hits, a.length, a.rotate, i) ? 'X' : '.';
    }
    text[len] = '\0';
    api->text(c, 8, 8 * pitch + 2, text, O3M_FACE_DATA, O3M_COLOR_PLAY);
  } else if (s->page >= kMaxLanes) {
    for (uint32_t l = 0; l < s->lanes; ++l) {
      const Lane& a = s->lane[l];
      n = put_text(text, 0, "L");
      n = put_uint(text, n, l + 1u, 1);
      n = put_text(text, n, "  ");
      n = put_uint(text, n, a.hits, 1);
      n = put_text(text, n, "/");
      n = put_uint(text, n, a.length, 1);
      n = put_text(text, n, "  ROT ");
      n = put_uint(text, n, a.rotate, 1);
      n = put_text(text, n, "  ");
      n = put_note(text, n, a.note);
      n = put_text(text, n, "  ");
      n = put_uint(text, n, a.prob, 1);
      n = put_text(text, n, "%");
      text[n] = '\0';
      api->text(c, w / 2 + 8, static_cast<int32_t>(3u + l) * pitch + 2, text, O3M_FACE_LABEL, O3M_COLOR_TEXT_DIM);
    }
  }

  // the answer to the last key
  n = 0;
  uint32_t color = O3M_COLOR_ACCENT;
  switch (s->msg) {
    case kMsgApplied:
      n = put_uint(text, n, s->msg_lanes, 1);
      n = put_text(text, n, s->msg_lanes == 1u ? " LANE WRITTEN  (UNDO TO TAKE BACK)" : " LANES WRITTEN  (UNDO TO TAKE BACK)");
      color = O3M_COLOR_PLAY;
      break;
    case kMsgNothing: n = put_text(text, n, "NOTHING TO WRITE: THE BLOCK IS PAST THE TRACK END"); break;
    case kMsgFailed:
      n = put_text(text, n, "NOT APPLIED  (CODE ");
      n = put_uint(text, n, static_cast<uint32_t>(s->msg_code), 1);
      n = put_text(text, n, ")");
      break;
    case kMsgNoPattern: n = put_text(text, n, "THE PATTERN DOES NOT EXIST YET: ENTER A NOTE FIRST"); break;
    case kMsgNoContext: n = put_text(text, n, "OPEN IT FROM PATTERN F7 TOOLS"); break;
    case kMsgNoEdit: n = put_text(text, n, "THIS HOST CANNOT EDIT"); break;
    default: break;
  }
  text[n] = '\0';
  if (n != 0u) {
    api->text(c, 8, static_cast<int32_t>(s->page < kMaxLanes ? 9u : 3u + kSharedRows) * pitch + 2, text, O3M_FACE_LABEL, color);  // (under the rows: the shared page has eight)
  }
}

void leave(void*) {}
void shutdown(void*) {}

// OS-KEYROWDLG S5 (ABI 1.9): what the keys do, for the page's key row (labels only: the OS delivers the keys to input() whatever they say; F8 is the OS's BACK).
const o3m_keys kKeys = {{"APPLY", "VARY"}, {"APPLY", "VARY"}};  // (Shift + Fn runs Fn: no app reads `mods`, so the Shift row says the same)

const o3m_keys* ui_keys(void*) { return &kKeys; }

const o3m_ui kUi = {O3M_UI_SIZE(RHYTHMGEN_ABI_MINOR), enter, input, view_draw, leave, ui_keys};

}  // namespace

extern "C" O3M_EXPORT const o3m_module o3m_entry __attribute__((section(".o3m_entry"))) = {
    RHYTHMGEN_ENTRY_HEAD, sizeof(State), RHYTHMGEN_ENTRY_PARAMS, init, nullptr, set_param, nullptr, nullptr, shutdown, RHYTHMGEN_ENTRY_VIEWS(&kUi),
};
