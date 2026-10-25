// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The edit rules of one pattern step (OS-009 P1a.1): what the pad, the encoder, Delete and the like do to a step, as pure functions
// over `Step`. They follow this OS's own reference model of pattern editing, kept as test vectors (tests/data/pattern_edit_vectors.h, the
// output of our own reference model, maintainer 2026-10-18; whether the official firmware edits the same way is an open research question), on the
// model's encoding (step.h): the vectors' note -1 / -2 / -3 / -4 / 0..127 is the model's 0 / 253 / 254 / 255 / 1..128, the vectors' instrument
// slot i is i + 1, and an FX of type 0 carries no value. A function changes the one step it is given and, where the reference model changes
// them, the session's "last used" instrument and FX (`Last`); nothing else: the caller walks the selection, applies the result
// through the Editor (one undo unit) and decides where the cursor goes.
//
// Single and multi. The reference model edits one step differently from a range, and decides it in two ways. `single_cell` is "the selection is one step"
// (the note and instrument edits); `single_state` is "no range was started" (a flag of the pattern editor: the FX edits).
// The two differ only for a range started and never extended; the caller says which holds.
//
// Untrusted data (PAR-003). A note byte in 129..252, a volume byte in 128..254, an FX type that is not in the table, a placeholder or
// an engine type is carried untouched by every delta and every multi-step set; only an explicit single-step entry replaces it.
//
// Pure functions, no heap, no globals.

#include <o3os/model/step.h>

#include <cstdint>

namespace o3os::model::edit {

inline constexpr unsigned kMaxEditStep = 32;       // the largest edit step
inline constexpr unsigned kDefaultNote = 60;       // the note an empty step takes when only an instrument is given
inline constexpr std::uint8_t kFirstVolume = 127;  // the value of a volume entered on an unset step (Q-P1-10, default)
inline constexpr unsigned kInstrumentSlots = 64;   // 48 samples + 16 MIDI channels: a step names one as slot + 1
inline constexpr unsigned kPadCount = 48;

// Elements for clear().
inline constexpr unsigned kElNote = 1;   // note and instrument
inline constexpr unsigned kElInstr = 2;  // the same as kElNote
inline constexpr unsigned kElFx1 = 4;
inline constexpr unsigned kElFx2 = 8;
inline constexpr unsigned kElVol = 16;   // OS-new
inline constexpr unsigned kElAll = kElNote | kElFx1 | kElFx2 | kElVol;  // note, both FX and the volume
inline constexpr unsigned kElMask = kElAll | kElInstr;                  // every bit clear() reads

struct Last {
  std::uint8_t instrument = 1;  // last used instrument as a step stores it (1..64: slot + 1)
  std::uint8_t fx = 0;          // last used FX as a picker index (0..35; 0 is None) (fx_table.h picker_index)
};

struct Rule {
  bool single_cell;   // the selection is one step: the single branch of the note and instrument routines
  bool single_state;  // no range started: the single branch of the FX and volume routines
};

// The integer range rescale: long arithmetic, with the `+1` branch when the input range is larger.
constexpr int map_int(int x, int in_min, int in_max, int out_min, int out_max) noexcept {
  const long lx = x, a = in_min, b = in_max, c = out_min, d = out_max;
  if ((b - a) > (d - c)) {
    return static_cast<int>((lx - a) * (d - c + 1) / (b - a + 1) + c);
  }
  return static_cast<int>((lx - a) * (d - c) / (b - a) + c);
}

// A note byte a pad or key may write: 1..128 notes and 253 fade, 254 cut, 255 off.
constexpr bool is_enterable_note(unsigned note) noexcept { return (note >= 1u && note <= 128u) || (note >= 253u && note <= 255u); }

// Notes.
void note_set(Step& step, unsigned note, const Rule& rule, Last& last) noexcept;
void note_delta(Step& step, int delta, const Rule& rule, Last& last) noexcept;

// Instruments (a step stores slot + 1; the walk is this OS's order: slots 63..48 then 0..47, a delta clamped to +-5).
void instrument_set(Step& step, unsigned instrument, const Rule& rule, Last& last) noexcept;
void instrument_delta(Step& step, int delta, const Rule& rule, Last& last, int limit = 5) noexcept;  // limit: the places one call may walk (5; OS-SHIFTENC's coarse step passes 64)
// The walk alone, for the tests: the instrument (1..64) `delta` places from `instrument` in this OS's order.
unsigned instrument_walk(unsigned instrument, int delta, int limit = 5) noexcept;

// Volume (OS-new: an OG pattern step has no volume byte, so Export writes the volume as FX 18). Same single/multi rule as an FX value; 255 = unset.
void volume_set(Step& step, unsigned value, const Rule& rule) noexcept;
void volume_delta(Step& step, int delta, const Rule& rule) noexcept;

// FX. `fx` is the column, 0 = FX1, 1 = FX2.
void fx_value_delta(Step& step, unsigned fx, int delta, const Rule& rule, Last& last) noexcept;
void fx_value_set(Step& step, unsigned fx, int value, const Rule& rule, Last& last) noexcept;
void fx_pad(Step& step, unsigned fx, unsigned pad, const Rule& rule, Last& last) noexcept;  // pad 0..47 spread over min..max by map_int
void fx_type_delta(Step& step, unsigned fx, int delta) noexcept;                           // one picker place up or down
void fx_type_set(Step& step, unsigned fx, unsigned type) noexcept;                         // an executable type, or 0 to clear

// Clear: Note and Instr empty the note and the instrument, Fx1 / Fx2 their type, Vol the volume.
void clear(Step& step, unsigned elements) noexcept;

}  // namespace o3os::model::edit
