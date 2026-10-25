// SPDX-License-Identifier: GPL-3.0-or-later
//
// The lock-free pieces of the graph's taps under a real concurrent writer and reader (host only: std::thread; also the TSan run,
// `audio_taps_tsan`): the meter snapshot is a seqlock and the sample rings detect that the writer lapped a window. A snapshot or window that is
// reported whole must be one writer's consistent state, never a mixture.
#include <doctest/doctest.h>

#include <o3os/audio/app_tap.h>
#include <o3os/audio/meters.h>
#include <o3os/audio/taps.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

TEST_CASE("audio taps thread: a whole meter snapshot is one window's publish") {
  using namespace o3os::audio;
  MeterStore store;
  MeterBank bank(store);
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    std::uint32_t w = 1;  // the window being filled
    while (!stop.load(std::memory_order_relaxed)) {
      for (std::uint32_t blk = 0; blk < kMeterWindowBlocks; ++blk) {
        for (unsigned c = 0; c < kMeterChannels; ++c) {
          bank.acc(c).peak = w;                 // everything about window w derives from w
          bank.acc(c).sumsq = (static_cast<std::uint64_t>(w) << 32) + c;
        }
        bank.end_block();
      }
      ++w;
    }
  });
  std::uint64_t whole = 0;
  std::uint64_t torn = 0;
  for (std::uint32_t i = 0; i < 200000; ++i) {
    MeterSnapshot s;
    if (!bank.snapshot(s)) {
      continue;  // the bounded retries ran out inside a publish: reported as such
    }
    ++whole;
    bool ok = s.frames == (s.windows == 0u ? 0u : kMeterWindowBlocks * 64u);
    for (unsigned c = 0; c < kMeterChannels && s.windows != 0u; ++c) {
      ok = ok && s.ch[c].peak == s.windows && s.ch[c].hold == s.windows && s.ch[c].sumsq == ((static_cast<std::uint64_t>(s.windows) << 32) + c);
    }
    torn += ok ? 0u : 1u;
  }
  stop.store(true);
  writer.join();
  CHECK(whole > 1000u);
  CHECK(torn == 0u);
}

TEST_CASE("audio taps thread: a whole window of a sample ring is consecutive samples, whatever the writer does meanwhile") {
  using namespace o3os::audio;
  SpectrumRing ring;
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    alignas(4) std::int16_t block[64];
    std::uint32_t counter = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      for (std::uint32_t i = 0; i < 64; ++i) {
        block[i] = static_cast<std::int16_t>(counter++);
      }
      ring.write(block, 64);
    }
  });
  std::uint64_t whole = 0;
  std::uint64_t lapped = 0;
  std::uint64_t torn = 0;
  alignas(4) std::int16_t window[kSpectrumWindowFrames];
  for (std::uint32_t i = 0; i < 100000; ++i) {
    const SpectrumRing::Window w = ring.read_latest(window, kSpectrumWindowFrames);
    if (!w.whole) {
      ++lapped;
      continue;
    }
    ++whole;
    if (w.head < kSpectrumWindowFrames) {
      continue;  // not yet a full window
    }
    bool ok = true;
    for (std::uint32_t k = 1; k < kSpectrumWindowFrames; ++k) {
      ok = ok && static_cast<std::int16_t>(window[k] - window[k - 1u]) == 1;
    }
    ok = ok && static_cast<std::int16_t>(window[kSpectrumWindowFrames - 1u] + 1) == static_cast<std::int16_t>(w.head);  // the newest is head - 1
    torn += ok ? 0u : 1u;
  }
  stop.store(true);
  writer.join();
  CHECK(whole > 1000u);
  CHECK(torn == 0u);
  (void)lapped;  // how often the writer lapped the reader is up to the scheduler; what matters is that a reported-whole window never tears
}

// OS-H2a: the apps' audio tap. The render writes one block at a time (the samples of block n are all n, so a whole window is monotone block by block and never a mixture), the main loop
// reads windows and level stats concurrently.
TEST_CASE("audio taps thread: the app tap's windows and level stats are whole blocks of one write, whatever the writer does meanwhile") {
  using namespace o3os::audio;
  static AppTap tap;
  tap.reset();
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    alignas(8) std::int16_t block[AppTap::kBlock];
    std::uint32_t n = 1;  // every sample of block n is n, so its peak is n and its sum of squares is 64 n^2
    while (!stop.load(std::memory_order_relaxed)) {
      for (std::uint32_t i = 0; i < AppTap::kBlock; ++i) {
        block[i] = static_cast<std::int16_t>(n & 0x3FFF);
      }
      tap.push(block);
      ++n;
    }
  });
  std::uint64_t windows = 0;
  std::uint64_t torn = 0;
  std::uint64_t stats = 0;
  std::uint64_t bad_stats = 0;
  std::uint32_t cursor = 0;
  alignas(4) std::int16_t window[8 * AppTap::kBlock];
  for (std::uint32_t i = 0; i < 100000; ++i) {
    const AppTap::Ring::Window w = tap.read_latest(window, 8 * AppTap::kBlock);
    if (w.whole && w.head >= 8u * AppTap::kBlock) {
      ++windows;
      bool ok = true;
      for (std::uint32_t b = 0; b < 8u; ++b) {  // every block of the window is one value, and they count up by one
        for (std::uint32_t k = 1; k < AppTap::kBlock; ++k) {
          ok = ok && window[b * AppTap::kBlock + k] == window[b * AppTap::kBlock];
        }
        ok = ok && (b == 0u || ((window[b * AppTap::kBlock] - window[(b - 1u) * AppTap::kBlock]) & 0x3FFF) == 1);
      }
      torn += ok ? 0u : 1u;
    }
    AppTap::Levels l;
    cursor = tap.head() - AppTap::kBlock;  // the newest block (or a few, if the writer moved on meanwhile)
    if (tap.levels_since(cursor, l)) {
      ++stats;
      // a whole read of ONE block: its peak and its sum of squares were written separately and must belong to the same block (every sample is the peak)
      bad_stats += (l.frames == AppTap::kBlock && l.sumsq != static_cast<std::uint64_t>(AppTap::kBlock) * l.peak * l.peak) ? 1u : 0u;
    }
  }
  stop.store(true);
  writer.join();
  CHECK(windows > 1000u);
  CHECK(torn == 0u);
  CHECK(stats > 1000u);
  CHECK(bad_stats == 0u);
}

// OS-BUG-TAPWRITE, a smoke test of the protocol: the sample feed is switched on and off while a writer thread runs (the reader sets the flag, then reads the head: the first position it
// may trust is two blocks on), and the ranges read from that position should each be two whole blocks of one write. It does not prove that no stale word is returned: the writer and the
// reader here are ordered by the host's memory model, not by the M4's one core and its render interrupt. The proofs of freshness are module_host_api_test and audio_app_tap_test.
TEST_CASE("audio taps thread: the app tap's sample feed, switched on while a writer runs, reads whole blocks from the position the reader computed (smoke test)") {
  using namespace o3os::audio;
  static AppTap tap;
  tap.reset();
  tap.set_samples(false);
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    alignas(8) std::int16_t block[AppTap::kBlock];
    std::uint32_t n = 1;  // every sample of block n is n
    while (!stop.load(std::memory_order_relaxed)) {
      for (std::uint32_t i = 0; i < AppTap::kBlock; ++i) {
        block[i] = static_cast<std::int16_t>(n & 0x3FFF);
      }
      tap.push(block);
      ++n;
      std::this_thread::sleep_for(std::chrono::microseconds(20));  // (a render is paced by the codec: the reader must not be lapped all the time)
    }
  });
  std::uint64_t reads = 0;
  std::uint64_t bad = 0;
  alignas(4) std::int16_t win[2 * AppTap::kBlock];
  for (int iter = 0; iter < 300; ++iter) {
    tap.set_samples(false);  // levels only for a while: the stores stop, the words of the ring keep the lap before
    for (int k = 0; k < 4; ++k) {
      std::this_thread::yield();
    }
    tap.set_samples(true);                                             // the flag first ...
    const std::uint32_t fresh = tap.head() + 2u * AppTap::kBlock;      // ... then the head: nothing before this position is trusted
    for (std::uint32_t spin = 0; spin < 200000u && static_cast<std::int32_t>(tap.head() - fresh) < static_cast<std::int32_t>(3u * AppTap::kBlock); ++spin) {
      std::this_thread::yield();
    }
    if (tap.read_range(fresh, win, 2u * AppTap::kBlock)) {
      ++reads;
      bool ok = true;
      for (std::uint32_t k = 1; k < AppTap::kBlock; ++k) {
        ok = ok && win[k] == win[0] && win[AppTap::kBlock + k] == win[AppTap::kBlock];
      }
      ok = ok && ((win[AppTap::kBlock] - win[0]) & 0x3FFF) == 1;  // block n, then block n + 1
      bad += ok ? 0u : 1u;
    }
  }
  stop.store(true);
  writer.join();
  CHECK(reads > 100u);
  CHECK(bad == 0u);
}
