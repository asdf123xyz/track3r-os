// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Where a note played while the pattern plays goes (OS-LIVEREC, the maintainer's design, 2026-10-13; NEW Community OS behaviour, no parity claim). It replaces the placement of OS-FU2, which was:
// the first track from the selected one upward whose step was free, not
// muted and not held, and track arming on top of it, OS-009 P1d / PAR-047, which the maintainer dropped: there is no arming). Pure functions over what the app knows, so a host test can walk them.
//
// TRACK. Always the cursor's track; no search for an empty step: the note overwrites what the step holds (its note and instrument; the volume and the FX stay, as a note entered by hand leaves them). Device setting REC POLY: Off (the default) is that track only (a second note on it while the first is held writes over the same step, as a monophonic
// track would play it); Spill sends the notes held together on to the next tracks, one a voice: the first track from the cursor's up whose recorded note is not still held.
//
// INSTRUMENT. The selected one, the one the strip shows (ViewState::instrument).
//
// STEP. Follow (Play + Shift + Rec) and free (Play + Rec) differ in whether the cursor and the view ride the playhead, not in where the note goes: it goes to the playhead's step on the
// cursor's track, rounded by REC QUANTIZE. Nearest step (the default): the step the playhead is on, or the next one once half of it has gone (the sequencer publishes the
// microstep the playhead is in, 0..47, audio/play_position.h); Playing step: the step the playhead is on, whatever the phase. The step after the last of the pattern is its
// first when the pattern loops (Pattern mode); in a song the pattern is left there, so the note stays on the last step. A track shorter than the pattern plays at the step modulo its own
// length, so a note on it goes to that row.
//
// THE TAKE (maintainer decision 2026-10-13, the whole take): everything recorded from the moment recording begins (Edit while the pattern plays) to the moment it ends (Rec off, Stop, another
// screen) is ONE undo unit, the notes and their OFFs alike: one Undo takes the whole take back, one Redo gives it again (rec_actions.cpp: rec_take_sync). An edit of another kind during the
// take closes it first and is a unit of its own; the next note opens a new take. A take with nothing recorded leaves no entry.
//
// RELEASE. The held note's track is closed and an OFF is written at the PLAYHEAD (never rounded) on the same track; when the note is released within the step the playhead was in when it
// was played, or the playhead is on the step the note was written to, the OFF goes to the step after the note (a note released within its step ends in the next). An OFF is not
// written over a step that holds a note or an OFF already (it would erase a take): that note ends the held one itself, the track is monophonic. Released while the pattern is not playing: nothing is
// written. In a song the step after the last of the pattern is not wrapped to: no OFF is written there (the pattern ends the note).
//
// REC MICRO and REC VELOCITY (OS-RECMICRO, PAR-037 / PAR-064; device settings, both Off by default; docs/specs/OS-RECMICRO.md). They are NEW Community OS behaviour (no parity claim; how the official firmware's one "Rec Quantization" setting writes microtiming is an open research question).
// A Micro-move (FX type 2) takes the microstep the playhead is in (0..47) scaled onto 0..100, from the sub-step phase:
//   - REC MICRO On: the note's offset from the step it is written to goes to FX 2 as a Micro-move, only when it is more than 0 (no unconditional m02, no m02 00);
//   - the offset is the playhead's microstep when the note is written to the step the playhead is on (REC QUANTIZE Playing step; Nearest step in the first half of the step; the last step of a song);
//     a note that Nearest step rounds UP to the next step is early for it, and the Micro-move (0..100 of a step, a delay only) has no negative: the offset is clamped to 0, so no FX (the grid wins);
//   - FX 2 is written only when it is empty or already a Micro-move (the earlier take's, which a new note replaces: a zero offset then clears it); an FX the user put there stays, and then the note has no Micro-move;
//   - the OFF is not micro-timed (whether the official firmware micro-times the OFF is an open research question);
//   - REC VELOCITY External: the velocity (1..127) of a note played on the MIDI keyboard is the step's volume column (the same 0..127 scale); the pads and the note of a step without a velocity (0) leave it
//     as it is. (What the official firmware writes is an open research question: OS-RECMICRO.)
//
// Pure functions, no heap, no globals.

#include <o3os/model/step.h>
#include <o3os/model/step_edit.h>

#include <cstdint>

namespace o3os::app::rec {

// The device settings (model::device_setting::RecPoly, RecQuantize): the values of the descriptors in model/params.cpp.
enum class Poly : std::uint8_t { Off = 0, Spill = 1 };
enum class Quantize : std::uint8_t { Nearest = 0, Playing = 1 };

inline constexpr std::uint8_t kFxMicroMove = 2;   // fx::MICROMOVE (range 0..100 of a step, model/fx_table.h)
inline constexpr int kUsteps = 48;                // microsteps a step (seq::kMicrostepsPerStep)
inline constexpr unsigned kMicroMax = 100;        // the top of the Micro-move range

// The row a note lands on: `head` is the playhead's step in the pattern, `ustep` the microstep of it the playhead is in (0..47), `length` the track's, `pattern_length` the pattern's. Nearest: the next
// step from the microstep 24 on, which is half the step (it rounds up); Playing: `head`. The step after the pattern's last is its first when `wrap` (Pattern mode: the pattern loops) and the last
// one again when not (a song leaves the pattern there). A track shorter than the pattern plays at the step modulo its own length: that is the row.
// Whether the note goes on to the step AFTER the one the playhead is on: Nearest step from the microstep 24 on (half the step; it rounds up), and the step after exists (the pattern's next, or its first when it
// loops). The last step of a song has none: the note stays there.
constexpr bool goes_on(int head, int ustep, int pattern_length, Quantize q, bool wrap) noexcept {
  return q == Quantize::Nearest && ustep >= kUsteps / 2 && (head + 1 < pattern_length || wrap);
}

constexpr int quantize_row(int head, int ustep, int length, int pattern_length, Quantize q, bool wrap) noexcept {
  int at = head;
  if (goes_on(head, ustep, pattern_length, q, wrap)) {
    at = head + 1 < pattern_length ? head + 1 : 0;
  }
  return length > 0 ? at % length : 0;
}

// REC MICRO: how far into its step the note was played, in microsteps; negative (early) when `early`: the note was rounded up to the next step (goes_on).
constexpr int micro_offset(int ustep, bool early) noexcept { return early ? ustep - kUsteps : ustep; }

// The Micro-move value of an offset: offset * 100 / 47 (0..100 over the 47 microsteps after the step's start), 0 for none. An early note (offset <= 0) is clamped to 0: the FX has no negative.
constexpr unsigned micro_value(int offset) noexcept {
  if (offset <= 0) {
    return 0;
  }
  const unsigned v = static_cast<unsigned>(model::edit::map_int(offset > kUsteps - 1 ? kUsteps - 1 : offset, 0, kUsteps - 1, 0, static_cast<int>(kMicroMax)));
  return v > kMicroMax ? kMicroMax : v;
}

// Whether FX 2 may take a Micro-move: empty, or already one. Any other FX is the user's and stays.
constexpr bool micro_slot_free(const model::Step& s) noexcept { return s.fx[1].type == model::kFxNone || s.fx[1].type == kFxMicroMove; }

// The first track of [from, tracks) whose recorded note is not held (bit t of `held`), or -1: where Spill puts the next note of a chord.
constexpr int next_free_track(unsigned from, unsigned tracks, std::uint32_t held) noexcept {
  for (unsigned t = from; t < tracks && t < 32u; ++t) {
    if (((held >> t) & 1u) == 0u) {
      return static_cast<int>(t);
    }
  }
  return -1;
}

// The step as a recorded note writes it: the note (0..127 as the OG pattern file has it; the model's 1..128) and the selected instrument (1..64); the volume column when REC VELOCITY gives `velocity` (1..127; 0: none,
// the volume stays), and FX 2 when REC MICRO is on (`micro` >= 0, a Micro-move value 0..100 from micro_value; -1: REC MICRO is off, FX 2 stays): the value when it is more than 0 and the slot is free, a
// zero offset clears a Micro-move left there by an earlier note. The rest of the step (FX 1, an FX of the user in FX 2) is left as it is.
constexpr void write_note(model::Step& s, unsigned note, unsigned instrument, int micro, unsigned velocity) noexcept {
  s.note = static_cast<std::uint8_t>((note > 127u ? 127u : note) + 1u);
  s.instr = static_cast<std::uint8_t>(instrument < 1u ? 1u : (instrument > model::edit::kInstrumentSlots ? model::edit::kInstrumentSlots : instrument));
  if (velocity != 0u) {
    s.vol = static_cast<std::uint8_t>(velocity > model::kVolumeMax ? model::kVolumeMax : velocity);
  }
  if (micro >= 0 && micro_slot_free(s)) {
    if (micro > 0) {
      s.fx[1].type = kFxMicroMove;
      s.fx[1].value = static_cast<std::uint8_t>(micro > static_cast<int>(kMicroMax) ? kMicroMax : static_cast<unsigned>(micro));
    } else if (s.fx[1].type == kFxMicroMove) {
      s.fx[1].type = model::kFxNone;
      s.fx[1].value = 0;
    }
  }
}

// Where a release's OFF goes, as rows of the track (the step modulo its length): `now` is the playhead's row, `head` its row when the note was played, `at` the row the note was written to (they
// differ under Nearest), `length` the track's, `pattern_length` the pattern's. The playhead, unless it is still in the step the note was played in or on the note's own: then the row after the
// note, wrapping to the first (a shorter track loops inside the pattern; the pattern's own last row wraps only when `wrap`, else -1: no OFF).
constexpr int off_row(int now, int head, int at, int length, int pattern_length, bool wrap) noexcept {
  if (now != head && now != at) {
    return now;
  }
  if (at + 1 < length) {
    return at + 1;
  }
  return at + 1 >= pattern_length && !wrap ? -1 : 0;
}

// The OFF as the release writes it: the note OFF, nothing else (no Micro-move: see REC MICRO above).
constexpr void write_off(model::Step& s) noexcept { s.note = model::kNoteOff; }

}  // namespace o3os::app::rec
