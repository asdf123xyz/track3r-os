// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace o3os::app {

// Static demo data for the Song screen (OS-002b): the order list, nothing stored, loaded or saved, and nothing here is a
// persistent format. The real song model (and with it editing the order) is OS-004.
inline constexpr int kSongMaxLength = 64;  // positions of the demo order list
inline constexpr int kSongPatterns = 12;   // demo patterns, numbered 1..12 in the order list
inline constexpr int kSongNameChars = 12;  // longest pattern name

struct SongPattern {
  char name[kSongNameChars + 1];
  std::uint8_t steps;  // length in steps (1..255)
};

struct SongData {
  std::uint8_t order[kSongMaxLength];  // pattern number (1..kSongPatterns) at each position
  int length;                          // positions in use (1..kSongMaxLength)
  int play;                            // the play position marker (a fixed demo position until there is a transport)
  SongPattern patterns[kSongPatterns];
};

// The positions in use, always 0..kSongMaxLength: what the views and the actions index the order list by, so a length that
// is out of range (a model that is not this demo) cannot make them read past the table.
constexpr int song_length(const SongData& data) noexcept {
  return data.length < 0 ? 0 : (data.length > kSongMaxLength ? kSongMaxLength : data.length);
}

// Fills `data` with the demo song (deterministic): 24 positions, so the 16 visible rows scroll.
void fill_song_demo(SongData& data) noexcept;

// The pattern of a position; a pattern number out of range gives pattern 1's data, so a screen never indexes outside.
const SongPattern& song_pattern(const SongData& data, int position) noexcept;

}  // namespace o3os::app
