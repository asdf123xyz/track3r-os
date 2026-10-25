// SPDX-License-Identifier: MIT
//
// Humanize (OS-H3B, new Community OS behaviour): the arithmetic, with no OS state and no libc, so that the module and its host test share it.
//   - The draw: per track of the target, xorshift32 seeded by the user's SEED and the track's place in the target, TWO draws for every step that has a note (a delay, then a volume jitter) whether or
//     not they are used, so that a step's result never depends on the FX or volume of another step, and the same settings always give the same steps.
//   - TIME 0..100: the delay is uniform in 0..TIME, written as FX 2 Micro-move (values 0..100; it delays only, as the FX does).
//   - VOL 0..64: the jitter is uniform in -VOL..+VOL, added to the volume of a step that CARRIES one, clamped to 0..127. A step with no volume (unset: the sequencer and the voices give it the instrument's own volume, which a written velocity would replace) is left alone. With VOL 0 no volume is written. Repeated APPLYs / VARYs jitter the volume they find, so it random-walks.
//   - The FX slot: a step that already has Micro-move gets that slot's value replaced; otherwise the delay goes in an empty slot, FX2 first, then FX1. A step that needs a delay (it drew one above 0)
//     and has both slots taken keeps its timing and is counted.
// Integer only.
#pragma once

#include <stdint.h>

namespace humanize {

constexpr uint32_t kFxMicroMove = 2;
constexpr uint32_t kMaxTime = 100;
constexpr uint32_t kMaxVol = 64;
constexpr uint32_t kNoBlockRows = 16;  // with no block: this many rows from the cursor row

struct Setup {
  uint32_t time, vol, seed, stream;  // stream: the track's place in the target (0..15)
};

struct Result {
  uint32_t touched;  // steps with a note
  uint32_t skipped;  // steps that drew a delay and had no free FX slot
};

// What one track writes over `n` rows (n <= 128): `cur` holds the steps as they are, `out` receives the steps to write with the field mask VOL | FX1 | FX2 (the notes and the instrument are never in the
// mask). A step without a note is copied as it is. `Draw` is o3edit::Draw (the template keeps this header free of the SDK).
template <class Step, class Draw>
Result humanize_steps(const Setup& u, const Step* cur, Step* out, uint32_t n) {
  Draw d;
  d.seed(u.seed, u.stream);
  Result r{0, 0};
  for (uint32_t j = 0; j < n; ++j) {
    out[j] = cur[j];
    const uint32_t note = cur[j].note;
    if (note < 1u || note > 128u) {
      continue;
    }
    ++r.touched;
    const uint32_t delay = d.next() % (u.time + 1u);
    const uint32_t jitter_draw = d.next() % (2u * u.vol + 1u);
    if (u.vol != 0u && cur[j].vol != 255u) {
      const int32_t base = static_cast<int32_t>(cur[j].vol);
      int32_t v = base + static_cast<int32_t>(jitter_draw) - static_cast<int32_t>(u.vol);
      v = v < 0 ? 0 : (v > 127 ? 127 : v);
      out[j].vol = static_cast<uint8_t>(v);
    }
    if (u.time == 0u) {
      continue;
    }
    if (cur[j].fx1_type == kFxMicroMove) {
      out[j].fx1_value = static_cast<uint8_t>(delay);
    } else if (cur[j].fx2_type == kFxMicroMove) {
      out[j].fx2_value = static_cast<uint8_t>(delay);
    } else if (delay != 0u) {
      if (cur[j].fx2_type == 0u) {
        out[j].fx2_type = kFxMicroMove;
        out[j].fx2_value = static_cast<uint8_t>(delay);
      } else if (cur[j].fx1_type == 0u) {
        out[j].fx1_type = kFxMicroMove;
        out[j].fx1_value = static_cast<uint8_t>(delay);
      } else {
        ++r.skipped;
      }
    }
  }
  return r;
}

}  // namespace humanize
