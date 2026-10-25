// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Fill generators (OS-PTOOLS part 1, docs/specs/OS-PTOOLS.md; OS-FILL2, docs/specs/OS-FILL2.md): which steps of a range a fill chooses and the note, instrument, volume or FX it writes there, as pure functions over
// `Step`. The caller cuts the range (scope, block, the track's length) and writes the result through the Editor as one undo unit.
// OS-FILL2 (NEW COMMUNITY OS BEHAVIOUR): what is written is a MODE (CONST: FROM; RAMP: FROM to TO over the track's hits; RANDOM: uniform in FROM..TO) between FROM and TO, in the domain of WHAT.
//
// EVERY n (this OS's rule, new Community OS behaviour; what the official firmware does is an open research question, there is no capture in the repo) chooses `offset % n == 0` from the first step of the range, n 1..32; the step is written by `note_set`
// (an empty step takes the last instrument); nothing is written outside 0..127 (PAR-150: an 8-bit note would wrap past 127).
// NEW COMMUNITY OS BEHAVIOUR (PAR-053's rounding rule is UNKNOWN, the others have no source): EUCLID, the density of RANDOM, the SCALE run and the no-duplicate rule. See the spec.
//
// Pure, no heap, no globals; the random numbers are the caller's `Rng`, so the same seed gives the same fill.

#include <o3os/model/step_edit.h>

#include <cstdint>

namespace o3os::model::fill {

enum class What : std::uint8_t { Note = 0, Instr = 1, Vol = 2, Fx1 = 3, Fx2 = 4 };
enum class Type : std::uint8_t { Every = 0, Random = 1, Euclid = 2, Scale = 3 };
enum class Mode : std::uint8_t { Const = 0, Ramp = 1, Random = 2 };

inline constexpr unsigned kMaxN = edit::kMaxEditStep;  // EVERY n / SCALE n / EUCLID events: 1..32
inline constexpr unsigned kDefaultDensity = 30;        // three steps in ten

// The OS's usual xorshift32 (seq/rng.h) kept here because the model does not depend on seq.
struct Rng {
  std::uint32_t s = 0x9E3779B9u;
  std::uint32_t next() noexcept {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  }
  unsigned below(unsigned n) noexcept { return n == 0u ? 0u : next() % n; }
};

struct Plan {
  What what = What::Note;
  Type type = Type::Every;
  std::uint8_t n = 1;            // EVERY, SCALE
  std::uint8_t events = 4;       // EUCLID
  std::uint8_t density = kDefaultDensity;  // RANDOM, 0..100 %
  std::uint8_t root = 0;         // WHAT = Note: pitch class 0..11
  std::uint8_t scale = 0;        // WHAT = Note: 0..39 (core/scales.h); 0 and 1 allow every note
  Mode mode = Mode::Const;       // how the hits take a value; a Note fill with TYPE Random or Scale ignores it (FROM..TO is the window those two pick from)
  std::uint8_t from = edit::kDefaultNote;       // the first value, in the domain of `what` (see domain()): a note 0..127 (an OG note), an instrument 1..64, a volume 0..127, an FX value
  std::uint8_t to = edit::kDefaultNote + 11;    // the last value (RAMP, RANDOM, the note window); FROM may be above TO
  std::uint8_t fx_type = 1;      // WHAT = Fx1 / Fx2: the FX type written (an executable type of the picker, never None)
};

// What the dialog starts from (OS-FILL2 Q2): the note under the cursor (a step's byte 1..128), the instrument in hand (1..64), the volume under the cursor (0..127) and the FX types of the cursor step (0: none).
struct Value {
  std::uint8_t note = 61;
  std::uint8_t instr = 1;
  std::uint8_t vol = edit::kFirstVolume;
  std::uint8_t fx_type[2] = {0, 0};
};

// The values FROM and TO can take for `plan.what` (for an FX fill, the table's min..max of `plan.fx_type`).
struct Range {
  std::uint8_t lo;
  std::uint8_t hi;
};
Range domain(const Plan& plan) noexcept;

// Puts FROM, TO (and FX TYPE for an FX fill) of `plan.what` at what a fill starts with, from the current value: NOTE from the cursor's note (an unset one is C-5) to an octave above it (at most 127), INSTR the instrument in hand
// for both, VOLUME the cursor's volume (else the first-entry 127) down to 0, FX the cursor step's type in that column (else the first picker type) with its default value for both.
void range_default(Plan& plan, const Value& value) noexcept;

// Is step `i` (0-based) of a range of `len` steps a hit of `events` Euclidean events? (i * events) mod len < events, events clamped to len; 0 events: never.
bool euclid_hit(unsigned events, unsigned len, unsigned i) noexcept;

// The lowest note >= `note` (0..127, an OG note) in `scale` with `root`, or 128 when there is none up to 127.
unsigned scale_up(unsigned note, unsigned root, unsigned scale) noexcept;

// Marks the steps the plan chooses in a range of `len` (at most kMaxPatternLength) steps; returns how many.
unsigned select(const Plan& plan, unsigned len, Rng& rng, bool* hit) noexcept;

// Fills `steps[0..len)` (one track's range) as the plan says. `last` is the session's last used instrument and FX (an empty step takes it). Returns the number of steps written.
// RAMP counts the hits of this range (a track of its own: the first hit is FROM, the last is TO; one hit is FROM). A note is moved up into the scale; one that cannot be placed at 127 or below leaves its step as it was (PAR-150).
// A FX fill replaces the type of the column on every hit step and writes the value (a single-value type takes its one value); the rest of the step stays.
unsigned apply(const Plan& plan, Step* steps, unsigned len, Rng& rng, edit::Last& last) noexcept;

}  // namespace o3os::model::fill
