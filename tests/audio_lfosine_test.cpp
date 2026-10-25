// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-LFOSINE (docs/specs/OS-LFOSINE.md, audio/lfo_sine.h): the LFO shape Sine, shape 5. The law itself (error bound against libm, the peak and the trough exact, symmetric, monotone on the rise, no value
// outside 0..1), then the generator in the voices: the period and the phase of the autopan against -A cos 2 pi u, the amount, the tempo-sync speeds, every destination moving, a live shape edit equal to a
// note on with the shape, and a shape above 5 held to Triangle. Host only (libm is the reference).
#include <doctest/doctest.h>

#include "support/lfo_probe.h"

#include <o3os/audio/lfo_sine.h>

#define REQ LFO_REQ

TEST_CASE("lfo sine: the law is (1 - cos 2 pi u) / 2 within 5e-5, exact at the ends, symmetric, monotone on the rise, inside 0..1") {
  CHECK(audio::lfo_sine_level(0.0f) == 0.0f);
  CHECK(audio::lfo_sine_level(0.5f) >= 0.99999f);
  CHECK(audio::lfo_sine_level(0.5f) <= 1.0f);
  CHECK(audio::lfo_sine_level(0.25f) == doctest::Approx(0.5f).epsilon(1e-4));
  CHECK(audio::lfo_sine_level(0.75f) == doctest::Approx(0.5f).epsilon(1e-4));
  double worst = 0.0;
  float last = -1.0f;
  unsigned outside = 0, falling = 0, asymmetric = 0;
  constexpr unsigned kN = 100000;
  for (unsigned i = 0; i < kN; ++i) {
    const float u = static_cast<float>(i) / static_cast<float>(kN);
    const float s = audio::lfo_sine_level(u);
    const double ideal = 0.5 - 0.5 * std::cos(6.283185307179586 * static_cast<double>(u));
    worst = std::max(worst, std::abs(static_cast<double>(s) - ideal));
    outside += (s < 0.0f || s > 1.0f) ? 1u : 0u;
    if (u <= 0.5f) {
      falling += s < last - 1e-6f ? 1u : 0u;  // up to the half period it does not fall
      last = s;
    }
    asymmetric += std::abs(s - audio::lfo_sine_level(1.0f - u)) > 2e-6f ? 1u : 0u;  // the same either side of the half
  }
  CHECK_MESSAGE(worst <= 5e-5, "worst error " << worst);
  CHECK(outside == 0u);
  CHECK(falling == 0u);
  CHECK(asymmetric == 0u);
}

namespace {

constexpr double kSamplesPerStep = 44100.0 / 20.0;  // 300 BPM (kTempo): 20 steps a second

// The pan's modulation of an autopan LFO at amount `a`: pan = 50 + int(100 x (2 level A - A)) = 50 + int(-100 A cos 2 pi u); the series of blocks with the transport stopped. Returns the rising crossings of 50.
struct Wave {
  std::vector<double> v;
  std::vector<double> rises;  // block positions (interpolated) where the pan crosses 50 upwards
};

Wave pan_wave(std::uint8_t shape, std::uint8_t rate, float amount, unsigned blocks) {
  Wave w;
  w.v = series(kPan, lfo(shape, rate, amount), blocks, false);
  for (std::size_t i = 9; i < w.v.size(); ++i) {
    if (w.v[i - 1] < 50.0 && w.v[i] >= 50.0 && w.v[i] > w.v[i - 1]) {
      w.rises.push_back(static_cast<double>(i - 1) + (50.0 - w.v[i - 1]) / (w.v[i] - w.v[i - 1]));
    }
  }
  return w;
}

double period_blocks(unsigned steps) { return static_cast<double>(steps) * kSamplesPerStep / kBlk; }

}  // namespace

TEST_CASE("lfo sine: the autopan follows -A cos 2 pi u: the period of the tempo-sync speed, the amplitude of the amount, the phase of the Triangle") {
  struct Case {
    std::uint8_t rate;
    unsigned steps;
    float amount;
  };
  // rate index 8 is 8 steps, 10 is 4, 12 is 2 (audio::kLfoRate)
  for (const Case& c : {Case{8, 8, 0.4f}, Case{8, 8, 0.2f}, Case{10, 4, 0.4f}, Case{12, 2, 0.3f}}) {
    const double period = period_blocks(c.steps);
    const unsigned blocks = static_cast<unsigned>(3.2 * period) + 20u;
    const Wave w = pan_wave(audio::kLfoShapeSine, c.rate, c.amount, blocks);
    REQ(w.rises.size() >= 2u);
    CHECK_MESSAGE(std::abs((w.rises[1] - w.rises[0]) - period) <= 1.5, "rate " << static_cast<unsigned>(c.rate) << ": period " << (w.rises[1] - w.rises[0]) << " blocks against " << period);
    // The amplitude: 50 +- 100 A, to the pan's whole units.
    double lo = 1e9, hi = -1e9;
    for (std::size_t i = 9; i < w.v.size(); ++i) {
      lo = std::min(lo, w.v[i]);
      hi = std::max(hi, w.v[i]);
    }
    const double amp = 100.0 * static_cast<double>(c.amount);
    CHECK_MESSAGE(std::abs(hi - (50.0 + amp)) <= 2.0, "hi " << hi << " against " << 50.0 + amp);
    CHECK_MESSAGE(std::abs(lo - (50.0 - amp)) <= 2.0, "lo " << lo << " against " << 50.0 - amp);
    // The shape: from the first rising crossing (u = 1/4 of -cos), every block of a period against -A cos 2 pi u, within what the phase of one block and the whole-unit pan allow. The Triangle is 8 units
    // off this curve at its worst at amount 0.4 (and the cases with a short period have fewer blocks a period, so the allowance follows the slope).
    const double slope = amp * 6.283185307179586 / period;  // units a block, at the steepest
    const double allow = 2.5 + 1.6 * slope;
    unsigned off = 0;
    double worst = 0;
    for (std::size_t i = static_cast<std::size_t>(w.rises[0]) + 1u; i < w.v.size() && static_cast<double>(i) < w.rises[0] + period; ++i) {
      const double u = 0.25 + (static_cast<double>(i) - w.rises[0]) / period;
      const double ideal = 50.0 - amp * std::cos(6.283185307179586 * u);
      worst = std::max(worst, std::abs(w.v[i] - ideal));
      off += std::abs(w.v[i] - ideal) > allow ? 1u : 0u;
    }
    CHECK_MESSAGE(off == 0u, "rate " << static_cast<unsigned>(c.rate) << " amount " << c.amount << ": " << off << " blocks off the cosine, worst " << worst << " (allowed " << allow << ")");
  }
}

TEST_CASE("lfo sine: is not the Triangle: the two differ by several pan units between the rising crossing and the peak") {
  const double period = period_blocks(8);
  const unsigned blocks = static_cast<unsigned>(2.2 * period);
  const Wave sine = pan_wave(audio::kLfoShapeSine, 8, 0.4f, blocks);
  const Wave tri = pan_wave(2, 8, 0.4f, blocks);
  REQ(!sine.rises.empty() && !tri.rises.empty());
  // The triangle of this generator is A (4u - 1) up to the half: it crosses 50 upwards at u = 1/4 as the sine does, and they differ by 0.07 x 100 A = 2.9 units at u = 3/8 and by more elsewhere
  // (the largest gap, near u = 1/8 and 3/8 of a half period, is 0.21 A = 8 units at amount 0.4).
  double worst = 0;
  const std::size_t from = static_cast<std::size_t>(std::max(sine.rises[0], tri.rises[0])) + 1u;
  for (std::size_t i = from; i < from + static_cast<std::size_t>(period * 0.5); ++i) {
    worst = std::max(worst, std::abs(sine.v[i] - tri.v[i]));
  }
  CHECK(worst >= 4.0);  // pan units, amount 0.4
}

namespace {

// The series with the transport running: a Step event every 17 blocks (a step is 50 ms at kTempo: 17.2 blocks of 128), which is what moves the LFO's count when a transport runs.
std::vector<double> running_series(Dest dest, const audio::FilterMod& m, unsigned blocks) {
  VoicePool& p = pool(0);
  p.set_sample(0, desc_for(dest, m));
  p.set_tempo_x100(kTempo);
  p.restart_clock();
  p.on_step(0);
  p.note_on(0, 60, 0, audio::kVelocityDefault);
  p.mark_live();
  Probe pr{dest, 0};
  std::vector<double> out;
  for (unsigned b = 0; b < blocks; ++b) {
    if (b != 0u && b % 17u == 0u) {
      p.on_step(0);
    }
    out.push_back(step_block(p, pr));
  }
  return out;
}

}  // namespace

TEST_CASE("lfo sine: every destination that shares the generator moves with it (tremolo, pan, finetune, cutoff, wavetable window, granular position)") {
  const struct {
    Dest dest;
    const char* name;
    double min_spread;
    unsigned min_distinct;
    double quantum;
  } cases[kDests] = {
      {kAmp, "amp (tremolo)", 3000.0, 6, 500.0}, {kPan, "pan", 40.0, 6, 5.0}, {kTune, "finetune", 5.0, 3, 2.0},
      {kCut, "cutoff", 500.0, 6, 150.0},         {kWt, "wt position", 5.0, 6, 1.0}, {kGran, "grain position", 3000.0, 6, 600.0},
  };
  for (const auto& c : cases) {
    const std::vector<double> v = series(c.dest, lfo(audio::kLfoShapeSine, kRate8, 1.0f), 2 * kPeriodBlocks, false);
    CHECK_MESSAGE(spread(v, 8) >= c.min_spread, c.name << ": spread " << spread(v, 8));
    CHECK_MESSAGE(distinct(v, 8, c.quantum) >= c.min_distinct, c.name << ": distinct " << distinct(v, 8, c.quantum));
    const std::vector<double> r = running_series(c.dest, lfo(audio::kLfoShapeSine, kRate8, 1.0f), 2 * kPeriodBlocks);  // the transport running as well
    CHECK_MESSAGE(spread(r, 8) >= c.min_spread, c.name << " (running): spread " << spread(r, 8));
  }
}

TEST_CASE("lfo sine: the tremolo's level rises from 0 to the amount and back, once a period") {
  const std::vector<double> v = series(kAmp, lfo(audio::kLfoShapeSine, kRate8, 1.0f), static_cast<unsigned>(1.5 * kPeriodBlocks), false);
  double hi = 0, lo = 1e9;
  for (std::size_t i = 12; i < v.size(); ++i) {
    hi = std::max(hi, v[i]);
    lo = std::min(lo, v[i]);
  }
  CHECK(hi > 0.0);
  CHECK(lo < 0.1 * hi);          // from (near) silence ...
  CHECK(hi > 5.0 * (lo + 1.0));  // ... to the full level
}

TEST_CASE("lfo sine: a live shape edit to Sine is what the note on with Sine does, from the second block after the edit; a shape above 5 is Triangle") {
  VoicePool& a = pool(0);
  VoicePool& c = pool(2);
  for (VoicePool* p : {&a, &c}) {
    p->set_sample(0, desc_for(kPan, lfo(p == &a ? 1 : audio::kLfoShapeSine, kRate8, 0.5f)));
    p->set_tempo_x100(kTempo);
    p->restart_clock();
    p->transport_stopped();
    REQ(p->note_on(0, 60, 0, audio::kVelocityDefault));
    p->mark_live();
  }
  Probe pa{kPan, 0}, pc{kPan, 0};
  unsigned equal = 0, total = 0;
  for (unsigned b = 0; b < 2 * kPeriodBlocks; ++b) {
    if (b == 60) {
      std::uint32_t pan = 0;
      CHECK(a.apply_param(0, ip::lfo(1, ip::LfoShape), 5.0f, pan));
    }
    const double xa = step_block(a, pa);
    const double xc = step_block(c, pc);
    if (b > 61) {
      ++total;
      equal += xa == xc ? 1u : 0u;
    }
  }
  CHECK_MESSAGE(equal == total, equal << " of " << total << " blocks equal after the edit");
  audio::SampleDesc d = desc_for(kPan, lfo(1, kRate8, 1.0f));
  CHECK(audio::apply_instrument_param(d, ip::lfo(1, ip::LfoShape), 5.0f, audio::LevelLaw::Og12) == audio::InstApply::Applied);
  CHECK(d.pan_mod.shape == audio::kLfoShapeSine);
  CHECK(audio::apply_instrument_param(d, ip::lfo(1, ip::LfoShape), 6.0f, audio::LevelLaw::Og12) == audio::InstApply::Applied);
  CHECK(d.pan_mod.shape == 2u);  // a larger shape is a triangle
}

TEST_CASE("lfo sine: a descriptor with a shape above 5 plays as the Triangle (sanitize_mod)") {
  const std::vector<double> big = series(kPan, lfo(9, kRate8, 0.4f), 2 * kPeriodBlocks, false);
  const std::vector<double> tri = series(kPan, lfo(2, kRate8, 0.4f), 2 * kPeriodBlocks, false);
  REQ(big.size() == tri.size());
  CHECK(big == tri);
}
