// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/mix_board.h>

namespace o3os::app {

namespace {

constexpr float kScaleFloorDb = -60.0f;  // the dB scale of the bars: the floor of what a mixer needs to see
constexpr float kScaleCeilDb = 0.0f;

bool same(const ui::plot::MeterState& a, const ui::plot::MeterState& b) noexcept {
  return a.rms() == b.rms() && a.peak() == b.peak() && a.hold() == b.hold() && a.clipping() == b.clipping();
}

}  // namespace

void MixBoard::init() noexcept {
  for (unsigned i = 0; i < kMixTracks; ++i) {
    route[i] = static_cast<std::uint8_t>(audio::route::kMain | audio::route::kUsb);  // graph.cpp init_defaults: a track is in the USB column from the start
  }
  route[audio::kInputStrip] = static_cast<std::uint8_t>(audio::route::kMain | audio::route::kCue);
  ret_route[0] = audio::route::kMain;
  ret_route[1] = audio::route::kMain;
  out_mode = static_cast<std::uint8_t>(audio::OutMode::Stereo);
  cue_level = 100;
  in_level = 0;
  for (unsigned i = 0; i < kSendFxRows; ++i) {
    fx[i] = kSendFxTable[i].def;
  }
  for (unsigned i = 0; i < kLimiterRows; ++i) {
    lim[i] = kLimiterTable[i].def;
  }
  scale.init(kScaleFloorDb, kScaleCeilDb);
  for (ui::plot::MeterState& m : meter) {
    m.reset();
  }
  windows = 0;
}

MixBoard::Bar MixBoard::track_bar(unsigned track) const noexcept {
  Bar bar;
  if (track >= kMixTracks) {
    return bar;
  }
  const ui::plot::MeterState& m = meter[audio::meter_ch::kTrack0 + track];
  const unsigned level = scale.position(m.rms()) >> 8;  // 0..65535 -> 0..255
  const unsigned peak = scale.position(m.hold()) >> 8;
  bar.level = static_cast<std::uint8_t>(level);
  bar.peak = static_cast<std::uint8_t>(peak > level ? peak : level);
  return bar;
}

bool MixBoard::poll() noexcept {
  if (meters == nullptr) {
    return false;
  }
  const std::uint32_t now = meters->windows();
  if (now == windows) {
    return false;  // the ballistics run once a window (32 ms): the bars fall at the rate the plot library tunes them for
  }
  audio::MeterSnapshot snap;
  if (!meters->snapshot(snap)) {
    return false;  // torn: the next pass reads again (windows is not advanced)
  }
  windows = now;
  bool moved = false;
  for (unsigned c = 0; c < audio::kMeterChannels; ++c) {
    const audio::MeterLevel level = audio::meter_level(snap, c);
    const ui::plot::MeterState before = meter[c];
    meter[c].update(ui::plot::MeterReading{level.peak, level.rms});
    moved = moved || !same(before, meter[c]);
  }
  return moved;
}

}  // namespace o3os::app
