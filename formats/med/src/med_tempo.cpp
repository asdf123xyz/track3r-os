// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/formats/med/tempo.h>

#include <o3os/model/limits.h>

#include <algorithm>

namespace o3os::formats::med {

static_assert(kMinBpmX100 == model::kMinTempoX100 && kMaxBpmX100 == model::kMaxTempoX100,
              "the MED tempo range is the model's tempo range");

namespace {

// BPM8 of tempo 1..10 in 8-channel mode (see tempo.h for the sources).
constexpr std::uint32_t kEightChannel[10] = {179, 164, 152, 141, 131, 123, 116, 110, 104, 99};

constexpr std::uint64_t kSpeedNumerator = 474326;   // T_tick = 474,326 / (CIA clock x tempo)
constexpr std::uint64_t kCompatX100 = 440550;       // 15 x 293.7 x 100
constexpr std::uint64_t kMixLowX100 = 94716;        // 15 x 157.86 / 2.5 x 100

inline std::uint64_t div_round(std::uint64_t n, std::uint64_t d) noexcept { return (n + d / 2) / d; }

}  // namespace

TempoModel tempo_model(const SongInfo& song, Clock clock) noexcept {
  TempoModel m;
  m.clock = clock;
  m.mixing = song.mixing();
  m.lines_per_beat = static_cast<std::uint8_t>(song.lines_per_beat());
  if (song.eight_channel()) {
    m.mode = TempoMode::EightChannel;
  } else if (song.bpm_mode()) {
    m.mode = TempoMode::Bpm;
  } else {
    m.mode = TempoMode::Speed;
  }
  return m;
}

Bpm tempo_to_bpm(const TempoModel& model, std::uint32_t tempo, std::uint32_t spd) noexcept {
  Bpm out;
  bool bad = false;
  if (spd == 0) {
    spd = 1;
    bad = true;
  }
  std::uint64_t v = 0;
  if (tempo == 0) {
    bad = true;
  } else if (model.mode == TempoMode::EightChannel) {
    v = div_round(600ull * kEightChannel[std::min<std::uint32_t>(tempo, 10) - 1], spd);
  } else if (model.mode == TempoMode::Bpm) {
    v = div_round(150ull * tempo * std::max<std::uint32_t>(model.lines_per_beat, 1), spd);
  } else if (model.mixing && tempo < 8) {
    v = div_round(kMixLowX100, spd);
  } else if (tempo <= 10 && !model.mixing) {
    v = div_round(kCompatX100, static_cast<std::uint64_t>(tempo) * spd);
  } else {
    const std::uint64_t clock = model.clock == Clock::Ntsc ? kCiaNtscHz : kCiaPalHz;
    v = div_round(1500ull * clock * tempo, kSpeedNumerator * spd);
  }
  if (v < kMinBpmX100) {
    out.x100 = kMinBpmX100;
    out.clamp = Clamp::Low;
  } else if (v > kMaxBpmX100) {
    out.x100 = kMaxBpmX100;
    out.clamp = Clamp::High;
  } else {
    out.x100 = static_cast<std::uint32_t>(v);
  }
  if (bad) {
    out.clamp = Clamp::BadInput;
  }
  return out;
}

}  // namespace o3os::formats::med
