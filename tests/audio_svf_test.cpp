// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-007l: the instrument filter (audio/svf.h, voices.h FILTER). The oracle below is a port of Teensy Audio code (the loop of
// AudioFilterStateVariable::update_variable), so this file carries the upstream notice (also in THIRD_PARTY.md):
//
//   Audio Library for Teensy 3.X
//   Copyright (c) 2014, Paul Stoffregen, paul@pjrc.com
//
//   Development of this audio library was funded by PJRC.COM, LLC by sales of Teensy and Audio Adaptor boards. Please support PJRC's efforts to develop open source
//   software by purchasing Teensy or other PJRC products.
//
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the
//   Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the
//   Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
//
//   The above copyright notice, development funding notice, and this permission notice shall be included in all copies or substantial portions of the Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
//   PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
//   OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// The oracle is a straight port of the fixed-point state variable filter loop of the Teensy Audio library
// (AudioFilterStateVariable, MIT, (c) 2014 Paul Stoffregen, PJRC.COM, LLC), driven with the filter's cutoff and resonance
// law (that of audio/svf.h; the official firmware's is Q): cutoff 0..1 -> index floor(c x 477) -> 20 Hz x 2^(index / 50) (here from the formula, not a table), Q = resonance + 0.7 held to 0.7..5. Against it: the float filter's output on an impulse, a sweep and noise for the three types at
// four cutoffs and three resonances; the bypass (bit for bit); stability at the top resonance; and the voices (one pool filtering, one not) through the same oracle,
// with the Cutoff FX. Host and Cortex-M4F.
#include <doctest/doctest.h>

#include <o3os/audio/svf.h>
#include <o3os/audio/voices.h>
#include <o3os/hal/audio.h>

#include "support/static_slot.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {

using o3os::audio::SampleDesc;
using o3os::audio::Svf;
using o3os::audio::VoicePool;
namespace audio = o3os::audio;

constexpr std::uint32_t kBlk = o3os::hal::audio::kBlock;
constexpr double kPi = 3.14159265358979323846;
constexpr double kFs = static_cast<double>(o3os::hal::audio::sample_rate_mhz()) / 1000.0;  // the engine's rate: 44,100 Hz (44,117.647 Hz with -DO3OS_RATE_44100=OFF; the Teensy's AUDIO_SAMPLE_RATE_EXACT)

// ---- The oracle: the Teensy fixed-point loop ------------------------------------------------------------------------------------------------------

struct Ref {
  std::int32_t fmult = 0, damp = 0, prev = 0, lp = 0, bp = 0;
  std::uint8_t type = 0;
};

// MULT(a, b) = multiply_32x32_rshift32_rounded(a, b) << 2
std::int32_t mult(std::int32_t a, std::int32_t b) {
  const std::int32_t r = static_cast<std::int32_t>((static_cast<std::int64_t>(a) * b + 0x80000000LL) >> 32);
  return static_cast<std::int32_t>(static_cast<std::uint32_t>(r) << 2);
}
// signed_saturate_rshift(v, 16, 13)
std::int16_t sat13(std::int32_t v) {
  const std::int32_t s = v >> 13;
  return static_cast<std::int16_t>(s > 32767 ? 32767 : (s < -32768 ? -32768 : s));
}

Ref ref_make(float cutoff, float stored_resonance, std::uint8_t type) {
  Ref o;
  const float c = cutoff < 0.0f ? 0.0f : (cutoff > 1.0f ? 1.0f : cutoff);
  const unsigned idx = static_cast<std::uint16_t>(static_cast<double>(c) * 477.0);
  double freq = 20.0 * std::exp2(static_cast<double>(idx) / 50.0);
  freq = freq < 20.0 ? 20.0 : (freq > kFs / 2.5 ? kFs / 2.5 : freq);
  o.fmult = static_cast<std::int32_t>(std::sin(freq * (kPi / (kFs * 2.0))) * 2147483647.0);
  double q = static_cast<double>(stored_resonance) + 0.7;
  q = q < 0.7 ? 0.7 : (q > 5.0 ? 5.0 : q);
  o.damp = static_cast<std::int32_t>((1.0 / q) * 1073741824.0);
  o.type = type;
  return o;
}

void ref_run(Ref& o, const std::int16_t* in, std::int16_t* out, std::uint32_t n) {
  for (std::uint32_t k = 0; k < n; ++k) {
    const std::int32_t input = static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::int32_t>(in[k])) << 12);
    std::int32_t lowpass = o.lp + mult(o.fmult, o.bp);
    std::int32_t highpass = ((input + o.prev) >> 1) - lowpass - mult(o.damp, o.bp);
    o.prev = input;
    std::int32_t bandpass = o.bp + mult(o.fmult, highpass);
    const std::int32_t lowpasstmp = lowpass, bandpasstmp = bandpass, highpasstmp = highpass;
    lowpass = lowpass + mult(o.fmult, bandpass);
    highpass = input - lowpass - mult(o.damp, bandpass);
    bandpass = bandpass + mult(o.fmult, highpass);
    o.lp = lowpass;
    o.bp = bandpass;
    out[k] = o.type == 0 ? sat13(lowpass + lowpasstmp) : (o.type == 1 ? sat13(highpass + highpasstmp) : sat13(bandpass + bandpasstmp));
  }
}

// ---- Signals ------------------------------------------------------------------------------------------------------------------------------

constexpr std::uint32_t kLen = 4096;

void make_impulse(std::int16_t* s) {
  std::memset(s, 0, kLen * sizeof(std::int16_t));
  s[0] = 20000;
}
void make_sweep(std::int16_t* s) {  // 20000 x sin, 20 Hz to 16 kHz, exponential, over kLen samples
  double phase = 0.0;
  for (std::uint32_t i = 0; i < kLen; ++i) {
    const double f = 20.0 * std::pow(800.0, static_cast<double>(i) / kLen);
    phase += 2.0 * kPi * f / kFs;
    s[i] = static_cast<std::int16_t>(std::lround(20000.0 * std::sin(phase)));
  }
}
void make_noise(std::int16_t* s) {  // a fixed LCG, +-16000
  std::uint32_t x = 12345u;
  for (std::uint32_t i = 0; i < kLen; ++i) {
    x = x * 1664525u + 1013904223u;
    s[i] = static_cast<std::int16_t>(static_cast<std::int32_t>(x >> 16) % 32001 - 16000);
  }
}

void run_float(std::uint8_t type, float cutoff, float res, const std::int16_t* in, std::int16_t* out, std::uint32_t n) {
  Svf s;
  audio::svf_set(s, cutoff, res);
  std::memcpy(out, in, n * sizeof(std::int16_t));
  switch (type) {
    case audio::kFilterHigh: audio::svf_run<audio::kFilterHigh>(s, out, n); break;
    case audio::kFilterBand: audio::svf_run<audio::kFilterBand>(s, out, n); break;
    default: audio::svf_run<audio::kFilterLow>(s, out, n); break;
  }
}

}  // namespace

TEST_CASE("svf: the float filter is the Teensy fixed-point filter within 2 LSB: 3 types x 4 cutoffs x 3 resonances x impulse, sweep, noise") {
  static std::int16_t sig[3][kLen];
  static std::int16_t want[kLen];
  static std::int16_t got[kLen];
  make_impulse(sig[0]);
  make_sweep(sig[1]);
  make_noise(sig[2]);
  const float cutoffs[] = {0.1f, 0.4f, 0.7f, 1.0f};
  const float resonances[] = {0.0f, 2.0f, 4.3f};  // Q 0.7, 2.7, 5.0
  std::int32_t worst = 0;
  std::uint32_t compared = 0;
  for (std::uint8_t type = 0; type < 3; ++type) {
    for (const float c : cutoffs) {
      for (const float r : resonances) {
        for (int k = 0; k < 3; ++k) {
          Ref o = ref_make(c, r, type);
          ref_run(o, sig[k], want, kLen);
          run_float(type, c, r, sig[k], got, kLen);
          std::int32_t w = 0;
          for (std::uint32_t i = 0; i < kLen; ++i) {
            const std::int32_t e = std::abs(static_cast<std::int32_t>(got[i]) - static_cast<std::int32_t>(want[i]));
            w = e > w ? e : w;
          }
          CHECK_MESSAGE(w <= 2, "type " << int(type) << " cutoff " << c << " resonance " << r << " signal " << k << ": worst " << w << " LSB");
          worst = w > worst ? w : worst;
          compared += kLen;
        }
      }
    }
  }
  MESSAGE("svf oracle: " << compared << " samples compared, worst difference " << worst << " LSB");
}

TEST_CASE("svf: the cutoff maps to the coefficient at every one of the 478 steps (fc = 20 Hz x 2^(i/50), f = 2 sin(pi fc / (2 fs)))") {
  double worst = 0.0;
  for (unsigned i = 0; i <= 477; ++i) {
    CHECK(i / 50u == ((i * 1311u) >> 16));  // the divide the compiler makes of i / 50 is exact over the range
    Svf s;
    // the exact value of c that truncates to i: a hair above i / 477
    audio::svf_set(s, (static_cast<float>(i) + 0.25f) / 477.0f, 0.0f);
    const double fc = 20.0 * std::exp2(static_cast<double>(i) / 50.0);
    const double want = 2.0 * std::sin(fc * (kPi / (kFs * 2.0)));
    const double err = std::fabs(static_cast<double>(s.f) / want - 1.0);
    worst = err > worst ? err : worst;
  }
  CHECK_MESSAGE(worst < 2e-6, "worst relative error of f " << worst);
  Svf lo, hi;
  audio::svf_set(lo, -3.0f, -1.0f);   // out of range: the ends
  audio::svf_set(hi, 7.0f, 99.0f);
  Svf nan;
  audio::svf_set(nan, std::nanf(""), std::nanf(""));
  CHECK(lo.f == doctest::Approx(2.0 * std::sin(20.0 * (kPi / (kFs * 2.0)))).epsilon(1e-6));
  CHECK(hi.d == doctest::Approx(1.0 / 5.0));       // Q held to 5
  CHECK(lo.d == doctest::Approx(1.0 / 0.7));       // and to 0.7
  CHECK(nan.f == lo.f);
  CHECK(nan.d == lo.d);
}

TEST_CASE("svf: stable at the top resonance for every type across the cutoff range: bounded under a full-scale drive, and it rings out") {
  static std::int16_t buf[kBlk];
  for (std::uint8_t type = 0; type < 3; ++type) {
    for (unsigned step = 0; step <= 477; step += 53) {
      const float c = static_cast<float>(step) / 477.0f + 0.001f;
      Svf s;
      audio::svf_set(s, c > 1.0f ? 1.0f : c, 4.3f);  // Q = 5
      float peak = 0.0f;
      // 1024 samples of a full-scale square of period 32 (the worst drive for a resonance near its frequency), then silence until it has decayed
      for (std::uint32_t n = 0; n < 1024 + 90000; n += kBlk) {
        for (std::uint32_t i = 0; i < kBlk; ++i) {
          buf[i] = n < 1024 ? (((n + i) / 16u) & 1u ? std::int16_t{32767} : std::int16_t{-32768}) : std::int16_t{0};
        }
        switch (type) {
          case audio::kFilterHigh: audio::svf_run<audio::kFilterHigh>(s, buf, kBlk); break;
          case audio::kFilterBand: audio::svf_run<audio::kFilterBand>(s, buf, kBlk); break;
          default: audio::svf_run<audio::kFilterLow>(s, buf, kBlk); break;
        }
        const float m = std::fabs(s.lp) > std::fabs(s.bp) ? std::fabs(s.lp) : std::fabs(s.bp);
        peak = m > peak ? m : peak;
        CHECK(std::isfinite(s.lp));
        CHECK(std::isfinite(s.bp));
      }
      CHECK_MESSAGE(peak < 1.0e6f, "type " << int(type) << " step " << step << ": state peak " << peak);  // the Q of 5 is at most 5x the drive, 164k
      const float left = std::fabs(s.lp) > std::fabs(s.bp) ? std::fabs(s.lp) : std::fabs(s.bp);
      CHECK_MESSAGE(left < 0.5f, "type " << int(type) << " step " << step << " did not ring out: " << s.lp << ", " << s.bp);
    }
  }
}

// ---- The voices ---------------------------------------------------------------------------------------------------------------------------

namespace {

VoicePool& pool_a() {
  static o3os::test::StaticSlot<VoicePool> slot;
  return slot.make();
}
VoicePool& pool_b() {
  static o3os::test::StaticSlot<VoicePool> slot;
  return slot.make();
}

constexpr std::uint32_t kSampleLen = 6000;
std::int16_t g_sine[kSampleLen];

void init_sine() {
  for (std::uint32_t i = 0; i < kSampleLen; ++i) {
    g_sine[i] = static_cast<std::int16_t>(std::lround(20000.0 * std::sin(2.0 * kPi * 37.3 * static_cast<double>(i) / kSampleLen) + 6000.0 * std::sin(0.9 * static_cast<double>(i))));
  }
}

SampleDesc desc_of(bool filter, std::uint8_t type, float cutoff, float res) {
  SampleDesc d{g_sine, kSampleLen, 0, 0, 65536};
  d.filter.enable = filter;
  d.filter.type = type;
  d.filter.cutoff = cutoff;
  d.filter.resonance = res;
  return d;
}

// `blocks` whole blocks of track 0 into out; each block is rendered as one span unless `split` > 0, when it is two spans at that offset.
void render_n(VoicePool& p, std::int16_t* out, std::uint32_t blocks, std::uint32_t split = 0) {
  for (std::uint32_t b = 0; b < blocks; ++b) {
    p.begin_block();
    if (split != 0u) {
      p.render(0, split);
      p.render(split, kBlk);
    } else {
      p.render(0, kBlk);
    }
    p.end_block();
    const std::int16_t* f = p.feed(0);
    for (std::uint32_t i = 0; i < kBlk; ++i) {
      out[b * kBlk + i] = f != nullptr ? f[i] : std::int16_t{0};
    }
  }
}

constexpr std::uint32_t kBlocks = 60;
std::int16_t g_plain[kBlocks * kBlk], g_filt[kBlocks * kBlk], g_want[kBlocks * kBlk];

}  // namespace

TEST_CASE("svf: a disabled filter is a bypass: the voices' output is bit for bit what it is without one, whatever the cutoff and resonance say") {
  init_sine();
  VoicePool& a = pool_a();
  VoicePool& b = pool_b();
  a.set_sample(0, desc_of(false, 0, 0.0f, 0.0f));
  b.set_sample(0, desc_of(false, audio::kFilterBand, 0.2f, 4.3f));
  a.note_on(0, 60, 0, 127);
  b.note_on(0, 60, 0, 127);
  render_n(a, g_plain, kBlocks);
  render_n(b, g_filt, kBlocks);
  CHECK(std::memcmp(g_plain, g_filt, sizeof g_plain) == 0);
  // an unknown type is bypassed too (no filter type has an output for it)
  b.set_sample(0, desc_of(true, 3, 0.2f, 4.3f));
  b.note_on(0, 60, 0, 127);
  render_n(b, g_filt, kBlocks);
  a.note_on(0, 60, 0, 127);
  render_n(a, g_plain, kBlocks);
  CHECK(std::memcmp(g_plain, g_filt, sizeof g_plain) == 0);
}

TEST_CASE("svf: a filtering voice is the oracle's filter over what the same voice puts out unfiltered, within 2 LSB, for the three types") {
  init_sine();
  const float cutoff = 0.45f;
  const float res = 3.0f;
  for (std::uint8_t type = 0; type < 3; ++type) {
    VoicePool& a = pool_a();
    VoicePool& b = pool_b();
    a.set_sample(0, desc_of(false, 0, 0.0f, 0.0f));
    b.set_sample(0, desc_of(true, type, cutoff, res));
    a.note_on(0, 60, 0, 127);
    b.note_on(0, 60, 0, 127);
    render_n(a, g_plain, kBlocks);
    render_n(b, g_filt, kBlocks);
    Ref o = ref_make(cutoff, res, type);
    ref_run(o, g_plain, g_want, kBlocks * kBlk);
    std::int32_t worst = 0;
    std::int32_t moved = 0;
    for (std::uint32_t i = 0; i < kBlocks * kBlk; ++i) {
      const std::int32_t e = std::abs(static_cast<std::int32_t>(g_filt[i]) - static_cast<std::int32_t>(g_want[i]));
      worst = e > worst ? e : worst;
      moved = std::abs(static_cast<std::int32_t>(g_filt[i]) - static_cast<std::int32_t>(g_plain[i])) > moved ? std::abs(static_cast<std::int32_t>(g_filt[i]) - static_cast<std::int32_t>(g_plain[i])) : moved;
    }
    CHECK_MESSAGE(worst <= 2, "type " << int(type) << ": worst " << worst << " LSB");
    CHECK_MESSAGE(moved > 1000, "type " << int(type) << ": the filter did change the signal (" << moved << ")");
  }
}

TEST_CASE("svf: the filter does not depend on how the block is split into spans (events inside a block)") {
  init_sine();
  VoicePool& a = pool_a();
  VoicePool& b = pool_b();
  a.set_sample(0, desc_of(true, audio::kFilterHigh, 0.6f, 2.0f));
  b.set_sample(0, desc_of(true, audio::kFilterHigh, 0.6f, 2.0f));
  a.note_on(0, 64, 0, 127);
  b.note_on(0, 64, 0, 127);
  render_n(a, g_plain, kBlocks);
  render_n(b, g_filt, kBlocks, 17);
  CHECK(std::memcmp(g_plain, g_filt, sizeof g_plain) == 0);
}

TEST_CASE("svf: the Cutoff FX filters a note whose instrument has no filter exactly as the instrument's own filter would (resonance 0), and ends back to the bypass") {
  init_sine();
  for (std::uint8_t type = 0; type < 3; ++type) {
    VoicePool& a = pool_a();
    VoicePool& b = pool_b();
    // a: the instrument filters (type, cutoff 0.37, resonance 0); b: the instrument does not, the track has the FX (type, value 37)
    a.set_sample(0, desc_of(true, type, 37.0f / 100.0f, 0.0f));
    b.set_sample(0, desc_of(false, 0, 0.0f, 0.0f));
    b.set_filter_fx(0, type, 37);
    a.note_on(0, 60, 0, 127);
    b.note_on(0, 60, 0, 127);
    render_n(a, g_plain, kBlocks);
    render_n(b, g_filt, kBlocks);
    CHECK(std::memcmp(g_plain, g_filt, sizeof g_plain) == 0);
    b.clear_filter_fx(0);
  }
  // The FX arriving while the note sounds acts at once (from the next span), and its end restores the instrument's (here no) filter: the output then is the
  // unfiltered voice's again, sample for sample.
  VoicePool& a = pool_a();
  VoicePool& b = pool_b();
  a.set_sample(0, desc_of(false, 0, 0.0f, 0.0f));
  b.set_sample(0, desc_of(false, 0, 0.0f, 0.0f));
  a.note_on(0, 60, 0, 127);
  b.note_on(0, 60, 0, 127);
  render_n(a, g_plain, 30);
  render_n(b, g_filt, 10);
  b.set_filter_fx(0, audio::kFilterLow, 20);
  render_n(b, g_filt + 10 * kBlk, 10);
  b.clear_filter_fx(0);
  render_n(b, g_filt + 20 * kBlk, 10);
  CHECK(std::memcmp(g_plain, g_filt, 10 * kBlk * sizeof(std::int16_t)) == 0);   // before the FX
  CHECK(std::memcmp(g_plain + 10 * kBlk, g_filt + 10 * kBlk, 10 * kBlk * sizeof(std::int16_t)) != 0);  // under it
  CHECK(std::memcmp(g_plain + 20 * kBlk, g_filt + 20 * kBlk, 10 * kBlk * sizeof(std::int16_t)) == 0);  // after it
  // a type outside the three, or a track outside the pool, is ignored
  b.set_filter_fx(0, 3, 50);
  b.set_filter_fx(99, 0, 50);
  b.note_on(0, 60, 0, 127);
  a.note_on(0, 60, 0, 127);
  render_n(a, g_plain, 4);
  render_n(b, g_filt, 4);
  CHECK(std::memcmp(g_plain, g_filt, 4 * kBlk * sizeof(std::int16_t)) == 0);
}
