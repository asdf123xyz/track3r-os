// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Which instrument slots something in a project names (OS-FILES part B, PURGE UNUSED; PAR-050 "delete all unused instruments"). Pure: it reads the project and nothing else.
//
// EVERY REFERENCE TO AN INSTRUMENT SLOT THE MODEL HAS (tests/instrument_use_test.cpp holds one case for each):
//   1. STEPS. A step's instrument byte (1..64 = slot + 1), in any pattern of the pool (the ones the song does not name too), on any of the 16 track slots (the tracks past `active_tracks` too), at any of
//      the 128 steps (the ones past a track's length too: they are kept, lengthening the track brings them back). A byte of 0 names nothing, one above 64 names nothing (steps are carried verbatim).
//   2. RANDOM INSTRUMENT (FX 6), in either FX column. The sequencer plays an instrument picked at random from slot - v to slot + v around the step's own (seq/sequencer.cpp), so those slots are named as
//      well; the two columns can both be one (the windows add). On a step with no instrument the window is around the instrument the track played last, which can itself be a random pick: the
//      slots such a track can reach are the closure of what its steps name under that window.
//   3. MIDI LEARN. A binding of the project's store (Settings::learn_slots) whose parameter is an instrument's (kind Instrument, not a "follow the selected instrument" binding, which names none).
//   Nothing else in a project holds an instrument slot: the song names patterns, tracks and the master have their own parameters, the Perform slots and scenes (model::PerfState) are FX per track,
//   the mixer, MIDI, USB, note-entry, mute and send-FX state hold no slot, and an app's parameters are its own. A MODULE instrument is never removed by a purge, used or not (an instance cannot
//   be unbound: OS-INSTUI DELETE).
// OUTSIDE THE MODEL (the caller passes them as `held`): the S7 instrument clipboard (SampleBank::clip_*) holds a copy of an instrument and a reference to its audio block, so every slot that reads that block
// is kept (file_ops_actions.cpp). There is NO pattern clipboard in this tree (Copy / Shift + Copy on Pattern are unbound; the Song clipboard holds pattern numbers); when one exists its steps' instrument bytes
// are references of kind 1 and belong in `held`. Instrument numbers held INSIDE an app (RhythmGen and the other SD apps keep their own parameters) are outside the model and cannot be known here: a purge
// may remove an instrument such an app plays, and an undo brings it back.
// The session's "last instrument" and the instrument in view are not references: they are where the cursor is.

#include <o3os/model/project.h>

#include <cstdint>

namespace o3os::model {

inline constexpr std::uint8_t kFxRandomInstrument = 6;  // seq::fx::kRandomInstrument (held equal by tests/instrument_use_test.cpp)

// Bit n: slot n is named by something above.
std::uint64_t instruments_in_use(const Project& project) noexcept;

// Bit n: slot n holds a sample or a MIDI instrument that nothing names (what PURGE removes). `modules_skipped` (optional) gets how many module instruments nothing names: they stay. `held` are slots
// something outside the model names (a bit each): the caller's holders, which are not purged either.
std::uint64_t instruments_unused(const Project& project, unsigned* modules_skipped = nullptr, std::uint64_t held = 0) noexcept;

}  // namespace o3os::model
