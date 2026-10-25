// SPDX-License-Identifier: MIT
//
// OS-H2b: the text helpers of the two tap apps (TUNER, SCOPE / SPECTRUM): a module has no libc, so numbers are written by hand. Same functions as the Loudness VU app keeps for itself.
#include <stdint.h>

namespace o3text {

inline uint32_t put_text(char* out, uint32_t at, const char* text) {
  while (*text != '\0') {
    out[at++] = *text++;
  }
  return at;
}

inline uint32_t put_num(char* out, uint32_t at, uint32_t v) {
  char digits[10];
  uint32_t n = 0;
  do {
    digits[n++] = static_cast<char>('0' + v % 10u);
    v /= 10u;
  } while (v != 0u && n < 10u);
  while (n != 0u) {
    out[at++] = digits[--n];
  }
  return at;
}

// A signed whole number with its sign always shown: "+3", "-12", "0".
inline uint32_t put_signed(char* out, uint32_t at, int32_t v) {
  if (v == 0) {
    out[at++] = '0';
    return at;
  }
  out[at++] = v < 0 ? '-' : '+';
  return put_num(out, at, v < 0 ? 0u - static_cast<uint32_t>(v) : static_cast<uint32_t>(v));
}

}  // namespace o3text
