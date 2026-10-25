// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-CPUOPT1, L1: the loop player's inner loop (audio/loop_mix.h) against the loop it replaced, bit for bit: the scalar `ssat16(o + ((p * g) >> 16))` on both channels, one frame at a time, with the gain
// moving by `step` a frame. Random runs (lengths 0..200, a take that starts on an even or an odd halfword, TX blocks of any content, gains 0..unity, ramps up and down, step 0), the edges (unity gain
// against -32768, saturation at both ends), and the guard: nothing before the first or past the last frame of the take is read or written.
#include <doctest/doctest.h>

#include <o3os/audio/kernels.h>
#include <o3os/audio/loop_mix.h>
#include <o3os/audio/simd.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using namespace o3os::audio;

std::int32_t sat16(std::int32_t x) { return x > 32767 ? 32767 : (x < -32768 ? -32768 : x); }

// The loop of dev 604d721 (LoopPlayer::run's `body`, the take's part), as it was.
std::int32_t reference(std::int16_t* o, const std::int16_t* p, std::uint32_t n, std::int32_t g, std::int32_t step) {
  for (std::uint32_t i = 0; i < n; ++i, o += 2, g += step) {
    const std::int32_t v = (static_cast<std::int32_t>(p[i]) * g) >> 16;
    o[0] = static_cast<std::int16_t>(sat16(o[0] + v));
    o[1] = static_cast<std::int16_t>(sat16(o[1] + v));
  }
  return g;
}

struct Rng {
  std::uint64_t s = 0x9E3779B97F4A7C15ull;
  std::uint32_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return static_cast<std::uint32_t>(s >> 16);
  }
};

}  // namespace

TEST_CASE("simd: qadd16 is a per-lane saturating add, the model and the instruction alike") {
  using simd::NativeOps;
  using simd::EmuOps;
  const std::int32_t edge[] = {-32768, -32767, -1, 0, 1, 32766, 32767, 12345, -23456};
  for (std::int32_t a0 : edge) {
    for (std::int32_t a1 : edge) {
      for (std::int32_t b0 : edge) {
        for (std::int32_t b1 : edge) {
          const std::uint32_t a = simd::pack_lanes(a0, a1);
          const std::uint32_t b = simd::pack_lanes(b0, b1);
          const std::uint32_t want = simd::pack_lanes(sat16(a0 + b0), sat16(a1 + b1));
          CHECK(EmuOps::qadd16(a, b) == want);
          CHECK(NativeOps::qadd16(a, b) == want);
        }
      }
    }
  }
}

TEST_CASE("loop mix: the register loop equals the scalar loop it replaced, on every run length, alignment, gain and ramp") {
  Rng rng;
  constexpr std::uint32_t kMax = 200;
  for (unsigned round = 0; round < 6000; ++round) {
    const std::uint32_t n = round < 400 ? round % 9u : rng.next() % (kMax + 1u);
    const std::uint32_t skew = rng.next() & 1u;  // the take starts on an odd halfword: the head frame goes by itself
    alignas(4) std::int16_t take_store[kMax + 8];
    alignas(4) std::int16_t out_a[2 * kMax + 8];
    alignas(4) std::int16_t out_b[2 * kMax + 8];
    for (auto& v : take_store) {
      v = static_cast<std::int16_t>(rng.next());
    }
    if (round % 7u == 0u) {
      for (auto& v : take_store) {
        v = static_cast<std::int16_t>(rng.next() & 1u ? -32768 : 32767);  // full scale both ways
      }
    }
    for (std::uint32_t i = 0; i < 2 * kMax + 8; ++i) {
      out_a[i] = static_cast<std::int16_t>(round % 5u == 0u ? ((rng.next() & 1u) ? 32767 : -32768) : static_cast<std::int16_t>(rng.next()));
      out_b[i] = out_a[i];
    }
    const std::int16_t* p = take_store + 2 + skew;
    std::int16_t* oa = out_a + 4;
    std::int16_t* ob = out_b + 4;
    std::int32_t g0, step;
    const unsigned kind = rng.next() % 4u;
    if (kind == 0u) {
      g0 = static_cast<std::int32_t>(rng.next() % 65537u);
      step = 0;
    } else {
      const std::int32_t from = static_cast<std::int32_t>(rng.next() % 65537u);
      const std::int32_t to = kind == 1u ? 65536 : (kind == 2u ? 0 : static_cast<std::int32_t>(rng.next() % 65537u));
      const std::uint32_t frames = 32u + rng.next() % 40u;  // the player's ramps are at most a block
      g0 = from;
      step = (to - from) / static_cast<std::int32_t>(frames);
      if (n > frames) {
        step = 0;  // (a ramp that is shorter than the run is two runs in the player; here one: the gain stays inside 0..unity)
      }
    }
    const std::int32_t ga = reference(oa, p, n, g0, step);
    const std::int32_t gb = loop_mix::take(ob, p, n, g0, step);
    CAPTURE(round);
    CAPTURE(n);
    CAPTURE(skew);
    CHECK(ga == gb);
    CHECK(std::memcmp(out_a, out_b, sizeof out_a) == 0);  // the frames before and after the run were not touched either
  }
}

TEST_CASE("loop mix: unity gain on -32768 is -32768 (not smeared into the other lane), saturation holds at both ends") {
  alignas(4) std::int16_t p[4] = {-32768, -32768, 32767, -1};
  alignas(4) std::int16_t o[8] = {0, 0, -1, -1, 32767, 32767, 5, -5};
  alignas(4) std::int16_t want[8];
  std::memcpy(want, o, sizeof o);
  (void)reference(want, p, 4, kernel::kUnity, 0);
  CHECK(loop_mix::take(o, p, 4, kernel::kUnity, 0) == kernel::kUnity);
  CHECK(std::memcmp(o, want, sizeof o) == 0);
  CHECK(o[0] == -32768);
  CHECK(o[1] == -32768);
  CHECK(o[2] == -32768);  // -1 + -32768 saturates
  CHECK(o[4] == 32767);   // 32767 + 32767 saturates
}

TEST_CASE("loop mix: it reads exactly n frames of the take (a guard word after the last one is never looked at)") {
  alignas(4) std::int16_t p[16];
  for (auto& v : p) {
    v = 1000;
  }
  alignas(4) std::int16_t o_a[32] = {};
  alignas(4) std::int16_t o_b[32] = {};
  // frames 0..4 are the take; p[5..] differs between the runs and must not matter
  for (unsigned n = 0; n <= 5; ++n) {
    for (unsigned skew = 0; skew < 2; ++skew) {
      std::int16_t q1[16], q2[16];
      std::memcpy(q1, p, sizeof q1);
      std::memcpy(q2, p, sizeof q2);
      for (unsigned i = n + skew; i < 16; ++i) {
        q2[i] = static_cast<std::int16_t>(-7 * static_cast<int>(i));
      }
      std::memset(o_a, 0, sizeof o_a);
      std::memset(o_b, 0, sizeof o_b);
      (void)loop_mix::take(o_a, q1 + skew, n, 40000, 100);
      (void)loop_mix::take(o_b, q2 + skew, n, 40000, 100);
      CHECK(std::memcmp(o_a, o_b, sizeof o_a) == 0);
    }
  }
}
