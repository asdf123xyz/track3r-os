// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/song_data.h>

namespace o3os::app {

namespace {

struct DemoPattern {
  const char* name;
  std::uint8_t steps;
};
constexpr DemoPattern kPatterns[kSongPatterns] = {
    {"INTRO", 64},    {"VERSE A", 64}, {"VERSE B", 64},    {"BUILD UP", 32}, {"DROP", 64},      {"BREAKDOWN", 64},
    {"FILL", 16},     {"BRIDGE", 48},  {"DROP TWO", 64},   {"OUTRO", 128},   {"ACID LOOP", 64}, {"THE END", 16},
};

// 24 positions: intro, two verses, a build into the drop, a breakdown and fill, the bridge and out.
constexpr std::uint8_t kOrder[] = {1, 1, 2, 2, 3, 3, 4, 5, 5, 5, 5, 7, 6, 6, 8, 4, 9, 9, 9, 9, 7, 10, 11, 12};
constexpr int kOrderLength = static_cast<int>(sizeof(kOrder) / sizeof(kOrder[0]));
static_assert(kOrderLength <= kSongMaxLength, "the demo order fits");

}  // namespace

void fill_song_demo(SongData& data) noexcept {
  data = SongData{};
  for (int i = 0; i < kSongPatterns; ++i) {
    int n = 0;
    for (; kPatterns[i].name[n] != '\0' && n < kSongNameChars; ++n) {
      data.patterns[i].name[n] = kPatterns[i].name[n];
    }
    data.patterns[i].name[n] = '\0';
    data.patterns[i].steps = kPatterns[i].steps;
  }
  for (int i = 0; i < kOrderLength; ++i) {
    data.order[i] = kOrder[i];
  }
  data.length = kOrderLength;
  data.play = 5;
}

const SongPattern& song_pattern(const SongData& data, int position) noexcept {
  const int p = position >= 0 && position < kSongMaxLength ? data.order[position] : 1;
  return data.patterns[p >= 1 && p <= kSongPatterns ? p - 1 : 0];
}

}  // namespace o3os::app
