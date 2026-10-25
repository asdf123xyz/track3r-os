// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The looper's overdub kernels (OS-LOOPER L2b, docs/specs/OS-LOOPER.md 3c.4 and 3c.6). Standalone functions in flash (loop_player.cpp, -O3), no state, no allocation, bounded loops: the
// session of L2c calls them once per render block, in the render context. Mono int16 throughout: the loop, its undo buffer and the input are arrays of frames.

#include <cstdint>

namespace o3os::audio::loop_ov {

// The budgets of one restore step (one render block, L2c): at most this many dirty blocks copied back, and this many bitmap halfwords (16 blocks each) read, whatever the bitmap holds. The step
// also stops at the session's cycle budget (LoopPlayer::kRestoreCycles); `diag bench looper` measures restore_step and restore_scan with exactly these.
constexpr std::uint32_t kRestoreStepCopies = 4;
constexpr std::uint32_t kRestoreStepWords = 32;

constexpr std::uint32_t kBlockFrames = 64;  // the copy-on-write grain: one bitmap bit covers 64 frames (SampleBank::undo_bitmap_samples)

// The read-modify-write: for i < n, loop[i] = ssat16(((loop[i] * fb) >> 16) + in[i]). `fb` is the FEEDBACK gain in Q16, 0..65536 (65536 = 100 %: the loop is kept as it is and the input is
// added; 0: the layer replaces); a value outside that range is clamped to it. `loop` and `in` are any halfword-aligned arrays that
// do not overlap; words are used where both are 4-aligned (after one scalar frame when `loop` is not), the packed saturating add is QADD16. Touches exactly loop[0..n) and in[0..n).
// loop_rmw_add is the fb = 65536 case without the multiplies (loop_rmw with 65536 gives the same result, bit for bit).
void loop_rmw(std::int16_t* loop, const std::int16_t* in, std::uint32_t n, std::int32_t fb) noexcept;
void loop_rmw_add(std::int16_t* loop, const std::int16_t* in, std::uint32_t n) noexcept;

// Copies n (<= 64) frames of one block: the copy-on-write copy (loop -> undo) and the restore (undo -> loop). `dst` and `src` are 4-aligned (a block starts at a multiple of 128 bytes of a
// 4-aligned buffer) and do not overlap.
void loop_block_copy(std::int16_t* dst, const std::int16_t* src, std::uint32_t n) noexcept;

// One bounded step of the restore. `bitmap` holds one bit per 64-frame block of the loop (bit b % 16 of halfword b / 16, set = the block was copied to `undo` and may have been written), `len`
// is the loop's frames. From block `from`, copies back at most `max_copies` dirty blocks (undo -> loop, the last block of a `len` that is not a multiple of 64 only as far as `len`), clears
// their bits, and looks at no more than `max_words` clean-or-dirty bitmap halfwords (16 blocks each), so a step costs at most max_copies block copies plus max_words reads whatever the bitmap holds.
// A budget of 0 counts as 1 (a step always makes progress). Never test the bitmap for zero to find the end: a stray bit above the last block is never cleared; the end is the returned cursor.
// Returns the block to continue from; (len + 63) / 64 when no dirty block is left.
std::uint32_t loop_restore_step(std::int16_t* loop, const std::int16_t* undo, std::uint16_t* bitmap, std::uint32_t len, std::uint32_t from, std::uint32_t max_copies,
                                std::uint32_t max_words) noexcept;

}  // namespace o3os::audio::loop_ov
