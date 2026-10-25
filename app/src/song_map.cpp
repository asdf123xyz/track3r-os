// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/song_map.h>

namespace o3os::app {

std::uint32_t SongTrackMap::compute(const model::Pattern& pattern) noexcept {
  std::uint32_t map = 0;
  for (unsigned t = 0; t < kTracks; ++t) {
    const unsigned length = pattern.track_length[t] < model::kMaxPatternLength ? pattern.track_length[t] : model::kMaxPatternLength;
    Cell cell = Empty;
    for (unsigned s = 0; s < length && cell != Note; ++s) {
      const model::Step& step = pattern.steps[t][s];
      if (step.note != model::kNoteEmpty) {
        cell = Note;
      } else if (!model::is_empty(step)) {
        cell = Fx;
      }
    }
    map |= static_cast<std::uint32_t>(cell) << (2u * t);
  }
  return map;
}

std::uint32_t SongTrackMap::get(const model::Project& project, unsigned id) const noexcept {
  if (id < 1u || id > model::kMaxPatternId) {
    return 0;
  }
  if (&project != project_) {
    project_ = &project;  // another project: nothing cached is its
    for (Entry& e : entry_) {
      e = Entry{};
    }
  }
  const model::Pattern* pattern = project.pattern(id);
  if (pattern == nullptr) {
    return 0;
  }
  const std::uint32_t generation = project.pattern_generation(id);
  Entry& e = entry_[id];
  if (e.generation != generation || generation == 0u) {
    e.map = compute(*pattern);
    e.generation = generation;
    ++computed_;
  }
  return e.map;
}

unsigned SongUseCount::get(const model::Project& project, unsigned id) const noexcept {
  if (id < 1u || id > model::kMaxPatternId) {
    return 0;
  }
  if (!valid_ || &project != project_ || project.song_generation() != generation_) {
    for (std::uint8_t& c : count_) {
      c = 0;
    }
    const std::uint32_t length = project.song_length();  // (at most 255: a count never outgrows its byte)
    for (std::uint32_t pos = 0; pos < length; ++pos) {
      ++count_[project.order(pos)];
    }
    project_ = &project;
    generation_ = project.song_generation();
    valid_ = true;
    ++built_;
  }
  return count_[id];
}

}  // namespace o3os::app
