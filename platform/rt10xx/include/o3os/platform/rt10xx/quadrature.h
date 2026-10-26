// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A quadrature decoder for the encoder (OS-PORT-RT10XX phase 4): the A and B levels as sampled, a 16-bit count that wraps (the MK66's FTM2
// counter, which the scanner's detent accumulator reads: platform/mk66 input/detent.h). The Gray sequence 00 -> 01 -> 11 -> 10 -> 00 counts
// up, the reverse down; a sample where both levels changed is a step the sampling missed: it is counted in `skipped` and not guessed.
// Pure: the image samples it from the main loop (src/input.cpp), the host test drives it with sequences.

#include <cstdint>

namespace o3os::platform::rt10xx::input {

class Quadrature {
 public:
  void reset(bool a, bool b) noexcept {
    state_ = code(a, b);
    primed_ = true;
  }
  void sample(bool a, bool b) noexcept {
    const std::uint8_t now = code(a, b);
    if (!primed_) {
      state_ = now;
      primed_ = true;
      return;
    }
    if (now == state_) {
      return;
    }
    const std::int8_t step = kStep[(state_ << 2) | now];
    if (step == 0) {
      ++skipped_;
    } else {
      count_ = static_cast<std::uint16_t>(count_ + static_cast<std::uint16_t>(step));
    }
    state_ = now;
  }
  std::uint16_t count() const noexcept { return count_; }
  std::uint32_t skipped() const noexcept { return skipped_; }

 private:
  // The position in the Gray sequence of each A/B code: 00 = 0, 01 = 1, 11 = 2, 10 = 3.
  static constexpr std::uint8_t code(bool a, bool b) noexcept { return static_cast<std::uint8_t>((a ? 2u : 0u) | (b ? 1u : 0u)); }
  // kStep[(previous << 2) | now] for the codes AB: +1 along 00 01 11 10, -1 back, 0 for no change or a change of both.
  static constexpr std::int8_t kStep[16] = {
      0, +1, -1, 0,   // from 00: to 01 up, to 10 down, to 11 skipped
      -1, 0, 0, +1,   // from 01: to 00 down, to 11 up, to 10 skipped
      +1, 0, 0, -1,   // from 10: to 00 up, to 11 down, to 01 skipped
      0, -1, +1, 0,   // from 11: to 01 down, to 10 up, to 00 skipped
  };
  std::uint8_t state_ = 0;
  bool primed_ = false;
  std::uint16_t count_ = 0;
  std::uint32_t skipped_ = 0;
};

}  // namespace o3os::platform::rt10xx::input
