// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-MOD: the instrument's pan (autopan), finetune (vibrato) and amp (tremolo) modulators, and the live loop points (audio/voices.h PAN, FINETUNE, AMP LFO MODULATION and LIVE LOOP).
// As for the cutoff's (audio_filter_mod_test.cpp) the oracles are written here, in double, from this OS's own model of the modulators: the ADSR in milliseconds, the LFO the transport's
// tick count over a period of (576 x rate) ticks, the pan and the fine tune the modulator's output times 100 cut to a whole number toward zero and added to the instrument's value, the tremolo the LFO's level A x shape(phase) as a gain (the amp slot's looped envelope). Read through the pool's own
// outputs: the pan the pool hands the Engine (pan_mod_value), the pitch (the slope of a ramp sample), the voice's samples (a constant sample under the tremolo). One render test of each
// target, the same through the Engine (the strip's pan equals a static pan of the same value), the block cut into spans not changing a sample, live enabling and disabling, the
// descriptor's sanitising, and the live loop: a loop point moved while the voice plays reaches it in the next block (apply_param and the Engine's ring), held to the sample and 2 samples
// apart, a position past the new end wrapped into the loop (this OS's rule). Host and Cortex-M4F.
#include <doctest/doctest.h>

#include <o3os/audio/engine.h>
#include <o3os/audio/event_source.h>
#include <o3os/audio/voices.h>
#include <o3os/core/param.h>
#include <o3os/hal/audio.h>

#include "support/static_slot.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace {

namespace audio = o3os::audio;
namespace haudio = o3os::hal::audio;
namespace ip = audio::inst_param;
using audio::Engine;
using audio::EventKind;
using audio::FilterMod;
using audio::SampleDesc;
using audio::SeqEvent;
using audio::VoicePool;
using o3os::core::ParamChange;
using o3os::core::ParamKind;

constexpr std::uint32_t kBlk = haudio::kBlock;
constexpr double kSamplesPerMs = static_cast<double>(haudio::sample_rate_mhz()) * 1e-6;
constexpr double kTicksPerSamplePerX100 = 384.0 / static_cast<double>(haudio::sample_rate_mhz());
constexpr std::uint32_t kLen = 6000;
constexpr std::uint32_t kPeriod17 = 216;  // rate index 17 of the cutoff table (0.375 step): 576 x 0.375 ticks; the amp table's index 12
constexpr std::uint32_t kPeriod18 = 191;  // index 18: a third of a step, truncated (voices.h FILTER MODULATION)

std::uint32_t fake_cycles_counter = 0;
std::uint32_t fake_cycles() noexcept { return fake_cycles_counter += 7u; }

VoicePool& pool_a() {
  static o3os::test::StaticSlot<VoicePool> slot;
  return slot.make();
}
VoicePool& pool_b() {
  static o3os::test::StaticSlot<VoicePool> slot;
  return slot.make();
}

std::int16_t g_dc[kLen];    // a constant: what the tremolo does to it is the multiplier itself
std::int16_t g_ramp[kLen];  // 5 a frame: the output is the position, so its slope is the pitch increment

void init_pcm() {
  for (std::uint32_t i = 0; i < kLen; ++i) {
    g_dc[i] = 16000;
    g_ramp[i] = static_cast<std::int16_t>(5u * i);
  }
}

// A sample that loops over its whole length (the note lasts), unity gain, no amp envelope; the modulators are set by the caller.
SampleDesc desc(const std::int16_t* pcm = g_dc, bool loop = true) {
  init_pcm();
  SampleDesc d{pcm, kLen, 0, loop ? kLen : 0u, 65536};
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

void clock(VoicePool& p) {
  p.restart_clock();
  p.on_step(0);
}

// One block of the pool (a single span), the voice's own block returned (track 0) so that its samples can be read.
const std::int16_t* run1(VoicePool& p) {
  p.begin_block();
  p.render(0, kBlk);
  p.end_block();
  return p.feed(0);
}

// ---- Oracles ------------------------------------------------------------------------------------------------------------------------------------------------------------
struct Adsr {
  double amount, sustain, delay, attack, hold, decay, release;  // ms
};

// The held level at sample `t` after the note on (the ADSR a note with kill 0 has).
double held_level(const Adsr& a, double t) {
  const double d = a.delay * kSamplesPerMs;
  const double at = a.attack * kSamplesPerMs;
  const double h = a.hold * kSamplesPerMs;
  const double dc = a.decay * kSamplesPerMs;
  if (t < d) {
    return 0.0;
  }
  if (t < d + at) {
    const double v = ((t - d) / at) * a.amount;
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
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

// The LFO's output, A x shape(u) (-A..A), and its level (0..A) for the tremolo; u the phase in the period.
double lfo_out(unsigned shape, double u, double a) {
  switch (shape) {
    case 0: return a * (1.0 - 2.0 * u);
    case 1: return a * (2.0 * u - 1.0);
    case 2: return u < 0.5 ? a * (4.0 * u - 1.0) : a * (3.0 - 4.0 * u);
    default: return u < 0.5 ? a : -a;
  }
}
double lfo_level(unsigned shape, double u, double a) { return (lfo_out(shape, u, a) + a) * 0.5; }

double ticks_at(double samples, std::uint32_t x100) { return samples * static_cast<double>(x100) * kTicksPerSamplePerX100; }

double phase_at(std::int64_t x, std::uint32_t period) { return static_cast<double>(((x % period) + period) % period) / static_cast<double>(period); }

// True when `ok(u)` holds at the tick count `ticks` or one tick either side (the pool's count is an integer that may sit across the floor of a product).
template <typename F>
bool near_phase(double ticks, std::uint32_t period, F ok) {
  const std::int64_t x0 = static_cast<std::int64_t>(std::floor(ticks + 1e-9));
  for (int d = -1; d <= 1; ++d) {
    if (ok(phase_at(x0 + d, period))) {
      return true;
    }
  }
  return false;
}

int trunc100(double v) { return static_cast<int>(v * 100.0); }
int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// The slope of a ramp voice's block: the increment a sample, from the first and last sample of the block (5 units a frame).
double slope(const std::int16_t* b) { return static_cast<double>(b[kBlk - 1] - b[0]) / (5.0 * static_cast<double>(kBlk - 1)); }
double inc_of(int fine) { return static_cast<double>(audio::tuned_inc_q32(60, 0, fine)) / 4294967296.0; }

}  // namespace

// ---- PAN ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
TEST_CASE("pan mod: the ADSR's output times 100, cut toward zero, is added to the instrument's pan and held to 0..100, once a block, from the block's first sample") {
  const Adsr a{0.8, 0.5, 2, 20, 3, 15, 10};
  VoicePool& p = pool_a();
  SampleDesc d = desc();
  d.pan = 50;
  d.env.enable = true;  // an amp envelope with a long release, so that the NoteOff does not end the voice and the modulator's own release is heard
  d.env.release = 60000;
  d.pan_mod = adsr(0.8f, 0.5f, 2, 20, 3, 15, 10);
  p.set_sample(0, d);
  p.note_on(0, 60, 0, 127);
  std::uint32_t worst = 0;
  for (std::uint32_t b = 0; b < 70; ++b) {
    if (b == 40) {
      p.note_off(0);
    }
    run1(p);
    const std::uint32_t mask = p.take_pan_mods();
    CHECK_MESSAGE((mask & 1u) != 0u, "block " << b << ": the voice's pan was not handed over");
    double level;
    if (b < 40) {
      level = held_level(a, static_cast<double>(b * kBlk));
    } else {  // the release starts at the first evaluation after the NoteOff, from the sustain, and runs its 10 ms
      const double v = (1.0 - static_cast<double>((b - 40u) * kBlk) / (a.release * kSamplesPerMs)) * (a.sustain * a.amount);
      level = v < 0.0 ? 0.0 : v;
    }
    const int want = clampi(50 + trunc100(level), 0, 100);
    const std::uint32_t err = static_cast<std::uint32_t>(std::abs(static_cast<int>(p.pan_mod_value(0)) - want));
    worst = err > worst ? err : worst;
  }
  CHECK_MESSAGE(worst <= 1u, "worst difference " << worst << " units of pan");
  CHECK(p.pan_mod_value(0) == 50);  // the release has run out: the instrument's pan again
}

TEST_CASE("pan mod: a negative amount pulls the pan under the instrument's, and the sum is held to 0..100") {
  VoicePool& p = pool_a();
  SampleDesc d = desc();
  d.pan = 20;
  d.pan_mod = adsr(-0.5f, 1.0f, 0, 1, 1, 1, 10);
  p.set_sample(0, d);
  p.note_on(0, 60, 0, 127);
  for (int b = 0; b < 12; ++b) {
    run1(p);
  }
  CHECK(p.pan_mod_value(0) == 0);  // 20 - 50, held
  VoicePool& q = pool_b();
  d.pan = 90;
  d.pan_mod = adsr(1.0f, 1.0f, 0, 1, 1, 1, 10);
  q.set_sample(0, d);
  q.note_on(0, 60, 0, 127);
  for (int b = 0; b < 12; ++b) {
    run1(q);
  }
  CHECK(q.pan_mod_value(0) == 100);  // 90 + 100, held
}

TEST_CASE("pan mod: the LFO (A = 0.5, 0.375 step) is the transport's phase at each block's start, saw and triangle around the instrument's pan") {
  for (const unsigned shape : {1u, 2u, 3u}) {
    VoicePool& p = pool_a();
    SampleDesc d = desc();
    d.pan = 40;
    d.pan_mod = lfo(static_cast<std::uint8_t>(shape), 17, 0.5f);
    p.set_sample(0, d);
    clock(p);
    p.note_on(0, 60, 0, 127);
    std::uint32_t bad = 0;
    for (std::uint32_t b = 0; b < 75; ++b) {
      run1(p);
      p.take_pan_mods();
      const int got = p.pan_mod_value(0);
      bad += near_phase(ticks_at(static_cast<double>(b * kBlk), 12000), kPeriod17, [&](double u) {
               return std::abs(got - clampi(40 + trunc100(lfo_out(shape, u, 0.5)), 0, 100)) <= 1;
             })
                 ? 0u
                 : 1u;
    }
    CHECK_MESSAGE(bad == 0u, "shape " << shape << ": " << bad << " blocks off the oracle");
  }
}

TEST_CASE("pan mod: a voice with no pan modulator hands the Engine no pan, and a cut clears the one it had evaluated in the block") {
  VoicePool& p = pool_a();
  p.set_sample(0, desc());
  p.note_on(0, 60, 0, 127);
  run1(p);
  CHECK(p.take_pan_mods() == 0u);
  SampleDesc d = desc();
  d.pan_mod = lfo(1, 17, 0.5f);
  p.set_sample(1, d);
  p.note_on(1, 60, 1, 127);
  p.begin_block();
  p.render(0, 16);
  p.cut(1);  // a note of another engine takes the track (the Engine cuts the sampler's voice): its pan is that note's
  p.render(16, kBlk);
  p.end_block();
  CHECK(p.take_pan_mods() == 0u);
  // a note without the modulator that replaces one that has it, later in the block: the pan the old note evaluated is not the track's
  p.set_sample(2, desc());
  p.note_on(1, 60, 1, 127);
  p.begin_block();
  p.render(0, 16);
  p.note_on(1, 60, 2, 127);
  p.render(16, kBlk);
  p.end_block();
  CHECK(p.take_pan_mods() == 0u);
}

TEST_CASE("pan mod: the Engine gives the strip the evaluated pan, so the output equals that of the instrument with that pan and no modulator") {
  static o3os::test::StaticSlot<Engine> slot_a;
  static o3os::test::StaticSlot<Engine> slot_b;
  static o3os::test::StaticSlot<VoicePool> pool_slot_b;
  static audio::TapMem taps_a;
  static audio::TapMem taps_b;
  static SeqEvent events[audio::kMaxEventsPerBlock];
  struct Src {
    static std::uint32_t step(void*, std::uint64_t block_start, std::uint32_t, SeqEvent* out, std::uint32_t cap) noexcept {
      if (block_start != 0u || cap < 1u) {
        return 0;
      }
      SeqEvent e{};
      e.kind = EventKind::NoteOn;
      e.track = 0;
      e.a = 60;
      e.b = 0;
      e.c = 127;
      out[0] = e;
      return 1;
    }
  };
  VoicePool& pa = pool_a();
  VoicePool& pb = pool_slot_b.make();
  SampleDesc d = desc();
  d.pan = 50;
  d.pan_mod = adsr(0.5f, 1.0f, 0, 1, 1, 1, 10);  // +50 from the second block on: pan 100
  pa.set_sample(0, d);
  d.pan_mod = FilterMod{};
  d.pan = 100;
  pb.set_sample(0, d);
  Engine& ea = slot_a.make(&fake_cycles, taps_a);
  Engine& eb = slot_b.make(&fake_cycles, taps_b);
  audio::EventSource src;
  src.step = &Src::step;
  src.buffer = events;
  src.capacity = audio::kMaxEventsPerBlock;
  ea.attach_voices(&pa);
  ea.attach_events(src);
  eb.attach_voices(&pb);
  eb.attach_events(src);
  alignas(8) static std::int16_t in[haudio::kBlockSamples];
  alignas(8) static std::int16_t oa[haudio::kBlockSamples];
  alignas(8) static std::int16_t ob[haudio::kBlockSamples];
  std::uint32_t same = 0;
  std::int32_t left = 0;
  std::int32_t right = 0;
  for (std::uint32_t b = 0; b < 12; ++b) {
    haudio::BlockReport ra{};
    haudio::BlockReport rb{};
    ea.render_block(haudio::BlockIo{in, oa, 0, &ra});
    eb.render_block(haudio::BlockIo{in, ob, 0, &rb});
    if (b >= 4u) {
      same += std::memcmp(oa, ob, sizeof oa) == 0 ? 1u : 0u;
      for (std::uint32_t i = 0; i < kBlk; ++i) {
        left = oa[2u * i] > left ? oa[2u * i] : left;
        right = oa[2u * i + 1u] > right ? oa[2u * i + 1u] : right;
      }
    }
  }
  CHECK(same == 8u);
  CHECK_MESSAGE(left != right, "the pan is not at the centre: " << left << " / " << right);  // (the strip's pan law moved it)
}

// ---- FINETUNE ----------------------------------------------------------------------------------------------------------------------------------------------------------------
TEST_CASE("finetune mod: the ADSR moves the pitch up to a semitone (100 of fine tune) at amount 1: the increment follows the linear law, per block") {
  const Adsr a{1.0, 1.0, 0, 20, 0, 0, 10};
  VoicePool& p = pool_a();
  SampleDesc d = desc(g_ramp, false);
  d.tune_mod = adsr(1.0f, 1.0f, 0, 20, 0, 0, 10);
  p.set_sample(0, d);
  p.note_on(0, 60, 0, 127);
  double worst = 0.0;
  double pos = 0.0;  // the position the oracle's increments have walked to, at the start of each block
  double worst_pos = 0.0;
  for (std::uint32_t b = 0; b < 40; ++b) {
    const std::int16_t* blk = run1(p);
    const int fine = clampi(trunc100(held_level(a, static_cast<double>(b * kBlk))), -100, 100);
    if (b >= 2u) {  // (the voice's own attack ramp is in the first samples)
      const double err = std::fabs(slope(blk) - inc_of(fine));
      worst = err > worst ? err : worst;
      const double perr = std::fabs(static_cast<double>(blk[0]) / 5.0 - pos);  // the ramp's value is 5 x its position
      worst_pos = perr > worst_pos ? perr : worst_pos;
    }
    pos += static_cast<double>(kBlk) * inc_of(fine);
  }
  CHECK_MESSAGE(worst < 0.01, "worst difference " << worst << " of the increment");
  CHECK_MESSAGE(worst_pos < 0.6, "the position drifted from the oracle's by " << worst_pos << " samples");  // a 10% error of the range is 10 samples by the end
  CHECK(std::fabs(inc_of(100) - std::exp2(1.0 / 12.0)) < 1e-4);  // the top of the range is the next semitone
}

TEST_CASE("finetune mod: the LFO (triangle, A = 0.4, a third of a step) is added to the instrument's fine tune and held to -100..100") {
  VoicePool& p = pool_a();
  SampleDesc d = desc(g_ramp, false);
  d.fine_tune = -30;
  d.tune_mod = lfo(2, 18, 0.4f);
  p.set_sample(0, d);
  clock(p);
  p.note_on(0, 60, 0, 127);
  std::uint32_t bad = 0;
  double top = -1e9;
  double bottom = 1e9;
  double pos = 0.0;
  double worst_pos = 0.0;
  for (std::uint32_t b = 0; b < 70; ++b) {
    const std::int16_t* blk = run1(p);
    const std::int64_t x = static_cast<std::int64_t>(std::floor(ticks_at(static_cast<double>(b * kBlk), 12000) + 1e-9));
    const double inc = inc_of(clampi(-30 + trunc100(lfo_out(2, phase_at(x, kPeriod18), 0.4)), -100, 100));
    if (b >= 2u) {
      const double perr = std::fabs(static_cast<double>(blk[0]) / 5.0 - pos);
      worst_pos = perr > worst_pos ? perr : worst_pos;
    }
    pos += static_cast<double>(kBlk) * inc;
    if (b < 2u) {
      continue;
    }
    const double got = slope(blk);
    top = got > top ? got : top;
    bottom = got < bottom ? got : bottom;
    bad += near_phase(ticks_at(static_cast<double>(b * kBlk), 12000), kPeriod18, [&](double u) {
             return std::fabs(got - inc_of(clampi(-30 + trunc100(lfo_out(2, u, 0.4)), -100, 100))) < 0.01;
           })
               ? 0u
               : 1u;
  }
  CHECK_MESSAGE(bad == 0u, bad << " blocks off the oracle");
  CHECK_MESSAGE(worst_pos < 1.5, "the position drifted from the oracle's by " << worst_pos << " samples");  // (a tick either side of a block's boundary moves a block's increment by under a unit of fine tune)
  CHECK(top - bottom > 0.02);  // it does swing: from -70 to +10 of fine tune is 80 steps of 0.0006
}

// ---- AMP LFO (tremolo) -------------------------------------------------------------------------------------------------------------------------------------------------------
TEST_CASE("amp lfo: the voice's gain is A x the LFO's shape, the transport's phase, re-evaluated every 16 samples and walked linearly between (rate index 12 of the amp table is 0.375 step)") {
  for (unsigned shape = 0; shape < 4; ++shape) {
    VoicePool& p = pool_a();
    SampleDesc d = desc();
    d.amp_mod = lfo(static_cast<std::uint8_t>(shape), 12, 0.8f);
    p.set_sample(0, d);
    clock(p);
    p.note_on(0, 60, 0, 127);
    std::uint32_t bad = 0;
    for (std::uint32_t b = 0; b < 75; ++b) {
      const std::int16_t* blk = run1(p);
      if (b == 0u) {
        continue;  // the attack ramp
      }
      for (std::uint32_t k = 0; k < kBlk / 16u; ++k) {
        const int got = blk[16u * k + 15u];  // the end of a segment: the next grid point's level, reached
        bad += near_phase(ticks_at(static_cast<double>(b * kBlk + 16u * k), 12000), kPeriod17, [&](double u) {
                 return std::fabs(static_cast<double>(got) - 16000.0 * lfo_level(shape, u, 0.8)) <= 3.0;
               })
                   ? 0u
                   : 1u;
      }
    }
    CHECK_MESSAGE(bad == 0u, "shape " << shape << ": " << bad << " segments off the oracle");
  }
}

TEST_CASE("amp lfo: the multiplier never exceeds its amount, a negative amount is silence, and a random level holds for a period") {
  VoicePool& p = pool_a();
  SampleDesc d = desc();
  d.amp_mod = lfo(4, 12, 0.5f);
  p.set_sample(0, d);
  clock(p);
  p.note_on(0, 60, 0, 127);
  unsigned jumps = 0;
  std::int32_t last = -1;
  for (std::uint32_t b = 0; b < 75; ++b) {
    const std::int16_t* blk = run1(p);
    if (b == 0u) {
      continue;
    }
    for (std::uint32_t k = 0; k < kBlk; ++k) {
      CHECK(blk[k] >= 0);
      CHECK(blk[k] <= 8000 + 2);
    }
    jumps += (last >= 0 && std::abs(blk[kBlk - 1u] - last) > 8) ? 1u : 0u;  // a new level: the first sample of a period's grid point
    last = blk[kBlk - 1u];
  }
  CHECK(jumps >= 1u);  // 501 ticks are 2.3 periods: two new levels...
  CHECK(jumps <= 6u);  // ...each one a ramp over 16 samples, so seen in one block (or two), not in every block
  VoicePool& q = pool_b();
  d.amp_mod = lfo(1, 12, -0.5f);
  q.set_sample(0, d);
  clock(q);
  q.note_on(0, 60, 0, 127);
  for (std::uint32_t b = 0; b < 10; ++b) {
    const std::int16_t* blk = run1(q);
    for (std::uint32_t k = 0; k < kBlk; ++k) {
      CHECK(blk[k] == 0);
    }
  }
}

TEST_CASE("amp lfo: the rate indexes the amp table (24 rates, 1/4 of a step is index 13) and is held to 23; the plain amp slot with no LFO is untouched") {
  VoicePool& p = pool_a();
  SampleDesc d = desc();
  d.amp_mod = lfo(1, 200, 1.0f);
  d.amp_mod.kind = audio::kFmodEnv;  // the amp slot's ADSR is `env`: here it is not a modulator
  d.pan_mod.rate = 200;
  p.set_sample(0, d);
  CHECK(p.sample(0).amp_mod.kind == audio::kFmodNone);
  CHECK(p.sample(0).pan_mod.rate == 28);
  d.amp_mod.kind = audio::kFmodLfo;
  p.set_sample(0, d);
  CHECK(p.sample(0).amp_mod.rate == 23);
  CHECK(audio::kLfoRate[audio::kAmpRateBase] == 24.0f);                // the amp table starts at 24 steps (the first amp LFO rate)
  CHECK(audio::kLfoRate[audio::kAmpRateBase + 23] == 0.015625f);       // and ends at 1/64 step, where the other table does
  // no modulator: the samples are the plain voice's, bit for bit
  VoicePool& a = pool_a();
  VoicePool& b = pool_b();
  a.set_sample(0, desc());
  b.set_sample(0, desc());
  a.note_on(0, 60, 0, 127);
  b.note_on(0, 60, 0, 127);
  for (int i = 0; i < 5; ++i) {
    const std::int16_t* x = run1(a);
    const std::int16_t* y = run1(b);
    CHECK(std::memcmp(x, y, kBlk * sizeof(std::int16_t)) == 0);
  }
  CHECK(a.feed(0)[10] == 16000);
}

// ---- Span independence ----------------------------------------------------------------------------------------------------------------------------------------------------------
TEST_CASE("mods: the output does not depend on how events split the block (pan, finetune and tremolo together)") {
  VoicePool& a = pool_a();
  VoicePool& b = pool_b();
  SampleDesc d = desc(g_ramp, false);
  d.pan_mod = lfo(2, 17, 0.5f);
  d.tune_mod = adsr(0.6f, 0.5f, 1, 15, 2, 10, 10);
  d.amp_mod = lfo(2, 12, 0.9f);
  for (VoicePool* p : {&a, &b}) {
    p->set_sample(0, d);
    clock(*p);
    p->note_on(0, 60, 0, 127);
  }
  for (std::uint32_t blk = 0; blk < 40; ++blk) {
    a.begin_block();
    a.render(0, kBlk);
    a.end_block();
    b.begin_block();
    b.render(0, 5);
    b.render(5, 21);
    b.render(21, 22);
    b.render(22, 47);
    b.render(47, kBlk);
    b.end_block();
    CHECK_MESSAGE(std::memcmp(a.feed(0), b.feed(0), kBlk * sizeof(std::int16_t)) == 0, "block " << blk);
    CHECK(a.take_pan_mods() == b.take_pan_mods());
    CHECK(a.pan_mod_value(0) == b.pan_mod_value(0));
  }
}

TEST_CASE("mods: a note that starts mid-block evaluates at once and counts its ADSR from that sample; a retrigger's ADSR starts from the old level") {
  VoicePool& p = pool_a();
  SampleDesc d = desc();
  d.pan_mod = adsr(0.5f, 1.0f, 3, 1, 1, 1, 10);  // delay 3 ms, attack 1 ms, then +50
  p.set_sample(0, d);
  p.begin_block();
  p.render(0, 30);
  p.note_on(0, 60, 0, 127);  // sample 30
  p.render(30, kBlk);
  p.end_block();
  CHECK((p.take_pan_mods() & 1u) != 0u);
  CHECK(p.pan_mod_value(0) == 50);  // the delay holds zero
  for (int b = 1; b < 4; ++b) {
    run1(p);
  }
  // the third evaluation after the note is at 34 + 2 x 64 = 162 samples: 29.65 samples into the 44.1-sample attack
  const double level = 0.5 * (162.0 - 3.0 * kSamplesPerMs) / (1.0 * kSamplesPerMs);
  CHECK_MESSAGE(std::abs(static_cast<int>(p.pan_mod_value(0)) - (50 + trunc100(level))) <= 1, "pan " << static_cast<int>(p.pan_mod_value(0)));
  for (int b = 0; b < 6; ++b) {
    run1(p);
  }
  CHECK(p.pan_mod_value(0) == 100);  // the sustain: 50 + 100 x 0.5 x 1
  p.note_on(0, 62, 0, 127);          // the retrigger: the delay holds the old level
  run1(p);
  CHECK(p.pan_mod_value(0) == 100);
  VoicePool& q = pool_b();
  q.set_sample(0, d);
  q.note_on(0, 62, 0, 127);
  run1(q);
  CHECK(q.pan_mod_value(0) == 50);  // a fresh note's holds zero
}

// ---- Live (OS-LIVEP) --------------------------------------------------------------------------------------------------------------------------------------------------------
TEST_CASE("mods live: enabling and disabling a modulator on the slot a voice plays starts and ends it, the pan and the pitch back at the instrument's") {
  VoicePool& p = pool_a();
  SampleDesc d = desc(g_ramp, false);
  d.pan = 30;
  p.set_sample(0, d);
  p.note_on(0, 60, 0, 127);
  run1(p);
  run1(p);
  CHECK(p.take_pan_mods() == 0u);
  std::uint32_t pan_tracks = 0;
  // finetune: ADSR amount 1, sustain 1, no stages: +100 at once
  const double base = slope(run1(p));
  CHECK(p.apply_param(0, ip::envelope(5, ip::EnvEnable), 1.0f, pan_tracks));
  const double mod = slope(run1(p));  // evaluated at the next block's first sample: the edit's next block has it
  CHECK(std::fabs(base - 1.0) < 0.01);
  CHECK(std::fabs(mod - std::exp2(1.0 / 12.0)) < 0.01);
  CHECK(p.apply_param(0, ip::envelope(5, ip::EnvEnable), 0.0f, pan_tracks));
  run1(p);
  CHECK(std::fabs(slope(run1(p)) - 1.0) < 0.01);
  // pan
  CHECK(p.apply_param(0, ip::envelope(1, ip::EnvEnable), 1.0f, pan_tracks));
  run1(p);
  CHECK((p.take_pan_mods() & 1u) != 0u);
  CHECK(p.pan_mod_value(0) == 100);  // 30 + 100, held
  pan_tracks = 0;
  CHECK(p.apply_param(0, ip::envelope(1, ip::EnvEnable), 0.0f, pan_tracks));
  CHECK(pan_tracks == 1u);  // the caller gives the track the instrument's pan again
  run1(p);
  CHECK(p.take_pan_mods() == 0u);
  // the instrument's pan edit while a modulator runs is the modulator's base, not a pan to set
  CHECK(p.apply_param(0, ip::envelope(1, ip::EnvEnable), 1.0f, pan_tracks));
  run1(p);
  pan_tracks = 0;
  CHECK(p.apply_param(0, ip::Panning, 10.0f, pan_tracks));
  CHECK(pan_tracks == 0u);
  run1(p);
  CHECK(p.pan_mod_value(0) == 100);  // 10 + 100, held
  CHECK(p.apply_param(0, ip::Panning, 0.0f, pan_tracks));
}

TEST_CASE("mods live: switching the amp LFO off ramps the multiplier back to unity and ends the pass; switching it on ramps from unity") {
  VoicePool& p = pool_a();
  SampleDesc d = desc();
  d.env.lfo = true;  // the stored amp slot is an LFO (loop byte): it is the tremolo
  d.amp_mod = lfo(3, 12, 0.5f);  // square: 0.5 for half the period, then silence
  p.set_sample(0, d);
  clock(p);
  p.note_on(0, 60, 0, 127);
  for (int b = 0; b < 5; ++b) {
    run1(p);
  }
  std::uint32_t pan_tracks = 0;
  CHECK(p.apply_param(0, ip::envelope(0, ip::EnvEnable), 0.0f, pan_tracks));
  run1(p);
  const std::int16_t* blk = run1(p);
  for (std::uint32_t k = 0; k < kBlk; ++k) {
    CHECK(blk[k] == 16000);
  }
  CHECK(p.apply_param(0, ip::envelope(0, ip::EnvEnable), 1.0f, pan_tracks));  // back on: a ramp, never a step
  std::int32_t biggest = 0;
  std::int32_t last = 16000;
  for (int b = 0; b < 12; ++b) {
    blk = run1(p);
    for (std::uint32_t k = 0; k < kBlk; ++k) {
      const std::int32_t step = std::abs(blk[k] - last);
      biggest = step > biggest ? step : biggest;
      last = blk[k];
    }
  }
  CHECK_MESSAGE(biggest < 600, "the biggest step of a sample was " << biggest);  // a ramp of 8000 over 16 samples is 500 a sample at the most
}

// ---- LIVE LOOP ---------------------------------------------------------------------------------------------------------------------------------------------------------------
namespace {

SampleDesc loop_desc(std::uint8_t mode, std::uint32_t ls = 1000, std::uint32_t le = 3000) {
  init_pcm();
  SampleDesc d{g_ramp, kLen, ls, le, 65536};
  d.loop_mode = mode;
  return d;
}

// The range of the voice's samples over `n` blocks (the pool's block of track 0).
void range_over(VoicePool& p, unsigned n, int& lo, int& hi) {
  lo = 99999;
  hi = -99999;
  for (unsigned b = 0; b < n; ++b) {
    const std::int16_t* blk = run1(p);
    for (std::uint32_t k = 0; k < kBlk; ++k) {
      lo = blk[k] < lo ? blk[k] : lo;
      hi = blk[k] > hi ? blk[k] : hi;
    }
  }
}

}  // namespace

TEST_CASE("live loop: a forward voice takes the moved loop end in the next block: a position past it wraps into the loop") {
  VoicePool& p = pool_a();
  p.set_sample(0, loop_desc(0));
  p.note_on(0, 60, 0, 127);
  for (int b = 0; b < 40; ++b) {
    run1(p);  // 2560 samples: the position is 2560 of the loop 1000..3000
  }
  std::uint32_t pan_tracks = 0;
  CHECK(p.apply_param(0, ip::LoopEnd, 2000.0f, pan_tracks));
  CHECK(p.sample(0).loop_end == 2000u);
  const std::int16_t* blk = run1(p);
  CHECK_MESSAGE(std::abs(blk[0] - 5 * (1000 + (2560 - 1000) % 1000)) <= 6, "first sample " << blk[0]);  // the wrap: loop_start + (position - loop_end) over a loop of 1000
  int lo, hi;
  range_over(p, 60, lo, hi);
  CHECK(hi <= 5 * 2000 + 6);
  CHECK(lo >= 5 * 1000 - 6);
  CHECK(p.apply_param(0, ip::LoopStart, 1800.0f, pan_tracks));
  range_over(p, 20, lo, hi);  // the voice reaches the end, wraps to the new start
  range_over(p, 60, lo, hi);
  CHECK(lo >= 5 * 1800 - 6);
  CHECK(hi <= 5 * 2000 + 6);
}

TEST_CASE("live loop: the points are held to the sample and 2 samples apart, whatever the order the edits come in") {
  VoicePool& p = pool_a();
  p.set_sample(0, loop_desc(0));
  std::uint32_t pan_tracks = 0;
  CHECK(p.apply_param(0, ip::LoopEnd, 500.0f, pan_tracks));  // under the start
  CHECK(p.sample(0).loop_start == 1000u);
  CHECK(p.sample(0).loop_end == 1002u);
  CHECK(p.apply_param(0, ip::LoopStart, 9000.0f, pan_tracks));  // past the end
  CHECK(p.sample(0).loop_start == 1000u);
  CHECK(p.apply_param(0, ip::LoopEnd, 90000.0f, pan_tracks));  // past the sample
  CHECK(p.sample(0).loop_end == kLen);
  CHECK(p.apply_param(0, ip::LoopStart, 5998.0f, pan_tracks));
  CHECK(p.sample(0).loop_start == 5998u);
  CHECK(p.apply_param(0, ip::LoopStart, 5999.0f, pan_tracks));  // 2 samples apart: 5998 is the last
  CHECK(p.sample(0).loop_start == 5998u);
  CHECK(p.apply_param(0, ip::LoopStart, 0.0f, pan_tracks));
  CHECK(p.sample(0).loop_start == 0u);
  CHECK_FALSE(p.apply_param(0, ip::LoopStart, std::nanf(""), pan_tracks));  // not a number: rejected
}

TEST_CASE("live loop: a one shot, a wavetable, a beat slice and an empty slot have no loop to move: accepted, nothing changes") {
  VoicePool& p = pool_a();
  SampleDesc d = loop_desc(0, 0, 0);  // no loop
  p.set_sample(0, d);
  std::uint32_t pan_tracks = 0;
  CHECK(p.apply_param(0, ip::LoopEnd, 500.0f, pan_tracks));
  CHECK(p.sample(0).loop_end == 0u);
  d = loop_desc(0, 0, 512);
  d.wt_size = 512;
  p.set_sample(1, d);
  CHECK(p.apply_param(1, ip::LoopEnd, 300.0f, pan_tracks));
  CHECK(p.sample(1).loop_end == 512u);
  CHECK(p.apply_param(2, ip::LoopEnd, 300.0f, pan_tracks));  // nothing in slot 2
  CHECK(p.sample(2).length == 0u);
}

TEST_CASE("live loop: a backward and a ping-pong voice past the new end are put at it and turn there; no sample is read past the new end") {
  for (const std::uint8_t mode : {std::uint8_t{1}, std::uint8_t{2}}) {
    VoicePool& p = pool_a();
    p.set_sample(0, loop_desc(mode));
    p.note_on(0, 60, 0, 127);
    for (int b = 0; b < 40; ++b) {
      run1(p);  // 2560: still on the first run toward the loop end
    }
    std::uint32_t pan_tracks = 0;
    CHECK(p.apply_param(0, ip::LoopEnd, 2000.0f, pan_tracks));
    int lo, hi;
    range_over(p, 50, lo, hi);
    CHECK_MESSAGE(hi <= 5 * 2000 + 6, "mode " << static_cast<int>(mode) << ": highest sample " << hi);
    CHECK(lo >= 5 * 1000 - 6);
  }
}

TEST_CASE("live loop: through the Engine, a ParamChange of kind Instrument with the loop's field moves the descriptor in the block it is drained and the voice in the next") {
  static o3os::test::StaticSlot<Engine> engine_slot;
  static o3os::test::StaticSlot<VoicePool> pool_slot;
  static audio::TapMem taps;
  static SeqEvent events[audio::kMaxEventsPerBlock];
  struct Src {
    static std::uint32_t step(void*, std::uint64_t block_start, std::uint32_t, SeqEvent* out, std::uint32_t cap) noexcept {
      if (block_start != 0u || cap < 1u) {
        return 0;
      }
      SeqEvent e{};
      e.kind = EventKind::NoteOn;
      e.track = 0;
      e.a = 60;
      e.b = 3;
      e.c = 127;
      out[0] = e;
      return 1;
    }
  };
  Engine& e = engine_slot.make(&fake_cycles, taps);
  VoicePool& p = pool_slot.make();
  p.set_sample(3, loop_desc(0));
  e.attach_voices(&p);
  audio::EventSource src;
  src.step = &Src::step;
  src.buffer = events;
  src.capacity = audio::kMaxEventsPerBlock;
  e.attach_events(src);
  alignas(8) static std::int16_t in[haudio::kBlockSamples];
  alignas(8) static std::int16_t out[haudio::kBlockSamples];
  const auto block = [&]() {
    haudio::BlockReport report{};
    e.render_block(haudio::BlockIo{in, out, 0, &report});
  };
  for (int b = 0; b < 40; ++b) {
    block();
  }
  CHECK(e.params().push(ParamChange{o3os::core::param_id(ParamKind::Instrument, 3, ip::LoopEnd), 2000.0f, 0u}));
  block();  // the ring is drained in this block's graph stage, after the voices: the descriptor moves here
  CHECK(p.sample(3).loop_end == 2000u);
  int hi = -1;
  for (int b = 0; b < 40; ++b) {
    block();
    const std::int16_t* f = p.feed(0);
    for (std::uint32_t k = 0; f != nullptr && k < kBlk; ++k) {
      hi = f[k] > hi ? f[k] : hi;
    }
  }
  CHECK(hi <= 5 * 2000 + 6);
  CHECK(hi > 5 * 1900);  // and it does play up to the new end
  CHECK(e.graph().params_rejected() == 0u);
}
