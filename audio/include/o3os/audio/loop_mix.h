// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The loop player's inner loop (OS-CPUOPT1, L1; loop_player.h is the player). Flash code (loop_player.cpp, -O3), called once per run of a block.

#include <cstdint>

namespace o3os::audio::loop_mix {

// Adds the loop's frames into the interleaved TX block: for i < n, both channels of frame i of `out` get saturating `(p[i] * g_i) >> 16`, where g_0 = g and g_(i+1) = g_i + step (Q16, 0..65536: the
// player holds its gain to unity, so the product is inside int16). Returns the gain after the n-th frame (g + n * step). Reads exactly p[0..n): two frames a word when `p` is 4-aligned (or after one
// scalar frame when it is not), never past the n-th. `out` is a frame array of 2 n int16.
std::int32_t take(std::int16_t* out, const std::int16_t* p, std::uint32_t n, std::int32_t g, std::int32_t step) noexcept;

}  // namespace o3os::audio::loop_mix
