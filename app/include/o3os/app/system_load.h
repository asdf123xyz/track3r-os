// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The status strip's CPU and MEM readouts (OS-FU3, new Community OS behaviour).
//
// CPU is the audio render's load: the cycles the render of one block took, over the cycles of the block, as the PEAK of a ~0.5 s window (a
// meter that shows the last render would flicker between idle and busy blocks, and the figure of interest is the worst). MEM is the sample
// arena's fill. The App holds no platform code: the platform hands it an AudioLoadSource (read-only, main loop only), which on the device
// reads audio::AudioStatus::snapshot() (the cycles are the DWT counter's); the simulator gives none and the strip shows "--".

#include <cstdint>

namespace o3os::app {

// OS-GOV (new Community OS behaviour): the CPU readout's colour. Above kCpuWarnPercent the figure sits on an amber chip (the theme's Accent), above kCpuAlertPercent on a red one (Rec): the render is
// closer to its limit than the module governor (audio/module_governor.h) lets a new voice take it, which is the same 90 % (a build check in app.cpp holds the two equal). Percent of the block, the
// CpuMeter's figure (the peak of half a second); an unknown figure (kUnknown, no source) is normal. HYSTERESIS: a level is left only when the figure is kCpuHysteresis points under the threshold that
// entered it, so a load that sits on 70 or 90 does not make the chip flicker (next_cpu_level; the App keeps the level in SystemLoad::level).
inline constexpr std::int8_t kCpuWarnPercent = 70;
inline constexpr std::int8_t kCpuAlertPercent = 90;
inline constexpr std::int8_t kCpuHysteresis = 3;
enum class CpuLevel : std::uint8_t { Normal, Warn, Alert };
// The level of a figure with no history: the thresholds that ENTER a level.
constexpr CpuLevel cpu_level(std::int8_t cpu) noexcept { return cpu > kCpuAlertPercent ? CpuLevel::Alert : (cpu > kCpuWarnPercent ? CpuLevel::Warn : CpuLevel::Normal); }
// The level after `cpu`, given the one before: a level is kept down to kCpuHysteresis under its entry threshold.
constexpr CpuLevel next_cpu_level(CpuLevel previous, std::int8_t cpu) noexcept {
  if (cpu < 0) {
    return CpuLevel::Normal;
  }
  if (cpu > kCpuAlertPercent || (previous == CpuLevel::Alert && cpu > kCpuAlertPercent - kCpuHysteresis)) {
    return CpuLevel::Alert;
  }
  if (cpu > kCpuWarnPercent || (previous != CpuLevel::Normal && cpu > kCpuWarnPercent - kCpuHysteresis)) {
    return CpuLevel::Warn;
  }
  return CpuLevel::Normal;
}

// A percentage 0..100 of a readout; kUnknown draws "--".
struct SystemLoad {
  static constexpr std::int8_t kUnknown = -1;
  std::int8_t cpu = kUnknown;
  std::int8_t mem = kUnknown;
  // OS-VOX: the sampler voices in use (0..`pool`; kUnknown: the platform gives none, the strip draws no indicator) of a pool of `pool`, and `steal`: a voice was
  // stolen in the last half second (the indicator is drawn inverted).
  std::int8_t voices = kUnknown;
  std::uint8_t pool = 0;
  bool steal = false;
  CpuLevel level = CpuLevel::Normal;  // OS-GOV: the colour of the CPU readout, with hysteresis (the App sets it from `cpu`; cpu_level() above)
  bool operator==(const SystemLoad& o) const noexcept { return cpu == o.cpu && mem == o.mem && voices == o.voices && pool == o.pool && steal == o.steal && level == o.level; }
  bool operator!=(const SystemLoad& o) const noexcept { return !(*this == o); }
};

// What the platform knows of the audio render. `render_max` is the all-time maximum (a writer's field the reader cannot reset), `block_cycles` the
// nominal cycles of one block (the rate's, hal::audio::kCyclesPerBlock).
struct AudioLoad {
  std::uint32_t render_last = 0;
  std::uint32_t render_max = 0;
  std::uint32_t block_cycles = 0;
  // OS-VOX (VoicePool::voice_snapshot, published by the render once a block): voices sounding or fading at the end of the last block, the pool's size (0: no figure) and the
  // steals since boot (a counter: the App turns a rise into a flash).
  std::uint32_t voices_in_use = 0;
  std::uint32_t voices_pool = 0;
  std::uint32_t steals = 0;
  // OS-GOV (audio::gov::Snapshot, governor_watch.h): the module governor's counters since boot (a rise is news) and its latest event, as audio::gov::Event << 8 | the instrument slot (0..63).
  std::uint32_t gov_drops = 0;
  std::uint32_t gov_mutes = 0;
  std::uint16_t gov_last = 0;
};

class AudioLoadSource {
 public:
  virtual ~AudioLoadSource() = default;
  virtual AudioLoad read() const noexcept = 0;  // main loop; never blocks
};

// The window peak. feed() is called once a pass with the platform's figures; it keeps the largest render seen in the window (the last render, and
// the all-time maximum when it rose since the previous feed: a spike between two feeds is not lost) and, when the window ends, publishes it.
class CpuMeter {
 public:
  static constexpr std::uint32_t kWindowMs = 500;

  // The published percent, or SystemLoad::kUnknown before the first window ends or without a block length.
  std::int8_t feed(std::uint64_t now_ms, const AudioLoad& a) noexcept {
    if (a.block_cycles == 0u) {
      return cpu_ = SystemLoad::kUnknown;
    }
    if (!primed_) {  // a maximum from before the App was built (a recovery) is not this window's
      primed_ = true;
      prev_max_ = a.render_max;
      start_ms_ = now_ms;
    }
    peak_ = peak_ > a.render_last ? peak_ : a.render_last;
    if (a.render_max > prev_max_) {
      peak_ = peak_ > a.render_max ? peak_ : a.render_max;
    }
    prev_max_ = a.render_max;
    if (now_ms - start_ms_ >= kWindowMs) {
      // A render longer than the block is a late block: 100. Below that, peak_ * 100 fits 32 bits for any block of a real rate (a UDIV, not the 64-bit helper).
      cpu_ = peak_ >= a.block_cycles ? std::int8_t{100} : static_cast<std::int8_t>(peak_ * 100u / a.block_cycles);
      peak_ = 0;
      start_ms_ = now_ms;
    }
    return cpu_;
  }

 private:
  bool primed_ = false;
  std::uint32_t prev_max_ = 0;
  std::uint32_t peak_ = 0;
  std::uint64_t start_ms_ = 0;
  std::int8_t cpu_ = SystemLoad::kUnknown;
};

// OS-VOX: the voices indicator's figures. `voices` is the PEAK of a half-second window of the voices in use (as the CPU's: a figure that followed every note
// would redraw the strip a dozen times a second), published when the window ends; `steal` is true for kHoldMs after the steal counter last rose. The first feed only takes
// the counter and starts the window (a steal from before the App was built is not news).
class VoiceMeter {
 public:
  static constexpr std::uint32_t kWindowMs = 500;
  static constexpr std::uint32_t kHoldMs = 500;

  void feed(std::uint64_t now_ms, std::uint32_t in_use, std::uint32_t steals) noexcept {
    if (!primed_) {
      primed_ = true;
      start_ms_ = now_ms;
    } else if (steals != prev_steals_) {
      until_ms_ = now_ms + kHoldMs;
    }
    prev_steals_ = steals;
    peak_ = peak_ > in_use ? peak_ : in_use;
    if (now_ms - start_ms_ >= kWindowMs) {
      shown_ = peak_;
      peak_ = in_use;
      start_ms_ = now_ms;
    }
  }
  std::uint32_t voices() const noexcept { return shown_ > peak_ ? shown_ : peak_; }  // before the first window ends: the peak so far
  bool steal(std::uint64_t now_ms) const noexcept { return now_ms < until_ms_; }

 private:
  bool primed_ = false;
  std::uint32_t prev_steals_ = 0;
  std::uint32_t peak_ = 0;
  std::uint32_t shown_ = 0;
  std::uint64_t start_ms_ = 0;
  std::uint64_t until_ms_ = 0;
};

// used / total as a whole percent rounded down (100 only when full), kUnknown for a total of 0. 32-bit arithmetic: both are halved until used * 100 fits.
constexpr std::int8_t percent_of(std::uint32_t used, std::uint32_t total) noexcept {
  if (total == 0u) {
    return SystemLoad::kUnknown;
  }
  if (used >= total) {
    return 100;
  }
  while (total > 0x028F5C28u) {  // 2^32 / 100
    used >>= 1;
    total >>= 1;
  }
  return static_cast<std::int8_t>(total != 0u ? used * 100u / total : 100u);
}

}  // namespace o3os::app
