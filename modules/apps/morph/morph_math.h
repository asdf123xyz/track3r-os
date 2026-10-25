// SPDX-License-Identifier: MIT
//
// Morph (OS-H3B, new Community OS behaviour): the arithmetic, with no OS state and no libc, so that the module and its host test share it.
//   A = A0 + (B - A0) x POS / 100 for a continuous parameter; a STEPPED or ENUM parameter takes B's value from POS 50 and A0's below it. The ends are exact: POS 0 gives A0 and POS 100 gives B to the
//   last bit (so RESET puts A back as it was), whatever the float rounding of the blend in between.
// Single-precision floats only.
#pragma once

#include <stdint.h>

namespace morph {

constexpr uint32_t kMaxPos = 100;
constexpr uint32_t kSwitchPos = 50;

inline float morph_value(float a0, float b, float mn, float mx, bool whole, uint32_t pos_pct) {
  if (pos_pct == 0u) {
    return a0;
  }
  if (pos_pct >= kMaxPos) {
    return b;
  }
  if (whole) {
    return pos_pct >= kSwitchPos ? b : a0;
  }
  float v = a0 + (b - a0) * (static_cast<float>(pos_pct) * 0.01f);
  v = v < mn ? mn : v;
  v = v > mx ? mx : v;
  return v;
}

}  // namespace morph
