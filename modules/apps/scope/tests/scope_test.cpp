// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-H2b (new Community OS behaviour): the Scope Spectrum app and the FFT it shares with the Tuner. The FFT (modules/apps/common/o3m_fft.h) against a direct DFT on random input; the trigger of
// the scope (scope_math.h) on its own; then the app driven through the ABI as the OS drives it, on a fake of the ABI 1.4 host tables (tap_latest answers the window the test sets): the
// descriptor, an OS that lends no tap, the tap point, the scope's trigger, free run, timebases and trace, the spectrum's bars (a sine lands in the right bar, the level in dBFS, the falls),
// silence / DC / a full-scale square / clipping, `frames == 0`, a hostile host, the draw budgets, and the packed .tmod. Host only.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../../../tests/o3m_test_support.h"

#include "../../common/o3m_fft.h"
#include "../scope_math.h"

#include <chrono>
#include <cmath>
#include <complex>
#include <functional>
#include <string>

using namespace o3m_test;
using namespace o3os::module;

namespace {

constexpr std::uint32_t kView = 0, kSource = 1, kTimebase = 2;

std::uint32_t lcg_state = 12345u;
double rnd() {  // deterministic, -1..1
  lcg_state = lcg_state * 1664525u + 1013904223u;
  return static_cast<double>(lcg_state >> 8) / 8388608.0 - 1.0;
}

// The reference: the DFT by its definition, in double.
std::vector<std::complex<double>> dft(const std::vector<double>& x) {
  const std::size_t n = x.size();
  std::vector<std::complex<double>> out(n);
  for (std::size_t k = 0; k < n; ++k) {
    std::complex<double> acc = 0.0;
    for (std::size_t j = 0; j < n; ++j) {
      const double a = -2.0 * kPi * static_cast<double>((k * j) % n) / static_cast<double>(n);
      acc += x[j] * std::complex<double>(std::cos(a), std::sin(a));
    }
    out[k] = acc;
  }
  return out;
}

// Runs the o3fft transform to the end, counting the calls (with the default budget one call is one unit).
unsigned run_fft(const o3m_host& host, std::vector<float>& re, std::vector<float>& im, std::uint32_t budget = 0xFFFFFFFFu) {
  const std::uint32_t n = static_cast<std::uint32_t>(re.size());
  std::vector<float> c(n / 2), s(n / 2);
  o3fft::init_twiddle(&host, c.data(), s.data(), n);
  o3fft::Job job;
  o3fft::begin(&job, n);
  unsigned units = 0;
  while (!o3fft::done(&job)) {
    const bool end = o3fft::step(&job, re.data(), im.data(), c.data(), s.data(), budget);
    ++units;
    CHECK(end == o3fft::done(&job));
  }
  CHECK(o3fft::step(&job, re.data(), im.data(), c.data(), s.data(), budget));  // a finished job stays finished and touches nothing
  return units;
}

}  // namespace

TEST_CASE("fft: matches a direct DFT on random input (64, 1024 and 2048 points), unit by unit") {
  const o3m_host host = make_host();
  for (const std::uint32_t n : {4u, 64u, 1024u, 2048u}) {
    std::vector<double> x(n), y(n);
    std::vector<float> re(n), im(n);
    for (std::uint32_t i = 0; i < n; ++i) {
      x[i] = rnd() * 20000.0;
      y[i] = rnd() * 20000.0;
      re[i] = static_cast<float>(x[i]);
      im[i] = static_cast<float>(y[i]);
    }
    std::vector<std::complex<double>> z(n);
    for (std::uint32_t i = 0; i < n; ++i) {
      z[i] = std::complex<double>(x[i], y[i]);
    }
    // the complex transform of (x, y): compare with the DFT of x + i y, built from two real DFTs
    const auto X = dft(x), Y = dft(y);
    std::vector<std::complex<double>> want(n);
    double peak = 0.0;
    for (std::uint32_t k = 0; k < n; ++k) {
      want[k] = X[k] + std::complex<double>(0.0, 1.0) * Y[k];
      peak = std::fmax(peak, std::abs(want[k]));
    }
    const unsigned units = run_fft(host, re, im);
    CHECK(units == o3fft::log2u(n) + 1u);  // the bit reversal and one unit a stage
    double worst = 0.0;
    for (std::uint32_t k = 0; k < n; ++k) {
      worst = std::fmax(worst, std::abs(std::complex<double>(re[k], im[k]) - want[k]));
    }
    CHECK_MESSAGE(worst <= 2.0e-5 * peak, "n = " << n << ": worst error " << worst << " against a peak of " << peak);
    MESSAGE("fft " << n << ": " << units << " units, worst error " << worst / peak << " of the peak");
  }
}

TEST_CASE("fft: chunks of any size give the very same result as whole units, and every call is bounded") {
  const o3m_host host = make_host();
  for (const std::uint32_t n : {64u, 1024u}) {
    std::vector<float> re0(n), im0(n), re1, im1;
    for (std::uint32_t i = 0; i < n; ++i) {
      re0[i] = static_cast<float>(rnd() * 9000.0);
      im0[i] = static_cast<float>(rnd() * 9000.0);
    }
    re1 = re0;
    im1 = im0;
    const unsigned whole = run_fft(host, re0, im0);
    for (const std::uint32_t budget : {1u, 7u, 100u, 256u}) {
      std::vector<float> re = re1, im = im1;
      const unsigned calls = run_fft(host, re, im, budget);
      CHECK(calls >= whole);
      CHECK(calls <= whole + (n / 2u) * o3fft::log2u(n) / budget + n / budget + 2u);
      for (std::uint32_t i = 0; i < n; ++i) {
        CHECK(re[i] == re0[i]);  // the same operations in the same order: bit for bit
        CHECK(im[i] == im0[i]);
      }
    }
  }
  // a budget of 0 does no work and does not hang the caller's accounting
  std::vector<float> re(64, 1.0f), im(64, 0.0f), c(32), s(32);
  o3fft::init_twiddle(&host, c.data(), s.data(), 64);
  o3fft::Job job;
  o3fft::begin(&job, 64);
  CHECK(!o3fft::step(&job, re.data(), im.data(), c.data(), s.data(), 0));
}

TEST_CASE("fft: a real sine lands in its bin, the twiddles and the Hann window are accurate, the cost in butterflies is the textbook one") {
  const o3m_host host = make_host();
  const std::uint32_t n = 1024;
  std::vector<float> re(n), im(n, 0.0f);
  for (std::uint32_t i = 0; i < n; ++i) {
    re[i] = static_cast<float>(1000.0 * std::sin(2.0 * kPi * 100.0 * i / n + 0.7));
  }
  run_fft(host, re, im);
  std::uint32_t best = 0;
  for (std::uint32_t k = 1; k < n / 2; ++k) {
    best = std::hypot(re[k], im[k]) > std::hypot(re[best], im[best]) ? k : best;
  }
  CHECK(best == 100u);
  CHECK(std::hypot(re[100], im[100]) == doctest::Approx(1000.0f * n / 2.0f).epsilon(1e-4));
  // the twiddles
  std::vector<float> c(1024), s(1024);
  o3fft::init_twiddle(&host, c.data(), s.data(), 2048);
  for (std::uint32_t k = 0; k < 1024; ++k) {
    CHECK(std::fabs(c[k] - std::cos(2.0 * kPi * k / 2048.0)) < 1e-6);
    CHECK(std::fabs(s[k] - std::sin(2.0 * kPi * k / 2048.0)) < 1e-6);
  }
  // the Hann window: periodic, 0 at the start, 1 in the middle, sum n / 2
  for (const std::uint32_t m : {1024u, 1536u, 2048u}) {
    std::vector<float> w(m, 1.0f);
    o3fft::hann(&host, w.data(), m);
    double sum = 0.0, worst = 0.0;
    for (std::uint32_t j = 0; j < m; ++j) {
      sum += w[j];
      worst = std::fmax(worst, std::fabs(w[j] - 0.5 * (1.0 - std::cos(2.0 * kPi * j / m))));
    }
    CHECK(w[0] == 0.0f);
    CHECK(worst < 1e-5);
    CHECK(sum == doctest::Approx(m / 2.0).epsilon(1e-5));
  }
  MESSAGE("butterflies of a 1024-point transform: " << (1024 / 2) * 10 << ", of 2048: " << (2048 / 2) * 11 << " (40 to 50 cycles each at -Os on the M4F, from the compiled loop)");
}

TEST_CASE("scope: the trigger finds the first rising zero crossing") {
  // a sine of 20 samples per period starting at the top: falls, crosses zero downward, rises through zero
  std::vector<std::int16_t> x(256);
  for (std::size_t i = 0; i < x.size(); ++i) {
    x[i] = static_cast<std::int16_t>(std::lrint(10000.0 * std::cos(2.0 * kPi * static_cast<double>(i) / 20.0)));
  }
  const int32_t t = scope_math::find_trigger(x.data(), static_cast<std::uint32_t>(x.size()), 0, 255);
  NEED(t > 0);
  CHECK((x[static_cast<std::size_t>(t) - 1u] < 0 && x[static_cast<std::size_t>(t)] >= 0));
  CHECK(t == 15);  // cos is negative from 5 to 15 and non-negative at 15
  // the window may not start before `pre` and may not end after `last`: the next crossing is taken
  CHECK(scope_math::find_trigger(x.data(), 256, 16, 255) == 35);
  CHECK(scope_math::find_trigger(x.data(), 256, 0, 14) == -1);
  CHECK(scope_math::find_trigger(x.data(), 256, 0, 15) == 15);
  CHECK(scope_math::find_trigger(x.data(), 256, 0, 100000) == 15);  // `last` is clamped to the window
  // a falling edge alone is not a trigger; a rise from a positive start needs the signal to have been low first
  std::vector<std::int16_t> fall(64);
  for (std::size_t i = 0; i < fall.size(); ++i) {
    fall[i] = static_cast<std::int16_t>(16000 - 500 * static_cast<int>(i));
  }
  CHECK(scope_math::find_trigger(fall.data(), 64, 0, 63) == -1);
  std::vector<std::int16_t> rise(64);
  for (std::size_t i = 0; i < rise.size(); ++i) {
    rise[i] = static_cast<std::int16_t>(-16000 + 500 * static_cast<int>(i));
  }
  CHECK(scope_math::find_trigger(rise.data(), 64, 0, 63) == 32);
  // noise around zero does not fire it: it must have been below -1/8 of the peak
  std::vector<std::int16_t> hiss(512);
  for (auto& v : hiss) {
    v = static_cast<std::int16_t>(std::lrint(rnd() * 40.0));
  }
  hiss[400] = 6000;  // one spike of 6000: the hysteresis is 750, the hiss never reaches -750
  CHECK(scope_math::find_trigger(hiss.data(), 512, 0, 511) == -1);
  hiss[420] = -2000;  // now it has been low: the next rise through zero fires
  const int32_t th = scope_math::find_trigger(hiss.data(), 512, 0, 511);
  CHECK(th > 420);
  CHECK(hiss[static_cast<std::size_t>(th)] >= 0);
  // silence, DC, a window too quiet, and the edge cases of the arguments
  std::vector<std::int16_t> zero(256, 0), dcv(256, 12000), faint(256);
  for (std::size_t i = 0; i < faint.size(); ++i) {
    faint[i] = static_cast<std::int16_t>(i % 20 < 10 ? -20 : 20);  // a square of peak 20: below the 32 floor
  }
  CHECK(scope_math::find_trigger(zero.data(), 256, 0, 255) == -1);
  CHECK(scope_math::find_trigger(dcv.data(), 256, 0, 255) == -1);
  CHECK(scope_math::find_trigger(faint.data(), 256, 0, 255) == -1);
  CHECK(scope_math::find_trigger(nullptr, 256, 0, 255) == -1);
  CHECK(scope_math::find_trigger(zero.data(), 1, 0, 0) == -1);
  CHECK(scope_math::find_trigger(zero.data(), 0, 0, 0) == -1);
  // full-scale values: -32768 has no positive counterpart (the magnitude is taken in 32 bits)
  std::vector<std::int16_t> sq(128);
  for (std::size_t i = 0; i < sq.size(); ++i) {
    sq[i] = i % 32 < 16 ? static_cast<std::int16_t>(-32768) : static_cast<std::int16_t>(32767);
  }
  CHECK(scope_math::find_trigger(sq.data(), 128, 0, 127) == 16);
  CHECK(scope_math::peak_of(sq.data(), 0, 128) == -32768);
  const std::int16_t two[2] = {-5, 5};
  CHECK(scope_math::peak_of(two, 0, 2) == -5);  // the first of equals
}

namespace {

using Wave = std::function<double(std::uint64_t)>;

struct Scope {
  Lib lib;
  Instance inst;
  FakeApi api;
  ViewHost views;
  bool ok = false;
  float p[3] = {1.0f, 2.0f, 3.0f};
  explicit Scope(std::uint32_t caps = O3M_CAP_AUDIO_TAP) {
    ViewHost::Metrics m;
    const std::int16_t cw[O3M_FACE_COUNT] = {16, 10, 8, 6}, ch[O3M_FACE_COUNT] = {32, 20, 16, 12};
    for (unsigned f = 0; f < O3M_FACE_COUNT; ++f) {
      m.cell_w[f] = cw[f];
      m.cell_h[f] = ch[f];
    }
    views.set_metrics(m);
    ok = lib.open(O3M_SCOPE_HOST_SO) && inst.start_with(*lib.entry, make_app_host(api, caps));
    if (ok) {
      inst.m->ui->enter(inst.state);
    }
  }
  void set(std::uint32_t id, float v) {
    inst.set(id, v);
    p[id] = inst.values[id];
  }
  void event(std::uint32_t kind, std::uint32_t id, std::int32_t value, std::uint32_t mods = 0) {
    const o3m_ui_event ev = {kind, id, value, mods};
    CHECK(views.call_input(*lib.entry, inst.state, 1, ev) == ViewStatus::Ok);
  }
  Drawing draw(int w = 490, int h = 384, std::uint32_t max_words = kViewMaxWords) {
    Drawing d;
    CHECK(views.draw(*lib.entry, inst.state, 1, 0, w, h, p, 3, max_words) == ViewStatus::Ok);
    views.replay(d);
    return d;
  }
  // The tap's newest 4,096 frames are the wave from `from` on.
  void window(const Wave& w, std::uint64_t from, std::size_t frames = 4096) {
    api.tap_window.resize(frames);
    for (std::size_t i = 0; i < frames; ++i) {
      api.tap_window[i] = static_cast<std::int16_t>(std::lrint(std::fmax(-32768.0, std::fmin(32767.0, w(from + i)))));
    }
  }
  Drawing frame(const Wave& w, std::uint64_t from, std::uint32_t ms = 33) {
    api.now += ms;
    window(w, from);
    return draw();
  }
  // Frames until the spectrum has been through a transform or two.
  Drawing settle(const Wave& w, int frames = 6) {
    Drawing d;
    for (int i = 0; i < frames; ++i) {
      d = frame(w, static_cast<std::uint64_t>(i) * 1456u);
    }
    return d;
  }
};

Wave sine(double hz, double amp, double phase = 0.3) {
  return [=](std::uint64_t i) { return amp * std::sin(2.0 * kPi * hz * static_cast<double>(i) / kRate + phase); };
}
Wave constant(double v) {
  return [=](std::uint64_t) { return v; };
}

// The value of a row of the page (o3edit::draw_rows: the name at the left, the value on the same line): "MASTER" of SOURCE, "SCOPE" of VIEW, "3" of TIMEBASE.
std::string row_text(const Drawing& d, const std::string& name) {
  for (const auto& n : d.texts) {
    if (n.s != name) {
      continue;
    }
    for (const auto& v : d.texts) {
      if (v.y == n.y && v.x > n.x) {
        return v.s;
      }
    }
  }
  return "";
}
bool has(const Drawing& d, const std::string& s) {
  for (const auto& t : d.texts) {
    if (t.s == s) {
      return true;
    }
  }
  return false;
}
bool contains(const Drawing& d, const std::string& s) {
  for (const auto& t : d.texts) {
    if (t.s.find(s) != std::string::npos) {
      return true;
    }
  }
  return false;
}
std::string starts(const Drawing& d, const std::string& prefix) {
  for (const auto& t : d.texts) {
    if (t.s.rfind(prefix, 0) == 0) {
      return t.s;
    }
  }
  return "";
}
// The bars of the spectrum view: the PLAY / ACCENT rects of the plot, by their x, with the height they have.
std::vector<int> bar_heights(const Drawing& d, int w = 490) {
  std::vector<int> h(48, 0);
  const int px = 44, pw = w - px - 8;
  for (const auto& r : d.rects) {
    if (r.color == O3M_COLOR_PLAY || r.color == O3M_COLOR_ACCENT) {
      for (int b = 0; b < 48; ++b) {
        if (r.x == px + b * pw / 48) {
          h[static_cast<std::size_t>(b)] = r.h;
        }
      }
    }
  }
  return h;
}
int tallest(const std::vector<int>& h) {
  int best = 0;
  for (std::size_t i = 1; i < h.size(); ++i) {
    best = h[i] > h[static_cast<std::size_t>(best)] ? static_cast<int>(i) : best;
  }
  return best;
}
// The bar a frequency falls in: 48 bars of 1/5.35 octave from 40 Hz.
int bar_of(double hz) { return static_cast<int>(std::floor(std::log2(hz / 40.0) / std::log2(500.0) * 48.0)); }

}  // namespace

TEST_CASE("scope: descriptor, capability and an OS that lends no draw table") {
  Scope a;
  NEED(a.ok);
  const o3m_module& m = *a.lib.entry;
  CHECK(m.kind == O3M_KIND_APP);
  CHECK(std::string(m.name) == "Scope Spectrum");
  CHECK((m.abi_major == 1u && m.abi_minor == 10u));  // (1.10: VIEW, SOURCE and TIMEBASE are the app's own rows, with an input() of their own, OS-APPUX S3)
  CHECK(m.view_count == 1u);
  CHECK((m.render == nullptr && m.note_on == nullptr && m.note_off == nullptr));
  CHECK(m.param_count == 3u);
  CHECK(std::string(m.params[0].name) == "View");
  CHECK((m.params[0].min == 1.0f && m.params[0].max == 2.0f && m.params[0].def == 1.0f && (m.params[0].flags & O3M_PARAM_INT) != 0u));
  CHECK(std::string(m.params[1].name) == "Source");
  CHECK((m.params[1].min == 1.0f && m.params[1].max == 2.0f && m.params[1].def == 2.0f));
  CHECK(std::string(m.params[2].name) == "Timebase");
  CHECK((m.params[2].min == 1.0f && m.params[2].max == 5.0f && m.params[2].def == 3.0f));
  alignas(16) static unsigned char state[65536] = {};
  o3m_host old = a.inst.host;
  old.draw = nullptr;
  CHECK(m.init(state, &old) != 0);
}

TEST_CASE("scope: an OS that lends no tap says so, whatever table is missing; the tap point follows the parameter") {
  {
    Scope a(0);
    NEED(a.ok);
    CHECK(has(a.draw(), "NO AUDIO TAP FOR THIS APP"));
    CHECK(a.api.tap_selects.empty());
  }
  {
    Scope a;
    NEED(a.ok);
    o3m_io_api v13 = a.api.io;
    v13.size = static_cast<std::uint32_t>(offsetof(o3m_io_api, tap_select));
    o3m_host h = a.inst.host;
    h.io = &v13;
    Instance b;
    NEED(b.start_with(*a.lib.entry, h));
    b.m->ui->enter(b.state);
    Drawing d;
    CHECK(a.views.draw(*a.lib.entry, b.state, 1, 0, 490, 384, a.p, 3) == ViewStatus::Ok);
    a.views.replay(d);
    CHECK(has(d, "NO AUDIO TAP FOR THIS APP"));
  }
  Scope a;
  NEED(a.ok);
  CHECK(row_text(a.draw(), "SOURCE") == "MASTER");
  NEED(a.api.tap_selects.size() == 1u);
  CHECK(a.api.tap_selects[0] == O3M_TAP_MASTER);
  (void)a.draw();
  CHECK(a.api.tap_selects.size() == 1u);
  a.set(kSource, 1.0f);
  CHECK(row_text(a.draw(), "SOURCE") == "INPUT");
  CHECK(a.api.tap_selects.back() == O3M_TAP_INPUT);
  a.set(kSource, 9.0f);
  CHECK(row_text(a.draw(), "SOURCE") == "MASTER");
  a.set(kView, 2.0f);
  CHECK(has(a.draw(), "SPECTRUM"));
  a.set(kView, std::nanf(""));
  CHECK(has(a.draw(), "SCOPE"));
}

TEST_CASE("scope: a rising edge triggers, the trace is steady from frame to frame, and the span follows the timebase") {
  Scope a;
  NEED(a.ok);
  const Wave w = sine(200.0, 20000.0);
  Drawing d = a.frame(w, 0);
  CHECK(contains(d, "TRIGGER RISING 0"));
  CHECK(has(d, "SPAN 11.6 MS   TRIGGER RISING 0"));
  NEED(d.polys.size() == 2u);
  CHECK(d.polys[0].xy.size() / 2u + d.polys[1].xy.size() / 2u - 1u == 248u);
  CHECK((d.polys[0].xy.size() / 2u <= O3M_VIEW_MAX_POLY_POINTS && d.polys[1].xy.size() / 2u <= O3M_VIEW_MAX_POLY_POINTS));
  // the second polyline starts where the first ends
  CHECK(d.polys[1].xy[0] == d.polys[0].xy[d.polys[0].xy.size() - 2u]);
  CHECK(d.polys[1].xy[1] == d.polys[0].xy[d.polys[0].xy.size() - 1u]);
  // the same wave seen from other offsets (the window moves by a few hundred frames a frame) draws the same trace, to a pixel or two (the trigger has the resolution of a frame)
  auto flat = [](const Drawing& x) {
    std::vector<int> y;
    for (const auto& p : x.polys) {
      for (std::size_t i = 1; i < p.xy.size(); i += 2) {
        y.push_back(p.xy[i]);
      }
    }
    return y;
  };
  const std::vector<int> y0 = flat(d);
  for (const std::uint64_t from : {1456ull, 2912ull, 4001ull, 77777ull}) {
    const Drawing e = a.frame(w, from);
    CHECK(has(e, "SPAN 11.6 MS   TRIGGER RISING 0"));
    const std::vector<int> y1 = flat(e);
    NEED(y1.size() == y0.size());
    int worst = 0;
    for (std::size_t i = 0; i < y0.size(); ++i) {
      worst = std::max(worst, std::abs(y0[i] - y1[i]));
    }
    CHECK_MESSAGE(worst <= 4, "offset " << from << ": the trace moved by " << worst << " px");
  }
  // the trigger sits at 1/8 of the width: the trace is at the middle line there, rising
  const int cy = 44 + (384 - 44 - 96) / 2;  // (the plot is 384 - 44 - 96 = 244 high since the three rows are at the bottom)
  const std::size_t k = 248 / 8;  // the point of the trigger
  CHECK(std::abs(y0[k] - cy) <= 12);
  CHECK(y0[k + 3] < y0[k]);  // y grows downward: rising means smaller y
  // the peak of a 20,000 sine (0.61 of full scale) is 0.61 of the half-height from the middle
  int top = 999;
  for (const int y : y0) {
    top = std::min(top, y);
  }
  CHECK(std::abs((cy - top) - static_cast<int>(0.61 * (244 / 2 - 2))) <= 3);
  // timebases: 128, 256, 512, 1024 and 1536 frames
  const char* spans[5] = {"SPAN 2.9 MS", "SPAN 5.8 MS", "SPAN 11.6 MS", "SPAN 23.2 MS", "SPAN 34.8 MS"};
  for (int tb = 1; tb <= 5; ++tb) {
    a.set(kTimebase, static_cast<float>(tb));
    const Drawing e = a.frame(w, 3000);
    CHECK(starts(e, "SPAN ").rfind(spans[tb - 1], 0) == 0);
    CHECK(e.polys.size() == 2u);
    CHECK(has(e, std::string(spans[tb - 1]) + "   TRIGGER RISING 0"));
  }
  a.set(kTimebase, 99.0f);
  CHECK(starts(a.frame(w, 0), "SPAN ").rfind("SPAN 34.8 MS", 0) == 0);
  // the number of points follows the span below 248: 128 frames draw 128 points
  a.set(kTimebase, 1.0f);
  const Drawing e = a.frame(w, 0);
  CHECK(e.polys[0].xy.size() / 2u + e.polys[1].xy.size() / 2u - 1u == 128u);
}

TEST_CASE("scope: no crossing is a free run of the newest span; silence, DC, a low note and a full-scale square draw within the budgets") {
  Scope a;
  NEED(a.ok);
  // silence: a flat line at the middle, FREE RUN
  Drawing d = a.frame(constant(0.0), 0);
  CHECK(has(d, "SPAN 11.6 MS   FREE RUN"));
  NEED(d.polys.size() == 2u);
  const int cy = 44 + (384 - 44 - 96) / 2;  // (the plot is 384 - 44 - 96 = 244 high since the three rows are at the bottom)
  for (const auto& p : d.polys) {
    for (std::size_t i = 1; i < p.xy.size(); i += 2) {
      CHECK(p.xy[i] == cy);
    }
  }
  // DC at +12000 and at -32768: flat lines off the middle, inside the plot
  d = a.frame(constant(12000.0), 0);
  CHECK(contains(d, "FREE RUN"));
  CHECK(d.polys[0].xy[1] < cy);
  d = a.frame(constant(-32768.0), 0);
  CHECK(d.polys[0].xy[1] > cy);
  CHECK(d.polys[0].xy[1] < 44 + (384 - 44 - 60));
  // a note too low to have a crossing in the 2,048 - span frames left to look in: free run
  a.set(kTimebase, 4.0f);
  d = a.frame(sine(20.0, 20000.0, 1.0), 0);
  CHECK(contains(d, "FREE RUN"));
  // a full-scale square wave, and a clipped sine: every point inside the plot
  const Wave sq = [](std::uint64_t i) { return (i / 100) % 2 == 0 ? 32767.0 : -32768.0; };
  d = a.draw(490, 384, 450);
  d = a.frame(sq, 0);
  CHECK(contains(d, "TRIGGER RISING 0"));
  for (const auto& p : d.polys) {
    for (std::size_t i = 1; i < p.xy.size(); i += 2) {
      CHECK((p.xy[i] >= 44 && p.xy[i] < 44 + (384 - 44 - 60)));
    }
  }
  d = a.frame([](std::uint64_t i) { return 90000.0 * std::sin(2.0 * kPi * 300.0 * static_cast<double>(i) / kRate); }, 0);
  CHECK(contains(d, "TRIGGER RISING 0"));
  // frames == 0 (the tap off or nothing yet): WAITING, no trace, and it draws again when the samples come
  a.api.tap_window.clear();
  d = a.draw();
  CHECK(contains(d, "WAITING FOR AUDIO"));
  CHECK(d.polys.empty());
  d = a.frame(sine(200.0, 10000.0), 0);
  CHECK(contains(d, "TRIGGER RISING 0"));
  // right after a select the front of the window is zeros and fewer frames are real: it still draws
  a.api.tap_window.assign(300, 8000);
  d = a.draw();
  CHECK(d.polys.size() == 2u);
  // a host that claims more frames than asked: not believed
  a.api.tap_overreturn = 0xFFFFFFFFu;
  d = a.draw();
  CHECK(contains(d, "WAITING FOR AUDIO"));
  // a small page and a tiny one
  a.api.tap_overreturn = 0;
  a.window(sine(200.0, 10000.0), 0);
  (void)a.draw(480, 288);
  (void)a.draw(800, 480);
  (void)a.draw(16, 16);
  (void)a.draw(100, 100);
}

TEST_CASE("spectrum: a sine lands in the right bar at the right level") {
  Scope a;
  NEED(a.ok);
  a.set(kView, 2.0f);
  // (below about 200 Hz a bar is narrower than a bin, 43 Hz: those bars share the Hann lobe and the tallest of them is not the one under the tone)
  for (const int bar : {14, 20, 27, 33, 40, 46}) {
    const double hz = 40.0 * std::pow(2.0, (bar + 0.5) * std::log2(500.0) / 48.0);  // the middle of the bar
    Scope b;
    NEED(b.ok);
    b.set(kView, 2.0f);
    const Drawing d = b.settle(sine(hz, 16384.0));
    const std::vector<int> h = bar_heights(d);
    const int want = bar_of(hz);
    CHECK_MESSAGE(tallest(h) == want, hz << " Hz: tallest bar " << tallest(h) << ", expected " << want);
    // the neighbours two bars away are well down: the Hann window's sidelobes are -31 dB and falling
    for (int k = want - 3; k <= want + 3; k += 6) {
      if (k >= 0 && k < 48) {
        CHECK_MESSAGE(h[static_cast<std::size_t>(k)] < h[static_cast<std::size_t>(want)] - 20, hz << " Hz: bar " << k << " too high");
      }
    }
  }
  // the level: a sine on a bin centre (861.3 Hz is bin 20) at half scale is -6 dBFS; a full-scale one is 0 dBFS
  Scope b;
  NEED(b.ok);
  b.set(kView, 2.0f);
  Drawing d = b.settle(sine(20.0 * kRate / 1024.0, 16384.0));
  CHECK(has(d, "PEAK 861 HZ  -6 DB"));
  Scope c;
  NEED(c.ok);
  c.set(kView, 2.0f);
  d = c.settle(sine(20.0 * kRate / 1024.0, 32767.0));
  const std::string pk = starts(d, "PEAK ");
  CHECK((pk == "PEAK 861 HZ  0 DB" || pk == "PEAK 861 HZ  -1 DB"));
  // the bar heights are the dB scale: 60 dB over the plot height (384 - 44 - 114 = 226)
  const std::vector<int> hc = bar_heights(d);
  CHECK(hc[static_cast<std::size_t>(bar_of(20.0 * kRate / 1024.0))] >= 219);
  // the plot has the dB scale and the frequency scale
  for (const char* t : {"-12", "-24", "-36", "-48", "-60", "100", "1K", "10K", "20K"}) {
    CHECK_MESSAGE(has(d, t), t);
  }
}

TEST_CASE("spectrum: the bars fall, the peak-hold line falls slower, and a sine moved to another bar moves the bar") {
  Scope a;
  NEED(a.ok);
  a.set(kView, 2.0f);
  const double hz = 20.0 * kRate / 1024.0;
  Drawing d = a.settle(sine(hz, 16384.0));
  const int bar = bar_of(hz);
  const int h0 = bar_heights(d)[static_cast<std::size_t>(bar)];
  auto hold_y = [&](const Drawing& x) {
    if (x.polys.size() != 1u) {
      CHECK(x.polys.size() == 1u);
      return 0;
    }
    return static_cast<int>(x.polys[0].xy[2u * static_cast<std::size_t>(bar) + 1u]);
  };
  const int y0 = hold_y(d);
  // silence for 0.5 s (15 frames): the bar falls 30 dB (half its 60 dB range, so 113 px of the 226), the hold line 6 dB (23 px)
  for (int i = 0; i < 16; ++i) {
    d = a.frame(constant(0.0), 0);
  }
  const int h1 = bar_heights(d)[static_cast<std::size_t>(bar)];
  CHECK_MESSAGE(std::abs((h0 - h1) - 113) <= 14, "the bar fell " << h0 - h1 << " px");
  CHECK_MESSAGE(std::abs((hold_y(d) - y0) - 23) <= 6, "the hold line fell " << hold_y(d) - y0 << " px");
  // 2 s of silence: the bar is gone, the line has fallen 24 dB
  for (int i = 0; i < 60; ++i) {
    d = a.frame(constant(0.0), 0);
  }
  CHECK(bar_heights(d)[static_cast<std::size_t>(bar)] == 0);
  CHECK(hold_y(d) - y0 > 90);
  // another tone: another bar
  d = a.settle(sine(3000.0, 16384.0));
  CHECK(tallest(bar_heights(d)) == bar_of(3000.0));
  // a stall of seconds is one step of the fall, not a plunge
  Scope b;
  NEED(b.ok);
  b.set(kView, 2.0f);
  d = b.settle(sine(hz, 16384.0));
  const int hb = bar_heights(d)[static_cast<std::size_t>(bar)];
  d = b.frame(constant(0.0), 0, 6000);
  d = b.frame(constant(0.0), 0, 6000);
  CHECK(bar_heights(d)[static_cast<std::size_t>(bar)] > hb - 113 - 113 - 20);
}

TEST_CASE("spectrum: the clock is checked before every chunk, so a frame is bounded; the bars still come over later frames") {
  const Wave w = sine(20.0 * kRate / 1024.0, 16384.0);
  auto run = [&](Scope& a, std::uint32_t tick_every, int frames, std::uint32_t& worst_ms, std::uint32_t& worst_calls, bool& engaged) {
    Drawing d;
    a.api.tick_every = tick_every;
    for (int i = 0; i < frames; ++i) {
      a.api.now += 33;
      a.window(w, static_cast<std::uint64_t>(i) * 1456u);
      const std::uint32_t n0 = a.api.now, c0 = a.api.now_calls;
      d = a.draw();
      worst_ms = std::max(worst_ms, a.api.now - n0);
      worst_calls = std::max(worst_calls, a.api.now_calls - c0);
      engaged = engaged || a.api.now - n0 >= 2u;
    }
    return d;
  };
  {  // a clock that never moves: the cap of 12 chunks bounds the frame
    Scope a;
    NEED(a.ok);
    a.set(kView, 2.0f);
    std::uint32_t ms = 0, calls = 0;
    bool engaged = false;
    const Drawing d = run(a, 0, 20, ms, calls, engaged);
    CHECK(calls <= 16u);
    CHECK(tallest(bar_heights(d)) == bar_of(20.0 * kRate / 1024.0));
  }
  {  // 1 ms every 4 looks: the guard stops the transform after about seven chunks, the transform takes more frames, and it completes
    Scope a;
    NEED(a.ok);
    a.set(kView, 2.0f);
    std::uint32_t ms = 0, calls = 0;
    bool engaged = false;
    const Drawing d = run(a, 4, 80, ms, calls, engaged);
    CHECK(engaged);
    CHECK(ms <= 3u);
    CHECK(calls <= 16u);
    CHECK(tallest(bar_heights(d)) == bar_of(20.0 * kRate / 1024.0));
    CHECK(has(d, "PEAK 861 HZ  -6 DB"));
  }
  {  // already past the guard when the work would start (every look is a millisecond): no chunk runs, the frame costs two looks, and nothing is drawn from a half-made transform
    Scope a;
    NEED(a.ok);
    a.set(kView, 2.0f);
    std::uint32_t ms = 0, calls = 0;
    bool engaged = false;
    const Drawing d = run(a, 1, 20, ms, calls, engaged);
    CHECK(calls <= 3u);
    for (const int h : bar_heights(d)) {
      CHECK(h == 0);
    }
  }
}

TEST_CASE("spectrum: silence, DC, a full-scale square and clipping draw sane bars; frames == 0 and a hostile host draw nothing") {
  Scope a;
  NEED(a.ok);
  a.set(kView, 2.0f);
  Drawing d = a.settle(constant(0.0));
  for (const int h : bar_heights(d)) {
    CHECK(h == 0);
  }
  CHECK(has(d, "PEAK --"));
  // DC is removed before the transform: no bar even at full scale
  d = a.settle(constant(32767.0));
  for (const int h : bar_heights(d)) {
    CHECK(h < 4);
  }
  d = a.settle(constant(-32768.0));
  for (const int h : bar_heights(d)) {
    CHECK(h < 4);
  }
  // a full-scale square at 100 Hz: harmonics every 200 Hz, the fundamental near 0 dB; heights inside the plot
  const Wave sq = [](std::uint64_t i) { return (i % 441) < 220 ? 32767.0 : -32768.0; };
  d = a.settle(sq);
  const std::vector<int> h = bar_heights(d);
  CHECK(tallest(h) >= 4);
  for (const int v : h) {
    CHECK(v <= 262);
  }
  d = a.draw(490, 384, 450);
  CHECK(has(d, "SPECTRUM"));
  // clipping: a sine at 5 x full scale is a near-square; the fundamental is the tallest bar
  d = a.settle(sine(500.0, 160000.0));
  CHECK(tallest(bar_heights(d)) == bar_of(500.0));
  // frames == 0: the bars stay as they were, nothing is drawn from the missing window
  a.api.tap_window.clear();
  d = a.draw();
  CHECK(has(d, "SPECTRUM"));
  // a hostile host
  Scope b;
  NEED(b.ok);
  b.set(kView, 2.0f);
  b.api.tap_overreturn = 0xFFFFFFFFu;
  for (int i = 0; i < 10; ++i) {
    b.api.now += 33;
    d = b.draw();
  }
  for (const int v : bar_heights(d)) {
    CHECK(v == 0);
  }
  // the clock wraps: the fall is computed with an unsigned difference
  Scope c;
  NEED(c.ok);
  c.set(kView, 2.0f);
  c.api.now = 0xFFFFFF80u;
  d = c.settle(sine(1100.0, 16384.0));
  CHECK(tallest(bar_heights(d)) == bar_of(1100.0));
  CHECK(bar_heights(d)[static_cast<std::size_t>(bar_of(1100.0))] > 170);
}

TEST_CASE("spectrum: the draw stays within the call budget at any size; the host cost per frame is reported") {
  Scope a;
  NEED(a.ok);
  a.set(kView, 2.0f);
  const Wave w = [](std::uint64_t i) {
    double v = 0.0;
    for (int k = 1; k < 40; ++k) {
      v += 900.0 * std::sin(2.0 * kPi * 110.0 * k * static_cast<double>(i) / kRate);
    }
    return v;
  };
  (void)a.settle(w);
  const Drawing d = a.draw(490, 384, 450);  // the budget the manifest aims at
  CHECK(d.rects.size() + d.texts.size() + d.polys.size() <= O3M_VIEW_MAX_CALLS);
  CHECK(!d.polys.empty());
  std::size_t chars = 0;
  for (const auto& t : d.texts) {
    chars += t.s.size();
  }
  CHECK(chars <= O3M_VIEW_MAX_TEXT);
  (void)a.draw(480, 288);
  (void)a.draw(800, 480);
  (void)a.draw(16, 16);
  (void)a.draw(100, 100);
  // wall-clock of a frame on this host (sanitised debug build in CI: only a coarse indication; the unit's figure is owed)
  for (const std::uint32_t view : {1u, 2u}) {
    Scope b;
    NEED(b.ok);
    b.set(kView, static_cast<float>(view));
    double worst = 0.0, total = 0.0;
    const int frames = 100;
    for (int i = 0; i < frames; ++i) {
      b.api.now += 33;
      b.window(w, static_cast<std::uint64_t>(i) * 1456u);
      const auto t0 = std::chrono::steady_clock::now();
      (void)b.draw();
      const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
      worst = std::max(worst, us);
      total += us;
    }
    MESSAGE(std::string(view == 1u ? "scope" : "spectrum") << " view_draw on this host: mean " << total / frames << " us, worst " << worst << " us per 33 ms frame");
    CHECK(worst < 200000.0);
  }
}

TEST_CASE("scope: the packed .tmod passes the loader's checks, carries the audio_tap capability, has no hot code and is small") {
  const Tmod t = load_tmod(O3M_SCOPE_TMOD);
  if (!t.present) {
    return;
  }
  CHECK(t.header == Status::Ok);
  CHECK(t.crc_ok);
  CHECK(t.relocation == Status::Ok);
  CHECK(t.descriptor == Status::Ok);
  CHECK(t.h.kind == O3M_KIND_APP);
  CHECK(t.h.abi_minor == 10u);
  CHECK(t.h.reserved2[0] == O3M_CAP_AUDIO_TAP);
  CHECK(t.h.reserved2[1] == 0u);
  CHECK(t.h.module_version == O3M_VERSION(1, 0, 0));
  CHECK(t.h.param_count == 3u);
  CHECK(t.h.view_count == 1u);
  CHECK(t.h.hot_size == 0u);
  CHECK(t.h.state_bytes <= 20000u);
  CHECK(t.file_bytes <= 8192u);
  MESSAGE("scope.tmod: " << t.file_bytes << " B file, hot " << t.h.hot_size << " B, cold " << (t.h.image_size - t.h.hot_size) << " B, state " << t.h.state_bytes << " B");
}

TEST_CASE("scope: VIEW, SOURCE and TIMEBASE are the page's three rows: Up / Down, the encoder, Shift + the encoder, Delete (the default), each kept with set_own_param (OS-APPUX S3)") {
  Scope a;
  NEED(a.ok);
  CHECK(a.inst.m->ui->input != nullptr);
  Drawing d = a.draw();
  CHECK(row_text(d, "VIEW") == "SCOPE");
  CHECK(row_text(d, "SOURCE") == "MASTER");
  CHECK(row_text(d, "TIMEBASE") == "3");
  CHECK(a.api.own_params.empty());
  a.event(O3M_EV_ENCODER, 0, 1);  // VIEW: SPECTRUM
  CHECK(row_text(a.draw(), "VIEW") == "SPECTRUM");
  NEED(a.api.own_params.size() == 1u);
  CHECK((a.api.own_params[0].index == 0u && a.api.own_params[0].value == 2.0f));
  a.event(O3M_EV_BUTTON, O3M_BTN_DOWN, 1);  // SOURCE
  a.event(O3M_EV_ENCODER, 0, -1);
  CHECK(row_text(a.draw(), "SOURCE") == "INPUT");
  CHECK((a.api.own_params.back().index == 1u && a.api.own_params.back().value == 1.0f));
  a.event(O3M_EV_BUTTON, O3M_BTN_DOWN, 1);  // TIMEBASE: 1..5, Shift + the encoder two a detent
  a.event(O3M_EV_ENCODER, 0, 1);
  CHECK(row_text(a.draw(), "TIMEBASE") == "4");
  a.event(O3M_EV_ENCODER, 0, 1, O3M_MOD_SHIFT);
  CHECK(row_text(a.draw(), "TIMEBASE") == "5");  // (stops at the last)
  a.event(O3M_EV_BUTTON, O3M_BTN_DOWN, 1);       // no wrap below the last row
  a.event(O3M_EV_ENCODER, 0, -2, O3M_MOD_SHIFT);
  CHECK(row_text(a.draw(), "TIMEBASE") == "1");
  a.event(O3M_EV_BUTTON, O3M_BTN_DELETE, 1);
  CHECK(row_text(a.draw(), "TIMEBASE") == "3");
  CHECK((a.api.own_params.back().index == 2u && a.api.own_params.back().value == 3.0f));
  a.event(O3M_EV_BUTTON, O3M_BTN_UP, 1);
  a.event(O3M_EV_BUTTON, O3M_BTN_DELETE, 1);  // SOURCE: MASTER
  CHECK(row_text(a.draw(), "SOURCE") == "MASTER");
  a.event(O3M_EV_BUTTON, O3M_BTN_UP, 1);
  a.event(O3M_EV_BUTTON, O3M_BTN_DELETE, 1);  // VIEW: SCOPE
  CHECK(row_text(a.draw(), "VIEW") == "SCOPE");
  // the OS hands the kept rows back before enter(), which leaves them alone
  a.set(0, 2.0f);
  a.set(1, 1.0f);
  a.set(2, 5.0f);
  a.inst.m->ui->enter(a.inst.state);
  d = a.draw();
  CHECK((row_text(d, "VIEW") == "SPECTRUM" && row_text(d, "SOURCE") == "INPUT" && row_text(d, "TIMEBASE") == "5"));
}
