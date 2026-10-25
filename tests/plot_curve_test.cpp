// SPDX-License-Identifier: GPL-3.0-or-later
//
// The curve / XY plot (ui/plot/curve.h), the parts that need no rasteriser (so that they also run on the Cortex-M4F, ctest
// qemu_plot): the polyline reduction and the word bounds of a series. The pixels of its fill, line, axes and handles, and the
// renderer's raw sections, are tests/plot_curve_pixels_test.cpp.
#include <doctest/doctest.h>

#include <o3os/eve/display_list_builder.h>
#include <o3os/eve/dl.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/ui/plot/curve.h>
#include <o3os/ui/renderer.h>
#include <o3os/ui/theme.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace dl = o3os::eve::dl;
using namespace o3os;
using namespace o3os::ui::plot;
using ui::Color;
using ui::Rect;

namespace {

const ui::Theme& amiga() { return ui::theme(ui::ThemeId::Amiga); }

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

struct Rng {
  std::uint32_t s = 2463534242u;
  float unit() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return static_cast<float>(s >> 8) / 16777216.0f;
  }
};

// Distance of point p from the chord a-b, in the units of the points.
double chord_distance(const std::vector<std::int16_t>& x, const std::vector<std::int16_t>& y, unsigned a, unsigned b, unsigned p) {
  const double dx = x[b] - x[a], dy = y[b] - y[a], ex = x[p] - x[a], ey = y[p] - y[a];
  const double len = std::sqrt(dx * dx + dy * dy);
  return len < 0.5 ? std::sqrt(ex * ex + ey * ey) : std::fabs(dx * ey - dy * ex) / len;
}

}  // namespace

TEST_CASE("plot curve: simplify_polyline keeps the ends, drops collinear points, keeps corners") {
  std::vector<std::int16_t> x, y;
  std::uint16_t keep[kCurveMaxVertices];
  for (int i = 0; i < 100; ++i) {  // a straight line
    x.push_back(static_cast<std::int16_t>(i * 16));
    y.push_back(static_cast<std::int16_t>(i * 8 + 100));
  }
  unsigned n = simplify_polyline(x.data(), y.data(), 100, 64, 4.0f, keep);
  CHECK(n == 2);
  CHECK((keep[0] == 0 && keep[1] == 99));
  // a V: the apex is kept
  x.clear();
  y.clear();
  for (int i = 0; i < 101; ++i) {
    x.push_back(static_cast<std::int16_t>(i * 16));
    y.push_back(static_cast<std::int16_t>(i <= 50 ? i * 16 : (100 - i) * 16));
  }
  n = simplify_polyline(x.data(), y.data(), 101, 64, 4.0f, keep);
  CHECK(n == 3);
  CHECK((keep[0] == 0 && keep[1] == 50 && keep[2] == 100));
  // degenerate inputs
  CHECK(simplify_polyline(x.data(), y.data(), 0, 8, 1.0f, keep) == 0);
  CHECK(simplify_polyline(x.data(), y.data(), 1, 8, 1.0f, keep) == 1);
  CHECK(simplify_polyline(x.data(), y.data(), 2, 8, 1.0f, keep) == 2);
  CHECK((keep[0] == 0 && keep[1] == 1));
  CHECK(simplify_polyline(x.data(), y.data(), 101, 1, 1.0f, keep) == 1);  // fewer than two kept: the first alone
  // a closed polyline (first point == last): the farthest point is chosen by distance from that point
  std::vector<std::int16_t> cx = {0, 160, 160, 0, 0}, cy = {0, 0, 160, 160, 0};
  n = simplify_polyline(cx.data(), cy.data(), 5, 8, 1.0f, keep);
  CHECK(n >= 3);
}


TEST_CASE("plot curve: simplify_polyline meets its budget with the points that matter, and its tolerance") {
  // a smooth curve of 600 points
  std::vector<std::int16_t> x, y;
  for (int i = 0; i < 600; ++i) {
    x.push_back(static_cast<std::int16_t>(i * 12));
    y.push_back(static_cast<std::int16_t>(2000.0 * std::sin(i * 0.02) + 800.0 * std::sin(i * 0.071)));
  }
  std::uint16_t keep[kCurveMaxVertices];
  float last_worst = 1e9f;
  for (const unsigned budget : {4u, 8u, 16u, 32u, 64u, 128u, 192u}) {
    const unsigned n = simplify_polyline(x.data(), y.data(), 600, budget, 0.0f, keep);
    CHECK(n == budget);  // tolerance 0: the whole budget is used
    CHECK(keep[0] == 0);
    CHECK(keep[n - 1] == 599);
    for (unsigned i = 1; i < n; ++i) {
      CHECK(keep[i] > keep[i - 1]);
    }
    // the worst dropped point's distance from its chord: it shrinks as the budget grows (largest error first)
    double worst = 0.0;
    for (unsigned i = 1; i < n; ++i) {
      for (unsigned p = keep[i - 1] + 1u; p < keep[i]; ++p) {
        const double d = chord_distance(x, y, keep[i - 1], keep[i], p);
        worst = d > worst ? d : worst;
      }
    }
    CHECK(static_cast<float>(worst) <= last_worst);
    last_worst = static_cast<float>(worst);
  }
  CHECK(last_worst < 6.0f);  // with 192 of 600 points the curve is within 6/16 px
  // a tolerance stops early: every dropped point within it, and fewer points than the budget
  const unsigned n = simplify_polyline(x.data(), y.data(), 600, 192, 16.0f, keep);
  CHECK(n < 192);
  CHECK(n > 8);
  for (unsigned i = 1; i < n; ++i) {
    for (unsigned p = keep[i - 1] + 1u; p < keep[i]; ++p) {
      CHECK(chord_distance(x, y, keep[i - 1], keep[i], p) <= 16.0 + 1e-3);
    }
  }
  // spikes in noise are kept before the flat parts (a single tall point is the first thing the reduction keeps)
  std::vector<std::int16_t> sx, sy;
  for (int i = 0; i < 300; ++i) {
    sx.push_back(static_cast<std::int16_t>(i * 20));
    sy.push_back(static_cast<std::int16_t>(i == 177 ? 3000 : 0));
  }
  CHECK(simplify_polyline(sx.data(), sy.data(), 300, 3, 0.0f, keep) == 3);
  CHECK(keep[1] == 177);
}


TEST_CASE("plot curve: a series is bounded in words whatever its size, noise and length") {
  const AxisSpec xa{AxisKind::Linear, 0.0f, 1.0f}, ya{AxisKind::Linear, -1.0f, 1.0f};
  const Rect area{56, 40, 704, 320};
  auto scratch = std::make_unique<CurveScratch>();
  std::vector<float> v;
  Rng rng;
  for (const std::uint32_t n : {2u, 3u, 50u, 704u, 705u, 1024u, 1025u, 5000u, 100000u}) {
    v.assign(n, 0.0f);
    for (std::uint32_t i = 0; i < n; ++i) {
      v[i] = rng.unit() * 2.0f - 1.0f;  // noise: nothing is collinear
    }
    for (const unsigned max_v : {2u, 16u, 128u, 192u, 500u}) {  // 500 is clamped to kCurveMaxVertices
      CurveStyle style;
      style.max_vertices = static_cast<std::uint16_t>(max_v);
      style.tolerance_px = 0.0f;
      CurveSeries s;
      s.y = v.data();
      s.n = n;
      unsigned drawn = 0;
      const std::size_t words = words_of([&](ui::Renderer& r) { drawn = draw_series(r, area, xa, ya, s, true, style, *scratch); });
      const unsigned limit = max_v > kCurveMaxVertices ? kCurveMaxVertices : max_v;
      CHECK_MESSAGE(drawn <= limit, "n " << n << " max " << max_v << " drew " << drawn);
      CHECK_MESSAGE(words <= curve_series_max_words(limit), "n " << n << " max " << max_v << ": " << words << " words");
      CHECK(drawn >= 2);
    }
  }
  CHECK(curve_series_max_words(kCurveMaxVertices) == 2 * kCurveMaxVertices + 16);
}

