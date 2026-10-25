// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-007m: the instrument filter's modulator, the cutoff ADSR and the cutoff LFO (audio/voices.h FILTER MODULATION). The oracles are written here, in double, from the behaviour
// audio/voices.h FILTER MODULATION documents (linear stages in milliseconds added to the cutoff and held to 0..1; an LFO that is the sequencer's tick count over a period of (576 x rate) ticks),
// independently of the voice code. The pool's cutoff is read through VoicePool::filter_cutoff() at the last evaluation of a block, which is sample 48 of it (the control
// grid is every 16th sample), so the time of a reading is known to the sample. Against them: the ADSR's stage times and levels from a held note, its release, its retrigger
// from the old level, the clamps; the LFO's five shapes at known phases, its period against the tempo, its lock to the transport (not to the note) with the step shift;
// the base from the Cutoff FX and the instrument with its filter off; the output not depending on how the block is cut into spans; the sanitising; and the whole path
// through the Engine (FxReset, Step, the tempo hook). Host and Cortex-M4F.
#include <doctest/doctest.h>

#include <o3os/audio/engine.h>
#include <o3os/audio/event_source.h>
#include <o3os/audio/voices.h>
#include <o3os/hal/audio.h>

#include "support/static_slot.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>

namespace {

namespace audio = o3os::audio;
namespace haudio = o3os::hal::audio;
using audio::Engine;
using audio::EventKind;
using audio::FilterMod;
using audio::SampleDesc;
using audio::SeqEvent;
using audio::VoicePool;

constexpr std::uint32_t kBlk = haudio::kBlock;
constexpr double kSamplesPerMs = static_cast<double>(haudio::sample_rate_mhz()) * 1e-6;
constexpr double kTicksPerSamplePerX100 = 384.0 / static_cast<double>(haudio::sample_rate_mhz());  // 576 ticks a step x (BPM / 15) steps a second / the rate
constexpr std::uint32_t kLen = 6000;
constexpr double kEvalAt = 48.0;  // the last evaluation of a block, in samples from its start

VoicePool& pool_a() {
  static o3os::test::StaticSlot<VoicePool> slot;
  return slot.make();
}
VoicePool& pool_b() {
  static o3os::test::StaticSlot<VoicePool> slot;
  return slot.make();
}

std::int16_t g_pcm[kLen];

void init_pcm() {
  for (std::uint32_t i = 0; i < kLen; ++i) {
    g_pcm[i] = static_cast<std::int16_t>(std::lround(14000.0 * std::sin(0.31 * static_cast<double>(i)) + 9000.0 * std::sin(0.047 * static_cast<double>(i))));
  }
}

// A looping instrument (the note lasts) with the filter on, low-pass, resonance 0, cutoff `base`, the modulator `m`, and an amp envelope with a long release so that a NoteOff
// does not end the voice (the ADSR's release is then audible in the cutoff).
SampleDesc desc(const FilterMod& m, float base, bool filter = true) {
  init_pcm();
  SampleDesc d{g_pcm, kLen, 0, kLen, 65536};
  d.filter.enable = filter;
  d.filter.type = audio::kFilterLow;
  d.filter.cutoff = base;
  d.filter.resonance = 0.0f;
  d.filter.mod = m;
  d.env.enable = true;
  d.env.release = 60000;
  return d;
}

FilterMod adsr(float amount, float sustain, std::uint16_t delay, std::uint16_t attack, std::uint16_t hold, std::uint16_t decay, std::uint16_t release) {
  FilterMod m;
  m.kind = audio::kFmodEnv;
  m.amount = amount;
  m.sustain = sustain;
  m.delay = delay;
  m.attack = attack;
  m.hold = hold;
  m.decay = decay;
  m.release = release;
  return m;
}

FilterMod lfo(std::uint8_t shape, std::uint8_t rate, float amount) {
  FilterMod m;
  m.kind = audio::kFmodLfo;
  m.shape = shape;
  m.rate = rate;
  m.amount = amount;
  return m;
}

void run(VoicePool& p, std::uint32_t blocks) {
  for (std::uint32_t b = 0; b < blocks; ++b) {
    p.begin_block();
    p.render(0, kBlk);
    p.end_block();
  }
}

double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

// ---- The ADSR's oracle: the level at sample `t` after the note on, the key held until `t_off` (a negative t_off: never) ----------------------------------------------------
struct Adsr {
  double amount, sustain, delay, attack, hold, decay, release;  // ms
};

double held_level(const Adsr& a, double t, double kill) {
  const double d = a.delay * kSamplesPerMs;
  const double at = a.attack * kSamplesPerMs;
  const double h = a.hold * kSamplesPerMs;
  const double dc = a.decay * kSamplesPerMs;
  if (t < d) {
    return kill;
  }
  if (t < d + at) {
    const double v = kill + ((t - d) / at) * (a.amount - kill);
    return v < kill ? kill : (v > 1.0 ? 1.0 : v);  // never below where it started, never past 1
  }
  if (t < d + at + h) {
    return a.amount;
  }
  if (t < d + at + h + dc) {
    const double v = ((1.0 - (t - d - at - h) / dc) * (1.0 - a.sustain) + a.sustain) * a.amount;
    return v < -1.0 ? -1.0 : (v > 1.0 ? 1.0 : v);
  }
  const double v = a.sustain * a.amount;
  return v < -1.0 ? -1.0 : (v > 1.0 ? 1.0 : v);
}

// The release starts at the first evaluation after the NoteOff, from the level of the one before it (16 samples earlier).
double adsr_level(const Adsr& a, double t, double t_off, double kill) {
  if (t_off < 0.0 || t < t_off) {
    return held_level(a, t, kill);
  }
  const double from = held_level(a, t_off - 16.0, kill);
  const double rel = a.release * kSamplesPerMs;
  const double v = (1.0 - (t - t_off) / rel) * from;
  return v < 0.0 && from >= 0.0 ? 0.0 : v;
}

// ---- The LFO's oracle: A x shape(u), u the phase in the period ----------------------------------------------------------------------------------------------------------
double lfo_shape(unsigned shape, double u, double a) {
  switch (shape) {
    case 0: return a * (1.0 - 2.0 * u);
    case 1: return a * (2.0 * u - 1.0);
    case 2: return u < 0.5 ? a * (4.0 * u - 1.0) : a * (3.0 - 4.0 * u);
    case 3: return u < 0.5 ? a : -a;
    default: return 0.0;
  }
}

// The tick count the LFO reads `samples` after the transport's last Step (a Step at tick 0), at tempo x100.
double ticks_at(double samples, std::uint32_t x100) { return samples * static_cast<double>(x100) * kTicksPerSamplePerX100; }

// True when `got` is the oracle's value at the tick count `ticks` or one tick either side (the pool's count is an integer that may sit across the floor of a product).
bool lfo_matches(float got, double base, unsigned shape, double a, double ticks, std::uint32_t period, std::uint32_t shift) {
  const std::int64_t x0 = static_cast<std::int64_t>(std::floor(ticks + 1e-9)) + shift;
  for (int d = -1; d <= 1; ++d) {
    const std::int64_t x = x0 + d;
    const double u = static_cast<double>(((x % period) + period) % period) / static_cast<double>(period);
    if (std::fabs(static_cast<double>(got) - clamp01(base + lfo_shape(shape, u, a))) < 2e-5) {
      return true;
    }
  }
  return false;
}

constexpr std::uint32_t kPeriod17 = 216;  // rate index 17 is 0.375 step: 576 x 0.375 ticks

}  // namespace

TEST_CASE("filter mod: the tempo-sync rates make periods of 576 x rate ticks, truncated in float (a third of a step is 191 ticks)") {
  const auto period = [](unsigned i) { return static_cast<std::uint32_t>(static_cast<float>(audio::kTicksPerStep) * audio::kLfoRate[i]); };
  CHECK(period(0) == 73728u);  // 128 steps
  CHECK(period(6) == 9216u);
  CHECK(period(14) == 576u);   // one step
  CHECK(period(17) == kPeriod17);
  CHECK(period(18) == 191u);   // 0.333333 x 576 = 191.9998
  CHECK(period(21) == 96u);
  CHECK(period(23) == 47u);
  CHECK(period(25) == 24u);
  CHECK(period(27) == 11u);
  CHECK(period(28) == 9u);     // 1/64 step
  CHECK(audio::kLfoRates == 29u);
}

TEST_CASE("filter mod: the cutoff ADSR walks its stages in milliseconds (delay, attack, hold, decay, sustain) and releases from where it is, added to the base and held to 0..1") {
  const Adsr a{0.6, 0.4, 5, 40, 3, 30, 50};
  VoicePool& p = pool_a();
  p.set_sample(0, desc(adsr(0.6f, 0.4f, 5, 40, 3, 30, 50), 0.3f));
  p.note_on(0, 60, 0, 127);
  const double t_off = 100.0 * kBlk;
  double worst = 0.0;
  for (std::uint32_t b = 0; b < 150; ++b) {
    if (b == 100) {
      p.note_off(0);
    }
    run(p, 1);
    const double t = static_cast<double>(b * kBlk) + kEvalAt;
    const double want = clamp01(0.3 + adsr_level(a, t, t_off, 0.0));
    const double err = std::fabs(static_cast<double>(p.filter_cutoff(0)) - want);
    worst = err > worst ? err : worst;
  }
  CHECK_MESSAGE(worst < 2e-5, "worst difference " << worst);
  // the corners: delay holds the base, the attack's end is base + amount, the sustain is base + sustain x amount, the release's end is the base again
  CHECK(std::fabs(static_cast<double>(p.filter_cutoff(0)) - 0.3) < 1e-6);
}

TEST_CASE("filter mod: a retrigger's ADSR starts from the old level, a fresh voice from zero") {
  const FilterMod m = adsr(0.6f, 0.4f, 5, 40, 3, 30, 50);
  VoicePool& a = pool_a();
  VoicePool& b = pool_b();
  a.set_sample(0, desc(m, 0.3f));
  b.set_sample(0, desc(m, 0.3f));
  a.note_on(0, 60, 0, 127);
  run(a, 60);  // 3840 samples: in the sustain, 0.4 x 0.6 = 0.24
  CHECK(std::fabs(static_cast<double>(a.filter_cutoff(0)) - 0.54) < 2e-5);
  a.note_on(0, 62, 0, 127);
  b.note_on(0, 62, 0, 127);
  run(a, 1);
  run(b, 1);
  CHECK_MESSAGE(std::fabs(static_cast<double>(a.filter_cutoff(0)) - 0.54) < 2e-5, "the retrigger's delay holds the old level: " << a.filter_cutoff(0));
  CHECK_MESSAGE(std::fabs(static_cast<double>(b.filter_cutoff(0)) - 0.30) < 2e-5, "a fresh note's delay holds zero: " << b.filter_cutoff(0));
}

TEST_CASE("filter mod: the sum is held to 0..1, and an attack toward a negative amount stays where it started until the hold") {
  VoicePool& a = pool_a();
  VoicePool& b = pool_b();
  a.set_sample(0, desc(adsr(0.5f, 1.0f, 0, 40, 3, 30, 50), 0.9f));    // 0.9 + 0.5 is past the top
  b.set_sample(0, desc(adsr(-0.5f, 1.0f, 0, 40, 3, 30, 50), 0.1f));   // 0.1 - 0.5 is below the bottom
  a.note_on(0, 60, 0, 127);
  b.note_on(0, 60, 0, 127);
  run(a, 5);
  run(b, 5);  // t = 368: in the attack (0 .. 1765 samples)
  CHECK(std::fabs(static_cast<double>(b.filter_cutoff(0)) - 0.1) < 1e-6);  // the attack to -0.5 never goes below its start
  run(a, 30);
  run(b, 30);  // t = 2288: in the sustain
  CHECK(a.filter_cutoff(0) == 1.0f);
  CHECK(b.filter_cutoff(0) == 0.0f);
}

TEST_CASE("filter mod: the LFO's five shapes at known phases of the transport (restart, Step 0, 120 BPM), A = 0.25 around the base") {
  for (unsigned shape = 0; shape < 4; ++shape) {
    VoicePool& p = pool_a();
    p.set_sample(0, desc(lfo(static_cast<std::uint8_t>(shape), 17, 0.25f), 0.5f));
    p.restart_clock();
    p.on_step(0);
    p.note_on(0, 60, 0, 127);
    std::uint32_t bad = 0;
    for (std::uint32_t b = 0; b < 75; ++b) {  // 4800 samples: 501 ticks, 2.3 periods, inside the step
      run(p, 1);
      const double ticks = ticks_at(static_cast<double>(b * kBlk) + kEvalAt, 12000);
      bad += lfo_matches(p.filter_cutoff(0), 0.5, shape, 0.25, ticks, kPeriod17, 0) ? 0u : 1u;
    }
    CHECK_MESSAGE(bad == 0u, "shape " << shape << ": " << bad << " blocks off the oracle");
  }
  // random: a level in [base - A, base + A), the same for a whole period, new in the next
  VoicePool& p = pool_a();
  p.set_sample(0, desc(lfo(4, 17, 0.25f), 0.5f));
  p.restart_clock();
  p.on_step(0);
  p.note_on(0, 60, 0, 127);
  float seen[3] = {-1.0f, -1.0f, -1.0f};
  for (std::uint32_t b = 0; b < 75; ++b) {
    run(p, 1);
    const double ticks = ticks_at(static_cast<double>(b * kBlk) + kEvalAt, 12000);
    const unsigned k = static_cast<unsigned>(ticks / kPeriod17);
    const float c = p.filter_cutoff(0);
    CHECK(c >= 0.25f);
    CHECK(c < 0.75f);
    if (k < 3u) {
      if (seen[k] < 0.0f) {
        seen[k] = c;
      }
      CHECK_MESSAGE(c == seen[k], "block " << b << ": period " << k << " changed its level");
    }
  }
  CHECK(seen[0] != seen[1]);
  CHECK(seen[1] != seen[2]);
}

TEST_CASE("filter mod: the LFO's period in samples follows the tempo: 216 ticks at 60, 120 and 240 BPM") {
  for (const std::uint32_t x100 : {6000u, 12000u, 24000u}) {
    VoicePool& p = pool_a();
    p.set_sample(0, desc(lfo(1, 17, 0.25f), 0.5f));  // the saw: a rise, then the wrap that drops the cutoff
    p.restart_clock();
    p.on_step(0);
    p.set_tempo_x100(x100);
    p.note_on(0, 60, 0, 127);
    const double wrap = static_cast<double>(kPeriod17) / (static_cast<double>(x100) * kTicksPerSamplePerX100);  // samples to the first wrap
    const std::uint32_t blocks = static_cast<std::uint32_t>(wrap / kBlk) + 3u;
    CHECK(blocks * kBlk < static_cast<std::uint32_t>(576.0 / (static_cast<double>(x100) * kTicksPerSamplePerX100)));  // inside the step: the clock is not held
    float prev = 0.0f;
    std::uint32_t at = 0;
    for (std::uint32_t b = 0; b < blocks; ++b) {
      run(p, 1);
      const float c = p.filter_cutoff(0);
      if (b != 0u && at == 0u && c < prev - 0.2f) {
        at = b;
      }
      prev = c;
    }
    CHECK(at != 0u);
    const double lo = static_cast<double>((at - 1u) * kBlk) + kEvalAt;
    const double hi = static_cast<double>(at * kBlk) + kEvalAt;
    CHECK_MESSAGE(lo < wrap, "tempo " << x100 << ": the wrap at " << wrap << " is after block " << at - 1u);
    CHECK_MESSAGE(wrap <= hi + 1.0, "tempo " << x100 << ": the wrap at " << wrap << " is before block " << at);
  }
}

TEST_CASE("filter mod: the LFO is locked to the transport, not to the note, and starts offset by 576 x (the step % 36) ticks") {
  VoicePool& a = pool_a();
  VoicePool& b = pool_b();
  const FilterMod m = lfo(2, 17, 0.25f);
  for (VoicePool* p : {&a, &b}) {
    p->set_sample(0, desc(m, 0.5f));
    p->restart_clock();
    p->on_step(0);
  }
  a.note_on(0, 60, 0, 127);
  run(a, 10);
  run(b, 10);
  b.note_on(0, 60, 0, 127);  // 640 samples after a's note, on the same transport
  for (std::uint32_t k = 0; k < 40; ++k) {
    run(a, 1);
    run(b, 1);
    CHECK_MESSAGE(a.filter_cutoff(0) == b.filter_cutoff(0), "block " << 10 + k << ": a note's own start does not move the LFO");
  }
  // the step the note starts in: 40 % 36 = 4 steps = 2304 ticks, 144 past a whole number of periods of 216
  VoicePool& p = pool_a();
  p.set_sample(0, desc(m, 0.5f));
  p.restart_clock();
  p.on_step(40);
  p.note_on(0, 60, 0, 127);
  std::uint32_t bad = 0;
  for (std::uint32_t k = 0; k < 75; ++k) {
    run(p, 1);
    bad += lfo_matches(p.filter_cutoff(0), 0.5, 2, 0.25, ticks_at(static_cast<double>(k * kBlk) + kEvalAt, 12000), kPeriod17, 4u * audio::kTicksPerStep) ? 0u : 1u;
  }
  CHECK_MESSAGE(bad == 0u, bad << " blocks off the oracle with the step shift");
}

TEST_CASE("filter mod: the Step events snap the tick count to the step grid (a step is 576 ticks, held at its end), so swing does not move the count") {
  VoicePool& p = pool_a();
  p.set_sample(0, desc(lfo(1, 14, 0.25f), 0.5f));  // one step a period: P = 576, so the saw is the position in the step
  p.restart_clock();
  p.on_step(0);
  p.set_tempo_x100(24000);  // a step is 2756 samples at 240 BPM: 43 blocks
  p.note_on(0, 60, 0, 127);
  run(p, 60);  // 3840 samples: the clock ran into the step's end at 2756 and was held at 576 ticks
  const double top = 0.5 + 0.25 * (2.0 * 575.0 / 576.0 - 1.0);
  CHECK_MESSAGE(std::fabs(static_cast<double>(p.filter_cutoff(0)) - top) < 3e-4, "held at the end of the step: " << p.filter_cutoff(0));
  p.on_step(1);  // the next step: tick 576 of the song, a whole period on: the phase is 0 again
  run(p, 1);
  CHECK_MESSAGE(std::fabs(static_cast<double>(p.filter_cutoff(0)) - (0.5 + 0.25 * (2.0 * ticks_at(kEvalAt, 24000) / 576.0 - 1.0))) < 3e-4, p.filter_cutoff(0));
}

TEST_CASE("filter mod: the base is the Cutoff FX's when the track has one, and a modulator needs the instrument's own filter on") {
  VoicePool& p = pool_a();
  const FilterMod m = lfo(3, 14, 0.2f);  // the square: +0.2 for the first half of the period, -0.2 for the second
  p.set_sample(0, desc(m, 0.5f));
  p.restart_clock();
  p.on_step(0);
  p.set_filter_fx(0, audio::kFilterLow, 80);  // value 80 = 0.8
  p.note_on(0, 60, 0, 127);
  run(p, 10);  // the first half
  CHECK(std::fabs(static_cast<double>(p.filter_cutoff(0)) - 1.0) < 1e-6);  // 0.8 + 0.2
  p.clear_filter_fx(0);
  run(p, 1);
  CHECK(std::fabs(static_cast<double>(p.filter_cutoff(0)) - 0.7) < 1e-6);  // back to the instrument's 0.5 + 0.2
  p.set_filter_fx(0, audio::kFilterLow, 30);
  run(p, 1);
  CHECK(std::fabs(static_cast<double>(p.filter_cutoff(0)) - 0.5) < 1e-6);  // 0.3 + 0.2: the FX's change reaches the next sample
  // the instrument's filter off: the FX turns the filter on and sets its cutoff, and nothing modulates it
  VoicePool& q = pool_b();
  q.set_sample(0, desc(m, 0.5f, false));
  q.restart_clock();
  q.on_step(0);
  q.set_filter_fx(0, audio::kFilterHigh, 80);
  q.note_on(0, 60, 0, 127);
  for (std::uint32_t b = 0; b < 30; ++b) {
    run(q, 1);
    CHECK(std::fabs(static_cast<double>(q.filter_cutoff(0)) - 0.8) < 1e-6);
  }
  // and with no filter at all there is no cutoff to report
  VoicePool& r = pool_b();
  r.set_sample(0, desc(m, 0.5f, false));
  r.note_on(0, 60, 0, 127);
  run(r, 1);
  CHECK(r.filter_cutoff(0) == -1.0f);
}

TEST_CASE("filter mod: what a voice makes does not depend on how the block is cut into spans, for the ADSR and the LFO") {
  static std::int16_t out_a[40 * kBlk];
  static std::int16_t out_b[40 * kBlk];
  const FilterMod mods[] = {adsr(0.7f, 0.3f, 2, 12, 2, 8, 5), lfo(2, 16, 0.4f), lfo(4, 17, 0.4f)};
  for (const FilterMod& m : mods) {
    VoicePool& a = pool_a();
    VoicePool& b = pool_b();
    for (VoicePool* p : {&a, &b}) {
      p->set_sample(0, desc(m, 0.45f));
      p->restart_clock();
      p->on_step(0);
      p->note_on(0, 64, 0, 127);
    }
    for (std::uint32_t blk = 0; blk < 40; ++blk) {
      a.begin_block();
      a.render(0, kBlk);
      a.end_block();
      b.begin_block();
      b.render(0, 17);
      b.render(17, 40);
      b.render(40, kBlk);
      b.end_block();
      std::memcpy(out_a + blk * kBlk, a.feed(0), kBlk * sizeof(std::int16_t));
      std::memcpy(out_b + blk * kBlk, b.feed(0), kBlk * sizeof(std::int16_t));
    }
    CHECK_MESSAGE(std::memcmp(out_a, out_b, sizeof out_a) == 0, "kind " << unsigned(m.kind) << " shape " << unsigned(m.shape));
  }
}

TEST_CASE("filter mod: the modulator is held to range where it is used: an unknown kind, shape or rate, an amount past +-1 or not a number") {
  VoicePool& p = pool_a();
  FilterMod m;
  m.kind = 7;
  m.shape = 9;
  m.rate = 200;
  m.amount = std::numeric_limits<float>::quiet_NaN();
  m.sustain = std::numeric_limits<float>::quiet_NaN();
  p.set_sample(0, desc(m, 0.5f));
  const FilterMod& s = p.sample(0).filter.mod;
  CHECK(s.kind == audio::kFmodNone);
  CHECK(s.shape == 2u);  // the sanitiser: an out-of-range shape becomes the triangle
  CHECK(s.rate == 28u);
  CHECK(s.amount == 0.0f);
  CHECK(s.sustain == 1.0f);
  m.amount = 5.0f;
  m.sustain = -3.0f;
  p.set_sample(1, desc(m, 0.5f));
  CHECK(p.sample(1).filter.mod.amount == 1.0f);
  CHECK(p.sample(1).filter.mod.sustain == 0.0f);
  m.amount = -5.0f;
  m.sustain = 3.0f;
  p.set_sample(2, desc(m, 0.5f));
  CHECK(p.sample(2).filter.mod.amount == -1.0f);
  CHECK(p.sample(2).filter.mod.sustain == 1.0f);
}

namespace {

std::uint32_t scripted_step(void*, std::uint64_t block_start, std::uint32_t, SeqEvent* out, std::uint32_t cap) noexcept {
  if (block_start != 0u || cap < 3u) {
    return 0;
  }
  SeqEvent reset{};
  reset.kind = EventKind::FxReset;
  reset.track = audio::kNoTrack;
  SeqEvent step{};
  step.kind = EventKind::Step;
  step.track = audio::kNoTrack;
  step.value = 4;
  SeqEvent on{};
  on.kind = EventKind::NoteOn;
  on.a = 60;
  on.b = 0;
  on.c = 127;
  out[0] = reset;
  out[1] = step;
  out[2] = on;
  return 3;
}

std::uint32_t tempo_240(void*) noexcept { return 24000u; }

std::uint32_t fake_cycles_counter = 0;
std::uint32_t fake_cycles() noexcept { return fake_cycles_counter += 7u; }

}  // namespace

TEST_CASE("filter mod: through the Engine, FxReset restarts the clock, a Step names the step the note starts in, and the event source's tempo sets the rate") {
  static o3os::test::StaticSlot<Engine> engine_slot;
  static audio::TapMem taps;
  static SeqEvent events[audio::kMaxEventsPerBlock];
  Engine& e = engine_slot.make(&fake_cycles, taps);
  VoicePool& p = pool_a();
  p.set_sample(0, desc(lfo(2, 17, 0.25f), 0.5f));
  e.attach_voices(&p);
  audio::EventSource src;
  src.step = &scripted_step;
  src.tempo = &tempo_240;
  src.buffer = events;
  src.capacity = audio::kMaxEventsPerBlock;
  e.attach_events(src);
  alignas(8) static std::int16_t in[haudio::kBlockSamples];
  alignas(8) static std::int16_t out[haudio::kBlockSamples];
  std::uint32_t bad = 0;
  for (std::uint32_t b = 0; b < 40; ++b) {  // 2560 samples at 240 BPM: 223 ticks, inside the step
    haudio::BlockReport report{};
    const haudio::BlockIo io{in, out, 0, &report};
    e.render_block(io);
    // the step is 4 (2304 ticks of shift = 144 past a period); the tempo is 240 BPM, not the 120 the pool assumes alone
    bad += lfo_matches(p.filter_cutoff(0), 0.5, 2, 0.25, ticks_at(static_cast<double>(b * kBlk) + kEvalAt, 24000), kPeriod17, 4u * audio::kTicksPerStep) ? 0u : 1u;
  }
  CHECK_MESSAGE(bad == 0u, bad << " blocks off the oracle through the engine");
}

// ---- OS-FU2: the Filter LFO FX (35) -------------------------------------------------------------------------------------------------------------------------------------

namespace {

// An instrument with no cutoff modulator of its own, whose lfo[2] has `shape` and `amount` (what the FX plays).
FilterMod no_mod(std::uint8_t shape, float lfo_amount) {
  FilterMod m;
  m.kind = audio::kFmodNone;
  m.shape = shape;
  m.lfo_amount = lfo_amount;
  return m;
}

constexpr std::uint32_t kPeriod20 = 108;  // rate index 20 is 0.1875 step: 576 x 0.1875 ticks

// Blocks of the voice that the LFO oracle does not match (period `period`, shift 0), `from` .. `to` blocks after the transport's start.
std::uint32_t lfo_misses(VoicePool& p, std::uint32_t from, std::uint32_t to, unsigned shape, double amount, std::uint32_t period) {
  std::uint32_t bad = 0;
  for (std::uint32_t b = from; b < to; ++b) {
    run(p, 1);
    bad += lfo_matches(p.filter_cutoff(0), 0.5, shape, amount, ticks_at(static_cast<double>(b * kBlk) + kEvalAt, 12000), period, 0) ? 0u : 1u;
  }
  return bad;
}

}  // namespace

TEST_CASE("filter lfo fx: an instrument with no modulator plays its lfo[2] shape and amount at the FX's rate, from the note, and is back at its base when the FX ends") {
  for (unsigned shape = 0; shape < 4; ++shape) {
    VoicePool& p = pool_a();
    p.set_sample(0, desc(no_mod(static_cast<std::uint8_t>(shape), 0.25f), 0.5f));
    p.restart_clock();
    p.on_step(0);
    p.set_filter_lfo(0, true, 17);
    p.note_on(0, 60, 0, 127);
    CHECK_MESSAGE(lfo_misses(p, 0, 70, shape, 0.25, kPeriod17) == 0u, "shape " << shape);
    p.set_filter_lfo(0, false, 0);  // the FX ends: nothing modulates the cutoff, the base is the instrument's
    run(p, 1);
    CHECK(std::fabs(static_cast<double>(p.filter_cutoff(0)) - 0.5) < 1e-6);
    run(p, 3);
    CHECK(std::fabs(static_cast<double>(p.filter_cutoff(0)) - 0.5) < 1e-6);
  }
}

TEST_CASE("filter lfo fx: it takes a voice that sounds, the LFO locked to the transport; an ADSR instrument plays the LFO instead and starts its ADSR over when the FX ends") {
  VoicePool& p = pool_a();
  FilterMod m = adsr(1.0f, 1.0f, 3000, 10, 10, 10, 10);  // a long delay: the ADSR holds the base for the whole test
  m.shape = 2;
  m.lfo_amount = 0.25f;
  p.set_sample(0, desc(m, 0.5f));
  p.restart_clock();
  p.on_step(0);
  p.note_on(0, 60, 0, 127);
  run(p, 5);
  CHECK(std::fabs(static_cast<double>(p.filter_cutoff(0)) - 0.5) < 1e-6);  // the ADSR's delay
  p.set_filter_lfo(0, true, 17);
  CHECK_MESSAGE(lfo_misses(p, 5, 70, 2, 0.25, kPeriod17) == 0u, "the LFO replaced the ADSR on the voice that sounds");
  p.set_filter_lfo(0, false, 0);
  run(p, 1);
  CHECK(std::fabs(static_cast<double>(p.filter_cutoff(0)) - 0.5) < 1e-6);  // the ADSR again, from its start: the delay holds the base
}

TEST_CASE("filter lfo fx: over an instrument's own LFO only the rate changes, and the phase stays the transport's") {
  VoicePool& p = pool_a();
  p.set_sample(0, desc(lfo(2, 17, 0.25f), 0.5f));
  p.restart_clock();
  p.on_step(0);
  p.note_on(0, 60, 0, 127);
  CHECK(lfo_misses(p, 0, 20, 2, 0.25, kPeriod17) == 0u);
  p.set_filter_lfo(0, true, 20);
  CHECK_MESSAGE(lfo_misses(p, 20, 60, 2, 0.25, kPeriod20) == 0u, "the FX's rate");
  p.set_filter_lfo(0, false, 0);
  CHECK_MESSAGE(lfo_misses(p, 60, 75, 2, 0.25, kPeriod17) == 0u, "the instrument's own rate again");
  // a rate past the table is the last one (the FX is held to 0..28 by the sequencer; the voices hold it again)
  p.set_filter_lfo(0, true, 200);
  run(p, 1);
  CHECK(std::isfinite(p.filter_cutoff(0)));
}

TEST_CASE("filter lfo fx: it needs the instrument's own filter on, and a note on another track is not touched; the transport's start ends it") {
  VoicePool& p = pool_a();
  p.set_sample(0, desc(no_mod(2, 0.25f), 0.5f, false));  // the filter is off
  p.restart_clock();
  p.on_step(0);
  p.set_filter_lfo(0, true, 17);
  p.note_on(0, 60, 0, 127);
  run(p, 3);
  CHECK(p.filter_cutoff(0) == -1.0f);  // nothing to modulate
  VoicePool& q = pool_b();
  q.set_sample(0, desc(no_mod(2, 0.25f), 0.5f));
  q.restart_clock();
  q.on_step(0);
  q.set_filter_lfo(1, true, 17);  // track 1 holds the FX
  q.note_on(0, 60, 0, 127);       // a note on track 0
  run(q, 5);
  CHECK(std::fabs(static_cast<double>(q.filter_cutoff(0)) - 0.5) < 1e-6);
  q.clear_filter_fxs();  // the transport starts (FxReset): the FX of every track ends
  q.note_on(1, 60, 0, 127);
  run(q, 5);
  CHECK(std::fabs(static_cast<double>(q.filter_cutoff(1)) - 0.5) < 1e-6);
}

namespace {

std::uint32_t lfo_fx_step(void*, std::uint64_t block_start, std::uint32_t, SeqEvent* out, std::uint32_t cap) noexcept {
  if (cap < 4u) {
    return 0;
  }
  if (block_start == 0u) {
    SeqEvent reset{};
    reset.kind = EventKind::FxReset;
    reset.track = audio::kNoTrack;
    SeqEvent step{};
    step.kind = EventKind::Step;
    step.track = audio::kNoTrack;
    step.value = 0;
    SeqEvent on{};
    on.kind = EventKind::NoteOn;
    on.a = 60;
    on.b = 0;
    on.c = 127;
    SeqEvent set{};
    set.kind = EventKind::ModSet;
    set.track = 0;
    set.a = 35;  // the step FX Filter LFO
    set.b = 17;  // its value: the rate index
    set.flags = audio::kModGroupLfoFilter;
    out[0] = reset;
    out[1] = step;
    out[2] = on;
    out[3] = set;
    return 4;
  }
  if (block_start == 20u * kBlk) {
    SeqEvent end{};
    end.kind = EventKind::ModEnd;
    end.track = 0;
    end.flags = audio::kModGroupLfoFilter;
    out[0] = end;
    return 1;
  }
  return 0;
}

}  // namespace

namespace {

// What the sequencer sends for the group (Sequencer::emit_changes): the FX alone (a == 35, c == kLayerFx1), the performance layer alone (c == kLayerPerf, a == 0, b == 0), both
// (c == kLayerPerf with the FX's type and value in a and b). g_lfo_perf_c / g_lfo_perf_a / g_lfo_perf_b are the ModSet the script sends at block 0 after the note.
std::uint8_t g_lfo_perf_a = 0;
std::uint8_t g_lfo_perf_b = 0;
std::uint8_t g_lfo_perf_c = 0;

std::uint32_t lfo_perf_step(void*, std::uint64_t block_start, std::uint32_t, SeqEvent* out, std::uint32_t cap) noexcept {
  if (cap < 4u || block_start != 0u) {
    return 0;
  }
  SeqEvent reset{};
  reset.kind = EventKind::FxReset;
  reset.track = audio::kNoTrack;
  SeqEvent step{};
  step.kind = EventKind::Step;
  step.track = audio::kNoTrack;
  SeqEvent on{};
  on.kind = EventKind::NoteOn;
  on.a = 60;
  on.c = 127;
  SeqEvent set{};
  set.kind = EventKind::ModSet;
  set.track = 0;
  set.a = g_lfo_perf_a;
  set.b = g_lfo_perf_b;
  set.c = g_lfo_perf_c;
  set.flags = audio::kModGroupLfoFilter;
  set.value = 5;  // the performance layer's own value (its FLT LFO slot's speed), ignored by the voices
  out[0] = reset;
  out[1] = step;
  out[2] = on;
  out[3] = set;
  return 4;
}

// Blocks through the engine with the script above; returns how many blocks left the cutoff off the oracle of an LFO at rate 17 (lfo) or off the flat 0.5 (!lfo).
std::uint32_t run_lfo_perf(bool lfo, std::uint32_t period = kPeriod17) {
  static o3os::test::StaticSlot<Engine> engine_slot;
  static audio::TapMem taps;
  static SeqEvent events[audio::kMaxEventsPerBlock];
  Engine& e = engine_slot.make(&fake_cycles, taps);
  VoicePool& p = pool_a();
  p.set_sample(0, desc(no_mod(2, 0.25f), 0.5f));
  e.attach_voices(&p);
  audio::EventSource src;
  src.step = &lfo_perf_step;
  src.buffer = events;
  src.capacity = audio::kMaxEventsPerBlock;
  e.attach_events(src);
  alignas(8) static std::int16_t in[haudio::kBlockSamples];
  alignas(8) static std::int16_t out[haudio::kBlockSamples];
  std::uint32_t bad = 0;
  for (std::uint32_t b = 0; b < 10; ++b) {
    haudio::BlockReport report{};
    const haudio::BlockIo io{in, out, 0, &report};
    e.render_block(io);
    if (lfo) {
      bad += lfo_matches(p.filter_cutoff(0), 0.5, 2, 0.25, ticks_at(static_cast<double>(b * kBlk) + kEvalAt, 12000), period, 0) ? 0u : 1u;
    } else {
      bad += std::fabs(static_cast<double>(p.filter_cutoff(0)) - 0.5) < 1e-6 ? 0u : 1u;
    }
  }
  return bad;
}

}  // namespace

TEST_CASE("filter lfo fx: the performance layer's FLT LFO row runs an LFO at index 14 + its offset (OS-PERF-LFO; before it the layer alone started none, OS-FU5); the FX alone keeps its own rate") {
  // The layer's value is 5 (the script above): the slot's stored triangle at index 14 + 5 = 19, on its own or over the FX (the layer's speed wins over the step FX's, spec 11.1).
  const std::uint32_t period19 = static_cast<std::uint32_t>(static_cast<float>(audio::kTicksPerStep) * audio::kLfoRate[19]);
  g_lfo_perf_a = 0;  // the layer alone: a == 0, b == 0, c == kLayerPerf
  g_lfo_perf_b = 0;
  g_lfo_perf_c = audio::kLayerPerf;
  CHECK(run_lfo_perf(true, period19) == 0u);   // the cutoff is that LFO
  CHECK(run_lfo_perf(false) == 10u);           // and no longer the instrument's flat cutoff
  g_lfo_perf_a = audio::kFxFilterLfo;  // the FX underneath the layer: the layer's speed wins
  g_lfo_perf_b = 17;
  CHECK(run_lfo_perf(true, period19) == 0u);
  g_lfo_perf_c = audio::kLayerFx1;  // the FX alone: the FX's rate
  CHECK(run_lfo_perf(true) == 0u);
}

TEST_CASE("filter lfo fx: through the Engine a ModSet of the LfoFilter group starts the LFO at the rate in b and a ModEnd takes it away") {
  static o3os::test::StaticSlot<Engine> engine_slot;
  static audio::TapMem taps;
  static SeqEvent events[audio::kMaxEventsPerBlock];
  Engine& e = engine_slot.make(&fake_cycles, taps);
  VoicePool& p = pool_a();
  p.set_sample(0, desc(no_mod(2, 0.25f), 0.5f));
  e.attach_voices(&p);
  audio::EventSource src;
  src.step = &lfo_fx_step;
  src.buffer = events;
  src.capacity = audio::kMaxEventsPerBlock;
  e.attach_events(src);
  alignas(8) static std::int16_t in[haudio::kBlockSamples];
  alignas(8) static std::int16_t out[haudio::kBlockSamples];
  std::uint32_t bad = 0;
  for (std::uint32_t b = 0; b < 30; ++b) {
    haudio::BlockReport report{};
    const haudio::BlockIo io{in, out, 0, &report};
    e.render_block(io);
    if (b < 20u) {
      bad += lfo_matches(p.filter_cutoff(0), 0.5, 2, 0.25, ticks_at(static_cast<double>(b * kBlk) + kEvalAt, 12000), kPeriod17, 0) ? 0u : 1u;
    } else if (b >= 21u) {
      CHECK(std::fabs(static_cast<double>(p.filter_cutoff(0)) - 0.5) < 1e-6);
    }
  }
  CHECK_MESSAGE(bad == 0u, bad << " blocks off the oracle through the engine");
}
