// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/audio/meters.h>

#include <o3os/hal/audio.h>

#include <cmath>

namespace o3os::audio {
namespace {

// The seqlock's data accesses (as in status.cpp): relaxed words ordered by the generation counter and a fence on each side; under
// ThreadSanitizer, which does not model fences, the same protocol with every access sequentially consistent.
#if defined(__SANITIZE_THREAD__)
constexpr std::memory_order kData = std::memory_order_seq_cst;
inline void fence_release() noexcept {}
inline void fence_acquire() noexcept {}
#else
constexpr std::memory_order kData = std::memory_order_relaxed;
O3OS_AUDIO_INLINE void fence_release() noexcept { std::atomic_thread_fence(std::memory_order_release); }
O3OS_AUDIO_INLINE void fence_acquire() noexcept { std::atomic_thread_fence(std::memory_order_acquire); }
#endif

}  // namespace

void MeterStore::reset() noexcept {
  for (unsigned c = 0; c < kMeterChannels; ++c) {
    for (unsigned w = 0; w < kMeterHoldWindows; ++w) {
      history[c][w] = 0;
    }
  }
  generation.store(0, std::memory_order_relaxed);
  for (unsigned i = 0; i < kWords; ++i) {
    words[i].store(0, std::memory_order_relaxed);
  }
}

void MeterBank::reset() noexcept {
  store_.reset();
  for (unsigned c = 0; c < kMeterChannels; ++c) {
    acc_[c] = kernel::MeterAcc{0u, 0u};
    clips_[c] = 0;
    hold_[c] = 0;
  }
  blocks_ = 0;
  windows_published_ = 0;
}

void MeterBank::end_block() noexcept {
  if (++blocks_ < kMeterWindowBlocks) {
    return;
  }
  blocks_ = 0;
  const std::uint32_t ring = windows_published_ & (kMeterHoldWindows - 1u);

  // The hold is a sliding maximum over the last kMeterHoldWindows peaks. The window that leaves is the one this one overwrites; the
  // maximum only has to be searched for again when that was the maximum.
  std::uint32_t peak[kMeterChannels];
  for (unsigned c = 0; c < kMeterChannels; ++c) {
    peak[c] = acc_[c].peak;
    const std::uint32_t leaving = store_.history[c][ring];
    store_.history[c][ring] = static_cast<std::uint16_t>(peak[c]);
    if (peak[c] >= hold_[c]) {
      hold_[c] = peak[c];
    } else if (leaving == hold_[c]) {
      std::uint32_t m = 0;
      for (unsigned w = 0; w < kMeterHoldWindows; ++w) {
        m = store_.history[c][w] > m ? store_.history[c][w] : m;
      }
      hold_[c] = m;
    }
    clips_[c] += peak[c] >= kMeterClipLevel ? 1u : 0u;
  }
  ++windows_published_;

  // Publish under the seqlock: odd while the words are inconsistent.
  const std::uint32_t g = store_.generation.load(std::memory_order_relaxed);
  store_.generation.store(g + 1u, kData);
  fence_release();
  store_.words[MeterStore::kWordWindows].store(windows_published_, kData);
  store_.words[MeterStore::kWordFrames].store(kMeterWindowBlocks * hal::audio::kBlock, kData);
  for (unsigned c = 0; c < kMeterChannels; ++c) {
    std::atomic<std::uint32_t>* w = &store_.words[MeterStore::kWordFirstChannel + c * MeterStore::kWordsPerChannel];
    w[0].store(peak[c], kData);
    w[1].store(hold_[c], kData);
    w[2].store(clips_[c], kData);
    w[3].store(static_cast<std::uint32_t>(acc_[c].sumsq & 0xFFFFFFFFu), kData);
    w[4].store(static_cast<std::uint32_t>(acc_[c].sumsq >> 32), kData);
    acc_[c] = kernel::MeterAcc{0u, 0u};
  }
  store_.generation.store(g + 2u, std::memory_order_release);
}

bool MeterBank::snapshot(MeterSnapshot& out) const noexcept {
  constexpr unsigned kTries = 8;
  for (unsigned attempt = 0; attempt < kTries; ++attempt) {
    const std::uint32_t g1 = store_.generation.load(std::memory_order_acquire);
    out.windows = store_.words[MeterStore::kWordWindows].load(kData);
    out.frames = store_.words[MeterStore::kWordFrames].load(kData);
    for (unsigned c = 0; c < kMeterChannels; ++c) {
      const std::atomic<std::uint32_t>* w = &store_.words[MeterStore::kWordFirstChannel + c * MeterStore::kWordsPerChannel];
      out.ch[c].peak = w[0].load(kData);
      out.ch[c].hold = w[1].load(kData);
      out.ch[c].clips = w[2].load(kData);
      out.ch[c].sumsq = static_cast<std::uint64_t>(w[3].load(kData)) | (static_cast<std::uint64_t>(w[4].load(kData)) << 32);
    }
    fence_acquire();
    const std::uint32_t g2 = store_.generation.load(std::memory_order_relaxed);
    if (g1 == g2 && (g1 & 1u) == 0u) {
      return true;
    }
  }
  return false;
}

MeterLevel meter_level(const MeterSnapshot& s, unsigned channel) noexcept {
  MeterLevel r{0.0f, 0.0f, 0.0f};
  if (channel >= kMeterChannels) {
    return r;
  }
  const MeterChannelSnapshot& c = s.ch[channel];
  constexpr float kScale = 1.0f / 32768.0f;
  r.peak = static_cast<float>(c.peak) * kScale;
  r.hold = static_cast<float>(c.hold) * kScale;
  if (s.frames != 0u) {
    // sumsq is at most 1408 * 2^30 (41 bits): high and low words combine in float without a 64-bit conversion.
    const float sumsq = static_cast<float>(static_cast<std::uint32_t>(c.sumsq >> 32)) * 4294967296.0f + static_cast<float>(static_cast<std::uint32_t>(c.sumsq & 0xFFFFFFFFu));
    r.rms = std::sqrt(sumsq / static_cast<float>(s.frames)) * kScale;
  }
  return r;
}

}  // namespace o3os::audio
