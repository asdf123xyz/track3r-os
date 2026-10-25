// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The pattern clipboard (OS-PATCOPY, new Community OS behaviour): the block that Copy took, `tracks` columns of `rows` steps, kept for the session (a pattern switch, an undo or a new
// document do not clear it). Room for the whole of a pattern, 16 tracks x 128 steps of 7 bytes = 14,336 bytes: it is a member of the App, which the device builds in SDRAM
// (g_app_storage), so it costs no SRAM. Main loop only.
//
// PAT-OPS-B (new Community OS behaviour, docs/specs/PAT-OPS.md): the same clip also holds a WHOLE pattern, the Copy of a held F1 (the PATTERN chooser): all 16 tracks x 128 steps plus the 16 track lengths, the
// name and the number it was taken from (49 bytes more). `whole()` says which it is; the Copy of the grid (begin()) makes a block clip again.

#include <o3os/model/limits.h>
#include <o3os/model/step.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace o3os::app {

class PatternClip {
 public:
  static constexpr std::uint32_t kCapacity = model::kMaxTracks * model::kMaxPatternLength;  // the largest block there is

  bool empty() const noexcept { return rows_ == 0 || tracks_ == 0; }
  int rows() const noexcept { return rows_; }
  int tracks() const noexcept { return tracks_; }

  // Starts a new clip of tracks x rows; false (the old clip is kept) when it does not fit.
  bool begin(int tracks, int rows) noexcept {
    if (tracks < 1 || rows < 1 || static_cast<std::uint32_t>(tracks) * static_cast<std::uint32_t>(rows) > kCapacity) {
      return false;
    }
    tracks_ = tracks;
    rows_ = rows;
    whole_ = false;
    col0_ = 0;
    col1_ = 4;
    return true;
  }
  // OS-AUDIT-56 (new Community OS behaviour): the clip of a column block holds whole steps but only the columns col0..col1 (Column values) are meant; a paste writes those and leaves the rest of the
  // target steps. begin() makes it all five again. Memory only, like the whole clip (no file or project holds it).
  void set_columns(int col0, int col1) noexcept {
    col0_ = static_cast<std::int8_t>(col0);
    col1_ = static_cast<std::int8_t>(col1);
  }
  int col0() const noexcept { return col0_; }
  int col1() const noexcept { return col1_; }
  bool whole_steps() const noexcept { return col0_ == 0 && col1_ == 4; }
  // The clip is a whole pattern (PAT-OPS-B): call after begin(kMaxTracks, kMaxPatternLength) and filling the columns. `lengths` are the 16 track lengths, `name` the pattern's (kPatternNameMax + 1 bytes,
  // NUL-terminated), `source` the pattern number it was taken from.
  void set_whole(const std::uint8_t* lengths, const char* name, std::uint8_t source) noexcept {
    std::memcpy(lengths_, lengths, sizeof lengths_);
    std::memcpy(name_, name, sizeof name_);
    name_[model::kPatternNameMax] = '\0';
    source_ = source;
    whole_ = true;
  }
  bool whole() const noexcept { return whole_; }  // (begin() ends it)
  const std::uint8_t* lengths() const noexcept { return lengths_; }  // (a whole clip)
  const char* name() const noexcept { return name_; }                // (a whole clip)
  unsigned source() const noexcept { return source_; }               // (a whole clip) the pattern number it was copied from
  // The column `t` of the clip: `rows()` steps.
  model::Step* column(int t) noexcept { return steps_ + static_cast<std::size_t>(t) * static_cast<std::size_t>(rows_); }
  const model::Step* column(int t) const noexcept { return steps_ + static_cast<std::size_t>(t) * static_cast<std::size_t>(rows_); }

 private:
  model::Step steps_[kCapacity];  // (only the rows x tracks that begin() set and Copy wrote are ever read)
  int rows_ = 0;
  int tracks_ = 0;
  bool whole_ = false;
  std::int8_t col0_ = 0;
  std::int8_t col1_ = 4;
  std::uint8_t source_ = 0;
  std::uint8_t lengths_[model::kMaxTracks] = {};
  char name_[model::kPatternNameMax + 1] = {};
};

}  // namespace o3os::app
