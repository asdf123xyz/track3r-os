// SPDX-License-Identifier: GPL-3.0-or-later
//
// The pure math under the plots (ui/plot/plot_math.h): axis mapping (linear, log-frequency, dB), decibel conversions, the
// biquad magnitude response against a complex-arithmetic oracle (and against the textbook values of an RBJ filter), tick
// placement and the tick labels.
#include <doctest/doctest.h>

#include <o3os/ui/plot/plot_math.h>

#include <cmath>
#include <complex>
#include <limits>
#include <cstring>
#include <string>
#include <vector>

using namespace o3os::ui::plot;

namespace {

constexpr double kPi = 3.14159265358979323846;

// |H(e^jw)|^2 by evaluating the polynomials in double complex arithmetic: the oracle for biquad_mag2.
double oracle_mag2(const Biquad& f, double w) {
  const std::complex<double> z1 = std::exp(std::complex<double>(0.0, -w));
  const std::complex<double> z2 = z1 * z1;
  const std::complex<double> num = static_cast<double>(f.b0) + static_cast<double>(f.b1) * z1 + static_cast<double>(f.b2) * z2;
  const std::complex<double> den = 1.0 + static_cast<double>(f.a1) * z1 + static_cast<double>(f.a2) * z2;
  return std::norm(num) / std::norm(den);
}

// RBJ audio-EQ-cookbook filters (the tests build their own coefficients: the library only plots them).
Biquad rbj_lowpass(double fs, double f0, double q) {
  const double w0 = 2.0 * kPi * f0 / fs, alpha = std::sin(w0) / (2.0 * q), c = std::cos(w0);
  const double a0 = 1.0 + alpha;
  return Biquad{static_cast<float>((1.0 - c) / 2.0 / a0), static_cast<float>((1.0 - c) / a0), static_cast<float>((1.0 - c) / 2.0 / a0),
                static_cast<float>(-2.0 * c / a0), static_cast<float>((1.0 - alpha) / a0)};
}
Biquad rbj_peaking(double fs, double f0, double q, double gain_db) {
  const double a = std::pow(10.0, gain_db / 40.0), w0 = 2.0 * kPi * f0 / fs, alpha = std::sin(w0) / (2.0 * q), c = std::cos(w0);
  const double a0 = 1.0 + alpha / a;
  return Biquad{static_cast<float>((1.0 + alpha * a) / a0), static_cast<float>(-2.0 * c / a0), static_cast<float>((1.0 - alpha * a) / a0),
                static_cast<float>(-2.0 * c / a0), static_cast<float>((1.0 - alpha / a) / a0)};
}

std::string fmt(unsigned (*f)(float, char*), float v) {
  char buf[16];
  const unsigned n = f(v, buf);
  CHECK(n == std::strlen(buf));
  return buf;
}

}  // namespace

TEST_CASE("plot math: axes map their ends to 0 and 1, and map back") {
  const AxisSpec lin{AxisKind::Linear, -2.0f, 6.0f}, db{AxisKind::Db, -24.0f, 24.0f}, hz{AxisKind::LogFreq, 20.0f, 20000.0f};
  for (const AxisSpec* a : {&lin, &db, &hz}) {
    CHECK(axis_t(*a, a->lo) == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(axis_t(*a, a->hi) == doctest::Approx(1.0f).epsilon(1e-6));
    for (const float t : {0.0f, 0.1f, 0.25f, 0.5f, 0.77f, 1.0f, -0.2f, 1.3f}) {
      CHECK(axis_t(*a, axis_value(*a, t)) == doctest::Approx(t).epsilon(1e-4).scale(1.0));
    }
  }
  CHECK(axis_t(lin, 2.0f) == doctest::Approx(0.5f));
  CHECK(axis_t(db, 0.0f) == doctest::Approx(0.5f));
  // log: the geometric mean is the middle, every decade is a third of 20 .. 20000 Hz
  CHECK(axis_t(hz, std::sqrt(20.0f * 20000.0f)) == doctest::Approx(0.5f).epsilon(1e-5));
  CHECK(axis_t(hz, 200.0f) == doctest::Approx(1.0f / 3.0f).epsilon(1e-5));
  CHECK(axis_t(hz, 2000.0f) == doctest::Approx(2.0f / 3.0f).epsilon(1e-5));
  CHECK(axis_value(hz, 0.5f) == doctest::Approx(632.455f).epsilon(1e-4));
  CHECK(axis_t(hz, 0.0f) < 0.0f);   // off the low end, not a NaN
  CHECK(axis_t(hz, -5.0f) < 0.0f);
  // degenerate axes map everything to 0 instead of dividing by zero
  CHECK(axis_t(AxisSpec{AxisKind::Linear, 1.0f, 1.0f}, 3.0f) == 0.0f);
  CHECK(axis_t(AxisSpec{AxisKind::Linear, 5.0f, 1.0f}, 3.0f) == 0.0f);
  CHECK(axis_t(AxisSpec{AxisKind::LogFreq, 0.0f, 100.0f}, 3.0f) == 0.0f);
  CHECK(axis_t(AxisSpec{AxisKind::LogFreq, -1.0f, 100.0f}, 3.0f) == 0.0f);
}

TEST_CASE("plot math: exp2_fast and log2_fast, the libm-free logarithm and exponential, against libm in double precision") {
  // exp2: relative error under 2e-7 over the whole range of a float, exact at the integers
  double worst_rel = 0.0;
  for (double x = -125.9; x < 127.0; x += 0.0137) {
    const float xf = static_cast<float>(x);  // the argument the function gets: its rounding is not the function's error
    const double want = std::exp2(static_cast<double>(xf));
    const double got = static_cast<double>(exp2_fast(xf));
    const double rel = std::fabs(got - want) / want;
    worst_rel = rel > worst_rel ? rel : worst_rel;
  }
  CHECK(worst_rel < 2.0e-7);
  CHECK(exp2_fast(-126.0f) == std::numeric_limits<float>::min());  // the smallest normal float is reachable
  for (int n = -126; n <= 127; ++n) {
    CHECK(exp2_fast(static_cast<float>(n)) == static_cast<float>(std::ldexp(1.0, n)));
  }
  CHECK(exp2_fast(0.0f) == 1.0f);
  CHECK(exp2_fast(0.5f) == doctest::Approx(1.41421356f).epsilon(1e-6));
  CHECK(exp2_fast(-200.0f) == 0.0f);
  CHECK(exp2_fast(std::nanf("")) == 0.0f);
  CHECK(exp2_fast(1000.0f) == exp2_fast(127.0f));
  CHECK(exp2_fast(-std::numeric_limits<float>::infinity()) == 0.0f);
  // log2: error under 1e-6 plus 2e-7 of the result (a float's own rounding) for every normal float
  double worst_abs = 0.0;
  for (double e = -125.0; e < 127.0; e += 0.37) {
    for (const double m : {1.0, 1.0000001, 1.2, 1.41421, 1.4143, 1.7, 1.9999}) {
      const float x = static_cast<float>(m * std::exp2(e));
      const double want = std::log2(static_cast<double>(x));
      const double err = std::fabs(static_cast<double>(log2_fast(x)) - want);
      worst_abs = err > worst_abs ? err : worst_abs;
      CHECK(err <= 1.0e-6 + 2.0e-7 * std::fabs(want));
    }
  }
  CHECK(worst_abs < 4.0e-6);  // the largest are float roundings of results near +-127
  CHECK(log2_fast(1.0f) == 0.0f);
  CHECK(log2_fast(2.0f) == 1.0f);
  CHECK(log2_fast(0.5f) == -1.0f);
  CHECK(log2_fast(1024.0f) == 10.0f);
  CHECK(log2_fast(0.0f) == -126.0f);
  CHECK(log2_fast(-3.0f) == -126.0f);
  CHECK(log2_fast(std::nanf("")) == -126.0f);
  CHECK(log2_fast(1.0e-45f) == -126.0f);  // a denormal
  CHECK(log2_fast(std::numeric_limits<float>::infinity()) == 128.0f);
  // and they invert one another
  for (double x = -100.0; x < 100.0; x += 0.731) {
    CHECK(static_cast<double>(log2_fast(exp2_fast(static_cast<float>(x)))) == doctest::Approx(x).epsilon(1e-5).scale(1.0));
  }
}

TEST_CASE("plot math: decibels") {
  CHECK(db_from_amp(1.0f) == doctest::Approx(0.0f).epsilon(1e-6).scale(1.0));
  CHECK(db_from_amp(0.5f) == doctest::Approx(-6.0206f).epsilon(1e-4));
  CHECK(db_from_amp(2.0f) == doctest::Approx(6.0206f).epsilon(1e-4));
  CHECK(db_from_amp(0.001f) == doctest::Approx(-60.0f).epsilon(1e-4));
  CHECK(db_from_amp(0.0f) == kMinDb);
  CHECK(db_from_amp(-1.0f) == kMinDb);
  CHECK(db_from_power(0.0f) == kMinDb);
  CHECK(db_from_power(0.25f) == doctest::Approx(-6.0206f).epsilon(1e-4));
  for (const float db : {-90.0f, -60.0f, -12.0f, -6.0f, 0.0f, 3.0f, 12.0f}) {
    CHECK(db_from_amp(amp_from_db(db)) == doctest::Approx(db).epsilon(1e-4).scale(1.0));
  }
  CHECK(amp_from_db(-6.0206f) == doctest::Approx(0.5f).epsilon(1e-4));
}

TEST_CASE("plot math: the biquad magnitude equals the complex-arithmetic oracle, and the textbook values") {
  const double fs = 44100.0;
  // random stable sections: poles inside the unit circle (|a2| < 1, |a1| < 1 + a2)
  unsigned seed = 12345;
  const auto rnd = [&]() {
    seed = seed * 1664525u + 1013904223u;
    return static_cast<double>(seed >> 8) / 16777216.0;
  };
  double worst = 0.0;
  for (int i = 0; i < 200; ++i) {
    Biquad f;
    f.a2 = static_cast<float>(rnd() * 1.8 - 0.9);
    f.a1 = static_cast<float>((rnd() * 2.0 - 1.0) * (1.0 + static_cast<double>(f.a2)) * 0.95);
    f.b0 = static_cast<float>(rnd() * 2.0 - 1.0);
    f.b1 = static_cast<float>(rnd() * 2.0 - 1.0);
    f.b2 = static_cast<float>(rnd() * 2.0 - 1.0);
    for (int k = 0; k < 30; ++k) {
      const double w = kPi * (static_cast<double>(k) + 0.5) / 30.0;
      const double want = oracle_mag2(f, w);
      const double s = std::sin(w / 2.0);
      const double got = static_cast<double>(biquad_mag2(f, static_cast<float>(s * s)));
      const double err = std::fabs(got - want) / (want + 1e-3);
      worst = err > worst ? err : worst;
    }
  }
  CHECK(worst < 1e-3);
  // a Butterworth lowpass (Q = 1/sqrt 2) is -3.01 dB at its corner, 0 dB at DC, -12 dB an octave above and falling
  const Biquad lp = rbj_lowpass(fs, 1000.0, 0.70710678);
  const auto response = [&](const Biquad& f, double hz) {
    const double s = std::sin(kPi * hz / fs);
    return biquad_db(f, static_cast<float>(s * s));
  };
  CHECK(response(lp, 1000.0) == doctest::Approx(-3.01f).epsilon(0.02));
  CHECK(response(lp, 20.0) == doctest::Approx(0.0f).epsilon(1e-2).scale(1.0));
  CHECK(response(lp, 2000.0) == doctest::Approx(-12.3f).epsilon(0.05));  // the bilinear transform warps the slope a little
  CHECK(response(lp, 10000.0) < -35.0f);
  // a resonant lowpass peaks above its corner by about Q
  const Biquad res = rbj_lowpass(fs, 1000.0, 4.0);
  float peak = -100.0f;
  for (double hz = 100.0; hz < 3000.0; hz *= 1.01) {
    const float v = response(res, hz);
    peak = v > peak ? v : peak;
  }
  CHECK(peak == doctest::Approx(12.0f).epsilon(0.05));  // 20 log10(4) = 12.04 dB
  // a peaking section has its gain at its centre and nothing far away
  const Biquad pk = rbj_peaking(fs, 2000.0, 1.0, 9.0);
  CHECK(response(pk, 2000.0) == doctest::Approx(9.0f).epsilon(0.01));
  CHECK(response(pk, 30.0) == doctest::Approx(0.0f).epsilon(0.02).scale(1.0));
  CHECK(response(pk, 20000.0) == doctest::Approx(0.0f).epsilon(0.2).scale(1.0));
  // a pole on the unit circle has no finite answer: the floor, not a NaN
  const Biquad pole{1.0f, 0.0f, 0.0f, -2.0f, 1.0f};  // 1 / (1 - z^-1)^2: infinite at DC
  const float at_dc = biquad_db(pole, 0.0f);
  CHECK(at_dc == kMinDb);
  CHECK(std::isfinite(at_dc));
}

TEST_CASE("plot math: response_basis gives sin^2(w/2) at evenly spread frequencies, the response uses it") {
  const AxisSpec hz{AxisKind::LogFreq, 20.0f, 20000.0f};
  constexpr unsigned kN = 64;
  float phi[kN], db[kN];
  response_basis(hz, 44100.0f, kN, phi);
  for (unsigned i = 0; i < kN; ++i) {
    const double f = 20.0 * std::pow(1000.0, static_cast<double>(i) / (kN - 1));
    const double s = std::sin(kPi * f / 44100.0);
    CHECK(phi[i] == doctest::Approx(s * s).epsilon(1e-4));
  }
  const Biquad lp = rbj_lowpass(44100.0, 1000.0, 0.70710678);
  biquad_response_db(lp, phi, kN, db);
  for (unsigned i = 0; i < kN; ++i) {
    CHECK(db[i] == biquad_db(lp, phi[i]));
  }
  CHECK(db[0] == doctest::Approx(0.0f).epsilon(1e-2).scale(1.0));
  CHECK(db[kN - 1] < -50.0f);
  // a linear axis spreads evenly: DC, a quarter of the sample rate (phi = 1/2), Nyquist (phi = 1)
  const AxisSpec lin{AxisKind::Linear, 0.0f, 22050.0f};
  float l[3];
  response_basis(lin, 44100.0f, 3, l);
  CHECK(l[0] == 0.0f);
  CHECK(l[1] == doctest::Approx(0.5f).epsilon(1e-5));
  CHECK(l[2] == doctest::Approx(1.0f).epsilon(1e-5));
  response_basis(lin, 44100.0f, 0, l);  // nothing to do
}

TEST_CASE("plot math: a lowpass is smooth down to a few Hz: single precision does not cancel at low frequencies") {
  // The case that motivated the form in phi: a 1.2 kHz resonant lowpass at 44.1 kHz, plotted from 20 Hz. Every column of the
  // 384-point response is within 0.05 dB of the double-precision answer, and neighbouring columns differ smoothly.
  const AxisSpec hz{AxisKind::LogFreq, 20.0f, 20000.0f};
  constexpr unsigned kN = 384;
  float phi[kN], db[kN];
  response_basis(hz, 44100.0f, kN, phi);
  const Biquad lp = rbj_lowpass(44100.0, 1200.0, 3.5);
  biquad_response_db(lp, phi, kN, db);
  double worst = 0.0;
  float jitter = 0.0f;
  for (unsigned i = 0; i < kN; ++i) {
    const double f = 20.0 * std::pow(1000.0, static_cast<double>(i) / (kN - 1));
    const double want = 10.0 * std::log10(oracle_mag2(lp, 2.0 * kPi * f / 44100.0));
    const double err = std::fabs(static_cast<double>(db[i]) - want);
    worst = err > worst ? err : worst;
    if (i > 0 && i < 200) {  // below 1 kHz the curve is flat to a fraction of a dB: no column jumps
      const float step = db[i] > db[i - 1] ? db[i] - db[i - 1] : db[i - 1] - db[i];
      jitter = step > jitter ? step : jitter;
    }
  }
  CHECK(worst < 0.05);
  CHECK(jitter < 0.2f);
}

TEST_CASE("plot math: ticks") {
  Tick t[32];
  // log frequency, 20 Hz .. 20 kHz: the 1s are major, 2s and 5s minor when there is room for them
  const AxisSpec hz{AxisKind::LogFreq, 20.0f, 20000.0f};
  unsigned n = make_ticks(hz, 12, t, 16);
  std::vector<float> values;
  for (unsigned i = 0; i < n; ++i) {
    values.push_back(t[i].value);
    const double lg = std::log10(static_cast<double>(t[i].value));
    CHECK(t[i].major == (std::fabs(lg - std::round(lg)) < 1e-4));
    if (i > 0) {
      CHECK(t[i].value > t[i - 1].value);
    }
  }
  const std::vector<float> want = {20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000};
  CHECK(values == want);
  // not enough room: only the decades
  n = make_ticks(hz, 4, t, 16);
  CHECK(n == 3);
  CHECK((t[0].value == 100.0f && t[1].value == 1000.0f && t[2].value == 10000.0f));
  for (unsigned i = 0; i < n; ++i) {
    CHECK(t[i].major);
  }
  // a cap is a cap
  CHECK(make_ticks(hz, 12, t, 4) == 4);
  CHECK(make_ticks(hz, 12, t, 0) == 0);
  CHECK(make_ticks(hz, 0, t, 16) == 0);
  CHECK(make_ticks(AxisSpec{AxisKind::LogFreq, 0.0f, 100.0f}, 8, t, 16) == 0);
  // dB: the smallest of 1, 2, 3, 6, 12, 24, 48 that gives at most max_count; 0 dB is major
  n = make_ticks(AxisSpec{AxisKind::Db, -24.0f, 24.0f}, 8, t, 16);
  CHECK(n == 9);
  CHECK((t[0].value == -24.0f && t[4].value == 0.0f && t[8].value == 24.0f));
  CHECK(t[4].major);
  CHECK(make_ticks(AxisSpec{AxisKind::Db, -24.0f, 24.0f}, 16, t, 32) == 17);   // 3 dB steps: the first of the list that gives <= 16
  CHECK(make_ticks(AxisSpec{AxisKind::Db, -24.0f, 24.0f}, 16, t, 16) == 16);   // the cap
  CHECK(make_ticks(AxisSpec{AxisKind::Db, -60.0f, 0.0f}, 6, t, 16) == 6);      // step 12: -60 -48 ... 0
  CHECK(t[5].value == 0.0f);
  // linear: 1, 2, 5 times a power of ten
  n = make_ticks(AxisSpec{AxisKind::Linear, 0.0f, 1.0f}, 5, t, 16);
  CHECK(n == 6);  // 0, 0.2 ... 1
  CHECK(t[1].value == doctest::Approx(0.2f));
  n = make_ticks(AxisSpec{AxisKind::Linear, 0.0f, 2.0f}, 20, t, 16);
  CHECK(n == 16);  // capped: step 0.1 would give 21
  n = make_ticks(AxisSpec{AxisKind::Linear, 0.0f, 100.0f}, 8, t, 16);
  CHECK(n == 6);  // step 20
  n = make_ticks(AxisSpec{AxisKind::Linear, 0.3f, 0.65f}, 4, t, 16);
  CHECK(n >= 2);
  CHECK(t[0].value >= 0.3f - 1e-6f);
  CHECK(t[n - 1].value <= 0.65f + 1e-6f);
  CHECK(make_ticks(AxisSpec{AxisKind::Linear, 1.0f, 1.0f}, 4, t, 16) == 0);
}

TEST_CASE("plot math: ranges that are not finite, overflow or are too small have no ticks and map to 0, and do not hang") {
  const float inf = std::numeric_limits<float>::infinity(), nan = std::nanf(""), big = 3.0e38f, tiny = 1.0e-45f;
  Tick t[32];
  const AxisKind kinds[3] = {AxisKind::Linear, AxisKind::Db, AxisKind::LogFreq};
  const AxisSpec bad[] = {
      {AxisKind::Linear, 0.0f, inf},     {AxisKind::Linear, -inf, 0.0f},   {AxisKind::Linear, -inf, inf},
      {AxisKind::Linear, 0.0f, nan},     {AxisKind::Linear, nan, 1.0f},    {AxisKind::Linear, -big, big},  // the width overflows
      {AxisKind::Db, 0.0f, inf},         {AxisKind::Db, -big, big},        {AxisKind::Db, nan, nan},
      {AxisKind::LogFreq, 20.0f, inf},   {AxisKind::LogFreq, tiny, 1.0f},  {AxisKind::LogFreq, tiny, big},  // the ratio overflows
      {AxisKind::LogFreq, 0.0f, 100.0f}, {AxisKind::LogFreq, nan, 100.0f}, {AxisKind::LogFreq, 1.0f, 1.0f},
  };
  for (const AxisSpec& a : bad) {
    for (const unsigned max_count : {1u, 5u, 12u, 1000u}) {
      CHECK(make_ticks(a, max_count, t, 32) == 0);
    }
    CHECK(axis_t(a, 3.0f) == 0.0f);
    CHECK((std::isnan(a.lo) ? std::isnan(axis_value(a, 0.5f)) : axis_value(a, 0.5f) == a.lo));  // the low end, whatever it is
    char buf[16];
    CHECK(format_tick(a.kind, 3.0f, buf) > 0);  // formatting a value is independent of the axis
  }
  // legal but extreme ranges finish, with a bounded number of ticks, and in order
  const AxisSpec odd[] = {
      {AxisKind::Linear, 0.0f, tiny},           {AxisKind::Linear, 0.0f, 1.0e-30f},       {AxisKind::Linear, 1.0e30f, 1.0e30f + 1.0e24f},
      {AxisKind::Linear, -1.0e9f, 1.0e9f},      {AxisKind::Linear, 0.0f, 1.0e38f},        {AxisKind::Db, -3.0e38f, 3.0e38f * 0.5f},
      {AxisKind::LogFreq, 1.0e-30f, 1.0e30f},   {AxisKind::LogFreq, 1.0e-40f, 1.0e-39f},  {AxisKind::LogFreq, 1.0f, 3.0e38f},
      {AxisKind::Linear, 16777216.0f, 16777218.0f}, {AxisKind::Linear, 1.0e30f, 1.0e30f * 1.0000001f},
  };
  for (const AxisSpec& a : odd) {
    for (const unsigned max_count : {1u, 5u, 12u, 1000u}) {
      const unsigned n = make_ticks(a, max_count, t, 32);
      CHECK(n <= 32);
      for (unsigned i = 1; i < n; ++i) {
        CHECK(t[i].value >= t[i - 1].value);
      }
    }
  }
  (void)kinds;
  // nothing to draw into, nothing to count
  CHECK(make_ticks(AxisSpec{AxisKind::Linear, 0.0f, 1.0f}, 5, nullptr, 32) == 0);
}

TEST_CASE("plot math: tick labels") {
  CHECK(fmt(format_hz, 20.0f) == "20");
  CHECK(fmt(format_hz, 100.0f) == "100");
  CHECK(fmt(format_hz, 999.0f) == "999");
  CHECK(fmt(format_hz, 1000.0f) == "1k");
  CHECK(fmt(format_hz, 1500.0f) == "1.5k");
  CHECK(fmt(format_hz, 2000.0f) == "2k");
  CHECK(fmt(format_hz, 10000.0f) == "10k");
  CHECK(fmt(format_hz, 20000.0f) == "20k");
  CHECK(fmt(format_hz, 22050.0f) == "22.1k");
  CHECK(fmt(format_hz, 0.4f) == "0");
  CHECK(fmt(format_db, -12.0f) == "-12");
  CHECK(fmt(format_db, 0.0f) == "0");
  CHECK(fmt(format_db, 0.2f) == "0");
  CHECK(fmt(format_db, -0.2f) == "0");
  CHECK(fmt(format_db, 6.0f) == "+6");
  CHECK(fmt(format_db, -48.0f) == "-48");
  CHECK(fmt(format_db, 100.0f) == "+100");
  CHECK(fmt(format_value, 0.5f) == "0.5");
  CHECK(fmt(format_value, 1.0f) == "1");
  CHECK(fmt(format_value, -0.25f) == "-0.25");
  CHECK(fmt(format_value, 0.0f) == "0");
  CHECK(fmt(format_value, 12.0f) == "12");
  CHECK(fmt(format_value, 1.999f) == "2");   // rounds, and the carry reaches the integer part
  CHECK(fmt(format_value, 0.05f) == "0.05");
  CHECK(fmt(format_value, 0.1f) == "0.1");
  CHECK(fmt(format_value, 1234.0f) == "1234");
  // out of range: the empty label, never a wrong one or an overflow of the caller's buffer; NaN reads as 0
  CHECK(fmt(format_value, 1.0e9f) == "");
  CHECK(fmt(format_value, -1.0e9f) == "");
  CHECK(fmt(format_value, 100000.0f) == "");
  CHECK(fmt(format_value, 99999.0f) == "99999");
  CHECK(fmt(format_value, -99999.5f) == "-99999.5");
  CHECK(fmt(format_value, std::numeric_limits<float>::infinity()) == "");
  CHECK(fmt(format_value, std::nanf("")) == "0");
  CHECK(fmt(format_db, 1.0e30f) == "");
  CHECK(fmt(format_db, -1.0e30f) == "");
  CHECK(fmt(format_db, 999.0f) == "+999");
  CHECK(fmt(format_db, -999.4f) == "-999");
  CHECK(fmt(format_db, std::nanf("")) == "0");
  CHECK(fmt(format_hz, 1.0e9f) == "");
  CHECK(fmt(format_hz, 999000.0f) == "999k");
  CHECK(fmt(format_hz, 192000.0f) == "192k");
  CHECK(fmt(format_hz, -5.0f) == "0");
  CHECK(fmt(format_hz, std::nanf("")) == "0");
  char buf[16];
  CHECK(format_tick(AxisKind::LogFreq, 1000.0f, buf) == 2);
  CHECK(std::string(buf) == "1k");
  CHECK(format_tick(AxisKind::Db, -6.0f, buf) == 2);
  CHECK(std::string(buf) == "-6");
  CHECK(format_tick(AxisKind::Linear, 0.5f, buf) == 3);
  CHECK(std::string(buf) == "0.5");
}
