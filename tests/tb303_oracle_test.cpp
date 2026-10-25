// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-012a, the dsp-port skill's gate 2: the TB-303 module's float port (modules/tb303/tb303_dsp.h, the very header the module is built from) against
// the ORIGINAL double-precision Open303 (third_party/open303, Robin Schmidt, MIT; support/open303_ref.*). Exact where the algorithm is the original's,
// with the tolerance of each stated in its test:
//   - the TB_303 ladder, the one-pole and biquad filters of the chain: the same input through both, SNR >= 80 dB (one of them has a documented reason
//     for less, see its test);
//   - the envelope section: the filter's cutoff and the amplitude, sample by sample, through triggers, accents, slides and releases;
//   - the oscillator: PolyBLEP against the wavetables, an intended deviation (no 197 KB of tables, no 4x oversampling), measured and bounded;
//   - the whole voice: spectra and envelopes within a stated distance, its pitch exact.
// Host only (the oracle is double precision).
#include <doctest/doctest.h>

#include "support/open303_ref.h"

#include "../modules/tb303/tb303_dsp.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include <o3os/audio/module_voices.h>

namespace {

namespace ref = open303_ref;
using tb303::Voice;

float exp2_libm(float x) { return std::exp2(x); }
float log2_libm(float x) { return std::log2(x); }
float sin_libm(float x) { return std::sin(x); }
float cos_libm(float x) { return std::cos(x); }
// What the device hands the module (o3m_host, audio/module_voices.cpp): the OS's libm-free exp2 / log2 (core/fast_math.h) and its small sin / cos.
const o3m_host& os_host() {
  static o3os::audio::ModuleVoices mv;
  return mv.host();
}
float exp2_os(float x) { return os_host().exp2_f(x); }
float log2_os(float x) { return os_host().log2_f(x); }
float sin_os(float x) { return os_host().sin_f(x); }
float cos_os(float x) { return os_host().cos_f(x); }

const tb303::Services kLibm = {exp2_libm, log2_libm, sin_libm, cos_libm};
const tb303::Services kOs = {exp2_os, log2_os, sin_os, cos_os};

struct Fixture {
  std::unique_ptr<Voice> v;
  Fixture(const tb303::Services& sv, float fs) : v(new Voice) {
    std::memset(v.get(), 0, sizeof(Voice));
    tb303::voice_init(*v, sv, fs);
  }
};

// A reproducible test signal: noise and a slow saw, amplitude about 0.5.
struct Noise {
  std::uint32_t s = 12345;
  double next() {
    s = s * 1664525u + 1013904223u;
    return (static_cast<double>(s >> 8) / 8388608.0 - 1.0);
  }
};

struct Err {
  double sig = 0, err = 0, max = 0;
  void add(double ref_v, double got) {
    const double e = got - ref_v;
    sig += ref_v * ref_v;
    err += e * e;
    max = std::fabs(e) > max ? std::fabs(e) : max;
  }
  double snr_db() const { return err == 0 ? 300.0 : 10.0 * std::log10(sig / err); }
};

}  // namespace

TEST_CASE("tb303 oracle: the TB_303 ladder against TeeBeeFilter") {
  for (float cutoff : {250.0f, 800.0f, 2400.0f, 6000.0f, 10000.0f}) {
    for (float res : {0.0f, 30.0f, 70.0f, 100.0f}) {
      ref::Ladder ladder(88200.0);
      ladder.set_resonance(res);
      ladder.set_cutoff(cutoff);
      Fixture f(kLibm, 44100.0f);
      tb303::voice_set_param(*f.v, tb303::kResonance, res);
      tb303::detail::filter_coefficients(*f.v, cutoff);
      Noise n;
      Err e;
      for (int i = 0; i < 6000; ++i) {
        const double x = 0.5 * n.next() + 0.4 * (std::fmod(i * 0.0137, 2.0) - 1.0);
        e.add(ladder.sample(x), tb303::ladder_step(f.v->lad, f.v->fbhp, static_cast<float>(x)));
      }
      std::printf("ladder cutoff %6.0f res %3.0f: SNR %6.1f dB, max err %.2e\n", static_cast<double>(cutoff), static_cast<double>(res), e.snr_db(), e.max);
      CHECK(e.snr_db() >= 80.0);
    }
  }
}

TEST_CASE("tb303 oracle: the one-pole and biquad filters of the chain against the original's") {
  struct One {
    const char* what;
    char mode;
    double hz, rate;
  };
  Noise n;
  const One ones[] = {{"pre high-pass 44.486 Hz @ 88.2k", 'h', 44.486, 88200.0}, {"feedback high-pass 150 Hz @ 88.2k", 'h', 150.0, 88200.0},
                      {"post high-pass 24.167 Hz @ 44.1k", 'h', 24.167, 44100.0}, {"all-pass 14.008 Hz @ 44.1k", 'a', 14.008, 44100.0}};
  for (const One& o : ones) {
    ref::OnePole r(o.mode, o.hz, o.rate);
    Fixture f(kLibm, static_cast<float>(o.rate));  // fs = rate here: the port's coefficient code at that rate
    tb303::OnePole p{};
    if (o.mode == 'h') {
      tb303::detail::one_pole_highpass(*f.v, p, static_cast<float>(o.hz), static_cast<float>(o.rate));
    } else {
      tb303::detail::one_pole_allpass(*f.v, p, static_cast<float>(o.hz), static_cast<float>(o.rate));
    }
    Err e;
    for (int i = 0; i < 20000; ++i) {
      const double x = 0.4 * n.next() + (i > 3000 ? 0.3 : 0.0);  // noise and a step
      e.add(r.sample(x), tb303::detail::run(p, static_cast<float>(x)));
    }
    std::printf("%-36s SNR %6.1f dB max err %.2e\n", o.what, e.snr_db(), e.max);
    CHECK(e.snr_db() >= 80.0);
  }
  {
    ref::Biquad r('n', 7.5164, 4.7, 0.0, 44100.0);
    Fixture f(kLibm, 44100.0f);
    tb303::Svf b{};
    tb303::detail::svf_bandreject(*f.v, b, 7.5164f, 4.7f, 44100.0f);
    Err e;
    for (int i = 0; i < 20000; ++i) {
      const double x = 0.4 * n.next() + (i > 3000 ? 0.3 : 0.0);
      e.add(r.sample(x), tb303::detail::run_svf<true>(b, static_cast<float>(x)));
    }
    std::printf("%-36s SNR %6.1f dB max err %.2e\n", "notch 7.5164 Hz, 4.7 oct @ 44.1k", e.snr_db(), e.max);
    CHECK(e.snr_db() >= 80.0);
  }
  {
    const double q_db = 20.0 * std::log10(std::sqrt(0.5));  // Open303: setGain(amp2dB(sqrt(0.5)))
    ref::Biquad r('l', 200.0, 0.0, q_db, 44100.0);
    Fixture f(kLibm, 44100.0f);
    tb303::Svf b{};
    tb303::detail::svf_lowpass12(*f.v, b, 200.0f, 0.70710678f, 44100.0f);
    Err e;
    for (int i = 0; i < 20000; ++i) {
      const double x = 0.4 * n.next() + (i > 3000 ? 0.3 : 0.0);
      e.add(r.sample(x), tb303::detail::run_svf<false>(b, static_cast<float>(x)));
    }
    std::printf("%-36s SNR %6.1f dB max err %.2e\n", "de-clicker 200 Hz low-pass @ 44.1k", e.snr_db(), e.max);
    CHECK(e.snr_db() >= 80.0);
  }
}

namespace {

// A note script shared by the envelope and whole-voice tests: {sample index, note, velocity}, velocity 0 = release. Sounding notes while another is held
// slide in both (the original's note list; the module's O3M_NOTE_LEGATO).
struct NoteEvent {
  int at, note, vel;
};

}  // namespace

TEST_CASE("tb303 oracle: the cutoff and the amplitude, sample by sample, through triggers, accents, slides and releases") {
  const NoteEvent script[] = {{0, 36, 64},     {5000, 40, 127}, {9000, 43, 64}, {13000, 0, 0},  {15000, 48, 127},
                              {20000, 0, 0}, {21000, 55, 64}, {25000, 57, 100}, {30000, 0, 0}};
  for (int variant = 0; variant < 2; ++variant) {
    const tb303::Services& sv = variant == 0 ? kLibm : kOs;
    const float cutoff = variant == 0 ? 1000.0f : 450.0f;
    ref::Voice r(44100.0);
    r.set_cutoff(cutoff);
    r.set_env_mod(variant == 0 ? 25.0 : 80.0);
    r.set_accent(variant == 0 ? 50.0 : 100.0);
    r.set_decay(variant == 0 ? 1000.0 : 300.0);
    r.set_volume(-12.0);
    Fixture f(sv, 44100.0f);
    tb303::voice_set_param(*f.v, tb303::kCutoff, cutoff);
    tb303::voice_set_param(*f.v, tb303::kEnvMod, variant == 0 ? 25.0f : 80.0f);
    tb303::voice_set_param(*f.v, tb303::kAccent, variant == 0 ? 50.0f : 100.0f);
    tb303::voice_set_param(*f.v, tb303::kDecay, variant == 0 ? 1000.0f : 300.0f);
    unsigned next = 0;
    int held = 0, last_note = 0;
    double worst_cutoff = 0, worst_amp = 0, amp_peak = 0;
    for (int i = 0; i < 34000; ++i) {
      while (next < sizeof script / sizeof script[0] && script[next].at == i) {
        const NoteEvent& e = script[next++];
        if (e.vel == 0) {
          r.note_on(last_note, 0);  // a release names the note it releases
          tb303::voice_note_off(*f.v);
          held = 0;
        } else {
          r.note_on(e.note, e.vel);
          if (held != 0) {
            r.note_on(last_note, 0);  // the note this one replaces is no longer held (the host's mono voice has one note); the new one still is: no release
          }
          tb303::voice_note_on(*f.v, static_cast<uint32_t>(e.note), static_cast<uint32_t>(e.vel), held != 0);
          held = 1;
          last_note = e.note;
        }
      }
      double rc, ra;
      r.control(rc, ra);
      float expo, amp;
      tb303::env_step(*f.v, f.v->env, f.v->gate != 0u, expo, amp);
      const double pc = static_cast<double>(f.v->p_cutoff) * std::exp2(static_cast<double>(expo));
      const double pa = static_cast<double>(tb303::detail::run_svf<false>(f.v->declick, amp));
      worst_cutoff = std::fmax(worst_cutoff, std::fabs(pc - rc) / rc);
      worst_amp = std::fmax(worst_amp, std::fabs(pa - ra));
      amp_peak = std::fmax(amp_peak, std::fabs(ra));
    }
    std::printf("envelopes (%s): worst relative cutoff error %.2e, worst amplitude error %.2e (peak %.2f)\n", variant == 0 ? "libm" : "OS exp2", worst_cutoff,
                worst_amp, amp_peak);
    CHECK(worst_cutoff < 1e-5);        // measured 2e-6: the decays run as y -= y d (see decay_rate), the OS exp2 adds under 1e-6
    CHECK(worst_amp < 2e-5 * amp_peak);  // measured 4e-6 of the peak
  }
}

namespace {

// A radix-2 FFT of a real signal (n a power of two): the magnitudes squared of bins 0 .. n/2.
std::vector<double> power_spectrum(const std::vector<double>& x) {
  const std::size_t n = x.size();
  std::vector<std::complex<double>> a(x.begin(), x.end());
  for (std::size_t i = 1, j = 0; i < n; ++i) {
    std::size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) {
      j ^= bit;
    }
    j ^= bit;
    if (i < j) {
      std::swap(a[i], a[j]);
    }
  }
  for (std::size_t len = 2; len <= n; len <<= 1) {
    const double ang = -2.0 * 3.14159265358979323846 / static_cast<double>(len);
    const std::complex<double> wl(std::cos(ang), std::sin(ang));
    for (std::size_t i = 0; i < n; i += len) {
      std::complex<double> w(1.0, 0.0);
      for (std::size_t k = 0; k < len / 2; ++k) {
        const std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
        w *= wl;
      }
    }
  }
  std::vector<double> p(n / 2 + 1);
  for (std::size_t k = 0; k <= n / 2; ++k) {
    p[k] = std::norm(a[k]);
  }
  return p;
}

}  // namespace

TEST_CASE("tb303 oracle: the PolyBLEP oscillator against the SAW303 / SQUARE303 wavetables, in the band the filter passes") {
  // The wavetables are brick-wall limited (their Gibbs ringing sits at the top of the band, 20-28 kHz); the PolyBLEP is not: the two are compared where
  // the 303's own filter and the decimator leave signal, below 8 kHz, in the frequency domain (periods of a power of two samples: no leakage).
  const int kN = 8192;
  const double rate2 = 88200.0;
  for (double blend : {0.0, 0.5, 1.0}) {
    for (int period : {2048, 1024, 256, 64}) {
      const double f = rate2 / period;
      ref::Oscillator o(2.0 * rate2, blend);
      o.set_frequency(f);
      float phase = 0.0f;
      const float dt = static_cast<float>(f / rate2);
      std::vector<double> a(kN), b(kN);
      for (int m = 0; m < kN; ++m) {
        a[static_cast<std::size_t>(m)] = o.sample();
        (void)o.sample();  // the port runs at 2x, the original at 4x: every other sample
        b[static_cast<std::size_t>(m)] = static_cast<double>(tb303::detail::oscillator(phase, dt, static_cast<float>(blend)));
        phase += dt;
        phase -= phase >= 1.0f ? 1.0f : 0.0f;
      }
      const std::vector<double> pa = power_spectrum(a);
      std::vector<double> diff(kN);
      for (int m = 0; m < kN; ++m) {
        diff[static_cast<std::size_t>(m)] = b[static_cast<std::size_t>(m)] - a[static_cast<std::size_t>(m)];
      }
      const std::vector<double> pd = power_spectrum(diff);
      const std::size_t top = static_cast<std::size_t>(8000.0 / rate2 * kN);
      double sig = 0, err = 0;
      for (std::size_t k = 1; k <= top; ++k) {
        sig += pa[k];
        err += pd[k];
      }
      const double snr = 10.0 * std::log10(sig / err);
      std::printf("oscillator blend %.1f %7.1f Hz: in-band (<8 kHz) SNR %5.1f dB\n", blend, f, snr);
      CHECK(snr >= 30.0);  // measured 35.8 .. 49.3 dB: the PolyBLEP's own band-limiting against the tables' (an intended deviation)
    }
  }
}

namespace {

struct VoiceRun {
  std::vector<double> ref;
  std::vector<float> mine;
};

struct VoiceSetup {
  double wave = 0.0, cutoff = 1000.0, res = 50.0, env_mod = 25.0, decay = 1000.0, accent = 50.0, slide = 60.0;
};

// The same note script through the original (one sample at a time) and the port (blocks of `block`, cut at every event).
VoiceRun run_voice(const VoiceSetup& su, const NoteEvent* script, std::size_t count, int total, int block, const tb303::Services& sv) {
  VoiceRun out;
  out.ref.resize(static_cast<std::size_t>(total));
  out.mine.resize(static_cast<std::size_t>(total));
  ref::Voice r(44100.0);
  r.set_waveform(su.wave);
  r.set_cutoff(su.cutoff);
  r.set_resonance(su.res);
  r.set_env_mod(su.env_mod);
  r.set_decay(su.decay);
  r.set_accent(su.accent);
  r.set_slide_time(su.slide);
  r.set_volume(-12.0);
  Fixture f(sv, 44100.0f);
  tb303::voice_set_param(*f.v, tb303::kWaveform, static_cast<float>(su.wave));
  tb303::voice_set_param(*f.v, tb303::kCutoff, static_cast<float>(su.cutoff));
  tb303::voice_set_param(*f.v, tb303::kResonance, static_cast<float>(su.res));
  tb303::voice_set_param(*f.v, tb303::kEnvMod, static_cast<float>(su.env_mod));
  tb303::voice_set_param(*f.v, tb303::kDecay, static_cast<float>(su.decay));
  tb303::voice_set_param(*f.v, tb303::kAccent, static_cast<float>(su.accent));
  tb303::voice_set_param(*f.v, tb303::kSlide, static_cast<float>(su.slide));
  tb303::voice_set_param(*f.v, tb303::kVolume, -12.0f);
  std::size_t next = 0;
  int held = 0, last_note = 0;
  int pos = 0;
  while (pos < total) {
    while (next < count && script[next].at <= pos) {
      const NoteEvent& e = script[next++];
      if (e.vel == 0) {
        r.note_on(last_note, 0);
        tb303::voice_note_off(*f.v);
        held = 0;
      } else {
        r.note_on(e.note, e.vel);
        if (held != 0) {
          r.note_on(last_note, 0);
        }
        tb303::voice_note_on(*f.v, static_cast<uint32_t>(e.note), static_cast<uint32_t>(e.vel), held != 0);
        held = 1;
        last_note = e.note;
      }
    }
    int end = pos + block;
    if (next < count && script[next].at < end) {
      end = script[next].at;
    }
    end = end > total ? total : end;
    for (int i = pos; i < end; ++i) {
      out.ref[static_cast<std::size_t>(i)] = r.sample();
    }
    tb303::voice_render(*f.v, out.mine.data() + pos, static_cast<uint32_t>(end - pos));
    pos = end;
  }
  return out;
}

double rms(const double* x, int n) {
  double s = 0;
  for (int i = 0; i < n; ++i) {
    s += x[i] * x[i];
  }
  return std::sqrt(s / n);
}

struct Distance {
  double env_mean = 0, env_max = 0;    // dB, level in 10 ms windows
  double spec_mean = 0, spec_p95 = 0;  // dB, third-octave bands in 2048-sample windows
  int env_n = 0, spec_n = 0;
};

Distance distance(const VoiceRun& run) {
  Distance d;
  const int total = static_cast<int>(run.ref.size());
  std::vector<double> mine(run.mine.begin(), run.mine.end());
  double peak = 0;
  for (int i = 0; i + 441 <= total; i += 441) {
    peak = std::fmax(peak, rms(run.ref.data() + i, 441));
  }
  for (int i = 0; i + 441 <= total; i += 441) {
    const double a = rms(run.ref.data() + i, 441), b = rms(mine.data() + i, 441);
    if (a > 1e-3 * peak) {
      const double db = std::fabs(20.0 * std::log10((b + 1e-12) / a));
      d.env_mean += db;
      d.env_max = std::fmax(d.env_max, db);
      ++d.env_n;
    }
  }
  d.env_mean /= std::fmax(1, d.env_n);
  std::vector<double> diffs;
  const int kW = 2048;
  std::vector<double> hann(kW);
  for (int i = 0; i < kW; ++i) {
    hann[static_cast<std::size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * i / kW);
  }
  double peak_power = 0;
  std::vector<std::vector<double>> pr, pm;
  for (int i = 0; i + kW <= total; i += kW) {
    std::vector<double> a(kW), b(kW);
    for (int k = 0; k < kW; ++k) {
      a[static_cast<std::size_t>(k)] = run.ref[static_cast<std::size_t>(i + k)] * hann[static_cast<std::size_t>(k)];
      b[static_cast<std::size_t>(k)] = mine[static_cast<std::size_t>(i + k)] * hann[static_cast<std::size_t>(k)];
    }
    pr.push_back(power_spectrum(a));
    pm.push_back(power_spectrum(b));
    double tot = 0;
    for (double v : pr.back()) {
      tot += v;
    }
    peak_power = std::fmax(peak_power, tot);
  }
  for (std::size_t w = 0; w < pr.size(); ++w) {
    double tot = 0;
    for (double v : pr[w]) {
      tot += v;
    }
    if (tot < 1e-4 * peak_power) {
      continue;
    }
    for (double fc = 100.0; fc < 9000.0; fc *= 1.2599210498948732) {
      const std::size_t lo = static_cast<std::size_t>(fc / 1.1224620483093730 / 44100.0 * kW);
      const std::size_t hi = static_cast<std::size_t>(fc * 1.1224620483093730 / 44100.0 * kW);
      double a = 0, b = 0;
      for (std::size_t k = lo; k <= hi && k < pr[w].size(); ++k) {
        a += pr[w][k];
        b += pm[w][k];
      }
      if (a < 1e-3 * tot) {
        continue;  // a band that holds under -30 dB of the window: not where the sound is
      }
      diffs.push_back(std::fabs(10.0 * std::log10((b + 1e-30) / a)));
    }
  }
  std::sort(diffs.begin(), diffs.end());
  d.spec_n = static_cast<int>(diffs.size());
  for (double v : diffs) {
    d.spec_mean += v;
  }
  d.spec_mean /= std::fmax(1, d.spec_n);
  d.spec_p95 = diffs.empty() ? 0 : diffs[static_cast<std::size_t>(diffs.size() * 95 / 100)];
  return d;
}

// The fundamental period, in samples, by autocorrelation over [from, to) (lags lo..hi).
double period_of(const double* x, int from, int to, int lo, int hi) {
  double best = -1e300;
  int best_lag = lo;
  for (int lag = lo; lag <= hi; ++lag) {
    double s = 0;
    for (int i = from; i + lag < to; ++i) {
      s += x[i] * x[i + lag];
    }
    if (s > best) {
      best = s;
      best_lag = lag;
    }
  }
  return best_lag;
}

}  // namespace

TEST_CASE("tb303 oracle: the whole voice against Open303, in level, spectrum and pitch") {
  const NoteEvent script[] = {{0, 36, 64},      {8000, 0, 0},    {12000, 48, 127}, {20000, 55, 64},
                              {26000, 0, 0},    {30000, 43, 100}, {36000, 47, 64},  {42000, 0, 0}};
  struct Case {
    const char* what;
    VoiceSetup su;
  };
  VoiceSetup a, b, c, d;
  b.wave = 1.0;
  b.cutoff = 600.0;
  b.res = 80.0;
  b.env_mod = 70.0;
  b.decay = 400.0;
  c.wave = 0.5;
  c.cutoff = 2000.0;
  c.res = 20.0;
  c.env_mod = 100.0;
  c.decay = 200.0;
  c.accent = 100.0;
  c.slide = 120.0;
  d.cutoff = 314.0;
  d.res = 100.0;
  d.env_mod = 0.0;
  d.accent = 0.0;
  d.slide = 0.0;
  const Case cases[] = {{"saw, default", a}, {"square, resonant", b}, {"blend, bright, accents", c}, {"saw, full resonance, no envelope", d}};
  for (const Case& cs : cases) {
    const VoiceRun run = run_voice(cs.su, script, sizeof script / sizeof script[0], 46000, 64, kOs);
    const Distance dist = distance(run);
    const double p_ref = period_of(run.ref.data(), 2000, 7000, 500, 900);
    std::vector<double> mine(run.mine.begin(), run.mine.end());
    const double p_mine = period_of(mine.data(), 2000, 7000, 500, 900);
    std::printf("whole voice %-34s level: mean %.2f max %.2f dB (%d windows); spectrum: mean %.2f p95 %.2f dB (%d bands); period %.0f / %.0f\n", cs.what,
                dist.env_mean, dist.env_max, dist.env_n, dist.spec_mean, dist.spec_p95, dist.spec_n, p_ref, p_mine);
    // Measured: level mean 0.30-0.62 dB, max 2.1-6.7 dB (a 10 ms window on a fast transient); spectrum mean 0.45-0.83 dB, p95 1.9-6.1 dB. The bounds keep
    // margin over the measurement, not over what a listener would call the same sound: the oscillator and the oversampling are different by design.
    CHECK(dist.env_n > 40);
    CHECK(dist.spec_n > 100);
    CHECK(dist.env_mean < 1.0);
    CHECK(dist.env_max < 8.0);
    CHECK(dist.spec_mean < 1.5);
    CHECK(dist.spec_p95 < 8.0);
    CHECK(p_ref == p_mine);  // the pitch is exact (the same note, the same tuning)
  }
}

TEST_CASE("tb303: no state depends on how a block is cut, bit for bit") {
  const NoteEvent script[] = {{0, 36, 64}, {5000, 48, 127}, {9000, 0, 0}, {11000, 60, 100}, {15000, 62, 64}, {19000, 0, 0}};
  VoiceSetup su;
  su.wave = 0.3;
  su.res = 85.0;
  su.env_mod = 60.0;
  const VoiceRun base = run_voice(su, script, sizeof script / sizeof script[0], 24000, 64, kOs);
  for (int block : {1, 7, 33, 128, 4096}) {
    const VoiceRun other = run_voice(su, script, sizeof script / sizeof script[0], 24000, block, kOs);
    CHECK_MESSAGE(base.mine == other.mine, "block " << block);
  }
}

TEST_CASE("tb303: parameter extremes stay finite and bounded, out-of-range values are held, NaN is ignored") {
  struct Range {
    uint32_t id;
    float lo, hi;
  };
  const Range ranges[] = {{tb303::kWaveform, 0, 1}, {tb303::kCutoff, 314, 2394}, {tb303::kResonance, 0, 100}, {tb303::kEnvMod, 0, 100}, {tb303::kDecay, 200, 2000},
                          {tb303::kAccent, 0, 100}, {tb303::kSlide, 0, 300}, {tb303::kVolume, -48, 6}, {tb303::kLegato, 0, 1},
                          {tb303::kTune, -100, 100}, {tb303::kTranspose, -24, 24}, {tb303::kAccThresh, 1, 127}, {tb303::kFilterTrack, 0, 100}, {tb303::kAccDecay, 30, 2000}};
  const float nan = std::nanf("");
  for (int corner = 0; corner < 64; ++corner) {
    Fixture f(kOs, 44117.647f);
    for (unsigned i = 0; i < sizeof ranges / sizeof ranges[0]; ++i) {
      const float v = ((corner >> (i % 6)) & 1) ? ranges[i].hi : ranges[i].lo;
      tb303::voice_set_param(*f.v, ranges[i].id, v * (i % 2 ? 1.0f : 1.0f));
    }
    tb303::voice_set_param(*f.v, tb303::kCutoff, 1.0e9f);  // held to 2394
    tb303::voice_set_param(*f.v, tb303::kResonance, nan);  // ignored
    tb303::voice_set_param(*f.v, 99, 1.0f);                 // no such parameter
    CHECK(f.v->p_cutoff == 2394.0f);
    CHECK(f.v->p_res == f.v->p_res);
    float out[64];
    double peak = 0;
    bool finite = true;
    uint32_t held = 0;
    for (int blk = 0; blk < 400; ++blk) {
      if (blk % 100 == 0) {
        tb303::voice_note_on(*f.v, static_cast<uint32_t>(24 + 12 * (blk / 100)), 127, held != 0);  // accents from the lowest octaves to the highest, legato after the first
        held = 1;
      }
      tb303::voice_render(*f.v, out, 64);
      for (float x : out) {
        finite = finite && std::isfinite(x);
        peak = std::fmax(peak, std::fabs(static_cast<double>(x)));
      }
    }
    CHECK(finite);
    CHECK(peak < 40.0);  // 6 dB of volume on an accent at full resonance: a few units, never a runaway
  }
}

TEST_CASE("tb303: a released note ends in exact silence, and the next note starts it again") {
  Fixture f(kOs, 44100.0f);
  float out[64];
  tb303::voice_note_on(*f.v, 40, 64, false);
  uint32_t active = 1;
  for (int i = 0; i < 100; ++i) {
    active = tb303::voice_render(*f.v, out, 64);
  }
  CHECK(active == 1);  // a held note decaying over 1.2 s is still sounding
  tb303::voice_note_off(*f.v);
  int blocks = 0;
  while (active != 0 && blocks < 2000) {
    active = tb303::voice_render(*f.v, out, 64);
    ++blocks;
  }
  CHECK(active == 0);
  CHECK(blocks < 400);  // the 1 ms release and the ring of the de-clicker: milliseconds, not seconds
  tb303::voice_render(*f.v, out, 64);
  bool zeros = true;
  for (float x : out) {
    zeros = zeros && x == 0.0f;
  }
  CHECK(zeros);
  tb303::voice_note_on(*f.v, 45, 64, false);
  double peak = 0;
  for (int i = 0; i < 10; ++i) {
    tb303::voice_render(*f.v, out, 64);
    for (float x : out) {
      peak = std::fmax(peak, std::fabs(static_cast<double>(x)));
    }
  }
  CHECK(peak > 0.01);
  // a legato note on an idle voice is a trigger (there is nothing to slide from)
  Fixture g(kOs, 44100.0f);
  tb303::voice_note_on(*g.v, 40, 64, true);
  CHECK(g.v->env.trig == 1);
}

TEST_CASE("tb303 oracle: the host's sin and cos (what the module's coefficients use) against libm") {
  double worst_abs = 0, worst_rel_small = 0;
  for (int i = -20000; i <= 20000; ++i) {
    const float x = static_cast<float>(i) * 0.05f;  // -1000 .. 1000 rad
    worst_abs = std::fmax(worst_abs, std::fabs(static_cast<double>(sin_os(x)) - std::sin(static_cast<double>(x))));
    worst_abs = std::fmax(worst_abs, std::fabs(static_cast<double>(cos_os(x)) - std::cos(static_cast<double>(x))));
  }
  for (double x = 1e-5; x < 0.5; x *= 1.01) {  // the corner frequencies of the chain: 7 Hz at 44 kHz is 1e-3 rad
    const float xf = static_cast<float>(x);
    worst_rel_small = std::fmax(worst_rel_small, std::fabs((static_cast<double>(sin_os(xf)) - std::sin(static_cast<double>(xf))) / std::sin(static_cast<double>(xf))));
  }
  std::printf("host sin/cos: worst absolute error %.2e (|x| <= 1000), worst relative error of sin below 0.5 rad %.2e\n", worst_abs, worst_rel_small);
  CHECK(worst_abs < 1e-6);
  CHECK(worst_rel_small < 2e-7);
  CHECK(sin_os(std::nanf("")) == 0.0f);
  CHECK(cos_os(1.0e30f) == 0.0f);
}

// ---- OS-MODPITCH (ABI 1.8): the host's Slide / Glide offset ---------------------------------------------------------------------------------------------------------------

namespace {

void set_params(Voice& v, float slide_ms, bool legato_slide) {
  tb303::voice_set_param(v, tb303::kSlide, slide_ms);
  tb303::voice_set_param(v, tb303::kLegato, legato_slide ? 1.0f : 0.0f);
}

}  // namespace

TEST_CASE("tb303 pitch: pitch(0, 0) is bit for bit not calling it, pitch(12, 0) is an octave up, and the frequency follows the host's offset block by block with no slew lag (ABI 1.8)") {
  // 0 and 0: the same samples
  Fixture a(kOs, 44100.0f), b(kOs, 44100.0f);
  for (Fixture* f : {&a, &b}) {
    set_params(*f->v, 100.0f, true);
    tb303::voice_note_on(*f->v, 45, 127, false);
  }
  tb303::voice_pitch(*b.v, 0.0f, 0.0f);
  std::vector<float> oa(64 * 40), ob(64 * 40);
  for (int blk = 0; blk < 40; ++blk) {
    tb303::voice_render(*a.v, oa.data() + 64 * blk, 64);
    tb303::voice_render(*b.v, ob.data() + 64 * blk, 64);
  }
  CHECK(oa == ob);
  // 12: the oscillator's frequency is twice the note's
  Fixture c(kOs, 44100.0f);
  tb303::voice_note_on(*c.v, 45, 127, false);
  const float hz = c.v->note_hz;
  CHECK(hz == doctest::Approx(110.0f).epsilon(1e-5));
  tb303::voice_pitch(*c.v, 12.0f, 0.0f);
  CHECK(c.v->freq_target == doctest::Approx(2.0f * hz).epsilon(1e-5));
  CHECK(c.v->freq == c.v->freq_target);  // straight there, past the slew
  // a ramp, one pitch() per block: the frequency IS the host's number at the end of every block (the module's own slew, 100 ms here, would lag)
  Fixture d(kOs, 44100.0f);
  set_params(*d.v, 100.0f, true);
  tb303::voice_note_on(*d.v, 45, 127, false);
  std::vector<float> out(64);
  for (int blk = 0; blk < 100; ++blk) {
    const float semis = -7.0f + 7.0f * static_cast<float>(blk) / 99.0f;
    tb303::voice_pitch(*d.v, semis, 0.0f);
    tb303::voice_render(*d.v, out.data(), 64);
    CHECK(d.v->freq == doctest::Approx(hz * std::exp2(semis / 12.0f)).epsilon(2e-5));
  }
}

TEST_CASE("tb303 pitch: a note with LEGATO | RAMP over a sounding one keeps the envelope and has no slew of its own, whatever the Slide and Legato Slide parameters; a live legato key still slides (ABI 1.8)") {
  const auto sounding = [](Fixture& f) {
    set_params(*f.v, 200.0f, true);
    tb303::voice_note_on(*f.v, 40, 127, false);
    std::vector<float> out(64);
    for (int blk = 0; blk < 60; ++blk) {
      tb303::voice_render(*f.v, out.data(), 64);
    }
  };
  // the host-ramped legato note: the envelope's value is untouched by the note, its trigger flag stays clear, the base frequency jumps to the note (pitch() then moves it)
  Fixture a(kOs, 44100.0f);
  sounding(a);
  const float main_before = a.v->env.main_y;
  const float amp_before = a.v->env.amp_y;
  tb303::voice_note_on(*a.v, 52, 127, true, true);
  CHECK(a.v->env.main_y == main_before);
  CHECK(a.v->env.amp_y == amp_before);
  CHECK(a.v->env.trig == 0u);
  CHECK(a.v->freq == a.v->freq_target);  // no portamento of the module's own
  CHECK(a.v->freq_target == doctest::Approx(440.0f * std::exp2((52.0f - 69.0f) / 12.0f)).epsilon(1e-5));
  // ... also with the Legato Slide parameter off: the pattern chose the slide
  Fixture b(kOs, 44100.0f);
  sounding(b);
  tb303::voice_set_param(*b.v, tb303::kLegato, 0.0f);
  const float main_b = b.v->env.main_y;
  tb303::voice_note_on(*b.v, 52, 127, true, true);
  CHECK(b.v->env.main_y == main_b);
  CHECK(b.v->env.trig == 0u);
  // a live legato key (no RAMP): the module's own slew, still lagging behind the target after a block
  Fixture c(kOs, 44100.0f);
  sounding(c);
  tb303::voice_note_on(*c.v, 52, 127, true);
  CHECK(c.v->freq != c.v->freq_target);
  std::vector<float> out(64);
  tb303::voice_render(*c.v, out.data(), 64);
  CHECK(c.v->freq < c.v->freq_target);  // still on its way up (200 ms)
  // a live legato key with the Legato Slide parameter off retriggers (the envelope starts again)
  Fixture d(kOs, 44100.0f);
  sounding(d);
  tb303::voice_set_param(*d.v, tb303::kLegato, 0.0f);
  tb303::voice_note_on(*d.v, 52, 127, true);
  CHECK(d.v->env.trig == 1u);
  // a plain note (no flags) after a ramped one: the offset is the note's own again
  Fixture e(kOs, 44100.0f);
  sounding(e);
  tb303::voice_pitch(*e.v, 5.0f, 0.0f);
  tb303::voice_note_on(*e.v, 40, 127, false);
  CHECK(e.v->freq == doctest::Approx(440.0f * std::exp2((40.0f - 69.0f) / 12.0f)).epsilon(1e-5));
  // the idle voice: LEGATO | RAMP with nothing sounding is an ordinary trigger
  Fixture f(kOs, 44100.0f);
  tb303::voice_note_on(*f.v, 52, 127, true, true);
  CHECK(f.v->env.trig == 1u);
}

// ---- OS-SNDMOD S3: Tune, Transpose, Acc Thresh, Filter Track, Acc Decay (ids 9..13) ------------------------------------------------------------------------------------------------
// None of them adds a sample of work: the oracle tests above run on the defaults, which these tests pin as "the parameters were never there".

namespace {

// A short phrase: plain and accented notes, a legato one, a release, a tail; the output of every block into `all`.
std::vector<float> sndmod_phrase(Voice& v) {
  std::vector<float> all;
  float out[64];
  const struct {
    std::uint32_t note, vel;
    bool legato;
  } notes[] = {{36, 96, false}, {43, 100, false}, {48, 127, true}, {55, 99, false}, {40, 110, false}};
  for (const auto& n : notes) {
    tb303::voice_note_on(v, n.note, n.vel, n.legato);
    for (int blk = 0; blk < 120; ++blk) {
      tb303::voice_render(v, out, 64);
      all.insert(all.end(), out, out + 64);
    }
    tb303::voice_note_off(v);
    for (int blk = 0; blk < 30; ++blk) {
      tb303::voice_render(v, out, 64);
      all.insert(all.end(), out, out + 64);
    }
  }
  return all;
}

}  // namespace

TEST_CASE("tb303 sndmod: the five appended parameters at their neutral values are not there (the output of a phrase is bit for bit the same), and they start at those values") {
  Fixture a(kOs, 44100.0f), b(kOs, 44100.0f);
  CHECK(a.v->p_tune == 0.0f);
  CHECK(a.v->p_transpose == 0.0f);
  CHECK(a.v->p_acc_thr == 100u);
  CHECK(a.v->p_track == 0.0f);
  CHECK(a.v->p_acc_decay == 200.0f);
  CHECK(a.v->track_mul == 1.0f);
  for (Fixture* f : {&a, &b}) {
    tb303::voice_set_param(*f->v, tb303::kCutoff, 700.0f);
    tb303::voice_set_param(*f->v, tb303::kResonance, 80.0f);
    tb303::voice_set_param(*f->v, tb303::kEnvMod, 70.0f);
  }
  // b is told the neutral values explicitly (as a project that stores them does), a never is
  tb303::voice_set_param(*b.v, tb303::kTune, 0.0f);
  tb303::voice_set_param(*b.v, tb303::kTranspose, 0.0f);
  tb303::voice_set_param(*b.v, tb303::kAccThresh, 100.0f);
  tb303::voice_set_param(*b.v, tb303::kFilterTrack, 0.0f);
  tb303::voice_set_param(*b.v, tb303::kAccDecay, 200.0f);
  const std::vector<float> oa = sndmod_phrase(*a.v);
  const std::vector<float> ob = sndmod_phrase(*b.v);
  CHECK(oa == ob);
  CHECK(std::memcmp(a.v.get(), b.v.get(), sizeof(Voice)) == 0);  // the whole voice, every byte
  // and the accent split is where it was: 96 and 99 are plain, 100 and above accent
  Fixture c(kOs, 44100.0f);
  tb303::voice_note_on(*c.v, 40, 99, false);
  CHECK(c.v->env.accent_gain == 0.0f);
  tb303::voice_note_on(*c.v, 40, 100, false);
  CHECK(c.v->env.accent_gain == doctest::Approx(0.5f));
}

TEST_CASE("tb303 sndmod: Tune and Transpose move the pitch of the next note by cents and semitones, held to the MIDI range; whole numbers, held to their ranges") {
  const auto hz_of = [](float tune, float transpose, std::uint32_t note) {
    Fixture f(kOs, 44100.0f);
    tb303::voice_set_param(*f.v, tb303::kTune, tune);
    tb303::voice_set_param(*f.v, tb303::kTranspose, transpose);
    tb303::voice_note_on(*f.v, note, 96, false);
    CHECK(f.v->freq == f.v->freq_target);
    return f.v->note_hz;
  };
  const float base = hz_of(0.0f, 0.0f, 45);
  CHECK(base == doctest::Approx(110.0f).epsilon(1e-5));
  CHECK(hz_of(0.0f, 12.0f, 45) == doctest::Approx(2.0f * base).epsilon(1e-5));
  CHECK(hz_of(0.0f, -24.0f, 45) == doctest::Approx(0.25f * base).epsilon(1e-5));
  CHECK(hz_of(100.0f, 0.0f, 45) == doctest::Approx(base * std::exp2(1.0f / 12.0f)).epsilon(1e-5));
  CHECK(hz_of(-50.0f, 0.0f, 45) == doctest::Approx(base * std::exp2(-0.5f / 12.0f)).epsilon(1e-5));
  CHECK(hz_of(100.0f, 0.0f, 45) == doctest::Approx(hz_of(0.0f, 1.0f, 45)).epsilon(1e-5));
  CHECK(hz_of(25.0f, 7.0f, 45) == doctest::Approx(base * std::exp2(7.25f / 12.0f)).epsilon(1e-5));
  // held to the MIDI range: note 120 + 24 sounds as note 127; note 3 - 24 as note 0
  CHECK(hz_of(0.0f, 24.0f, 120) == doctest::Approx(440.0f * std::exp2((127.0f - 69.0f) / 12.0f)).epsilon(1e-5));
  CHECK(hz_of(0.0f, -24.0f, 3) == doctest::Approx(440.0f * std::exp2(-69.0f / 12.0f)).epsilon(1e-5));
  // out-of-range and fractional values: held and rounded; NaN ignored; the running note does not move until the next one
  Fixture f(kOs, 44100.0f);
  tb303::voice_set_param(*f.v, tb303::kTune, 1.0e9f);
  tb303::voice_set_param(*f.v, tb303::kTranspose, -1.0e9f);
  CHECK(f.v->p_tune == 100.0f);
  CHECK(f.v->p_transpose == -24.0f);
  tb303::voice_set_param(*f.v, tb303::kTune, 12.6f);
  tb303::voice_set_param(*f.v, tb303::kTranspose, -2.4f);
  CHECK(f.v->p_tune == 13.0f);
  CHECK(f.v->p_transpose == -2.0f);
  tb303::voice_set_param(*f.v, tb303::kTune, std::nanf(""));
  CHECK(f.v->p_tune == 13.0f);
  tb303::voice_note_on(*f.v, 45, 96, false);
  const float held_hz = f.v->note_hz;
  tb303::voice_set_param(*f.v, tb303::kTranspose, 12.0f);
  CHECK(f.v->note_hz == held_hz);
  CHECK(f.v->freq_target == held_hz);
}

TEST_CASE("tb303 sndmod: Acc Thresh is the velocity from which a note is an accent, 1..127, and the host's default velocity 96 stays plain at the default") {
  Fixture f(kOs, 44100.0f);
  CHECK(f.v->p_acc_thr == 100u);
  tb303::voice_note_on(*f.v, 40, 96, false);
  CHECK(f.v->env.accent_gain == 0.0f);
  tb303::voice_set_param(*f.v, tb303::kAccThresh, 90.0f);
  tb303::voice_note_on(*f.v, 40, 89, false);
  CHECK(f.v->env.accent_gain == 0.0f);
  CHECK(f.v->env.main_d == f.v->main_d_norm);
  tb303::voice_note_on(*f.v, 40, 90, false);
  CHECK(f.v->env.accent_gain == doctest::Approx(0.5f));
  CHECK(f.v->env.main_d == f.v->main_d_acc);
  CHECK(f.v->env.amp_rel_d == f.v->amp_rel_acc_d);
  tb303::voice_set_param(*f.v, tb303::kAccThresh, 1.0f);  // every note
  tb303::voice_note_on(*f.v, 40, 1, false);
  CHECK(f.v->env.accent_gain == doctest::Approx(0.5f));
  tb303::voice_set_param(*f.v, tb303::kAccThresh, 127.0f);  // only a full-velocity note
  tb303::voice_note_on(*f.v, 40, 126, false);
  CHECK(f.v->env.accent_gain == 0.0f);
  tb303::voice_note_on(*f.v, 40, 127, false);
  CHECK(f.v->env.accent_gain == doctest::Approx(0.5f));
  // held to 1..127, whole numbers, NaN ignored
  tb303::voice_set_param(*f.v, tb303::kAccThresh, 0.0f);
  CHECK(f.v->p_acc_thr == 1u);
  tb303::voice_set_param(*f.v, tb303::kAccThresh, 1.0e9f);
  CHECK(f.v->p_acc_thr == 127u);
  tb303::voice_set_param(*f.v, tb303::kAccThresh, 64.4f);
  CHECK(f.v->p_acc_thr == 64u);
  tb303::voice_set_param(*f.v, tb303::kAccThresh, std::nanf(""));
  CHECK(f.v->p_acc_thr == 64u);
}

TEST_CASE("tb303 sndmod: Acc Decay is the time constant of an accented note's filter decay (a normal note keeps Decay), held to 30..2000 ms") {
  const auto main_after = [](float acc_ms, float decay_ms, std::uint32_t vel, int samples) {
    Fixture f(kOs, 44100.0f);
    tb303::voice_set_param(*f.v, tb303::kDecay, decay_ms);
    tb303::voice_set_param(*f.v, tb303::kAccDecay, acc_ms);
    tb303::voice_note_on(*f.v, 40, vel, false);
    float out[64];
    for (int done = 0; done < samples; done += 64) {
      tb303::voice_render(*f.v, out, 64);
    }
    return f.v->env.main_y;
  };
  // the main envelope is e^(-t / tau) (y starts at 1 on the first sample): 4416 samples = 100.1 ms at 44.1 kHz
  const float t = 4416.0f / 44100.0f;
  for (float ms : {30.0f, 100.0f, 200.0f, 800.0f}) {
    CAPTURE(ms);
    CHECK(main_after(ms, 1000.0f, 127, 4416) == doctest::Approx(std::exp(-t / (ms * 0.001f))).epsilon(2e-3));
  }
  // an accent decays with Acc Decay whatever Decay says, a plain note with Decay whatever Acc Decay says
  CHECK(main_after(400.0f, 2000.0f, 127, 4416) == doctest::Approx(std::exp(-t / 0.4f)).epsilon(2e-3));
  CHECK(main_after(30.0f, 400.0f, 96, 4416) == doctest::Approx(std::exp(-t / 0.4f)).epsilon(2e-3));
  // the default is the old fixed 200 ms, exactly
  Fixture a(kOs, 44100.0f), b(kOs, 44100.0f);
  tb303::voice_set_param(*b.v, tb303::kAccDecay, 200.0f);
  CHECK(a.v->main_d_acc == b.v->main_d_acc);
  CHECK(a.v->main_d_acc == tb303::detail::decay_rate(*a.v, 44100.0f, 200.0f));
  // held to 30..2000, NaN ignored; set_param does not touch the filter or the other envelope factors
  tb303::voice_set_param(*a.v, tb303::kAccDecay, 1.0f);
  CHECK(a.v->p_acc_decay == 30.0f);
  tb303::voice_set_param(*a.v, tb303::kAccDecay, 1.0e9f);
  CHECK(a.v->p_acc_decay == 2000.0f);
  const float norm = a.v->main_d_norm;
  tb303::voice_set_param(*a.v, tb303::kAccDecay, std::nanf(""));
  CHECK(a.v->p_acc_decay == 2000.0f);
  CHECK(a.v->main_d_norm == norm);
}

TEST_CASE("tb303 sndmod: Filter Track puts a factor on the knob cutoff at the note start (an octave per octave at 100 % from note 48), and 0 is the knob") {
  const auto cutoff_coef_after_hold = [](float track, std::uint32_t note, float& factor) {
    Fixture f(kOs, 44100.0f);
    tb303::voice_set_param(*f.v, tb303::kCutoff, 600.0f);
    tb303::voice_set_param(*f.v, tb303::kDecay, 200.0f);
    tb303::voice_set_param(*f.v, tb303::kFilterTrack, track);
    tb303::voice_note_on(*f.v, note, 96, false);
    factor = f.v->track_mul;
    float out[64];
    for (int blk = 0; blk < 1500; ++blk) {  // 2.2 s: the decay (200 ms) is gone, the cutoff is at its resting place
      tb303::voice_render(*f.v, out, 64);
    }
    return f.v->lad;
  };
  float k0, k100, k50;
  const tb303::Ladder base = cutoff_coef_after_hold(0.0f, 60, k0);
  const tb303::Ladder full = cutoff_coef_after_hold(100.0f, 60, k100);
  cutoff_coef_after_hold(50.0f, 60, k50);
  CHECK(k0 == 1.0f);
  CHECK(k100 == doctest::Approx(2.0f).epsilon(1e-5));  // note 60 is an octave above 48
  CHECK(k50 == doctest::Approx(std::sqrt(2.0f)).epsilon(1e-5));
  // the factor reaches the filter: the stage coefficient of the tracked voice is the one the library computes for twice the cutoff the untracked one rests at
  Fixture ref(kOs, 44100.0f);
  tb303::voice_set_param(*ref.v, tb303::kCutoff, 600.0f);
  const float resting = 600.0f * std::exp2(ref.v->env_scaler * (0.0f - ref.v->env_offset));  // the render's cutoff x exp2(expo), expo = scaler * (envelope 0 - offset)
  tb303::detail::filter_coefficients(*ref.v, resting);
  CHECK(base.b0 == doctest::Approx(ref.v->lad.b0).epsilon(2e-3));
  tb303::detail::filter_coefficients(*ref.v, 2.0f * resting);
  CHECK(full.b0 == doctest::Approx(ref.v->lad.b0).epsilon(2e-3));
  CHECK(full.b0 > 1.5f * base.b0);
  // an octave below the reference closes it, the reference leaves it
  float k36, k48;
  cutoff_coef_after_hold(100.0f, 36, k36);
  cutoff_coef_after_hold(100.0f, 48, k48);
  CHECK(k36 == doctest::Approx(0.5f).epsilon(1e-5));
  CHECK(k48 == doctest::Approx(1.0f).epsilon(1e-5));
  // a legato (slide) note sets the factor for its own pitch; a plain note after Track 0 is 1 again; the knob and Track do not move a note's factor until the next note
  Fixture f(kOs, 44100.0f);
  tb303::voice_set_param(*f.v, tb303::kFilterTrack, 100.0f);
  tb303::voice_note_on(*f.v, 48, 96, false);
  CHECK(f.v->track_mul == doctest::Approx(1.0f).epsilon(1e-5));
  tb303::voice_note_on(*f.v, 60, 96, true);
  CHECK(f.v->track_goal == doctest::Approx(2.0f).epsilon(1e-5));  // (a slide: the goal moves at once, the factor follows the glide: the test below)
  CHECK(f.v->track_mul == doctest::Approx(1.0f).epsilon(1e-5));
  tb303::voice_set_param(*f.v, tb303::kCutoff, 900.0f);
  tb303::voice_set_param(*f.v, tb303::kFilterTrack, 0.0f);
  CHECK(f.v->track_goal == doctest::Approx(2.0f).epsilon(1e-5));
  tb303::voice_note_on(*f.v, 72, 96, false);
  CHECK(f.v->track_mul == 1.0f);
  // the highest note at 100 % cannot push the filter past its fit (held to the top of it) nor produce anything but finite samples
  Fixture hi(kOs, 44100.0f);
  tb303::voice_set_param(*hi.v, tb303::kFilterTrack, 100.0f);
  tb303::voice_set_param(*hi.v, tb303::kResonance, 100.0f);
  tb303::voice_set_param(*hi.v, tb303::kTranspose, 24.0f);
  tb303::voice_note_on(*hi.v, 120, 127, false);
  bool finite = true;
  float out[64];
  for (int blk = 0; blk < 300; ++blk) {
    tb303::voice_render(*hi.v, out, 64);
    for (float x : out) {
      finite = finite && std::isfinite(x) && std::fabs(x) < 40.0f;
    }
  }
  CHECK(finite);
}

// Unit finding 2026-10-19 (int.11): the factor was fixed at the note start, so at a slide the cutoff stepped the whole interval while the pitch glided. It follows the pitch now.
TEST_CASE("tb303 sndmod: at Filter Track 100 a legato slide from note 36 to 48 moves the cutoff with the glide (no step), and ends an octave up; pitch() moves the goal") {
  Fixture f(kOs, 44100.0f);
  tb303::voice_set_param(*f.v, tb303::kCutoff, 600.0f);
  tb303::voice_set_param(*f.v, tb303::kFilterTrack, 100.0f);
  tb303::voice_set_param(*f.v, tb303::kEnvMod, 0.0f);
  tb303::voice_set_param(*f.v, tb303::kDecay, 200.0f);
  tb303::voice_note_on(*f.v, 36, 96, false);
  float big[64];
  for (int blk = 0; blk < 1500; ++blk) {  // 2.2 s: the decay (200 ms) is gone, the cutoff is at its resting place
    tb303::voice_render(*f.v, big, 64);
  }
  // The stage coefficient b0 is the filter's cutoff (lad.g2 is the output gain, which hardly depends on it: 3.77 -> 3.89 over this octave); the library's own value at the resting cutoff, and at twice it
  Fixture ref(kOs, 44100.0f);
  tb303::voice_set_param(*ref.v, tb303::kCutoff, 600.0f);
  tb303::voice_set_param(*ref.v, tb303::kEnvMod, 0.0f);
  const float resting = 600.0f * std::exp2(ref.v->env_scaler * (0.0f - ref.v->env_offset));
  tb303::detail::filter_coefficients(*ref.v, 0.5f * resting);  // note 36: the factor is 0.5
  const float b0_low = ref.v->lad.b0;
  tb303::detail::filter_coefficients(*ref.v, resting);  // note 48: 1
  const float b0_high = ref.v->lad.b0;
  const float first = f.v->lad.b0;
  CHECK(first == doctest::Approx(b0_low).epsilon(2e-3));
  float prev = first, worst = 1.0f, last = first;
  tb303::voice_note_on(*f.v, 48, 96, true);  // the slide (Legato Slide is on by default)
  CHECK(f.v->track_goal == doctest::Approx(1.0f).epsilon(1e-5));
  CHECK(f.v->track_mul == doctest::Approx(0.5f).epsilon(1e-5));
  float out[4];
  for (int blk = 0; blk < 6000; ++blk) {  // 0.6 s: far past the 12 ms time constant; one coefficient update a block
    tb303::voice_render(*f.v, out, 4);
    last = f.v->lad.b0;
    worst = last / prev > worst ? last / prev : worst;
    prev = last;
  }
  CAPTURE(worst);
  CHECK(worst < 1.05f);  // (the whole octave, b0 x 1.9, in one update before the fix)
  CHECK(worst > 1.0f);   // and it did move
  CHECK(last == doctest::Approx(b0_high).epsilon(2e-3));  // the octave arrived
  CHECK(last > 1.5f * first);
  // the host's Slide/Glide (OS-MODPITCH): the goal is the played note plus the offset, and 0 offset is the note's own factor; Filter Track 0 keeps 1 (no exp2)
  tb303::voice_pitch(*f.v, -12.0f, 0.0f);
  CHECK(f.v->track_goal == doctest::Approx(0.5f).epsilon(1e-5));
  tb303::voice_pitch(*f.v, 0.0f, 0.0f);
  CHECK(f.v->track_goal == doctest::Approx(1.0f).epsilon(1e-5));
  tb303::voice_set_param(*f.v, tb303::kFilterTrack, 0.0f);
  tb303::voice_pitch(*f.v, 7.0f, 0.0f);
  CHECK(f.v->track_goal == 1.0f);
}

// Unit finding 2026-10-19 (int.12, QA-MOD-152): the tester's metric is the peak second difference d2 of the audio in the 60 ms around the slid note (from 20 ms before to 40 ms after) and it grew with Filter Track
// (36 -> 60, velocity 127: 0.0027 at 0 %, 0.0051 at 50 %, 0.0091 at 100 %; the host reproduces 0.0027 / 0.0051 / 0.0094). That peak is not a click. It sits at the END of the window and rises smoothly with the
// cutoff; a legato "slide" 60 -> 60 (no glide, no cutoff motion) at 100 % has a larger one (0.0123), because d2 of a saw scales with the cutoff squared and Filter Track 100 doubles it at note 60. The click is
// the d2 at the note-on instant itself: 0.0084 with the factor stepped at the slide (int.11, the unit's 0.0109 total), 0.0013 since ec51b91 (Filter Track 0: 0.0022). This test holds the instant, and the
// swell to its destination's own brightness.
namespace {
struct SlideD2 {
  float instant;  // peak d2 from 1 ms before to 3 ms after the second note-on
  float before;   // peak d2 of the note that is left, from 21 ms to 1 ms before the second note-on (a few cycles of it)
  float window;   // peak d2 from 20 ms before to 40 ms after (the unit metric)
};
SlideD2 slide_d2(float track, uint32_t na, uint32_t nb) {
  Fixture f(kOs, 44100.0f);
  using namespace tb303;
  voice_set_param(*f.v, kResonance, 70.0f);
  voice_set_param(*f.v, kCutoff, 1000.0f);
  voice_set_param(*f.v, kEnvMod, 25.0f);
  voice_set_param(*f.v, kDecay, 1000.0f);
  voice_set_param(*f.v, kWaveform, 0.0f);
  voice_set_param(*f.v, kLegato, 1.0f);
  voice_set_param(*f.v, kSlide, 60.0f);
  voice_set_param(*f.v, kFilterTrack, track);
  const uint32_t total = 66150, on1 = 8820, on2 = on1 + 44100;  // 1.5 s: the first note at 0.2 s, the second 1 s later
  std::vector<float> x(total);
  uint32_t i = 0;
  while (i < total) {
    if (i == on1) {
      voice_note_on(*f.v, na, 96, false);
    }
    if (i == on2) {
      voice_note_on(*f.v, nb, 127, true);
    }
    uint32_t n = std::min<uint32_t>(64, total - i);
    n = (i < on1 && i + n > on1) ? on1 - i : n;
    n = (i < on2 && i + n > on2) ? on2 - i : n;
    voice_render(*f.v, &x[i], n);
    i += n;
  }
  auto peak = [&](int lo, int hi) {
    float m = 0.0f;
    for (int k = static_cast<int>(on2) + lo; k < static_cast<int>(on2) + hi; ++k) {
      m = std::max(m, std::fabs(x[k + 1] - 2.0f * x[k] + x[k - 1]));
    }
    return m;
  };
  return SlideD2{peak(-44, 132), peak(-926, -44), peak(-882, 1764)};
}
}  // namespace

TEST_CASE("tb303 sndmod: a legato slide with Filter Track has no click at the slid note; the d2 swell behind it is the destination's own brightness") {
  const SlideD2 ft0 = slide_d2(0.0f, 36, 60);
  const SlideD2 ft50 = slide_d2(50.0f, 36, 60);
  const SlideD2 ft100 = slide_d2(100.0f, 36, 60);
  const SlideD2 stay = slide_d2(100.0f, 60, 60);  // the same destination and cutoff, no glide at all
  const SlideD2 mid100 = slide_d2(100.0f, 48, 60);
  CAPTURE(ft0.instant);
  CAPTURE(ft100.instant);
  CAPTURE(mid100.instant);
  CAPTURE(ft0.window);
  CAPTURE(ft50.window);
  CAPTURE(ft100.window);
  CAPTURE(stay.window);
  // the unit's finding, reproduced: the window peak grows with Filter Track (a steady cutoff of twice the knob at note 60 makes d2 about four times)
  CHECK(ft50.window > 1.5f * ft0.window);
  CHECK(ft100.window > 2.5f * ft0.window);
  // ... and it is the destination's, not a click: a slide that does not move is louder in d2 than the glide
  CHECK(ft100.window < stay.window);
  // the click: the instant of the second note stays within 1.5 x Filter Track 0's (3.7 x with the factor stepped at the slide)
  CHECK(ft100.instant < 1.5f * ft0.instant);
  CHECK(mid100.instant < 1.5f * ft0.instant);
}

// The tester's re-analysis of int.12 (downward slides, 60 -> 36): the instant's d2 over Filter Track 0's was x1.59 at 50 % and x3.98 at 100 % (0.0013 / 0.0021 / 0.0053). Not a click either: the note that is LEFT is that
// much brighter (note 60 at 100 % has twice the cutoff, d2 four times), the 4 ms window holds one of its saw edges, and the window is the same 0.0053 whether the slide goes to 36, to 48 or stays at 60 (the
// glide has not moved yet). A click is an excess over the d2 of the note being left. With the factor stepped at the slide the instant is 1.42 x (60 -> 36, 100 %) and 1.16 x (60 -> 48) its own note's.
TEST_CASE("tb303 sndmod: a downward legato slide with Filter Track adds nothing to the d2 of the note it leaves") {
  for (float track : {50.0f, 100.0f}) {
    for (uint32_t to : {36u, 48u, 60u}) {
      const SlideD2 s = slide_d2(track, 60, to);
      CAPTURE(track);
      CAPTURE(to);
      CAPTURE(s.before);
      CAPTURE(s.instant);
      CHECK(s.before > 0.001f);  // the note that is left sounds (and is the bright one)
      CHECK(s.instant < 1.05f * s.before);
    }
  }
}
