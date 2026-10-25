// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where the plots' bars, holds, lamps, scale marks and scope trails land (ui/plot/meters.h, scope.h), in the simulator's pixels:
// host only. The pure parts (the dB LUT, ballistics, decimation, word counts) are plot_meters_test.cpp, which also runs on QEMU.
#include <doctest/doctest.h>

#include <o3os/eve/display_list_builder.h>
#include <o3os/eve/dl.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/sim/eve_raster.h>
#include <o3os/sim/raster_display.h>
#include <o3os/ui/plot/meters.h>
#include <o3os/ui/plot/plot_math.h>
#include <o3os/ui/plot/scope.h>
#include <o3os/ui/renderer.h>
#include <o3os/ui/theme.h>

#include <cmath>
#include <cstdint>
#include <vector>

namespace dl = o3os::eve::dl;
using namespace o3os;
using namespace o3os::ui::plot;
using ui::Axis;
using ui::Color;
using ui::Rect;

namespace {

const ui::Theme& amiga() { return ui::theme(ui::ThemeId::Amiga); }

struct Panel {
  sim::EveRaster raster;
  sim::RasterDisplay display{raster};
  Panel() { CHECK(sim::load_assets(display) == hal::UploadResult::Ok); }
  template <typename Fn>
  bool draw(Fn fn) {
    std::vector<std::uint32_t> storage(eve::kMaxDisplayListWords);
    eve::DisplayListBuilder builder(display, storage.data(), eve::kMaxDisplayListWords);
    ui::Renderer r(builder, amiga());
    CHECK(r.begin(Color::Pane));
    fn(r);
    const bool ok = r.end();
    return ok && display.render_failures() == 0;
  }
  std::uint32_t pixel(int x, int y) const {
    const std::uint8_t* p = raster.rgba() + (static_cast<std::size_t>(y) * 800 + static_cast<std::size_t>(x)) * 4;
    return (std::uint32_t{p[0]} << 16) | (std::uint32_t{p[1]} << 8) | p[2];
  }
};

std::uint32_t rgb_of(Color c) {
  const ui::Rgb& v = amiga().rgb(c);
  return (std::uint32_t{v.r} << 16) | (std::uint32_t{v.g} << 8) | v.b;
}

}  // namespace

TEST_CASE("plot meters: a vertical bar is a frame, a slot, the RMS body from the bottom, the peak above it, a hold tick and a lamp") {
  DbScale scale;
  scale.init(-60.0f, 0.0f);
  MeterState m;
  m.update({amp_from_db(-6.0f), amp_from_db(-20.0f)});
  const Rect bar{100, 50, 20, 200};  // inside: x 101..118, y 51..248 (198 rows)
  MeterStyle style;
  Panel p;
  CHECK(p.draw([&](ui::Renderer& r) { draw_meters(r, &bar, &m, 1, Axis::Vertical, scale, style); }));
  const int in_bottom = 248;  // the last row inside the frame
  const auto row_of_db = [&](float db) {
    return in_bottom - static_cast<int>((static_cast<std::uint32_t>(scale.position_db(db)) * 198u + 32768u) / 65535u) + 1;
  };
  CHECK(p.pixel(100, 100) == rgb_of(Color::Empty));       // the frame
  CHECK(p.pixel(119, 100) == rgb_of(Color::Empty));
  CHECK(p.pixel(110, 50) == rgb_of(Color::Empty));
  CHECK(p.pixel(110, 249) == rgb_of(Color::Empty));
  CHECK(p.pixel(110, 60) == rgb_of(Color::Pane));         // the empty slot above the bar
  // the RMS body is the smoothed RMS (70 % of the way to the reading on the first update), from the bottom
  const int rms_top = in_bottom - static_cast<int>((static_cast<std::uint32_t>(scale.position(m.rms())) * 198u + 32768u) / 65535u) + 1;
  CHECK(p.pixel(110, in_bottom) == rgb_of(Color::Play));
  CHECK(p.pixel(110, rms_top + 2) == rgb_of(Color::Play));
  // the peak above the body: the middle half of the bar's width (x 105 .. 114 of the inside 101 .. 118), the same green
  CHECK(p.pixel(110, rms_top - 2) == rgb_of(Color::Play));
  CHECK(p.pixel(103, rms_top - 2) == rgb_of(Color::Pane));  // the sides of the bar above the body are the slot
  CHECK(p.pixel(116, rms_top - 2) == rgb_of(Color::Pane));
  CHECK(p.pixel(103, rms_top + 2) == rgb_of(Color::Play));  // and the body is the full width
  CHECK(p.pixel(110, row_of_db(-6.0f) + 3) == rgb_of(Color::Play));
  CHECK(p.pixel(110, row_of_db(-6.0f) - 4) == rgb_of(Color::Pane));  // and nothing above the hold
  // the hold tick: two Text pixels at the hold level, which is the peak here
  unsigned tick = 0;
  for (int y = row_of_db(-6.0f) - 3; y <= row_of_db(-6.0f) + 3; ++y) {
    tick += p.pixel(110, y) == rgb_of(Color::Text) ? 1u : 0u;
  }
  CHECK(tick == 2);
  // no lamp: the top rows of the bar are the slot
  CHECK(p.pixel(110, 51) == rgb_of(Color::Pane));
  // a clip lights the top 3 rows of the inside
  m.update({1.0f, amp_from_db(-20.0f)});
  CHECK(p.draw([&](ui::Renderer& r) { draw_meters(r, &bar, &m, 1, Axis::Vertical, scale, style); }));
  for (int y = 51; y < 54; ++y) {
    CHECK(p.pixel(110, y) == rgb_of(Color::Accent));  // the lamp is the one orange
  }
  CHECK(p.pixel(110, 54) != rgb_of(Color::Accent));
  // silence draws only the frame and the slot
  MeterState quiet;
  CHECK(p.draw([&](ui::Renderer& r) { draw_meters(r, &bar, &quiet, 1, Axis::Vertical, scale, style); }));
  for (int y = 51; y < 249; y += 7) {
    CHECK(p.pixel(110, y) == rgb_of(Color::Pane));
  }
}


TEST_CASE("plot meters: a horizontal bar runs from the left") {
  DbScale scale;
  scale.init(-60.0f, 0.0f);
  MeterState m;
  m.update({amp_from_db(-3.0f), amp_from_db(-12.0f)});
  const Rect bar{200, 100, 202, 16};  // inside: x 201..400
  Panel p;
  CHECK(p.draw([&](ui::Renderer& r) { draw_meters(r, &bar, &m, 1, Axis::Horizontal, scale, MeterStyle{}); }));
  CHECK(p.pixel(201, 108) == rgb_of(Color::Play));                         // the body starts at the left
  const int x_rms = 201 + static_cast<int>((static_cast<std::uint32_t>(scale.position(m.rms())) * 200u + 32768u) / 65535u);
  CHECK(p.pixel(x_rms - 3, 108) == rgb_of(Color::Play));
  CHECK(p.pixel(x_rms + 3, 108) == rgb_of(Color::Play));  // the peak, in the middle half of the bar's height
  CHECK(p.pixel(x_rms + 3, 102) == rgb_of(Color::Pane));   // not in its top rows: the body's top is 100 + 1, the peak's 104
  CHECK(p.pixel(x_rms - 3, 102) == rgb_of(Color::Play));
  CHECK(p.pixel(399, 108) == rgb_of(Color::Pane));
  CHECK(p.pixel(200, 108) == rgb_of(Color::Empty));
}


TEST_CASE("plot meters: the scale's marks line up with the bar's levels") {
  DbScale scale;
  scale.init(-60.0f, 0.0f);
  const Rect bar{100, 50, 20, 200};
  const float marks[] = {0.0f, -6.0f, -24.0f};
  MeterStyle style;
  Panel p;
  CHECK(p.draw([&](ui::Renderer& r) { draw_meter_scale(r, bar, Axis::Vertical, scale, marks, 3, style); }));
  // the 3-pixel rules lie left of the bar at the row of the level, in the mark colour
  for (const float db : marks) {
    const int len = static_cast<int>((static_cast<std::uint32_t>(scale.position_db(db)) * 198u + 32768u) / 65535u);
    const int y = 51 + 198 - 1 - (len >= 198 ? 197 : len);
    CHECK_MESSAGE(p.pixel(bar.x - 3, y) == rgb_of(Color::Empty), "mark " << db << " at row " << y);
    CHECK(p.pixel(bar.x - 3, y + 3) == rgb_of(Color::Pane));
  }
  unsigned labels = 0;
  for (int y = 40; y < 260; ++y) {
    for (int x = 60; x < 94; ++x) {
      labels += p.pixel(x, y) == rgb_of(Color::TextDim) ? 1u : 0u;
    }
  }
  CHECK(labels > 30u);
  // a horizontal scale puts them below the bar
  const Rect hbar{200, 300, 202, 16};
  CHECK(p.draw([&](ui::Renderer& r) { draw_meter_scale(r, hbar, Axis::Horizontal, scale, marks, 3, style); }));
  const int x0 = 201 + 199;  // 0 dB: the last column of the inside
  CHECK(p.pixel(x0, hbar.y + hbar.h + 2) == rgb_of(Color::Empty));
  CHECK(p.pixel(x0 - 12, hbar.y + hbar.h + 2) == rgb_of(Color::Pane));
}

// ---- the scope ---------------------------------------------------------------------------------------------------------


TEST_CASE("plot scope: drawing stays inside its square, and a mono signal in mid / side is a vertical line") {
  Scope s;
  std::vector<std::int16_t> lr(2 * 2048);
  for (unsigned i = 0; i < 2048; ++i) {
    lr[2 * i] = static_cast<std::int16_t>(32767 * std::sin(i * 0.05));
    lr[2 * i + 1] = static_cast<std::int16_t>(32767 * std::sin(i * 0.031));
  }
  for (int g = 0; g < 6; ++g) {
    s.push(lr.data(), 2048, ScopeMode::LeftRight);
  }
  const Rect area{100, 40, 300, 400};  // not square: the picture is the 300 px square, centred
  // nothing outside the square: the area is 300 wide and 400 tall, the square 300 x 300 from y = 90
  Panel p;
  CHECK(p.draw([&](ui::Renderer& r) { s.draw(r, area, ScopeStyle{}); }));
  for (int x = 90; x < 410; x += 5) {
    CHECK(p.pixel(x, 85) == rgb_of(Color::Pane));
    CHECK(p.pixel(x, 395) == rgb_of(Color::Pane));
  }
  // a mono signal in mid / side draws on the vertical axis only
  Scope mono;
  for (unsigned i = 0; i < 2048; ++i) {
    lr[2 * i + 1] = lr[2 * i];
  }
  mono.push(lr.data(), 2048, ScopeMode::MidSide);
  ScopeStyle dots;
  dots.glow = false;
  Panel q;
  CHECK(q.draw([&](ui::Renderer& r) { mono.draw(r, Rect{100, 40, 300, 300}, dots); }));
  const int centre = 100 + 150;
  unsigned off_axis = 0, on_axis = 0;
  for (int y = 41; y < 339; ++y) {
    for (int x = 101; x < 399; ++x) {
      const std::uint32_t px = q.pixel(x, y);
      if (px != rgb_of(Color::Pane) && px != rgb_of(Color::Empty) && x != 100 && x != 399) {
        (x >= centre - 3 && x <= centre + 3) ? ++on_axis : ++off_axis;
      }
    }
  }
  CHECK(on_axis > 20u);
  // the ring (Empty) is the only thing off the axis, and its antialiased edge pixels are not the pane's colour: they show
  // up in off_axis as blends of the ring colour, so count only pixels that are the trace's own green
  unsigned green_off = 0;
  for (int y = 41; y < 339; ++y) {
    for (int x = 101; x < 399; ++x) {
      const std::uint32_t px = q.pixel(x, y);
      const unsigned g = (px >> 8) & 255, rr = (px >> 16) & 255, bl = px & 255;
      if ((x < centre - 3 || x > centre + 3) && g > rr + 60 && g > bl + 60) {
        ++green_off;
      }
    }
  }
  CHECK(green_off == 0u);
  (void)off_axis;
}
