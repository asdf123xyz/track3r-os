// SPDX-License-Identifier: GPL-3.0-or-later
//
// The key matrices of the Tracker OG's two TCA8418 scanners (input_map.h): every key of each matrix is a pad, a button or unwired, the
// 48 pads and the 33 buttons are each reached exactly once, the inverse functions are inverses, and the corners are the board's. Runs on the host and on QEMU's Cortex-M4F (`qemu_mk66`).
#include <doctest/doctest.h>

#include <o3os/platform/mk66/input/input_map.h>

#include <cstdint>

// This tree builds without exceptions, so doctest's REQUIRE is unavailable: MUST checks and leaves the test case on failure.
#define MUST(cond)               \
  do {                           \
    const bool must_ok_ = (cond); \
    CHECK(must_ok_);             \
    if (!must_ok_) {             \
      return;                    \
    }                            \
  } while (0)

namespace input = o3os::platform::mk66::input;

TEST_CASE("input map: the pads are reached exactly once and the rest of the pad matrix is unwired") {
  unsigned seen[input::kPadCount] = {};
  unsigned unwired = 0;
  for (unsigned key = 0; key < input::kKeyCount; ++key) {
    const std::uint8_t pad = input::pad_of_key(key);
    if (pad == input::kNoKey) {
      ++unwired;
    } else {
      MUST(pad < input::kPadCount);
      ++seen[pad];
    }
  }
  for (unsigned pad = 0; pad < input::kPadCount; ++pad) {
    CHECK_MESSAGE(seen[pad] == 1u, "pad " << pad);
  }
  CHECK(unwired == input::kKeyCount - input::kPadCount);  // 80 positions, 48 pads
}

TEST_CASE("input map: the buttons are reached exactly once and the rest of the button matrix is unwired") {
  unsigned seen[input::kButtonCount] = {};
  unsigned unwired = 0;
  for (unsigned key = 0; key < input::kKeyCount; ++key) {
    const std::uint8_t button = input::button_of_key(key);
    if (button == input::kNoKey) {
      ++unwired;
    } else {
      MUST(button < input::kButtonCount);
      ++seen[button];
    }
  }
  for (unsigned button = 0; button < input::kButtonCount; ++button) {
    CHECK_MESSAGE(seen[button] == 1u, "button " << button);
  }
  CHECK(unwired == input::kKeyCount - input::kButtonCount);  // 80 positions, 33 buttons
}

TEST_CASE("input map: the inverse functions are inverses") {
  for (unsigned pad = 0; pad < input::kPadCount; ++pad) {
    CHECK(input::pad_of_key(input::key_of_pad(pad)) == pad);
  }
  for (unsigned button = 0; button < input::kButtonCount; ++button) {
    CHECK(input::button_of_key(input::key_of_button(button)) == button);
  }
  // a key out of the matrix is nothing
  CHECK(input::pad_of_key(input::kKeyCount) == input::kNoKey);
  CHECK(input::button_of_key(input::kKeyCount) == input::kNoKey);
  CHECK(input::pad_of_key(255) == input::kNoKey);
}

TEST_CASE("input map: the corners are the board's") {
  // Key = row * 10 + column. Pad matrix: pad row 0 on matrix row 0, pad columns 10 and 11 on matrix rows 4..7, columns 9 and 8.
  CHECK(input::pad_of_key(0) == 0);    // row 0, column 0
  CHECK(input::pad_of_key(9) == 9);    // row 0, column 9
  CHECK(input::pad_of_key(10) == 12);  // row 1, column 0: the second pad row starts at 12
  CHECK(input::pad_of_key(39) == 45);  // row 3, column 9
  CHECK(input::pad_of_key(48) == 11);  // row 4, column 8: pad 11 (the last of the first row)
  CHECK(input::pad_of_key(49) == 10);  // row 4, column 9: pad 10
  CHECK(input::pad_of_key(78) == 47);  // row 7, column 8
  CHECK(input::pad_of_key(79) == 46);  // row 7, column 9
  CHECK(input::pad_of_key(40) == input::kNoKey);  // row 4, column 0: no pad
  CHECK(input::pad_of_key(47) == input::kNoKey);  // row 4, column 7
  // Control matrix: F1..F8 are row 7, columns 7..0; the screen buttons run row by row from code 8.
  CHECK(input::button_of_key(77) == 0);   // F1 (row 7, column 7)
  CHECK(input::button_of_key(70) == 7);   // F8 (row 7, column 0)
  CHECK(input::button_of_key(0) == 8);    // Params
  CHECK(input::button_of_key(4) == 12);   // Master
  CHECK(input::button_of_key(10) == 13);  // SampleLoad
  CHECK(input::button_of_key(44) == 32);  // Right (row 4, column 4)
  CHECK(input::button_of_key(5) == input::kNoKey);   // row 0, column 5: nothing
  CHECK(input::button_of_key(78) == input::kNoKey);  // row 7, column 8
}

TEST_CASE("input map: the encoder has four counts a detent (96 per turn, 24 detents)") {
  CHECK(input::kEncoderCountsPerDetent * 24 == 96);
}
