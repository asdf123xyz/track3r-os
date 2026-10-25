// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Reading a step from the model arena in the render context (OS-005c; ADR 0013 D5: "pattern rows are read from the model arena (D7) with a generation
// check and a row copy (H4 out); residency and preload are OS-004a's (H5/H6 out: the render never requests SD)").
//
// The arena belongs to the main loop, which edits patterns while they play. The render never holds a pointer into a step across a call: it copies the
// seven bytes of the step when its row starts and works from the copy (H4: an interrupt routine must not mutate live pattern data). The copy is CHECKED against the
// pattern's generation (model/project.h: a counter that every accepted mutation of the pattern advances, and that is 0 for an id that does not exist):
//
//     g0 = generation(id);   copy;   g1 = generation(id)       g0 == g1: the copy is whole, as of one moment
//                                                              g0 != g1: an edit landed inside the window, copy again (once)
//                                                              g0 == 0 : the pattern is gone: the row is empty
//
// Two attempts at most, so the work is bounded; a second mismatch gives an empty row and is counted by the caller (RowStatus::Torn). What the check cannot
// see is a write that has stored the new bytes but not yet advanced the counter: Project::set_step stores, then bumps (model/project.cpp: touch_pattern after the
// store), so a copy that overlaps the store itself sees the same generation twice around a half-written step. A Step is seven independent bytes, every
// combination of which is a legal step (steps are untrusted data, sanitised where used), so the worst such a copy does is play a note with the new note
// and the old instrument ONCE; it cannot crash or index out of range. Closing it fully needs the writer to count before AND after (an odd generation while
// writing), which is a change to the model's mutators, not to this reader: a follow-up for OS-004a, listed in the spec.
//
// A row is one step of one track. Each track has its own cursor and length (polymetric patterns are legal in the model), so rows are read per track, as
// each track's row starts: 16 x 7 bytes = 112 bytes at most for all tracks of one step.

#include <o3os/model/project.h>

#include <cstdint>
#include <cstring>

namespace o3os::seq {

enum class RowStatus : std::uint8_t { Ok, NoPattern, Torn };

// The check around a copy, over two functions so that a test can land an edit between them. `gen()` returns the generation (0 for no such pattern),
// `copy()` does the copy.
template <typename Gen, typename Copy>
RowStatus checked_copy(Gen&& gen, Copy&& copy) noexcept {
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    const std::uint32_t g0 = gen();
    if (g0 == 0u) {
      return RowStatus::NoPattern;
    }
    copy();
    if (gen() == g0) {
      return RowStatus::Ok;
    }
  }
  return RowStatus::Torn;
}

// Step `pos` of `track` in pattern `id`. `out` is the empty step unless the status is Ok.
inline RowStatus read_step(const model::Project& p, std::uint32_t id, std::uint32_t track, std::uint32_t pos, model::Step& out) noexcept {
  out = model::kEmptyStep;
  const model::Pattern* pat = p.pattern(id);
  if (pat == nullptr || track >= model::kMaxTracks || pos >= model::kMaxPatternLength) {
    return RowStatus::NoPattern;
  }
  model::Step tmp = model::kEmptyStep;
  const RowStatus st = checked_copy([&p, id] { return p.pattern_generation(id); }, [pat, track, pos, &tmp] { std::memcpy(&tmp, &pat->steps[track][pos], sizeof(model::Step)); });
  if (st == RowStatus::Ok) {
    out = tmp;
  }
  return st;
}

// The length of a track in a pattern (1..128), or `fallback` when the pattern does not exist. One byte, no check needed: a torn length is one of 1..128
// or a value the model never stores, and the caller clamps.
inline std::uint32_t track_length(const model::Project& p, std::uint32_t id, std::uint32_t track, std::uint32_t fallback) noexcept {
  const model::Pattern* pat = p.pattern(id);
  if (pat == nullptr || track >= model::kMaxTracks) {
    return fallback;
  }
  const std::uint32_t len = pat->track_length[track];
  return len < model::kMinPatternLength ? model::kMinPatternLength : (len > model::kMaxPatternLength ? model::kMaxPatternLength : len);
}

}  // namespace o3os::seq
