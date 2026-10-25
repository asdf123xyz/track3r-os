// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-CPUOPT1, C3: svf_set_cutoff keeps the coefficient while the cutoff stays in the same table index (the coefficient is a function of the index alone). The cached filter's f is bit for bit the f of a
// filter made fresh for every cutoff, over random walks of the cutoff (slow, fast, out of range, NaN), and the resonance and the state are not touched.
#include <doctest/doctest.h>

#include <o3os/audio/svf.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

TEST_CASE("svf: a cutoff that stays in its table index keeps its coefficient, bit for bit what a fresh set makes") {
  using namespace o3os::audio;
  std::uint64_t s = 99;
  const auto rnd = [&s] {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<float>(static_cast<std::uint32_t>(s >> 33)) / 2147483648.0f;
  };
  Svf cached;
  svf_set(cached, 0.5f, 2.0f);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  float c = 0.5f;
  for (unsigned n = 0; n < 200000; ++n) {
    switch (n % 5u) {
      case 0: c += (rnd() - 0.5f) * 0.0005f; break;  // slow: stays in an index for several steps
      case 1: c = rnd(); break;
      case 2: c += (rnd() - 0.5f) * 0.01f; break;
      case 3: c = (n % 25u == 3u) ? (n % 50u == 3u ? nan : -0.3f) : c; break;
      default: c = (n % 25u == 4u) ? 1.7f : c; break;
    }
    Svf fresh;
    svf_set_cutoff(fresh, c);
    cached.lp = 3.0f;
    cached.bp = -2.0f;
    cached.prev = 1.0f;
    const float d_before = cached.d;
    svf_set_cutoff(cached, c);
    CAPTURE(n);
    CHECK(std::memcmp(&cached.f, &fresh.f, sizeof(float)) == 0);
    CHECK(cached.d == d_before);
    CHECK((cached.lp == 3.0f && cached.bp == -2.0f && cached.prev == 1.0f));
  }
}
