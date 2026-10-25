// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/model/limits.h>
#include <o3os/model/step.h>

#include <cstddef>
#include <cstdint>

namespace o3os::model {

// One pattern of the pool: 16 track slots of 128 steps each, whatever the length. Every track has its own
// length (1..128 steps that play), as in the OG file (PAR-007: a length byte per track); nothing
// requires them equal. Steps past a track's length are kept, so lengthening it brings them back.
// There is no pattern-level length: Project::pattern_length() derives one (the longest of the active tracks).
// Read it freely through Project::pattern(); it changes only through the Project mutators (which bump its
// generation). Alignment 1 throughout (see step.h), so sizeof is the same on the host and on the MK66.
struct Pattern {
  std::uint8_t id;  // 1..255, 0 = this pool slot is free
  std::uint8_t track_length[kMaxTracks];  // kMinPatternLength..kMaxPatternLength
  char name[kPatternNameMax + 1];
  Step steps[kMaxTracks][kMaxPatternLength];
};

static_assert(alignof(Pattern) == 1, "Pattern has no alignment requirement");
static_assert(sizeof(Pattern) == 1 + kMaxTracks + (kPatternNameMax + 1) + kMaxTracks * kMaxPatternLength * 7,
              "Pattern has no padding");

}  // namespace o3os::model
