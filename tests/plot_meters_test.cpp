// SPDX-License-Identifier: GPL-3.0-or-later
//
// Level meters (ui/plot/meters.h) and the XY scope (ui/plot/scope.h), the parts that need no rasteriser (so that they also run on
// the Cortex-M4F, ctest qemu_plot): the dB LUT against the logarithm, the ballistics of a meter, the colour passes of a bank, the
// scope's decimation and trails, and the word counts. Where bars, lamps and trails land is tests/plot_meters_pixels_test.cpp.
#include <doctest/doctest.h>

#include <o3os/eve/display_list_builder.h>
#include <o3os/eve/dl.h>
#include <o3os/platform/host/fakes.h>
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

struct Counted {
  std::size_t words = 0;
  std::size_t colour_words = 0;
};

template <typename Fn>
Counted counted(Fn fn) {
  platform::host::FakeDisplay sink;
  std::vector<std::uint32_t> storage(eve::kMaxDisplayListWords * 4);
  eve::DisplayListBuilder builder(sink, storage.data(), storage.size());
  ui::Renderer r(builder, amiga());
  CHECK(r.begin(Color::Pane));
  const std::size_t before = r.words();
  fn(r);
  Counted c;
  c.words = r.words() - before;
  CHECK(r.end());
  const auto& frame = sink.last_frame();
  for (std::size_t i = before + 35; i + 1 < frame.size() && i < before + 35 + c.words; ++i) {  // 35 words of preamble
    c.colour_words += (frame[i] >> 24) == eve::opcode::kColorRgb ? 1u : 0u;
  }
  return c;
}

}  // namespace

TEST_CASE("plot meters: the dB LUT follows 20 log10 within 0.27 dB, and clamps") {
  DbScale s;
  s.init(-60.0f, 0.0f);
  float worst = 0.0f;
  for (float db = -59.0f; db <= -0.4f; db += 0.037f) {
    const float amp = amp_from_db(db);
    const float got_db = -60.0f + static_cast<float>(s.position(amp)) * (60.0f / 65535.0f);
    const float err = std::fabs(got_db - db);
    worst = err > worst ? err : worst;
  }
  CHECK(worst < 0.27f);  // half the widest step of 16 an octave (0.53 dB, at the bottom of an octave)
  CHECK(worst > 0.2f);   // and it is that wide: the table is not finer than it says
  CHECK(s.position(0.0f) == 0);
  CHECK(s.position(-1.0f) == 0);
  CHECK(s.position(std::nanf("")) == 0);
  CHECK(s.position(amp_from_db(-70.0f)) == 0);   // below the floor
  CHECK(s.position(amp_from_db(-60.5f)) <= 40);  // at it: within the step
  CHECK(s.position(1.0f) == 65535);              // at the ceiling and above
  CHECK(s.position(1.5f) == 65535);
  CHECK(s.position(5.0f) == 65535);
  CHECK(s.position(1.0e-9f) == 0);
  CHECK(s.position(amp_from_db(-6.0f)) > s.position(amp_from_db(-12.0f)));
  // monotonic over the whole range
  std::uint16_t last = 0;
  for (float db = -90.0f; db <= 6.0f; db += 0.05f) {
    const std::uint16_t p = s.position(amp_from_db(db));
    CHECK(p >= last);
    last = p;
  }
  // other scales: -48 .. +6 puts 0 dB at 48/54 of the way; the dB form has no table
  DbScale w;
  w.init(-48.0f, 6.0f);
  CHECK(static_cast<float>(w.position_db(0.0f)) == doctest::Approx(65535.0f * 48.0f / 54.0f).epsilon(1e-3));
  CHECK(w.position_db(-48.0f) == 0);
  CHECK(w.position_db(-100.0f) == 0);
  CHECK(w.position_db(6.0f) == 65535);
  CHECK(w.position_db(60.0f) == 65535);
  CHECK(w.floor_db() == -48.0f);
  CHECK(w.ceil_db() == 6.0f);
  // a degenerate scale does not divide by zero
  DbScale d;
  d.init(-10.0f, -10.0f);
  CHECK(d.ceil_db() > d.floor_db());
}


TEST_CASE("plot meters: the ballistics: a peak falls, the RMS follows, the hold holds then falls, the lamp lights and goes out") {
  MeterState m;
  m.update({1.0f, 0.5f});
  CHECK(m.peak() == 1.0f);
  CHECK(m.hold() == 1.0f);
  CHECK(m.clipping());
  CHECK(m.rms() == doctest::Approx(0.5f * MeterState::kRmsAttack).epsilon(1e-5));  // 70 % of the way up
  // silence: the peak falls by kPeakRelease a call, the hold holds for kHoldCalls calls
  for (unsigned i = 0; i < MeterState::kHoldCalls; ++i) {
    m.update({0.0f, 0.0f});
    CHECK(m.hold() == 1.0f);
  }
  CHECK(m.peak() == doctest::Approx(std::pow(MeterState::kPeakRelease, static_cast<float>(MeterState::kHoldCalls))).epsilon(1e-4));
  m.update({0.0f, 0.0f});  // the next call it falls
  CHECK(m.hold() == doctest::Approx(MeterState::kHoldFall).epsilon(1e-5));
  // the hold never falls below the peak bar
  for (int i = 0; i < 200; ++i) {
    m.update({0.3f, 0.2f});
    CHECK(m.hold() >= m.peak());
  }
  CHECK(m.rms() == doctest::Approx(0.2f).epsilon(1e-3));
  CHECK(m.peak() == doctest::Approx(0.3f).epsilon(1e-4));
  // the lamp: lit by a sample at full scale, out kClipHoldCalls updates later, or when cleared
  MeterState c;
  CHECK_FALSE(c.clipping());
  c.update({0.9998f, 0.4f});
  CHECK_FALSE(c.clipping());  // not quite full scale
  c.update({1.0f, 0.4f});
  CHECK(c.clipping());
  for (unsigned i = 0; i < MeterState::kClipHoldCalls - 1; ++i) {
    c.update({0.1f, 0.1f});
    CHECK(c.clipping());
  }
  c.update({0.1f, 0.1f});
  CHECK_FALSE(c.clipping());
  c.update({1.7f, 0.9f});  // above full scale (a float source): clip too
  CHECK(c.clipping());
  c.clear_clip();
  CHECK_FALSE(c.clipping());
  c.update({-0.5f, -0.5f});  // negatives are treated as 0
  CHECK(c.peak() >= 0.0f);
  CHECK(c.rms() >= 0.0f);
  c.reset();
  CHECK(c.peak() == 0.0f);
  CHECK(c.hold() == 0.0f);
}


TEST_CASE("plot meters: a bank is drawn in colour passes: five colours however many meters, two words a rectangle") {
  DbScale scale;
  scale.init(-60.0f, 0.0f);
  for (const unsigned n : {1u, 4u, 8u, 16u}) {
    std::vector<Rect> rects(n);
    std::vector<MeterState> states(n);
    for (unsigned i = 0; i < n; ++i) {
      rects[i] = {40 + static_cast<int>(i) * 44, 40, 28, 300};
      states[i].update({1.0f, 0.5f});
      states[i].update({0.5f, 0.5f});
    }
    const Counted c = counted([&](ui::Renderer& r) { draw_meters(r, rects.data(), states.data(), n, Axis::Vertical, scale, MeterStyle{}); });
    CHECK_MESSAGE(c.words <= meters_max_words(n), n << " meters: " << c.words << " words");
    CHECK_MESSAGE(c.colour_words <= 6, n << " meters: " << c.colour_words << " colours");
    CHECK(c.words >= 2u * 5u * n);  // frame, slot, body, peak, hold at least (+ the lamp)
  }
  // nothing to draw
  CHECK(counted([&](ui::Renderer& r) { draw_meters(r, nullptr, nullptr, 0, Axis::Vertical, scale, MeterStyle{}); }).words == 0);
}


TEST_CASE("plot scope: a block is decimated by a uniform stride, generations stack, the oldest falls off") {
  Scope s;
  CHECK(s.generations() == 0);
  const std::int16_t* x;
  const std::int16_t* y;
  CHECK(s.points(0, x, y) == 0);
  CHECK(x == nullptr);
  // a block of 480 frames: L = frame index, R = -frame index; 96 points a block: the frames 0, 5, 10, ...
  std::vector<std::int16_t> lr(2 * 480);
  for (unsigned i = 0; i < 480; ++i) {
    lr[2 * i] = static_cast<std::int16_t>(i * 10);
    lr[2 * i + 1] = static_cast<std::int16_t>(-static_cast<int>(i) * 10);
  }
  s.push(lr.data(), 480, ScopeMode::LeftRight);
  CHECK(s.generations() == 1);
  unsigned n = s.points(0, x, y);
  CHECK(n == Scope::kMaxPoints);
  for (unsigned i = 0; i < n; ++i) {
    CHECK(x[i] == static_cast<std::int16_t>(i * 5 * 10));
    CHECK(y[i] == static_cast<std::int16_t>(-static_cast<int>(i * 5) * 10));
  }
  // a short block keeps all its frames; one frame is ignored
  s.push(lr.data(), 10, ScopeMode::LeftRight);
  CHECK(s.points(0, x, y) == 10);
  CHECK(s.points(1, x, y) == Scope::kMaxPoints);  // the older block is age 1
  const unsigned before = s.generations();
  s.push(lr.data(), 1, ScopeMode::LeftRight);
  s.push(nullptr, 100, ScopeMode::LeftRight);
  CHECK(s.generations() == before);
  // more than kGenerations blocks: the oldest is dropped, ages count from the newest
  for (unsigned g = 0; g < 6; ++g) {
    for (unsigned i = 0; i < 480; ++i) {
      lr[2 * i] = static_cast<std::int16_t>(g * 1000);
    }
    s.push(lr.data(), 480, ScopeMode::LeftRight);
  }
  CHECK(s.generations() == Scope::kGenerations);
  for (unsigned age = 0; age < Scope::kGenerations; ++age) {
    s.points(age, x, y);
    CHECK(x[0] == static_cast<std::int16_t>((5 - age) * 1000));
  }
  CHECK(s.points(Scope::kGenerations, x, y) == 0);
  s.clear();
  CHECK(s.generations() == 0);
}


TEST_CASE("plot scope: mid / side turns a mono signal vertical and an anti-phase one horizontal") {
  Scope s;
  std::vector<std::int16_t> lr(2 * 200);
  for (unsigned i = 0; i < 200; ++i) {
    lr[2 * i] = static_cast<std::int16_t>(static_cast<int>(i) * 100 - 10050);  // never 0
    lr[2 * i + 1] = lr[2 * i];  // mono
  }
  s.push(lr.data(), 200, ScopeMode::MidSide);
  const std::int16_t* x;
  const std::int16_t* y;
  unsigned n = s.points(0, x, y);
  for (unsigned i = 0; i < n; ++i) {
    CHECK(x[i] == 0);  // side is nothing
    CHECK(y[i] != 0);
  }
  CHECK(y[n - 1] > y[0]);
  for (unsigned i = 0; i < 200; ++i) {
    lr[2 * i + 1] = static_cast<std::int16_t>(-lr[2 * i]);  // anti-phase
  }
  s.push(lr.data(), 200, ScopeMode::MidSide);
  n = s.points(0, x, y);
  for (unsigned i = 0; i < n; ++i) {
    CHECK(y[i] == 0);  // mid is nothing
  }
  // full scale does not overflow: (R - L) / 2 of +-32767 stays inside int16
  lr[0] = -32768;
  lr[1] = 32767;
  lr[2] = 32767;
  lr[3] = -32768;
  s.push(lr.data(), 2, ScopeMode::MidSide);
  n = s.points(0, x, y);
  CHECK(n == 2);
  CHECK(x[0] == 32767);
  CHECK(x[1] == -32767);  // (R - L) / 2 = -65535 / 2, truncated
}


TEST_CASE("plot scope: drawing is bounded, clipped to its square, with the glow as one more strip") {
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
  for (const bool lines : {false, true}) {
    for (const bool glow : {false, true}) {
      ScopeStyle style;
      style.lines = lines;
      style.glow = glow;
      const Counted c = counted([&](ui::Renderer& r) { s.draw(r, area, style); });
      CHECK_MESSAGE(c.words <= scope_max_words(), "lines " << lines << " glow " << glow << ": " << c.words);
      CHECK(c.words > 250u);  // the points of four generations (96 + 48 + 32 + 32), and the graticule
    }
  }
  ScopeStyle with, without;
  without.glow = false;
  const Counted a = counted([&](ui::Renderer& r) { s.draw(r, area, with); });
  const Counted b = counted([&](ui::Renderer& r) { s.draw(r, area, without); });
  CHECK(a.words == b.words + 5 + Scope::kMaxPoints);  // COLOR_A, size, BEGIN, the points, END, size
  CHECK(a.words < 400u);                               // the ADR's budget for a plot, glow included
  // too small a square draws nothing at all
  CHECK(counted([&](ui::Renderer& r) { s.draw(r, Rect{0, 0, 7, 400}, ScopeStyle{}); }).words == 0);
  // an empty scope still draws its graticule
  Scope empty;
  CHECK(counted([&](ui::Renderer& r) { empty.draw(r, area, ScopeStyle{}); }).words > 20);
}
