// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The picture of a take as it records (OS-011c, new Community OS behaviour). A take is up to millions of frames long and grows while it is drawn, and a PeakCache is a pyramid
// over a sound of a known length. So the recorder keeps an OVERVIEW: one (min, max) pair per `slot_frames` frames of the take, filled by the main loop from the frames the
// render has written (feed()), and every time the take outgrows the slots, `slot_frames` doubles and the pairs are merged two by two, in place. The pairs are stored as a
// sequence of int16 samples, max then min of each slot, so that a PeakCache built over THAT sequence (which is a tiny sound: at most 2 * kSlots frames) gives the screen the
// min and max of every column of any window of the take, drawn by the same wave view as every other waveform (ui/plot/wave_view.h), at the cost of a few hundred reads.
//
// Exact: a slot's pair is the true minimum and maximum of its frames, and a merged pair the minimum and maximum of both; the result does not depend on how the frames were
// fed (a chunk, or one frame at a time). Main loop only; no heap; kSlots * 4 bytes.

#include <cstdint>

namespace o3os::app {

class TakeOverview {
 public:
  static constexpr std::uint32_t kSlots = 2048;           // pairs; a power of two
  static constexpr std::uint32_t kFirstSlotFrames = 64;   // the finest resolution: 1.45 ms a slot
  static constexpr std::uint32_t kMaxSlotFrames = 1u << 24;

  TakeOverview() noexcept { reset(); }
  void reset() noexcept;

  // Reads the frames [fed(), end) of `pcm` (mono int16; the render wrote them: the caller read the take's frame count with acquire before). At most `budget` frames a call; the
  // number read is returned (fed() moves by it). `pcm` must stay valid and unchanged for the frames already fed.
  std::uint32_t feed(const std::int16_t* pcm, std::uint32_t end, std::uint32_t budget) noexcept;

  std::uint32_t fed() const noexcept { return fed_; }              // frames of the take the overview has seen
  std::uint32_t slot_frames() const noexcept { return slot_frames_; }
  // Pairs filled so far, the last of them perhaps partly (its frames so far); the sequence of samples is 2 * slots().
  std::uint32_t slots() const noexcept { return (fed_ + slot_frames_ - 1u) / slot_frames_; }
  std::uint32_t samples() const noexcept { return 2u * slots(); }
  const std::int16_t* data() const noexcept { return env_; }
  // A frame of the take as a frame of the sample sequence (the position the wave view and its marks use), and back.
  std::uint32_t env_frame(std::uint32_t take_frame) const noexcept { return 2u * (take_frame / slot_frames_); }
  std::uint32_t take_frame(std::uint32_t env_frame) const noexcept { return (env_frame / 2u) * slot_frames_; }
  // Incremented whenever the sequence may have changed under a PeakCache built over it (a feed that moved fed(), a merge, a reset).
  std::uint32_t epoch() const noexcept { return epoch_; }

 private:
  void merge() noexcept;

  alignas(4) std::int16_t env_[2u * kSlots];
  std::uint32_t slot_frames_ = kFirstSlotFrames;
  std::uint32_t fed_ = 0;
  std::uint32_t epoch_ = 0;
};

}  // namespace o3os::app
