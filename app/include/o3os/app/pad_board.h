// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The pad board (OS-009 P1a.1): which note each of the 48 pads plays (48 pads, a note a pad, the pads that play a note).
// The layout is this OS's own; whether it matches the official firmware's is an open research question. Pad 0 is the top left of the
// panel and carries the HIGHEST notes: the pads are four rows of twelve, the bottom row is the lowest octave. Each row continues the
// one below it by the `offset`: 0..11 starts a row at the note `12 - offset` places back in the row below, 12 continues right after
// its last note, 13 starts a perfect fourth up (5 semitones). Only the notes of the scale are used (core/scales.h), the root being
// the first. Notes are 0..120 (as the OG files number them); a step stores one more (model note byte = note + 1, step.h).
//
// Session state (D5 of the spec, not persisted): scale 0 (no scale), offset 12, root note 36 until Config (P3)
// changes them. Pure data, no heap.

#include <cstdint>

namespace o3os::app {

class PadBoard {
 public:
  static constexpr unsigned kPads = 48;
  static constexpr std::uint8_t kMaxOffset = 13;   // the highest offset: a row starts 5 places up
  static constexpr std::uint8_t kMaxNote = 120;    // the board never goes above this note
  static constexpr std::uint8_t kDefaultScale = 0;
  static constexpr std::uint8_t kDefaultOffset = 12;
  static constexpr std::uint8_t kDefaultRoot = 36;
  static constexpr unsigned kPreviewPad = 96;      // the preview key plays note 60

  PadBoard() noexcept { set(kDefaultScale, kDefaultOffset, kDefaultRoot); }

  // Sets the notes of the pads (an offset above 13 is 13; a scale above 39 is read as 39 by is_in_scale).
  void set(std::uint8_t scale, std::uint8_t offset, std::uint8_t root) noexcept;

  std::uint8_t scale() const noexcept { return scale_; }
  std::uint8_t offset() const noexcept { return offset_; }
  std::uint8_t root() const noexcept { return root_; }

  // The note (0..120) of pad 0..47; the preview key is 60. A pad outside both gives 0.
  std::uint8_t note(unsigned pad) const noexcept;
  // The model note byte a step stores for that pad (1..121), or 0 for a pad that is not on the board.
  std::uint8_t step_note(unsigned pad) const noexcept { return pad < kPads ? static_cast<std::uint8_t>(note(pad) + 1u) : std::uint8_t{0}; }
  // Sets pads[i] to 1 for every pad whose note is `note`, 0 for the rest; returns how many.
  unsigned pads_with_note(int note, std::uint8_t (&pads)[kPads]) const noexcept;

 private:
  std::uint8_t scale_ = 0;
  std::uint8_t offset_ = 0;
  std::uint8_t root_ = 0;
  std::uint8_t notes_[kPads] = {};  // the note of each pad: by board position, not by pad
};

}  // namespace o3os::app
