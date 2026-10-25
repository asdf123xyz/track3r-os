// SPDX-License-Identifier: GPL-3.0-or-later
//
// The sequencer against the reference vectors (OS-005c, ADR 0013 V1): swing and step duration against the
// start of each step in the reference clock, and random patterns of the deterministic FX (Micro-move, Roll in four directions, Gate,
// Off / Cut / Fade, the velocity FX, the Tempo FX, the Swing FX) against a fingerprint of the voice events the reference model produced.
// Expected values: the output of this project's own reference model (maintainer, 2026-10-18), kept as test vectors (data/seq_oracle_vectors.h);
// whether the official firmware agrees is an open research question. Host and
// Cortex-M4F (`qemu_seq_model`).
#include <doctest/doctest.h>

#include <o3os/seq/sequencer.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#include "data/seq_oracle_vectors.h"
#include "support/need.h"
#include "support/step_vectors.h"
#include "support/rng.h"
#include "support/seq_fixture.h"
#include "support/static_slot.h"

using namespace o3os;
using namespace o3os::test;
using audio::EventKind;
namespace fx = o3os::seq::fx;
namespace vec = o3os::test::vec;
namespace seqvec = o3os::test::seqvec;

#if defined(__arm__)
constexpr unsigned kTrials = 12;
#else
constexpr unsigned kTrials = 400;
#endif
static_assert(kTrials <= sizeof(o3os::test::seqvec::kTrials) / sizeof(o3os::test::seqvec::kTrials[0]), "the recorded trials cover the run");

namespace {

// One step of a generated pattern, in the vectors' terms (note -1 empty, 0..127, -2 fade, -3 cut, -4 off; volume -1 none).
struct GenStep {
  int note;
  int vol;
  int fx_type[2];
  int fx_val[2];
};

// What a test keeps of a run: the digest of the voice events and two sums of their times (vectors::Trial).
struct Fp {
  vec::Digest d;
  long long sum = 0, wsum = 0;
  unsigned n = 0;
  void add(unsigned kind, unsigned track, int note, int vel, long long t) {
    const std::uint8_t b[4] = {static_cast<std::uint8_t>(kind), static_cast<std::uint8_t>(track), static_cast<std::uint8_t>(note), static_cast<std::uint8_t>(vel)};
    d.add_bytes(b, sizeof b);
    sum += t;
    wsum += t * static_cast<long long>(n % 5 + 1);
    ++n;
  }
};

StaticSlot<seq::Sequencer>& g_slot = seq_slot();
Collector& g_col = seq_collector();

// The sample the reference timer puts a time at: the nearest (the sequencer rounds half up; the reference time is a double, so a tie is in the noise: one sample of tolerance).
bool within(std::uint64_t got, double want, double tol = 1.0) { return std::fabs(static_cast<double>(got) - want) <= tol; }

model::Step to_model(const GenStep& s, std::uint8_t instr) {
  model::Step m = model::kEmptyStep;
  if (s.note >= 0) {
    m.note = static_cast<std::uint8_t>(s.note + 1);
    m.instr = instr;
  } else if (s.note == -2) {
    m.note = model::kNoteFade;
    m.instr = instr;
  } else if (s.note == -3) {
    m.note = model::kNoteCut;
    m.instr = instr;
  } else if (s.note == -4) {
    m.note = model::kNoteOff;
    m.instr = instr;
  }
  m.vol = s.vol >= 0 ? static_cast<std::uint8_t>(s.vol) : model::kVolumeUnset;
  for (int c = 0; c < 2; ++c) {
    m.fx[c].type = static_cast<std::uint8_t>(s.fx_type[c]);
    m.fx[c].value = static_cast<std::uint8_t>(s.fx_val[c]);
  }
  return m;
}

}  // namespace

TEST_CASE("oracle: steps and microsteps against the reference clock, for tempos 40..800 and swings 25..75") {
  for (const seqvec::TimerCase& tc : seqvec::kTimer) {
    {
      const std::uint32_t t100 = tc.tempo_x100;
      const std::uint32_t sw = tc.swing;
      fresh_project(1, 1);
      model::Project& p = seq_project();
      CHECK(p.set_tempo_x100(t100) == model::Status::Ok);
      CHECK(p.set_swing(sw) == model::Status::Ok);
      CHECK(p.create_pattern(1, 8) == model::Status::Ok);
      for (unsigned k = 0; k < 8; ++k) {
        const GenStep st{static_cast<int>(40 + k), -1, {0, 0}, {0, 0}};
        CHECK(p.set_step(1, 0, k, to_model(st, 1)) == model::Status::Ok);
      }
      seq::Sequencer& s = g_slot.make(p);
      s.play_pattern(1);
      g_col.n = 0;
      g_col.run(s, 0, 400000, 64);
      // only the steps that fit in the run
      std::uint32_t compared = 0;
      for (unsigned i = 0; i < 24; ++i) {
        const double want = tc.step_start[i] * seqvec::kRateScale;  // the recorded sample, in the build's clock
        if (want + 2.0 > 400000.0) {
          break;
        }
        const Rec* r = g_col.nth(EventKind::NoteOn, i, 0);
        NEED(r != nullptr);
        CHECK_MESSAGE(within(r->t, want), t100);
        ++compared;
      }
      CHECK(compared >= 4u);
      // and the Step events too
      for (unsigned i = 0; i < compared; ++i) {
        CHECK(within(g_col.nth(EventKind::Step, i)->t, tc.step_start[i] * seqvec::kRateScale));
      }
    }
  }
}

TEST_CASE("oracle: random patterns of the deterministic FX against the reference events") {
  Rng rng(42);
  std::uint32_t total_events = 0;
  std::uint32_t with_roll = 0, with_micro = 0, with_gate = 0, with_tempo = 0, with_swing = 0;
  for (unsigned trial = 0; trial < kTrials; ++trial) {
    const unsigned tracks = 1 + rng.below(4);
    const unsigned len = 8 + rng.below(17);
    const std::uint32_t t100 = 6000 + rng.below(40000);
    const std::uint32_t sw = 25 + rng.below(51);
    fresh_project(tracks, 4);
    model::Project& p = seq_project();
    CHECK(p.set_tempo_x100(t100) == model::Status::Ok);
    CHECK(p.set_swing(sw) == model::Status::Ok);
    CHECK(p.create_pattern(1, len) == model::Status::Ok);
    // the pattern
    for (unsigned t = 0; t < tracks; ++t) {
      for (unsigned k = 0; k < len; ++k) {
        GenStep st{-1, -1, {0, 0}, {0, 0}};
        const std::uint32_t r = rng.below(100);
        if (r < 52) {
          st.note = static_cast<int>(30 + rng.below(70));
        } else if (r < 56) {
          st.note = -4;
        } else if (r < 59) {
          st.note = -3;
        } else if (r < 62) {
          st.note = -2;
        }
        if (st.note >= 0 && rng.below(100) < 25) {
          st.vol = static_cast<int>(rng.below(128));
        }
        for (int c = 0; c < 2; ++c) {
          if (rng.below(100) >= 33) {
            continue;
          }
          const std::uint32_t w = rng.below(100);
          if (w < 22) {
            st.fx_type[c] = 2;
            st.fx_val[c] = static_cast<int>(rng.below(101));
            ++with_micro;
          } else if (w < 52 && st.note >= 0) {
            const int dir = static_cast<int>(rng.below(5));  // const, vol down, vol up, note down, note up
            st.fx_type[c] = 3;
            st.fx_val[c] = 9 * dir + 1 + static_cast<int>(rng.below(8));
            ++with_roll;
          } else if (w < 64) {
            st.fx_type[c] = 20;
            st.fx_val[c] = 1 + static_cast<int>(rng.below(100));
            ++with_gate;
          } else if (w < 76) {
            st.fx_type[c] = 18;
            st.fx_val[c] = static_cast<int>(rng.below(101));
          } else if (w < 88) {
            st.fx_type[c] = 1;
          } else if (w < 94) {
            st.fx_type[c] = 15;
            st.fx_val[c] = 20 + static_cast<int>(rng.below(100));
            ++with_tempo;
          } else {
            st.fx_type[c] = 17;
            st.fx_val[c] = 25 + static_cast<int>(rng.below(51));
            ++with_swing;
          }
        }
        CHECK(p.set_step(1, t, k, to_model(st, 1)) == model::Status::Ok);
      }
    }
    // the sequencer for three laps, to the end of the compared window recorded for this trial
    const seqvec::Trial& want = seqvec::kTrials[trial];
    seq::Sequencer& s = g_slot.make(p);
    s.play_pattern(1);
    g_col.n = 0;
    g_col.total_events = 0;
    const double end_time = want.end_time * seqvec::kRateScale;  // the recorded window end, in the build's clock
    g_col.run(s, 0, static_cast<std::uint64_t>(end_time) + 300, 64);
    // per track, the sequence of voice events (kind, note, velocity) must be the recorded one, and their times agree to a sample or two
    Fp got;
    for (unsigned t = 0; t < tracks; ++t) {
      for (std::uint32_t si = 0; si < g_col.n; ++si) {
        const Rec& r = g_col.rec[si];
        if (r.ev.track != t || !(static_cast<double>(r.t) < end_time - 2.0)) {
          continue;
        }
        unsigned kind;
        switch (r.ev.kind) {
          case EventKind::NoteOn: kind = 0; break;
          case EventKind::NoteOff: kind = 1; break;
          case EventKind::NoteCut: kind = 2; break;
          case EventKind::NoteFade: kind = 3; break;
          default: continue;
        }
        got.add(kind, t, kind < 2 ? static_cast<int>(r.ev.a) : 0, kind == 0 ? (r.ev.c == audio::kVelocityDefault ? -1 : static_cast<int>(r.ev.c)) : 0,
                static_cast<long long>(r.t));
      }
    }
    got.d.add_u32(got.n);
    CHECK_MESSAGE(got.d.value() == want.digest, "trial " << trial);
    // At the recorded clock the sums agree to 2 and 10. At another clock the recorded sums lose the per-event times: each recorded time is
    // a sample rounded at the old rate, and the sequencer rounds its own exact time. Both are within half a sample of the same exact time
    // (scaled), so each event is off by less than one sample, and the sums by at most n and by the sum of the weights (1..5, as Fp::add)
    // on top of the recorded slack. That is a bound, not a statistic: at 44,100 Hz the steps fall on half samples, which rounds one way.
    double weights = 0.0;
    for (unsigned i = 0; i < got.n; ++i) {
      weights += static_cast<double>(i % 5u + 1u);
    }
    const double sum_tol = 2.0 + (seqvec::kRateScale == 1.0 ? 0.0 : static_cast<double>(got.n));
    const double wsum_tol = 10.0 + (seqvec::kRateScale == 1.0 ? 0.0 : weights);
    CHECK_MESSAGE(std::fabs(static_cast<double>(got.sum) - static_cast<double>(want.time_sum) * seqvec::kRateScale) <= sum_tol, "trial " << trial);
    CHECK_MESSAGE(std::fabs(static_cast<double>(got.wsum) - static_cast<double>(want.time_wsum) * seqvec::kRateScale) <= wsum_tol, "trial " << trial);
    total_events += got.n;
  }
  CHECK(total_events > kTrials * 20u);
  CHECK(with_roll > 10u);
  CHECK(with_micro > 10u);
  CHECK(with_gate > 10u);
  CHECK(with_tempo > 3u);
  CHECK(with_swing > 3u);
  if (std::getenv("O3OS_SEQ_REPORT") != nullptr) {
    std::printf("orc  %u random patterns, %u events matched the reference vectors (%u rolls, %u micro-moves, %u gates, %u tempo FX, %u swing FX)\n", static_cast<unsigned>(kTrials), static_cast<unsigned>(total_events), static_cast<unsigned>(with_roll), static_cast<unsigned>(with_micro),
                static_cast<unsigned>(with_gate), static_cast<unsigned>(with_tempo), static_cast<unsigned>(with_swing));
  }
}
