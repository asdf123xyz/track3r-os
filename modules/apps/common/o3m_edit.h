// SPDX-License-Identifier: MIT
//
// OS-H3B: what the four editing apps (CHORDS, HUMANIZE, MUTATE, MORPH) share, in one header so that each module stays small. A module has no libc, so every loop is written out. Integer arithmetic
// except where a float parameter is the point (MUTATE, MORPH).
//   - text:     put_text / put_uint, the note names of the Pattern;
//   - random:   a seeded xorshift32 and a hash of (seed, key), so that the same settings always give the same result;
//   - the target: the block the app was launched on (pattern, tracks, rows) from the launch context and pattern_info, and its five rows (PATTERN, TRACK, ROW, ROWS, TRACKS) that move it inside the app (OS-APPUX S5);
//   - the grammar: the one set of keys every app page answers to (OS-APPUX S2), `grammar()` below;
//   - the page: one function that draws the app's rows (name, value, cursor), a line of facts and the answer to the last key (the key row names the F-keys).
#pragma once

#include <o3module.h>

#include <stdint.h>

namespace o3edit {

// ---- text ---------------------------------------------------------------------------------------------------------------------------------------

inline uint32_t put_text(char* out, uint32_t at, const char* text) {
  while (*text != '\0') {
    out[at++] = *text++;
  }
  return at;
}

inline uint32_t put_uint(char* out, uint32_t at, uint32_t v, uint32_t min_digits) {
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

// A float as whole.tenths ("12.5", "-0.3"), for a value shown beside its range. Tenths are truncated toward zero; |v| < 1e9.
inline uint32_t put_float1(char* out, uint32_t at, float v) {
  if (v < 0.0f) {
    out[at++] = '-';
    v = -v;
  }
  const uint32_t whole = static_cast<uint32_t>(v);
  const uint32_t tenth = static_cast<uint32_t>((v - static_cast<float>(whole)) * 10.0f);
  at = put_uint(out, at, whole, 1);
  out[at++] = '.';
  out[at++] = static_cast<char>('0' + (tenth > 9u ? 9u : tenth));
  return at;
}

// A step byte's note as the Pattern's tile names it: 1 is C-0, 49 is C-4 (the MIDDLE C setting only renames the notes on screen).
inline uint32_t put_note(char* out, uint32_t at, uint32_t step_note) {
  static const char* const kNames[12] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};
  const uint32_t n = step_note - 1u;
  at = put_text(out, at, kNames[n % 12u]);
  return put_uint(out, at, n / 12u, 1);
}

inline int32_t clamp_i(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---- random -------------------------------------------------------------------------------------------------------------------------------------

// A 32-bit integer hash (lowbias32): neighbouring inputs give unrelated outputs.
inline uint32_t mix32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

// The state from the user's seed (0..9999) and a stream number (a track, a parameter id).
struct Draw {
  uint32_t s;
  void seed(uint32_t user_seed, uint32_t stream) {
    const uint32_t x = mix32(user_seed * 2654435761u + (stream + 1u) * 0x9E3779B9u);
    s = x != 0u ? x : 0x6D2B79F5u;
  }
  uint32_t next() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  }
};

// A uniform number in [0, 1) from (seed, key), 24 bits: the same for the same pair whatever else was drawn.
inline float unit_of(uint32_t user_seed, uint32_t key) { return static_cast<float>(mix32(user_seed * 2654435761u + (key + 1u) * 0x9E3779B9u) >> 8) * (1.0f / 16777216.0f); }

// ---- the target ---------------------------------------------------------------------------------------------------------------------------------

constexpr uint32_t kMaxSeed = 9999;

struct Target {
  uint32_t from_tools;    // the launch context is a selection (TOOLS), not the app list
  uint32_t have_pattern;  // pattern_info answered
  uint32_t has_block;
  uint32_t pattern;       // the pattern id
  uint32_t first_track;   // the block's first track (the cursor's with no block)
  uint32_t tracks;        // tracks the block covers (1 with no block)
  uint32_t first_row;     // the block's first row (the cursor's with no block)
  uint32_t rows;          // the block's rows; with no block `no_block_rows`, or to the end of the first track when that is 0
  uint32_t instrument;    // 1..64, the context's
  uint32_t home_pattern, home_track, home_row, home_rows, home_tracks;  // the five fields as the launch left them: what Delete puts back (OS-APPUX S5)
  o3m_pattern_info info;
};

// Reads the launch context and the pattern. False: not launched from TOOLS, or the pattern does not exist (have_pattern says which).
inline bool read_launch_target(const o3m_read_api* read, Target* t, uint32_t no_block_rows) {
  o3m_launch_ctx lc;
  lc.size = sizeof lc;
  lc.flags = lc.pattern = lc.track = lc.instrument = lc.block_start = lc.block_length = 0;
  lc.block_tracks = 0;
  lc.reserved[0] = lc.reserved[1] = lc.reserved[2] = 0;
  t->from_tools = read->launch_ctx(&lc);
  t->have_pattern = 0;
  t->has_block = 0;
  t->pattern = lc.pattern;
  t->first_track = lc.track;
  t->first_row = lc.block_start;
  t->tracks = 1;
  t->rows = 0;
  t->instrument = lc.instrument + 1u <= O3M_INSTR_MAX ? lc.instrument + 1u : O3M_INSTR_MAX;
  t->has_block = lc.block_length != 0u && lc.block_tracks != 0u ? 1u : 0u;
  if (t->has_block != 0u) {  // (known from the context alone: a block's rows and tracks stay when the pattern is missing)
    t->tracks = lc.block_tracks;
    t->rows = lc.block_length < O3M_PATTERN_ROWS_MAX ? lc.block_length : O3M_PATTERN_ROWS_MAX;
  } else {
    t->rows = no_block_rows;
  }
  if (t->from_tools == 0u || lc.pattern < 1u || lc.pattern > 255u) {
    return false;
  }
  t->info.size = sizeof t->info;
  if (read->pattern_info(static_cast<uint16_t>(t->pattern), &t->info) != O3M_EDIT_OK) {
    return false;
  }
  t->have_pattern = 1;
  if (t->has_block == 0u) {
    if (t->first_row >= t->info.length) {
      t->first_row = 0;
    }
    const uint32_t len = t->first_track < O3M_PATTERN_TRACKS_MAX ? t->info.track_length[t->first_track] : 0u;
    t->rows = no_block_rows != 0u ? no_block_rows : (len > t->first_row ? len - t->first_row : 0u);
  }
  return true;
}

// The launch target, with the values the target rows' Delete returns to (OS-APPUX S5).
inline bool read_target(const o3m_read_api* read, Target* t, uint32_t no_block_rows) {
  const bool ok = read_launch_target(read, t, no_block_rows);
  t->home_pattern = t->pattern;
  t->home_track = t->first_track;
  t->home_row = t->first_row;
  t->home_rows = t->rows;
  t->home_tracks = t->tracks;
  return ok;
}

// The rows of the target's track number `l` (0 = the first) that exist: the block's rows cut at the track's own length. 0: the track is not in the pattern or the block lies past its end.
inline uint32_t track_rows(const Target* t, uint32_t l) {
  const uint32_t track = t->first_track + l;
  if (!t->have_pattern || l >= t->tracks || track >= t->info.tracks || track >= O3M_PATTERN_TRACKS_MAX) {
    return 0;
  }
  const uint32_t len = t->info.track_length[track];
  if (t->first_row >= len) {
    return 0;
  }
  const uint32_t room = len - t->first_row;
  const uint32_t n = t->rows < room ? t->rows : room;
  return n < O3M_PATTERN_ROWS_MAX ? n : O3M_PATTERN_ROWS_MAX;
}

// ---- instrument parameters (ABI 1.6) -------------------------------------------------------------------------------------------------------------

// One writable parameter, as MUTATE and MORPH keep it: 12 bytes. `whole`: STEPPED or ENUM (a value between two steps means nothing).
struct PD {
  float min, max;
  uint16_t id;
  uint8_t group, whole;
};
constexpr uint32_t kMaxPD = O3M_SAMPLE_PARAM_COUNT;  // a SAMPLE has the most (83); a MODULE at most O3M_PARAMS_MAX

// Whether the OS can say what an instrument is and read its parameters (an OS of 1.6 and an app with read_project).
inline bool have_params(const o3m_read_api* read) {
  return O3M_TABLE_HAS(read, get_instrument_param) && read->instrument_info != nullptr && read->get_param_desc != nullptr && read->get_instrument_param != nullptr;
}
inline bool have_param_write(const o3m_edit_api* edit) { return edit != nullptr && O3M_TABLE_HAS(edit, set_instrument_param) && edit->set_instrument_param != nullptr; }

// The kind and engine of an instrument (1..64), and its writable parameters in the OS's order into `out` (at most `cap`). Returns how many. *kind is O3M_INSTRUMENT_*.
inline uint32_t load_writable(const o3m_read_api* read, uint32_t instrument, PD* out, uint32_t cap, uint32_t* kind, uint32_t* engine) {
  o3m_instrument_info info;
  info.size = sizeof info;
  info.kind = O3M_INSTRUMENT_EMPTY;
  info.engine = 0;
  info.param_count = 0;
  *kind = O3M_INSTRUMENT_EMPTY;
  *engine = 0;
  if (read->instrument_info(static_cast<uint8_t>(instrument), &info) != O3M_EDIT_OK) {
    return 0;
  }
  *kind = info.kind;
  *engine = info.engine;
  if (info.kind != O3M_INSTRUMENT_SAMPLE && info.kind != O3M_INSTRUMENT_MODULE) {
    return 0;
  }
  uint32_t n = 0;
  for (uint32_t i = 0; i < info.param_count && i < 0xFFFFu; ++i) {
    o3m_param_desc d;
    d.size = sizeof d;
    if (read->get_param_desc(static_cast<uint8_t>(instrument), static_cast<uint16_t>(i), &d) != O3M_EDIT_OK || (d.flags & O3M_PD_APP_WRITABLE) == 0u || n >= cap) {
      continue;
    }
    out[n].min = d.min;
    out[n].max = d.max;
    out[n].id = d.id;
    out[n].group = d.group;
    out[n].whole = (d.flags & (O3M_PD_STEPPED | O3M_PD_ENUM)) != 0u ? 1u : 0u;
    ++n;
  }
  return n;
}

// The groups that hold a writable parameter, ascending, into `out` (at most `cap`); returns how many.
inline uint32_t list_groups(const PD* pd, uint32_t n, uint8_t* out, uint32_t cap) {
  uint32_t count = 0;
  for (uint32_t g = 0; g < 256u && count < cap; ++g) {
    for (uint32_t i = 0; i < n; ++i) {
      if (pd[i].group == g) {
        out[count++] = static_cast<uint8_t>(g);
        break;
      }
    }
  }
  return count;
}

// The name of group `g` of an instrument of `kind` ("VOICE" ... for a SAMPLE, "PAGE 1" ... for a MODULE), into out; returns the length.
inline uint32_t put_group_name(char* out, uint32_t at, uint32_t kind, uint32_t g) {
  static const char* const kSample[6] = {"VOICE", "FILTER", "ENV", "SENDS", "SOURCE", "MOD"};
  if (kind == O3M_INSTRUMENT_SAMPLE) {
    return put_text(out, at, g < 6u ? kSample[g] : "?");
  }
  at = put_text(out, at, "PAGE ");
  return put_uint(out, at, g + 1u, 1);
}

// Rounds to the nearest whole number, halves away from zero (a module has no libc).
inline float round_whole(float x) { return static_cast<float>(static_cast<int32_t>(x + (x < 0.0f ? -0.5f : 0.5f))); }

// ---- the grammar --------------------------------------------------------------------------------------------------------------------------------

// OS-APPUX S2 (docs/specs/OS-APPUX.md 3.2): the keys every app page answers to, so that the user learns them once. The OS puts no cursor over an app's fields; the app moves its own, with these keys.
//   Up / Down       the previous / next row (stops at the ends, no wrap)
//   Left / Right    the previous / next page (an app with one page ignores them)
//   encoder         the selected row's value, one step per detent
//   Shift + encoder the same in coarse steps (a row without a coarse step passes 1 and reads as the plain encoder)
//   Delete          the selected row's default
// The F-keys are the app's own actions (the key row names them) and stay in its input(). Nothing else changes a value: Shift + an arrow or Delete, Copy, Insert and the pads answer `kMoveNone`.
enum Move : uint32_t { kMoveNone, kMoveRow, kMovePage, kMoveValue, kMoveDefault };

struct Input {
  uint32_t move;    // Move
  int32_t detents;  // kMoveValue: the encoder's detents, signed, at most kMaxDetents either way
  bool coarse;      // kMoveValue: Shift was held
};
constexpr int32_t kMaxDetents = 1 << 20;  // (a turn of any size runs a value into its end; this only keeps the sums below 2^31)

// Reads one event. Up / Down and Left / Right have moved `*row` (of `rows`, the current page's) and `*page` (of `pages`; `page` may be null with one page) when the answer is kMoveRow or kMovePage; the app
// then keeps its row inside the new page's rows. kMoveValue and kMoveDefault leave it to the app to change the value of the row under `*row`: `steps()` gives the amount.
inline Input grammar(const o3m_ui_event* ev, uint32_t* row, uint32_t rows, uint32_t* page, uint32_t pages) {
  Input in = {kMoveNone, 0, false};
  if (rows != 0u && *row >= rows) {
    *row = rows - 1u;
  }
  if (ev->kind == O3M_EV_ENCODER) {
    if (ev->value != 0) {
      in.move = kMoveValue;
      in.detents = clamp_i(ev->value, -kMaxDetents, kMaxDetents);
      in.coarse = (ev->mods & O3M_MOD_SHIFT) != 0u;
    }
    return in;
  }
  if (ev->kind != O3M_EV_BUTTON || ev->value == 0 || (ev->mods & O3M_MOD_SHIFT) != 0u) {
    return in;
  }
  switch (ev->id) {
    case O3M_BTN_UP:
      *row = *row != 0u ? *row - 1u : 0u;
      in.move = kMoveRow;
      break;
    case O3M_BTN_DOWN:
      *row = *row + 1u < rows ? *row + 1u : (rows != 0u ? rows - 1u : 0u);
      in.move = kMoveRow;
      break;
    case O3M_BTN_LEFT:
    case O3M_BTN_RIGHT:
      if (page != nullptr && pages > 1u) {
        *page = ev->id == O3M_BTN_LEFT ? (*page != 0u ? *page - 1u : 0u) : (*page + 1u < pages ? *page + 1u : pages - 1u);
        in.move = kMovePage;
      }
      break;
    case O3M_BTN_DELETE:
      in.move = kMoveDefault;
      break;
    default:
      break;
  }
  return in;
}

// A kMoveValue in the row's own units: the detents, times `coarse` while Shift was held (1: the row has no coarse step).
inline int32_t steps(const Input& in, int32_t coarse) { return in.coarse ? in.detents * coarse : in.detents; }

// ---- storage (OS-APPUX S3, ABI 1.10) -------------------------------------------------------------------------------------------------------------------

// The app keeps one of its declared rows with the project: the OS clamps and rounds the value to the row's descriptor, hands it back to set_param() at the next launch before enter(), and does NOT call set_param()
// for it now (the app holds the value it just stored). A table that is missing (an OS below 1.10) is not an error: the row is then only remembered while the app runs.
inline void own_set(const o3m_io_api* io, uint32_t index, float value) {
  if (io != nullptr && O3M_TABLE_HAS(io, set_own_param) && io->set_own_param != nullptr) {
    (void)io->set_own_param(index, value);
  }
}

// ---- the page -----------------------------------------------------------------------------------------------------------------------------------

constexpr uint32_t kMaxRows = 8;  // the rows of a page (UI map rule 2)

struct PageRow {
  const char* name;
  char value[28];
  bool dim;  // drawn in the empty colour (a row that does not apply)
};

// ---- the target rows (OS-APPUX S5) --------------------------------------------------------------------------------------------------------------

// The target as five ordinary rows of the app's page, in this order, in the grammar: the encoder moves the row (Shift + the encoder in coarse steps) and Delete puts back what the launch gave (the Pattern cursor's
// pattern, track and row, the block's rows and tracks, `Target::home_*`). They are not declared and not kept: a relaunch from TOOLS starts at the cursor again. With no launch context (the MODULES list) the rows are
// dim, read "-" and do not move. An app that does not use all five draws the first ones (Rhythm Gen has no TRACKS: its lanes are the block's tracks). The values are the user's: ROWS is the count asked for and the
// target covers what exists of it (`track_rows`); ROW and TRACK stay inside the pattern, PATTERN is any of 1..255 and reads (EMPTY) while it does not exist.
enum TargetRow : uint32_t { kTrPattern, kTrTrack, kTrRow, kTrRows, kTrTracks, kTargetRows };

inline int32_t target_coarse(uint32_t which) { return which == kTrPattern ? 10 : (which == kTrRow || which == kTrRows ? 16 : 4); }

// The row `which` (name, value as the page shows it, dim) into `row`.
inline void fill_target_row(PageRow* row, const Target* t, uint32_t which) {
  static const char* const kNames[kTargetRows] = {"PATTERN", "TRACK", "ROW", "ROWS", "TRACKS"};
  row->name = kNames[which < kTargetRows ? which : 0u];
  row->dim = t->from_tools == 0u;
  uint32_t v = 0;
  switch (which) {
    case kTrPattern: v = t->pattern; break;
    case kTrTrack: v = t->first_track + 1u; break;  // (the Pattern counts tracks and rows from 1)
    case kTrRow: v = t->first_row + 1u; break;
    case kTrRows: v = t->rows; break;
    default: v = t->tracks; break;
  }
  uint32_t n = 0;
  if (t->from_tools == 0u) {
    n = put_text(row->value, 0, "-");
  } else {
    n = put_uint(row->value, 0, v, 1);
    if (which == kTrPattern && t->have_pattern == 0u) {
      n = put_text(row->value, n, " (EMPTY)");
    }
  }
  row->value[n] = '\0';
}

// Moves row `which` by `d` (kMoveValue: steps(in, target_coarse(which))) or, with `reset`, back to the launch value; re-reads the pattern when it changed and keeps the rest inside it (a smaller pattern or
// another track can be shorter; that is one way: the start does not come back when the pattern does). A launch that asked for "to the end of the track" (`rows` 0) before the pattern was known gets it when the
// pattern appears. Does nothing with no launch context. The app then redoes what depends on the target (its AUTO values, the chords that fit).
inline void edit_target(const o3m_read_api* read, Target* t, uint32_t which, int32_t d, bool reset) {
  if (t->from_tools == 0u) {
    return;
  }
  int32_t width = t->have_pattern != 0u ? static_cast<int32_t>(t->info.tracks) : static_cast<int32_t>(O3M_PATTERN_TRACKS_MAX);
  width = width < 1 ? 1 : (width > static_cast<int32_t>(O3M_PATTERN_TRACKS_MAX) ? static_cast<int32_t>(O3M_PATTERN_TRACKS_MAX) : width);
  switch (which) {
    case kTrPattern:
      t->pattern = static_cast<uint32_t>(reset ? clamp_i(static_cast<int32_t>(t->home_pattern), 1, 255) : clamp_i(static_cast<int32_t>(t->pattern) + d, 1, 255));
      t->info.size = sizeof t->info;
      t->have_pattern = read->pattern_info(static_cast<uint16_t>(t->pattern), &t->info) == O3M_EDIT_OK ? 1u : 0u;
      break;
    case kTrTrack:
      t->first_track = static_cast<uint32_t>(reset ? static_cast<int32_t>(t->home_track) : clamp_i(static_cast<int32_t>(t->first_track) + d, 0, width - 1));
      break;
    case kTrRow: {
      const int32_t len = t->have_pattern != 0u && t->first_track < O3M_PATTERN_TRACKS_MAX ? static_cast<int32_t>(t->info.track_length[t->first_track]) : static_cast<int32_t>(O3M_PATTERN_ROWS_MAX);
      t->first_row = static_cast<uint32_t>(reset ? static_cast<int32_t>(t->home_row) : clamp_i(static_cast<int32_t>(t->first_row) + d, 0, len > 0 ? len - 1 : 0));
      break;
    }
    case kTrRows:
      t->rows = reset ? t->home_rows : static_cast<uint32_t>(clamp_i(static_cast<int32_t>(t->rows) + d, 1, static_cast<int32_t>(O3M_PATTERN_ROWS_MAX)));  // (a home of 0 is the launch's "to the end of the track", resolved below)
      break;
    default:
      t->tracks = static_cast<uint32_t>(clamp_i(reset ? static_cast<int32_t>(t->home_tracks) : static_cast<int32_t>(t->tracks) + d, 1, width));
      break;
  }
  int32_t tracks_now = t->have_pattern != 0u ? static_cast<int32_t>(t->info.tracks) : static_cast<int32_t>(O3M_PATTERN_TRACKS_MAX);
  tracks_now = tracks_now < 1 ? 1 : (tracks_now > static_cast<int32_t>(O3M_PATTERN_TRACKS_MAX) ? static_cast<int32_t>(O3M_PATTERN_TRACKS_MAX) : tracks_now);
  t->first_track = static_cast<uint32_t>(clamp_i(static_cast<int32_t>(t->first_track), 0, tracks_now - 1));
  t->tracks = static_cast<uint32_t>(clamp_i(static_cast<int32_t>(t->tracks), 1, tracks_now));
  const int32_t len_now = t->have_pattern != 0u ? static_cast<int32_t>(t->info.track_length[t->first_track]) : static_cast<int32_t>(O3M_PATTERN_ROWS_MAX);
  if (which != kTrRows && which != kTrTracks) {  // (ROWS and TRACKS leave the start alone: a block can start past its first track's end, where only a longer track of it holds rows)
    t->first_row = static_cast<uint32_t>(clamp_i(static_cast<int32_t>(t->first_row), 0, len_now > 0 ? len_now - 1 : 0));
  }
  if (t->have_pattern != 0u && (t->rows == 0u || t->home_rows == 0u)) {  // the launch asked for "to the end of the track" before the pattern was known (a missing pattern, no block): now it is
    const uint32_t room = len_now > static_cast<int32_t>(t->first_row) ? static_cast<uint32_t>(len_now) - t->first_row : 0u;
    const uint32_t end = room < O3M_PATTERN_ROWS_MAX ? room : O3M_PATTERN_ROWS_MAX;
    t->rows = t->rows == 0u ? end : t->rows;
    t->home_rows = t->home_rows == 0u ? end : t->home_rows;
  }
}

// The instrument row of Mutate and Morph (1..64): the same rule, with the instrument the launch gave as its default.
inline uint32_t step_instrument(uint32_t current, uint32_t home, int32_t d, bool reset) {
  return static_cast<uint32_t>(clamp_i(reset ? static_cast<int32_t>(home) : static_cast<int32_t>(current) + d, 1, static_cast<int32_t>(O3M_INSTR_MAX)));
}

// The page's layout is fixed, in pixels of the theme's faces (Data 10 x 20, Label 8 x 16): the title at 2, the facts at 24, the rows from 48 every 22 (eight rows end at 222), the answer under them.
// It holds from the smallest body (480 x 288) up; nothing leaves the canvas or overlaps another text.
constexpr int32_t kRowTop = 48;
constexpr int32_t kRowPitch = 22;

// The rows alone: `n` rows from (x, y), each kRowPitch high and `width` wide (the name at the left, the value at the right edge, the cursor row on a selection bar). An app that draws more than rows
// (a scope, a meter) puts them in a column of its own with this and keeps the rest of the canvas.
inline void draw_rows(o3m_canvas* c, int32_t x, int32_t y, int32_t width, const PageRow* rows, uint32_t n, uint32_t cursor) {
  const o3m_draw_api* api = c->draw;
  for (uint32_t r = 0; r < n && r < kMaxRows; ++r) {
    const int32_t ry = y + static_cast<int32_t>(r) * kRowPitch;
    const bool sel = r == cursor;
    if (sel) {
      api->rect(c, x + 4, ry, width, kRowPitch - 2, O3M_COLOR_SEL);
    }
    api->text(c, x + 8, ry + 1, rows[r].name, O3M_FACE_DATA, sel ? O3M_COLOR_SEL_TEXT : (rows[r].dim ? O3M_COLOR_EMPTY : O3M_COLOR_TEXT));
    api->text(c, x + width - 4 - api->text_width(c, rows[r].value, O3M_FACE_DATA), ry + 1, rows[r].value, O3M_FACE_DATA, sel ? O3M_COLOR_SEL_TEXT : (rows[r].dim ? O3M_COLOR_EMPTY : O3M_COLOR_INSTR));
  }
}

// Draws the whole body: `title` and `facts` on top, the rows with the cursor on `cursor`, `extra` in the right half (wrapped at its spaces, up to four lines) and `answer` (or nothing) under the rows in
// `answer_color`. There are no lines of keys: the key row at the bottom of the screen names them (OS-KEYROWDLG S5) and the grammar is the manual's.
inline void draw_page(o3m_canvas* c, const char* title, const char* facts, const PageRow* rows, uint32_t n, uint32_t cursor, const char* extra, const char* answer, uint32_t answer_color) {
  const o3m_draw_api* api = c->draw;
  const int32_t w = c->width;
  api->rect(c, 0, 0, w, c->height, O3M_COLOR_PANE);
  api->text(c, 8, 2, title, O3M_FACE_DATA, O3M_COLOR_TEXT);
  if (facts[0] != '\0') {
    api->text(c, 8, 24, facts, O3M_FACE_DATA, O3M_COLOR_TEXT_DIM);
  }
  draw_rows(c, 0, kRowTop, w / 2, rows, n, cursor);
  // the right half: the text wrapped at its spaces into lines that fit between the middle and the edge
  const int32_t cell = api->text_width(c, "0", O3M_FACE_LABEL);
  const uint32_t room = cell > 0 && w > w / 2 + 16 ? static_cast<uint32_t>((w - (w / 2 + 8) - 8) / cell) : 0u;
  uint32_t at = 0, line = 0;
  while (extra[at] != '\0' && line < 4u && room >= 4u) {
    uint32_t len = 0;
    while (extra[at + len] != '\0' && len < room) {
      ++len;
    }
    if (extra[at + len] != '\0') {  // cut at the last space inside the line, if there is one
      uint32_t cut = len;
      while (cut > 0u && extra[at + cut] != ' ') {
        --cut;
      }
      len = cut != 0u ? cut : len;
    }
    char buf[48];
    uint32_t k = 0;
    for (; k < len && k + 1u < sizeof buf; ++k) {
      buf[k] = extra[at + k];
    }
    buf[k] = '\0';
    api->text(c, w / 2 + 8, kRowTop + static_cast<int32_t>(line) * 18, buf, O3M_FACE_LABEL, O3M_COLOR_TEXT_DIM);
    at += len;
    while (extra[at] == ' ') {
      ++at;
    }
    ++line;
  }
  if (answer[0] != '\0') {
    api->text(c, 8, kRowTop + static_cast<int32_t>(n) * kRowPitch + 6, answer, O3M_FACE_LABEL, answer_color);
  }
}

}  // namespace o3edit
