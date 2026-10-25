// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-AMG1: the AMIGA conversion's resampler (app/amiga_convert.h): the length and the rates, the response of the low-pass (a tone below the new Nyquist kept, one above it
// attenuated), the length and the points of the converted sample, the 8-bit quantise, and the pitch the Amiga voice plays it at (C-4, both rates). The job and the screen's
// keys are in sample_edit_screen_test.cpp.
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/static_slot.h"

#include <o3os/app/amiga_convert.h>
#include <o3os/app/sample_dsp.h>
#include <o3os/app/sample_edit_job.h>
#include <o3os/app/wave_math.h>
#include <o3os/audio/amiga_voices.h>
#include <o3os/audio/voices.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

using namespace o3os::app;
namespace audio = o3os::audio;
namespace haudio = o3os::hal::audio;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kEngineHz = haudio::sample_rate_mhz() / 1000.0;
constexpr std::uint32_t kEngineSecond = haudio::sample_rate_mhz() / 1000u + 1u;  // one second of the engine's rate in frames, rounded up (44,118 at the old 44,117.647 Hz; 44,101 at 44,100 Hz)

const AmigaKernel& kernel() {
  static AmigaKernel k;
  while (!k.ready()) {
    (void)k.build(256);
  }
  return k;
}

std::vector<std::int16_t> convert(const std::vector<std::int16_t>& src, unsigned period, bool src_amiga, bool q8 = false) {
  const std::uint32_t frames = static_cast<std::uint32_t>(src.size());
  const std::uint64_t step = amiga_step_q32(period, src_amiga);
  std::vector<std::int16_t> out(amiga_frames(frames, step));
  amiga_resample(kernel(), src.data(), frames, step, q8, out.data(), 0, static_cast<std::uint32_t>(out.size()));
  return out;
}

std::vector<std::int16_t> tone(std::uint32_t frames, double cycles_per_frame, double amp = 10000.0) {
  std::vector<std::int16_t> v(frames);
  for (std::uint32_t i = 0; i < frames; ++i) {
    v[i] = static_cast<std::int16_t>(std::lround(amp * std::sin(2.0 * kPi * cycles_per_frame * i + 0.7)));
  }
  return v;
}

// The level in dB, against `amp`, of the component at `cycles_per_frame` in the middle of `v` (the ends, where the filter meets the silence outside, are left out).
double level_db(const std::vector<std::int16_t>& v, double cycles_per_frame, double amp = 10000.0) {
  const std::size_t from = 200;
  const std::size_t to = v.size() - 200;
  double s = 0;
  double c = 0;
  for (std::size_t i = from; i < to; ++i) {
    s += v[i] * std::sin(2.0 * kPi * cycles_per_frame * static_cast<double>(i));
    c += v[i] * std::cos(2.0 * kPi * cycles_per_frame * static_cast<double>(i));
  }
  const double a = 2.0 * std::sqrt(s * s + c * c) / static_cast<double>(to - from);
  return 20.0 * std::log10((a + 1e-9) / amp);
}

}  // namespace

TEST_CASE("amiga convert: the rates, the ratio and the length") {
  CHECK(amiga_rate_hz(kAmigaPeriod8) == 8287u);
  CHECK(amiga_rate_hz(kAmigaPeriod16) == 16574u);
  const double ratio8 = static_cast<double>(amiga_step_q32(kAmigaPeriod8, false)) / 4294967296.0;
  CHECK(ratio8 == doctest::Approx(kEngineHz / (3546895.0 / 428.0)).epsilon(1e-9));  // the engine rate over 8,287.14 Hz
  CHECK(amiga_step_q32(kAmigaPeriod8, true) == (std::uint64_t{1} << 32));            // an Amiga sample converted to 8,287 is what it is
  CHECK(amiga_step_q32(kAmigaPeriod16, true) == (std::uint64_t{1} << 31));           // ... and to 16,574 it doubles
  CHECK(amiga_frames(kEngineSecond, amiga_step_q32(kAmigaPeriod8, false)) == 8287u);  // one second of the engine's rate is one second of Paula's
  CHECK(amiga_frames(kEngineSecond, amiga_step_q32(kAmigaPeriod16, false)) == 16574u);
  CHECK(amiga_frames(3, amiga_step_q32(kAmigaPeriod8, false)) == 1u);                 // never empty
  CHECK(amiga_frames(1000, amiga_step_q32(kAmigaPeriod16, true)) == 2000u);
  // OS-RATEPITCH: a Modern sample plays at its own rate, so that is the rate converted from: a rate of 0 or 44,100 is the engine's (as above), 22,050 Hz is half of it
  CHECK(amiga_step_q32(kAmigaPeriod8, false, 0) == amiga_step_q32(kAmigaPeriod8, false));
  CHECK(amiga_step_q32(kAmigaPeriod8, false, 44100) == amiga_step_q32(kAmigaPeriod8, false));
  const std::int64_t half = static_cast<std::int64_t>(amiga_step_q32(kAmigaPeriod8, false, 22050)) - static_cast<std::int64_t>(amiga_step_q32(kAmigaPeriod8, false) / 2);
  CHECK(std::llabs(half) <= 1024);  // half the engine's step, to the integer mHz of the rate (2e-8 of it)
  const std::uint32_t one_second = amiga_frames(static_cast<std::uint32_t>(22050.0 * kEngineHz / 44100.0) + 1u, amiga_step_q32(kAmigaPeriod8, false, 22050));  // one second of a 22,050 Hz sample is one second of Paula's (the engine's second of it, rounded up: 22,059 at 44,117.647 Hz, 22,051 at 44,100 Hz)
  CHECK((one_second >= 8286u && one_second <= 8288u));
  CHECK(amiga_step_q32(kAmigaPeriod8, true, 22050) == amiga_step_q32(kAmigaPeriod8, true));  // an Amiga source: the period's pitch, not the rate's
}

TEST_CASE("amiga convert: a tone below the new Nyquist keeps its level, one above it is attenuated and does not fold back (both rates)") {
  struct Case {
    unsigned period;
    bool src_amiga;
    double rho;  // source frames per output frame
  };
  // rho follows the engine's rate (5.3215 and 2.6608 at 44,100 Hz; they were 5.3236 and 2.6618 at 44,117.647 Hz)
  const Case cases[] = {{kAmigaPeriod8, false, kEngineHz / (3546895.0 / 428.0)}, {kAmigaPeriod16, false, kEngineHz / (3546895.0 / 214.0)}, {kAmigaPeriod16, true, 0.5}};
  for (const Case& c : cases) {
    const std::uint32_t frames = 30000;
    // The passband: phi is the fraction of the LOWER of the two rates (the new one for a decimation, the source's for an interpolation).
    for (const double phi : {0.10, 0.30, 0.40}) {
      const double f_src = c.rho > 1.0 ? phi / c.rho : phi;  // cycles per source frame
      const std::vector<std::int16_t> out = convert(tone(frames, f_src), c.period, c.src_amiga);
      const double db = level_db(out, f_src * c.rho);
      INFO("period " << c.period << " src_amiga " << c.src_amiga << " phi " << phi << ": " << db << " dB");
      CHECK(std::fabs(db) < 0.05);
    }
    {  // the corner: -6 dB at 0.45 of the lower rate
      const double f_src = c.rho > 1.0 ? 0.45 / c.rho : 0.45;
      const double db = level_db(convert(tone(frames, f_src), c.period, c.src_amiga), f_src * c.rho);
      CHECK(db == doctest::Approx(-6.0).epsilon(0.05));
    }
    if (c.rho > 1.0) {  // the stopband: above the new Nyquist, where the tone would fold to 1 - phi of the output rate
      for (const double phi : {0.52, 0.55, 0.60, 0.70}) {
        const std::vector<std::int16_t> out = convert(tone(frames, phi / c.rho), c.period, c.src_amiga);
        const double db = level_db(out, 1.0 - phi);
        INFO("period " << c.period << " phi " << phi << " folded: " << db << " dB");
        CHECK(db < -85.0);
      }
    }
  }
}

TEST_CASE("amiga convert: DC is exact, silence stays silent, a step is not wrapped, the ends read silence outside the sample") {
  std::vector<std::int16_t> dc(20000, 12000);
  const std::vector<std::int16_t> out = convert(dc, kAmigaPeriod8, false);
  for (std::size_t i = 100; i + 100 < out.size(); ++i) {
    if (out[i] != 12000) {
      FAIL_CHECK("frame " << i << " = " << out[i]);
      break;
    }
  }
  CHECK(out.front() < 12000);  // the edge: the filter meets the silence before the sample
  for (std::int16_t v : convert(std::vector<std::int16_t>(5000, 0), kAmigaPeriod16, false)) {
    CHECK(v == 0);
  }
}

TEST_CASE("amiga convert: the chunks are the same as the whole, whatever the cut") {
  const std::vector<std::int16_t> src = tone(9000, 0.021);
  const std::vector<std::int16_t> whole = convert(src, kAmigaPeriod8, false);
  const std::uint64_t step = amiga_step_q32(kAmigaPeriod8, false);
  std::vector<std::int16_t> cut(whole.size());
  std::uint32_t at = 0;
  for (std::uint32_t n : {1u, 7u, 100u, 3u}) {
    while (at < cut.size()) {
      const std::uint32_t m = at + n > cut.size() ? static_cast<std::uint32_t>(cut.size()) - at : n;
      amiga_resample(kernel(), src.data(), 9000, step, false, cut.data() + at, at, m);
      at += m;
      if (n == 3u) {
        continue;
      }
      break;
    }
  }
  CHECK(cut == whole);
}

TEST_CASE("amiga convert: the points, the loop and the slices are fractions of the length: they land on the same instant to within a frame of the new rate") {
  const std::uint32_t old_frames = 44118;
  const std::uint32_t new_frames = amiga_frames(old_frames, amiga_step_q32(kAmigaPeriod8, false));
  for (unsigned point : {0u, 1000u, 16384u, 32768u, 49152u, 65535u}) {
    const double t_old = static_cast<double>(point_to_frame(point, old_frames)) / kEngineHz;
    const double t_new = static_cast<double>(point_to_frame(point, new_frames)) / (3546895.0 / 428.0);
    CHECK(std::fabs(t_old - t_new) < 1.0 / 8287.0);
  }
}

TEST_CASE("amiga convert: the 8-bit option rounds to the nearest level (plain, no dither) and is what Paula reads") {
  const std::vector<std::int16_t> src = tone(20000, 0.013, 30000.0);
  const std::vector<std::int16_t> plain = convert(src, kAmigaPeriod8, false, false);
  const std::vector<std::int16_t> q = convert(src, kAmigaPeriod8, false, true);
  NEED(plain.size() == q.size());
  std::int32_t worst = 0;
  for (std::size_t i = 0; i < q.size(); ++i) {
    CHECK(q[i] % 256 == 0);
    CHECK((q[i] >> 8) >= -128);
    CHECK((q[i] >> 8) <= 127);
    const std::int32_t e = q[i] - plain[i];
    worst = std::abs(e) > worst ? std::abs(e) : worst;
  }
  CHECK(worst <= 128);       // half a level
  CHECK(worst >= 100);       // ... and it does use the whole range of the rounding
  CHECK(convert(std::vector<std::int16_t>(4000, 32767), kAmigaPeriod8, false, true)[375] == 127 * 256);  // full scale is level 127
}

namespace {

// The frequency in Hz of the positive-going zero crossings of `v` (sampled at the engine's rate), by linear interpolation between the first and the last. A crossing counts
// after the signal was below -3000 (hysteresis): the Paula model's steps and their BLEP ringing would chatter around zero.
double crossing_hz(const std::vector<std::int16_t>& v) {
  double first = -1;
  double last = -1;
  unsigned n = 0;
  bool armed = false;
  for (std::size_t i = 1; i < v.size(); ++i) {
    armed = armed || v[i] < -3000;
    if (armed && v[i - 1] < 0 && v[i] >= 0) {
      const double t = static_cast<double>(i - 1) + -static_cast<double>(v[i - 1]) / static_cast<double>(v[i] - v[i - 1]);
      first = first < 0 ? t : first;
      last = t;
      ++n;
      armed = false;
    }
  }
  return n < 3 ? 0.0 : (n - 1) * kEngineHz / (last - first);
}

// The converted sample played by the Amiga voice at note 60 (with `tune`), one shot, the engine's output at its rate.
std::vector<std::int16_t> play_amiga(const std::vector<std::int16_t>& pcm, int tune, unsigned blocks) {
  static o3os::test::StaticSlot<audio::AmigaVoices> av_slot;
  static o3os::test::StaticSlot<audio::VoicePool> pool_slot;
  audio::AmigaVoices& a = av_slot.make();
  audio::VoicePool& p = pool_slot.make();
  a.set_filter(audio::paula::Model::Off, false);
  audio::SampleDesc d;
  d.data = pcm.data();
  d.length = static_cast<std::uint32_t>(pcm.size());
  d.engine = audio::kEngineAmiga;
  d.tune = static_cast<std::int8_t>(tune);
  p.set_sample(0, d);
  CHECK(a.live_note_on(0, 60, 0, 127, p));
  std::vector<std::int16_t> out;
  for (unsigned b = 0; b < blocks; ++b) {
    a.begin_block();
    a.render(0, haudio::kBlock);
    a.end_block();
    const std::int16_t* f = a.feed(0);
    for (std::uint32_t i = 0; i < haudio::kBlock; ++i) {
      out.push_back(f != nullptr ? f[i] : std::int16_t{0});
    }
  }
  return out;
}

}  // namespace

TEST_CASE("amiga convert: the pitch at C-4 stays: a 220 Hz tone converted to 8,287 (tune 0) and to 16,574 (tune +12) plays at 220 Hz on the Amiga voice") {
  const double f0 = 220.0;
  const std::vector<std::int16_t> src = tone(44118, f0 / kEngineHz);  // the Modern voice reads it at the engine's rate at the unity note: 220 Hz
  for (const unsigned period : {kAmigaPeriod8, kAmigaPeriod16}) {
    const std::vector<std::int16_t> pcm = convert(src, period, false);
    const int tune = period == kAmigaPeriod16 ? kAmigaTune16 : 0;
    // 0.7 s of the output (the data lasts 1 s at its rate): the first 0.1 s are left out (the BLEP and the ramp-in of the voice).
    std::vector<std::int16_t> out = play_amiga(pcm, tune, 480);
    out.erase(out.begin(), out.begin() + 4000);
    const double hz = crossing_hz(out);
    INFO("period " << period << " tune " << tune << ": " << hz << " Hz");
    CHECK(hz == doctest::Approx(f0).epsilon(0.003));
  }
  // And without the +12 the 16,574 data would sound an octave low: the tune is what keeps it.
  std::vector<std::int16_t> low = play_amiga(convert(src, kAmigaPeriod16, false), 0, 480);
  low.erase(low.begin(), low.begin() + 4000);
  CHECK(crossing_hz(low) == doctest::Approx(f0 / 2).epsilon(0.004));
}
