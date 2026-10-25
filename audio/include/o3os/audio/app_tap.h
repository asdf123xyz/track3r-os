// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The audio tap of the SD apps (OS-H2a; ADR 0014 decision 3(b), amendment E): ONE selectable mono tap, input or master, for the apps that hold the audio_tap capability (TUNER, SCOPE /
// SPECTRUM, LOUDNESS / VU). The render writes it once per block; the main loop reads it; nothing in the render ever waits for a reader or calls module code.
//
//   what         the mono mix ((L + R + 1) >> 1, kernel::mono_mix) of the INPUT bus as the render holds it at the end of the block (the input strip's level applied while that strip
//                is open: what the Mixer's input meter sees; the raw RX while it is closed), or of the MASTER output pair (what is heard: the USB return included, the metronome click not, as for the spectrum ring). Source::Off = the tap costs one
//                relaxed load per block.
//   ring         kBlocks (64) blocks of kBlock (64) frames = 4,096 int16 samples (8 KiB as the atomic words the SampleRing keeps them in), in SDRAM on the unit (app_main.cpp), the render
//                its one writer. It is a taps.h SampleRing: `head` is the sequence counter (frames ever written, free running, always a multiple of kBlock). 64 blocks are 93 ms at 44.1 kHz:
//                the main loop's frame is 16..33 ms, so a reader that runs every frame never laps it (ADR 0014 said 16 blocks = 23 ms, which a 33 ms frame laps: amendment E).
//   per block    peak (largest |sample|, 0..32768) and sum of squares (64 bits) of the block just written, in a slot of a stats ring beside the samples (same index, same lap rule), so a
//                reader that comes every 33 ms still sees the peak of every block in between, and the clip blocks.
//
// PROTOCOL (single writer = the render, single reader = the main loop; a third context, `diag`, reads the cost counters only):
//   writer   push(): stats slot first (relaxed stores), then the samples (SampleRing::write), whose last store publishes `head` with release. Nothing is read back, nothing is waited for.
//   reader   loads `head` (acquire), copies, then re-loads `head`: a sample or stats slot written at position p is intact iff h2 - p <= kMaxFrames (the block the writer may be in the middle
//            of is the ring's slack). A failed check is retried a bounded number of times (3); if the reader is lapped (it was away for more than kMaxFrames, 90 ms) the OLDEST data is
//            dropped and counted (`lost`), never the render's time. Every cursor is in FRAMES and free running (u32 wrap safe: only differences are used).
//   source   written by the main loop (relaxed), read by the render once a block: a change lands at a block boundary. A reader that wants only the data of the new source discards what is
//            older than 2 blocks after its select (ModuleUi does).
//   samples  (OS-BUG-TAPWRITE) written by the main loop (relaxed), read by the render once a block, like `source`. Set (the default): push() stores the block's samples. Clear: push() measures the
//            block and advances `head` but stores NO sample (SampleRing::skip): the 32 word stores to SDRAM are the larger part of a block's tap cost, and an app that reads only the
//            statistics (LOUDNESS VU) never looks at them. The positions written while it was clear hold STALE words: a sample reader (read_latest, read_range) must start at a position
//            written after it set the flag and the render saw it, i.e. at head() + 2 blocks (ModuleUi: tap_fresh_, set by arm). The statistics are always written.
// There is no read-modify-write anywhere (no LDREX / STREX: the SDRAM alias rule of platform/mk66 sdram_view.h): plain loads and stores of 32-bit words.
//
// Cost (rt-optimize note). Per block with the tap on: mono_mix 64 frames (32 word pairs) + meter (32 pairs, SMLALD) + 3 stats stores + the cost counters, and, while `samples` is set, 32 word
// stores to SDRAM. The two kernels are the ones the meters use (`diag bench kernels`: k_mono_mix, k_meter). The unit measured the tap at mean 1,765 / max 6,077 cycles with the samples written
// (QA-MOD-023, image int.6) against ADR 0014's host estimate of 600..1,400 at worst: by the code the store loop is the large part (9 instructions an iteration, 32 times, plus the wait of
// each store on the SDRAM bus, which the estimate took as 10..28 cycles); the split is to be confirmed by QA-MOD-023 with the samples clear and set. With `samples`
// clear a block costs the two kernels, 4 stores and the counters. ADR 0014 amendment J has the figures. Off: one relaxed load.

#include <o3os/audio/hot.h>
#include <o3os/audio/kernels.h>
#include <o3os/audio/taps.h>
#include <o3os/hal/audio.h>

#include <atomic>
#include <cstdint>

namespace o3os::audio {

class AppTap {
 public:
  // The values are the SDK's O3M_TAP_* (module/sdk/o3module.h).
  enum class Source : std::uint32_t { Off = 0, Input = 1, Master = 2 };

  static constexpr std::uint32_t kBlock = hal::audio::kBlock;
  static constexpr std::uint32_t kBlocks = 64;  // a power of two
  using Ring = SampleRing<kBlocks * kBlock, kBlock>;
  static constexpr std::uint32_t kMaxFrames = Ring::max_window();  // the largest window or backlog a reader can rely on: (kBlocks - 1) blocks
  static_assert((kBlocks & (kBlocks - 1u)) == 0u && kBlock == 64u, "the stats slot of a block is (position >> 6) & (kBlocks - 1)");

  AppTap() noexcept { reset(); }
  AppTap(const AppTap&) = delete;
  AppTap& operator=(const AppTap&) = delete;
  // Everything back to zero, tap off. Only while the render cannot run (before start, between tests).
  void reset() noexcept;

  // ---- Main loop ---------------------------------------------------------------------------------------------------------------
  void set_source(Source s) noexcept { source_.store(static_cast<std::uint32_t>(s), std::memory_order_relaxed); }
  Source source() const noexcept { return static_cast<Source>(source_.load(std::memory_order_relaxed)); }
  // Whether the render stores the samples of a block (see `samples` above). The default is yes; ModuleUi clears it when an audio_tap app starts and sets it when the app first reads samples.
  // Set it BEFORE reading head() to compute the first position a sample reader may trust (head() + 2 blocks): a block the render has already started when the flag lands is skipped.
  void set_samples(bool on) noexcept { samples_.store(on ? 1u : 0u, std::memory_order_relaxed); }
  bool samples() const noexcept { return samples_.load(std::memory_order_relaxed) != 0u; }
  // Frames ever written (the sequence counter): a cursor to start a stream from.
  std::uint32_t head() const noexcept { return ring_.head(); }
  // The newest `count` frames (even, at most kMaxFrames, `dst` 4-byte aligned); zeros in front before `count` frames exist. See SampleRing::read_latest.
  Ring::Window read_latest(std::int16_t* dst, std::uint32_t count) const noexcept { return ring_.read_latest(dst, count); }

  // The `count` frames from the free-running position `start` (see SampleRing::read_range); false if not all written or lapped.
  bool read_range(std::uint32_t start, std::int16_t* dst, std::uint32_t count) const noexcept { return ring_.read_range(start, dst, count); }

  // What the blocks from `cursor` (frames, a multiple of kBlock) up to the head measured.
  struct Levels {
    std::uint32_t frames = 0;       // frames measured (0: nothing new)
    std::uint32_t lost = 0;         // frames in front of the first one measured that the reader was too slow for
    std::uint32_t peak = 0;         // largest |sample| in them, 0..32768
    std::uint64_t sumsq = 0;        // sum of the squares of those frames
    std::uint32_t clip_blocks = 0;  // blocks whose peak reached full scale (>= 32767)
  };
  // Folds the stats of the blocks [cursor, head) into `out` and moves `cursor` to the head. False when it could not get a whole read in 3 tries (the figures are those of the last try).
  bool levels_since(std::uint32_t& cursor, Levels& out) const noexcept;

  // Linear amplitudes (1.0 = full scale) of a Levels. Main loop only: the RMS takes a square root.
  static float linear_peak(const Levels& l) noexcept { return static_cast<float>(l.peak) * (1.0f / 32768.0f); }
  static float linear_rms(const Levels& l) noexcept;

  // The cost of the tap work as the render timed it (cycles of the whole tap work of a block: mix, stats, ring write).
  struct Cost {
    std::uint32_t blocks = 0;
    std::uint32_t last = 0;
    std::uint32_t max = 0;
    std::uint32_t sum = 0;  // wraps after about 4 million blocks (a day and a half): reset_cost() before a long measurement
  };
  Cost cost() const noexcept {
    return Cost{cost_blocks_.load(std::memory_order_relaxed), cost_last_.load(std::memory_order_relaxed), cost_max_.load(std::memory_order_relaxed), cost_sum_.load(std::memory_order_relaxed)};
  }
  // `diag cpu reset`: asks the render to zero its figures (it does so at its next block: the render is their one writer).
  void reset_cost() noexcept { cost_reset_.store(cost_reset_.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed); }

  // ---- Render ------------------------------------------------------------------------------------------------------------------
  // One block: `mono` is kBlock int16 samples, 4-byte aligned. Wait-free, bounded (64 frames).
  O3OS_AUDIO_HOT void push(const std::int16_t* mono) noexcept;
  // The cycles the tap work of this block took. The render keeps the running figures in the object and publishes them every kPublishBlocks blocks (a reader sees them that late): fewer
  // SDRAM accesses per block than publishing every block.
  O3OS_AUDIO_INLINE void note_cost(std::uint32_t cycles) noexcept {
    const std::uint32_t rq = cost_reset_.load(std::memory_order_relaxed);
    if (rq != cost_reset_seen_) {
      cost_reset_seen_ = rq;
      priv_.blocks = priv_.last = priv_.max = priv_.sum = 0;  // (field by field: a Cost{} assignment becomes a memset call, which SRAM_L may not make)
      cost_blocks_.store(0, std::memory_order_relaxed);
      cost_last_.store(0, std::memory_order_relaxed);
      cost_max_.store(0, std::memory_order_relaxed);
      cost_sum_.store(0, std::memory_order_relaxed);
    }
    ++priv_.blocks;
    priv_.sum += cycles;
    priv_.last = cycles;
    priv_.max = cycles > priv_.max ? cycles : priv_.max;
    if ((priv_.blocks & (kPublishBlocks - 1u)) == 0u) {
      cost_blocks_.store(priv_.blocks, std::memory_order_relaxed);
      cost_last_.store(priv_.last, std::memory_order_relaxed);
      cost_max_.store(priv_.max, std::memory_order_relaxed);
      cost_sum_.store(priv_.sum, std::memory_order_relaxed);
    }
  }
  static constexpr std::uint32_t kPublishBlocks = 16;  // a power of two

 private:
  std::atomic<std::uint32_t> source_{0};
  std::atomic<std::uint32_t> samples_{1};  // main loop writes, the render reads once a block (relaxed): the block's samples are stored (1) or only its statistics (0)
  Ring ring_;
  std::atomic<std::uint32_t> peak_[kBlocks];
  std::atomic<std::uint32_t> sq_lo_[kBlocks];
  std::atomic<std::uint32_t> sq_hi_[kBlocks];
  // cost: priv_ is the render's, the atomics are what diag reads
  Cost priv_;
  std::uint32_t cost_reset_seen_ = 0;
  std::atomic<std::uint32_t> cost_reset_{0};
  std::atomic<std::uint32_t> cost_blocks_{0}, cost_last_{0}, cost_max_{0}, cost_sum_{0};
};

}  // namespace o3os::audio
