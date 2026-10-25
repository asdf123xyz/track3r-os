// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/take_overview.h>

namespace o3os::app {

void TakeOverview::reset() noexcept {
  slot_frames_ = kFirstSlotFrames;
  fed_ = 0;
  ++epoch_;
}

// Two slots into one: the first half of the sequence becomes the pairs of the merged slots, in place (slot i of the result comes from slots 2i and 2i + 1, which are
// at or after it, so reading ahead of the writes is safe).
void TakeOverview::merge() noexcept {
  const std::uint32_t n = slots();
  for (std::uint32_t i = 0; 2u * i < n; ++i) {
    std::int16_t hi = env_[4u * i];
    std::int16_t lo = env_[4u * i + 1u];
    if (2u * i + 1u < n) {
      hi = env_[4u * i + 2u] > hi ? env_[4u * i + 2u] : hi;
      lo = env_[4u * i + 3u] < lo ? env_[4u * i + 3u] : lo;
    }
    env_[2u * i] = hi;
    env_[2u * i + 1u] = lo;
  }
  slot_frames_ *= 2u;
}

std::uint32_t TakeOverview::feed(const std::int16_t* pcm, std::uint32_t end, std::uint32_t budget) noexcept {
  if (pcm == nullptr || end <= fed_) {
    return 0;
  }
  std::uint32_t todo = end - fed_;
  todo = todo < budget ? todo : budget;
  const std::uint32_t done = todo;
  ++epoch_;
  while (todo != 0u) {
    if (fed_ / slot_frames_ >= kSlots) {
      if (slot_frames_ >= kMaxSlotFrames) {
        break;  // a take longer than the picture can show (16.7 M frames: the cache's own limit): the rest is not drawn
      }
      merge();
      continue;
    }
    const std::uint32_t slot = fed_ / slot_frames_;
    const std::uint32_t in_slot = fed_ - slot * slot_frames_;
    const std::uint32_t run = slot_frames_ - in_slot;
    const std::uint32_t n = todo < run ? todo : run;
    std::int16_t hi = in_slot == 0u ? pcm[fed_] : env_[2u * slot];
    std::int16_t lo = in_slot == 0u ? pcm[fed_] : env_[2u * slot + 1u];
    for (std::uint32_t i = 0; i < n; ++i) {
      const std::int16_t v = pcm[fed_ + i];
      hi = v > hi ? v : hi;
      lo = v < lo ? v : lo;
    }
    env_[2u * slot] = hi;
    env_[2u * slot + 1u] = lo;
    fed_ += n;
    todo -= n;
  }
  return done - todo;
}

}  // namespace o3os::app
