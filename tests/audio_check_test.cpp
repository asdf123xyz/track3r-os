// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-E2E-2: the measurements behind `assert audio` (sim/include/o3os/sim/audio_check.h), each on synthetic sines, impulses, steps, sweeps and planted clicks, with the accuracy and the
// blind spots stated by a case: pitch to a cent, a click found from a few hundred LSB (and not under the floor), a zipper at every gain step and none on a smooth one, the null level, the
// level curve against the engine's envelope, and the grammar (every error, a pass and a fail plant for each check).
#include <doctest/doctest.h>

#include <o3os/sim/audio_check.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace o3os::sim;

constexpr double kRate = 44117.647;
constexpr double kTau = 6.283185307179586;

std::vector<double> sine(double hz, double amp, double seconds, double rate = kRate, double phase = 0.0) {
  std::vector<double> x(static_cast<std::size_t>(seconds * rate));
  for (std::size_t i = 0; i < x.size(); ++i) {
    x[i] = amp * std::sin(kTau * hz * static_cast<double>(i) / rate + phase);
  }
  return x;
}

// Deterministic noise in [-1, 1) (xorshift32; the same bytes everywhere).
std::vector<double> noise(std::size_t n, double amp, std::uint32_t seed = 12345u) {
  std::vector<double> x(n);
  std::uint32_t s = seed;
  for (double& v : x) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    v = amp * (static_cast<double>(s) / 2147483648.0 - 1.0);
  }
  return x;
}

Signal stereo(const std::vector<double>& l, const std::vector<double>& r) {
  Signal s;
  s.l = l;
  s.r = r;
  s.rate = kRate;
  return s;
}
Signal dual(const std::vector<double>& x) { return stereo(x, x); }

// A Pcm of `x` (both channels), rounded to 16 bits as the codec's frames are.
Pcm pcm_of(const std::vector<double>& l, const std::vector<double>& r) {
  Pcm p;
  p.rate = kRate;
  p.lr.resize(2u * l.size());
  const auto q = [](double v) { return static_cast<std::int16_t>(std::lround(std::max(-32768.0, std::min(32767.0, v * 32768.0)))); };
  for (std::size_t i = 0; i < l.size(); ++i) {
    p.lr[2u * i] = q(l[i]);
    p.lr[2u * i + 1u] = q(r[i]);
  }
  return p;
}
Pcm pcm_of(const std::vector<double>& x) { return pcm_of(x, x); }

double cents(double got, double want) { return 1200.0 * std::log2(got / want); }

}  // namespace

TEST_CASE("audio check: levels, DC and silence") {
  const Signal s = dual(sine(441.0, 0.5, 1.0));
  CHECK(rms_db(s) == doctest::Approx(-9.03).epsilon(0.002));  // 0.5 / sqrt 2 = 0.3536
  CHECK(peak_db(s) == doctest::Approx(-6.02).epsilon(0.002));
  CHECK(dc_offset(s) < 0.002);  // 441 Hz over 441 periods: the mean of the open half cycle is A / (2 pi n)
  CHECK_FALSE(is_silent(s));
  // A DC offset is found at its size, either channel.
  std::vector<double> off = sine(441.0, 0.25, 1.0);
  for (double& v : off) {
    v += 0.05;
  }
  CHECK(dc_offset(stereo(off, sine(441.0, 0.25, 1.0))) == doctest::Approx(0.05).epsilon(0.01));
  CHECK(dc_offset(stereo(sine(441.0, 0.25, 1.0), off)) == doctest::Approx(0.05).epsilon(0.01));
  // Silence: zeros, and one LSB of rounding; two LSB is sound.
  const std::vector<double> zeros(1000, 0.0);
  CHECK(is_silent(dual(zeros)));
  CHECK(rms_db(dual(zeros)) == kSilenceDb);
  std::vector<double> lsb(1000, 0.0);
  lsb[500] = kLsb;
  lsb[501] = -kLsb;
  CHECK(is_silent(dual(lsb)));
  lsb[502] = 2.0 * kLsb;
  CHECK_FALSE(is_silent(dual(lsb)));
  CHECK(is_silent(Signal{}));  // nothing at all is silent
}

TEST_CASE("audio check: pitch to a cent, from 55 Hz to 5 kHz, with silence before and after") {
  for (const double hz : {55.0, 82.4, 110.0, 261.6, 441.176, 1000.0, 2093.0, 4000.0, 5000.0}) {
    INFO("hz " << hz);
    const double got = pitch_hz(sine(hz, 0.4, 0.5), kRate);
    CHECK(std::fabs(cents(got, hz)) < 1.0);
  }
  // The limit, stated: from 5.5 kHz up the period is under 8 samples and the answer is a sub-multiple of the tone, not the tone.
  CHECK(std::fabs(cents(pitch_hz(sine(8000.0, 0.4, 0.5), kRate), 8000.0)) > 100.0);
  // A note in a longer render: 100 ms of silence, 300 ms of tone at 0.3, a quieter 100 ms of another tone (below 6 dB of the loudest, so not the part measured).
  std::vector<double> x(static_cast<std::size_t>(0.1 * kRate), 0.0);
  const std::vector<double> tone = sine(329.63, 0.5, 0.3);
  x.insert(x.end(), tone.begin(), tone.end());
  const std::vector<double> quiet = sine(900.0, 0.1, 0.1);
  x.insert(x.end(), quiet.begin(), quiet.end());
  CHECK(std::fabs(cents(pitch_hz(x, kRate), 329.63)) < 1.0);
  // A sawtooth's fundamental is its repetition rate; a 16-bit rounding of the tone changes nothing.
  std::vector<double> saw(static_cast<std::size_t>(0.3 * kRate));
  for (std::size_t i = 0; i < saw.size(); ++i) {
    saw[i] = 0.4 * (2.0 * std::fmod(196.0 * static_cast<double>(i) / kRate, 1.0) - 1.0);
  }
  CHECK(std::fabs(cents(pitch_hz(saw, kRate), 196.0)) < 3.0);
  const Pcm rounded = pcm_of(sine(440.0, 0.01, 0.5));  // 330 LSB: the 16-bit rounding is 0.3 % of the tone
  CHECK(std::fabs(cents(pitch_hz(to_signal(rounded).mid(), kRate), 440.0)) < 2.0);
  // No pitch: noise, silence, too short.
  CHECK(pitch_hz(noise(20000, 0.5), kRate) == 0.0);
  CHECK(pitch_hz(std::vector<double>(20000, 0.0), kRate) == 0.0);
  CHECK(pitch_hz(sine(440.0, 0.5, 0.01), kRate) == 0.0);
}

TEST_CASE("audio check: clicks are found from a few hundred LSB, a clean tone has none") {
  const std::vector<double> clean = sine(441.176, 0.5, 0.5);
  CHECK(count_clicks(clean) == 0u);
  const auto with_step = [&](double step, std::size_t at) {  // the tone jumps by `step` at `at` and stays there (a DC step)
    std::vector<double> x = clean;
    for (std::size_t i = at; i < x.size(); ++i) {
      x[i] += step;
    }
    return x;
  };
  CHECK(count_clicks(with_step(0.2, 5000)) == 1u);
  CHECK(count_clicks(with_step(0.05, 5000)) == 1u);   // 1638 LSB, -26 dB: the tone's own d2 is 65 LSB, the limit about 12.6 times that (813 LSB)
  CHECK(count_clicks(with_step(0.03, 5000)) == 1u);   // 983 LSB, -30.5 dB
  // A single wrong sample (an impulse) is one click, not two (its second difference has three outliers within 2 samples).
  std::vector<double> spike = clean;
  spike[8000] += 0.1;
  CHECK(count_clicks(spike) == 1u);
  // Two clicks far apart are two; two within 4 samples are one.
  std::vector<double> two = with_step(0.05, 3000);
  for (std::size_t i = 12000; i < two.size(); ++i) {
    two[i] -= 0.05;
  }
  CHECK(count_clicks(two) == 2u);
  // The blind spot, stated: a step under the floor of 24 LSB (-62.7 dB) is not a click, and a quiet tone shows a smaller one than a loud tone.
  CHECK(count_clicks(with_step(10.0 * kLsb, 5000)) == 0u);
  CHECK(count_clicks(with_step(0.02, 5000)) == 0u);   // 655 LSB, -34 dB: under the limit of a tone this loud (a click that small hides in it)
  std::vector<double> quiet = sine(441.176, 0.02, 0.5);  // the same tone 28 dB down: its own d2 is 2.6 LSB, the floor of 24 LSB decides
  for (std::size_t i = 5000; i < quiet.size(); ++i) {
    quiet[i] += 0.002;  // 66 LSB, the same -54 dB step that is invisible on the loud tone
  }
  CHECK(count_clicks(quiet) == 1u);
  // A tone with a smooth fade is clean; a tone cut off at a peak is a click; a tone cut at a zero crossing is not.
  std::vector<double> fade = clean;
  for (std::size_t i = 0; i < fade.size(); ++i) {
    fade[i] *= 1.0 - static_cast<double>(i) / static_cast<double>(fade.size());
  }
  CHECK(count_clicks(fade) == 0u);
  std::vector<double> cut = sine(441.176, 0.5, 0.5, kRate, 1.5707963);  // starts at a peak
  CHECK(count_clicks(std::vector<double>(cut.begin() + 4000, cut.end())) == 0u);  // (the first sample of a signal is no click either: nothing before it)
  std::vector<double> gated = cut;
  std::fill(gated.begin() + 6000, gated.end(), 0.0);
  CHECK(count_clicks(gated) == 1u);
  // The engine's own declick ramps are not clicks: a tone that fades in over 32 samples after silence and out over 256, as voices.h makes a note (the knee where a linear ramp meets the tone is 536 LSB
  // here, a kink and not a step), and the same tone started at its peak with no ramp is one.
  std::vector<double> note(20000, 0.0);
  const std::vector<double> carrier = sine(441.176, 0.5, 0.3);
  for (std::size_t i = 0; i < 12000; ++i) {
    note[4000 + i] = carrier[i] * std::min(1.0, static_cast<double>(i) / 32.0) * std::min(1.0, static_cast<double>(12000 - i) / 256.0);
  }
  CHECK(count_clicks(note) == 0u);
  std::vector<double> abrupt(20000, 0.0);
  const std::vector<double> peaked = sine(441.176, 0.5, 0.3, kRate, 1.5707963);
  for (std::size_t i = 0; i < 12000; ++i) {
    abrupt[4000 + i] = peaked[i];
  }
  CHECK(count_clicks(abrupt) == 2u);  // the start at the peak and the cut off at the end (also at a peak of the cosine: 12000 samples is not a whole number of periods)
  // Silence has none; the stereo count adds the channels.
  CHECK(count_clicks(std::vector<double>(5000, 0.0)) == 0u);
  CHECK(count_clicks(stereo(with_step(0.2, 5000), with_step(0.2, 9000))) == 2u);
  // Noise hides a small one (its own spread is the yardstick) and a large one is still found.
  std::vector<double> n = noise(20000, 0.3);
  CHECK(count_clicks(n) == 0u);
  n[10000] += 10.0;  // d2 of -20 against a limit of about 9 (13 x the 90th percentile of the noise's own d2, 0.69)
  CHECK(count_clicks(n) == 1u);
}

TEST_CASE("audio check: clicks at 110 Hz and 220 Hz, a planted step is found at every phase, the engine's own fade-in is a click below about 350 Hz") {
  for (const double hz : {110.0, 220.0}) {
    INFO("tone of " << hz << " Hz");
    const std::vector<double> clean = sine(hz, 0.5, 0.5);
    CHECK(count_clicks(clean) == 0u);
    const auto with_step = [&](double step, std::size_t at) {
      std::vector<double> x = clean;
      for (std::size_t i = at; i < x.size(); ++i) {
        x[i] += step;
      }
      return x;
    };
    // A step of 655 LSB (-34 dB) is one click wherever it falls in the period (the positions are 7 samples apart over 600: 1.5 periods of the 110 Hz tone), and a step under the floor of 24 LSB is none.
    unsigned found = 0;
    unsigned tried = 0;
    for (std::size_t at = 5000; at < 5600; at += 7) {
      ++tried;
      found += count_clicks(with_step(0.02, at)) == 1u ? 1u : 0u;
      CHECK(count_clicks(with_step(10.0 * kLsb, at)) == 0u);
    }
    CHECK(found == tried);
  }
  // The lower the tone the smaller the step that shows: the tone's own second difference falls with the square of its frequency (65 LSB at 441 Hz, 16 at 220 Hz, 4 at 110 Hz). A step of 131 LSB (-48 dB) is
  // one click at 110 Hz and none at 220 Hz (a limit of about 200 LSB there), and a step of 983 LSB is one at both.
  const std::vector<double> low = sine(110.0, 0.5, 0.5);
  const std::vector<double> mid = sine(220.0, 0.5, 0.5);
  const auto stepped = [](std::vector<double> x, double step) {
    for (std::size_t i = 5000; i < x.size(); ++i) {
      x[i] += step;
    }
    return x;
  };
  CHECK(count_clicks(stepped(low, 0.004)) == 1u);
  CHECK(count_clicks(stepped(mid, 0.004)) == 0u);
  CHECK(count_clicks(stepped(low, 0.03)) == 1u);
  CHECK(count_clicks(stepped(mid, 0.03)) == 1u);
  // The limit, stated: a note with the engine's declick ramps (32 samples in, 256 out) is clean at 441 Hz and has clicks below about 350 Hz. The ramp's kink is the same size whatever the tone, 1/32 of its
  // value at the end of the ramp, and a slow tone has hardly any second difference of its own to hide it in: at 441 Hz the knee is 8 times the tone's d2, at 220 Hz 32 times, at 110 Hz 127 times (the limit is 13).
  // The steady part of the same note, between the ramps, has none: that is the window to measure (`in=`) in a low note.
  for (const double hz : {110.0, 220.0, 441.176}) {
    INFO("note of " << hz << " Hz");
    const std::vector<double> carrier = sine(hz, 0.5, 0.4, kRate, 0.7);
    std::vector<double> note(24000, 0.0);
    for (std::size_t i = 0; i < 12000; ++i) {
      note[4000 + i] = carrier[i] * std::min(1.0, static_cast<double>(i) / 32.0) * std::min(1.0, static_cast<double>(12000 - i) / 256.0);
    }
    if (hz < 300.0) {
      CHECK(count_clicks(note) >= 1u);
    } else {
      CHECK(count_clicks(note) == 0u);
    }
    CHECK(count_clicks(std::vector<double>(note.begin() + 4000 + 64, note.begin() + 4000 + 12000 - 512)) == 0u);
  }
}

TEST_CASE("audio check: onset") {
  std::vector<double> x(static_cast<std::size_t>(0.05 * kRate), 0.0);  // 50 ms of silence
  const std::vector<double> tone = sine(441.176, 0.5, 0.2);
  x.insert(x.end(), tone.begin(), tone.end());
  CHECK(onset_ms(dual(x)) == doctest::Approx(50.0).epsilon(0.02));
  CHECK(onset_ms(dual(std::vector<double>(1000, 0.0))) < 0.0);
  CHECK(onset_ms(dual(sine(441.176, 0.001, 0.2))) < 0.0);  // -60 dB never reaches -50
  // The floor is a parameter: -70 finds the quiet one at its start.
  CHECK(onset_ms(dual(sine(441.176, 0.001, 0.2)), -70.0) >= 0.0);
  // Either channel counts.
  const std::vector<double> silent(x.size(), 0.0);
  CHECK(onset_ms(stereo(silent, x)) == doctest::Approx(50.0).epsilon(0.02));
}

TEST_CASE("audio check: centroid orders brightness, a filter and a bit depth move it") {
  const std::vector<double> bright = noise(44100, 0.5);
  std::vector<double> dark = bright;  // a one-pole low-pass (about 700 Hz)
  double y = 0.0;
  for (double& v : dark) {
    y += 0.1 * (v - y);
    v = y;
  }
  const double cb = centroid_hz(bright, kRate);
  const double cd = centroid_hz(dark, kRate);
  CHECK(cb > 9000.0);  // white noise: the middle of the band, 11 kHz
  CHECK(cb < 13000.0);
  CHECK(cd < 0.6 * cb);  // by magnitude a 1/f spectrum keeps much of its centroid: 5.8 kHz for a pole at 700 Hz
  // A tone's centroid is its pitch (the window's leakage is small).
  CHECK(centroid_hz(sine(1000.0, 0.5, 0.5), kRate) == doctest::Approx(1000.0).epsilon(0.03));
  CHECK(centroid_hz(sine(4000.0, 0.5, 0.5), kRate) > 3.0 * centroid_hz(sine(1000.0, 0.5, 0.5), kRate));
  // Bit reduction adds a harmonic noise floor: the centroid of a quiet low tone rises when it is cut to 4 bits.
  std::vector<double> low = sine(220.0, 0.3, 0.5);
  std::vector<double> crushed = low;
  for (double& v : crushed) {
    v = std::floor(v * 8.0) / 8.0;
  }
  CHECK(centroid_hz(crushed, kRate) > 2.0 * centroid_hz(low, kRate));
  CHECK(centroid_hz(std::vector<double>(5000, 0.0), kRate) == 0.0);
  CHECK(centroid_hz(std::vector<double>(), kRate) == 0.0);
}

TEST_CASE("audio check: pan") {
  const std::vector<double> t = sine(441.0, 0.5, 0.2);
  const std::vector<double> none(t.size(), 0.0);
  CHECK(pan_balance_db(stereo(t, none)) >= 100.0);   // hard left: the far side exactly zero reads as the floor
  CHECK(pan_balance_db(stereo(none, t)) <= -100.0);
  CHECK(std::fabs(pan_balance_db(dual(t))) < 1e-9);
  std::vector<double> half = t;
  for (double& v : half) {
    v *= 0.5;
  }
  CHECK(pan_balance_db(stereo(t, half)) == doctest::Approx(6.02).epsilon(0.002));
}

TEST_CASE("audio check: zipper, a stepped gain has a spike at every step and a smooth one has none") {
  const std::size_t n = static_cast<std::size_t>(1.0 * kRate);
  const std::vector<double> carrier = sine(441.176, 0.4, 1.0);
  std::vector<double> smooth = carrier;
  std::vector<double> stairs = carrier;
  std::vector<double> big = carrier;
  for (std::size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(n);
    smooth[i] *= 0.5 + 0.5 * t;                                        // a linear fade in
    stairs[i] *= 0.5 + 0.5 * std::floor(t * 8.0) / 8.0;                // 8 steps of 6 % of the full gain
    big[i] *= 0.4 + 0.6 * std::floor(t * 4.0) / 4.0;                   // 4 steps of 15 %
  }
  CHECK(count_zipper(smooth, kRate) == 0u);
  CHECK(count_zipper(carrier, kRate) == 0u);
  CHECK(count_zipper(big, kRate) == 3u);  // the steps inside the signal: at 1/4, 1/2, 3/4 (the first is at 0)
  // A staircase of small steps is found at most of its steps (7 inside the signal) and never more than that.
  const unsigned found = count_zipper(stairs, kRate);
  CHECK(found <= 7u);
  CHECK(found >= 3u);
  // A swept tone (a chirp with a smooth amplitude) has none: the spectrum moves, the flux stays small.
  std::vector<double> chirp(n);
  for (std::size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / kRate;
    chirp[i] = 0.4 * std::sin(kTau * (200.0 * t + 400.0 * t * t));
  }
  CHECK(count_zipper(chirp, kRate) == 0u);
  CHECK(count_zipper(std::vector<double>(100, 0.0), kRate) == 0u);  // too short
}

TEST_CASE("audio check: alias, the energy outside the harmonics of a tone") {
  const std::vector<double> clean = sine(3000.0, 0.5, 0.5);
  CHECK(alias_db(clean, kRate) < -95.0);  // a double sine: the Blackman-Harris window's skirt beyond 6 bins only
  // A planted component off the harmonic grid, 40 dB under the tone.
  std::vector<double> dirty = clean;
  const std::vector<double> spur = sine(7777.0, 0.005, 0.5);
  for (std::size_t i = 0; i < dirty.size(); ++i) {
    dirty[i] += spur[i];
  }
  CHECK(alias_db(dirty, kRate) == doctest::Approx(-40.0).epsilon(0.05));
  // A harmonic is the signal's own: a square-ish tone (third harmonic at -10 dB) is not alias.
  std::vector<double> rich = clean;
  const std::vector<double> third = sine(9000.0, 0.15, 0.5);
  for (std::size_t i = 0; i < rich.size(); ++i) {
    rich[i] += third[i];
  }
  CHECK(alias_db(rich, kRate) < -88.0);
  // The 16-bit rounding of the tone is a floor of its own: -98 dB of a full-scale sine, 6 dB more against this one (the measure reads -92).
  CHECK(alias_db(to_signal(pcm_of(clean)).mid(), kRate) < -88.0);
  CHECK(alias_db(to_signal(pcm_of(clean)).mid(), kRate) > -96.0);
  // Nothing to measure is NaN, not a figure: a signal under 8192 samples, and silence.
  CHECK(std::isnan(alias_db(sine(440.0, 0.5, 0.05), kRate)));
  CHECK(std::isnan(alias_db(std::vector<double>(9000, 0.0), kRate)));
}

TEST_CASE("audio check: null") {
  const std::vector<double> a = sine(441.0, 0.5, 0.3);
  CHECK(null_db(dual(a), dual(a)) == kSilenceDb);
  std::vector<double> b = a;
  const std::vector<double> n = noise(a.size(), 0.0005);  // about -70 dB re the tone's 0.354 rms
  for (std::size_t i = 0; i < b.size(); ++i) {
    b[i] += n[i];
  }
  const double residual = null_db(dual(a), dual(b));
  CHECK(residual < -55.0);
  CHECK(residual > -70.0);
  CHECK(null_db(dual(a), dual(std::vector<double>(a.size(), 0.0))) == doctest::Approx(0.0).epsilon(0.01));  // against nothing: the whole of it
  CHECK(null_db(dual(std::vector<double>(100, 0.0)), dual(a)) > 100.0);
  // Nothing compared is NaN, not a perfect null: silence against silence, no frame (a lag as long as the signal), and nothing at all.
  CHECK(std::isnan(null_db(dual(std::vector<double>(100, 0.0)), dual(std::vector<double>(100, 0.0)))));
  CHECK(std::isnan(null_db(dual(a), dual(a), a.size())));
  CHECK(std::isnan(null_db(Signal(), Signal())));
  // A lag: the same tone 63 frames later against the original, and the sum of two halves against the whole.
  std::vector<double> late(63, 0.0);
  late.insert(late.end(), a.begin(), a.end());
  CHECK(null_db(dual(late), dual(a), 63) == kSilenceDb);
  CHECK(null_db(dual(late), dual(a), 0) > -10.0);
  const std::vector<double> half(a.size(), 0.0);
  std::vector<double> first = a, second = half;
  for (std::size_t i = a.size() / 2u; i < a.size(); ++i) {
    second[i] = a[i];
    first[i] = 0.0;
  }
  CHECK(null_db(dual(a), sum_signals({dual(first), dual(second)})) == kSilenceDb);
  CHECK(sum_signals({}).size() == 0u);
  CHECK(sum_signals({dual(first), dual(std::vector<double>(100, 1.0))}).size() == 100u);
}

TEST_CASE("audio check: the level curve against the engine's envelope") {
  // A tone shaped by an ADSR of the engine's kind: attack 40 ms, decay 200 ms to 0.6, release 300 ms, released 500 ms after the start of the note at 20 ms.
  const Adsr model{40.0, 200.0, 0.6, 300.0, 500.0};
  const std::size_t lead = static_cast<std::size_t>(0.02 * kRate);
  const std::vector<double> carrier = sine(441.176, 0.5, 1.0);
  std::vector<double> x(carrier.size(), 0.0);
  for (std::size_t i = lead; i < x.size(); ++i) {
    x[i] = carrier[i] * adsr_level(model, 1000.0 * static_cast<double>(i - lead) / kRate);
  }
  CHECK(envelope_error(dual(x), model) < 0.06);
  // The same sound against other models: a slower attack, a higher sustain, a later release are each off by more than 0.2.
  CHECK(envelope_error(dual(x), Adsr{200.0, 200.0, 0.6, 300.0, 500.0}) > 0.2);
  CHECK(envelope_error(dual(x), Adsr{40.0, 200.0, 0.9, 300.0, 500.0}) > 0.2);
  CHECK(envelope_error(dual(x), Adsr{40.0, 200.0, 0.6, 300.0, 700.0}) > 0.2);
  CHECK(envelope_error(dual(std::vector<double>(5000, 0.0)), model) < 0.0);  // never starts
  // The model itself.
  CHECK(adsr_level(model, -1.0) == 0.0);
  CHECK(adsr_level(model, 20.0) == doctest::Approx(0.5));
  CHECK(adsr_level(model, 40.0) == doctest::Approx(1.0));
  CHECK(adsr_level(model, 140.0) == doctest::Approx(0.8));
  CHECK(adsr_level(model, 300.0) == doctest::Approx(0.6));
  CHECK(adsr_level(model, 650.0) == doctest::Approx(0.3));
  CHECK(adsr_level(model, 800.0) == 0.0);
  CHECK(adsr_level(Adsr{0.0, 0.0, 1.0, 0.0, 100.0}, 99.0) == 1.0);  // a stage of 0 ms is a step
  CHECK(adsr_level(Adsr{0.0, 0.0, 1.0, 0.0, 100.0}, 100.0) == 0.0);
}

TEST_CASE("audio check: a window of a render, the files") {
  Pcm p = pcm_of(sine(441.176, 0.5, 1.0));
  const Signal all = to_signal(p);
  const Signal part = to_signal(p, 100.0, 300.0);
  CHECK(all.size() == p.frames());
  CHECK(part.size() == doctest::Approx(0.2 * kRate).epsilon(0.001));
  CHECK(to_signal(p, 900.0, 5000.0).size() == doctest::Approx(0.1 * kRate).epsilon(0.01));  // clipped to what there is
  CHECK(to_signal(p, 2000.0).size() == 0u);
  // The WAV: a 44-byte header, then the frames little-endian.
  Pcm tiny;
  tiny.rate = 44117.647;
  tiny.lr = {1, -1, 256, -2};
  const std::vector<std::uint8_t> wav = wav_bytes(tiny);
  if (wav.size() != 44u + 8u) {
    CHECK(wav.size() == 44u + 8u);
    return;
  }
  CHECK(std::string(wav.begin(), wav.begin() + 4) == "RIFF");
  CHECK(std::string(wav.begin() + 8, wav.begin() + 12) == "WAVE");
  CHECK(wav[22] == 2);                           // channels
  CHECK(wav[24] == (44118 & 0xFF));              // the rate, to the nearest Hz
  CHECK(wav[25] == ((44118 >> 8) & 0xFF));
  CHECK(wav[40] == 8);                           // data bytes
  CHECK(wav[44] == 1);
  CHECK(wav[45] == 0);
  CHECK(wav[46] == 0xFF);
  CHECK(wav[47] == 0xFF);
  CHECK(wav[48] == 0);
  CHECK(wav[49] == 1);
  // The hash is the samples' and moves with one of them.
  const std::string h = pcm_sha256(tiny);
  CHECK(h.size() == 64u);
  tiny.lr[3] = -3;
  CHECK(pcm_sha256(tiny) != h);
}

namespace {

AudioAssert parsed(const std::vector<std::string>& words) {
  AudioAssert a;
  std::string error;
  INFO("parse: " << (words.empty() ? "" : words[0]) << " ...");
  CHECK_MESSAGE(parse_audio_assert(words, a, error), error);
  return a;
}
std::string parse_error(const std::vector<std::string>& words) {
  AudioAssert a;
  std::string error;
  CHECK_FALSE(parse_audio_assert(words, a, error));
  return error;
}

struct Renders {
  Pcm tone, tone_quiet, silent, left, noisy, dark;
  Renders() {
    tone = pcm_of(sine(441.176, 0.5, 1.0));
    tone_quiet = pcm_of(sine(441.176, 0.25, 1.0));  // 6.02 dB under
    silent.rate = kRate;
    silent.lr.assign(2000, 0);
    const std::vector<double> t = sine(441.176, 0.5, 1.0);
    left = pcm_of(t, std::vector<double>(t.size(), 0.0));
    const std::vector<double> n = noise(44100, 0.5);
    std::vector<double> d = n;
    double y = 0.0;
    for (double& v : d) {
      y += 0.1 * (v - y);
      v = y;
    }
    noisy = pcm_of(n);
    dark = pcm_of(d);
  }
  const Pcm* find(const std::string& name) const {
    if (name == "tone") return &tone;
    if (name == "quiet") return &tone_quiet;
    if (name == "silent") return &silent;
    if (name == "left") return &left;
    if (name == "noisy") return &noisy;
    if (name == "dark") return &dark;
    return nullptr;
  }
};

std::string verdict(const Renders& r, const std::vector<std::string>& words, std::string* summary = nullptr) {
  const AudioAssert a = parsed(words);
  return audio_problem(a, [&r](const std::string& name) { return r.find(name); }, summary);
}

}  // namespace

TEST_CASE("audio check: the grammar, every error") {
  CHECK(parse_error({}).find("needs the name of a render") != std::string::npos);
  CHECK(parse_error({"tone"}).find("no check") != std::string::npos);
  CHECK(parse_error({"tone", "loud"}).find("'loud' is no check") != std::string::npos);
  CHECK(parse_error({"tone", "volume=3"}).find("unknown check 'volume'") != std::string::npos);
  CHECK(parse_error({"tone", "pitch=441"}).find("pitch=<Hz>") != std::string::npos);
  CHECK(parse_error({"tone", "pitch=441+-3"}).find("in cents") != std::string::npos);
  CHECK(parse_error({"tone", "pitch=abc+-3c"}).find("is not a number") != std::string::npos);
  CHECK(parse_error({"tone", "pitch=m200+-3c"}).find("MIDI note 0..127") != std::string::npos);
  CHECK(parse_error({"tone", "pitch<441+-3c"}).find("pitch=<Hz>") != std::string::npos);
  CHECK(parse_error({"tone", "rms=-9"}).find("needs a tolerance") != std::string::npos);
  CHECK(parse_error({"tone", "rms<-9+-1"}).find("take no tolerance") != std::string::npos);
  CHECK(parse_error({"tone", "rms<nan"}).find("is not a number") != std::string::npos);
  CHECK(parse_error({"tone", "rms<@"}).find("needs the name of a render") != std::string::npos);
  CHECK(parse_error({"tone", "rms<@quiet-x"}).find("comes a number") != std::string::npos);
  CHECK(parse_error({"tone", "clicks=1.5"}).find("clicks=<count>") != std::string::npos);
  CHECK(parse_error({"tone", "clicks>1"}).find("clicks=<count>") != std::string::npos);
  CHECK(parse_error({"tone", "zipper<2"}).find("zipper=<count>") != std::string::npos);
  CHECK(parse_error({"tone", "onset=50"}).find("needs a tolerance") != std::string::npos);
  CHECK(parse_error({"tone", "envelope=adsr(a=1)+-0.1"}).find("off=<ms>") != std::string::npos);
  CHECK(parse_error({"tone", "envelope=adsr(a=1,off=2,x=3)+-0.1"}).find("unknown key 'x'") != std::string::npos);
  CHECK(parse_error({"tone", "envelope=adsr(a=1,off=2,s=2)+-0.1"}).find("s is a level 0..1") != std::string::npos);
  CHECK(parse_error({"tone", "envelope=ramp+-0.1"}).find("envelope wants adsr") != std::string::npos);
  CHECK(parse_error({"tone", "pan=X"}).find("pan=<dB>+-<d>") != std::string::npos);
  CHECK(parse_error({"tone", "pan=L+-3"}).find("take no tolerance") != std::string::npos);
  CHECK(parse_error({"tone", "pan=C"}).find("needs a tolerance") != std::string::npos);
  CHECK(parse_error({"tone", "null=quiet"}).find("null=<other render>") != std::string::npos);
  CHECK(parse_error({"tone", "null=quiet,-30,lag=-1"}).find("lag=<frames 0..4096>") != std::string::npos);
  CHECK(parse_error({"tone", "null=quiet,-30,x=3"}).find("lag=<frames 0..4096>") != std::string::npos);
  CHECK(parse_error({"tone", "null=quiet+,-30"}).find("an empty render name") != std::string::npos);
  CHECK(parse_error({"tone", "null=,-30"}).find("null=<other render>") != std::string::npos);
  CHECK(parse_error({"tone", "alias=-40"}).find("alias<dB") != std::string::npos);
  CHECK(parse_error({"tone", "in=300-100", "silent"}).find("in= wants") != std::string::npos);
  CHECK(parse_error({"tone", "in=100", "silent"}).find("in= wants") != std::string::npos);
  CHECK(parse_error({"tone", "in=100-end"}).find("no check") != std::string::npos);
  // The accepted forms: the symbol ± as UTF-8 and as +-, a note, a reference with an offset, a window.
  const AudioAssert a = parsed({"tone", "in=100-end", "pitch=m69\xC2\xB1" "5c", "rms=-9+-1", "peak<-6", "centroid<@dark*0.5", "rms>@quiet-3", "null=quiet,-6", "clicks=0", "clicks<2", "pan=C+-1", "pan=-6+-1", "pan=L",
                                "envelope=adsr(a=40,d=200,s=0.6,r=300,off=500)+-0.1", "zipper=0", "alias<-40", "dc<0.002", "silent", "sound", "onset=0+-2"});
  CHECK(a.from_ms == 100.0);
  CHECK(a.to_ms < 0.0);
  if (a.checks.size() != 18u) {
    CHECK(a.checks.size() == 18u);
    return;
  }
  CHECK(a.checks[0].value == doctest::Approx(440.0));
  CHECK(a.checks[0].tol == 5.0);
  CHECK(a.checks[3].ref == "dark");
  CHECK(a.checks[3].op == '*');
  CHECK(a.checks[3].operand == 0.5);
  CHECK(a.checks[4].op == '-');
  CHECK(a.checks[5].others.size() == 1u);
  CHECK(a.checks[11].model.attack_ms == 40.0);
  CHECK(a.checks[11].model.sustain == 0.6);
  CHECK(a.checks[11].model.off_ms == 500.0);
  const AudioAssert w = parsed({"tone", "in=0-250", "silent"});
  CHECK(w.to_ms == 250.0);
}

TEST_CASE("audio check: every check, a pass and a fail plant") {
  const Renders r;
  const auto passes = [&](const std::vector<std::string>& w) {
    const std::string why = verdict(r, w);
    INFO(w[1] << ": " << why);
    return why.empty();
  };
  const auto fails = [&](const std::vector<std::string>& w, const char* part) {
    const std::string why = verdict(r, w);
    INFO(w[1] << ": " << why);
    return why.find(part) != std::string::npos;
  };
  // pitch
  CHECK(passes({"tone", "pitch=441.18+-3c"}));
  CHECK(passes({"tone", "pitch=m69+-40c"}));              // 440 Hz is 7 cents under 441.18
  CHECK(passes({"tone", "pitch=@quiet+-1c"}));
  CHECK(fails({"tone", "pitch=500+-5c"}, "measured 441.2 Hz, wanted 500 Hz"));
  CHECK(fails({"tone", "pitch=441.18+-0.01c"}, "cents off"));
  CHECK(fails({"noisy", "pitch=441+-5c"}, "no periodic part"));
  // levels
  CHECK(passes({"tone", "rms=-9.03+-0.1", "peak=-6.02+-0.1", "peak<-6", "peak>-7", "rms>-10", "rms<-9"}));
  CHECK(fails({"tone", "rms=-12+-1"}, "measured -9.03"));
  CHECK(fails({"tone", "peak<-7"}, "wanted under -7 dB"));
  CHECK(fails({"tone", "peak>-5"}, "wanted over -5 dB"));
  CHECK(passes({"quiet", "rms<@tone-5", "rms>@tone-7", "peak=@tone-6.02+-0.1"}));
  CHECK(passes({"quiet", "rms<@tone"}));
  CHECK(fails({"tone", "rms<@quiet"}, "wanted under -15.05 dB"));
  CHECK(fails({"tone", "rms<@nothing"}, "no finished render 'nothing'"));
  // dc
  CHECK(passes({"tone", "dc<0.002"}));
  CHECK(fails({"tone", "dc<0.0000001"}, "wanted under"));
  // silence
  CHECK(passes({"silent", "silent"}));
  CHECK(passes({"tone", "sound"}));
  CHECK(fails({"tone", "silent"}, "not silent"));
  CHECK(fails({"silent", "sound"}, "silent"));
  // clicks
  CHECK(passes({"tone", "clicks=0", "clicks<1"}));
  CHECK(fails({"tone", "clicks=1"}, "measured 0 click(s), wanted 1"));
  CHECK(fails({"tone", "clicks<0"}, "fewer than 0"));
  // onset
  CHECK(passes({"tone", "onset=0+-0.2"}));
  CHECK(fails({"tone", "onset=20+-1"}, "measured 0"));
  CHECK(fails({"silent", "onset=0+-1"}, "nothing to measure"));
  // centroid
  CHECK(passes({"dark", "centroid<@noisy*0.7"}));
  CHECK(passes({"noisy", "centroid>@dark*1.5", "centroid>5000"}));
  CHECK(fails({"noisy", "centroid<@dark"}, "wanted under"));
  CHECK(fails({"silent", "centroid<5000"}, "nothing to measure"));  // a silent render has no centre of brightness
  CHECK(fails({"silent", "centroid>0"}, "nothing to measure"));
  // pan
  CHECK(passes({"tone", "pan=C+-0.1"}));
  CHECK(fails({"silent", "pan=C+-6"}, "nothing to measure (both channels are silent"));  // silence is no balance
  CHECK(fails({"silent", "pan=L"}, "nothing to measure"));
  CHECK(fails({"silent", "pan=0+-6"}, "nothing to measure"));
  CHECK(passes({"left", "pan=L"}));
  CHECK(passes({"left", "pan=100+-50"}));
  CHECK(fails({"tone", "pan=L"}, "the balance (20 log10 L/R) is 0 dB"));
  CHECK(fails({"left", "pan=R"}, "the balance"));
  CHECK(fails({"left", "pan=C+-6"}, "the balance"));
  // zipper and alias
  CHECK(passes({"tone", "zipper=0"}));
  CHECK(fails({"tone", "zipper=1"}, "measured 0 spectral-flux"));
  CHECK(fails({"silent", "zipper=0"}, "nothing to measure (the render is silent"));  // no spikes in silence is no smoothness
  CHECK(fails({"tone", "in=0-10", "zipper=0"}, "too short to measure (441 frames, at least 640)"));
  CHECK(fails({"tone", "in=0-100", "alias<-60"}, "nothing to measure (it needs a tone and at least 8192 frames in the window, this one has 4412)"));
  CHECK(fails({"silent", "alias<-60"}, "nothing to measure"));
  CHECK(fails({"silent", "alias>-10"}, "nothing to measure"));
  CHECK(fails({"tone", "alias<@silent"}, "'silent' has no alias to compare with"));
  CHECK(passes({"tone", "alias<-60"}));
  CHECK(fails({"tone", "alias<-200"}, "wanted under -200 dB"));
  // null
  CHECK(passes({"tone", "null=tone,-100"}));
  CHECK(fails({"tone", "null=quiet,-30"}, "the residual is -6.02"));
  CHECK(fails({"tone", "null=silent,-30"}, "differ in length"));
  CHECK(fails({"tone", "null=nothing,-30"}, "no finished render 'nothing'"));
  CHECK(fails({"silent", "null=silent,-30"}, "nothing to compare (this render and the others are all silent"));
  CHECK(fails({"tone", "in=0-10", "null=tone,-30,lag=441"}, "the lag of 441 frames leaves nothing to compare in a window of 441"));
  CHECK(passes({"tone", "null=quiet+quiet,-80"}));  // two quiet ones add to the tone, but for the 16-bit rounding of each (under 1 LSB)
  CHECK(fails({"tone", "null=quiet+quiet+quiet,-30"}, "the residual is"));
  CHECK(passes({"tone", "null=tone,-100,lag=0"}));
  CHECK(fails({"tone", "null=tone,-100,lag=100"}, "the residual is"));
  // envelope: the tone is flat, a model with sustain 1 and a late release fits; one with a short sustain does not
  CHECK(passes({"tone", "in=0-800", "envelope=adsr(a=0,d=0,s=1,r=0,off=2000)+-0.15"}));
  CHECK(fails({"tone", "in=0-800", "envelope=adsr(a=300,d=0,s=1,r=0,off=2000)+-0.15"}, "off the model"));
  // windows and missing renders
  CHECK(passes({"tone", "in=100-300", "onset=0+-0.2"}));
  CHECK(fails({"nothing", "silent"}, "no finished render 'nothing'"));
  CHECK(fails({"tone", "in=5000-6000", "silent"}, "no frames in the window"));
  // The summary says what was measured, one phrase a check.
  std::string summary;
  CHECK(verdict(r, {"tone", "pitch=441.18+-3c", "rms<-9", "clicks=0"}, &summary).empty());
  CHECK(summary.find("pitch 441.2 Hz") != std::string::npos);
  CHECK(summary.find("rms -9.03") != std::string::npos);
  CHECK(summary.find("clicks 0") != std::string::npos);
}
