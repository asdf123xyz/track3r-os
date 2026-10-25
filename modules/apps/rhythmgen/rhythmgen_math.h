// SPDX-License-Identifier: MIT
//
// Rhythm Gen (OS-H3A, new Community OS behaviour): the arithmetic of the generator, with no OS state and no libc, so that the module and its host test share it.
//   - hit(): is row i of a cycle of `length` steps a hit of `hits` events? (i * hits) mod length < hits: the Euclid rule of the Fill dialog (model/pattern_fill.h euclid_hit), so both give the same
//     pattern. The cycle is then ROTATED: step k of the rotated cycle is step (k - rotate) mod length of the plain one (a hit moves `rotate` steps later).
//   - Draw: the probability. xorshift32 seeded from the user's SEED and the lane through an integer hash, so the same settings always give the same steps. ONE draw is made per hit, kept or not, so
//     raising PROB only ever adds hits to the pattern a lower PROB gave (the draws do not shift).
//   - lane_steps(): the steps a lane writes for a range of rows, from what is there.
// Integer only.
#pragma once

#include <stdint.h>

namespace rhythmgen {

constexpr uint32_t kMaxLanes = 4;
constexpr uint32_t kMaxLength = 64;
constexpr uint32_t kDefaultNote = 49;  // C-4 as the Pattern names it (it counts the model's note 1 as C-0): the step byte 49, the OG note 48
constexpr uint32_t kMaxSeed = 9999;
constexpr uint32_t kNoBlockRows = 16;  // with no block: this many rows from the cursor row

// The Euclid rule: row `i` (0-based, any size) of a cycle of `length` steps with `hits` events. hits is clamped to length; 0 hits never.
constexpr bool euclid_hit(uint32_t hits, uint32_t length, uint32_t i) {
  if (length == 0u) {
    return false;
  }
  if (hits > length) {
    hits = length;
  }
  return hits != 0u && (i % length) * hits % length < hits;
}

// The rotated cycle: row `j` of the range is step j mod length of the cycle, moved `rotate` steps later.
constexpr bool cycle_hit(uint32_t hits, uint32_t length, uint32_t rotate, uint32_t j) {
  if (length == 0u) {
    return false;
  }
  const uint32_t k = j % length;
  const uint32_t r = rotate % length;
  return euclid_hit(hits, length, (k + length - r) % length);
}

struct Draw {
  uint32_t s;
  // The state from the user's seed (0..9999) and the lane (0..3): a 32-bit integer hash (lowbias32) so that neighbouring seeds and lanes start unrelated.
  void seed(uint32_t user_seed, uint32_t lane) {
    uint32_t x = user_seed * 2654435761u + (lane + 1u) * 0x9E3779B9u;
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    s = x != 0u ? x : 0x6D2B79F5u;
  }
  uint32_t next() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  }
  // true with probability prob/100 (0 never, 100 and more always).
  bool keep(uint32_t prob) { return next() % 100u < prob; }
};

struct LaneSetup {
  uint32_t hits, length, rotate, note, prob;  // the lane's values
  uint32_t instr;                              // 1..64 (shared)
  uint32_t vol;                                // 0..127, or 255 = leave the volume as it is (shared)
  bool replace;                                // MODE: REPLACE (true) empties the note on every row that is not a hit; ADD (false) keeps it
  uint32_t seed, lane;                         // SEED and the lane (0..3)
};

// What one lane writes over `n` rows (n <= 128): `cur` holds the 8-byte steps as they are (note, instr, vol, fx...), `out` receives the steps to write with the field mask NOTE | INSTR | VOL
// (the FX are never in the mask, so they are never touched; out keeps them as read). Returns how many rows are hits that were kept.
template <class Step>
uint32_t lane_steps(const LaneSetup& l, const Step* cur, Step* out, uint32_t n) {
  Draw d;
  d.seed(l.seed, l.lane);
  uint32_t kept = 0;
  for (uint32_t j = 0; j < n; ++j) {
    out[j] = cur[j];
    bool hit = false;
    if (cycle_hit(l.hits, l.length, l.rotate, j)) {
      hit = d.keep(l.prob);  // one draw per hit
    }
    if (hit) {
      out[j].note = static_cast<uint8_t>(l.note);
      out[j].instr = static_cast<uint8_t>(l.instr);
      if (l.vol != 255u) {
        out[j].vol = static_cast<uint8_t>(l.vol);
      }
      ++kept;
    } else if (l.replace) {
      out[j].note = 0;
    }
  }
  return kept;
}

}  // namespace rhythmgen
