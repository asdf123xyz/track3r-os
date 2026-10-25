// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The INPUT TEST boot chord (OS-DIAG1, INP-8): Shift + F1 + F8 held while the unit powers up opens the hidden INPUT TEST screen. Pure, so the
// decision is tested on the host (tests/mk66_boot_chord_test.cpp); app_main.cpp supplies the scan step and the key reading.
//
// The chord is read from the key scanner's debounced state, which is only as old as the last service() pass: a key held at power-up reaches the
// chip's FIFO at its first scan, and the scanner accepts it one pass after it has read it. How long that takes on the unit is not known (the
// first read was a fixed 40 ms window and the chord did not open; nothing in the code settled why), so the read is a bounded POLL: the scanner is
// stepped once a millisecond until all three keys are down or kBootChordWindowMs has passed. A chord that is complete ends it at once; a boot
// with no chord pays the whole window once, with the display serviced from the idle hook while it waits (boot::delay_ms).

#include <cstdint>

namespace o3os::platform::mk66 {

inline constexpr unsigned kBootChordKeys = 3;                               // Shift, F1, F8 (app_main.cpp kChordButtons)
inline constexpr unsigned kBootChordAll = (1u << kBootChordKeys) - 1u;      // bit i of `held` = chord key i is down
inline constexpr std::uint32_t kBootChordWindowMs = 150;                    // the longest the poll waits (100..200 ms by the task; audio starts after it)

constexpr bool boot_chord_complete(unsigned held) noexcept { return held == kBootChordAll; }

// OS-USBDISK U1c: the boot disk chord, Shift + F1 + F7 (maintainer decision 2026-10-16). It has its own mask and a lower precedence than INPUT TEST: `held` is the INPUT TEST poll's reading (bit 0 Shift,
// bit 1 F1, bit 2 F8) and `f7` the scanner's F7 at the end of the same poll (the poll runs its whole window when INPUT TEST is not complete, so the scanner has seen every key held at power-up).
// The disk chord is exactly Shift and F1 with F8 up and F7 down: Shift + F1 + F7 + F8 is INPUT TEST (boot_chord_complete) and never both; a partial chord is neither.
inline constexpr unsigned kBootDiskMask = 0x3u;  // Shift + F1 in `held`, F8 (bit 2) clear; F7 comes separately
constexpr bool boot_disk_chord(unsigned held, bool f7) noexcept { return f7 && held == kBootDiskMask; }

// Steps (one millisecond: wait, scan) until the chord is complete or the window has passed; `held()` returns the chord keys' bits and is read once
// before the first step (a chord already complete costs nothing) and after each one. Returns the steps taken; `held` is left at the last reading.
template <typename Step, typename Held>
std::uint32_t poll_boot_chord(Step&& step, Held&& read_held, unsigned& held) noexcept {
  std::uint32_t steps = 0;
  held = read_held();
  while (!boot_chord_complete(held) && steps < kBootChordWindowMs) {
    step();
    ++steps;
    held = read_held();
  }
  return steps;
}

}  // namespace o3os::platform::mk66
