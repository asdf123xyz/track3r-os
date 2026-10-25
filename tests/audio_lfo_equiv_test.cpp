// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-CPUOPT1 (docs/specs/OS-CPUOPT1.md): the live LFO paths of the voice pool are bit for bit what they were before the optimisation. The pool is run over many scenarios (the six destinations x the five
// shapes x two speeds x two amounts x the transport running or stopped x a note, a retrigger and a Step event that split the blocks at 0, 1, 17, 33 and 63 samples, Slide FX and live Tune / FineTune
// edits on the finetune destination, every filter type on the cutoff) and everything the outside sees is hashed: the voice's feed of every block, the pan the autopan hands the strip, and the
// random shape's draws (they are in the samples). The hashes below were recorded on dev (voices.cpp, voices.h and svf.h of origin/dev 27f2788, which carry none of C1..C5: the files were swapped in and the test run, see the commit); an optimisation that changes one sample of one scenario fails here.
#include <doctest/doctest.h>

#include "support/lfo_probe.h"

#include <o3os/core/audio_clock.h>

#include <cstdio>
#include <string>

namespace {

std::uint64_t fnv(std::uint64_t h, std::uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) {
    h ^= (v >> (8 * i)) & 0xFFu;
    h *= 1099511628211ull;
  }
  return h;
}

std::uint64_t scenarios(Dest dest) {
  std::uint64_t h = 1469598103934665603ull;
  const unsigned splits[] = {0, 1, 17, 33, 63};
  unsigned run_no = 0;
  for (unsigned shape = 0; shape < 5; ++shape) {
    for (unsigned rate : {3u, 20u}) {
      for (float amount : {1.0f, 0.3f}) {
        for (bool running : {true, false}) {
          for (unsigned split : splits) {
            ++run_no;
            VoicePool& p = pool(0);
            audio::SampleDesc d = desc_for(dest, lfo(static_cast<std::uint8_t>(shape), static_cast<std::uint8_t>(rate), amount));
            if (dest == kCut) {
              d.filter.type = static_cast<std::uint8_t>(shape % 3u);
              d.filter.resonance = (rate % 2u) != 0u ? 2.0f : 0.5f;
            }
            p.set_sample(0, d);
            p.set_tempo_x100(kTempo + 1000u * (run_no % 5u));
            p.restart_clock();
            if (!running) {
              p.transport_stopped();
            }
            if (!p.note_on(0, 55 + run_no % 12u, 0, audio::kVelocityDefault)) {
              FAIL_CHECK("note_on");
              return 0;
            }
            p.mark_live();
            for (unsigned b = 0; b < 30; ++b) {
              p.begin_block();
              if (b % 7u == 0u && running) {
                p.on_step(b % 32u);  // a Step event: the tick count snaps to its step (tick_steps_ 0, 1, 2 ...)
              }
              if (dest == kTune && run_no % 4u == 0u) {
                if (b == 3u) {
                  p.fx_event(0, audio::kModGroupSlide, audio::kFxSlideUp, 40);  // a Slide FX under the finetune LFO
                } else if (b == 20u) {
                  p.fx_event(0, audio::kModGroupSlide, 0, 0);  // ... and its end mid-LFO
                }
              }
              if (dest == kTune && run_no % 4u == 1u) {
                std::uint32_t pan = 0;
                if (b == 9u) {
                  (void)p.apply_param(0, ip::FineTune, 12.0f, pan);  // live edits of the pitch the LFO adds to
                } else if (b == 15u) {
                  (void)p.apply_param(0, ip::Tune, 3.0f, pan);
                } else if (b == 21u) {
                  (void)p.apply_param(0, ip::FineTune, -30.0f, pan);
                }
              }
              if (b % 3u == 1u && split != 0u) {
                p.render(0, split);
                (void)p.note_on(0, 55 + (run_no + b) % 12u, 0, audio::kVelocityDefault);  // a retrigger inside the block
                p.render(split, kBlk);
              } else {
                p.render(0, kBlk);
              }
              p.end_block();
              const std::int16_t* f = p.feed(0);
              for (std::uint32_t i = 0; f != nullptr && i < kBlk; ++i) {
                h = fnv(h, static_cast<std::uint32_t>(static_cast<std::uint16_t>(f[i])));
              }
              if (p.take_pan_mods() != 0u) {
                h = fnv(h, 0x10000u + p.pan_mod_value(0));
              }
            }
            p.cut(0);
            for (unsigned b = 0; b < 3; ++b) {
              p.begin_block();
              p.render(0, kBlk);
              p.end_block();
            }
          }
        }
      }
    }
  }
  return h;
}

// The cutoff's ADSR (kFmodEnv) through the same filter code (filter_run's chunks: the envelope is evaluated every 16th sample and its `since` counter is advanced per chunk), with a note-off and a
// retrigger in the middle of the run.
std::uint64_t scenarios_env() {
  std::uint64_t h = 1469598103934665603ull;
  for (unsigned i = 0; i < 24; ++i) {
    audio::FilterMod m;
    m.kind = audio::kFmodEnv;
    m.amount = (i % 3u == 2u) ? -0.6f : 1.0f;
    m.sustain = static_cast<float>(i % 4u) * 0.3f;
    m.delay = static_cast<std::uint16_t>((i % 5u) * 3u);
    m.attack = static_cast<std::uint16_t>(2u + (i % 7u) * 4u);
    m.hold = static_cast<std::uint16_t>((i % 3u) * 5u);
    m.decay = static_cast<std::uint16_t>(5u + (i % 6u) * 9u);
    m.release = static_cast<std::uint16_t>(3u + (i % 4u) * 11u);
    VoicePool& p = pool(0);
    audio::SampleDesc d = desc_for(kCut, m);
    d.filter.type = static_cast<std::uint8_t>(i % 3u);
    d.filter.resonance = (i % 2u) != 0u ? 3.0f : 0.5f;
    p.set_sample(0, d);
    p.set_tempo_x100(kTempo);
    p.restart_clock();
    if (!p.note_on(0, 60, 0, audio::kVelocityDefault)) {
      FAIL_CHECK("note_on");
      return 0;
    }
    p.mark_live();
    for (unsigned b = 0; b < 40; ++b) {
      p.begin_block();
      if (b == 14u) {
        p.note_off(0);  // the ADSR's release
      }
      if (b == 26u) {
        (void)p.note_on(0, 62, 0, audio::kVelocityDefault);  // and a retrigger inside the block
        p.render(0, 29u);
        (void)p.note_on(0, 64, 0, audio::kVelocityDefault);
        p.render(29u, kBlk);
      } else {
        p.render(0, kBlk);
      }
      p.end_block();
      const std::int16_t* f = p.feed(0);
      for (std::uint32_t k = 0; f != nullptr && k < kBlk; ++k) {
        h = fnv(h, static_cast<std::uint32_t>(static_cast<std::uint16_t>(f[k])));
      }
    }
    p.cut(0);
    for (unsigned b = 0; b < 3; ++b) {
      p.begin_block();
      p.render(0, kBlk);
      p.end_block();
    }
  }
  return h;
}

// Two voices whose LFOs are the random shape: they draw from one generator (rng_), so the order of the evaluations of both voices is part of the sound.
std::uint64_t scenarios_rng() {
  std::uint64_t h = 1469598103934665603ull;
  for (unsigned i = 0; i < 12; ++i) {
    VoicePool& p = pool(0);
    const unsigned rate = (i % 3u == 0u) ? 3u : ((i % 3u == 1u) ? 12u : 20u);
    const float amount = (i % 2u) != 0u ? 1.0f : 0.5f;
    p.set_sample(0, desc_for(kCut, lfo(4, static_cast<std::uint8_t>(rate), amount)));
    p.set_sample(1, desc_for(i % 2u ? kAmp : kTune, lfo(4, static_cast<std::uint8_t>(rate + 1u), 1.0f)));
    p.set_tempo_x100(kTempo);
    p.restart_clock();
    if (i % 4u == 3u) {
      p.transport_stopped();
    }
    if (!p.note_on(0, 55, 0, audio::kVelocityDefault) || !p.note_on(1, 62, 1, audio::kVelocityDefault)) {
      FAIL_CHECK("note_on");
      return 0;
    }
    p.mark_live();
    for (unsigned b = 0; b < 40; ++b) {
      p.begin_block();
      if (b == 11u) {
        p.render(0, 21u);
        (void)p.note_on(1, 66, 1, audio::kVelocityDefault);  // the second voice restarts its LFO between the first one's evaluations
        p.render(21u, kBlk);
      } else {
        p.render(0, kBlk);
      }
      p.end_block();
      for (unsigned t = 0; t < 2; ++t) {
        const std::int16_t* f = p.feed(t);
        for (std::uint32_t k = 0; f != nullptr && k < kBlk; ++k) {
          h = fnv(h, static_cast<std::uint32_t>(static_cast<std::uint16_t>(f[k])));
        }
      }
    }
    p.cut(0);
    p.cut(1);
    for (unsigned b = 0; b < 3; ++b) {
      p.begin_block();
      p.render(0, kBlk);
      p.end_block();
    }
  }
  return h;
}

// Recorded on the unchanged tree; the six destinations also run green on origin/dev 27f2788 (voices.cpp, voices.h, svf.h swapped in), and the two groups below were recorded there.
// These are the values at the 44,117.647 Hz clock (-DO3OS_RATE_44100=OFF). The envelope times, the filter's cutoff table and the LFO rates follow the engine's rate, so the 44,100 Hz clock (the default,
// OS-RATE) has its own set, recorded on the tree that made the OFF set (the DSP constants derive from core/audio_clock.h and are bit for bit the old ones at OFF).
constexpr bool k44100 = o3os::core::kRefCyclesPerSample.num == 1800000u;
constexpr std::uint64_t kExpectedOff[kDests] = {0xcc3d29625eadabf5ull, 0x5a0a02b68ae686c7ull, 0x8568769e7b4ba8a9ull, 0xe0e0ed3f2d8a4284ull, 0xb3404b1a295fadd5ull, 0x9f0659772e1c31baull};
constexpr std::uint64_t kExpectedOn[kDests] = {0x1a697f71ced62a8bull, 0xbbd5a575ceb3a217ull, 0x61c11a52e35e7af9ull, 0x958a12ccee0fd420ull, 0x0c4f159040bed6e2ull, 0xcfcc27fc08fe144bull};
constexpr std::uint64_t kExpectedEnv = k44100 ? 0x43ae3a3603d3f0caull : 0x505de87b46ae8c90ull;
constexpr std::uint64_t kExpectedRng = k44100 ? 0x3c0ef0a2d64bf1d5ull : 0x3faa10e78b2482e5ull;
const char* const kNames[kDests] = {"tremolo", "pan", "finetune", "cutoff", "wavetable window", "granular position"};

}  // namespace

TEST_CASE("OS-CPUOPT1: the live LFO paths are bit for bit what they were (six destinations, every shape, splits, Slide FX, live edits)") {
  for (unsigned d = 0; d < kDests; ++d) {
    const std::uint64_t h = scenarios(static_cast<Dest>(d));
    char hex[40];
    std::snprintf(hex, sizeof hex, "0x%016llxull", static_cast<unsigned long long>(h));
    CAPTURE(std::string(kNames[d]));
    CAPTURE(hex);
    CHECK(h == (k44100 ? kExpectedOn[d] : kExpectedOff[d]));
  }
}

TEST_CASE("OS-CPUOPT1: the cutoff's ADSR through the filter chunks, and two random-shape LFOs sharing one generator, are bit for bit what they were") {
  char hex[40];
  const std::uint64_t e = scenarios_env();
  std::snprintf(hex, sizeof hex, "0x%016llxull", static_cast<unsigned long long>(e));
  CAPTURE(std::string(hex));
  CHECK(e == kExpectedEnv);
  const std::uint64_t r = scenarios_rng();
  std::snprintf(hex, sizeof hex, "0x%016llxull", static_cast<unsigned long long>(r));
  CAPTURE(std::string(hex));
  CHECK(r == kExpectedRng);
}
