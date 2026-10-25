// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/pad_board.h>

#include <o3os/core/scales.h>

namespace o3os::app {

namespace {

// Pad -> position in the note table. The top row of pads is the last twelve notes.
constexpr std::uint8_t kPadBoard[PadBoard::kPads] = {
    36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,  //
    24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35,  //
    12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23,  //
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11,
};

}  // namespace

void PadBoard::set(std::uint8_t scale, std::uint8_t offset, std::uint8_t root) noexcept {
  if (offset > kMaxOffset) {
    offset = kMaxOffset;
  }
  unsigned temp = root;  // a wide temporary
  for (unsigned a = 0; a < kPads; ++a) {
    if (a > 11u && a % 12u == 0u) {
      if (offset < 12u) {
        temp = notes_[a - (12u - offset)];
      } else if (offset == 12u) {
        temp = static_cast<unsigned>(notes_[a - 1u]) + 1u;
      } else {
        temp = notes_[a - 7u];  // the highest offset: a row starts 5 places up
      }
    }
    // The first note of the scale at or above temp (bounded by `guard`: the root is in every scale, so 11 steps are the most).
    for (unsigned guard = 0; guard < 12u && !core::is_in_scale(static_cast<std::uint8_t>(temp), root, scale); ++guard) {
      ++temp;
    }
    notes_[a] = static_cast<std::uint8_t>(temp > kMaxNote ? kMaxNote : temp);
    if (temp < kMaxNote) {
      ++temp;
    }
  }
  scale_ = scale;
  offset_ = offset;
  root_ = root;
}

std::uint8_t PadBoard::note(unsigned pad) const noexcept {
  if (pad == kPreviewPad) {
    return 60;
  }
  return pad < kPads ? notes_[kPadBoard[pad]] : std::uint8_t{0};
}

unsigned PadBoard::pads_with_note(int note, std::uint8_t (&pads)[kPads]) const noexcept {
  unsigned count = 0;
  for (unsigned i = 0; i < kPads; ++i) {
    const bool hit = static_cast<int>(notes_[kPadBoard[i]]) == note;
    pads[i] = hit ? 1u : 0u;
    count += hit ? 1u : 0u;
  }
  return count;
}

}  // namespace o3os::app
