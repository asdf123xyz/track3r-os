// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The track map of the Song screen (OS-SONG1): what each row of the order list shows of its pattern, one small cell per track: a note in the track (filled), only other data in it (an
// instrument, a volume or FX: dim), nothing (empty). Computed on the main loop from the model, at most once per change of the pattern: the cache is keyed by the pattern's generation
// (model::Project::pattern_generation, which moves whenever a step of it, its name or its lengths change), so drawing a frame costs a table read per row, never a scan of the pattern.
// Nothing here allocates or writes outside the cache, and none of it is saved.

#include <o3os/model/project.h>

#include <cstdint>

namespace o3os::app {

class SongTrackMap {
 public:
  static constexpr unsigned kTracks = model::kMaxTracks;  // the map covers every track of the model; the screen shows the active ones
  enum Cell : std::uint8_t { Empty = 0, Fx = 1, Note = 2 };

  // Two bits a track (track t at bits 2t..2t+1, a Cell), over the steps that play (up to the track's own length). Pure: a note step (any non-empty note byte: a note, fade, cut or off)
  // makes the track a Note, else any other non-empty step makes it an Fx.
  static std::uint32_t compute(const model::Pattern& pattern) noexcept;
  static Cell cell(std::uint32_t map, unsigned track) noexcept { return track < kTracks ? static_cast<Cell>((map >> (2u * track)) & 3u) : Empty; }

  // The map of pattern `id` (1..255) of `project`: 0 for one that does not exist. Cached by the pattern's generation, and by the project (another project's patterns are never mistaken for these).
  std::uint32_t get(const model::Project& project, unsigned id) const noexcept;

  // How many maps were computed (a scan of a pattern) since this object was made: what a test counts to prove the cache.
  std::uint32_t computed() const noexcept { return computed_; }

 private:
  struct Entry {
    std::uint32_t generation;  // 0: nothing cached (an existing pattern's generation is never 0)
    std::uint32_t map;
  };
  mutable Entry entry_[model::kMaxPatternId + 1] = {};
  mutable const model::Project* project_ = nullptr;
  mutable std::uint32_t computed_ = 0;
};

// How many slots of the order list hold each pattern (OS-SONGSHARE S1, docs/specs/OS-SONGSHARE.md): the Song screen marks a pattern that two or more slots hold and says how many on the info line. A table of 256 bytes (one a pattern id; at
// most 255 positions, so a count fits a byte), rebuilt in one pass over the order list when the project's song_generation moves, so drawing a frame reads a byte. The next-use key (S4) walks the list by itself and does not need the table.
// Main loop only, like the map's cache; none of it is saved.
class SongUseCount {
 public:
  // The slots that hold pattern `id` (1..255) in `project`'s order list: 0 for an id no slot names (and for 0 or a number past 255). A pattern a slot names counts whether or not it exists yet.
  unsigned get(const model::Project& project, unsigned id) const noexcept;

  // How many times the table was rebuilt since this object was made: what a test counts to prove the cache.
  std::uint32_t built() const noexcept { return built_; }

 private:
  mutable std::uint8_t count_[model::kMaxPatternId + 1] = {};
  mutable const model::Project* project_ = nullptr;  // the project it was built for, with song_generation_: another project (or a moved generation) rebuilds
  mutable std::uint32_t generation_ = 0;
  mutable bool valid_ = false;
  mutable std::uint32_t built_ = 0;
};

}  // namespace o3os::app
