// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-005b: the meters (audio/meters.h) against brute-force oracles. A MeterBank is fed random blocks through the real kernel for several
// channels; after every window the published snapshot must equal what a plain scan of all the samples of that window says: the peak, the sum of
// squares, the count of windows that reached full scale, the hold (a sliding maximum of the last 32 window peaks, recomputed from scratch each
// time), the frame count and the window number. Also: the dB text of `diag mix`, the linear levels the plot library takes, and the window
// timing. Host and Cortex-M4F.
#include <doctest/doctest.h>

#include <o3os/audio/diag.h>
#include <o3os/audio/kernels.h>
#include <o3os/audio/meters.h>
#include <o3os/hal/audio.h>

#include "support/rng.h"
#include "support/static_slot.h"

#include <cstdint>
#include <cstring>

namespace {

using namespace o3os::audio;
using o3os::test::Rng;
namespace ha = o3os::hal::audio;

constexpr std::uint32_t kB = ha::kBlock;
constexpr unsigned kChannelsUsed = 5;  // a few channels carry signals, the rest stay untouched

// What one window of one channel is, by scanning every sample.
struct Oracle {
  std::uint32_t peak = 0;
  std::uint64_t sumsq = 0;
  std::uint32_t clips = 0;
  std::uint32_t history[4096] = {};
  std::uint32_t windows = 0;
  void window(std::uint32_t p, std::uint64_t s) {
    peak = p;
    sumsq = s;
    clips += p >= kMeterClipLevel ? 1u : 0u;
    history[windows++] = p;
  }
  std::uint32_t hold() const {
    std::uint32_t m = 0;
    const std::uint32_t first = windows > kMeterHoldWindows ? windows - kMeterHoldWindows : 0u;
    for (std::uint32_t w = first; w < windows; ++w) {
      m = history[w] > m ? history[w] : m;
    }
    return m;
  }
};

}  // namespace

TEST_CASE("meters: every published window equals a brute-force scan (peak, sum of squares, clips, hold, frames)") {
  static o3os::test::StaticSlot<MeterBank> slot;
  static MeterStore store;  // plain storage: the MK66 puts it in SRAM_L, the cases in static storage
  MeterBank& bank = slot.make(store);
  static Oracle oracle[kChannelsUsed];
  for (Oracle& o : oracle) {
    o = Oracle{};
  }
  MeterSnapshot s;
  CHECK(bank.snapshot(s));
  CHECK(s.windows == 0u);  // nothing measured yet: a whole snapshot of zeros
  CHECK(s.ch[0].peak == 0u);

  Rng rng(41);
  constexpr unsigned kWindows = 120;  // hold's ring (32) wraps three times and loud windows expire
  std::uint32_t win_peak[kChannelsUsed] = {};
  std::uint64_t win_sumsq[kChannelsUsed] = {};
  for (unsigned w = 0; w < kWindows; ++w) {
    for (unsigned c = 0; c < kChannelsUsed; ++c) {
      win_peak[c] = 0;
      win_sumsq[c] = 0;
    }
    // Loudness changes over the run: a loud stretch, a quiet one, rails now and then.
    const std::int32_t amp = (w % 40u) < 6u ? 32767 : ((w % 40u) < 20u ? 3000 : 200);
    for (std::uint32_t blk = 0; blk < kMeterWindowBlocks; ++blk) {
      for (unsigned c = 0; c < kChannelsUsed; ++c) {
        if (c == 4u && (w % 3u) != 0u) {
          continue;  // channel 4 sounds only sometimes: a window without any block of it
        }
        alignas(8) std::int16_t block[kB];
        const bool rail = rng.below(40) == 0 && c == 1u;
        for (std::uint32_t i = 0; i < kB; ++i) {
          std::int32_t v = static_cast<std::int32_t>(rng.below(static_cast<std::uint32_t>(2 * amp + 1))) - amp;
          if (rail && i == 9) {
            v = -32768;
          }
          block[i] = static_cast<std::int16_t>(v);
          const std::uint32_t a = static_cast<std::uint32_t>(v < 0 ? -v : v);
          win_peak[c] = a > win_peak[c] ? a : win_peak[c];
          win_sumsq[c] += static_cast<std::uint64_t>(static_cast<std::int64_t>(v) * v);
        }
        kernel::meter(block, kB, bank.acc(meter_ch::kTrack0 + c));
      }
      const std::uint32_t before = bank.windows();
      bank.end_block();
      CHECK(bank.windows() == before + (blk + 1u == kMeterWindowBlocks ? 1u : 0u));  // a window is exactly kMeterWindowBlocks blocks
    }
    for (unsigned c = 0; c < kChannelsUsed; ++c) {
      oracle[c].window(win_peak[c], win_sumsq[c]);
    }
    CHECK(bank.snapshot(s));
    CHECK(s.windows == w + 1u);
    CHECK(s.frames == kMeterWindowBlocks * kB);
    for (unsigned c = 0; c < kChannelsUsed; ++c) {
      INFO("window " << w << " channel " << c);
      CHECK(s.ch[c].peak == oracle[c].peak);
      CHECK(s.ch[c].sumsq == oracle[c].sumsq);
      CHECK(s.ch[c].clips == oracle[c].clips);
      CHECK(s.ch[c].hold == oracle[c].hold());
      CHECK(s.ch[c].hold >= s.ch[c].peak);
    }
    for (unsigned c = kChannelsUsed; c < kMeterChannels; ++c) {
      CHECK(s.ch[c].peak == 0u);  // untouched channels stay silent
      CHECK(s.ch[c].hold == 0u);
      CHECK(s.ch[c].clips == 0u);
      CHECK(s.ch[c].sumsq == 0u);
    }
  }
  // The runs above include windows where the hold expired (a 32767 stretch followed by 26+ quiet windows): the sliding maximum fell.
  CHECK(oracle[0].clips > 0u);
  CHECK(oracle[1].clips > 0u);
}

TEST_CASE("meters: the plot library's linear levels, the dB text of `diag mix`") {
  static o3os::test::StaticSlot<MeterBank> slot;
  static MeterStore store;  // plain storage: the MK66 puts it in SRAM_L, the cases in static storage
  MeterBank& bank = slot.make(store);
  // A constant block of 16384 (half scale) on channel 0, a full-scale constant on channel 1, silence elsewhere.
  alignas(8) std::int16_t half[kB], full[kB];
  for (std::uint32_t i = 0; i < kB; ++i) {
    half[i] = 16384;
    full[i] = 32767;
  }
  for (std::uint32_t blk = 0; blk < kMeterWindowBlocks; ++blk) {
    kernel::meter(half, kB, bank.acc(0));
    kernel::meter(full, kB, bank.acc(1));
    bank.end_block();
  }
  MeterSnapshot s;
  CHECK(bank.snapshot(s));
  const MeterLevel a = meter_level(s, 0);
  CHECK(a.peak == 0.5f);
  CHECK(a.hold == 0.5f);
  CHECK(a.rms > 0.4999f);
  CHECK(a.rms < 0.5001f);  // a constant's RMS is the constant
  const MeterLevel b = meter_level(s, 1);
  CHECK(b.peak > 0.99996f);
  CHECK(b.peak < 1.0f);
  const MeterLevel none = meter_level(s, 5);
  CHECK(none.peak == 0.0f);
  CHECK(none.rms == 0.0f);
  const MeterLevel bad = meter_level(s, 999);
  CHECK(bad.peak == 0.0f);

  char buf[1024];
  const std::size_t n = diag::format_mix(s, buf, sizeof buf);
  CHECK(n == std::strlen(buf));
  const char* const expected =
      "mix  windows 1  frames 1408  (dBFS; clip = windows at full scale)\n"
      "  T01  pk  -6.0  hold  -6.0  rms  -6.0  clip 0\n"
      "  T02  pk   0.0  hold   0.0  rms   0.0  clip 1\n"
      "  MST L  pk  -inf  hold  -inf  rms  -inf  clip 0\n"
      "  MST R  pk  -inf  hold  -inf  rms  -inf  clip 0\n";
  CHECK_MESSAGE(std::strcmp(buf, expected) == 0, buf);
  char small[30];
  diag::format_mix(s, small, sizeof small);
  CHECK(std::strlen(small) == sizeof small - 1u);  // bounded and terminated

  MeterSnapshot empty{};
  diag::format_mix(empty, buf, sizeof buf);
  CHECK(std::strstr(buf, "nothing measured yet") != nullptr);
}
