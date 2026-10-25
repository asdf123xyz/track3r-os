// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The sequencer's random numbers (Chance, Random note / instrument / velocity / value, the random play mode and roll direction), deterministic: a
// xorshift32 that advances only when a row asks, never per block or per sample, and is reseeded when the transport starts, so the same project plays and
// exports the same way every time and in every block size (ADR 0013 D8, offline render determinism). The distributions are uniform over the range; how the official
// firmware draws its numbers is an open research question.

#include <cstdint>

namespace o3os::seq {

class Rng {
 public:
  explicit constexpr Rng(std::uint32_t seed = 0x9E3779B9u) noexcept : s_(seed != 0u ? seed : 0x9E3779B9u) {}
  void seed(std::uint32_t seed) noexcept { s_ = seed != 0u ? seed : 0x9E3779B9u; }
  std::uint32_t next() noexcept {
    s_ ^= s_ << 13;
    s_ ^= s_ >> 17;
    s_ ^= s_ << 5;
    return s_;
  }
  // Uniform in [min, max), min when the range is empty. (Modulo bias is below 1e-7 for ranges of this size.)
  std::int32_t range(std::int32_t min, std::int32_t max) noexcept {
    if (max <= min) {
      return min;
    }
    return min + static_cast<std::int32_t>(next() % static_cast<std::uint32_t>(max - min));
  }

 private:
  std::uint32_t s_;
};

}  // namespace o3os::seq
