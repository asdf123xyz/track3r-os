// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Fill dialog's rows and session state (OS-PTOOLS part 1, docs/specs/OS-PTOOLS.md; OS-FILL2, docs/specs/OS-FILL2.md; NEW COMMUNITY OS BEHAVIOUR: how the official firmware's popup compares is an open research question). Eight rows: SCOPE, WHAT (NOTE, INSTR,
// VOLUME, FX 1, FX 2), TYPE, N (EVERY N, SCALE) or EVENTS (EUCLID) or DENSITY (RANDOM), MODE (CONST, RAMP, RANDOM), KEY (a note fill: root and scale in one row) or FX TYPE (an FX fill), FROM (VALUE in CONST) and TO.
// Row 6 and the captions of rows 4 and 7 follow WHAT, TYPE and MODE; a row that means nothing for the other rows is drawn dim, shows "-" and ignores the encoder.
// The values are session state (OS-009 D5), not the project's and not Learn targets. The generators are model/pattern_fill.h; the actions are fill_actions.cpp.

#include <o3os/core/param.h>
#include <o3os/model/pattern_fill.h>

#include <cstdint>

namespace o3os::app {

enum class FillRow : std::uint8_t { Scope = 0, What = 1, Type = 2, Count = 3, Mode = 4, Key = 5, From = 6, To = 7 };
inline constexpr unsigned kFillRows = 8;

enum class FillScope : std::uint8_t { Block = 0, Track = 1, Pattern = 2 };

struct FillView {
  model::fill::Plan plan;
  FillScope scope = FillScope::Track;
  std::uint8_t row = 0;      // the row the cursor is on (FillRow)
  bool valid = false;        // a fill was made with the dialog: the action pattern.fill (no key since OS-QJUMP, it was Shift + Fx) repeats it, with the stored FROM and TO. Before that it is the OS-CFGWIRE fill (every Nth step)
  model::fill::Rng rng;      // advances with every random fill, so a repeated random fill draws again
  model::fill::Value open;   // what the cursor held when the dialog opened (OS-FILL2 Q2): the FROM and TO that a change of WHAT starts from
};

const core::ParamDesc& fill_desc(FillRow row) noexcept;
// The caption: row 4 is "N" for EVERY N and SCALE, "EVENTS" for EUCLID and "DENSITY" for RANDOM; row 6 "KEY" or, for an FX fill, "FX TYPE"; row 7 "VALUE" in CONST (one value), else "FROM".
const char* fill_caption(const FillView& f, FillRow row) noexcept;
// Whether the row means anything with the other rows as they are (an unused row is drawn dim and does not change): MODE is dim for a note fill of TYPE RANDOM or SCALE (they choose the notes), KEY for INSTR and VOLUME, TO in CONST (but not for that note window).
bool fill_row_used(const FillView& f, FillRow row) noexcept;
// The row's value: the enumerated rows' index, the count (N, EVENTS or DENSITY), KEY's scale, the FX TYPE's picker index (1..59), FROM and TO as the plan holds them.
float fill_get(const FillView& f, FillRow row) noexcept;
// Sets the row, clamped to its range. A change of WHAT puts FROM and TO (and FX TYPE) back at what the dialog opened with; a change of FX TYPE puts FROM and TO at the new type's default (OS-FILL2 D3).
void fill_set(FillView& f, FillRow row, float value) noexcept;
// The row `detents` places on: an enumerated row (SCOPE, WHAT, TYPE, MODE, KEY, FX TYPE) rolls over, the others stop at their ends; FROM and TO of INSTR walk the instrument order (64..49, then 1..48). False for an unused row or when nothing changed.
bool fill_step(FillView& f, FillRow row, int detents) noexcept;
// Shift + encoder (OS-SHIFTENC): the row's coarse step: N 4, DENSITY 10, FROM and TO 12 for a note and 10 for the others, KEY of a note fill the root (a ring of twelve). The other enumerated rows keep their fine step.
bool fill_step_coarse(FillView& f, FillRow row, int detents) noexcept;
// The value as text (no unit): the names of the enumerated rows, the note name (`octave_shift`: the device setting MIDDLE C, as the grid names it), "C# Dorian" for KEY, the picker's name for FX TYPE,
// the words of an FX value where it has some (fx_text.h), "-" for an unused row. Returns the length.
unsigned fill_text(const FillView& f, FillRow row, char* out, unsigned cap, int octave_shift = 0) noexcept;

}  // namespace o3os::app
