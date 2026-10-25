// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-BUG-WTLFO (audio/voices.h LFOS WHILE STOPPED). The six modulators that share the transport-phase generator (the tremolo, the pan, the finetune, the cutoff, the wavetable window and the granular
// position) move while the transport is STOPPED, at the tempo in force, and keep their phase when it starts; the running behaviour (the count held at the end of a step between Step events) is what it
// was. Host and Cortex-M4F.
#include <doctest/doctest.h>

#include "support/lfo_probe.h"

#define REQ LFO_REQ

// ---- OS-BUG-WTLFO (a): the LFOs run while the transport is stopped -----------------------------------------------------------------------------------------------------

TEST_CASE("lfo stopped: each of the six modulators moves with the transport stopped (it held its phase before), at the tempo in force") {
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
    const std::vector<double> v = series(c.dest, lfo(1, kRate8, 1.0f), 2 * kPeriodBlocks, false);
    CHECK_MESSAGE(spread(v, 8) >= c.min_spread, c.name << ": spread " << spread(v, 8));
    CHECK_MESSAGE(distinct(v, 8, c.quantum) >= c.min_distinct, c.name << ": distinct " << distinct(v, 8, c.quantum));
  }
}

TEST_CASE("lfo stopped: the wavetable window sweeps the stack in step with the tempo, twice in two periods") {
  const std::vector<double> v = series(kWt, lfo(1, kRate8, 1.0f), 2 * kPeriodBlocks + 20, false);
  // saw, amount 1 around window 4: window = 4 + (2u - 1) x 8, held to 0..7: 0 up to u = 0.5, then 4..7 by 0.0625 of the period each; the sweep ends in a wrap
  unsigned wraps = 0;
  double hi = 0;
  for (std::size_t i = 10; i < v.size(); ++i) {
    hi = std::max(hi, v[i]);
    wraps += (v[i] + 2.0 < v[i - 1]) ? 1u : 0u;
  }
  CHECK(hi == 7.0);
  CHECK(wraps == 2u);
  // the period: the first wrap and the second are one period apart (8 steps x 576 ticks at 300 BPM, in blocks)
  std::vector<std::size_t> at;
  for (std::size_t i = 10; i < v.size(); ++i) {
    if (v[i] + 2.0 < v[i - 1]) {
      at.push_back(i);
    }
  }
  REQ(at.size() == 2u);
  const double period_blocks = static_cast<double>(8u * 576u) / (576.0 * 20.0 /*steps a second at 300 BPM*/ / (static_cast<double>(haudio::sample_rate_mhz()) * 1e-3)) / kBlk;
  CHECK(std::abs(static_cast<double>(at[1] - at[0]) - period_blocks) <= 1.5);
}

TEST_CASE("lfo stopped: the same tempo, the same sweep, whatever the Stopped event did (a stop after a run goes on from where the count was)") {
  VoicePool& p = pool(0);
  p.set_sample(0, desc_for(kWt, lfo(1, kRate8, 1.0f)));
  p.set_tempo_x100(kTempo);
  p.restart_clock();
  p.on_step(0);
  REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
  Probe pr{kWt, 0};
  std::vector<double> run;
  for (unsigned b = 0; b < 60; ++b) {
    run.push_back(step_block(p, pr));
  }
  p.transport_stopped();  // a stop without further Step events: the count was held at the end of its step, it runs on now
  for (unsigned b = 0; b < kPeriodBlocks; ++b) {
    run.push_back(step_block(p, pr));
  }
  CHECK(spread(run, 60) >= 4.0);
}

// ---- the phase is continuous across a Step after a stop and a tempo change while stopped --------------------------------------------------------------------------------

namespace {

// One voice of the tremolo (a saw of 8 steps, a continuous level), the transport stopped; `before` blocks, then `act`, then `after` blocks. Returns the levels; the jump at the seam is |v[before] - v[before - 1]|.
template <class F>
std::vector<double> seam_series(unsigned before, unsigned after, F act) {
  VoicePool& p = pool(0);
  p.set_sample(0, desc_for(kAmp, lfo(1, kRate8, 1.0f)));
  p.set_tempo_x100(kTempo);
  p.restart_clock();
  p.transport_stopped();
  p.note_on(0, 60, 0, audio::kVelocityDefault);
  p.mark_live();
  Probe pr{kAmp, 0};
  std::vector<double> v;
  for (unsigned b = 0; b < before; ++b) {
    v.push_back(step_block(p, pr));
  }
  act(p);
  for (unsigned b = 0; b < after; ++b) {
    v.push_back(step_block(p, pr));
  }
  return v;
}

double largest_step(const std::vector<double>& v, std::size_t seam) {
  double step = 0;
  for (std::size_t i = 21; i < v.size(); ++i) {
    if (i != seam) {
      step = std::max(step, std::abs(v[i] - v[i - 1]));
    }
  }
  return step;
}

}  // namespace

TEST_CASE("lfo stopped: a Step after a stop with no start (Continue, a resume) keeps the phase of the voice that sounds") {
  for (const unsigned step_no : {0u, 5u, 40u}) {
    for (const unsigned before : {45u, 120u, 200u}) {
      const std::vector<double> v = seam_series(before, 30, [&](VoicePool& p) { p.on_step(step_no); });
      const double jump = std::abs(v[before] - v[before - 1]);
      const double step = largest_step(v, before);
      CHECK_MESSAGE(jump <= 2.0 * step + 1e-9, "Step " << step_no << " after " << before << " blocks: jump " << jump << " against a step of " << step);
      CHECK(spread(v, 21) > 100.0);
    }
  }
}

TEST_CASE("lfo stopped: a Step after a stop holds the random level already drawn (the period index of the new count is the voice's)") {
  for (const unsigned before : {45u, 330u}) {  // (330: in the second period of 8 steps, a level has been drawn twice)
    VoicePool& p = pool(0);
    p.set_sample(0, desc_for(kPan, lfo(4, kRate8, 1.0f)));
    p.set_tempo_x100(kTempo);
    p.restart_clock();
    p.transport_stopped();
    p.note_on(0, 60, 0, audio::kVelocityDefault);
    p.mark_live();
    Probe pr{kPan, 0};
    std::vector<double> v;
    for (unsigned b = 0; b < before; ++b) {
      v.push_back(step_block(p, pr));
    }
    p.on_step(5);
    for (unsigned b = 0; b < 20; ++b) {
      v.push_back(step_block(p, pr));
    }
    CHECK_MESSAGE((v[before - 1] == v[before] && v[before] == v[before + 1] && v[before + 1] == v[before + 2]), "after " << before << " blocks: " << v[before - 1] << " " << v[before] << " " << v[before + 1] << " " << v[before + 2]);
  }
}

TEST_CASE("lfo stopped: a tempo change while stopped changes the rate and not the phase") {
  const std::vector<double> v = seam_series(100, 60, [](VoicePool& p) { p.set_tempo_x100(kTempo / 2u); });
  const double jump = std::abs(v[100] - v[99]);
  CHECK_MESSAGE(jump <= 2.0 * largest_step(v, 100) + 1e-9, "jump " << jump);
  const double fast = v[99] - v[89];   // blocks 89..99 at 300 BPM
  const double slow = v[150] - v[140];  // blocks 140..150 at 150 BPM
  const bool half = slow > 0.4 * fast && slow < 0.6 * fast;
  CHECK_MESSAGE(half, "slope " << fast << " -> " << slow);
}

// ---- running behaviour unchanged ------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("lfo running: between Step events the count is held at the end of its step, as before") {
  VoicePool& p = pool(0);
  p.set_sample(0, desc_for(kWt, lfo(1, kRate8, 1.0f)));
  p.set_tempo_x100(kTempo);
  p.restart_clock();
  p.on_step(0);
  REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
  Probe pr{kWt, 0};
  std::vector<double> v;
  for (unsigned b = 0; b < 200; ++b) {
    v.push_back(step_block(p, pr));  // a step is 50 ms (34 blocks): no further Step event: held from the first step's end on
  }
  CHECK(spread(v, 60) == 0.0);
}

// ---- the phase when the transport starts ---------------------------------------------------------------------------------------------------------------------------

TEST_CASE("lfo stopped: a voice that sounds when the transport starts keeps its phase; a restart while running restarts the count") {
  VoicePool& p = pool(0);
  p.set_sample(0, desc_for(kWt, lfo(1, kRate8, 1.0f)));
  p.set_tempo_x100(kTempo);
  p.restart_clock();
  p.transport_stopped();
  REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
  Probe pr{kWt, 0};
  std::vector<double> v;
  for (unsigned b = 0; b < 190; ++b) {  // 0.68 of a period: u = 0.68, window 4 + int(0.36 x 8) = 6
    v.push_back(step_block(p, pr));
  }
  const double before = v.back();
  CHECK(before >= 5.0);
  p.restart_clock();  // the transport starts (FxReset) ...
  p.on_step(0);       // ... and its first step
  v.clear();
  for (unsigned b = 0; b < 4; ++b) {
    v.push_back(step_block(p, pr));
  }
  for (const double w : v) {
    CHECK_MESSAGE(std::abs(w - before) <= 1.0, "window " << w << " after the start, " << before << " before");  // not the start of the period (window 0)
  }
  // a start while the transport ran: the count starts over, and a voice that began with the count (no shift of its own) is at the start of its period again
  REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
  v.clear();
  unsigned n = 0;
  for (unsigned b = 0; b < 200; ++b) {
    if (b % 34u == 0u) {
      p.on_step(++n);  // a step is 50 ms, 34.4 blocks: the Step events go on (the count is held at a step's end only between them)
    }
    v.push_back(step_block(p, pr));
  }
  const double running_before = v.back();
  CHECK(running_before >= 5.0);
  p.restart_clock();
  p.on_step(0);
  CHECK(step_block(p, pr) <= 1.0);
}
