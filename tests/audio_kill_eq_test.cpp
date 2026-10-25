// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-DJ A3 / A3b: the master kill EQ (audio/master_kill_eq.h) - two Linkwitz-Riley crossovers (24 dB an octave) at 180 Hz and 2.8 kHz and an all-pass on the LOW band: the three bands sum to an
// all-pass (flat magnitude, impulse energy 1), no kill lifts anything (so the HIGH kill needs no compensation any more), a killed band is -40 dB or deeper at its centre (LOW at 50 Hz, MID at 700 Hz,
// HIGH at 10 kHz), the response of the real stage against the analytic one, bypass is exact, the 5 ms ramp and the crossfade in and out of bypass have no click (a full-scale sine at 100 Hz, 1 kHz
// and 8 kHz, at several phases, every kill), the export's reset, and the bad values are refused. The stage in the graph (the eq slot, the ParamChange, the export) is audio_perform_test.cpp's.
#include <doctest/doctest.h>

#include <o3os/audio/master_kill_eq.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <vector>

using o3os::audio::MasterKillEq;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 44117.647;  // the rate the corners are written for
constexpr unsigned kB = MasterKillEq::kBlock;
using Cx = std::complex<double>;

// A second-order Butterworth section of the corner fc at the frequency hz, in the bilinear form the stage runs (s = j tan(pi hz / fs) / tan(pi fc / fs)): low-pass, high-pass and all-pass.
struct Sec {
  Cx lp;
  Cx hp;
  Cx ap;
};
Sec section(double fc, double hz) {
  const double x = std::tan(kPi * hz / kRate) / std::tan(kPi * fc / kRate);
  const Cx d(1.0 - x * x, std::sqrt(2.0) * x);
  return {1.0 / d, Cx(-x * x) / d, std::conj(d) / d};
}

// The three bands as the stage makes them: LR4 = two sections in a row; LOW with the all-pass of the upper corner on it.
struct Bands {
  Cx low;
  Cx mid;
  Cx high;
};
Bands bands(double hz) {
  const Sec a = section(static_cast<double>(MasterKillEq::kLowHz), hz);
  const Sec b = section(static_cast<double>(MasterKillEq::kHighHz), hz);
  return {a.lp * a.lp * b.ap, a.hp * a.hp * b.lp * b.lp, a.hp * a.hp * b.hp * b.hp};
}

// The gain of the stage with `mask` killed at `hz`: the sum of the live bands.
double analytic_db(unsigned mask, double hz) {
  const Bands b = bands(hz);
  Cx sum = 0.0;
  sum += (mask & MasterKillEq::kLow) != 0u ? 0.0 : b.low;
  sum += (mask & MasterKillEq::kMid) != 0u ? 0.0 : b.mid;
  sum += (mask & MasterKillEq::kHigh) != 0u ? 0.0 : b.high;
  return 20.0 * std::log10(std::abs(sum));
}

struct Run {
  MasterKillEq f;
  bool bypass = false;
  Run() { f.attach(&bypass); }
  // `blocks` blocks of the signal sig(n) on the left and 0.5 of it on the right, appended to xl / yl (the input and the output of the left channel).
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
      if (!bypass) {  // (the graph does not call a bypassed stage)
        f.process(l, r, kB);
      }
      for (unsigned i = 0; i < kB; ++i) {
        yl.push_back(l[i]);
        CHECK(r[i] == doctest::Approx(0.5f * l[i]).epsilon(1e-4).scale(1.0));  // the two channels are the same stage
      }
    }
  }
};

auto sine(double hz, double amp = 0.5, double phase = 0.0) {
  return [=](unsigned n) { return static_cast<float>(amp * std::sin(2.0 * kPi * hz * n / kRate + phase)); };
}

double rms(const std::vector<float>& v, std::size_t from) {
  double s = 0;
  for (std::size_t i = from; i < v.size(); ++i) {
    s += static_cast<double>(v[i]) * static_cast<double>(v[i]);
  }
  return std::sqrt(s / static_cast<double>(v.size() - from));
}

// The nearest frequency that fits a whole number of periods in the measuring window (the second half of an 80-block run, 2560 samples), so that the RMS has no leakage.
double bin_hz(double hz) {
  const double bins = std::max(1.0, std::round(hz * 2560.0 / kRate));
  return bins * kRate / 2560.0;
}

// The run of a sine through the stage with `mask` killed: the input and the output of the left channel. The ramp has run down long before the window.
void steady(unsigned mask, double hz, std::vector<float>& x, std::vector<float>& y) {
  Run r;
  CHECK(r.f.set(static_cast<float>(mask)));
  r.go(80, sine(bin_hz(hz)), x, y);
}

// The steady gain of a sine through the stage with `mask` killed, in dB (at bin_hz(hz)).
double measured_db(unsigned mask, double hz) {
  std::vector<float> x, y;
  steady(mask, hz, x, y);
  return 20.0 * std::log10(rms(y, y.size() / 2) / rms(x, x.size() / 2));
}

// The gain of the three bands together (no kill, every gain at 1, the crossfade done): the three single kills add up to twice the sum, (3 - 1) x the bands, so the RMS of their mean is the sum's.
double wet_db(double hz) {
  std::vector<float> x, ya, yb, yc;
  steady(MasterKillEq::kLow, hz, x, ya);
  x.clear();
  steady(MasterKillEq::kMid, hz, x, yb);
  x.clear();
  steady(MasterKillEq::kHigh, hz, x, yc);
  std::vector<float> sum(ya.size());
  for (std::size_t i = 0; i < sum.size(); ++i) {
    sum[i] = 0.5f * (ya[i] + yb[i] + yc[i]);
  }
  return 20.0 * std::log10(rms(sum, sum.size() / 2) / rms(x, x.size() / 2));
}

double max_step(const std::vector<float>& v, std::size_t from = 1, std::size_t to = SIZE_MAX) {
  double m = 0;
  for (std::size_t i = std::max<std::size_t>(from, 1); i < v.size() && i < to; ++i) {
    m = std::max(m, static_cast<double>(std::fabs(v[i] - v[i - 1])));
  }
  return m;
}

// Frequencies spread over the audio band (20 Hz to 20 kHz) that the response tests walk, both the analytic and the real stage.
constexpr double kFreqs[] = {20.0, 31.0, 50.0, 80.0, 125.0, 180.0, 250.0, 394.0, 500.0, 700.0, 1000.0, 1600.0, 2800.0, 4000.0, 6300.0, 10000.0, 16000.0, 20000.0};

// The sweep the analytic checks walk: 600 frequencies, 10 Hz to 20 kHz.
template <class Fn>
void sweep(Fn fn) {
  for (int i = 0; i < 600; ++i) {
    fn(10.0 * std::pow(2000.0, i / 599.0));
  }
}

}  // namespace

TEST_CASE("kill eq (A3b): an LR4 split is -6 dB at its corner for both bands, and the stage agrees at the two corners") {
  const Sec a = section(static_cast<double>(MasterKillEq::kLowHz), static_cast<double>(MasterKillEq::kLowHz));
  const Sec b = section(static_cast<double>(MasterKillEq::kHighHz), static_cast<double>(MasterKillEq::kHighHz));
  CHECK(20.0 * std::log10(std::abs(a.lp * a.lp)) == doctest::Approx(-6.02).epsilon(0.001));
  CHECK(20.0 * std::log10(std::abs(a.hp * a.hp)) == doctest::Approx(-6.02).epsilon(0.001));
  CHECK(20.0 * std::log10(std::abs(b.lp * b.lp)) == doctest::Approx(-6.02).epsilon(0.001));
  // the bands at the corners: LOW is -6 dB at 180 Hz and HIGH at 2.8 kHz (the other split is open there to within 0.2 dB)
  CHECK(20.0 * std::log10(std::abs(bands(static_cast<double>(MasterKillEq::kLowHz)).low)) == doctest::Approx(-6.02).epsilon(0.001));
  CHECK(20.0 * std::log10(std::abs(bands(static_cast<double>(MasterKillEq::kHighHz)).high)) == doctest::Approx(-6.02).epsilon(0.05));
  // and the real stage follows (only LOW live, MID and HIGH killed; only HIGH live): at the nearest frequency that fits the measuring window, within 0.05 dB
  const unsigned only_low = MasterKillEq::kMid | MasterKillEq::kHigh;
  const unsigned only_high = MasterKillEq::kLow | MasterKillEq::kMid;
  const double lo = bin_hz(static_cast<double>(MasterKillEq::kLowHz));
  const double hi = bin_hz(static_cast<double>(MasterKillEq::kHighHz));
  CHECK(measured_db(only_low, lo) == doctest::Approx(analytic_db(only_low, lo)).epsilon(0.01).scale(1.0));
  CHECK(measured_db(only_high, hi) == doctest::Approx(analytic_db(only_high, hi)).epsilon(0.01).scale(1.0));
  CHECK(analytic_db(only_low, lo) < -4.5);  // (the window's frequency is within 5 % of the corner)
  CHECK(analytic_db(only_low, lo) > -7.5);
}

TEST_CASE("kill eq (A3b): the bands sum to an all-pass - flat within 0.001 dB analytically and within 0.1 dB on the real stage from 20 Hz to 20 kHz, and the impulse response has energy 1") {
  double worst = 0;
  sweep([&worst](double hz) {
    const Bands b = bands(hz);
    worst = std::max(worst, std::fabs(20.0 * std::log10(std::abs(b.low + b.mid + b.high))));
  });
  CHECK(worst < 0.001);
  // the real stage: the mean of the three single kills is the sum of the bands (3 x bands - bands = 2 x bands)
  double worst_real = 0;
  for (const double hz : kFreqs) {
    const double db = wet_db(hz);
    worst_real = std::max(worst_real, std::fabs(db));
    CHECK_MESSAGE(std::fabs(db) < 0.1, "at " << hz << " Hz: " << db << " dB");
  }
  CHECK(worst_real < 0.02);  // (the stage, not only the bound: float rounding is 1e-7)
  // the impulse response of the sum, the kill held: an all-pass keeps the energy of the impulse (Parseval), tails and all
  std::vector<float> imp[3];
  const unsigned masks[3] = {MasterKillEq::kLow, MasterKillEq::kMid, MasterKillEq::kHigh};
  for (unsigned m = 0; m < 3u; ++m) {
    Run r;
    CHECK(r.f.set(static_cast<float>(masks[m])));
    std::vector<float> x;
    r.go(12, [](unsigned) { return 0.0f; }, x, imp[m]);  // the ramp is done and the states are at rest
    imp[m].clear();
    x.clear();
    r.go(60, [](unsigned n) { return n == 0u ? 0.5f : 0.0f; }, x, imp[m]);
  }
  double energy = 0;
  for (std::size_t i = 0; i < imp[0].size(); ++i) {
    const double s = 0.5 * (static_cast<double>(imp[0][i]) + static_cast<double>(imp[1][i]) + static_cast<double>(imp[2][i]));
    energy += s * s;
  }
  CHECK(energy / 0.25 == doctest::Approx(1.0).epsilon(1e-3));
}

TEST_CASE("kill eq (A3b): no kill lifts anything above +0.1 dB - analytically for every mask over the whole band, on the real stage over the frequencies, and the HIGH kill is not compensated") {
  double top = -1000;
  for (unsigned mask = 1; mask <= MasterKillEq::kAll; ++mask) {
    sweep([&](double hz) { top = std::max(top, analytic_db(mask, hz)); });
  }
  CHECK(top < 0.001);  // (the worst of the 4200 points: in fact none is above 0 dB)
  for (unsigned mask = 1; mask <= MasterKillEq::kAll; ++mask) {
    for (const double hz : kFreqs) {
      const double got = measured_db(mask, hz);
      CHECK_MESSAGE(got < 0.1, "mask " << mask << " at " << hz << " Hz: " << got << " dB");
    }
  }
  // The first-order split lifted 394 Hz by 0.43 dB with HIGH killed and carried a constant 0.95 on the sum (-0.45 dB for everything left). There is no such gain: what a HIGH kill leaves in the low
  // and mid bands is at 0 dB (within 0.02) at 50 Hz, 100 Hz and 394 Hz, and nothing of it is above 0 (what the HIGH band's own skirt takes, 0.13 dB at 1 kHz, is not a loss to compensate).
  for (const double hz : {50.0, 100.0, 394.0}) {
    const double got = measured_db(MasterKillEq::kHigh, hz);
    CHECK_MESSAGE(got > -0.1, "HIGH kill at " << hz << " Hz: " << got << " dB");
    CHECK_MESSAGE(got < 0.02, "HIGH kill at " << hz << " Hz: " << got << " dB");
  }
  double high_peak = -1000;
  sweep([&high_peak](double hz) { high_peak = std::max(high_peak, analytic_db(MasterKillEq::kHigh, hz)); });
  CHECK(high_peak < 0.0005);
  CHECK(analytic_db(MasterKillEq::kHigh, 50.0) > -0.01);  // (the 0.95 constant would be -0.45 dB here)
}

TEST_CASE("kill eq (A3b): a killed band is -40 dB or deeper at its centre - LOW at 50 Hz, MID at 700 Hz, HIGH at 10 kHz - and the real stage follows the analytic response") {
  // the depths the maintainer asked for (kill depth A, 2026-10-19), on the real stage; the margin to the bound is small only for the MID kill, whose two neighbours' leaks add up in phase
  const double low_50 = measured_db(MasterKillEq::kLow, 50.0);
  const double mid_700 = measured_db(MasterKillEq::kMid, 700.0);
  const double high_10k = measured_db(MasterKillEq::kHigh, 10000.0);
  CHECK_MESSAGE(low_50 <= -40.0, "LOW killed at 50 Hz: " << low_50 << " dB");
  CHECK_MESSAGE(mid_700 <= -40.0, "MID killed at 700 Hz: " << mid_700 << " dB");
  CHECK_MESSAGE(high_10k <= -40.0, "HIGH killed at 10 kHz: " << high_10k << " dB");
  CHECK(low_50 > -48.0);  // (the same numbers, bounded below: they are the analytic ones, about -44, -42 and -50 dB, not a float floor)
  CHECK(mid_700 > -46.0);
  CHECK(high_10k > -55.0);
  // further in, deeper: 24 dB an octave
  CHECK(measured_db(MasterKillEq::kLow, 31.0) < -55.0);
  CHECK(measured_db(MasterKillEq::kMid, 500.0) < -30.0);
  CHECK(measured_db(MasterKillEq::kHigh, 6300.0) < -30.0);
  // and the edge of a band is -6 dB, not a wall (a kill is a steep drop, not silence at the corner)
  CHECK(measured_db(MasterKillEq::kLow, 180.0) > -7.0);
  CHECK(measured_db(MasterKillEq::kHigh, 2800.0) > -7.0);
  // what is not killed stays: a low kill leaves 10 kHz, a high kill 50 Hz, a mid kill 20 Hz and 10 kHz
  CHECK(measured_db(MasterKillEq::kLow, 10000.0) > -0.05);
  CHECK(measured_db(MasterKillEq::kHigh, 50.0) > -0.05);
  CHECK(measured_db(MasterKillEq::kMid, 20.0) > -0.05);
  CHECK(measured_db(MasterKillEq::kMid, 10000.0) > -0.05);
  // the real stage against the analytic model, every mask: within 0.05 dB while the response is above -60 dB; deeper it is bounded (float rounding is about -120 dB)
  double worst = 0;
  for (unsigned mask = 1; mask <= MasterKillEq::kAll; ++mask) {
    for (const double hz : kFreqs) {
      const double want = analytic_db(mask, bin_hz(hz));
      const double got = measured_db(mask, hz);
      if (want > -60.0) {
        worst = std::max(worst, std::fabs(got - want));
      } else {
        CHECK_MESSAGE(got < -50.0, "mask " << mask << " at " << hz << " Hz: " << got << " dB, analytic " << want);
      }
    }
  }
  CHECK(worst < 0.05);
  // all three killed: nothing is left
  Run r;
  std::vector<float> xi, y;
  CHECK(r.f.set(7.0f));
  r.go(40, sine(300.0), xi, y);
  double left = 0;
  for (std::size_t i = 6u * kB; i < y.size(); ++i) {
    left = std::max(left, static_cast<double>(std::fabs(y[i])));
  }
  CHECK(left == 0.0);
}

TEST_CASE("kill eq (A3b): no kill is bypass - the stage switches itself off once everything is back, and the planes are then not touched, bit for bit") {
  Run r;
  CHECK(r.f.idle());
  CHECK(r.f.mask() == 0u);
  // a stage that is called with nothing to do puts itself in bypass and leaves the block as it is
  float l[kB], rr[kB], l0[kB], r0[kB];
  for (unsigned i = 0; i < kB; ++i) {
    l[i] = l0[i] = std::sin(0.37f * static_cast<float>(i));
    rr[i] = r0[i] = std::cos(0.11f * static_cast<float>(i));
  }
  r.f.process(l, rr, kB);
  CHECK(r.bypass);
  CHECK(std::memcmp(l, l0, sizeof l) == 0);
  CHECK(std::memcmp(rr, r0, sizeof rr) == 0);
  // on with a kill, then released: it crossfades back to the input, runs the gains to 1 and bypasses itself
  r.bypass = false;
  std::vector<float> x, y;
  CHECK(r.f.set(1.0f));
  r.go(20, sine(300.0), x, y);
  CHECK_FALSE(r.bypass);
  CHECK(r.f.gain(0) == 0.0f);
  CHECK(r.f.wet() == 1.0f);
  CHECK(r.f.set(0.0f));
  CHECK_FALSE(r.f.idle());  // the gain is still down and the bands are still in the output
  r.go(10, sine(300.0), x, y);
  CHECK(r.bypass);
  CHECK(r.f.idle());
  CHECK(r.f.wet() == 0.0f);
  CHECK(r.f.gain(0) == 1.0f);
  CHECK(r.f.gain(1) == 1.0f);
  CHECK(r.f.gain(2) == 1.0f);
  // the bypassed blocks are the input bit for bit; the last block that ran (the one that reached the rest state) is the input to the rounding of 64 ramp steps, under the 16-bit step
  for (std::size_t i = x.size() - 4u * kB; i < x.size(); ++i) {
    CHECK(std::fabs(y[i] - x[i]) < 3e-6);
  }
  for (std::size_t i = x.size() - 2u * kB; i < x.size(); ++i) {
    CHECK(y[i] == x[i]);
  }
  // a wrong block size is left alone
  float one[3] = {0.1f, 0.2f, 0.3f};
  r.f.process(one, one, 3);
  CHECK(one[0] == 0.1f);
}

TEST_CASE("kill eq (A3b): entry and exit have no click - a full-scale sine at 100 Hz, 180 Hz, 1 kHz, 2.8 kHz and 8 kHz, every kill, several phases: no step above the sine's own plus the crossfade's slope") {
  // The input's own step is the most the output may take, plus the crossfade's slope: the weight moves 1/221 a sample and the input and the bands differ by 2 A at the very most, so 2 A / 221 = 0.009 A.
  // Measured: the output's largest step is at most 0.004 A over the sine's (0.0181 against 0.0142 at 100 Hz, 0.1445 against 0.1423 at 1 kHz, none over at 8 kHz: the filters' start-up from the DC state of the first sample and the ramp). An instant
  // kill would step by the killed band's amplitude, up to A = 1 (LOW at 100 Hz). The bound is 1.02 x the sine's step + 2 A / 221.
  // The two corners (180 Hz and 2.8 kHz) are where the dry-to-bands crossfade is worst for the LEVEL, not for the step: see the next test case.
  constexpr double kAmp = 1.0;
  double worst_ratio = 0;
  for (const double hz : {100.0, 180.0, 1000.0, 2800.0, 8000.0}) {
    for (const double phase : {0.0, 1.0, 2.0, 3.5}) {
      for (unsigned mask = 1; mask <= MasterKillEq::kAll; ++mask) {
        Run r;
        std::vector<float> x, y;
        r.go(10, sine(hz, kAmp, phase), x, y);  // (bypassed: not called)
        r.bypass = false;                       // ... the graph clears the flag when the mask comes
        const std::size_t on_at = x.size();
        CHECK(r.f.set(static_cast<float>(mask)));
        r.go(14, sine(hz, kAmp, phase), x, y);
        const std::size_t off_at = x.size();
        r.bypass = false;
        CHECK(r.f.set(0.0f));
        r.go(14, sine(hz, kAmp, phase), x, y);
        CHECK(r.bypass);  // the hand-over to bypass has happened, in the run
        const double bound = 1.02 * max_step(x) + 2.0 * kAmp / 221.0;
        const double got_on = max_step(y, on_at - 1u, off_at);
        const double got_off = max_step(y, off_at - 1u);
        worst_ratio = std::max({worst_ratio, got_on / bound, got_off / bound});
        CHECK_MESSAGE(got_on <= bound, "on, " << hz << " Hz, phase " << phase << ", mask " << mask << ": " << got_on << " > " << bound);
        CHECK_MESSAGE(got_off <= bound, "off, " << hz << " Hz, phase " << phase << ", mask " << mask << ": " << got_off << " > " << bound);
      }
    }
  }
  CHECK(worst_ratio <= 1.0);
  // The ramp is 5 ms: the gain lands on 0 within 4 blocks, not before 3 (5 ms is 3.45 blocks), and the wet weight with it.
  for (const unsigned band_mask : {MasterKillEq::kLow, MasterKillEq::kMid, MasterKillEq::kHigh, MasterKillEq::kAll}) {
    MasterKillEq t;
    CHECK(t.set(static_cast<float>(band_mask)));
    float l[kB] = {}, rr[kB] = {};
    unsigned blocks = 0;
    while (t.wet() < 1.0f) {
      for (unsigned i = 0; i < kB; ++i) {
        l[i] = rr[i] = 0.25f;
      }
      t.process(l, rr, kB);
      ++blocks;
      if (blocks >= 10u) {
        FAIL("the ramp did not end");
      }
      if (blocks < 3u) {
        CHECK(t.wet() < 1.0f);
      }
    }
    CHECK(blocks == 4u);
    for (unsigned b = 0; b < 3u; ++b) {
      CHECK(t.gain(b) == (((band_mask >> b) & 1u) != 0u ? 0.0f : 1.0f));
    }
  }
}

TEST_CASE("kill eq (A3b): a ramp on its way turns round without a step, and the stage starting from bypass starts with the input's first sample") {
  Run r;
  std::vector<float> x, y;
  CHECK(r.f.set(MasterKillEq::kLow));
  r.go(2, sine(120.0, 1.0), x, y);  // half way down
  CHECK(r.f.gain(0) > 0.0f);
  CHECK(r.f.gain(0) < 1.0f);
  CHECK(r.f.set(0.0f));  // changed its mind
  r.go(8, sine(120.0, 1.0), x, y);
  CHECK(max_step(y) <= max_step(x) * 1.02 + 2.0 / 221.0);
  CHECK(r.bypass);
  CHECK(y.front() == x.front());
  // and a kill moved to another band in mid-air
  Run s;
  std::vector<float> xs, ys;
  CHECK(s.f.set(MasterKillEq::kLow));
  s.go(2, sine(900.0, 1.0), xs, ys);
  CHECK(s.f.set(MasterKillEq::kHigh));
  s.go(10, sine(900.0, 1.0), xs, ys);
  CHECK(max_step(ys) <= max_step(xs) * 1.02 + 2.0 / 221.0);
  // a fresh start in the middle of a loud passage: the first output sample is the input's, whatever the sample is, to one step of the ramp (the crossfade moves by kStep / 64 = 0.45 % a sample)
  Run t;
  CHECK(t.f.set(MasterKillEq::kLow | MasterKillEq::kHigh));
  float l[kB], rr[kB];
  for (unsigned i = 0; i < kB; ++i) {
    l[i] = 0.9f;
    rr[i] = -0.7f;
  }
  t.f.process(l, rr, kB);
  CHECK(std::fabs(l[0] - 0.9f) < 0.9f * MasterKillEq::kStep / 64.0f);
  CHECK(std::fabs(rr[0] + 0.7f) < 0.7f * MasterKillEq::kStep / 64.0f);
  CHECK(std::fabs(l[1] - 0.9f) < 0.02f);  // and a constant level goes on smoothly (no step on the second sample)
}

TEST_CASE("kill eq (A3b): a mask that is not a whole number 0..7 is refused and changes nothing; reset() clears the state and crossfades a kill in without a step") {
  MasterKillEq f;
  CHECK(f.set(5.0f));
  CHECK(f.mask() == 5u);
  for (const float bad : {8.0f, -1.0f, 1.5f, 0.5f, std::nanf(""), 1e9f, -0.25f}) {
    CHECK_FALSE(f.set(bad));
    CHECK(f.mask() == 5u);
  }
  CHECK(f.set(0.0f));
  CHECK(f.set(7.0f));
  CHECK(f.mask() == 7u);
  // run it down, then reset with a mask: gains at 1 and the input in the output, the mask the target, so the block after starts a ramp
  float l[kB] = {}, r[kB] = {};
  for (int i = 0; i < 6; ++i) {
    f.process(l, r, kB);
  }
  CHECK(f.gain(0) == 0.0f);
  CHECK(f.wet() == 1.0f);
  f.reset(MasterKillEq::kMid);
  CHECK(f.mask() == MasterKillEq::kMid);
  CHECK(f.gain(0) == 1.0f);
  CHECK(f.gain(1) == 1.0f);
  CHECK(f.wet() == 0.0f);
  CHECK_FALSE(f.idle());
  f.reset(0u);
  CHECK(f.idle());
  f.reset(0xffu);  // (only the three bands exist)
  CHECK(f.mask() == MasterKillEq::kAll);
  // The export's reset (Graph::perf_clear / perf_restore): a kill held, put away with reset(0) (the stage is then bypassed: the export's blocks are the input), and given back with reset(mask)
  // (the live render's first blocks): the kill comes in by the crossfade, with no step, from the first block.
  for (const double hz : {100.0, 1000.0, 8000.0}) {
    Run run;
    std::vector<float> x, y;
    CHECK(run.f.set(static_cast<float>(MasterKillEq::kLow | MasterKillEq::kMid)));
    run.go(12, sine(hz, 1.0), x, y);
    run.f.reset(0u);
    CHECK(run.f.idle());
    run.go(2, sine(hz, 1.0), x, y);  // (the export: the stage is called once more, finds itself idle and bypasses)
    CHECK(run.bypass);
    const std::size_t back_at = x.size();
    run.f.reset(MasterKillEq::kLow | MasterKillEq::kMid);
    run.bypass = false;
    run.go(12, sine(hz, 1.0), x, y);
    CHECK(run.f.gain(0) == 0.0f);
    const double bound = 1.02 * max_step(x) + 2.0 / 221.0;
    CHECK_MESSAGE(max_step(y, back_at - 1u) <= bound, "reset back, " << hz << " Hz: " << max_step(y, back_at - 1u) << " > " << bound);
    CHECK(std::fabs(y[back_at] - x[back_at]) < static_cast<double>(MasterKillEq::kStep) / 64.0);  // the first block back starts at the input, to one step of the ramp
  }
}

// OS-DJ A3 review fold: the entry and the exit are a crossfade between the input and the bands, and the bands are the input through an all-pass, so the two are not in phase. Where the all-pass has turned
// the tone by half a cycle or more (it is 180 degrees at the upper corner, 2.8 kHz, and near it for the lower one) the half-way mix is a partial or total cancellation: the tone dips for the 5 ms of
// the ramp, deeper than both the level it comes from and the level it goes to. Measured here as the lowest envelope (the highest |y| of any half cycle) over the ramp, in dB below the lower of the
// 0 dB of the input and the steady level of the kill. Measured (the worst of every kill that leaves the tone audible and four phases): 180 Hz -11 dB on the way in and -17 dB on the way out, 2.8 kHz
// -24 dB in and -21 dB out; at 150 Hz -12 dB and at 100 Hz -3.7 dB. Nothing is changed for it: a shorter ramp moves the dip in time and not in depth (the half-way mix is the same) and raises the click
// (the step bound above is 2 A / ramp length); the unit's by-ear check decides whether a phase-aligned crossfade is worth its cost. The bounds below document the dip, both ways: a change that makes
// it shallower or deeper than the band fails this test and the note above is then rewritten.
TEST_CASE("kill eq (A3 review): the corner tones dip during the crossfade - 180 Hz about -11 to -17 dB, 2.8 kHz about -21 to -24 dB below the lower of the two levels it moves between (documented, not fixed)") {
  struct Dip {
    double in = 1e9;   // the lowest envelope on the way in, in dB under min(0 dB, the kill's steady level)
    double out = 1e9;  // ... and on the way out
  };
  const auto dip_of = [](double hz) {
    Dip worst;
    for (unsigned mask = 1; mask < MasterKillEq::kAll; ++mask) {
      const double steady_db = analytic_db(mask, hz);
      if (steady_db < -40.0) {  // the tone is killed to silence: nothing audible to dip
        continue;
      }
      for (const double phase : {0.0, 1.0, 2.0, 3.5}) {
        Run r;
        std::vector<float> x, y;
        r.go(10, sine(hz, 1.0, phase), x, y);
        r.bypass = false;
        const std::size_t on_at = x.size();
        CHECK(r.f.set(static_cast<float>(mask)));
        r.go(14, sine(hz, 1.0, phase), x, y);
        const std::size_t off_at = x.size();
        r.bypass = false;
        CHECK(r.f.set(0.0f));
        r.go(14, sine(hz, 1.0, phase), x, y);
        // The envelope: the highest |y| in a window of half a period (|sine| repeats every half period, so each window holds one peak). The ramp is 4 blocks long.
        const std::size_t win = static_cast<std::size_t>(std::ceil(kRate / hz / 2.0)) + 1u;
        const auto lowest_env = [&](std::size_t from) {
          double low = 1e9;
          for (std::size_t i = from; i < from + 4u * kB; ++i) {
            double peak = 0;
            for (std::size_t k = 0; k < win; ++k) {
              peak = std::max(peak, static_cast<double>(std::fabs(y[i + k])));
            }
            low = std::min(low, peak);
          }
          return 20.0 * std::log10(low) - std::min(0.0, steady_db);
        };
        worst.in = std::min(worst.in, lowest_env(on_at));
        worst.out = std::min(worst.out, lowest_env(off_at));
      }
    }
    return worst;
  };
  const Dip d180 = dip_of(180.0);
  CHECK_MESSAGE(d180.in <= -8.0, "180 Hz in: " << d180.in);
  CHECK_MESSAGE(d180.in >= -14.0, "180 Hz in: " << d180.in);
  CHECK_MESSAGE(d180.out <= -13.0, "180 Hz out: " << d180.out);
  CHECK_MESSAGE(d180.out >= -20.0, "180 Hz out: " << d180.out);
  const Dip d2800 = dip_of(2800.0);
  CHECK_MESSAGE(d2800.in <= -18.0, "2.8 kHz in: " << d2800.in);
  CHECK_MESSAGE(d2800.in >= -30.0, "2.8 kHz in: " << d2800.in);
  CHECK_MESSAGE(d2800.out <= -18.0, "2.8 kHz out: " << d2800.out);
  CHECK_MESSAGE(d2800.out >= -30.0, "2.8 kHz out: " << d2800.out);
}

// OS-DJ A3 review fold: a kill held over silence leaves no denormal in the ten integrators of a channel (a state that decays into the denormals can stick there at 1 ulp, and the audio core would
// then spend its cycles on them): the block's end flushes any state under 1e-20 to 0. 10 s of silence is 8000 decay constants of the slowest section (1.25 ms, the 180 Hz corner).
TEST_CASE("kill eq (A3 review): a kill held over 10 s of silence leaves every state exactly 0 or a normal number - and a state is never denormal on the way") {
  for (const unsigned mask : {1u, 2u, 4u, 7u}) {
    MasterKillEq f;
    CHECK(f.set(static_cast<float>(mask)));
    float l[kB], r[kB];
    const auto block = [&](bool loud) {
      for (unsigned i = 0; i < kB; ++i) {
        l[i] = loud ? static_cast<float>(std::sin(0.31 * i) * 0.9) : 0.0f;
        r[i] = loud ? static_cast<float>(std::sin(0.57 * i) * 0.6) : 0.0f;
      }
      f.process(l, r, kB);
    };
    const auto audit = [&](const char* when) {
      for (unsigned ch = 0; ch < 2u; ++ch) {
        for (unsigned i = 0; i < 10u; ++i) {
          const float s = f.state(ch, i);
          CHECK_MESSAGE(std::isfinite(s), when << ": mask " << mask << " ch " << ch << " state " << i);
          CHECK_MESSAGE((s == 0.0f || std::fpclassify(s) == FP_NORMAL), when << ": mask " << mask << " ch " << ch << " state " << i << " = " << s);
        }
      }
    };
    for (int b = 0; b < 12; ++b) {  // a loud burst: the ramp is done and the states are alive
      block(true);
    }
    bool alive = false;
    for (unsigned i = 0; i < 10u; ++i) {
      alive = alive || std::fabs(f.state(0, i)) > 1e-3f;
    }
    CHECK(alive);
    const int blocks = static_cast<int>(10.0 * 44117.647 / kB);  // 10 s
    for (int b = 0; b < blocks; ++b) {
      block(false);
      if (b % 64 == 0) {
        audit("on the way");
      }
    }
    audit("after 10 s");
    for (unsigned ch = 0; ch < 2u; ++ch) {
      for (unsigned i = 0; i < 10u; ++i) {
        CHECK_MESSAGE(f.state(ch, i) == 0.0f, "mask " << mask << " ch " << ch << " state " << i << " = " << f.state(ch, i));
      }
    }
    for (unsigned i = 0; i < kB; ++i) {  // and the output is an exact silence
      CHECK(l[i] == 0.0f);
      CHECK(r[i] == 0.0f);
    }
  }
}

TEST_CASE("kill eq (OS-DJ rt): a kill held skips the sections its bands do not need, and the release after a long hold has no click - every mask, 200 held blocks") {
  // Held, a kill runs only the sections its live terms need (LOW alone: sections 1 to 3; LOW + MID and LOW + HIGH: 1 to 4; MID + HIGH: 1, 2 and 5). The sections left out are primed when the
  // ramp runs, so the release has the bound of the exit test above (the sine's own step plus the crossfade's slope) after a hold of 200 blocks.
  constexpr double kAmp = 1.0;
  for (const double hz : {100.0, 1000.0, 2800.0, 5000.0}) {
    for (const double phase : {0.0, 2.0}) {
      for (unsigned mask = 1; mask <= MasterKillEq::kAll; ++mask) {
        Run r;
        std::vector<float> x, y;
        r.go(2, sine(hz, kAmp, phase), x, y);
        r.bypass = false;
        CHECK(r.f.set(static_cast<float>(mask)));
        r.go(200, sine(hz, kAmp, phase), x, y);
        const std::size_t off_at = x.size();
        r.bypass = false;
        CHECK(r.f.set(0.0f));
        r.go(14, sine(hz, kAmp, phase), x, y);
        CHECK(r.bypass);
        const double bound = 1.02 * max_step(x) + 2.0 * kAmp / 221.0;
        const double got = max_step(y, off_at - 1u);
        CHECK_MESSAGE(got <= bound, "off, " << hz << " Hz, phase " << phase << ", mask " << mask << ": " << got << " > " << bound);
        if (mask == MasterKillEq::kAll) {
          for (std::size_t i = off_at - 64u; i < off_at; ++i) {  // the last held block: exactly silence
            CHECK(y[i] == 0.0f);
          }
        }
      }
    }
  }
}
