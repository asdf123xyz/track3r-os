// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Level meters of the fixed graph (OS-005b; ADR 0013 D6, ADR 0010 amendment 2: "meters are fed only from the audio -> main ring", "live
// plots never touch audio buffers"). The render measures; the UI reads a SNAPSHOT of plain numbers, never a buffer.
//
// What is measured. 28 channels (meter_ch below): the 16 track strips after their fader (mono, what the StemTap carries), the input bus
// after its level, the buses (MAIN, CUE, REC left and right, the two send buses) after their saturation, and the master (the output block's
// two channels, after the chain, the master level and OUT MODE). Per block each live channel is run through kernel::meter (peak, sum of
// squares); a silent channel is not touched at all.
//
// Windows. A window is kMeterWindowBlocks blocks (22: 31.9 ms, the 30 Hz tier of the UI). At the end of a window the render publishes, per
// channel, exact integers over the window's kMeterWindowBlocks * kBlock frames:
//     peak    the largest |sample| (0..32768)
//     hold    the largest peak of the last kMeterHoldWindows windows (a sliding maximum: 32 windows are 1.02 s)
//     clips   a running count of the windows in which the peak reached full scale (|sample| >= 32767); the UI lights its lamp when it moves
//     sumsq   the sum of the squares, 64 bits: the RMS is sqrt(sumsq / frames) / 32768, taken by the reader (meter_level below), because a
//             square root has no place in the render
// Nothing time-based is decided here beyond the window and the hold: the ballistics (the fall of a bar, the lamp's hold time) are the plot
// library's (ui/plot/meters.h MeterState), fed from `meter_level()` at its own rate. A UI that reads slower than a window misses that window's
// `peak` but never a clip (`clips` is a count) and never a peak above the last second (`hold`).
//
// Protocol. One writer (the render context), any number of readers in lower contexts: a seqlock (the generation is odd while the window is
// being written), the same as AudioStatus; a reader retries (bounded) and is told when it could not get a whole snapshot. The words are
// relaxed atomics, so a read that races with the writer is well defined and simply fails the check.

#include <o3os/audio/hot.h>
#include <o3os/audio/kernels.h>

#include <atomic>
#include <cstdint>

namespace o3os::audio {

namespace meter_ch {
inline constexpr unsigned kTrack0 = 0;    // 16 tracks
inline constexpr unsigned kInL = 16;      // the external input bus, after its level
inline constexpr unsigned kInR = 17;
inline constexpr unsigned kMainL = 18;    // buses after saturation, before the master chain; MAIN with a limiter installed (OS-LIMIT): as it arrives at the limiter, so its clips are the overload it caught
inline constexpr unsigned kMainR = 19;
inline constexpr unsigned kCueL = 20;
inline constexpr unsigned kCueR = 21;
inline constexpr unsigned kSndA = 22;     // the send buses (mono), before their effects
inline constexpr unsigned kSndB = 23;
inline constexpr unsigned kRecL = 24;
inline constexpr unsigned kRecR = 25;
inline constexpr unsigned kMasterL = 26;  // the output block, after everything
inline constexpr unsigned kMasterR = 27;
inline constexpr unsigned kCount = 28;
}  // namespace meter_ch
inline constexpr unsigned kMeterChannels = meter_ch::kCount;

inline constexpr std::uint32_t kMeterWindowBlocks = 22;
inline constexpr unsigned kMeterHoldWindows = 32;  // a power of two: the history is a ring
inline constexpr std::uint32_t kMeterClipLevel = 32767;

struct MeterChannelSnapshot {
  std::uint32_t peak;
  std::uint32_t hold;
  std::uint32_t clips;
  std::uint64_t sumsq;
};

struct MeterSnapshot {
  std::uint32_t windows;     // windows published since reset (0: nothing measured yet)
  std::uint32_t frames;      // frames in each window
  MeterChannelSnapshot ch[kMeterChannels];
};

// What the plot library's MeterReading holds, as linear amplitudes (1.0 = full scale). Main loop only (a square root).
struct MeterLevel {
  float peak;
  float hold;
  float rms;
};
MeterLevel meter_level(const MeterSnapshot& s, unsigned channel) noexcept;

// The published half of the meters: the last windows' peaks (for the hold) and the snapshot's words behind their generation counter. Plain
// storage, so that the platform may place it where it likes (the MK66 puts it in SRAM_L with the other taps, tap_mem.h); MeterBank is its one writer.
struct MeterStore {
  static constexpr unsigned kWordWindows = 0;
  static constexpr unsigned kWordFrames = 1;
  static constexpr unsigned kWordsPerChannel = 5;  // peak, hold, clips, sumsq low, sumsq high
  static constexpr unsigned kWordFirstChannel = 2;
  static constexpr unsigned kWords = kWordFirstChannel + kMeterChannels * kWordsPerChannel;

  std::uint16_t history[kMeterChannels][kMeterHoldWindows];
  std::atomic<std::uint32_t> generation{0};
  std::atomic<std::uint32_t> words[kWords];
  void reset() noexcept;
};

class MeterBank {
 public:
  explicit MeterBank(MeterStore& store) noexcept : store_(store) { reset(); }
  // Everything back to zero. Only while the render cannot run (before start, between tests).
  void reset() noexcept;

  // ---- Render context --------------------------------------------------------------------------------------------------
  kernel::MeterAcc& acc(unsigned channel) noexcept { return acc_[channel]; }
  // One block measured: counts it and, at the end of a window, publishes the window and starts the next.
  O3OS_AUDIO_HOT void end_block() noexcept;

  // ---- Any lower context -----------------------------------------------------------------------------------------------
  // Fills `out`; true when it is one whole publish. After the bounded retries a torn read returns false with `out` as read.
  bool snapshot(MeterSnapshot& out) const noexcept;
  std::uint32_t windows() const noexcept { return store_.words[MeterStore::kWordWindows].load(std::memory_order_relaxed); }

 private:
  MeterStore& store_;
  kernel::MeterAcc acc_[kMeterChannels];
  std::uint32_t blocks_ = 0;                // render only: blocks in the window so far
  std::uint32_t clips_[kMeterChannels];     // render only
  std::uint32_t hold_[kMeterChannels];      // render only
  std::uint32_t windows_published_ = 0;     // render only
};

}  // namespace o3os::audio
