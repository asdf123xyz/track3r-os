// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The AUTO slicer of the Chop page (OS-011a, new Community OS behaviour): finds where a sound hits (a drum, a pluck: a sudden rise in energy) so that a break can
// be cut into slices. It is our own detector: the energy of a window against the average of the
// ones before it, with a refinement of the position and a gap after each hit. It uses 512-frame windows against 16 of history (8192 frames, 186 ms) for a tight position, and keeps the strongest hits
// when there are more than fit.
//
//   energy of a window  mean of the squares of its 512 frames (a 32-bit value: 1 << 30 at most)
//   background          the mean energy of the 16 windows before it (fewer at the start of the sound, at least 2)
//   a hit               energy > background x ratio, above an absolute floor (rms 64: silence and dither are not hits), and rising (more than the window
//                       before): at least 8 windows (93 ms) after the previous one
//   its position        refined inside the two windows around it, in blocks of 64 frames: the first block whose energy passes the same test, less one block of pre-roll
//                       so that the attack is not cut off
//   its score           energy / background, Q8: what ranks it when there are more than 128, and when the slices are chosen (choose_slices)
//
// Resumable: step() reads at most about `max_frames` frames (always at least one window) and says when it has read them all, so it runs as a job (slice_job.h)
// without holding the main loop. Reads the PCM, never writes it; the PCM must stay unchanged while it runs. No heap; about 150 bytes besides the hits it keeps. Main loop only.

#include <cstdint>

namespace o3os::app {

class TransientDetector {
 public:
  static constexpr unsigned kMaxOnsets = 128;
  static constexpr std::uint32_t kWindow = 512;
  static constexpr std::uint32_t kBlock = 64;
  static constexpr unsigned kHistory = 16;
  static constexpr unsigned kMinGapWindows = 8;
  static constexpr std::uint32_t kEnergyFloor = 64u * 64u;
  static constexpr unsigned kDefaultSensitivity = 50;  // 0..100: higher finds softer hits

  struct Onset {
    std::uint32_t frame = 0;
    std::uint32_t score = 0;  // Q8: 256 = the hit's energy equals the background's
  };

  // Starts over a sound of `frames` frames. The hits are kept in `storage` (kMaxOnsets of them: 1 KB, which the caller keeps out of SRAM, sample_bank.h
  // SampleBankMem). `sensitivity` 0..100 sets the ratio: 4.0 (0) down to 1.75 (100), 2.875 at the default 50. Without storage nothing is found.
  void begin(const std::int16_t* pcm, std::uint32_t frames, Onset* storage, unsigned sensitivity = kDefaultSensitivity) noexcept;
  // Reads on; true when the whole sound has been read.
  bool step(std::uint32_t max_frames) noexcept;
  bool done() const noexcept { return done_; }
  std::uint32_t position() const noexcept { return pos_; }  // frames read
  std::uint32_t frames() const noexcept { return frames_; }
  unsigned count() const noexcept { return count_; }
  const Onset& onset(unsigned i) const noexcept { return onsets_[i < count_ ? i : 0u]; }  // (count() entries are valid)
  // The ratio (Q8) a sensitivity makes.
  static std::uint32_t ratio_q8(unsigned sensitivity) noexcept;

 private:
  std::uint32_t window_energy(std::uint32_t first, std::uint32_t n) const noexcept;
  void found(std::uint32_t window, std::uint32_t energy, std::uint32_t background) noexcept;
  void record(std::uint32_t frame, std::uint32_t score) noexcept;

  const std::int16_t* pcm_ = nullptr;
  std::uint32_t frames_ = 0;
  std::uint32_t pos_ = 0;      // frames consumed (a whole number of windows until the last)
  std::uint32_t ratio_ = 0;
  std::uint32_t prev_energy_ = 0;
  std::uint32_t history_[kHistory] = {};
  unsigned history_n_ = 0;
  unsigned history_head_ = 0;
  std::uint32_t window_index_ = 0;
  std::uint32_t last_window_ = 0;
  bool have_last_ = false;
  bool done_ = true;
  unsigned count_ = 0;
  Onset* onsets_ = nullptr;
};

// The slice starts for a sound from its detector: slice 0 at point 0 (what comes before the first hit is not orphaned), then the strongest hits, at most `cap`
// slices in all (at most 48), as points (0..65535 of `frames`), ascending and unique. Writes into `out` (cap entries); returns how many.
// `scratch` is kMaxOnsets entries of working room (the caller keeps it out of the stack and SRAM, sample_bank.h).
unsigned choose_slices(const TransientDetector& d, unsigned cap, std::uint16_t* out, TransientDetector::Onset* scratch) noexcept;

}  // namespace o3os::app
