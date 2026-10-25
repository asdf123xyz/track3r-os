// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The one-line text builder of the loaders' summaries (og_folder.cpp, og_samples.cpp): bounded, always terminated, numbers by hand (no printf).
#include <cstddef>
#include <cstdint>

namespace o3os::app {

class Line {
 public:
  Line(char* out, std::size_t cap) noexcept : out_(out), cap_(cap) {
    if (cap_ != 0u) {
      out_[0] = '\0';
    }
  }
  Line& str(const char* s) noexcept {
    while (*s != '\0' && len_ + 1u < cap_) {
      out_[len_++] = *s++;
    }
    if (cap_ != 0u) {
      out_[len_] = '\0';
    }
    return *this;
  }
  Line& dec(std::uint32_t v) noexcept {
    char digits[10];
    unsigned n = 0;
    do {
      digits[n++] = static_cast<char>('0' + v % 10u);
      v /= 10u;
    } while (v != 0u);
    while (n != 0u && len_ + 1u < cap_) {
      out_[len_++] = digits[--n];
    }
    if (cap_ != 0u) {
      out_[len_] = '\0';
    }
    return *this;
  }
  // `digits` lower-case hex digits of v (the low ones), zero padded.
  Line& hex(std::uint32_t v, unsigned digits) noexcept {
    for (unsigned i = digits; i != 0u && len_ + 1u < cap_; --i) {
      out_[len_++] = "0123456789abcdef"[(v >> (4u * (i - 1u))) & 15u];
    }
    if (cap_ != 0u) {
      out_[len_] = '\0';
    }
    return *this;
  }
  // v / 1000 with three decimals: 504 is "0.504", 1000 is "1.000".
  Line& milli(std::uint32_t v) noexcept {
    dec(v / 1000u).str(".");
    const std::uint32_t f = v % 1000u;
    return str(f < 100u ? "0" : "").str(f < 10u ? "0" : "").dec(f);
  }
  std::size_t size() const noexcept { return len_; }

 private:
  char* out_;
  std::size_t cap_;
  std::size_t len_ = 0;
};

}  // namespace o3os::app
