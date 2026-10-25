// SPDX-License-Identifier: GPL-3.0-or-later
//
// The real FFT (ui/plot/fft.h) against a DFT oracle in double precision, and the spectrum analyser built on it
// (ui/plot/spectrum.h): a sine of full scale reads 0 dB on its bar, bars are log-spaced, smoothing and peak hold behave,
// and drawing is bounded in words.
#include <doctest/doctest.h>

#include <o3os/eve/display_list_builder.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/ui/plot/fft.h>
#include <o3os/ui/plot/spectrum.h>
#include <o3os/ui/renderer.h>
#include <o3os/ui/theme.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

using namespace o3os;
using namespace o3os::ui::plot;

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Rng {
  std::uint32_t s = 88172645u;
  std::uint32_t next() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  }
  float unit() { return static_cast<float>(next() >> 8) / 16777216.0f * 2.0f - 1.0f; }
};

// X[k] = sum x[m] e^(-2 pi i k m / n), in the CMSIS layout: [Re0, ReN/2, Re1, Im1, Re2, Im2, ...].
std::vector<double> dft_packed(const std::vector<float>& x) {
  const std::size_t n = x.size();
  std::vector<double> out(n);
  for (std::size_t k = 0; k <= n / 2; ++k) {
    std::complex<double> acc = 0.0;
    for (std::size_t m = 0; m < n; ++m) {
      acc += static_cast<double>(x[m]) * std::exp(std::complex<double>(0.0, -2.0 * kPi * static_cast<double>((k * m) % n) / static_cast<double>(n)));
    }
    if (k == 0) {
      out[0] = acc.real();
    } else if (k == n / 2) {
      out[1] = acc.real();
    } else {
      out[2 * k] = acc.real();
      out[2 * k + 1] = acc.imag();
    }
  }
  return out;
}

double worst_error(const std::vector<float>& x, unsigned n) {
  RealFft fft;
  std::vector<float> out(n);
  CHECK(fft.init(n));
  fft.forward(x.data(), out.data());
  const std::vector<double> want = dft_packed(x);
  double scale = 1e-9, worst = 0.0;
  for (const double w : want) {
    scale = std::fabs(w) > scale ? std::fabs(w) : scale;
  }
  for (unsigned i = 0; i < n; ++i) {
    const double e = std::fabs(static_cast<double>(out[i]) - want[i]) / scale;
    worst = e > worst ? e : worst;
  }
  return worst;
}

}  // namespace

TEST_CASE("plot fft: equals a double-precision DFT for every size, on noise") {
  Rng rng;
  for (const unsigned n : {16u, 32u, 64u, 128u, 256u, 512u}) {
    std::vector<float> x(n);
    for (float& v : x) {
      v = rng.unit();
    }
    CHECK_MESSAGE(worst_error(x, n) < 2e-6, "n = " << n);
  }
}

TEST_CASE("plot fft: impulse, DC, a bin-centred sine and a Nyquist alternation") {
  for (const unsigned n : {256u, 512u}) {
    std::vector<float> impulse(n, 0.0f), dc(n, 1.0f), sine(n), nyq(n);
    impulse[0] = 1.0f;
    for (unsigned m = 0; m < n; ++m) {
      sine[m] = static_cast<float>(std::sin(2.0 * kPi * 5.0 * m / n));
      nyq[m] = (m % 2 == 0) ? 1.0f : -1.0f;
    }
    CHECK(worst_error(impulse, n) < 1e-6);
    CHECK(worst_error(dc, n) < 1e-6);
    CHECK(worst_error(sine, n) < 2e-6);
    CHECK(worst_error(nyq, n) < 1e-6);
    // spot values: the impulse is flat 1 in every bin; DC puts n in bin 0; the sine puts n/2 in bin 5's imaginary part (-n/2)
    RealFft fft;
    std::vector<float> out(n);
    CHECK(fft.init(n));
    fft.forward(impulse.data(), out.data());
    CHECK(out[0] == doctest::Approx(1.0f));
    CHECK(out[1] == doctest::Approx(1.0f));
    CHECK(out[2 * 7] == doctest::Approx(1.0f));
    CHECK(std::fabs(out[2 * 7 + 1]) < 1e-6f);
    fft.forward(dc.data(), out.data());
    CHECK(out[0] == doctest::Approx(static_cast<float>(n)));
    fft.forward(sine.data(), out.data());
    CHECK(out[2 * 5 + 1] == doctest::Approx(-static_cast<float>(n) / 2.0f).epsilon(1e-5));
    CHECK(std::fabs(out[2 * 5]) < 1e-3f);
    fft.forward(nyq.data(), out.data());
    CHECK(out[1] == doctest::Approx(static_cast<float>(n)));
  }
}

TEST_CASE("plot fft: bad sizes are refused, the input is left alone") {
  RealFft fft;
  for (const unsigned n : {0u, 1u, 8u, 15u, 17u, 100u, 255u, 513u, 1024u}) {
    CHECK_FALSE(fft.init(n));
    CHECK(fft.size() == 0);
  }
  CHECK(fft.init(64));
  CHECK(fft.size() == 64);
  std::vector<float> in(64, 0.5f), keep = in, out(64);
  fft.forward(in.data(), out.data());
  CHECK(in == keep);
  RealFft unset;  // forward() on an uninitialised object does nothing
  unset.forward(in.data(), out.data());
}

TEST_CASE("plot spectrum: a full-scale sine reads 0 dB on its bar, a half-scale one -6 dB, silence the floor") {
  Spectrum::Config cfg;
  cfg.bars = 48;
  Spectrum s;
  CHECK(s.init(cfg));
  std::vector<std::int16_t> block(512);
  const auto fill_sine = [&](double hz, double amp) {
    for (unsigned i = 0; i < 512; ++i) {
      block[i] = static_cast<std::int16_t>(std::lround(amp * 32767.0 * std::sin(2.0 * kPi * hz * i / 44100.0)));
    }
  };
  // a bin-centred tone (bin 40 = 3445.3 Hz) is the best case; a tone between two bins reads lower by the scalloping loss
  const double bin40 = 40.0 * 44100.0 / 512.0;
  fill_sine(bin40, 1.0);
  s.analyze(block.data(), 1);
  float loudest = -200.0f;
  unsigned at = 0;
  for (unsigned b = 0; b < s.bars(); ++b) {
    if (s.raw_db(b) > loudest) {
      loudest = s.raw_db(b);
      at = b;
    }
  }
  CHECK(loudest == doctest::Approx(0.0f).epsilon(0.02).scale(1.0));  // within 0.05 dB
  CHECK(std::fabs(s.bar_center_hz(at) - static_cast<float>(bin40)) < static_cast<float>(bin40) * 0.15f);  // the right bar
  fill_sine(bin40, 0.5);
  s.analyze(block.data(), 1);
  float half = -200.0f;
  for (unsigned b = 0; b < s.bars(); ++b) {
    half = s.raw_db(b) > half ? s.raw_db(b) : half;
  }
  CHECK(half == doctest::Approx(-6.02f).epsilon(0.01));
  // far from the tone the bars fall to the window's leakage: Hann's sidelobes are below -31 dB and fall 18 dB an octave
  float far = -200.0f;
  for (unsigned b = 0; b < s.bars(); ++b) {
    if (std::fabs(s.bar_center_hz(b) - static_cast<float>(bin40)) > 1500.0f) {
      far = s.raw_db(b) > far ? s.raw_db(b) : far;
    }
  }
  CHECK(far < -50.0f);
  // silence is the floor on every bar
  std::fill(block.begin(), block.end(), static_cast<std::int16_t>(0));
  s.analyze(block.data(), 1);
  for (unsigned b = 0; b < s.bars(); ++b) {
    CHECK(s.raw_db(b) == cfg.floor_db);
  }
  // stereo: two equal channels read as the mono signal, opposite channels cancel
  std::vector<std::int16_t> st(1024);
  fill_sine(bin40, 1.0);
  for (unsigned i = 0; i < 512; ++i) {
    st[2 * i] = block[i];
    st[2 * i + 1] = block[i];
  }
  s.analyze(st.data(), 2);
  float both = -200.0f;
  for (unsigned b = 0; b < s.bars(); ++b) {
    both = s.raw_db(b) > both ? s.raw_db(b) : both;
  }
  CHECK(both == doctest::Approx(0.0f).epsilon(0.02).scale(1.0));
  for (unsigned i = 0; i < 512; ++i) {
    st[2 * i + 1] = static_cast<std::int16_t>(-block[i]);
  }
  s.analyze(st.data(), 2);
  for (unsigned b = 0; b < s.bars(); ++b) {
    CHECK(s.raw_db(b) == cfg.floor_db);
  }
}

TEST_CASE("plot spectrum: bars are log spaced, the bar-to-bin map covers 86 Hz to 16 kHz, low bars are interpolated") {
  Spectrum::Config cfg;
  cfg.bars = 48;
  Spectrum s;
  CHECK(s.init(cfg));
  float prev = 0.0f;
  for (unsigned b = 0; b < s.bars(); ++b) {
    const float f = s.bar_center_hz(b);
    CHECK(f > prev);
    if (b > 0) {
      CHECK(f / prev == doctest::Approx(std::pow(cfg.f_hi / cfg.f_lo, 1.0f / 48.0f)).epsilon(1e-4));  // a constant ratio
    }
    prev = f;
  }
  CHECK(s.bar_center_hz(0) > cfg.f_lo);
  CHECK(s.bar_center_hz(47) < cfg.f_hi);
  CHECK(s.bar_center_hz(48) == 0.0f);
  // a tone sweeping across the lowest bars moves smoothly over them rather than stepping from one to the next
  std::vector<std::int16_t> block(512);
  float last = -200.0f;
  float biggest_jump = 0.0f;
  for (float hz = 90.0f; hz < 260.0f; hz += 8.0f) {
    for (unsigned i = 0; i < 512; ++i) {
      block[i] = static_cast<std::int16_t>(std::lround(32000.0 * std::sin(2.0 * kPi * static_cast<double>(hz) * i / 44100.0)));
    }
    s.analyze(block.data(), 1);
    float v = -200.0f;
    for (unsigned b = 0; b < 4; ++b) {
      v = s.raw_db(b) > v ? s.raw_db(b) : v;
    }
    if (last > -190.0f) {
      const float jump = v > last ? v - last : last - v;
      biggest_jump = jump > biggest_jump ? jump : biggest_jump;
    }
    last = v;
  }
  CHECK(biggest_jump < 9.0f);
  // invalid configurations are refused
  Spectrum::Config bad = cfg;
  bad.fft_size = 100;
  CHECK_FALSE(Spectrum{}.init(bad));
  bad = cfg;
  bad.bars = 0;
  CHECK_FALSE(Spectrum{}.init(bad));
  bad = cfg;
  bad.bars = Spectrum::kMaxBars + 1;
  CHECK_FALSE(Spectrum{}.init(bad));
  bad = cfg;
  bad.f_hi = 30000.0f;
  CHECK_FALSE(Spectrum{}.init(bad));
  bad = cfg;
  bad.f_lo = bad.f_hi;
  CHECK_FALSE(Spectrum{}.init(bad));
  bad = cfg;
  bad.ceil_db = bad.floor_db;
  CHECK_FALSE(Spectrum{}.init(bad));
  Spectrum none;
  none.analyze(block.data(), 1);  // not ready: nothing happens
  CHECK_FALSE(none.ready());
}

TEST_CASE("plot spectrum: the lowest bar starts at one FFT bin: the default config is valid, less is refused, no two bars copy a bin") {
  // 512 points at 44.1 kHz: a bin is 86.13 Hz. The default starts there; a bar below it can only repeat bin 1 (with 30 Hz the
  // first eight bars of 48 did), so the configuration is refused.
  Spectrum::Config def;
  CHECK(def.f_lo == Spectrum::bin_hz(512, 44100.0f));
  Spectrum s;
  CHECK(s.init(def));
  Spectrum::Config low = def;
  low.f_lo = 30.0f;
  CHECK_FALSE(s.init(low));
  CHECK_FALSE(s.ready());
  low.f_lo = Spectrum::bin_hz(512, 44100.0f) * 0.99f;
  CHECK_FALSE(s.init(low));
  low.f_lo = Spectrum::bin_hz(512, 44100.0f);
  CHECK(s.init(low));
  // another size or rate moves the bin: 256 points is 172 Hz, 48 kHz at 512 is 93.75 Hz; the old default is then too low
  Spectrum::Config c256 = def;
  c256.fft_size = 256;
  CHECK_FALSE(s.init(c256));
  c256.f_lo = Spectrum::bin_hz(256, 44100.0f);
  CHECK(s.init(c256));
  Spectrum::Config c48 = def;
  c48.sample_rate = 48000.0f;
  CHECK_FALSE(s.init(c48));
  c48.f_lo = Spectrum::bin_hz(512, 48000.0f);
  CHECK(c48.f_lo == doctest::Approx(93.75f));
  CHECK(s.init(c48));
  Spectrum::Config no_rate = def;
  no_rate.sample_rate = 0.0f;
  CHECK_FALSE(s.init(no_rate));
  // With the default, a tone swept over the lowest bars gives each its own level: no two of the first eight bars read the same
  // for every tone (a copied bin would make them equal for all of them)
  CHECK(s.init(def));
  std::vector<std::int16_t> block(512);
  unsigned pairs_equal = 0, pairs = 0;
  for (float hz = 90.0f; hz < 700.0f; hz += 23.0f) {
    for (unsigned i = 0; i < 512; ++i) {
      block[i] = static_cast<std::int16_t>(std::lround(30000.0 * std::sin(2.0 * kPi * static_cast<double>(hz) * i / 44100.0)));
    }
    s.analyze(block.data(), 1);
    for (unsigned b = 0; b + 1 < 8; ++b) {
      ++pairs;
      pairs_equal += s.raw_db(b) == s.raw_db(b + 1) ? 1u : 0u;
    }
  }
  CHECK(pairs_equal * 4 < pairs);  // most neighbours differ; equal ones are the floor (both far from the tone)
}

TEST_CASE("plot spectrum: smoothing attacks fast and releases slowly, the peak is held for a second and then falls") {
  Spectrum::Config cfg;
  Spectrum s;
  CHECK(s.init(cfg));
  std::vector<std::int16_t> loud(512), quiet(512, 0);
  const double bin = 40.0 * 44100.0 / 512.0;
  for (unsigned i = 0; i < 512; ++i) {
    loud[i] = static_cast<std::int16_t>(std::lround(32000.0 * std::sin(2.0 * kPi * bin * i / 44100.0)));
  }
  unsigned bar = 0;
  s.analyze(loud.data(), 1);
  for (unsigned b = 0; b < s.bars(); ++b) {
    bar = s.raw_db(b) > s.raw_db(bar) ? b : bar;
  }
  const float target = s.raw_db(bar);
  // the level approaches the target by kAttack of the way each call
  const float first = s.level_db(bar);
  CHECK(first == doctest::Approx(cfg.floor_db + (target - cfg.floor_db) * Spectrum::kAttack).epsilon(1e-4).scale(1.0));
  for (int i = 0; i < 12; ++i) {
    s.analyze(loud.data(), 1);
  }
  CHECK(s.level_db(bar) == doctest::Approx(target).epsilon(1e-3).scale(1.0));
  CHECK(s.peak_db(bar) >= s.level_db(bar) - 1e-4f);
  const float peak = s.peak_db(bar);
  // silence: the level releases by kRelease of the way down per call; the peak holds for kHoldCalls calls ...
  s.analyze(quiet.data(), 1);
  CHECK(s.level_db(bar) < target);
  CHECK(s.level_db(bar) == doctest::Approx(target + (cfg.floor_db - target) * Spectrum::kRelease).epsilon(1e-3).scale(1.0));
  for (unsigned i = 1; i < Spectrum::kHoldCalls; ++i) {
    s.analyze(quiet.data(), 1);
    CHECK(s.peak_db(bar) == peak);  // held
  }
  // ... and then falls kPeakFallDb a call (the 21st call is the first that falls), down to the level and no further
  s.analyze(quiet.data(), 1);
  CHECK(s.peak_db(bar) == doctest::Approx(peak - Spectrum::kPeakFallDb).epsilon(1e-3).scale(1.0));
  s.analyze(quiet.data(), 1);
  CHECK(s.peak_db(bar) == doctest::Approx(peak - 2.0f * Spectrum::kPeakFallDb).epsilon(1e-3).scale(1.0));
  for (int i = 0; i < 400; ++i) {
    s.analyze(quiet.data(), 1);
  }
  CHECK(s.peak_db(bar) == doctest::Approx(cfg.floor_db).epsilon(1e-3).scale(1.0));
  CHECK(s.level_db(bar) >= cfg.floor_db - 1e-3f);
  s.reset();
  CHECK(s.peak_db(bar) == cfg.floor_db);
  CHECK(s.level_db(99) == cfg.floor_db);  // no such bar
}

TEST_CASE("plot spectrum: drawing is bounded in words, bars and caps are where the levels say") {
  Spectrum::Config cfg;
  cfg.bars = 64;
  Spectrum s;
  CHECK(s.init(cfg));
  Rng rng;
  std::vector<std::int16_t> noise(512);
  for (int pass = 0; pass < 30; ++pass) {
    for (std::int16_t& v : noise) {
      v = static_cast<std::int16_t>(rng.unit() * 30000.0f);
    }
    s.analyze(noise.data(), 1);
  }
  const ui::Theme& theme = ui::theme(ui::ThemeId::Amiga);
  const auto words_of = [&](SpectrumShape shape, bool caps, ui::Rect area) {
    platform::host::FakeDisplay sink;
    std::vector<std::uint32_t> storage(eve::kMaxDisplayListWords * 2);
    eve::DisplayListBuilder builder(sink, storage.data(), storage.size());
    ui::Renderer r(builder, theme);
    CHECK(r.begin(ui::Color::Pane));
    const std::size_t before = r.words();
    SpectrumStyle st;
    st.shape = shape;
    st.caps = caps;
    draw_spectrum(r, area, s, st);
    const std::size_t n = r.words() - before;
    CHECK(r.end());
    return n;
  };
  const std::size_t bars = words_of(SpectrumShape::Bars, true, {56, 40, 704, 250});
  CHECK(bars <= spectrum_max_words(64));
  CHECK(bars >= 2u * 64u);  // at least a rectangle each
  const std::size_t fill = words_of(SpectrumShape::Fill, true, {56, 40, 704, 250});
  CHECK(fill <= spectrum_max_words(64));
  CHECK(words_of(SpectrumShape::Bars, false, {56, 40, 704, 250}) < bars);
  CHECK(words_of(SpectrumShape::Bars, true, {56, 40, 0, 10}) == 0);  // nothing to draw into
  // an unprepared spectrum draws nothing
  Spectrum none;
  platform::host::FakeDisplay sink;
  std::vector<std::uint32_t> storage(eve::kMaxDisplayListWords);
  eve::DisplayListBuilder builder(sink, storage.data(), storage.size());
  ui::Renderer r(builder, theme);
  CHECK(r.begin(ui::Color::Pane));
  const std::size_t before = r.words();
  draw_spectrum(r, {0, 0, 100, 100}, none, SpectrumStyle{});
  CHECK(r.words() == before);
}
