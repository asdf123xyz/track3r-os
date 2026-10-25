// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Text helpers the sample screens share (private to app/src, OS-011a): a length as seconds, a card path. No heap, no <cstdio>.

#include <o3os/app/text_format.h>
#include <o3os/core/fixed_string.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace o3os::app {

// "1.250 s": `frames` at `rate` (0: 44,100 Hz, not recorded; a rate above 192 kHz is shown as 192 kHz: the arithmetic stays in 32 bits).
template <std::size_t N>
void append_seconds(core::FixedString<N>& s, std::uint32_t frames, std::uint32_t rate) noexcept {
  rate = rate == 0u ? 44100u : (rate > 192000u ? 192000u : rate);
  const std::uint32_t whole = frames / rate;
  const std::uint32_t ms = (frames % rate) * 1000u / rate;
  append_uint(s, whole);
  s.append(".");
  append_dec(s, ms, 3);
  s.append(" s");
}

// Joins `folder` (a card path without a leading or trailing '/', "" for the root) and `name` into `out` (`cap` bytes); false when it does not fit or `name` is empty.
inline bool path_join(char* out, std::size_t cap, const char* folder, const char* name) noexcept {
  const std::size_t fl = std::strlen(folder);
  const std::size_t nl = std::strlen(name);
  const std::size_t sep = fl != 0u ? 1u : 0u;
  if (nl == 0u || fl + sep + nl + 1u > cap) {
    return false;
  }
  std::memcpy(out, folder, fl);
  if (sep != 0u) {
    out[fl] = '/';
  }
  std::memcpy(out + fl + sep, name, nl + 1u);
  return true;
}

// The folder above `path` ("a/b" -> "a", "a" -> "", "" -> ""), in place.
inline void path_parent(char* path) noexcept {
  std::size_t n = std::strlen(path);
  while (n > 0u && path[n - 1u] != '/') {
    --n;
  }
  if (n > 0u) {
    --n;  // the '/'
  }
  path[n] = '\0';
}

}  // namespace o3os::app
