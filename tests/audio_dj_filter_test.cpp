// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-DJ A2: the master DJ filter (audio/master_dj_filter.h) - the response against the analytic one at a few points, 0 is bypass and bit for bit, the morph is continuous across 0, no step in the
// output when it starts, glides, crosses 0 or runs down, and the bad values are refused. The filter in the graph (the bass slot, the ParamChange, the export) is audio_perform_test.cpp's.
#include <doctest/doctest.h>

#include <o3os/audio/master_dj_filter.h>
#include <o3os/audio/master_limiter.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

using o3os::audio::MasterDjFilter;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 44117.65;  // the rate the cutoff law is written for
constexpr unsigned kB = MasterDjFilter::kBlock;

// The analytic gain of the trapezoidal state-variable filter at `hz` for the knob at `percent` (the law of the header, in double).
double gain_of(double percent, double hz) {
  const bool lp = percent < 0.0;
  const double t = std::fabs(percent) / 100.0;
  const double open = lp ? static_cast<double>(MasterDjFilter::kLpOpenLog2) : static_cast<double>(MasterDjFilter::kHpOpenLog2);
  const double span = lp ? static_cast<double>(MasterDjFilter::kLpSpanLog2) : static_cast<double>(MasterDjFilter::kHpSpanLog2);
  const double g = std::exp2(open + span * t);
  const double r = std::tan(kPi * hz / kRate) / g;
  const double k = static_cast<double>(MasterDjFilter::kK);
  const double den = std::sqrt((1.0 - r * r) * (1.0 - r * r) + k * k * r * r);
  return (lp ? 1.0 : r * r) / den;
}

struct Run {
  MasterDjFilter f;
  bool bypass = false;
  Run() { f.attach(&bypass); }
  // `blocks` blocks of the two-channel signal `sig(n)` (the right channel is 0.5 of it), appended to l / r; the input is kept in xl.
  template <class Sig>
  void go(unsigned blocks, Sig sig, std::vector<float>& xl, std::vector<float>& yl) {
    for (unsigned b = 0; b < blocks; ++b) {
      float l[kB], r[kB];
      for (unsigned i = 0; i < kB; ++i) {
        const float x = sig(static_cast<unsigned>(xl.size()));
        l[i] = x;
        r[i] = 0.5f * x;
        xl.push_back(x);
      }
      f.process(l, r, kB);
      for (unsigned i = 0; i < kB; ++i) {
        yl.push_back(l[i]);
        CHECK(r[i] == doctest::Approx(0.5f * l[i]).epsilon(1e-4).scale(1.0));  // the two channels are the same filter
      }
    }
  }
};

auto sine(double hz, double amp = 0.5) {
  return [=](unsigned n) { return static_cast<float>(amp * std::sin(2.0 * kPi * hz * n / kRate)); };
}

double rms(const std::vector<float>& v, std::size_t from) {
  double s = 0;
  for (std::size_t i = from; i < v.size(); ++i) {
    s += static_cast<double>(v[i]) * static_cast<double>(v[i]);
  }
  return std::sqrt(s / static_cast<double>(v.size() - from));
}

// The nearest frequency that fits a whole number of periods in the measuring window (the second half of the run, 2560 samples), so that the RMS has no leakage: the test names its frequencies
// and measures at these, and the analytic gain is taken at the same one.
double bin_hz(double hz) {
  const double bins = std::max(1.0, std::round(hz * 2560.0 / kRate));
  return bins * kRate / 2560.0;
}

// The steady gain of a sine through the knob held at `percent`, in dB (at bin_hz(hz)).
double measured_db(double percent, double hz) {
  Run r;
  std::vector<float> x, y;
  CHECK(r.f.set(static_cast<float>(percent), 1));
  r.go(80, sine(bin_hz(hz)), x, y);
  return 20.0 * std::log10(rms(y, y.size() / 2) / rms(x, x.size() / 2));
}

double max_step(const std::vector<float>& v, std::size_t* at = nullptr) {
  double m = 0;
  for (std::size_t i = 1; i < v.size(); ++i) {
    const double d = static_cast<double>(std::fabs(v[i] - v[i - 1]));
    if (d > m) {
      m = d;
      if (at != nullptr) {
        *at = i;
      }
    }
  }
  return m;
}

}  // namespace

TEST_CASE("dj filter (A2): the response is the trapezoidal filter's at the law's cutoffs - low-pass left, high-pass right") {
  struct P {
    double percent, hz;
  };
  for (const P p : {P{-50, 100}, P{-50, 1000}, P{-50, 5000}, P{-100, 40}, P{-100, 500}, P{-100, 5000}, P{50, 100}, P{50, 1000}, P{50, 5000}, P{100, 100}, P{100, 6000}}) {
    const double want = 20.0 * std::log10(gain_of(p.percent, bin_hz(p.hz)));
    CHECK_MESSAGE(std::fabs(measured_db(p.percent, p.hz) - want) < 0.1, "knob " << p.percent << " at " << p.hz << " Hz: want " << want);
  }
  CHECK(measured_db(-100, 5000) < -40.0);  // fully left: the highs are gone
  CHECK(measured_db(-100, 40) > -3.5);     // ... and the bass is not
  CHECK(measured_db(100, 100) < -40.0);    // fully right: the lows are gone
  CHECK(measured_db(100, 12000) > -3.0);   // ... and the air is only at its corner (8 kHz, critically damped: -2.4 dB at 12 kHz)
  CHECK(measured_db(-75, 1000) < -3.0);  // (the cutoff at -50 is 3.1 kHz, at -75 it is 440 Hz)
  CHECK(measured_db(50, 1000) < 0.0);      // (critically damped: no resonance, no sine is lifted; the stage adds no level to a sine)
}

TEST_CASE("dj filter (A2): 0 is bypass - the planes are not touched, bit for bit - and the stage switches itself off once the glide has run down") {
  Run r;
  std::vector<float> x, y;
  r.go(4, sine(440), x, y);
  CHECK(std::memcmp(x.data(), y.data(), x.size() * sizeof(float)) == 0);
  CHECK(r.f.idle());
  CHECK(r.f.set(0.0f, 1));  // a 0 told to a filter at 0 changes nothing
  x.clear();
  y.clear();
  r.go(2, sine(440), x, y);
  CHECK(std::memcmp(x.data(), y.data(), x.size() * sizeof(float)) == 0);
  // up, heard, and back down: not bypassed until it has arrived
  r.bypass = false;
  CHECK(r.f.set(-80.0f, 10 * kB));
  CHECK_FALSE(r.f.idle());
  x.clear();
  y.clear();
  r.go(20, sine(3000), x, y);
  CHECK_FALSE(std::memcmp(x.data(), y.data(), x.size() * sizeof(float)) == 0);
  CHECK_FALSE(r.bypass);
  CHECK(r.f.set(0.0f, 10 * kB));
  CHECK_FALSE(r.f.idle());
  x.clear();
  y.clear();
  r.go(9, sine(3000), x, y);
  CHECK_FALSE(r.bypass);  // still gliding
  r.go(3, sine(3000), x, y);
  CHECK(r.bypass);
  CHECK(r.f.idle());
  x.clear();
  y.clear();
  r.go(3, sine(3000), x, y);  // (as the graph would no longer call it: called anyway, it leaves the block alone)
  CHECK(std::memcmp(x.data(), y.data(), x.size() * sizeof(float)) == 0);
}

TEST_CASE("dj filter (A2): the morph is continuous - both halves are flat at +-1 and the gain moves by small steps across the whole knob") {
  // next to 0 either way the stage is flat from 200 Hz to 9 kHz; critically damped (k = 2) it is not to the ends: 30 Hz is down by up to 1.2 dB (the high-pass side's open end is 10 Hz) and 16 kHz by up to 0.5 dB
  for (const double p : {-1.0, -0.2, 0.2, 1.0}) {
    for (const double hz : {30.0, 200.0, 2000.0, 9000.0, 16000.0}) {
      CHECK_MESSAGE(std::fabs(measured_db(p, hz)) < (hz < 100.0 ? 1.2 : (hz > 12000.0 ? 0.5 : 0.05)), "knob " << p << " at " << hz << " Hz");
    }
  }
  for (const double hz : {1000.0, 3000.0}) {
    double prev = measured_db(-100, hz);
    for (int p = -99; p <= 100; ++p) {
      if (p == 0) {
        continue;
      }
      const double db = measured_db(p, hz);
      CHECK_MESSAGE(std::fabs(db - prev) < 2.0, "knob " << p << " at " << hz << " Hz: " << prev << " to " << db << " dB");
      prev = db;
    }
  }
  // the low-pass side only closes, the high-pass side only closes, as the knob leaves 0 (critically damped: nothing is lifted)
  double lp = measured_db(-1, 1000);
  for (int p = -2; p >= -100; --p) {
    const double db = measured_db(p, 1000);
    CHECK_MESSAGE(db <= lp + 0.001, "knob " << p);
    lp = db;
  }
  double hp = measured_db(1, 1000);
  for (int p = 2; p <= 100; ++p) {
    const double db = measured_db(p, 1000);
    CHECK_MESSAGE(db <= hp + 0.001, "knob " << p);
    hp = db;
  }
}

TEST_CASE("dj filter (A2): no step in the output when it starts, glides, crosses 0 or runs down") {
  std::vector<float> ref_x, ref_y;
  {
    Run plain;
    plain.go(1, sine(440), ref_x, ref_y);
    for (unsigned i = 0; i < 4000; ++i) {
      ref_x.push_back(sine(440)(i));
    }
  }
  const double bound = 1.2 * max_step(ref_x);  // the input's own largest step (a click is many times that)
  struct Case {
    float from, to;
  };
  for (const Case c : {Case{-60, 0}, Case{60, 0}, Case{-100, 0}, Case{100, 0}, Case{-80, 80}, Case{80, -80}, Case{-3, 3}}) {
    Run r;
    std::vector<float> x, y;
    r.go(4, sine(440), x, y);
    CHECK(r.f.set(c.from, 41 * kB));  // 60 ms glide in, as the page tells it
    r.go(60, sine(440), x, y);
    CHECK(r.f.set(c.to, 80 * kB));
    r.go(100, sine(440), x, y);
    std::size_t at = 0;
    const double m = max_step(y, &at);
    CHECK_MESSAGE(m <= bound, "from " << c.from << " to " << c.to << ": largest step " << m << " at " << at << " against " << bound);
  }
  // a low-pass that starts on a level (a DC offset) leaves it where it is: its state is made from the first sample
  Run d;
  std::vector<float> x, y;
  CHECK(d.f.set(-100.0f, 1));
  d.go(8, [](unsigned) { return 0.4f; }, x, y);
  for (const float v : y) {
    CHECK(std::fabs(v - 0.4f) < 1e-5f);
  }
}

TEST_CASE("dj filter (A2): out of range or not a number is refused and changes nothing; jump() is at once and clears the state") {
  Run r;
  CHECK_FALSE(r.f.set(100.5f, 1));
  CHECK_FALSE(r.f.set(-101.0f, 1));
  CHECK_FALSE(r.f.set(std::nanf(""), 1));
  CHECK(r.f.idle());
  CHECK(r.f.set(100.0f, 0));
  CHECK(r.f.set(-100.0f, 1));
  r.f.jump(40.0f);
  CHECK(r.f.target() == 40.0f);
  CHECK(r.f.position() == 40.0f);
  r.f.jump(0.0f);
  CHECK(r.f.idle());
  float l[kB] = {}, rr[kB] = {};
  l[3] = 0.25f;
  rr[5] = -0.25f;
  r.f.process(l, rr, kB - 1u);  // a block of any other length is left alone
  CHECK(l[3] == 0.25f);
}

// OS-DJ phase-A review fold: a ceiling-level (-0.3 dBFS) signal leaves 3.5 % of headroom, and the converter saturates above full scale. A sine cannot show what a step does
// (a kick, a square): so a step and ceiling-level squares go through the filter and the peak is read off. The critically damped (k = 2) low-pass has no overshoot at all while its cutoff is
// under about a quarter of the sample rate (knob -40 and beyond: cutoff 8 kHz and down): the peaks are asserted at the ceiling itself (1e-6 for the float rounding), not at full scale. Further open than that the bilinear filter rings by itself (the cutoff, warped, is near the Nyquist
// frequency): 4 to 31 % over a ceiling-level edge, NOT fixed here and not asserted: since A3c the stage runs ahead of the limiter, which catches it (audio_master_limiter_graph_test.cpp).
TEST_CASE("dj filter (review fold): a ceiling-level step and square wave do not go over full scale - low-pass from knob -40 to -100, high-pass step all along") {
  constexpr float kCeil = o3os::audio::MasterLimiter::kCeiling;
  constexpr double kBound = static_cast<double>(kCeil) * (1.0 + 1e-6);  // the ceiling itself, to the float rounding of the filter's sums: NOT the 3.5 % of headroom above it
  const auto peak = [](double percent, auto sig) {
    Run r;
    std::vector<float> x, y;
    CHECK(r.f.set(static_cast<float>(percent), 1));
    r.go(1, [](unsigned) { return 0.0f; }, x, y);  // a block of silence: the state is at rest, the signal's first sample is a real edge
    r.go(200, sig, x, y);
    double m = 0;
    for (const float v : y) {
      m = std::max(m, static_cast<double>(std::fabs(v)));
    }
    return m;
  };
  const auto square = [](double hz) {
    return [=](unsigned n) { return (std::fmod(static_cast<double>(n - kB) * hz / kRate, 1.0) < 0.5) ? kCeil : -kCeil; };
  };
  for (int p = -40; p >= -100; p -= 10) {
    CHECK_MESSAGE(peak(p, [](unsigned) { return kCeil; }) <= kBound, "step up, knob " << p);
    CHECK_MESSAGE(peak(p, [](unsigned) { return -kCeil; }) <= kBound, "step down, knob " << p);
    for (const double hz : {30.0, 100.0, 440.0, 1000.0, 3000.0, 8000.0}) {
      CHECK_MESSAGE(peak(p, square(hz)) <= kBound, "square " << hz << " Hz, knob " << p);
    }
  }
  for (int p = 10; p <= 100; p += 10) {  // a high-pass passes an edge at its height and brings it back: a step never goes over (a bipolar square at full scale does, which no high-pass can help)
    CHECK_MESSAGE(peak(p, [](unsigned) { return kCeil; }) <= kBound, "high-pass step, knob " << p);
  }
}
