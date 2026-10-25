// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the Song screen reads (OS-005f): one read-only view of the order list, backed by the attached document's or, when there is none,
// by the demo order of OS-002b. The screen, its cursor (count, position, set) and the Play action ask this and never look at where the
// list is, so Play on row N is the row the screen shows.
//
//   document  Project::order(pos) for pos < Project::song_length(): the pattern number, and through it the pattern's name and length. A
//             position may name a pattern that does not exist yet: it is listed (exists() says false, the screen dims it) and, as the
//             sequencer plays it, it is empty for the project's default length.
//   demo      the 24 positions of fill_song_demo(): read-only.
//
// Nothing here allocates or writes.

#include <o3os/app/model.h>
#include <o3os/app/song_data.h>
#include <o3os/audio/play_position.h>
#include <o3os/model/project.h>

namespace o3os::app {

class SongSource {
 public:
  explicit SongSource(const Model& model) noexcept : project_(model.project), demo_(&model.song) {}

  bool is_demo() const noexcept { return project_ == nullptr; }

  // The positions in the list: the project's song length (at most model::kMaxSongPositions), the demo's for the demo.
  int count() const noexcept { return project_ != nullptr ? static_cast<int>(project_->song_length()) : song_length(*demo_); }

  // The pattern number at a position, 0 outside the list.
  unsigned pattern(int pos) const noexcept {
    if (pos < 0 || pos >= count()) {
      return 0;
    }
    return project_ != nullptr ? project_->order(static_cast<std::uint32_t>(pos)) : demo_->order[pos];
  }
  // The pattern at the position exists (always for the demo); false for a position the list does not have.
  bool exists(int pos) const noexcept {
    return project_ == nullptr ? (pos >= 0 && pos < count()) : project_->pattern(pattern(pos)) != nullptr;
  }
  // Its name, "" for a pattern without one or that does not exist (the project's names are longer than the screen's column: it cuts them).
  const char* name(int pos) const noexcept {
    if (project_ == nullptr) {
      return song_pattern(*demo_, pos).name;
    }
    const model::Pattern* p = project_->pattern(pattern(pos));
    return p != nullptr ? p->name : "";
  }
  // Its length in steps, as it plays: the longest of the active tracks, the project's default for a pattern that does not exist yet (what
  // the sequencer does, Sequencer::song_pattern_len).
  int steps(int pos) const noexcept {
    if (project_ == nullptr) {
      return song_pattern(*demo_, pos).steps;
    }
    const std::uint32_t len = project_->pattern_length(pattern(pos));
    return static_cast<int>(len != 0u ? len : project_->default_pattern_length());
  }

  // The row to mark as playing, -1 for none: the song row the sequencer reports (audio::PlayPosition, OS-005e) while it plays the song. Stopped, or
  // playing a single pattern, the demo keeps its fixed marker and a document shows none.
  int marker(const audio::PlayPosition::Value& play) const noexcept {
    if (play.playing && play.order != audio::PlayPosition::kNoOrder) {
      return static_cast<int>(play.order);
    }
    return project_ == nullptr ? demo_->play : -1;
  }

 private:
  const model::Project* project_ = nullptr;
  const SongData* demo_ = nullptr;
};

}  // namespace o3os::app
