// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-LFOLIVE (audio/voices.h LIVE LFO FIELDS, docs/specs/OS-LFOLIVE.md): the LFO's SOURCE, shape, speed and amount are live. An edit of the descriptor through apply_param is what a voice that had the new
// value from its note on does from the next block, bit for bit (the phase is the transport's, so the two cannot differ); an edit that changes nothing changes nothing; the fields are held to range.
// Host and Cortex-M4F.
#include <doctest/doctest.h>

#include "support/lfo_probe.h"

#define REQ LFO_REQ

// ---- OS-LFOLIVE: the fields are live ------------------------------------------------------------------------------------------------------------------------------------

namespace {

// One field of the slot of `dest` (e: tremolo 0, pan 1, cutoff 2, wt 3, gran 4, finetune 5).
unsigned slot_of(Dest d) {
  static constexpr unsigned kSlot[kDests] = {0, 1, 5, 2, 3, 4};
  return kSlot[d];
}

// Two pools in lockstep: A starts with `from` and receives the edit after `at` blocks, C has `to` from its note on. Their outputs agree bit for bit from the second block after the edit (the descriptor is
// what the voices read, and the LFO's phase is the transport's: the voices carry nothing else of the old value). The series of A and C returned.
void edit_equals_note_on(Dest dest, const audio::FilterMod& from, const audio::FilterMod& to, unsigned field, float value, bool (*extra)(VoicePool&, unsigned), const char* what) {
  VoicePool& a = pool(0);
  VoicePool& c = pool(2);
  for (VoicePool* p : {&a, &c}) {
    p->set_sample(0, desc_for(dest, p == &a ? from : to));
    p->set_tempo_x100(kTempo);
    p->restart_clock();
    p->transport_stopped();
    REQ(p->note_on(0, 60, 0, audio::kVelocityDefault));
    p->mark_live();
  }
  Probe pa{dest, 0}, pc{dest, 0};
  const unsigned at = 70;
  unsigned equal_after = 0, total_after = 0;
  double differ_before = 0;
  for (unsigned b = 0; b < 2 * kPeriodBlocks; ++b) {
    if (b == at) {
      std::uint32_t pan = 0;
      CHECK_MESSAGE(a.apply_param(0, ip::lfo(slot_of(dest), field), value, pan), what);
      if (extra != nullptr) {
        (void)extra(a, slot_of(dest));
      }
    }
    const double xa = step_block(a, pa);
    const double xc = step_block(c, pc);
    if (b > at + 1u) {  // (the tremolo's multiplier walks to its new target over the first block: the window, the pan are exact from the first)
      ++total_after;
      equal_after += xa == xc ? 1u : 0u;
    } else if (b < at) {
      differ_before = std::max(differ_before, std::abs(xa - xc));
    }
  }
  CHECK_MESSAGE(equal_after == total_after, what << ": " << equal_after << " of " << total_after << " blocks equal after the edit");
  (void)differ_before;
}

}  // namespace

TEST_CASE("lfo live: a shape or amount edit is what the note on with the new value does, from the next block (wavetable window, tremolo, autopan)") {
  for (const Dest dest : {kWt, kAmp, kPan}) {
    const float tgt_amount = 0.5f;
    edit_equals_note_on(dest, lfo(1, kRate8, 1.0f), lfo(0, kRate8, 1.0f), ip::LfoShape, 0.0f, nullptr, "shape saw -> reverse saw");
    edit_equals_note_on(dest, lfo(1, kRate8, 1.0f), lfo(1, kRate8, tgt_amount), ip::LfoAmount, tgt_amount, nullptr, "amount 1 -> 0.5");
  }
}

TEST_CASE("lfo live: a SPEED edit keeps the phase: the output is continuous across the edit (no zipper at a step of the encoder), and the new rate is in force") {
  for (const Dest dest : {kAmp, kPan, kCut}) {
    for (const unsigned at : {70u, 150u}) {
      VoicePool& p = pool(0);
      p.set_sample(0, desc_for(dest, lfo(1, kRate8, 1.0f)));
      p.set_tempo_x100(kTempo);
      p.restart_clock();
      p.transport_stopped();
      REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
      p.mark_live();
      Probe pr{dest, 0};
      std::vector<double> v;
      for (unsigned b = 0; b < at; ++b) {
        v.push_back(step_block(p, pr));
      }
      std::uint32_t pan = 0;
      REQ(p.apply_param(0, ip::lfo(slot_of(dest), ip::LfoSpeed), 6.0f, pan));  // 8 steps -> 16: the period doubles
      for (unsigned b = 0; b < 40; ++b) {
        v.push_back(step_block(p, pr));
      }
      double step = 0;  // the largest block-to-block change outside the seam (the edit and the two blocks after: the tremolo's multiplier and the pan walk to a new level over a block)
      double jump = 0;
      for (std::size_t i = 21; i < v.size(); ++i) {
        const double d = std::abs(v[i] - v[i - 1]);
        (i >= at && i <= at + 2u ? jump : step) = std::max(i >= at && i <= at + 2u ? jump : step, d);
      }
      CHECK_MESSAGE(jump <= 2.0 * step + 1e-9, "dest " << static_cast<unsigned>(dest) << " at " << at << ": jump " << jump << " against a step of " << step);
      // the new rate is in force: over the 40 blocks after the edit the saw climbs at about half the old slope
      const double before = v[at - 1] - v[at - 11];
      const double after = v[at + 30] - v[at + 20];
      if (dest == kAmp) {
        const bool half = after > 0.3 * before && after < 0.7 * before;
        CHECK_MESSAGE(half, "slope " << before << " -> " << after);
      }
    }
  }
}

namespace {

// A voice of `dest` (a pan or the cutoff: the pan's value and the filter's rms follow the LFO closely), the transport stopped, an LFO of `shape` at rate index `from`; at block `at` its SPEED becomes `to`; `after`
// more blocks. Blocks of the new period: kLfoRate[to] steps of 576 ticks at 300 BPM (20 steps a second).
std::vector<double> speed_series(Dest dest, std::uint8_t shape, std::uint8_t from, std::uint8_t to, unsigned at, unsigned after) {
  VoicePool& p = pool(0);
  p.set_sample(0, desc_for(dest, lfo(shape, from, 1.0f)));
  p.set_tempo_x100(kTempo);
  p.restart_clock();
  p.transport_stopped();
  p.note_on(0, 60, 0, audio::kVelocityDefault);
  p.mark_live();
  Probe pr{dest, 0};
  std::vector<double> v;
  for (unsigned b = 0; b < at; ++b) {
    v.push_back(step_block(p, pr));
  }
  std::uint32_t pan = 0;
  p.apply_param(0, ip::lfo(slot_of(dest), ip::LfoSpeed), static_cast<float>(to), pan);
  for (unsigned b = 0; b < after; ++b) {
    v.push_back(step_block(p, pr));
  }
  return v;
}

double period_blocks(unsigned rate) {
  return static_cast<double>(audio::kLfoRate[rate]) * static_cast<double>(haudio::sample_rate_mhz()) * 1e-3 / 20.0 / kBlk;
}

}  // namespace

TEST_CASE("lfo live: a SPEED edit up or down, with a proportion that truncates, is continuous at the seam and the new period is in force (pan and cutoff)") {
  const struct {
    std::uint8_t from, to;
  } cases[] = {{8, 6}, {6, 8}, {8, 9}, {9, 8}, {6, 9}};  // 8 -> 16 steps, 16 -> 8, 8 -> 6 (x3/4: (x % op) x np / op truncates), 6 -> 8 (x4/3), 16 -> 6
  for (const Dest dest : {kPan, kCut}) {
    for (const auto& c : cases) {
      for (const unsigned at : {70u, 150u}) {
        const unsigned after = static_cast<unsigned>(3.2 * period_blocks(c.to)) + 20u;
        const std::vector<double> v = speed_series(dest, 1, c.from, c.to, at, after);
        const double wrap = (dest == kPan ? 0.4 : 0.15) * spread(v, 21);  // (the cutoff's rms is a compressed saw: its wrap is a smaller part of the range)
        double step = 0, jump = 0;
        for (std::size_t i = 21; i < v.size(); ++i) {
          const double d = std::abs(v[i] - v[i - 1]);
          if (i >= at && i <= at + 2u) {
            jump = std::max(jump, d);  // (no wrap is due at 70 or 150 blocks: the phase is 0.13 .. 0.54 of the old period)
          } else if (d < wrap) {
            step = std::max(step, d);
          }
        }
        CHECK_MESSAGE(jump <= 2.0 * step + 1e-9, "dest " << static_cast<unsigned>(dest) << " " << int(c.from) << " -> " << int(c.to) << " at " << at << ": jump " << jump << " against a step of " << step);
        // the new period: two wraps of the saw, one period of the new rate apart. A wrap is where the mean of the 8 blocks before exceeds the mean of the 8 after by the most (the cutoff's rms is noisy), per cluster.
        std::vector<std::size_t> w;
        double best = 0;
        std::size_t best_i = 0;
        for (std::size_t i = at + 8u; i + 8u <= v.size(); ++i) {
          double before = 0, next = 0;
          for (unsigned k = 1; k <= 8; ++k) {
            before += v[i - k];
            next += v[i + k - 1];
          }
          const double drop = (before - next) / 8.0;
          if (drop > 0.25 * spread(v, 21)) {
            if (best_i != 0 && i - best_i > 30u) {
              w.push_back(best_i);
              best = 0;
            }
            if (drop > best) {
              best = drop;
              best_i = i;
            }
          }
        }
        if (best_i != 0) {
          w.push_back(best_i);
        }
        if (w.size() < 2u) {
          FAIL_CHECK("dest " << static_cast<unsigned>(dest) << " " << int(c.from) << " -> " << int(c.to) << " at " << at << ": " << w.size() << " wraps, spread " << spread(v, 21));
          continue;
        }
        const double dist = static_cast<double>(w[1] - w[0]);
        const double tol = 2.0 + (dest == kCut ? 0.06 * period_blocks(c.to) : 0.0);  // (the cutoff's rms is noisy: its wrap is found to a few percent, the rates differ by 33 % at least)
        CHECK_MESSAGE(std::abs(dist - period_blocks(c.to)) <= tol, "dest " << static_cast<unsigned>(dest) << " " << int(c.from) << " -> " << int(c.to) << ": wraps " << dist << " blocks apart, the period is " << period_blocks(c.to));
      }
    }
  }
}

TEST_CASE("lfo live: a SPEED edit holds the random level already drawn (it is not drawn again at the edit)") {
  const struct {
    std::uint8_t from, to;
    unsigned at;
  } cases[] = {{8, 6, 70}, {8, 6, 330}, {6, 8, 70}, {6, 8, 700}, {8, 9, 330}, {9, 8, 70}};  // (330 is in the second period of 8 steps, 700 in the second of 16: the period index is not 0)
  for (const auto& c : cases) {
    const std::vector<double> v = speed_series(kPan, 4, c.from, c.to, c.at, 20);
    CHECK_MESSAGE((v[c.at - 1] == v[c.at] && v[c.at] == v[c.at + 1] && v[c.at + 1] == v[c.at + 2]), int(c.from) << " -> " << int(c.to) << " at " << c.at << ": " << v[c.at - 1] << " " << v[c.at] << " " << v[c.at + 1] << " " << v[c.at + 2]);
  }
}

TEST_CASE("lfo live: a speed or amount edit reaches the granular position, the finetune and the cutoff within one block") {
  for (const Dest dest : {kGran, kTune, kCut}) {
    // not bit for bit (the grain's wrap, the pitch's phase and the filter's state carry the history): the voice that is edited departs from the one that is not at once
    VoicePool& a = pool(0);
    VoicePool& c = pool(2);
    for (VoicePool* p : {&a, &c}) {
      p->set_sample(0, desc_for(dest, lfo(1, kRate8, 1.0f)));
      p->set_tempo_x100(kTempo);
      p->restart_clock();
      p->transport_stopped();
      REQ(p->note_on(0, 60, 0, audio::kVelocityDefault));
      p->mark_live();
    }
    Probe pa{dest, 0}, pc{dest, 0};
    for (unsigned b = 0; b < 200; ++b) {
      CHECK(step_block(a, pa) == step_block(c, pc));  // lockstep: the same, bit for bit, while nothing is edited
    }
    std::uint32_t pan = 0;
    CHECK(a.apply_param(0, ip::lfo(slot_of(dest), ip::LfoAmount), 0.25f, pan));
    CHECK(a.apply_param(0, ip::lfo(slot_of(dest), ip::LfoShape), 0.0f, pan));
    double worst = 0;
    for (unsigned b = 0; b < 3; ++b) {
      worst = std::max(worst, std::abs(step_block(a, pa) - step_block(c, pc)));
    }
    CHECK_MESSAGE(worst > 0.0, "dest " << static_cast<unsigned>(dest) << ": no change after an amount and shape edit");
  }
}

TEST_CASE("lfo live: SOURCE Off, Envelope and LFO switch the modulator on a sounding voice, and an LFO started live is the note on's") {
  // Off -> LFO: A has no modulator, then SOURCE LFO with the fields; C had the LFO from its note on
  // (shape, speed and amount are in the descriptor already: a SPEED edit on a running LFO keeps its phase, so only the SOURCE edit is made here)
  audio::FilterMod off = lfo(1, kRate8, 1.0f);
  off.kind = audio::kFmodNone;
  off.lfo = false;
  for (const Dest dest : {kWt, kAmp, kPan}) {
    edit_equals_note_on(dest, off, lfo(1, kRate8, 1.0f), ip::LfoSource, 2.0f, nullptr, "SOURCE Off -> LFO");
  }
  // LFO -> Off: the window returns to the instrument's at once, and stays
  VoicePool& p = pool(0);
  p.set_sample(0, desc_for(kWt, lfo(1, kRate8, 1.0f)));
  p.set_tempo_x100(kTempo);
  p.restart_clock();
  p.transport_stopped();
  REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
  Probe pr{kWt, 0};
  std::vector<double> v;
  for (unsigned b = 0; b < 200; ++b) {
    v.push_back(step_block(p, pr));
  }
  CHECK(v.back() >= 5.0);
  std::uint32_t pan = 0;
  CHECK(p.apply_param(0, ip::lfo(3, ip::LfoSource), 0.0f, pan));
  CHECK(p.sample(0).pos_mod.kind == audio::kFmodNone);
  CHECK(step_block(p, pr) == 4.0);
  CHECK(step_block(p, pr) == 4.0);
  // Off -> Envelope -> LFO again: the modulator restarts as at a note
  CHECK(p.apply_param(0, ip::lfo(3, ip::LfoSource), 2.0f, pan));
  CHECK(p.sample(0).pos_mod.kind == audio::kFmodLfo);
  std::vector<double> w;
  for (unsigned b = 0; b < 100; ++b) {
    w.push_back(step_block(p, pr));
  }
  CHECK(spread(w, 0) >= 2.0);
}

TEST_CASE("lfo live: an edit that changes nothing changes nothing (same values every block: bit for bit the voice that is not edited)") {
  for (const Dest dest : {kWt, kAmp, kPan, kGran, kTune, kCut}) {
    VoicePool& a = pool(0);
    VoicePool& c = pool(2);
    for (VoicePool* p : {&a, &c}) {
      p->set_sample(0, desc_for(dest, lfo(2, kRate8, 0.75f)));
      p->set_tempo_x100(kTempo);
      p->restart_clock();
      p->transport_stopped();
      REQ(p->note_on(0, 60, 0, audio::kVelocityDefault));
      p->mark_live();
    }
    Probe pa{dest, 0}, pc{dest, 0};
    unsigned same = 0;
    for (unsigned b = 0; b < kPeriodBlocks; ++b) {
      std::uint32_t pan = 0;
      const unsigned e = slot_of(dest);
      a.apply_param(0, ip::lfo(e, ip::LfoShape), 2.0f, pan);
      a.apply_param(0, ip::lfo(e, ip::LfoSpeed), static_cast<float>(kRate8), pan);
      a.apply_param(0, ip::lfo(e, ip::LfoAmount), 0.75f, pan);
      a.apply_param(0, ip::lfo(e, ip::LfoSource), 2.0f, pan);
      same += step_block(a, pa) == step_block(c, pc) ? 1u : 0u;
    }
    CHECK_MESSAGE(same == kPeriodBlocks, "dest " << static_cast<unsigned>(dest) << ": " << same << " of " << kPeriodBlocks);
  }
}

TEST_CASE("lfo live: the fields are held to range, a position slot that does not belong to the instrument ignores them, a NaN is no value") {
  SampleDesc d = desc_for(kAmp, lfo(1, kRate8, 1.0f));
  using audio::InstApply;
  CHECK(audio::apply_instrument_param(d, ip::lfo(0, ip::LfoSpeed), 99.0f, audio::LevelLaw::Og12) == InstApply::Applied);
  CHECK(d.amp_mod.rate == audio::kAmpLfoRates - 1u);  // the amp slot's table has 24
  CHECK(audio::apply_instrument_param(d, ip::lfo(1, ip::LfoSpeed), 99.0f, audio::LevelLaw::Og12) == InstApply::Applied);
  CHECK(d.pan_mod.rate == audio::kLfoRates - 1u);
  CHECK(audio::apply_instrument_param(d, ip::lfo(1, ip::LfoShape), 9.0f, audio::LevelLaw::Og12) == InstApply::Applied);
  CHECK(d.pan_mod.shape == 2u);  // a larger shape is a triangle
  CHECK(audio::apply_instrument_param(d, ip::lfo(1, ip::LfoAmount), 7.0f, audio::LevelLaw::Og12) == InstApply::Applied);
  CHECK(d.pan_mod.lfo_amount == 1.0f);
  CHECK(audio::apply_instrument_param(d, ip::lfo(3, ip::LfoAmount), 0.5f, audio::LevelLaw::Og12) == InstApply::Ignored);  // no wavetable here
  CHECK(audio::apply_instrument_param(d, ip::lfo(4, ip::LfoSource), 2.0f, audio::LevelLaw::Og12) == InstApply::Ignored);  // no granular instrument here
  CHECK(audio::apply_instrument_param(d, ip::lfo(5, 4), 1.0f, audio::LevelLaw::Og12) != InstApply::Applied);  // field 4 of a slot is the next slot's SOURCE: not ours to take twice
  std::uint32_t pan = 0;
  VoicePool& p = pool(0);
  p.set_sample(0, desc_for(kPan, lfo(1, kRate8, 1.0f)));
  CHECK_FALSE(p.apply_param(0, ip::lfo(1, ip::LfoAmount), std::nanf(""), pan));
  CHECK(p.apply_param(0, ip::lfo(1, ip::LfoAmount), 0.3f, pan));
  CHECK(p.sample(0).pan_mod.amount == doctest::Approx(0.3f));
}
