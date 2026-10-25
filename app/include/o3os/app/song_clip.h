// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The song clip (OS-SONGMAP, docs/specs/SONG-UX.md section 1.9; new Community OS behaviour): the third clipboard, beside the slot clipboard of the Song view (pattern numbers) and OS-PATCOPY's PatternClip
// (steps of one pattern). It holds a rectangle of the Song map: for each slot row and each track, that track's steps from the pattern of the slot, up to the track's length. Pasting writes them from
// step 0 of the target tracks (clipped at the target's length: Q2, no length travels). A slot that named a pattern that does not exist copies as an empty cell of the project's default length (D10).
//
// Capacity (D6): 32 cells of 128 steps, 28,672 bytes plus the cells' lengths. It is a member of the App, which the device builds in SDRAM (g_app_storage), so it costs no SRAM. It lives for the session: a
// pattern switch, an undo or a new document do not clear it (as PatternClip). Main loop only, no heap.

#include <o3os/model/limits.h>
#include <o3os/model/step.h>

#include <cstddef>
#include <cstdint>

namespace o3os::app {

class SongClip {
 public:
  static constexpr unsigned kCells = 32;                          // slots x tracks the clip holds
  static constexpr unsigned kSteps = model::kMaxPatternLength;    // steps a cell holds: a whole track of a pattern

  bool empty() const noexcept { return rows_ == 0 || cols_ == 0; }
  int rows() const noexcept { return rows_; }  // slots
  int cols() const noexcept { return cols_; }  // tracks

  // Starts a new clip of rows x cols cells with every cell empty and one default length; false (the old clip is kept) when it does not fit.
  bool begin(int rows, int cols, unsigned length) noexcept {
    if (rows < 1 || cols < 1 || length < 1u || length > kSteps || static_cast<unsigned>(rows) * static_cast<unsigned>(cols) > kCells) {
      return false;
    }
    rows_ = rows;
    cols_ = cols;
    for (unsigned i = 0; i < static_cast<unsigned>(rows * cols); ++i) {
      len_[i] = static_cast<std::uint8_t>(length);
    }
    return true;
  }
  // The cell of row r, column c (r < rows(), c < cols()): `length(r, c)` steps are held.
  model::Step* cell(int r, int c) noexcept { return steps_[index(r, c)]; }
  const model::Step* cell(int r, int c) const noexcept { return steps_[index(r, c)]; }
  unsigned length(int r, int c) const noexcept { return len_[index(r, c)]; }
  void set_length(int r, int c, unsigned length) noexcept { len_[index(r, c)] = static_cast<std::uint8_t>(length < 1u ? 1u : (length > kSteps ? kSteps : length)); }

 private:
  unsigned index(int r, int c) const noexcept {
    const unsigned i = static_cast<unsigned>(r) * static_cast<unsigned>(cols_) + static_cast<unsigned>(c);
    return i < kCells ? i : 0u;  // (a caller inside rows() x cols() never gets here)
  }

  model::Step steps_[kCells][kSteps];  // (only the cells and steps that begin() and the copy wrote are ever read)
  std::uint8_t len_[kCells] = {};
  int rows_ = 0;
  int cols_ = 0;
};

static_assert(sizeof(SongClip) < 29 * 1024, "the song clip is about 28 KiB (D6)");
static_assert(SongClip::kSteps <= 255, "a cell's length fits a byte");

}  // namespace o3os::app
