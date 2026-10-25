// SPDX-License-Identifier: GPL-3.0-or-later
//
// The curve / XY plot (ui/plot/curve.h) in the simulator's pixels (host only): the series' fill and line, the axes and grid, the
// handles and the raw-section contract of ui::Renderer (what a plot leaves behind it). The polyline reduction and the word bounds
// are plot_curve_test.cpp and plot_bounds_test.cpp, which also run on QEMU.
#include <doctest/doctest.h>

#include <o3os/eve/display_list_builder.h>
#include <o3os/eve/dl.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/sim/eve_raster.h>
#include <o3os/sim/raster_display.h>
#include <o3os/ui/plot/curve.h>
#include <o3os/ui/renderer.h>
#include <o3os/ui/theme.h>

#include <csignal>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace dl = o3os::eve::dl;
using namespace o3os;
using namespace o3os::ui::plot;
using ui::Color;
using ui::Rect;

namespace {

const ui::Theme& amiga() { return ui::theme(ui::ThemeId::Amiga); }

struct Panel {
  sim::EveRaster raster;
  sim::RasterDisplay display{raster};
  Panel() { CHECK(sim::load_assets(display) == hal::UploadResult::Ok); }
  template <typename Fn>
  bool draw(Fn fn, const ui::Theme& theme = amiga()) {
    std::vector<std::uint32_t> storage(eve::kMaxDisplayListWords);
    eve::DisplayListBuilder builder(display, storage.data(), eve::kMaxDisplayListWords);
    ui::Renderer r(builder, theme);
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

template <typename Fn>
std::size_t words_of(Fn fn) {
  platform::host::FakeDisplay sink;
  std::vector<std::uint32_t> storage(eve::kMaxDisplayListWords * 4);
  eve::DisplayListBuilder builder(sink, storage.data(), storage.size());
  ui::Renderer r(builder, amiga());
  CHECK(r.begin(Color::Pane));
  const std::size_t before = r.words();
  fn(r);
  const std::size_t after = r.words();
  CHECK(r.end());
  return after - before;
}

}  // namespace

TEST_CASE("plot curve: a series is a line and a translucent fill, at the pixels the axes say") {
  const AxisSpec xa{AxisKind::Linear, 0.0f, 1.0f}, ya{AxisKind::Linear, 0.0f, 1.0f};
  const Rect area{100, 60, 300, 201};  // an odd height: the middle of the data is the centre of a pixel row
  float v[64];
  for (float& f : v) {
    f = 0.5f;  // a horizontal line at the middle
  }
  CurveSeries s;
  s.y = v;
  s.n = 64;
  CurveStyle style;
  auto scratch = std::make_unique<CurveScratch>();
  Panel p;
  unsigned vertices = 0;
  CHECK(p.draw([&](ui::Renderer& r) { vertices = draw_series(r, area, xa, ya, s, true, style, *scratch); }));
  CHECK(vertices == 2);  // collinear: the ends
  // the line is on row area.y + round(0.5 * 199) = 60 + 100 = 160 (centre of the pixel row), 3 px thick with width 12/16 x2
  const int row = area.y + static_cast<int>(0.5f * 199.0f + 0.5f);
  CHECK(p.pixel(250, row) == rgb_of(Color::Instr));
  CHECK(p.pixel(250, row - 3) == rgb_of(Color::Pane));  // above the line: untouched
  // below: the fill, Instr at alpha 0x40 over Pane
  const std::uint32_t below = p.pixel(250, row + 20);
  CHECK(below != rgb_of(Color::Pane));
  CHECK(below != rgb_of(Color::Instr));
  const ui::Rgb pane = amiga().rgb(Color::Pane), instr = amiga().rgb(Color::Instr);
  const auto mix = [&](int a, int b) { return static_cast<std::uint32_t>((a * 64 + b * (255 - 64) + 127) / 255); };
  (void)mix;
  const int dr = static_cast<int>((below >> 16) & 255) - pane.r, dg = static_cast<int>((below >> 8) & 255) - pane.g;
  CHECK(dr > 0);
  CHECK(dr < (instr.r - pane.r) / 2);  // well short of the line's colour: about a quarter
  CHECK(dg < (instr.g - pane.g) / 2);
  CHECK(p.pixel(250, area.y + area.h - 1) == below);  // the fill runs to the bottom of the area
  CHECK(p.pixel(250, area.y + area.h) == rgb_of(Color::Pane));  // and not beyond it
  CHECK(p.pixel(area.x - 1, row + 20) == rgb_of(Color::Pane));  // nor left of it
  CHECK(p.pixel(area.x + area.w, row + 20) == rgb_of(Color::Pane));
  // without a fill, only the line
  CHECK(p.draw([&](ui::Renderer& r) { draw_series(r, area, xa, ya, s, false, style, *scratch); }));
  CHECK(p.pixel(250, row + 20) == rgb_of(Color::Pane));
  CHECK(p.pixel(250, row) == rgb_of(Color::Instr));
}


TEST_CASE("plot curve: a series reaches the last column of a plot at the right edge of the panel") {
  // The mapped points are clamped to what VERTEX2F holds (1000 px); 800 px panels must not lose their last columns to it.
  const AxisSpec lin{AxisKind::Linear, 0.0f, 1.0f};
  const Rect area{56, 40, 744, 201};  // x 56 .. 799
  float v[300];
  for (float& f : v) {
    f = 0.5f;
  }
  CurveSeries s;
  s.y = v;
  s.n = 300;
  auto scratch = std::make_unique<CurveScratch>();
  CurveStyle style;
  Panel p;
  CHECK(p.draw([&](ui::Renderer& r) { draw_series(r, area, lin, lin, s, true, style, *scratch); }));
  const int row = area.y + 100;
  CHECK(p.pixel(56, row) == rgb_of(Color::Instr));
  CHECK(p.pixel(799, row) == rgb_of(Color::Instr));
  CHECK(p.pixel(799, row + 30) != rgb_of(Color::Pane));  // and the fill reaches it too
  // the same on a long series that is reduced to a point a column
  std::vector<float> many(5000, 0.5f);
  s.y = many.data();
  s.n = 5000;
  CHECK(p.draw([&](ui::Renderer& r) { draw_series(r, area, lin, lin, s, true, style, *scratch); }));
  CHECK(p.pixel(799, row) == rgb_of(Color::Instr));
}


TEST_CASE("plot curve: explicit x, Q15 values, a log axis and the edges of the plot") {
  const AxisSpec hz{AxisKind::LogFreq, 20.0f, 20000.0f}, db{AxisKind::Db, -24.0f, 24.0f};
  const Rect area{60, 40, 640, 300};
  // x: 20, 200, 2000, 20000 at db -24, 0, 24, 0: first point at the bottom-left corner, last on the 0 dB row
  const float xs[4] = {20.0f, 200.0f, 2000.0f, 20000.0f};
  const float ys[4] = {-24.0f, 0.0f, 24.0f, 0.0f};
  CurveSeries s;
  s.x = xs;
  s.y = ys;
  s.n = 4;
  CurveStyle style;
  style.tolerance_px = 0.0f;
  auto scratch = std::make_unique<CurveScratch>();
  Panel p;
  // (the 4 points are almost collinear on the first leg: at least 3 vertices)
  CHECK(p.draw([&](ui::Renderer& r) { CHECK(draw_series(r, area, hz, db, s, false, style, *scratch) >= 3); }));
  // the corner pixel and the top point (x = 2000 Hz is two thirds along)
  CHECK(p.pixel(area.x, area.y + area.h - 1) != rgb_of(Color::Pane));
  const int x2k = area.x + static_cast<int>(2.0f / 3.0f * 639.0f + 0.5f);
  CHECK(p.pixel(x2k, area.y) != rgb_of(Color::Pane));
  CHECK(p.pixel(x2k, area.y + 20) == rgb_of(Color::Pane));  // the peak is a spike: nothing under its tip
  // Q15 values: 0.5 and -0.5 give the same picture as floats 0.5 and -0.5
  const std::int16_t q[4] = {-16384, 16384, 0, 16384};
  const float f[4] = {-0.5f, 0.5f, 0.0f, 0.5f};
  const AxisSpec lin{AxisKind::Linear, -1.0f, 1.0f};
  CurveSeries sq, sf;
  sq.y_q15 = q;
  sq.n = 4;
  sf.y = f;
  sf.n = 4;
  const auto render = [&](const CurveSeries& series) {
    Panel pp;
    CHECK(pp.draw([&](ui::Renderer& r) { draw_series(r, area, lin, lin, series, true, style, *scratch); }));
    std::vector<std::uint32_t> img;
    for (int y = area.y; y < area.y + area.h; y += 3) {
      for (int x = area.x; x < area.x + area.w; x += 3) {
        img.push_back(pp.pixel(x, y));
      }
    }
    return img;
  };
  CHECK(render(sq) == render(sf));
  // nothing to draw: no data, one point, a collapsed area, no values
  CurveSeries none;
  CHECK(words_of([&](ui::Renderer& r) { CHECK(draw_series(r, area, lin, lin, none, true, style, *scratch) == 0); }) == 0);
  none.n = 1;
  none.y = f;
  CHECK(words_of([&](ui::Renderer& r) { CHECK(draw_series(r, area, lin, lin, none, true, style, *scratch) == 0); }) == 0);
  CHECK(words_of([&](ui::Renderer& r) { CHECK(draw_series(r, Rect{0, 0, 1, 50}, lin, lin, sf, true, style, *scratch) == 0); }) == 0);
  CurveSeries no_values;
  no_values.n = 8;
  CHECK(words_of([&](ui::Renderer& r) { CHECK(draw_series(r, area, lin, lin, no_values, true, style, *scratch) == 0); }) == 0);
}


TEST_CASE("plot curve: axes draw a frame, a grid at the ticks, a 0 dB reference and labels, in bounded words") {
  const AxisSpec hz{AxisKind::LogFreq, 20.0f, 20000.0f}, db{AxisKind::Db, -24.0f, 24.0f};
  const Rect area{80, 40, 600, 300};
  CurveStyle style;
  Panel p;
  CHECK(p.draw([&](ui::Renderer& r) { draw_axes(r, area, hz, db, style); }));
  // the frame is a pixel outside the area
  CHECK(p.pixel(area.x - 1, area.y + 100) == rgb_of(Color::Empty));
  CHECK(p.pixel(area.x + area.w, area.y + 100) == rgb_of(Color::Empty));
  CHECK(p.pixel(area.x + 100, area.y - 1) == rgb_of(Color::Empty));
  CHECK(p.pixel(area.x + 100, area.y + area.h) == rgb_of(Color::Empty));
  // a vertical grid line at 1 kHz: two thirds along, on pixel column area.x + round(2/3 * 599)
  const int x1k = area.x + static_cast<int>(2.0f / 3.0f * 599.0f + 0.5f);
  CHECK(p.pixel(x1k, area.y + 7) == rgb_of(Color::Empty));
  CHECK(p.pixel(x1k + 5, area.y + 7) == rgb_of(Color::Pane));
  // the 0 dB line is the reference colour, on the middle row; other dB lines are grid colour
  const int y0 = area.y + static_cast<int>(0.5f * 299.0f + 0.5f);
  CHECK(p.pixel(area.x + 5, y0) == rgb_of(Color::TextDim));
  const int y12 = area.y + static_cast<int>(0.25f * 299.0f + 0.5f);  // +12 dB
  CHECK(p.pixel(area.x + 5, y12) == rgb_of(Color::Empty));
  // labels: glyphs of the Micro face in TextDim under the frame (x labels) and left of it (y labels)
  unsigned under = 0, left = 0;
  for (int y = area.y + area.h + 2; y < area.y + area.h + 16; ++y) {
    for (int x = area.x - 10; x < area.x + area.w + 10; ++x) {
      under += p.pixel(x, y) == rgb_of(Color::TextDim) ? 1u : 0u;
    }
  }
  for (int y = area.y - 8; y < area.y + area.h + 8; ++y) {
    for (int x = area.x - 30; x < area.x - 2; ++x) {
      left += p.pixel(x, y) == rgb_of(Color::TextDim) ? 1u : 0u;
    }
  }
  CHECK(under > 40u);
  CHECK(left > 40u);
  // bounded
  const std::size_t words = words_of([&](ui::Renderer& r) { draw_axes(r, area, hz, db, style); });
  CHECK(words <= kCurveAxesMaxWords);
  const std::size_t dense = words_of([&](ui::Renderer& r) {
    draw_axes(r, Rect{56, 20, 720, 400}, AxisSpec{AxisKind::Linear, 0.0f, 1.0f}, AxisSpec{AxisKind::Linear, -1.0f, 1.0f}, style);
  });
  CHECK(dense <= kCurveAxesMaxWords);
  CHECK(words_of([&](ui::Renderer& r) { draw_axes(r, Rect{0, 0, 1, 1}, hz, db, style); }) == 0);  // nothing fits
}


TEST_CASE("plot curve: a tick label wider than kCurveLabelChars is not drawn; one that fits is") {
  const Rect area{80, 40, 300, 260};
  CurveStyle style;
  const auto label_pixels = [&](float lo, float hi) {
    Panel p;
    CHECK(p.draw([&](ui::Renderer& r) {
      draw_axes(r, area, AxisSpec{AxisKind::Linear, lo, hi}, AxisSpec{AxisKind::Linear, lo, hi}, style);
    }));
    unsigned n = 0;
    for (int y = area.y + area.h + 2; y < area.y + area.h + 18; ++y) {  // under the frame: the x labels
      for (int x = area.x - 20; x < area.x + area.w + 20; ++x) {
        n += p.pixel(x, y) == rgb_of(Color::TextDim) ? 1u : 0u;
      }
    }
    for (int y = area.y - 8; y < area.y + area.h + 8; ++y) {  // left of it: the y labels
      for (int x = area.x - 50; x < area.x - 2; ++x) {
        n += p.pixel(x, y) == rgb_of(Color::TextDim) ? 1u : 0u;
      }
    }
    return n;
  };
  CHECK(label_pixels(1000.0f, 5000.0f) > 100u);     // "1000" .. "5000"
  CHECK(label_pixels(10000.0f, 50000.0f) > 100u);   // five characters
  CHECK(label_pixels(1.0e6f, 5.0e6f) == 0u);        // seven (clamped to eight): none
}


TEST_CASE("plot curve: handles are discs in a ring, the active one in the accent and a pixel bigger, clipped to the plot's margin") {
  const AxisSpec xa{AxisKind::Linear, 0.0f, 1.0f}, ya{AxisKind::Linear, 0.0f, 1.0f};
  const Rect area{100, 60, 400, 200};
  CurveStyle style;
  const CurveHandle hs[] = {{0.25f, 0.5f, false}, {0.75f, 0.5f, true}, {1.0f, 0.0f, false}};
  Panel p;
  CHECK(p.draw([&](ui::Renderer& r) {
    r.panel(area, Color::Chrome);  // a background the ring must be seen against
    draw_handles(r, area, xa, ya, hs, 3, style);
  }));
  const int cx0 = area.x + static_cast<int>(0.25f * 399.0f + 0.5f), cy = area.y + static_cast<int>(0.5f * 199.0f + 0.5f);
  const int cx1 = area.x + static_cast<int>(0.75f * 399.0f + 0.5f);
  CHECK(p.pixel(cx0, cy) == rgb_of(Color::Text));          // the disc
  CHECK(p.pixel(cx1, cy) == rgb_of(Color::Accent));        // the active one
  CHECK(p.pixel(cx0 + 6, cy) == rgb_of(Color::Pane));      // the ring: radius 5 + 2
  CHECK(p.pixel(cx0 + 10, cy) == rgb_of(Color::Chrome));   // and nothing beyond it
  // the one on the corner is whole: the scissor is grown by the radius
  const int ex = area.x + area.w - 1, ey = area.y + area.h - 1;
  CHECK(p.pixel(ex, ey) == rgb_of(Color::Text));
  CHECK(p.pixel(ex + 4, ey + 4) != rgb_of(Color::Chrome));  // part of the disc lies outside the area (inside the grown scissor)
  // at most kCurveMaxHandles are drawn; none for none
  std::vector<CurveHandle> many(40, CurveHandle{0.5f, 0.5f, false});
  const std::size_t words = words_of([&](ui::Renderer& r) { draw_handles(r, area, xa, ya, many.data(), 40, style); });
  CHECK(words <= kCurveHandlesMaxWords);
  CHECK(words_of([&](ui::Renderer& r) { draw_handles(r, area, xa, ya, many.data(), 0, style); }) == 0);
  CHECK(words_of([&](ui::Renderer& r) { draw_handles(r, area, xa, ya, nullptr, 3, style); }) == 0);
  CHECK(words_of([&](ui::Renderer& r) { draw_handles(r, Rect{0, 0, 1, 1}, xa, ya, hs, 3, style); }) == 0);
}


TEST_CASE("renderer raw section: two words round it, the renderer's colour, origin and handle caches survive it") {
  // The section is SAVE_CONTEXT ... RESTORE_CONTEXT: colour, alpha, scissor, line width and translation come back, and the
  // renderer forgets what it cached, so its next colour is emitted again.
  const std::size_t empty = words_of([](ui::Renderer& r) {
    r.begin_raw();
    r.end_raw();
  });
  CHECK(empty == 2);
  // with the origin moved by a tile at x >= 512, begin_raw also zeroes the translation (one more word), end_raw restores it
  const std::size_t moved = words_of([](ui::Renderer& r) {
    r.hex_tile(600, 10, 0x12, Color::Text);  // VERTEX_TRANSLATE_X moves
    const std::size_t before = r.words();
    r.begin_raw();
    r.end_raw();
    CHECK(r.words() - before == 4);  // END of the open bitmaps, SAVE_CONTEXT, VERTEX_TRANSLATE_X 0, RESTORE_CONTEXT
  });
  (void)moved;
  Panel p;
  CHECK(p.draw([&](ui::Renderer& r) {
    r.panel({10, 10, 40, 10}, Color::Chrome);
    r.begin_raw();
    r.raw(dl::color_a(0x40));
    r.panel({10, 30, 40, 10}, Color::Sel);  // translucent
    r.raw(r.color_word(Color::Fx));
    r.raw(dl::scissor_xy(0, 0));
    r.raw(dl::scissor_size(20, 20));
    r.end_raw();
    // after the section: alpha and scissor are back, and the colour is re-emitted (Chrome was the colour before it, and
    // Sel the renderer's last: a cache that survived would skip the word and draw in the section's last colour, Fx)
    r.panel({60, 10, 40, 10}, Color::Chrome);
    r.panel({60, 30, 40, 10}, Color::Sel);
    r.panel({200, 100, 10, 10}, Color::Chrome);  // outside the section's 20 x 20 scissor: it must draw
  }));
  CHECK(p.pixel(20, 15) == rgb_of(Color::Chrome));
  CHECK(p.pixel(20, 35) != rgb_of(Color::Sel));  // translucent
  CHECK(p.pixel(20, 35) != rgb_of(Color::Pane));
  CHECK(p.pixel(80, 15) == rgb_of(Color::Chrome));
  CHECK(p.pixel(80, 35) == rgb_of(Color::Sel));
  CHECK(p.pixel(205, 105) == rgb_of(Color::Chrome));
  // the origin: a tile at x = 700 (the renderer moves its origin), a plot, then another tile at x = 720: the same pixels as
  // two tiles with no plot between them
  const auto two_tiles = [&](bool with_plot) {
    Panel q;
    CHECK(q.draw([&](ui::Renderer& r) {
      r.hex_tile(700, 20, 0xA5, Color::Text);
      if (with_plot) {
        const AxisSpec a{AxisKind::Linear, 0.0f, 1.0f};
        float v[2] = {0.0f, 1.0f};
        CurveSeries s;
        s.y = v;
        s.n = 2;
        auto scratch = std::make_unique<CurveScratch>();
        CurveStyle style;
        draw_series(r, Rect{40, 100, 200, 100}, a, a, s, true, style, *scratch);
      }
      r.hex_tile(740, 20, 0x3C, Color::Text);
    }));
    std::vector<std::uint32_t> px;
    for (int y = 20; y < 40; ++y) {
      for (int x = 700; x < 760; ++x) {
        px.push_back(q.pixel(x, y));
      }
    }
    return px;
  };
  CHECK(two_tiles(true) == two_tiles(false));
  // and the plot itself is drawn where it should be after the origin moved (VERTEX_TRANSLATE_X zeroed inside the section)
  Panel q;
  CHECK(q.draw([&](ui::Renderer& r) {
    r.hex_tile(700, 20, 0xA5, Color::Text);
    const AxisSpec a{AxisKind::Linear, 0.0f, 1.0f};
    float v[2] = {0.5f, 0.5f};
    CurveSeries s;
    s.y = v;
    s.n = 2;
    auto scratch = std::make_unique<CurveScratch>();
    draw_series(r, Rect{40, 100, 200, 101}, a, a, s, false, CurveStyle{}, *scratch);
  }));
  CHECK(q.pixel(100, 150) == rgb_of(Color::Instr));
}


namespace {

// Runs `misuse` in a child process and reports whether it died of SIGABRT: the raw-section checks abort on a programming error.
template <typename Fn>
bool aborts(Fn misuse) {
  std::fflush(nullptr);
  const pid_t pid = fork();
  if (pid == 0) {
    std::signal(SIGABRT, SIG_DFL);  // not doctest's crash report: the child's death is the answer
    platform::host::FakeDisplay sink;
    std::vector<std::uint32_t> storage(eve::kMaxDisplayListWords);
    eve::DisplayListBuilder builder(sink, storage.data(), eve::kMaxDisplayListWords);
    ui::Renderer r(builder, amiga());
    r.begin(Color::Pane);
    misuse(r);
    _exit(0);  // did not abort
  }
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

}  // namespace

TEST_CASE("renderer raw section: in_raw() says so, and nesting, a raw word outside a section, a stray end or an open section at the end abort") {
  platform::host::FakeDisplay sink;
  std::vector<std::uint32_t> storage(eve::kMaxDisplayListWords);
  eve::DisplayListBuilder builder(sink, storage.data(), eve::kMaxDisplayListWords);
  ui::Renderer r(builder, amiga());
  CHECK(r.begin(Color::Pane));
  CHECK_FALSE(r.in_raw());
  r.begin_raw();
  CHECK(r.in_raw());
  r.raw(dl::color_a(0x80));
  r.panel({10, 10, 5, 5}, Color::Chrome);  // the renderer's own calls are fine inside
  CHECK(r.in_raw());
  r.end_raw();
  CHECK_FALSE(r.in_raw());
  CHECK(r.end());
  // a frame that begins again forgets a section the last one left open
  CHECK(r.begin(Color::Pane));
  r.begin_raw();
  CHECK(r.begin(Color::Pane));
  CHECK_FALSE(r.in_raw());
  r.begin_raw();
  r.end_raw();
  CHECK(r.end());
  // the misuses abort (checked in a child process)
  CHECK(aborts([](ui::Renderer& rr) {
    rr.begin_raw();
    rr.begin_raw();  // nested: the inner one would lose the origin the outer one saved
  }));
  CHECK(aborts([](ui::Renderer& rr) { rr.raw(dl::color_a(1)); }));  // outside a section
  CHECK(aborts([](ui::Renderer& rr) { rr.end_raw(); }));            // no section to end
  CHECK(aborts([](ui::Renderer& rr) {
    rr.begin_raw();
    rr.end();  // the frame ends inside a section
  }));
  CHECK_FALSE(aborts([](ui::Renderer& rr) {  // the proper use does not
    rr.begin_raw();
    rr.raw(dl::color_a(1));
    rr.end_raw();
    rr.end();
  }));
}
