// SPDX-License-Identifier: GPL-3.0-or-later
//
// The display-list word bounds the plots declare (ui/plot/*.h) are hard: each is measured here against the worst case, in both
// translate modes, with the renderer's origin moved and a primitive of its own open when the plot starts, and with every colour
// of the plot's style a different token (so that no colour change is saved). Where the worst case can be built, the bound is met
// exactly: it is not just an upper limit, it is the cost.
#include <doctest/doctest.h>

#include <o3os/eve/display_list_builder.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/ui/plot/curve.h>
#include <o3os/ui/plot/meters.h>
#include <o3os/ui/plot/peak_cache.h>
#include <o3os/ui/plot/scope.h>
#include <o3os/ui/plot/spectrum.h>
#include <o3os/ui/plot/wave_bitmap.h>
#include <o3os/ui/plot/wave_view.h>
#include <o3os/ui/renderer.h>
#include <o3os/ui/theme.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

using namespace o3os;
using namespace o3os::ui::plot;
using ui::Axis;
using ui::Color;
using ui::Rect;

namespace {

// Words `fn` adds to a frame, in a translate mode, after (disturbed) a tile past x = 511 (which moves the renderer's origin when
// it can) and a rectangle (which leaves a primitive open and a colour cached) or on a fresh frame.
template <typename Fn>
std::size_t measure(Fn fn, bool translate, bool disturbed) {
  platform::host::FakeDisplay sink;
  std::vector<std::uint32_t> storage(eve::kMaxDisplayListWords * 4);
  eve::DisplayListBuilder builder(sink, storage.data(), storage.size());
  ui::Renderer r(builder, ui::theme(ui::ThemeId::Amiga));
  r.use_translate(translate);
  CHECK(r.begin(Color::Pane));
  if (disturbed) {
    r.hex_tile(600, 10, 0x12, Color::Text);
    r.panel({10, 10, 5, 5}, Color::Chrome);
  }
  const std::size_t before = r.words();
  fn(r);
  const std::size_t n = r.words() - before;
  CHECK(r.end());
  return n;
}

// The largest over the four situations, which also must not exceed `bound`.
template <typename Fn>
std::size_t worst(Fn fn, std::size_t bound, const char* what) {
  std::size_t most = 0;
  for (const bool translate : {true, false}) {
    for (const bool disturbed : {false, true}) {
      const std::size_t n = measure(fn, translate, disturbed);
      CHECK_MESSAGE(n <= bound, what << (translate ? " translate" : " vtx2f") << (disturbed ? " disturbed: " : " fresh: ") << n << " > " << bound);
      most = n > most ? n : most;
    }
  }
  return most;
}

struct Sound {
  std::vector<std::int16_t> samples, store;
  PeakCache cache;
  explicit Sound(Frame frames) : samples(frames), store(PeakCache::elements_required(frames, 1)) {
    std::uint32_t s = 7;
    for (Frame i = 0; i < frames; ++i) {
      s ^= s << 13;
      s ^= s >> 17;
      s ^= s << 5;
      samples[i] = static_cast<std::int16_t>(i % 5000 < 40 ? 32767 : static_cast<int>(s >> 16) / 2);
    }
    CHECK(cache.init(store.data(), store.size(), samples.data(), frames, 1));
    cache.build();
  }
};

}  // namespace

TEST_CASE("plot bounds: the wave marks, every mark on, distinct tokens: exactly kWaveMarksMaxWords") {
  const Rect lane{40, 100, 740, 150};
  const WaveWindow win{0, 60000};
  WaveMarks m;
  m.zero_line = true;
  m.clip_tint = true;
  m.has_selection = true;
  m.sel_begin = 10000;
  m.sel_end = 30000;
  m.has_loop = true;
  m.loop_start = 5000;
  m.loop_end = 50000;
  m.has_playhead = true;
  m.playhead = 25000;
  m.slice_count = kWaveMaxSlices;
  for (unsigned i = 0; i < kWaveMaxSlices; ++i) {
    m.slices[i] = i * 1800u;
  }
  ClipRun runs[kMaxClipRuns];
  for (unsigned i = 0; i < kMaxClipRuns; ++i) {
    runs[i] = ClipRun{static_cast<std::uint16_t>(i * 50), static_cast<std::uint16_t>(i * 50 + 20)};
  }
  WaveStyle style;
  style.zero = Color::Text;
  style.clip = Color::Accent;
  style.selection = Color::Sel;
  style.loop = Color::Fx;
  style.slice = Color::Instr;
  style.playhead = Color::Play;
  const std::size_t most = worst([&](ui::Renderer& r) { draw_wave_marks(r, lane, win, m, style, runs, kMaxClipRuns); }, kWaveMarksMaxWords, "marks");
  CHECK(most == kWaveMarksMaxWords);
  m.slice_count = 500;  // more than the limit is cut at the limit
  CHECK(worst([&](ui::Renderer& r) { draw_wave_marks(r, lane, win, m, style, runs, kMaxClipRuns); }, kWaveMarksMaxWords, "marks, 500 slices") == kWaveMarksMaxWords);
}

TEST_CASE("plot bounds: the wave: envelope, line and bitmap, at the widest and the longest they go") {
  Sound s(80000);
  WaveStyle style;
  style.wave = Color::Instr;
  style.background = Color::Fx;
  style.dots = Color::Accent;
  const unsigned mv = style.max_vertices;
  // the envelope: 192 columns is a vertex each; more are grouped
  for (const int w : {1, 100, 192, 193, 384, 640, 800}) {
    const Rect lane{0, 50, w, 120};
    const std::size_t most = worst([&](ui::Renderer& r) {
      draw_wave_vector(r, lane, s.cache, 0, WaveWindow{0, 80000}, style, nullptr, nullptr);
    }, wave_envelope_max_words(mv), "envelope");
    if (w == 192) {
      CHECK(most == wave_envelope_max_words(mv));  // the bound is the cost of a lane of max_vertices columns
    }
  }
  // the line: the longest view that is one, in a lane of 800 px, and a short one with dots
  for (const Frame span : {Frame{20}, Frame{133}, Frame{134}, Frame{2 * mv - 2}}) {
    const Rect lane{0, 50, 800, 120};
    worst([&](ui::Renderer& r) { draw_wave_vector(r, lane, s.cache, 0, WaveWindow{1000, span}, style, nullptr, nullptr); },
          wave_line_max_words(mv, 800), "line");
  }
  // the bitmap lane: a lane wider than 511 columns takes the size_h word
  WaveBitmap bm;
  platform::host::FakeDisplay display;
  const Rect lane{0, 50, 740, 150};
  bm.request(0, wave_key(s.cache, 0, 0, WaveWindow{0, 80000}, lane, WaveDepth::L4));
  CHECK(bm.service_all(display));
  const std::size_t most = worst([&](ui::Renderer& r) { draw_wave_bitmap(r, lane, bm, 0, style); }, kWaveBitmapMaxWords, "bitmap");
  CHECK(most == kWaveBitmapMaxWords);  // 4 + 2 + 5 + 5: the bound is the cost
  CHECK(wave_max_words(WaveMode::Vector, mv, 800) >= wave_envelope_max_words(mv));
  CHECK(wave_max_words(WaveMode::BitmapL1, mv, 800) == kWaveBitmapMaxWords);
}

TEST_CASE("plot bounds: the curve: a series at 192 vertices, sixteen handles, the densest axes with six-character labels") {
  const AxisSpec x{AxisKind::Linear, 0.0f, 1.0f}, y{AxisKind::Linear, -1.0f, 1.0f};
  const Rect area{56, 40, 704, 320};
  std::vector<float> v(1000);
  std::uint32_t seed = 3;
  for (float& f : v) {
    seed = seed * 1664525u + 1013904223u;
    f = static_cast<float>(seed >> 8) / 8388608.0f - 1.0f;
  }
  CurveSeries series;
  series.y = v.data();
  series.n = 1000;
  CurveStyle style;
  style.max_vertices = kCurveMaxVertices;
  style.tolerance_px = 0.0f;
  style.line = Color::Instr;
  style.fill = Color::Fx;
  auto scratch = std::make_unique<CurveScratch>();
  const std::size_t most = worst([&](ui::Renderer& r) { draw_series(r, area, x, y, series, true, style, *scratch); },
                                 curve_series_max_words(kCurveMaxVertices), "series");
  CHECK(most == curve_series_max_words(kCurveMaxVertices));
  // handles: eight idle, eight active, in three colours
  CurveHandle handles[kCurveMaxHandles];
  for (unsigned i = 0; i < kCurveMaxHandles; ++i) {
    handles[i] = {static_cast<float>(i) / 15.0f, 0.0f, i % 2 == 0};
  }
  style.handle = Color::Text;
  style.handle_active = Color::Accent;
  style.handle_ring = Color::Chrome;
  CHECK(worst([&](ui::Renderer& r) { draw_handles(r, area, x, y, handles, kCurveMaxHandles, style); }, kCurveHandlesMaxWords, "handles") ==
        kCurveHandlesMaxWords);
  CHECK(worst([&](ui::Renderer& r) { draw_handles(r, area, x, y, handles, 100, style); }, kCurveHandlesMaxWords, "handles, 100") ==
        kCurveHandlesMaxWords);  // more than the limit are cut at it
  // axes: a plot of the whole panel, ticks everywhere, labels of up to six characters, every colour a different token
  style.grid = Color::Empty;
  style.frame = Color::ChromeDim;
  style.reference = Color::Play;
  style.label = Color::TextDim;
  const AxisSpec big_x{AxisKind::Linear, 0.0f, 20000.0f}, big_y{AxisKind::Db, -999.0f, 999.0f}, hz{AxisKind::LogFreq, 20.0f, 20000.0f};
  const Rect whole{56, 20, 720, 400};
  const AxisSpec xs[3] = {big_x, hz, AxisSpec{AxisKind::Linear, -12345.0f, 12345.0f}};
  const AxisSpec ys[3] = {big_y, AxisSpec{AxisKind::Db, -24.0f, 24.0f}, AxisSpec{AxisKind::Linear, -1.0e5f, 1.0e5f}};
  for (unsigned i = 0; i < 3; ++i) {
    worst([&](ui::Renderer& r) { draw_axes(r, whole, xs[i], ys[i], style); }, kCurveAxesMaxWords, "axes");
  }
}

TEST_CASE("plot bounds: the spectrum: 64 bars with caps, bars and fill, exactly 4 * bars + 17 for the fill") {
  Spectrum::Config cfg;
  cfg.bars = Spectrum::kMaxBars;
  Spectrum s;
  CHECK(s.init(cfg));
  std::vector<std::int16_t> noise(512);
  std::uint32_t seed = 9;
  for (int pass = 0; pass < 30; ++pass) {
    for (std::int16_t& v : noise) {
      seed = seed * 1664525u + 1013904223u;
      v = static_cast<std::int16_t>(seed >> 16);
    }
    s.analyze(noise.data(), 1);
  }
  SpectrumStyle st;
  st.bar = Color::Instr;
  st.cap = Color::Accent;
  st.caps = true;
  st.shape = SpectrumShape::Fill;
  const Rect area{56, 40, 704, 250};
  CHECK(worst([&](ui::Renderer& r) { draw_spectrum(r, area, s, st); }, spectrum_max_words(64), "fill") == spectrum_max_words(64));
  st.shape = SpectrumShape::Bars;
  CHECK(worst([&](ui::Renderer& r) { draw_spectrum(r, area, s, st); }, spectrum_max_words(64), "bars") == 4u * 64u + 3u);
}

TEST_CASE("plot bounds: meters: sixteen with every part and six different tokens cost exactly 7 + 12 a meter; their scale is bounded") {
  DbScale scale;
  scale.init(-60.0f, 0.0f);
  MeterStyle style;
  style.frame = Color::ChromeDim;
  style.slot = Color::Pane;
  style.rms = Color::Play;
  style.peak = Color::Instr;
  style.hold = Color::Text;
  style.clip = Color::Accent;
  for (const Axis axis : {Axis::Vertical, Axis::Horizontal}) {
    for (const unsigned n : {1u, 4u, 16u}) {
      std::vector<Rect> rects(n);
      std::vector<MeterState> states(n);
      for (unsigned i = 0; i < n; ++i) {
        rects[i] = axis == Axis::Vertical ? Rect{40 + static_cast<int>(i) * 44, 40, 28, 300} : Rect{40, 40 + static_cast<int>(i) * 22, 300, 16};
        states[i].update({1.0f, 0.3f});  // a clip, a peak above the RMS, a hold
        states[i].update({0.6f, 0.3f});
      }
      const std::size_t most = worst([&](ui::Renderer& r) { draw_meters(r, rects.data(), states.data(), n, axis, scale, style); },
                                     meters_max_words(n), "meters");
      CHECK(most == meters_max_words(n));
    }
  }
  // the scale: marks with the widest labels (format_db clamps to four characters), either way
  const float marks[] = {-999.0f, -48.0f, -24.0f, -12.0f, -6.0f, -3.0f, 0.0f, 999.0f};  // "-999" and "+999": four characters
  for (const Axis axis : {Axis::Vertical, Axis::Horizontal}) {
    const Rect bar = axis == Axis::Vertical ? Rect{100, 50, 20, 300} : Rect{100, 50, 300, 16};
    worst([&](ui::Renderer& r) { draw_meter_scale(r, bar, axis, scale, marks, 8, style); }, meter_scale_max_words(8), "scale");
  }
}

TEST_CASE("plot bounds: the scope: four generations, lines or dots, with and without the glow, every token different") {
  Scope scope;
  std::vector<std::int16_t> lr(2 * 2048);
  for (unsigned i = 0; i < 2048; ++i) {
    lr[2 * i] = static_cast<std::int16_t>(30000 * std::sin(i * 0.07));
    lr[2 * i + 1] = static_cast<std::int16_t>(30000 * std::sin(i * 0.05));
  }
  for (int g = 0; g < 5; ++g) {
    scope.push(lr.data(), 2048, ScopeMode::LeftRight);
  }
  CHECK(scope.generations() == Scope::kGenerations);
  std::size_t overall = 0;
  for (const bool lines : {false, true}) {
    for (const bool glow : {false, true}) {
      ScopeStyle style;
      style.lines = lines;
      style.glow = glow;
      style.trace = Color::Play;
      style.graticule = Color::ChromeDim;
      style.axes = Color::Empty;
      const std::size_t most = worst([&](ui::Renderer& r) { scope.draw(r, Rect{40, 40, 330, 330}, style); }, scope_max_words(), "scope");
      overall = most > overall ? most : overall;
    }
  }
  CHECK(overall == scope_max_words());
}
