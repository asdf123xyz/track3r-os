// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host benchmarks of the plot library (ui/plot; OS-003d): relative figures only. The host is not the MK66: what counts is how
// the cost of each kernel scales (a column query is flat in the zoom, an update is O(edit), a wave bitmap is O(columns +
// rows)) and how the kernels compare with each other; cycle counts on the Cortex-M4F are a hw-session item (docs/specs/
// OS-003-ui-toolkit.md, 003d). The host build uses the scalar min/max loop; the M4's SSUB16 + SEL loop is checked for
// correctness on QEMU (see the spec) and measured on the device.
//
//   o3os_plot_bench [--smoke]     the full table (build with -DCMAKE_BUILD_TYPE=Release: the sanitizer build is for
//                                 correctness); --smoke runs every kernel a handful of times, as ctest does
#include <o3os/eve/display_list_builder.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/ui/plot/curve.h>
#include <o3os/ui/plot/fft.h>
#include <o3os/ui/plot/meters.h>
#include <o3os/ui/plot/peak_cache.h>
#include <o3os/ui/plot/scope.h>
#include <o3os/ui/plot/spectrum.h>
#include <o3os/ui/plot/wave_bitmap.h>
#include <o3os/ui/plot/wave_view.h>
#include <o3os/ui/renderer.h>
#include <o3os/ui/theme.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

using namespace o3os;
using namespace o3os::ui::plot;
using Clock = std::chrono::steady_clock;

namespace {

bool g_smoke = false;
volatile std::uint32_t g_sink = 0;  // keeps the optimiser from dropping what is measured

double seconds_since(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

// The best of `reps` runs of fn(), in seconds.
template <typename Fn>
double best_of(unsigned reps, Fn fn) {
  double best = 1e9;
  for (unsigned i = 0; i < (g_smoke ? 1u : reps); ++i) {
    const Clock::time_point t0 = Clock::now();
    fn();
    const double s = seconds_since(t0);
    best = s < best ? s : best;
  }
  return best;
}

struct Lcg {
  std::uint32_t s = 12345u;
  std::int16_t next() {
    s = s * 1664525u + 1013904223u;
    return static_cast<std::int16_t>(s >> 16);
  }
};

struct Sound {
  std::vector<std::int16_t> samples, store;
  PeakCache cache;
  Sound(Frame frames, unsigned channels) : samples(static_cast<std::size_t>(frames) * channels), store(PeakCache::elements_required(frames, channels)) {
    Lcg rng;
    for (std::size_t i = 0; i < samples.size(); ++i) {
      samples[i] = static_cast<std::int16_t>(rng.next() / (1 + static_cast<int>((i >> 12) % 7)));  // loud and quiet stretches
    }
    cache.init(store.data(), store.size(), samples.data(), frames, channels);
  }
};

template <typename Fn>
std::size_t words_of(Fn fn, std::size_t* out_words = nullptr) {
  platform::host::FakeDisplay sink;
  std::vector<std::uint32_t> storage(eve::kMaxDisplayListWords * 4);
  eve::DisplayListBuilder builder(sink, storage.data(), storage.size());
  ui::Renderer r(builder, ui::theme(ui::ThemeId::Amiga));
  r.begin(ui::Color::Pane);
  const std::size_t before = r.words();
  fn(r);
  const std::size_t n = r.words() - before;
  r.end();
  if (out_words != nullptr) {
    *out_words = n;
  }
  return n;
}

void row(const char* what, const char* unit_text, double value) { std::printf("%-58s %12.3f  %s\n", what, value, unit_text); }

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    g_smoke = g_smoke || std::strcmp(argv[i], "--smoke") == 0;
  }
  std::printf("o3os plot benches (host, relative figures%s)\n\n", g_smoke ? ", smoke run" : "");

  // ---- the peak cache ----
  const Frame big = g_smoke ? (1u << 18) : (1u << 22);  // 4 Mi frames: 8 MB mono, 16 MB stereo
  {
    Sound mono(big, 1), stereo(big, 2);
    const double t1 = best_of(3, [&] { mono.cache.build(); });
    const double t2 = best_of(3, [&] { stereo.cache.build(); });
    row("mipmap build, mono, per Msample", "ms", t1 * 1e3 / (static_cast<double>(big) / 1e6));
    row("mipmap build, stereo, per Msample (both channels)", "ms", t2 * 1e3 / (static_cast<double>(big) * 2.0 / 1e6));
    std::printf("%-58s %12.4f  bytes / sample\n", "mipmap storage", static_cast<double>(PeakCache::bytes_required(big, 1)) / static_cast<double>(big));
    std::vector<PeakPair> out(740);
    for (const Frame span : {Frame{740}, Frame{20000}, Frame{1000000}, big}) {
      const unsigned reps = 200;
      const double t = best_of(5, [&] {
        for (unsigned i = 0; i < reps; ++i) {
          mono.cache.columns(0, i % 64u, span > big - 64 ? big - 64 : span, 740, out.data());
          g_sink += static_cast<std::uint32_t>(out[7].max);
        }
      });
      char what[96];
      std::snprintf(what, sizeof(what), "query 740 columns over %u frames (%.0f frames a column)", span, static_cast<double>(span) / 740.0);
      row(what, "us", t * 1e6 / reps);
    }
    for (const Frame edit : {Frame{100}, Frame{10000}, Frame{1000000}}) {
      const double t = best_of(5, [&] {
        for (unsigned i = 0; i < 20; ++i) {
          mono.cache.update(1234 + i, edit);
        }
      });
      char what[96];
      std::snprintf(what, sizeof(what), "update after an edit of %u frames (in %u)", edit, big);
      row(what, "us", t * 1e6 / 20.0);
    }
  }

  // ---- the wave bitmap and the wave view ----
  {
    Sound s(1u << 20, 1);
    s.cache.build();
    platform::host::FakeDisplay display;
    const struct {
      unsigned w, h;
      WaveDepth depth;
      const char* name;
    } pictures[] = {{740, 150, WaveDepth::L1, "wave bitmap L1 740 x 150 (extents, rows, 4 uploads)"},
                    {740, 150, WaveDepth::L4, "wave bitmap L4 740 x 150"},
                    {800, 240, WaveDepth::L1, "wave bitmap L1 800 x 240"},
                    {800, 240, WaveDepth::L4, "wave bitmap L4 800 x 240 (the largest: 96 KB, 24 uploads)"}};
    auto bm = std::make_unique<WaveBitmap>();
    for (const auto& p : pictures) {
      unsigned n = 0;
      const double t = best_of(20, [&] {
        WaveKey key = wave_key(s.cache, ++n, 0, WaveWindow{0, 1u << 20}, ui::Rect{0, 0, static_cast<int>(p.w), static_cast<int>(p.h)}, p.depth);
        bm->request(0, key);
        bm->service_all(display);
      });
      row(p.name, "us", t * 1e6);
    }
    WaveMarks marks;
    marks.zero_line = true;
    marks.clip_tint = true;
    marks.has_selection = true;
    marks.sel_begin = 100000;
    marks.sel_end = 300000;
    marks.has_playhead = true;
    marks.playhead = 200000;
    const ui::Rect lane{20, 40, 740, 150};
    bm->request(0, wave_key(s.cache, 0, 0, WaveWindow{0, 1u << 20}, lane, WaveDepth::L1));
    bm->service_all(display);
    std::size_t w = 0;
    const double tb = best_of(20, [&] {
      for (unsigned i = 0; i < 50; ++i) {
        words_of([&](ui::Renderer& r) { draw_wave(r, lane, s.cache, 0, WaveWindow{0, 1u << 20}, WaveMode::BitmapL1, bm.get(), 0, 0, marks, WaveStyle{}); }, &w);
      }
    });
    char what[128];
    std::snprintf(what, sizeof(what), "draw: bitmap lane with marks (%zu words), display list only", w);
    row(what, "us", tb * 1e6 / 50.0);
    const double tv = best_of(20, [&] {
      for (unsigned i = 0; i < 50; ++i) {
        words_of([&](ui::Renderer& r) { draw_wave(r, lane, s.cache, 0, WaveWindow{0, 1u << 20}, WaveMode::Vector, nullptr, 0, 0, marks, WaveStyle{}); }, &w);
      }
    });
    std::snprintf(what, sizeof(what), "draw: vector envelope with marks (%zu words), incl. 740-column query", w);
    row(what, "us", tv * 1e6 / 50.0);
  }

  // ---- curves ----
  {
    const AxisSpec x{AxisKind::LogFreq, 20.0f, 20000.0f}, y{AxisKind::Db, -24.0f, 24.0f};
    const ui::Rect area{56, 40, 704, 320};
    constexpr unsigned kN = 384;
    float phi[kN], db[kN], xs[kN];
    response_basis(x, 44100.0f, kN, phi);
    for (unsigned i = 0; i < kN; ++i) {
      xs[i] = axis_value(x, static_cast<float>(i) / static_cast<float>(kN - 1));
    }
    const Biquad f{0.0201f, 0.0402f, 0.0201f, -1.561f, 0.6414f};
    const double tr = best_of(20, [&] {
      for (unsigned i = 0; i < 100; ++i) {
        biquad_response_db(f, phi, kN, db);
        g_sink += static_cast<std::uint32_t>(db[3]);
      }
    });
    row("biquad response, 384 points, from the basis", "us", tr * 1e6 / 100.0);
    CurveSeries s;
    s.x = xs;
    s.y = db;
    s.n = kN;
    auto scratch = std::make_unique<CurveScratch>();
    std::size_t w = 0;
    const double tc = best_of(20, [&] {
      for (unsigned i = 0; i < 100; ++i) {
        words_of([&](ui::Renderer& r) { draw_series(r, area, x, y, s, true, CurveStyle{}, *scratch); }, &w);
      }
    });
    char what[128];
    std::snprintf(what, sizeof(what), "draw: curve, 384 points reduced, fill + line (%zu words)", w);
    row(what, "us", tc * 1e6 / 100.0);
    std::vector<float> noise(1000);
    Lcg rng;
    for (float& v : noise) {
      v = static_cast<float>(rng.next()) / 32768.0f * 20.0f;
    }
    CurveSeries sn;
    sn.y = noise.data();
    sn.n = 1000;
    CurveStyle worst;
    worst.max_vertices = kCurveMaxVertices;
    worst.tolerance_px = 0.0f;
    const double tw = best_of(20, [&] {
      for (unsigned i = 0; i < 50; ++i) {
        words_of([&](ui::Renderer& r) { draw_series(r, area, x, y, sn, true, worst, *scratch); }, &w);
      }
    });
    std::snprintf(what, sizeof(what), "draw: curve, 1000 noisy points to 192 vertices (%zu words)", w);
    row(what, "us", tw * 1e6 / 50.0);
  }

  // ---- FFT, spectrum, meters, scope ----
  {
    for (const unsigned n : {256u, 512u}) {
      RealFft fft;
      fft.init(n);
      std::vector<float> in(n), out(n);
      for (unsigned i = 0; i < n; ++i) {
        in[i] = std::sin(0.3f * static_cast<float>(i));
      }
      const double t = best_of(20, [&] {
        for (unsigned i = 0; i < 200; ++i) {
          fft.forward(in.data(), out.data());
          g_sink += static_cast<std::uint32_t>(out[5]);
        }
      });
      char what[96];
      std::snprintf(what, sizeof(what), "real FFT, %u points", n);
      row(what, "us", t * 1e6 / 200.0);
    }
    Spectrum::Config cfg;
    cfg.bars = 64;
    Spectrum sp;
    sp.init(cfg);
    std::vector<std::int16_t> block(512);
    Lcg rng;
    for (std::int16_t& v : block) {
      v = rng.next();
    }
    const double ta = best_of(20, [&] {
      for (unsigned i = 0; i < 200; ++i) {
        sp.analyze(block.data(), 1);
      }
    });
    row("spectrum analyse, 512 points -> 64 bars (window, FFT, dB, smoothing)", "us", ta * 1e6 / 200.0);
    std::size_t w = 0;
    const double td = best_of(20, [&] {
      for (unsigned i = 0; i < 100; ++i) {
        words_of([&](ui::Renderer& r) { draw_spectrum(r, ui::Rect{56, 40, 704, 250}, sp, SpectrumStyle{}); }, &w);
      }
    });
    char what[96];
    std::snprintf(what, sizeof(what), "draw: spectrum, 64 bars with caps (%zu words)", w);
    row(what, "us", td * 1e6 / 100.0);
    Scope scope;
    std::vector<std::int16_t> lr(2 * 2048);
    for (std::int16_t& v : lr) {
      v = rng.next();
    }
    const double ts = best_of(20, [&] {
      for (unsigned i = 0; i < 100; ++i) {
        scope.push(lr.data(), 2048, ScopeMode::MidSide);
      }
    });
    row("scope: push a block of 2048 frames (decimate to 96)", "us", ts * 1e6 / 100.0);
    const double tsd = best_of(20, [&] {
      for (unsigned i = 0; i < 100; ++i) {
        words_of([&](ui::Renderer& r) { scope.draw(r, ui::Rect{40, 40, 330, 330}, ScopeStyle{}); }, &w);
      }
    });
    std::snprintf(what, sizeof(what), "draw: scope, 4 generations + glow (%zu words)", w);
    row(what, "us", tsd * 1e6 / 100.0);
    DbScale scale;
    scale.init(-60.0f, 0.0f);
    const ui::Rect bar{100, 40, 20, 300};
    std::vector<ui::Rect> rects(16, bar);
    std::vector<MeterState> states(16);
    for (MeterState& m : states) {
      m.update({0.8f, 0.4f});
    }
    const double tm = best_of(20, [&] {
      for (unsigned i = 0; i < 100; ++i) {
        words_of([&](ui::Renderer& r) { draw_meters(r, rects.data(), states.data(), 16, ui::Axis::Vertical, scale, MeterStyle{}); }, &w);
      }
    });
    std::snprintf(what, sizeof(what), "draw: 16 meters (%zu words)", w);
    row(what, "us", tm * 1e6 / 100.0);
    const double tl = best_of(20, [&] {
      for (unsigned i = 0; i < 100000; ++i) {
        g_sink += scale.position(0.0001f * static_cast<float>(i % 10000));
      }
    });
    row("dB LUT position(), per call", "ns", tl * 1e9 / 100000.0);
  }
  std::printf("\n(sink %u)\n", static_cast<unsigned>(g_sink));
  return 0;
}
