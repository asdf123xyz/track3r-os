// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace o3os::formats {

// OS-BUG-PASTE24 (docs/specs/OS-BUG-PASTE24.md, D2): the instrument level (0..100, the 1.2+ law: 50 is 0 dB, 0.48 dB a step, level 1 is -24 dB) that
// plays a tracker volume 0..64 of a MOD, S3M or MED sample at the volume's own ratio in dB: 64 is level 50, 32 is level 37 (-6.24 dB; 6.02 wanted).
// Volumes over 64 count as 64; volumes 1..4 (under -24 dB) are level 1; 0 is level 0 (silence). Import time only (once per instrument).
std::uint8_t volume_level_of_tracker(unsigned tracker_volume) noexcept;

}  // namespace o3os::formats
