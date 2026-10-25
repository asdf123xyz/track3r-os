// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// DUPLICATE, EXPAND and SHRINK (PAT-OPS-A, docs/specs/PAT-OPS.md; OS-009 D8), pure functions over ONE track's column of 128 steps and its length. NEW COMMUNITY OS BEHAVIOUR (the operations are
// documented in the release notes and manual of the official firmware, not verified on a unit: OS-009:151). The caller walks the tracks of its scope and writes the result through the Editor as one undo unit; nothing here changes its input.
//
//   DUPLICATE  length 2L; steps L..2L-1 repeat steps 0..L-1.
//   EXPAND     length 2L; step i goes to 2i and the odd steps are empty.
//   SHRINK     length ceil(L/2); step i takes step 2i, the steps from the new length up to the old one are cleared. shrink(expand(p)) == p for the steps of p (its length and steps 0..L-1).
//   All three leave the steps past both lengths as they were. A result over kMaxPatternLength is refused, never clipped.
//
// No heap, no globals.

#include <o3os/model/limits.h>
#include <o3os/model/step.h>

#include <cstdint>

namespace o3os::model::ops {

enum class Op : std::uint8_t { Duplicate = 0, Expand = 1, Shrink = 2 };

// The length a track of `len` steps has after `op` (the unchecked result: DUPLICATE and EXPAND of 65..128 give more than kMaxPatternLength).
constexpr unsigned result_length(Op op, unsigned len) noexcept { return op == Op::Shrink ? (len + 1u) / 2u : 2u * len; }

// `in` and `out` are the whole 128-step column (distinct arrays), `len` the track's length (1..128). Writes the column after the operation into `out` and its length into `out_len`; false (nothing
// written) for a `len` outside 1..128 and for a result over kMaxPatternLength steps.
inline bool apply(Op op, const Step* in, unsigned len, Step* out, unsigned& out_len) noexcept {
  if (len < kMinPatternLength || len > kMaxPatternLength || result_length(op, len) > kMaxPatternLength) {
    return false;
  }
  out_len = result_length(op, len);
  for (unsigned i = 0; i < kMaxPatternLength; ++i) {
    out[i] = in[i];
  }
  switch (op) {
    case Op::Duplicate:
      for (unsigned i = 0; i < len; ++i) {
        out[len + i] = in[i];
      }
      break;
    case Op::Expand:
      for (unsigned i = 0; i < len; ++i) {
        out[2u * i] = in[i];
        out[2u * i + 1u] = kEmptyStep;
      }
      break;
    case Op::Shrink:
      for (unsigned i = 0; i < len; ++i) {  // (one loop to len <= 128, as the other two: the first out_len steps take every second one, the rest up to the old length is cleared)
        out[i] = i < out_len ? in[2u * i] : kEmptyStep;
      }
      break;
  }
  return true;
}

}  // namespace o3os::model::ops
