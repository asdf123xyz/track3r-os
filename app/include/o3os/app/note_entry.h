// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The note-entry settings (OS-PADS-B; new Community OS behaviour, the maintainer's design of 2026-10-13): the root, the
// scale, the octave and the SCALE LOCK that the pads of the NOTES, PIANO and ISO modes and the MIDI keyboard share. The jump (the edit step) is the same setting as the F3 chooser's
// (PatternEdit::edit_step), not a second one. Held Note on the Pattern screen shows the panel: the 48 pads are the four rows ROOT, SCALE, JUMP, OCTAVE + LOCK.
//
// The project's since OS-PERSIST2 (the NOTE chunk: the four bytes and the jump; app/project_state.h note_state_of / note_apply). The PAD MODE is not (a performer's layout: a device
// preference, see pad_layer.h). Pure data, main loop only, no heap.

#include <o3os/core/scales.h>

#include <cstdint>
#include <type_traits>

namespace o3os::app {

// The twelve scales the panel offers, as indices into the 40 masks of core/scales.h, so no second table of masks: Chromatic, Major, Minor, Dorian, Phrygian, Lydian, Mixolydian,
// Pent. maj, Pent. min, Blues (Blues Min), Harm. minor, Whole tone.
inline constexpr unsigned kNoteScales = 12;
inline constexpr std::uint8_t kNoteScaleOg[kNoteScales] = {1, 3, 2, 4, 8, 5, 10, 19, 18, 16, 12, 24};
inline constexpr char kNoteScaleNames[kNoteScales][12] = {"Chromatic", "Major",     "Minor",     "Dorian",    "Phrygian",    "Lydian",
                                                          "Mixolydian", "Pent. maj", "Pent. min", "Blues",     "Harm. minor", "Whole tone"};
inline constexpr char kRootNames[12][3] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

inline constexpr unsigned kEntryOctaves = 10;   // OCTAVE 0..9
inline constexpr unsigned kEntryMaxNote = 120;  // the pad board's kMaxNote (pad_board.h): no pad plays above it
inline constexpr std::uint8_t kNoNote = 0xFF;

struct NoteEntry {
  std::uint8_t root = 0;     // the pitch class 0..11 (C..B)
  std::uint8_t scale = 0;    // 0..kNoteScales-1
  std::uint8_t octave = 3;   // 0..9; with the C root, the 36 (C-3) that the pad board always started at
  std::uint8_t lock = 0;     // SCALE LOCK: 0 off, 1 on. The pads of the NOTES mode lay out the scale and the pads and the MIDI keyboard snap to it

  std::uint8_t og_scale() const noexcept { return kNoteScaleOg[scale < kNoteScales ? scale : 0u]; }
  const char* scale_name() const noexcept { return kNoteScaleNames[scale < kNoteScales ? scale : 0u]; }
  const char* root_name() const noexcept { return kRootNames[root < 12u ? root : 0u]; }
  bool in_scale(unsigned note) const noexcept { return core::is_in_scale(static_cast<std::uint8_t>(note), root, og_scale()); }
  bool is_root(unsigned note) const noexcept { return note % 12u == root; }
  // The lowest pad of the NOTES board: the root in the octave (a note number, 0..kEntryMaxNote).
  unsigned board_root() const noexcept {
    const unsigned n = (octave < kEntryOctaves ? octave : kEntryOctaves - 1u) * 12u + (root < 12u ? root : 0u);
    return n > kEntryMaxNote ? kEntryMaxNote : n;
  }
  // The nearest in-scale note to `note` (0..127), the lower one on a tie; a note already in the scale is itself. Pitch classes only, so it holds for every octave.
  unsigned snap(unsigned note) const noexcept {
    if (note > 127u || in_scale(note)) {
      return note;
    }
    for (unsigned d = 1; d <= 6u; ++d) {
      if (note >= d && in_scale(note - d)) {
        return note - d;
      }
      if (note + d <= 127u && in_scale(note + d)) {
        return note + d;
      }
    }
    return note;  // unreachable: the root is in every scale and no gap is wider than 3 semitones (Harm. minor, Pent.)
  }
  // The next in-scale note above (dir > 0) or below `note` (0..127); `note` itself when there is none (the end of the range).
  unsigned next(unsigned note, int dir) const noexcept {
    for (unsigned k = 1; k <= 12u; ++k) {
      if (dir > 0 ? note + k > 127u : note < k) {
        break;
      }
      const unsigned n = dir > 0 ? note + k : note - k;
      if (in_scale(n)) {
        return n;
      }
    }
    return note;
  }
  // The note a pad or the MIDI keyboard plays: snapped with SCALE LOCK on.
  unsigned locked(unsigned note) const noexcept { return lock != 0u ? snap(note) : note; }
};
static_assert(std::is_trivially_copyable_v<NoteEntry> && sizeof(NoteEntry) == 4, "the NOTE chunk's fields (OS-PERSIST2)");

}  // namespace o3os::app
