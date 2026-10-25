// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-LOOPER L2b: the overdub kernels (audio/loop_overdub.h) against scalar references, bit for bit: the read-modify-write at every FEEDBACK that matters (0, 1, 50 %, 99 %, just under and at 100 %), both
// parities of the loop and of the input, odd and even lengths, random data and the rails (+32767 and -32768 over a full-scale loop), the guard (nothing before the first or past the last frame is
// touched); the block copy; the bounded restore step (dirty bitmaps, a partial last block, the copy and word budgets, resumption).
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3os/audio/loop_overdub.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using namespace o3os::audio;

std::int32_t sat16(std::int32_t x) { return x > 32767 ? 32767 : (x < -32768 ? -32768 : x); }

struct Rng {
  std::uint64_t s = 0x9E3779B97F4A7C15ull;
  std::uint32_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return static_cast<std::uint32_t>(s >> 16);
  }
  std::int16_t sample() { return static_cast<std::int16_t>(next()); }
};

// loop[i] = sat16(((loop[i] * fb) >> 16) + in[i]), one frame at a time.
void ref_rmw(std::int16_t* loop, const std::int16_t* in, std::uint32_t n, std::int32_t fb) {
  for (std::uint32_t i = 0; i < n; ++i) {
    loop[i] = static_cast<std::int16_t>(sat16(((static_cast<std::int32_t>(loop[i]) * fb) >> 16) + in[i]));
  }
}

constexpr std::int16_t kGuard = 0x5A5A;

}  // namespace

TEST_CASE("loop overdub: the read-modify-write equals the scalar reference at every feedback, parity and length, with a guard around it") {
  Rng rng;
  const std::int32_t fbs[] = {0, 1, 16384, 32768, 64880, 65535, 65536};
  for (std::int32_t fb : fbs) {
    for (unsigned lp = 0; lp < 2; ++lp) {
      for (unsigned ip = 0; ip < 2; ++ip) {
        for (std::uint32_t n = 0; n <= 130; ++n) {
          std::vector<std::int16_t> a(n + 8, kGuard);
          std::vector<std::int16_t> in_store(n + 8, kGuard);
          // a std::vector's data is 16-aligned: +lp / +ip (halfwords) give every parity, and the guards stay around the span.
          std::int16_t* loop = a.data() + 2 + lp;
          std::int16_t* in = in_store.data() + 2 + ip;
          for (std::uint32_t i = 0; i < n; ++i) {
            loop[i] = rng.sample();
            in[i] = rng.sample();
          }
          std::vector<std::int16_t> b = a;
          std::vector<std::int16_t> init = a;
          std::vector<std::int16_t> in_before = in_store;
          ref_rmw(b.data() + 2 + lp, in, n, fb);
          loop_ov::loop_rmw(loop, in, n, fb);
          CHECK(a == b);
          CHECK(in_store == in_before);  // the input is only read
          if (fb == 65536) {  // the add variant is the unity case without the multiplies, bit for bit
            loop_ov::loop_rmw_add(init.data() + 2 + lp, in, n);
            CHECK(init == b);
          }
        }
      }
    }
  }
}

TEST_CASE("loop overdub: saturation at both rails, and the extremes of the feedback product") {
  for (unsigned lp = 0; lp < 2; ++lp) {
    for (unsigned ip = 0; ip < 2; ++ip) {
      const std::int16_t loops[] = {32767, -32768, 32767, -32768, 0, 1, -1, 32767, -32768};
      const std::int16_t ins[] = {32767, -32768, -32768, 32767, 32767, -32768, 32767, 1, -1};
      for (std::int32_t fb : {0, 32768, 65536}) {
        for (bool add : {false, true}) {
          if (add && fb != 65536) {
            continue;
          }
          std::vector<std::int16_t> l(16, 0), i(16, 0);
          for (unsigned k = 0; k < 9; ++k) {
            l[lp + k] = loops[k];
            i[ip + k] = ins[k];
          }
          std::vector<std::int16_t> want = l;
          ref_rmw(want.data() + lp, i.data() + ip, 9, fb);
          if (add) {
            loop_ov::loop_rmw_add(l.data() + lp, i.data() + ip, 9);
          } else {
            loop_ov::loop_rmw(l.data() + lp, i.data() + ip, 9, fb);
          }
          CHECK(l == want);
        }
      }
    }
  }
  // The rails explicitly: +32767 + 32767 -> 32767, -32768 + -32768 -> -32768 at unity feedback; at zero the layer replaces.
  std::int16_t l[4] = {32767, -32768, 32767, -32768};
  const std::int16_t i[4] = {32767, -32768, -32768, 32767};
  loop_ov::loop_rmw_add(l, i, 4);
  CHECK(l[0] == 32767);
  CHECK(l[1] == -32768);
  CHECK(l[2] == -1);
  CHECK(l[3] == -1);
  std::int16_t m[4] = {32767, -32768, 5, -5};
  loop_ov::loop_rmw(m, i, 4, 0);
  CHECK(m[0] == 32767);
  CHECK(m[1] == -32768);
  CHECK(m[2] == -32768);
  CHECK(m[3] == 32767);
}

TEST_CASE("loop overdub: a block copy moves exactly n frames") {
  Rng rng;
  for (std::uint32_t n : {0u, 1u, 7u, 63u, 64u}) {
    std::vector<std::int16_t> src(80), dst(80, kGuard);
    for (std::int16_t& v : src) {
      v = rng.sample();
    }
    loop_ov::loop_block_copy(dst.data() + 4, src.data() + 4, n);
    for (std::uint32_t k = 0; k < 80; ++k) {
      const bool in_span = k >= 4 && k < 4 + n;
      CHECK(dst[k] == (in_span ? src[k] : kGuard));
    }
  }
}

namespace {

struct Restore {
  std::vector<std::int16_t> loop, undo, want;
  std::vector<std::uint16_t> bitmap;
  std::uint32_t len;
  explicit Restore(std::uint32_t frames, Rng& rng, unsigned density) : len(frames) {
    const std::uint32_t blocks = (frames + 63u) / 64u;
    loop.resize(frames);
    undo.resize(frames);
    bitmap.assign((blocks + 15u) / 16u + 1u, 0);  // (+1: a spare halfword the step must not touch)
    for (std::uint32_t i = 0; i < frames; ++i) {
      loop[i] = rng.sample();
      undo[i] = rng.sample();
    }
    want = loop;
    for (std::uint32_t b = 0; b < blocks; ++b) {
      if (rng.next() % 100u < density) {
        bitmap[b >> 4] = static_cast<std::uint16_t>(bitmap[b >> 4] | (1u << (b & 15u)));
        const std::uint32_t at = b * 64u;
        const std::uint32_t n = frames - at < 64u ? frames - at : 64u;
        std::memcpy(want.data() + at, undo.data() + at, n * sizeof(std::int16_t));
      }
    }
  }
  std::uint32_t blocks() const { return (len + 63u) / 64u; }
  std::uint32_t dirty() const {
    std::uint32_t d = 0;
    for (std::uint32_t b = 0; b < blocks(); ++b) {
      d += (bitmap[b >> 4] >> (b & 15u)) & 1u;
    }
    return d;
  }
};

}  // namespace

TEST_CASE("loop overdub: the restore step puts every dirty block back, clears its bit, and stays inside its budgets") {
  Rng rng;
  for (std::uint32_t frames : {1u, 63u, 64u, 65u, 1000u, 1024u, 1025u, 4097u, 20000u}) {
    for (unsigned density : {0u, 3u, 50u, 100u}) {
      for (std::uint32_t copies : {1u, 3u, 1000u}) {
        for (std::uint32_t words : {1u, 4u, 1000u}) {
          Restore r(frames, rng, density);
          std::uint32_t cursor = 0;
          unsigned steps = 0;
          while (cursor < r.blocks()) {
            const std::uint32_t before = r.dirty();
            const std::uint32_t next = loop_ov::loop_restore_step(r.loop.data(), r.undo.data(), r.bitmap.data(), r.len, cursor, copies, words);
            const std::uint32_t after = r.dirty();
            CHECK(next > cursor);                                   // always progresses
            CHECK(before - after <= copies);                        // the copy budget
            CHECK(next - cursor <= (copies + words) * 16u + 16u);   // the scan budget (a halfword is 16 blocks)
            cursor = next;
            NEED(++steps < 100000u);
          }
          CHECK(cursor == r.blocks());
          CHECK(r.loop == r.want);
          CHECK(r.dirty() == 0u);
          CHECK(r.bitmap.back() == 0u);  // the spare halfword is untouched
          // a second pass over a clean bitmap changes nothing and returns the end
          CHECK(loop_ov::loop_restore_step(r.loop.data(), r.undo.data(), r.bitmap.data(), r.len, 0, 1000, 1000000) == r.blocks());
          CHECK(r.loop == r.want);
        }
      }
    }
  }
}

TEST_CASE("loop overdub: the restore step with nothing to do, an empty loop, or a cursor at the end") {
  std::int16_t loop[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  const std::int16_t undo[8] = {};
  std::uint16_t bm[2] = {0xFFFF, 0xFFFF};  // (bits beyond the one block of 8 frames are not a block: never copied)
  CHECK(loop_ov::loop_restore_step(loop, undo, bm, 0, 0, 10, 10) == 0u);
  CHECK(loop[0] == 1);
  CHECK(loop_ov::loop_restore_step(loop, undo, bm, 8, 1, 10, 10) == 1u);
  CHECK(loop[0] == 1);
  CHECK(loop_ov::loop_restore_step(loop, undo, bm, 8, 0, 0, 0) == 1u);  // a budget of 0 counts as 1: the step still makes progress (and does its copy)
  CHECK(loop[0] == 0);
  loop[0] = 1;
  CHECK(loop_ov::loop_restore_step(loop, undo, bm, 8, 0, 10, 10) == 1u);  // (the bit was cleared by the step above: nothing to copy now)
  CHECK(loop[0] == 1);
  bm[0] |= 1u;
  CHECK(loop_ov::loop_restore_step(loop, undo, bm, 8, 0, 10, 10) == 1u);
  CHECK(loop[0] == 0);
  CHECK(loop[7] == 0);
  CHECK(bm[0] == 0xFFFE);
}

TEST_CASE("loop overdub: loop_rmw clamps a feedback outside 0..65536") {
  const std::int16_t in[4] = {100, -100, 7, 0};
  for (std::int32_t fb : {-1, -100000, 65537, 1000000, 0x7FFFFFFF}) {
    std::int16_t a[4] = {1000, -2000, 32767, -32768};
    std::int16_t b[4] = {1000, -2000, 32767, -32768};
    loop_ov::loop_rmw(a, in, 4, fb);
    loop_ov::loop_rmw(b, in, 4, fb < 0 ? 0 : 65536);
    CHECK(std::memcmp(a, b, sizeof a) == 0);
  }
}
