// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The play modes of the OG instrument file (OS-INSTUI S3 / S4): SamplerParams::play_mode 0..7 in the file's order and the words of the OS's pages. One table for the
// source page (F1), the Wave screen's mode list and the strip.

#include <cstdint>

namespace o3os::app {

inline constexpr std::uint8_t kPlayModeOneShot = 0;
inline constexpr std::uint8_t kPlayModeSlice = 4;
inline constexpr std::uint8_t kPlayModeBeatSlice = 5;  // the pads play the slices
inline constexpr std::uint8_t kPlayModeWavetable = 6;
inline constexpr std::uint8_t kPlayModeGranular = 7;
inline constexpr unsigned kPlayModeCount = 8;

// The mode's name in capitals: the strip, the Wave mode list and the notifications (the Params row's value uses the table's mixed-case names).
inline constexpr const char* kPlayModeNames[kPlayModeCount] = {"ONE SHOT", "LOOP FWD", "LOOP BWD", "PING-PONG", "SLICE", "BEAT SLICE", "WAVETABLE", "GRANULAR"};

constexpr bool play_mode_is_slice(unsigned mode) noexcept { return mode == kPlayModeSlice || mode == kPlayModeBeatSlice; }

}  // namespace o3os::app
