// SPDX-License-Identifier: MIT
//
// Tap Calc (OS-TAPCALC, new Community OS behaviour): the arithmetic of the app, with no state of the OS and no libc, so that the module and its host test share it.
//   - Tapper: tap times in ms -> BPM x10. The tempo is taken over the last N intervals (N = 4..8) as (last tap - tap N intervals earlier) / N, which is the mean of those
//     intervals and, because the middle taps cancel, is as exact as the two end times are: a clock that is off by one frame costs at most one frame over the whole span.
//     A gap of more than 2 s starts a new series. The result is rounded to one decimal and kept in 20.0..300.0.
//   - note_tenths / hz_hundredths: the length of 1/1..1/32, straight, dotted (x 3/2) or triplet (x 2/3), at a BPM x10, in 0.1 ms; and its rate in 0.01 Hz.
//   - put_fixed: "<int>.<frac>" from a scaled integer.
// Single-precision float only (the FPU of the MK66; a double would be a library call).
#pragma once

#include <stdint.h>

namespace tapcalc {

constexpr uint32_t kMinBpm10 = 200;   // 20.0 BPM
constexpr uint32_t kMaxBpm10 = 3000;  // 300.0 BPM
constexpr uint32_t kResetMs = 2000;   // a longer pause between two taps starts a new series
constexpr uint32_t kMaxIntervals = 8;
constexpr uint32_t kMinIntervals = 4;

constexpr uint32_t clamp_bpm10(uint32_t v) { return v < kMinBpm10 ? kMinBpm10 : (v > kMaxBpm10 ? kMaxBpm10 : v); }

struct Tapper {
  uint32_t t[kMaxIntervals + 1];  // the times (ms) of the last taps of this series, oldest first
  uint32_t n;                     // how many are held (0..kMaxIntervals + 1)

  void clear() { n = 0; }
  // A tap at `now_ms` (wrap-safe: only differences are used). True when it began a new series: the first tap, or one more than kResetMs after the last.
  bool tap(uint32_t now_ms) {
    const bool fresh = n == 0u || now_ms - t[n - 1u] > kResetMs;
    if (fresh) {
      n = 0;
    }
    if (n == kMaxIntervals + 1u) {
      for (uint32_t i = 1; i < n; ++i) {
        t[i - 1u] = t[i];
      }
      --n;
    }
    t[n++] = now_ms;
    return fresh;
  }
  uint32_t intervals() const { return n != 0u ? n - 1u : 0u; }
  // BPM x 10 over the last `window` intervals (or all there are, when fewer); 0 with fewer than two taps. Taps in the same millisecond give the top of the range.
  uint32_t bpm10(uint32_t window) const {
    uint32_t k = n != 0u ? n - 1u : 0u;
    if (window > kMaxIntervals) {
      window = kMaxIntervals;
    }
    if (k > window) {
      k = window;
    }
    if (k == 0u) {
      return 0;
    }
    const uint32_t span = t[n - 1u] - t[n - 1u - k];
    if (span == 0u) {
      return kMaxBpm10;
    }
    // 60000 ms/min * 10 / (span / k); float: 24 bits hold every value here exactly enough (the quotient is rounded to a tenth)
    const float v = 600000.0f * static_cast<float>(k) / static_cast<float>(span);
    if (v >= static_cast<float>(kMaxBpm10)) {
      return kMaxBpm10;
    }
    return clamp_bpm10(static_cast<uint32_t>(v + 0.5f));
  }
};

constexpr uint32_t kDivisions = 6;  // 1/1 1/2 1/4 1/8 1/16 1/32
constexpr uint32_t kVariants = 3;   // straight, dotted, triplet
constexpr float kDivInQuarters[kDivisions] = {4.0f, 2.0f, 1.0f, 0.5f, 0.25f, 0.125f};
constexpr float kVariantFactor[kVariants] = {1.0f, 1.5f, 2.0f / 3.0f};

// The length in ms of division `div` (0 = 1/1 .. 5 = 1/32), variant `var` (0 straight, 1 dotted, 2 triplet) at `bpm10` BPM x 10 (clamped to the range). A delay set to it echoes in time.
inline float note_ms(uint32_t bpm10, uint32_t div, uint32_t var) {
  const float quarter_ms = 600000.0f / static_cast<float>(clamp_bpm10(bpm10));
  return quarter_ms * kDivInQuarters[div < kDivisions ? div : kDivisions - 1u] * kVariantFactor[var < kVariants ? var : 0u];
}
inline uint32_t note_tenths(uint32_t bpm10, uint32_t div, uint32_t var) { return static_cast<uint32_t>(note_ms(bpm10, div, var) * 10.0f + 0.5f); }
// The same note as a rate: an LFO that runs once per note, in 0.01 Hz.
inline uint32_t hz_hundredths(uint32_t bpm10, uint32_t div, uint32_t var) { return static_cast<uint32_t>(100000.0f / note_ms(bpm10, div, var) + 0.5f); }

// "<value / scale>.<fraction>" at out[at...], `decimals` digits (1 or 2); returns the next index. No terminator.
inline uint32_t put_fixed(char* out, uint32_t at, uint32_t value, uint32_t decimals) {
  uint32_t scale = 1;
  for (uint32_t i = 0; i < decimals; ++i) {
    scale *= 10u;
  }
  uint32_t whole = value / scale;
  char digits[10];
  uint32_t n = 0;
  do {
    digits[n++] = static_cast<char>('0' + whole % 10u);
    whole /= 10u;
  } while (whole != 0u && n < 10u);
  while (n != 0u) {
    out[at++] = digits[--n];
  }
  out[at++] = '.';
  uint32_t frac = value % scale;
  for (uint32_t d = scale / 10u; d != 0u; d /= 10u) {
    out[at++] = static_cast<char>('0' + frac / d);
    frac %= d;
  }
  return at;
}

}  // namespace tapcalc
