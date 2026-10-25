// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-011a: the AUTO slicer (app/transient.h): hits found at the right place in a synthetic break, nothing in silence or steady noise, the gap after a hit, the
// strongest kept when there are more than fit, the same answer however the reads are cut into steps.
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3os/app/transient.h>
#include <o3os/app/wave_math.h>

#include <cmath>
#include <cstdint>
#include <vector>

using namespace o3os::app;

namespace {

struct Noise {
  std::uint32_t s = 99u;
  int next(int amp) {
    s = s * 1664525u + 1013904223u;
    return static_cast<int>((s >> 16) % static_cast<std::uint32_t>(2 * amp + 1)) - amp;
  }
};

// `frames` of low noise (amplitude `floor`) with a decaying burst at each of `at`, of amplitude `amp[i]` (a short exponential decay: 1500 frames).
std::vector<std::int16_t> make(std::uint32_t frames, const std::vector<std::uint32_t>& at, const std::vector<int>& amp, int floor = 20) {
  std::vector<std::int16_t> pcm(frames);
  Noise n;
  for (std::uint32_t i = 0; i < frames; ++i) {
    pcm[i] = static_cast<std::int16_t>(n.next(floor));
  }
  for (std::size_t k = 0; k < at.size(); ++k) {
    for (std::uint32_t j = 0; j < 6000 && at[k] + j < frames; ++j) {
      const double env = std::exp(-static_cast<double>(j) / 1500.0);
      const int v = static_cast<int>(amp[k] * env * std::sin(0.31 * j + 0.2 * static_cast<double>(k)));
      pcm[at[k] + j] = static_cast<std::int16_t>(pcm[at[k] + j] + v);
    }
  }
  return pcm;
}

std::vector<TransientDetector::Onset>& store() {
  static std::vector<TransientDetector::Onset> v(TransientDetector::kMaxOnsets);
  return v;
}

std::vector<TransientDetector::Onset>& scratch() {
  static std::vector<TransientDetector::Onset> v(TransientDetector::kMaxOnsets);
  return v;
}

std::vector<std::uint32_t> run(const std::vector<std::int16_t>& pcm, std::uint32_t step_frames, unsigned sensitivity = TransientDetector::kDefaultSensitivity) {
  TransientDetector d;
  d.begin(pcm.data(), static_cast<std::uint32_t>(pcm.size()), store().data(), sensitivity);
  unsigned calls = 0;
  while (!d.step(step_frames) && calls < 10000000u) {
    ++calls;
  }
  CHECK(d.done());
  std::vector<std::uint32_t> out;
  for (unsigned i = 0; i < d.count(); ++i) {
    out.push_back(d.onset(i).frame);
  }
  return out;
}

}  // namespace

TEST_CASE("transient: hits are found where they start, within a window before the attack and never after it") {
  const std::vector<std::uint32_t> at = {10000, 30000, 52000, 80000};
  const std::vector<std::int16_t> pcm = make(100000, at, {20000, 18000, 24000, 15000});
  const std::vector<std::uint32_t> found = run(pcm, 4096);
  NEED(found.size() == 4u);
  for (std::size_t k = 0; k < 4; ++k) {
    CHECK_MESSAGE(found[k] <= at[k] + 64u, "hit " << k << " at " << found[k] << ", the attack is at " << at[k]);
    CHECK_MESSAGE(found[k] + 1100u >= at[k], "hit " << k << " at " << found[k] << ", the attack is at " << at[k]);
  }
}

TEST_CASE("transient: silence, dither and steady noise have no hits; a sound shorter than a window, or none, has none either") {
  CHECK(run(std::vector<std::int16_t>(50000, 0), 4096).empty());
  CHECK(run(make(50000, {}, {}, 30), 4096).empty());
  CHECK(run(make(50000, {}, {}, 8000), 4096).empty());  // loud but steady
  CHECK(run(make(300, {100}, {20000}), 4096).empty());
  TransientDetector d;
  d.begin(nullptr, 1000, store().data());
  CHECK(d.step(100));
  CHECK(d.count() == 0u);
  d.begin(nullptr, 0, store().data());
  CHECK(d.done());
}

TEST_CASE("transient: the answer does not depend on how the reads are cut into steps; every step reads at least a window") {
  const std::vector<std::int16_t> pcm = make(120000, {5000, 25000, 26500, 61000, 90000}, {20000, 16000, 16000, 22000, 12000});
  const std::vector<std::uint32_t> a = run(pcm, 1);
  const std::vector<std::uint32_t> b = run(pcm, 512);
  const std::vector<std::uint32_t> c = run(pcm, 100000);
  CHECK(a == b);
  CHECK(a == c);
  CHECK(!a.empty());
  TransientDetector d;
  d.begin(pcm.data(), static_cast<std::uint32_t>(pcm.size()), store().data());
  CHECK_FALSE(d.step(1));
  CHECK(d.position() == TransientDetector::kWindow);  // one step is one window at least, and no more at that budget
  CHECK_FALSE(d.step(2000));
  CHECK(d.position() == 5u * TransientDetector::kWindow);  // 512 + 4 more windows (the budget is passed at the fourth)
}

TEST_CASE("transient: a second hit within eight windows of the first is not a hit of its own") {
  const std::vector<std::int16_t> close = make(60000, {10000, 12500}, {20000, 20000});  // 2500 frames apart: under 4096
  const std::vector<std::int16_t> apart = make(60000, {10000, 20000}, {20000, 20000});
  CHECK(run(close, 4096).size() == 1u);
  CHECK(run(apart, 4096).size() == 2u);
}

TEST_CASE("transient: the sensitivity decides whether a soft hit counts") {
  // A hit of amplitude 400 over noise of 400: its first window has about 2.5 times the background's energy: past the ratio of a keen setting (1.75), short of
  // that of a strict one (4.0).
  const std::vector<std::int16_t> pcm = make(60000, {20000, 40000}, {400, 400}, 400);
  CHECK(run(pcm, 4096, 0).empty());
  CHECK(run(pcm, 4096, 100).size() == 2u);
  CHECK(TransientDetector::ratio_q8(0) == 1024u);
  CHECK(TransientDetector::ratio_q8(100) == 448u);
  CHECK(TransientDetector::ratio_q8(50) == 736u);
  CHECK(TransientDetector::ratio_q8(500) == 448u);
}

TEST_CASE("transient: choose_slices starts at 0, keeps the strongest hits in order, never more than the cap, unique and ascending") {
  // Ten hits 20000 frames apart (each is judged against the noise floor alone): the even ones strong, the odd ones soft.
  std::vector<std::uint32_t> at;
  std::vector<int> amp;
  for (unsigned i = 0; i < 10; ++i) {
    at.push_back(5000 + i * 20000u);
    amp.push_back(i % 2 == 0 ? 20000 - static_cast<int>(i) * 100 : 3500);
  }
  const std::vector<std::int16_t> pcm = make(5000 + 11 * 20000u, at, amp);
  TransientDetector d;
  d.begin(pcm.data(), static_cast<std::uint32_t>(pcm.size()), store().data());
  while (!d.step(65536)) {
  }
  NEED(d.count() == 10u);
  std::uint16_t out[kMaxSliceCount];
  const unsigned n = choose_slices(d, 6, out, scratch().data());  // slice 0 and the five strongest
  CHECK(n == 6u);
  CHECK(out[0] == 0u);
  for (unsigned i = 1; i < n; ++i) {
    CHECK(out[i - 1] < out[i]);
  }
  const std::uint32_t frames = static_cast<std::uint32_t>(pcm.size());
  for (unsigned i = 1; i < n; ++i) {
    const std::uint32_t f = point_to_frame(out[i], frames);
    const std::uint32_t nearest = at[2 * (i - 1)];  // the even (strong) hits, in order
    CHECK_MESSAGE((f + 1200u >= nearest && f <= nearest + 64u), "slice " << i << " at " << f << ", the strong hit is at " << nearest);
  }
  CHECK(choose_slices(d, 48, out, scratch().data()) == 11u);  // slice 0 and the ten hits: all of them fit
  CHECK(choose_slices(d, 1, out, scratch().data()) == 1u);
  CHECK(choose_slices(d, 0, out, scratch().data()) == 1u);
  CHECK(choose_slices(d, 999, out, scratch().data()) == 11u);
  // Nothing found: just the one slice, the whole sample.
  TransientDetector quiet;
  const std::vector<std::int16_t> none(40000, 0);
  quiet.begin(none.data(), 40000, store().data());
  while (!quiet.step(65536)) {
  }
  CHECK(choose_slices(quiet, 48, out, scratch().data()) == 1u);
  CHECK(out[0] == 0u);
}
