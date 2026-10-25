// SPDX-License-Identifier: MIT
//
// Mutate (OS-H3B, new Community OS behaviour): the arithmetic, with no OS state and no libc, so that the module and its host test share it.
//   new = old + (r - 0.5) x AMOUNT x (max - min), clamped to min..max, with r uniform in [0, 1) drawn from the user's SEED and the PARAMETER'S ID (not its place in the list), so the same SEED moves a
//   parameter the same way whichever GROUP is chosen. A STEPPED or ENUM parameter moves by the delta rounded to a whole number. AMOUNT is a percentage, 0..100.
// Single-precision floats only.
#pragma once

#include <stdint.h>

namespace mutate {

constexpr uint32_t kMaxAmount = 100;

// `whole`: the parameter is STEPPED or ENUM. `r` in [0, 1).
inline float mutate_value(float old, float mn, float mx, bool whole, uint32_t amount_pct, float r) {
  float delta = (r - 0.5f) * (static_cast<float>(amount_pct) * 0.01f) * (mx - mn);
  if (whole) {
    delta = static_cast<float>(static_cast<int32_t>(delta + (delta < 0.0f ? -0.5f : 0.5f)));
  }
  float v = old + delta;
  v = v < mn ? mn : v;
  v = v > mx ? mx : v;
  return v;
}

}  // namespace mutate
