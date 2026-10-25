// SPDX-License-Identifier: GPL-3.0-or-later
//
// The recorder's overview of a take (app/take_overview.h, OS-011c): exact min / max pairs, the same whatever the chunking, merged two by two when the take outgrows the
// slots, and drawable as a PeakCache over its own sample sequence.
#include <doctest/doctest.h>

#include <o3os/app/take_overview.h>
#include <o3os/ui/plot/peak_cache.h>

#include "support/rng.h"

#include <cstdint>
#include <memory>
#include <vector>

using o3os::app::TakeOverview;

namespace {

std::vector<std::int16_t> noise(std::uint32_t n, std::uint32_t seed) {
  o3os::test::Rng rng(seed);
  std::vector<std::int16_t> v(n);
  for (std::uint32_t i = 0; i < n; ++i) {
    v[i] = static_cast<std::int16_t>(static_cast<std::int32_t>(rng.below(60001)) - 30000);
  }
  return v;
}

// The pair of the slot `s` by brute force, over the frames it covers (the last slot perhaps partly).
void brute(const std::vector<std::int16_t>& pcm, std::uint32_t slot_frames, std::uint32_t s, std::int16_t& hi, std::int16_t& lo) {
  hi = -32768;
  lo = 32767;
  for (std::uint32_t i = s * slot_frames; i < (s + 1u) * slot_frames && i < pcm.size(); ++i) {
    hi = pcm[i] > hi ? pcm[i] : hi;
    lo = pcm[i] < lo ? pcm[i] : lo;
  }
}

void check_exact(const TakeOverview& ov, const std::vector<std::int16_t>& pcm) {
  CHECK(ov.fed() == pcm.size());
  CHECK(ov.slots() == (pcm.size() + ov.slot_frames() - 1u) / ov.slot_frames());
  CHECK(ov.slots() <= TakeOverview::kSlots);
  for (std::uint32_t s = 0; s < ov.slots(); ++s) {
    std::int16_t hi, lo;
    brute(pcm, ov.slot_frames(), s, hi, lo);
    CHECK(ov.data()[2u * s] == hi);
    CHECK(ov.data()[2u * s + 1u] == lo);
  }
}

}  // namespace

TEST_CASE("take overview: pairs are the exact minimum and maximum of their frames, the last slot partly filled") {
  const std::vector<std::int16_t> pcm = noise(64u * 10u + 17u, 5);
  auto ov = std::make_unique<TakeOverview>();
  CHECK(ov->slots() == 0u);
  CHECK(ov->feed(pcm.data(), static_cast<std::uint32_t>(pcm.size()), 0xFFFFFFFFu) == pcm.size());
  CHECK(ov->slot_frames() == 64u);
  CHECK(ov->slots() == 11u);
  check_exact(*ov, pcm);
  CHECK(ov->feed(pcm.data(), static_cast<std::uint32_t>(pcm.size()), 100) == 0u);  // nothing new
}

TEST_CASE("take overview: the chunks it is fed in do not matter, and a budget caps a call") {
  const std::vector<std::int16_t> pcm = noise(64u * 2048u * 2u + 5u, 6);  // past the slots: it merges twice
  auto whole = std::make_unique<TakeOverview>();
  whole->feed(pcm.data(), static_cast<std::uint32_t>(pcm.size()), 0xFFFFFFFFu);
  check_exact(*whole, pcm);
  CHECK(whole->slot_frames() == 64u * 4u);  // 262,149 frames: 2048 slots of 128 hold 262,144, the last five need the next merge
  for (std::uint32_t chunk : {1u, 7u, 63u, 64u, 65u, 1000u, 16384u}) {
    auto ov = std::make_unique<TakeOverview>();
    std::uint32_t end = 0;
    while (end < pcm.size()) {
      end = end + chunk < pcm.size() ? end + chunk : static_cast<std::uint32_t>(pcm.size());
      ov->feed(pcm.data(), end, 0xFFFFFFFFu);
      if (end == pcm.size() || end % 40009u < chunk) {
        const std::vector<std::int16_t> so_far(pcm.begin(), pcm.begin() + end);
        check_exact(*ov, so_far);  // exact at every stage
      }
    }
    CHECK(ov->slot_frames() == whole->slot_frames());
    CHECK(ov->slots() == whole->slots());
    for (std::uint32_t i = 0; i < 2u * ov->slots(); ++i) {
      CHECK(ov->data()[i] == whole->data()[i]);
    }
  }
  auto budgeted = std::make_unique<TakeOverview>();
  CHECK(budgeted->feed(pcm.data(), static_cast<std::uint32_t>(pcm.size()), 1000) == 1000u);
  CHECK(budgeted->fed() == 1000u);
  CHECK(budgeted->feed(pcm.data(), static_cast<std::uint32_t>(pcm.size()), 1000) == 1000u);
  CHECK(budgeted->fed() == 2000u);
}

TEST_CASE("take overview: the epoch moves with every feed that read frames and with a reset; frames map to the sample sequence and back") {
  const std::vector<std::int16_t> pcm = noise(5000, 7);
  auto ov = std::make_unique<TakeOverview>();
  const std::uint32_t e0 = ov->epoch();
  CHECK(ov->feed(pcm.data(), 100, 1000) == 100u);
  const std::uint32_t e1 = ov->epoch();
  CHECK(e1 != e0);
  CHECK(ov->feed(pcm.data(), 100, 1000) == 0u);
  CHECK(ov->epoch() == e1);  // nothing read: the pairs did not change
  ov->reset();
  CHECK(ov->epoch() != e1);
  CHECK(ov->fed() == 0u);
  CHECK(ov->env_frame(64u * 5u) == 10u);
  CHECK(ov->take_frame(10u) == 64u * 5u);
  CHECK(ov->env_frame(64u * 5u + 63u) == 10u);
  CHECK(ov->take_frame(11u) == 64u * 5u);
}

TEST_CASE("take overview: a PeakCache over its sequence answers the min and max of any window of the take, which is what the wave view draws") {
  const std::vector<std::int16_t> pcm = noise(64u * 2048u * 2u, 8);  // 262144 frames: slots of 128 after one merge
  auto ov = std::make_unique<TakeOverview>();
  ov->feed(pcm.data(), static_cast<std::uint32_t>(pcm.size()), 0xFFFFFFFFu);
  const std::uint32_t n = ov->samples();
  std::vector<std::int16_t> storage(o3os::ui::plot::PeakCache::elements_required(n, 1) + 8);
  o3os::ui::plot::PeakCache cache;
  CHECK(cache.init(storage.data(), storage.size(), ov->data(), n, 1));
  cache.build();
  // The whole take: the global extremes.
  std::int16_t hi = -32768, lo = 32767;
  for (const std::int16_t v : pcm) {
    hi = v > hi ? v : hi;
    lo = v < lo ? v : lo;
  }
  const o3os::ui::plot::PeakPair all = cache.range(0, 0, n);
  CHECK(all.max == hi);
  CHECK(all.min == lo);
  // A window that is a whole number of slots: exact.
  const std::uint32_t sf = ov->slot_frames();
  const o3os::ui::plot::PeakPair part = cache.range(0, ov->env_frame(10u * sf), ov->env_frame(20u * sf));
  std::int16_t phi = -32768, plo = 32767;
  for (std::uint32_t i = 10u * sf; i < 20u * sf; ++i) {
    phi = pcm[i] > phi ? pcm[i] : phi;
    plo = pcm[i] < plo ? pcm[i] : plo;
  }
  CHECK(part.max == phi);
  CHECK(part.min == plo);
}
