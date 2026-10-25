// SPDX-License-Identifier: GPL-3.0-or-later
//
// The wave view (ui/plot/wave_view.h) in the simulator's pixels (host only; the wave bitmap's own job is
// plot_wave_bitmap_test.cpp, which also runs on QEMU): the vector mode and the L1
// bitmap draw the same pixels, L4 differs from L1 only on edge pixels, every call stays inside its declared display-list
// words, and the raw sections leave the renderer's caches as they found them.
#include <doctest/doctest.h>

#include <o3os/eve/display_list_builder.h>
#include <o3os/eve/dl.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/sim/eve_raster.h>
#include <o3os/sim/raster_display.h>
#include <o3os/ui/plot/peak_cache.h>
#include <o3os/ui/plot/wave_bitmap.h>
#include <o3os/ui/plot/wave_view.h>
#include <o3os/ui/renderer.h>
#include <o3os/ui/theme.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace dl = o3os::eve::dl;
using namespace o3os;
using namespace o3os::ui::plot;
using ui::Color;
using ui::Rect;

namespace {

struct Rng {
  std::uint32_t s;
  explicit Rng(std::uint32_t seed) : s(seed ? seed : 1u) {}
  std::uint32_t next() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  }
  std::uint32_t below(std::uint32_t n) { return n == 0 ? 0 : next() % n; }
};

// A sound with its cache: signals that make a waveform worth looking at (decay, noise, silence, a clipped square).
struct Sound {
  std::vector<std::uint32_t> words;
  std::vector<std::uint64_t> store;
  Frame frames;
  unsigned channels;
  PeakCache cache;
  Sound(Frame n, unsigned ch, unsigned kind, std::uint32_t seed = 1)
      : words((static_cast<std::size_t>(n) * ch + 1) / 2 + 1),
        store(PeakCache::elements_required(n, ch) / 4 + 1),
        frames(n),
        channels(ch) {
    Rng rng(seed);
    auto* p = reinterpret_cast<std::int16_t*>(words.data());
    for (Frame f = 0; f < n; ++f) {
      for (unsigned c = 0; c < ch; ++c) {
        int v = 0;
        switch (kind) {
          case 0:  // a decaying burst of noise: loud at the start, dying away
          {
            const int amp = static_cast<int>(30000u * (n - f) / n);
            v = amp == 0 ? 0 : static_cast<int>(rng.below(static_cast<std::uint32_t>(2 * amp + 1))) - amp;
            break;
          }
          case 1:  // silence
            v = 0;
            break;
          case 2:  // a square at full scale: every column clips
            v = ((f / 37u) % 2u) != 0 ? 32767 : -32768;
            break;
          case 3:  // a slow triangle with an offset: no column crosses the zero line
            v = 4000 + static_cast<int>(((f * 7u) % 20000u)) - ((f * 7u) % 40000u > 20000u ? 10000 : 0);
            break;
          default:  // noise at full range
            v = static_cast<int>(rng.below(65536u)) - 32768;
            break;
        }
        p[static_cast<std::size_t>(f) * ch + c] = static_cast<std::int16_t>(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
      }
    }
    const bool ok = cache.init(reinterpret_cast<std::int16_t*>(store.data()), store.size() * 4, p, n, ch);
    CHECK(ok);
    cache.build();
  }
  const std::int16_t* data() const { return reinterpret_cast<const std::int16_t*>(words.data()); }
};

const ui::Theme& amiga() { return ui::theme(ui::ThemeId::Amiga); }

struct Panel {
  sim::EveRaster raster;
  sim::RasterDisplay display{raster};
  std::size_t last_words = 0;  // words of the last frame, DISPLAY included
  Panel() { CHECK(sim::load_assets(display) == hal::UploadResult::Ok); }
  // Draws a frame of `fn(renderer)` on a Pane background. Returns whether the raster took it.
  template <typename Fn>
  bool draw(Fn fn, const ui::Theme& theme = amiga(), bool translate = true) {
    std::vector<std::uint32_t> storage(eve::kMaxDisplayListWords);
    eve::DisplayListBuilder builder(display, storage.data(), eve::kMaxDisplayListWords);
    ui::Renderer r(builder, theme);
    r.use_translate(translate);
    CHECK(r.begin(Color::Pane));
    fn(r);
    const bool ok = r.end();
    last_words = display.last_frame_words();
    return ok && display.render_failures() == 0;
  }
  std::uint32_t pixel(int x, int y) const {
    const std::uint8_t* p = raster.rgba() + (static_cast<std::size_t>(y) * 800 + static_cast<std::size_t>(x)) * 4;
    return (std::uint32_t{p[0]} << 16) | (std::uint32_t{p[1]} << 8) | p[2];
  }
  std::vector<std::uint8_t> frame() const { return std::vector<std::uint8_t>(raster.rgba(), raster.rgba() + sim::EveRaster::kFramebufferBytes); }
};

std::uint32_t rgb_of(const ui::Theme& t, Color c) {
  const ui::Rgb& v = t.rgb(c);
  return (std::uint32_t{v.r} << 16) | (std::uint32_t{v.g} << 8) | v.b;
}

// Words one lambda adds to a frame.
template <typename Fn>
std::size_t words_of(Fn fn, const ui::Theme& theme = amiga(), bool translate = true) {
  platform::host::FakeDisplay sink;
  std::vector<std::uint32_t> storage(eve::kMaxDisplayListWords * 4);
  eve::DisplayListBuilder builder(sink, storage.data(), storage.size());
  ui::Renderer r(builder, theme);
  r.use_translate(translate);
  CHECK(r.begin(Color::Pane));
  const std::size_t before = r.words();
  fn(r);
  const std::size_t after = r.words();
  CHECK(r.end());
  return after - before;
}

}  // namespace

// ---- the wave bitmap -----------------------------------------------------------------------------------------------

TEST_CASE("plot wave view: the vector mode and the L1 bitmap draw the same pixels, everywhere") {
  struct View {
    unsigned kind;
    Frame first, span;
    Rect lane;
  };
  const View views[] = {
      {0, 0, 60000, {20, 40, 300, 150}},   {0, 0, 1000, {0, 0, 500, 100}},      {4, 100, 20000, {600, 300, 200, 160}},
      {2, 0, 20000, {40, 40, 320, 100}},   {3, 0, 40000, {100, 100, 256, 120}}, {1, 0, 5000, {7, 9, 301, 33}},
      {0, 30000, 60000, {0, 200, 320, 90}},  // the view runs past the end of the sound
      {4, 0, 700, {0, 0, 64, 240}},          // 11 frames a column
      {0, 0, 640, {10, 10, 320, 50}},        // two frames a column: just outside the line mode
  };
  Sound loud(60000, 1, 0, 8), noisy(60000, 1, 4, 9), square(60000, 1, 2, 1), offset(60000, 1, 3, 1), quiet(60000, 1, 1, 1);
  for (const ui::ThemeId id : {ui::ThemeId::Amiga, ui::ThemeId::Black, ui::ThemeId::White}) {
    const ui::Theme& theme = ui::theme(id);
    for (const View& v : views) {
      Sound& s = v.kind == 0 ? loud : (v.kind == 4 ? noisy : (v.kind == 2 ? square : (v.kind == 3 ? offset : quiet)));
      Panel a, b;
      WaveBitmap bm;
      WaveStyle style;
      style.max_vertices = 800;  // a vertex for every column: the case where the two modes overlap
      const WaveWindow window{v.first, v.span};
      // A lane is a flat fill of the background first, as the doc says; the same in both pictures.
      const auto lane_fill = [&](ui::Renderer& r) { r.panel(v.lane, Color::Pane); };
      CHECK(a.draw([&](ui::Renderer& r) {
        lane_fill(r);
        draw_wave(r, v.lane, s.cache, 0, window, WaveMode::Vector, nullptr, 0, 0, WaveMarks{}, style);
      }, theme));
      CHECK(bm.service_all(a.display) == true);  // nothing is requested by the vector mode
      bm.request(0, wave_key(s.cache, 0, 0, window, v.lane, WaveDepth::L1));
      CHECK(bm.service_all(b.display));
      CHECK(b.draw([&](ui::Renderer& r) {
        lane_fill(r);
        draw_wave(r, v.lane, s.cache, 0, window, WaveMode::BitmapL1, &bm, 0, 0, WaveMarks{}, style);
      }, theme));
      const bool same = a.frame() == b.frame();
      CHECK_MESSAGE(same, "kind " << v.kind << " lane " << v.lane.w << "x" << v.lane.h << " theme " << theme.name);
      // and the picture is not blank: some pixels are in the wave colour
      unsigned lit = 0;
      for (int y = v.lane.y; y < v.lane.y + v.lane.h; ++y) {
        for (int x = v.lane.x; x < v.lane.x + v.lane.w; ++x) {
          lit += a.pixel(x, y) == rgb_of(theme, style.wave) ? 1u : 0u;
        }
      }
      CHECK(lit > 0u);
      // nothing outside the lane changed
      CHECK(a.pixel(v.lane.x > 0 ? v.lane.x - 1 : 799, v.lane.y) == rgb_of(theme, Color::Pane));
    }
  }
}


TEST_CASE("plot wave view: a lane in the middle of a loud passage looks like the columns: hand-checked pixels") {
  // A square wave +-1/2 full scale: every column spans rows 1/4 to 3/4 of the lane.
  std::vector<std::uint32_t> words(2000);
  auto* p = reinterpret_cast<std::int16_t*>(words.data());
  for (unsigned i = 0; i < 4000; ++i) {
    p[i] = (i / 8) % 2 == 0 ? 16384 : -16384;
  }
  std::vector<std::uint64_t> store(PeakCache::elements_required(4000, 1) / 4 + 1);
  PeakCache cache;
  CHECK(cache.init(reinterpret_cast<std::int16_t*>(store.data()), store.size() * 4, p, 4000, 1));
  cache.build();
  const Rect lane{50, 100, 100, 80};
  // 40 frames a column: min -16384, max 16384: rows wave_row(16384, 80) .. wave_row(-16384, 80)
  const unsigned top = wave_row(16384, 80), bot = wave_row(-16384, 80);
  CHECK(top == 19);   // (32767 - 16384) * 80 >> 12 = 320 -> row 20? computed below instead of by hand
  CHECK(bot == 59);
  Panel panel;
  WaveStyle style;
  CHECK(panel.draw([&](ui::Renderer& r) {
    r.panel(lane, Color::Pane);
    draw_wave(r, lane, cache, 0, WaveWindow{0, 4000}, WaveMode::Vector, nullptr, 0, 0, WaveMarks{}, style);
  }));
  const std::uint32_t wave = rgb_of(amiga(), Color::Instr), pane = rgb_of(amiga(), Color::Pane);
  for (int c = 0; c < 100; c += 17) {
    CHECK(panel.pixel(lane.x + c, lane.y + static_cast<int>(top) - 1) == pane);
    CHECK(panel.pixel(lane.x + c, lane.y + static_cast<int>(top)) == wave);
    CHECK(panel.pixel(lane.x + c, lane.y + static_cast<int>(bot)) == wave);
    CHECK(panel.pixel(lane.x + c, lane.y + static_cast<int>(bot) + 1) == pane);
  }
  CHECK(panel.pixel(lane.x - 1, lane.y + 40) == pane);
  CHECK(panel.pixel(lane.x + 100, lane.y + 40) == pane);
}


TEST_CASE("plot wave view: wider than max_vertices the envelope is grouped, bounded in words, and still reaches the extremes") {
  Sound s(80000, 1, 0, 21);
  WaveStyle style;  // max_vertices 192: 2 words a column, the ADR's 400 words for a plot
  const unsigned kMax = style.max_vertices;
  CHECK(kMax == 192);
  Panel panel;
  const Rect lane{0, 50, 740, 200};
  std::size_t worst = 0;
  for (const int w : {1, 2, 100, 191, 192, 193, 320, 500, 640, 740, 800}) {
    const Rect l{0, 50, w, 200};
    const std::size_t words = words_of([&](ui::Renderer& r) {
      draw_wave(r, l, s.cache, 0, WaveWindow{0, 80000}, WaveMode::Vector, nullptr, 0, 0, WaveMarks{}, style);
    });
    worst = words > worst ? words : worst;
    CHECK_MESSAGE(words <= wave_envelope_max_words(style.max_vertices), "width " << w << ": " << words << " words");
    const unsigned k = (static_cast<unsigned>(w) + kMax - 1u) / kMax, groups = (static_cast<unsigned>(w) + k - 1u) / k;
    CHECK(words == 2u * groups + 10u);  // two strips, one vertex per group of k columns, and 10 words around them
  }
  CHECK(worst <= wave_envelope_max_words(kMax));
  CHECK(worst < 400);  // a vector lane at the default setting stays under the ADR's 400 words
  // The wave reaches the top of its extremes: the first column's loud start fills nearly the whole lane height.
  CHECK(panel.draw([&](ui::Renderer& r) {
    r.panel(lane, Color::Pane);
    draw_wave(r, lane, s.cache, 0, WaveWindow{0, 80000}, WaveMode::Vector, nullptr, 0, 0, WaveMarks{}, style);
  }));
  unsigned tall = 0;
  for (int y = lane.y; y < lane.y + lane.h; ++y) {
    tall += panel.pixel(2, y) == rgb_of(amiga(), Color::Instr) ? 1u : 0u;
  }
  CHECK(tall > 150u);
}


TEST_CASE("plot wave view: zoomed in past a frame a pixel the lane is a line through the frames, with dots, in bounded words") {
  Sound s(5000, 1, 4, 3);
  const Rect lane{40, 60, 700, 160};
  WaveStyle style;
  Panel panel;
  // 40 frames over 700 px: 17.5 px a frame, so dots too
  CHECK(panel.draw([&](ui::Renderer& r) {
    r.panel(lane, Color::Pane);
    draw_wave(r, lane, s.cache, 0, WaveWindow{1000, 40}, WaveMode::Vector, nullptr, 0, 0, WaveMarks{}, style);
  }));
  // a dot is Text-coloured and sits on each frame's centre: frame 1010 is at 40 + (10.5) * 17.5 = 223.75 px
  const std::int16_t v1010 = s.data()[1010];
  const int yc = lane.y + static_cast<int>(wave_y_q4(v1010, 160) >> 4);
  CHECK(panel.pixel(223, yc) == rgb_of(amiga(), Color::Text));
  // the line is in the wave colour between the dots
  unsigned touched = 0;  // antialiased: few pixels are exactly the wave colour, many are tinted by it
  for (int x = lane.x; x < lane.x + lane.w; ++x) {
    for (int y = lane.y; y < lane.y + lane.h; ++y) {
      touched += panel.pixel(x, y) != rgb_of(amiga(), Color::Pane) ? 1u : 0u;
    }
  }
  CHECK(touched > 300u);
  // words: a vertex a frame (42, one each side more than the view), a dot a frame (40) and a dozen
  const std::size_t words = words_of([&](ui::Renderer& r) {
    draw_wave(r, lane, s.cache, 0, WaveWindow{1000, 40}, WaveMode::Vector, nullptr, 0, 0, WaveMarks{}, style);
  });
  CHECK(words == 1 + 2 + 1 + 1 + 1 + 42 + 1 + 1 + 1 + 1 + 40 + 1 + 1);  // SAVE, scissor 2, colour, width, BEGIN, 42, END, colour, size, BEGIN, 40, END, RESTORE
  CHECK(words <= wave_line_max_words(style.max_vertices, 700));
  // the most a line may take: the view of 2 * max_vertices - 2 frames
  const std::size_t most = words_of([&](ui::Renderer& r) {
    draw_wave(r, lane, s.cache, 0, WaveWindow{100, 2u * style.max_vertices - 2u}, WaveMode::Vector, nullptr, 0, 0, WaveMarks{}, style);
  });
  CHECK(most <= wave_line_max_words(style.max_vertices, 700));
  // one frame more is an envelope (a column holds a frame or two) and is in the envelope's bound
  const std::size_t env = words_of([&](ui::Renderer& r) {
    draw_wave(r, lane, s.cache, 0, WaveWindow{100, 2u * style.max_vertices - 1u}, WaveMode::Vector, nullptr, 0, 0, WaveMarks{}, style);
  });
  CHECK(env <= wave_envelope_max_words(style.max_vertices));
  // a view past the end of the sound draws nothing at all of the wave
  CHECK(words_of([&](ui::Renderer& r) {
          draw_wave(r, lane, s.cache, 0, WaveWindow{9000, 40}, WaveMode::Vector, nullptr, 0, 0, WaveMarks{}, style);
        }) == 0);
}


TEST_CASE("plot wave view: the bitmap lane costs about ten words whatever its width, and says it cannot draw before it has a picture") {
  Sound s(60000, 1, 0, 3);
  Panel panel;
  WaveStyle style;
  std::size_t narrow = 0;
  for (const int w : {64, 300, 511, 512, 740, 800}) {
    WaveBitmap bm;  // a fresh lane each time: no picture yet
    const Rect lane{0, 100, w, 150};
    // without a picture: nothing is drawn, and the call says so; it asks for one
    bool drawn = true;
    const std::size_t before = words_of([&](ui::Renderer& r) {
      drawn = draw_wave(r, lane, s.cache, 0, WaveWindow{0, 60000}, WaveMode::BitmapL1, &bm, 0, 0, WaveMarks{}, style);
    });
    CHECK_FALSE(drawn);
    CHECK(before == 0);
    CHECK(bm.busy());
    CHECK(bm.service_all(panel.display));  // the job the call queued
    const std::size_t words = words_of([&](ui::Renderer& r) {
      drawn = draw_wave(r, lane, s.cache, 0, WaveWindow{0, 60000}, WaveMode::BitmapL1, &bm, 0, 0, WaveMarks{}, style);
    });
    CHECK(drawn);
    if (narrow == 0) {
      narrow = words;
    }
    // SAVE, scissor 2, handle, source, layout, size, [size_h past 511 columns], colour, BEGIN, CELL, vertex, END, RESTORE
    CHECK(words == (w > 511 ? 14u : 13u));
    CHECK(words <= kWaveBitmapMaxWords);
  }
  CHECK(narrow == 13);
  WaveBitmap bm;
  const Rect lane{0, 100, 800, 240};
  const std::size_t first = 14;
  // L4 costs the same
  bm.request(0, wave_key(s.cache, 0, 0, WaveWindow{0, 60000}, lane, WaveDepth::L4));
  CHECK(bm.service_all(panel.display));
  CHECK(words_of([&](ui::Renderer& r) {
          draw_wave(r, lane, s.cache, 0, WaveWindow{0, 60000}, WaveMode::BitmapL4, &bm, 0, 0, WaveMarks{}, style);
        }) == first);
  // a lane that is not allowed (too wide, too tall, no cache, no span, no such channel) draws nothing
  CHECK_FALSE(words_of([&](ui::Renderer& r) {
    CHECK_FALSE(draw_wave(r, Rect{0, 0, 801, 50}, s.cache, 0, WaveWindow{0, 100}, WaveMode::Vector, nullptr, 0, 0, WaveMarks{}, style));
    CHECK_FALSE(draw_wave(r, Rect{0, 0, 100, 241}, s.cache, 0, WaveWindow{0, 100}, WaveMode::BitmapL1, &bm, 0, 0, WaveMarks{}, style));
    CHECK_FALSE(draw_wave(r, Rect{0, 0, 100, 50}, s.cache, 0, WaveWindow{0, 0}, WaveMode::Vector, nullptr, 0, 0, WaveMarks{}, style));
    CHECK_FALSE(draw_wave(r, Rect{0, 0, 100, 50}, s.cache, 1, WaveWindow{0, 100}, WaveMode::Vector, nullptr, 0, 0, WaveMarks{}, style));
    PeakCache none;
    CHECK_FALSE(draw_wave(r, Rect{0, 0, 100, 50}, none, 0, WaveWindow{0, 100}, WaveMode::Vector, nullptr, 0, 0, WaveMarks{}, style));
    CHECK_FALSE(draw_wave(r, Rect{0, 0, 100, 50}, s.cache, 0, WaveWindow{0, 100}, WaveMode::BitmapL1, nullptr, 0, 0, WaveMarks{}, style));
  }) > 0);
}

// ---- the marks -----------------------------------------------------------------------------------------------------


TEST_CASE("plot wave marks: playhead, selection, loop, slices, zero line and clip bands land on the columns the view says") {
  Sound s(40000, 1, 1);  // silence: the wave is a one-pixel line, the marks are the only other thing in the lane
  const Rect lane{40, 100, 400, 100};
  const WaveWindow win{0, 40000};  // 100 frames a column
  WaveStyle style;
  const auto col = [&](Frame f) { return lane.x + static_cast<int>(PeakCache::column_of(win.first, win.span, 400, f)); };
  CHECK(col(10000) == 40 + 100);
  const std::uint32_t pane = rgb_of(amiga(), Color::Pane);
  Panel p;
  // playhead: a one-pixel rule in Play the full height of the lane, and a head at the top
  WaveMarks marks;
  marks.has_playhead = true;
  marks.playhead = 10000;
  CHECK(p.draw([&](ui::Renderer& r) {
    r.panel(lane, Color::Pane);
    draw_wave_marks(r, lane, win, marks, style, nullptr, 0);
  }));
  CHECK(p.pixel(col(10000), lane.y + 50) == rgb_of(amiga(), Color::Play));
  CHECK(p.pixel(col(10000) + 1, lane.y + 50) == pane);
  CHECK(p.pixel(col(10000) - 1, lane.y + 50) == pane);
  CHECK(p.pixel(col(10000), lane.y) == rgb_of(amiga(), Color::Play));
  CHECK(p.pixel(col(10000), lane.y + lane.h - 1) == rgb_of(amiga(), Color::Play));
  CHECK(p.pixel(col(10000), lane.y - 1) == pane);  // clipped to the lane
  CHECK(p.pixel(col(10000) + 3, lane.y + 1) == rgb_of(amiga(), Color::Play));  // the head is wider than the rule
  // selection: a translucent tint between two Sel rules; frames given in either order
  marks = WaveMarks{};
  marks.has_selection = true;
  marks.sel_begin = 16000;
  marks.sel_end = 8000;
  CHECK(p.draw([&](ui::Renderer& r) {
    r.panel(lane, Color::Pane);
    draw_wave_marks(r, lane, win, marks, style, nullptr, 0);
  }));
  CHECK(p.pixel(col(8000), lane.y + 30) == rgb_of(amiga(), Color::Sel));
  CHECK(p.pixel(col(16000), lane.y + 30) == rgb_of(amiga(), Color::Sel));
  const std::uint32_t tint = p.pixel(col(12000), lane.y + 30);
  CHECK(tint != pane);
  CHECK(tint != rgb_of(amiga(), Color::Sel));
  CHECK(p.pixel(col(8000) - 2, lane.y + 30) == pane);
  CHECK(p.pixel(col(16000) + 2, lane.y + 30) == pane);
  // a selection that begins before the view runs from the lane's left edge and has no rule there
  const WaveWindow later{10000, 40000};
  marks.sel_begin = 5000;
  marks.sel_end = 20000;
  CHECK(p.draw([&](ui::Renderer& r) {
    r.panel(lane, Color::Pane);
    draw_wave_marks(r, lane, later, marks, style, nullptr, 0);
  }));
  CHECK(p.pixel(lane.x, lane.y + 30) != pane);
  CHECK(p.pixel(lane.x, lane.y + 30) != rgb_of(amiga(), Color::Sel));  // tint, not a rule
  const int end_col = lane.x + static_cast<int>(PeakCache::column_of(later.first, later.span, 400, 20000));
  CHECK(p.pixel(end_col, lane.y + 30) == rgb_of(amiga(), Color::Sel));
  // loop: Fx rules and flags, a tint between
  marks = WaveMarks{};
  marks.has_loop = true;
  marks.loop_start = 4000;
  marks.loop_end = 24000;
  CHECK(p.draw([&](ui::Renderer& r) {
    r.panel(lane, Color::Pane);
    draw_wave_marks(r, lane, win, marks, style, nullptr, 0);
  }));
  CHECK(p.pixel(col(4000), lane.y + 60) == rgb_of(amiga(), Color::Fx));
  CHECK(p.pixel(col(24000), lane.y + 60) == rgb_of(amiga(), Color::Fx));
  CHECK(p.pixel(col(4000) + 3, lane.y + 2) == rgb_of(amiga(), Color::Fx));   // the start's flag points right
  CHECK(p.pixel(col(24000) - 3, lane.y + 2) == rgb_of(amiga(), Color::Fx));  // the end's points left
  CHECK(p.pixel(col(14000), lane.y + 60) != pane);                            // the tint
  // slices: a rule each, in the slice colour
  marks = WaveMarks{};
  marks.slice_count = 3;
  marks.slices[0] = 0;
  marks.slices[1] = 20000;
  marks.slices[2] = 39999;
  CHECK(p.draw([&](ui::Renderer& r) {
    r.panel(lane, Color::Pane);
    draw_wave_marks(r, lane, win, marks, style, nullptr, 0);
  }));
  for (const Frame f : {0u, 20000u, 39999u}) {
    CHECK(p.pixel(col(f), lane.y + 60) == rgb_of(amiga(), Color::Text));
  }
  CHECK(p.pixel(lane.x + 399, lane.y + 60) == rgb_of(amiga(), Color::Text));  // the last frame is the last column
  // the zero line: a translucent hairline on the row of value 0
  marks = WaveMarks{};
  marks.zero_line = true;
  CHECK(p.draw([&](ui::Renderer& r) {
    r.panel(lane, Color::Pane);
    draw_wave_marks(r, lane, win, marks, style, nullptr, 0);
  }));
  const int zr = lane.y + static_cast<int>(wave_zero_row(100));
  CHECK(p.pixel(lane.x + 50, zr) != pane);
  CHECK(p.pixel(lane.x + 50, zr - 1) == pane);
  CHECK(p.pixel(lane.x + 50, zr + 1) == pane);
  // clip bands: translucent over the runs' columns, the whole height
  marks = WaveMarks{};
  marks.clip_tint = true;
  const ClipRun runs[2] = {{10, 20}, {300, 300}};
  CHECK(p.draw([&](ui::Renderer& r) {
    r.panel(lane, Color::Pane);
    draw_wave_marks(r, lane, win, marks, style, runs, 2);
  }));
  CHECK(p.pixel(lane.x + 10, lane.y + 5) != pane);
  CHECK(p.pixel(lane.x + 20, lane.y + 95) != pane);
  CHECK(p.pixel(lane.x + 9, lane.y + 50) == pane);
  CHECK(p.pixel(lane.x + 21, lane.y + 50) == pane);
  CHECK(p.pixel(lane.x + 300, lane.y + 50) != pane);
  CHECK(p.pixel(lane.x + 301, lane.y + 50) == pane);
  // marks outside the view are not drawn at all
  marks = WaveMarks{};
  marks.has_playhead = true;
  marks.playhead = 50000;
  marks.has_selection = true;
  marks.sel_begin = 41000;
  marks.sel_end = 45000;
  marks.has_loop = true;
  marks.loop_start = 60000;
  marks.loop_end = 70000;
  marks.slice_count = 1;
  marks.slices[0] = 99999;
  CHECK(words_of([&](ui::Renderer& r) { draw_wave_marks(r, lane, win, marks, style, nullptr, 0); }) <= 8);  // a raw section and nothing in it
  // and none at all: no section
  CHECK(words_of([&](ui::Renderer& r) { draw_wave_marks(r, lane, win, WaveMarks{}, style, nullptr, 0); }) == 0);
}


TEST_CASE("plot wave marks: the worst case stays inside kWaveMarksMaxWords, and draw_wave composes the wave and the marks") {
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
  // moved origin: one more word
  const std::size_t worst = words_of([&](ui::Renderer& r) {
    r.hex_tile(600, 10, 1, Color::Text);
    const std::size_t before = r.words();
    draw_wave_marks(r, lane, win, m, style, runs, kMaxClipRuns);
    CHECK(r.words() - before <= kWaveMarksMaxWords);
  });
  (void)worst;
  // more slices than the limit are cut at the limit
  m.slice_count = 500;
  CHECK(words_of([&](ui::Renderer& r) { draw_wave_marks(r, lane, win, m, style, runs, kMaxClipRuns); }) <= kWaveMarksMaxWords);
  // draw_wave: wave first, then the marks: the wave's words plus the marks' words
  Sound s(60000, 1, 0, 3);
  WaveBitmap bm;
  Panel panel;
  bm.request(0, wave_key(s.cache, 0, 0, win, lane, WaveDepth::L1));
  CHECK(bm.service_all(panel.display));
  const std::size_t wave_only = words_of([&](ui::Renderer& r) {
    draw_wave(r, lane, s.cache, 0, win, WaveMode::BitmapL1, &bm, 0, 0, WaveMarks{}, style);
  });
  const std::size_t marks_only = words_of([&](ui::Renderer& r) { draw_wave_marks(r, lane, win, m, style, bm.shown_clips(0), bm.shown_clip_count(0)); });
  const std::size_t both = words_of([&](ui::Renderer& r) { draw_wave(r, lane, s.cache, 0, win, WaveMode::BitmapL1, &bm, 0, 0, m, style); });
  CHECK(both == wave_only + marks_only);
}


TEST_CASE("plot wave view: draws are bracketed so the renderer carries on correctly after them, in both translate modes") {
  Sound s(60000, 1, 0, 3);
  Panel panel;
  WaveBitmap bm;
  const Rect lane{40, 100, 400, 120};
  const WaveWindow win{0, 60000};
  bm.request(0, wave_key(s.cache, 0, 0, win, lane, WaveDepth::L1));
  CHECK(bm.service_all(panel.display));
  WaveMarks marks;
  marks.has_selection = true;
  marks.sel_begin = 10000;
  marks.sel_end = 20000;
  for (const bool translate : {true, false}) {
    // a tile past x = 511 (the renderer moves its origin), the wave lane, another tile: the tiles must be what they would be
    // without the wave between them, and the wave must be where it was without the tile before it
    const auto picture = [&](bool with_wave, bool with_tiles) -> std::vector<std::uint8_t> {
      Panel q;
      CHECK(q.draw([&](ui::Renderer& r) {
        if (with_tiles) {
          r.hex_tile(600, 10, 0x5A, Color::Instr);
        }
        r.panel({10, 10, 20, 20}, Color::Chrome);
        if (with_wave) {
          WaveBitmap local;
          local.request(0, wave_key(s.cache, 0, 0, win, lane, WaveDepth::L1));
          local.service_all(r.display());
          draw_wave(r, lane, s.cache, 0, win, WaveMode::BitmapL1, &local, 0, 0, marks, WaveStyle{});
        }
        r.panel({10, 40, 20, 20}, Color::Chrome);  // the colour the renderer had before the wave
        if (with_tiles) {
          r.hex_tile(640, 10, 0xC3, Color::Instr);
        }
      }, amiga(), translate));
      return q.frame();
    };
    const std::vector<std::uint8_t> with = picture(true, true), tiles_only = picture(false, true), wave_only = picture(true, false);
    const auto at = [](const std::vector<std::uint8_t>& f, int x, int y) {
      const std::uint8_t* px = &f[(static_cast<std::size_t>(y) * 800 + static_cast<std::size_t>(x)) * 4];
      return (std::uint32_t{px[0]} << 16) | (std::uint32_t{px[1]} << 8) | px[2];
    };
    for (int y = 0; y < 480; ++y) {
      for (int x = 0; x < 800; ++x) {
        const bool in_lane = x >= lane.x && x < lane.x + lane.w && y >= lane.y && y < lane.y + lane.h;
        if (in_lane) {
          if (at(with, x, y) != at(wave_only, x, y)) {
            CHECK_MESSAGE(false, "wave pixel (" << x << "," << y << ") changed by the tiles, translate " << translate);
            return;
          }
        } else if (at(with, x, y) != at(tiles_only, x, y)) {
          CHECK_MESSAGE(false, "pixel (" << x << "," << y << ") outside the lane changed by the wave, translate " << translate);
          return;
        }
      }
    }
  }
}


TEST_CASE("plot wave view: a view opened while the cache is building draws and renders nothing, and a rebuilt cache is never shown by a stale picture") {
  Sound s(60000, 1, 0, 3);
  // The storage is poisoned and the build started over: the entries are not there (on the device: SDRAM nobody wrote).
  for (std::uint64_t& w : s.store) {
    w = 0x7F7F7F7F7F7F7F7Full;
  }
  s.cache.build_begin();
  CHECK_FALSE(s.cache.built());
  CHECK(s.cache.build_step(500) == false);
  const Rect lane{40, 100, 300, 120};
  const WaveWindow win{0, 60000};
  const WaveKey key = wave_key(s.cache, 0, 0, win, lane, WaveDepth::L1);
  CHECK_FALSE(key.valid());
  Panel panel;
  WaveBitmap bm;
  WaveStyle style;
  WaveMarks marks;
  marks.has_playhead = true;
  marks.playhead = 100;
  // bitmap mode: nothing drawn (not even the marks), no job started, nothing uploaded
  bool drawn = true;
  CHECK(words_of([&](ui::Renderer& r) {
          drawn = draw_wave(r, lane, s.cache, 0, win, WaveMode::BitmapL1, &bm, 0, 0, marks, style);
        }) == 0);
  CHECK_FALSE(drawn);
  CHECK_FALSE(bm.busy());
  CHECK(bm.service_all(panel.display));
  CHECK(bm.completed() == 0);
  CHECK(bm.failed() == 0);
  const std::size_t uploads_before = panel.display.accepted_uploads();
  bm.request(0, key);
  CHECK_FALSE(bm.busy());  // an invalid key is no request
  CHECK(bm.service_all(panel.display));
  CHECK(panel.display.accepted_uploads() == uploads_before);
  // vector mode: nothing either
  CHECK(words_of([&](ui::Renderer& r) {
          draw_wave(r, lane, s.cache, 0, win, WaveMode::Vector, nullptr, 0, 0, marks, style);
          draw_wave_vector(r, lane, s.cache, 0, win, style, nullptr, nullptr);
        }) == 0);
  // a request made before the build began, and still queued when it began again, is dropped without being counted a failure
  while (!s.cache.build_step(1u << 30)) {
  }
  CHECK(s.cache.built());
  const WaveKey ready = wave_key(s.cache, 0, 0, win, lane, WaveDepth::L1);
  CHECK(ready.valid());
  bm.request(0, ready);
  CHECK(bm.busy());
  s.cache.build_begin();  // the build starts over before the job runner gets to it
  CHECK(bm.service_all(panel.display));
  CHECK(bm.failed() == 0);
  CHECK(bm.completed() == 0);
  CHECK_FALSE(bm.ready(0));
  while (!s.cache.build_step(1u << 30)) {
  }
  // after the build the view renders, from the entries the build made: the picture is the reference's
  CHECK(words_of([&](ui::Renderer& r) { draw_wave(r, lane, s.cache, 0, win, WaveMode::BitmapL1, &bm, 0, 0, WaveMarks{}, style); }) == 0);  // asks, nothing to draw yet
  CHECK(bm.busy());
  CHECK(bm.service_all(panel.display));
  std::vector<PeakPair> peaks(300);
  s.cache.columns(0, 0, 60000, 300, peaks.data());
  std::vector<std::uint8_t> want(WaveBitmap::stride_of(300, WaveDepth::L1) * 120);
  WaveBitmap::render_reference(peaks.data(), 300, 120, WaveDepth::L1, want.data());
  CHECK(std::memcmp(want.data(), panel.raster.ram_g() + bm.shown_addr(0), want.size()) == 0);
  // the sound is edited and the cache rebuilt: the picture of the old samples is not the new view's, though its key's fields
  // (generation, view, size) are unchanged: the epoch is what differs
  const WaveKey old_key = bm.shown_key(0);
  auto* p = reinterpret_cast<std::int16_t*>(s.words.data());
  for (Frame f = 0; f < 60000; ++f) {
    p[f] = static_cast<std::int16_t>(p[f] / 4);
  }
  s.cache.build();
  const WaveKey new_key = wave_key(s.cache, 0, 0, win, lane, WaveDepth::L1);
  CHECK(new_key.valid());
  CHECK(new_key.generation == old_key.generation);
  CHECK(new_key.epoch != old_key.epoch);
  CHECK(new_key != old_key);
  CHECK_FALSE(bm.shows(0, new_key));
  CHECK(words_of([&](ui::Renderer& r) { draw_wave(r, lane, s.cache, 0, win, WaveMode::BitmapL1, &bm, 0, 0, WaveMarks{}, style); }) > 0);  // the old picture, till the new is ready
  CHECK(bm.busy());
  CHECK(bm.service_all(panel.display));
  CHECK(bm.shows(0, new_key));
  s.cache.columns(0, 0, 60000, 300, peaks.data());
  std::vector<std::uint8_t> want_new(want.size());
  WaveBitmap::render_reference(peaks.data(), 300, 120, WaveDepth::L1, want_new.data());
  CHECK(std::memcmp(want_new.data(), panel.raster.ram_g() + bm.shown_addr(0), want.size()) == 0);
  CHECK(want_new != want);  // and it really is a different picture
}
