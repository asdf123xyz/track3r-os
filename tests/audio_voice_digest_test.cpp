// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-CPU10 slice 2: the sampler voice's output, pinned. The slice moves code of the voice's sample loop (A5), its filter (A6) and the strip's pan law (B1) without changing a sample of sound; this
// test renders a set of voices through VoicePool (the notes of one-shots and loops at many pitches, the amplitude ramps of a release, a retrigger and a live volume edit, the backward and
// ping-pong loops, the three filters with and without a cutoff envelope, an LFO and the shapers), hashes every block's feed (FNV-1a over the int16 samples, plus the voice's final position and
// amplitude) and compares the hash of each group with the value the code had BEFORE the slice (dev a8abd929). The blocks are cut into spans at offsets that move from block to block, so the fast
// loop's partial runs and the checked loop's takeovers are both in it. Host and Cortex-M4F (qemu_audio_voice_digest): the two must give the same hash.
#include <doctest/doctest.h>

#include <o3os/audio/voices.h>
#include <o3os/hal/audio.h>

#include "support/static_slot.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>

namespace {

namespace haudio = o3os::hal::audio;
using namespace o3os::audio;

constexpr std::uint32_t kBlk = haudio::kBlock;
constexpr std::uint32_t kLen = 3000;

std::int16_t g_noise[kLen];

struct Lcg {
  std::uint32_t s;
  std::uint32_t next() {
    s = s * 1664525u + 1013904223u;
    return s >> 8;
  }
};

void init_noise() {
  Lcg r{4242u};
  for (std::uint32_t i = 0; i < kLen; ++i) {
    // a noise with a slope under it, so that a sample read one place off, or a fraction that is off, changes the output
    g_noise[i] = static_cast<std::int16_t>(static_cast<std::int32_t>(r.next() & 0x7FFFu) - 16384 + static_cast<std::int32_t>((i * 7u) % 9000u) - 4500);
  }
}

o3os::test::StaticSlot<VoicePool> g_slot;

struct Fnv {
  std::uint32_t h = 2166136261u;
  void add(std::uint32_t v) {
    for (unsigned k = 0; k < 4u; ++k) {
      h = (h ^ ((v >> (8u * k)) & 0xFFu)) * 16777619u;
    }
  }
};

enum class Act : std::uint8_t { Off, On, Volume };
struct Ev {
  int block;
  Act act;
  unsigned arg;
};

struct Run {
  unsigned note = 60;
  unsigned vel = 127;
  int blocks = 24;
  Ev evs[4] = {};
  unsigned n_evs = 0;
  void add(int block, Act act, unsigned arg = 0) { evs[n_evs++] = Ev{block, act, arg}; }
};

void apply(VoicePool& p, const Ev& e) {
  switch (e.act) {
    case Act::Off: p.note_off(0); break;
    case Act::On: (void)p.note_on(0, e.arg, 0, 127); break;
    case Act::Volume: {
      std::uint32_t pan = 0;
      CHECK(p.apply_param(0, inst_param::Volume, static_cast<float>(e.arg), pan));
      break;
    }
  }
}

// One voice for `r.blocks` blocks: the events at their blocks' first sample, each block in two spans cut at an offset that moves.
void play(Fnv& f, const SampleDesc& d, const Run& r, std::uint32_t seed) {
  VoicePool& p = g_slot.make();
  p.set_sample(0, d);
  (void)p.note_on(0, r.note, 0, r.vel);
  Lcg rng{seed * 2654435761u + 11u};
  for (int b = 0; b < r.blocks; ++b) {
    p.begin_block();
    for (unsigned k = 0; k < r.n_evs; ++k) {
      if (r.evs[k].block == b) {
        apply(p, r.evs[k]);
      }
    }
    const std::uint32_t cut = 1u + rng.next() % (kBlk - 1u);
    p.render(0, cut);
    p.render(cut, kBlk);
    p.end_block();
    const std::int16_t* const out = p.feed(0);
    if (out == nullptr) {
      f.add(0x5A5A5A5Au);
    } else {
      for (std::uint32_t i = 0; i < kBlk; ++i) {
        f.add(static_cast<std::uint16_t>(out[i]));
      }
    }
  }
  VoicePool::Probe pr{};
  if (p.probe(0, pr)) {
    f.add(static_cast<std::uint32_t>(pr.phase));
    f.add(static_cast<std::uint32_t>(pr.phase >> 32));
    f.add(static_cast<std::uint32_t>(pr.amp));
  }
}

SampleDesc base(std::uint32_t from, std::uint32_t to) {
  SampleDesc d{g_noise, kLen, from, to, 65536};
  return d;
}

FilterMod env_mod(std::uint16_t attack, std::uint16_t decay, float sustain, std::uint16_t release, float amount) {
  FilterMod m;
  m.kind = kFmodEnv;
  m.amount = amount;
  m.sustain = sustain;
  m.attack = attack;
  m.decay = decay;
  m.release = release;
  return m;
}

FilterMod lfo_mod(std::uint8_t shape, std::uint8_t rate, float amount) {
  FilterMod m;
  m.kind = kFmodLfo;
  m.lfo = true;
  m.shape = shape;
  m.rate = rate;
  m.amount = amount;
  m.lfo_amount = amount;
  return m;
}

// `pinned` is the group's digest at 4080 cycles a sample (-DO3OS_RATE_44100=OFF); `pinned_44100` the one of the 44,100 Hz default when it differs (the groups with a filter,
// an envelope or an LFO, whose coefficients and rates follow the clock; the one-shots, the loops and the amplitude ramps do not).
void report(const char* group, std::uint32_t got, std::uint32_t pinned, std::uint32_t pinned_44100 = 0) {
#if defined(O3OS_RATE_44100) && O3OS_RATE_44100
  if (pinned_44100 != 0u) {
    pinned = pinned_44100;
  }
#else
  (void)pinned_44100;
#endif
  CHECK_MESSAGE(got == pinned, group << ": the digest changed");
  if (got != pinned) {
    std::printf("DIGEST %s 0x%08x\n", group, static_cast<unsigned>(got));
  }
}

}  // namespace

TEST_CASE("voice digest: one shots at every pitch, at two velocities (the unchecked loop, then the checked end)") {
  init_noise();
  Fnv f;
  const unsigned notes[] = {12, 36, 55, 60, 72, 84, 96, 108, 120};
  const std::uint32_t lens[] = {40, 300, kLen};
  std::uint32_t seed = 1;
  for (const unsigned note : notes) {
    for (const std::uint32_t len : lens) {
      for (const unsigned vel : {127u, 70u}) {
        Run r;
        r.note = note;
        r.vel = vel;
        r.blocks = 30;
        SampleDesc d = base(0, 0);
        d.length = len;
        play(f, d, r, seed++);
      }
    }
  }
  report("oneshot", f.h, 0x5250f4e6u);
}

TEST_CASE("voice digest: forward loops with a release, a retrigger and a live volume edit (the amplitude ramps)") {
  init_noise();
  Fnv f;
  const std::uint32_t loops[][2] = {{0, kLen}, {5, 8}, {100, 117}, {1000, kLen}, {0, 64}};
  const unsigned notes[] = {24, 48, 60, 84, 108};
  std::uint32_t seed = 100;
  for (const auto& lp : loops) {
    for (const unsigned note : notes) {
      Run r;
      r.note = note;
      r.blocks = 30;
      r.add(3, Act::Volume, 40);
      r.add(9, Act::Off);
      r.add(14, Act::On, note + 5u);
      r.add(19, Act::Volume, 90);
      play(f, base(lp[0], lp[1]), r, seed++);
    }
  }
  report("loop", f.h, 0x4b992821u);
}

TEST_CASE("voice digest: backward and ping-pong loops") {
  init_noise();
  Fnv f;
  const std::uint32_t loops[][2] = {{0, kLen}, {100, 117}};
  std::uint32_t seed = 200;
  for (const std::uint8_t mode : {std::uint8_t{1}, std::uint8_t{2}}) {
    for (const auto& lp : loops) {
      for (const unsigned note : {36u, 60u, 84u}) {
        Run r;
        r.note = note;
        r.blocks = 24;
        r.add(8, Act::Off);
        SampleDesc d = base(lp[0], lp[1]);
        d.loop_mode = mode;
        play(f, d, r, seed++);
      }
    }
  }
  report("dirloop", f.h, 0x786250e9u);
}

TEST_CASE("voice digest: the instrument filter, the three types, no modulator") {
  init_noise();
  Fnv f;
  std::uint32_t seed = 300;
  for (const std::uint8_t type : {kFilterLow, kFilterHigh, kFilterBand}) {
    for (const float cutoff : {0.25f, 0.6f, 0.95f}) {
      for (const float res : {0.0f, 2.0f, 4.0f}) {
        for (const unsigned note : {60u, 72u}) {
          Run r;
          r.note = note;
          r.blocks = 14;
          r.add(9, Act::Off);
          SampleDesc d = base(0, kLen);
          d.filter.enable = true;
          d.filter.wanted = true;
          d.filter.type = type;
          d.filter.cutoff = cutoff;
          d.filter.resonance = res;
          play(f, d, r, seed++);
        }
      }
    }
  }
  report("filter", f.h, 0x5491bf9eu, 0x57d3384du);
}

TEST_CASE("voice digest: the cutoff ADSR through its stages and the release, the cutoff LFO in five shapes") {
  init_noise();
  Fnv f;
  std::uint32_t seed = 400;
  for (const std::uint8_t type : {kFilterLow, kFilterHigh, kFilterBand}) {
    for (const float sustain : {0.0f, 0.4f, 1.0f}) {
      Run r;
      r.blocks = 80;
      r.add(30, Act::Volume, 60);
      r.add(52, Act::Off);
      SampleDesc d = base(0, kLen);
      d.filter.enable = true;
      d.filter.wanted = true;
      d.filter.type = type;
      d.filter.cutoff = 0.5f;
      d.filter.resonance = 1.5f;
      d.filter.mod = env_mod(5, 30, sustain, 40, 0.5f);
      d.env.enable = true;
      d.env.release = 20;
      play(f, d, r, seed++);
    }
  }
  report("fenv", f.h, 0xe5eafa12u, 0xb26ecacau);
  Fnv g;
  for (const std::uint8_t shape : {std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{2}, std::uint8_t{3}, std::uint8_t{4}}) {
    for (const std::uint8_t rate : {std::uint8_t{4}, std::uint8_t{12}, std::uint8_t{20}}) {
      Run r;
      r.blocks = 40;
      r.add(20, Act::Off);
      SampleDesc d = base(0, kLen);
      d.filter.enable = true;
      d.filter.wanted = true;
      d.filter.type = kFilterLow;
      d.filter.cutoff = 0.5f;
      d.filter.resonance = 1.0f;
      d.filter.mod = lfo_mod(shape, rate, 0.4f);
      d.env.enable = true;
      d.env.release = 30;
      play(g, d, r, seed++);
    }
  }
  report("flfo", g.h, 0xcd674967u, 0x6be1602eu);
}

TEST_CASE("voice digest: overdrive, bit depth, the tremolo and the pan and tune modulators") {
  init_noise();
  Fnv f;
  std::uint32_t seed = 500;
  for (const unsigned od : {0u, 25u, 80u}) {
    for (const unsigned bits : {16u, 12u, 6u}) {
      Run r;
      r.blocks = 20;
      SampleDesc d = base(0, kLen);
      d.overdrive = static_cast<std::uint8_t>(od);
      d.bit_depth = static_cast<std::uint8_t>(bits);
      d.filter.enable = true;
      d.filter.wanted = true;
      d.filter.type = kFilterBand;
      d.filter.cutoff = 0.7f;
      d.filter.resonance = 1.0f;
      d.pan_mod = lfo_mod(2, 8, 0.5f);
      d.tune_mod = lfo_mod(1, 8, 0.2f);
      d.amp_mod = lfo_mod(3, 8, 0.3f);
      play(f, d, r, seed++);
    }
  }
  report("shape", f.h, 0x7e0c8e84u, 0xc9237a30u);
}
