// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What a set of stems holds (OS-WAVEXP slice B, docs/specs/OS-WAVEXP.md section 4.2): pure functions of the project, so the files a stem export will make are known before the first pass.
//
//   T01..T16   one audio track alone, post-fader, its sends off and every return silent. A track with no note in the range, a MIDI track and a track outside the active ones are skipped.
//   DELAY      the delay's return only (the dry mix off), fed by every unmuted track. Skipped when the delay can sound from nothing: its master mute is on, its master level is 0, or every
//   REVERB     instrument the range plays has a send of 0 on that bus (a module instrument is assumed to send). The reverb's the same.
//   MIX        the plain render. Always the last file of a set.
//
// The request of each part is the base request (a song, or one pattern) with the mixer fields of audio/offline.h filled in; the render is the engine's own, with the master limiter in for every
// part: a stem loud enough to drive the limiter alone is limited alone, so the stems sum to the MIX only while the limiter is idle. Main loop only; no allocation.

#include <o3os/audio/offline.h>
#include <o3os/model/project.h>

#include <cstddef>
#include <cstdint>

namespace o3os::app {

struct StemPart {
  enum class Kind : std::uint8_t { Track, Delay, Reverb, Mix };
  Kind kind = Kind::Mix;
  std::uint8_t track = 0;        // Track: 0-based
  audio::OfflineRequest rq{};
};

struct StemPlan {
  static constexpr unsigned kMax = model::kMaxTracks + 3u;  // T01..T16, DELAY, REVERB, MIX
  static constexpr std::size_t kLabelChars = 8;             // "REVERB" and the terminator
  unsigned count = 0;
  StemPart part[kMax];
};

// The parts of the stem set of `base` (What::Song or What::Pattern with its pattern; the other fields of `base` are kept in every part), in the order they are rendered and written.
void plan_stems(const model::Project& project, const audio::OfflineRequest& base, StemPlan& out) noexcept;

// The part's name inside a file name: "T01".."T16", "DELAY", "REVERB", "MIX" (`cap` is at least StemPlan::kLabelChars).
void stem_label(const StemPart& part, char* out, std::size_t cap) noexcept;

// Every label a set can hold, for the names a set must not collide with: the n-th of kLabelCount (T01..T16, DELAY, REVERB, MIX).
inline constexpr unsigned kStemLabelCount = StemPlan::kMax;
void stem_label_at(unsigned n, char* out, std::size_t cap) noexcept;

}  // namespace o3os::app
