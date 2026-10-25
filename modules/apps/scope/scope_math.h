// SPDX-License-Identifier: MIT
//
// OS-H2b: the pure arithmetic of the scope view (the trigger and the point reduction), a header so that the host test checks it directly, like tapcalc_math.h.
#include <stdint.h>

namespace scope_math {

// The rising zero-crossing trigger. Looks in x[0 .. n) for the first sample index i in [pre, last] with x[i - 1] < 0 <= x[i] that comes after the signal has been below -h (so that noise
// around zero does not fire it), h being an eighth of the window's peak. Returns i, or -1 when there is no such crossing or the window is too quiet to trigger (peak < 32 of 32768): the
// caller then free-runs (draws the newest samples as they are).
inline int32_t find_trigger(const int16_t* x, uint32_t n, uint32_t pre, uint32_t last) {
  if (x == nullptr || n < 2u) {
    return -1;
  }
  int32_t peak = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const int32_t a = x[i] < 0 ? -static_cast<int32_t>(x[i]) : static_cast<int32_t>(x[i]);
    peak = a > peak ? a : peak;
  }
  if (peak < 32) {
    return -1;
  }
  const int32_t h = peak / 8;
  last = last < n - 1u ? last : n - 1u;
  bool armed = false;
  for (uint32_t i = 0; i <= last; ++i) {
    if (x[i] < -h) {
      armed = true;
    } else if (armed && i >= 1u && i >= pre && x[i - 1u] < 0 && x[i] >= 0) {
      return static_cast<int32_t>(i);
    }
  }
  return -1;
}

// The sample of x[lo .. hi) with the largest magnitude (the first of equals): a view of a long span with few points keeps the peaks. hi > lo.
inline int16_t peak_of(const int16_t* x, uint32_t lo, uint32_t hi) {
  int16_t best = x[lo];
  int32_t best_a = best < 0 ? -static_cast<int32_t>(best) : static_cast<int32_t>(best);
  for (uint32_t i = lo + 1u; i < hi; ++i) {
    const int32_t a = x[i] < 0 ? -static_cast<int32_t>(x[i]) : static_cast<int32_t>(x[i]);
    if (a > best_a) {
      best_a = a;
      best = x[i];
    }
  }
  return best;
}

}  // namespace scope_math
