// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "support/mmd_fixture.h"
#include "support/need.h"

#include <o3os/formats/med/mmd.h>
#include <o3os/formats/med/tempo.h>

#include <cmath>
#include <cstdint>

using namespace o3os;
using namespace o3os::formats::med;

// MED timing on the 192 PPQN grid (OS-008a slice M1, docs/formats/med.md 3.5): the worked examples of the spec, the
// 8-channel table, the SoundTracker-compatible tempos, mixing, the clamps and the divergence from libxmp and OpenMPT.

namespace {

TempoModel speed(Clock c = Clock::Pal) {
  TempoModel m;
  m.mode = TempoMode::Speed;
  m.clock = c;
  return m;
}
TempoModel bpm(unsigned lpb) {
  TempoModel m;
  m.mode = TempoMode::Bpm;
  m.lines_per_beat = static_cast<std::uint8_t>(lpb);
  return m;
}
TempoModel eight() {
  TempoModel m;
  m.mode = TempoMode::EightChannel;
  return m;
}

// The same arithmetic in double, written from the table of the spec rather than from the code under test.
double ideal_bpm(const TempoModel& m, unsigned tempo, unsigned spd) {
  double t_tick;
  if (m.mode == TempoMode::EightChannel) {
    static const unsigned bpm8[10] = {179, 164, 152, 141, 131, 123, 116, 110, 104, 99};
    t_tick = 2.5 / bpm8[(tempo > 10 ? 10 : tempo) - 1];
  } else if (m.mode == TempoMode::Bpm) {
    t_tick = 10.0 / (static_cast<double>(tempo) * m.lines_per_beat);
  } else if (m.mixing && tempo < 8) {
    t_tick = 2.5 / 157.86;
  } else if (tempo <= 10 && !m.mixing) {
    t_tick = tempo / 293.7;
  } else {
    const double cia = m.clock == Clock::Ntsc ? 715909.0 : 709379.0;
    t_tick = 474326.0 / (cia * tempo);
  }
  return 15.0 / (spd * t_tick);
}

}  // namespace

TEST_CASE("med tempo: the worked examples of the spec") {
  // speed mode 33, SPD 6, PAL: T = 474,326 / (709,379 x 33) s, line = 6 T, 123.38 BPM (the spec's 123.39 is rounded up; the
  // exact value is 123.383); NTSC 124.52 (spec 124.51); the 0.264 convention of libxmp and OpenMPT gives 125.00
  Bpm b = tempo_to_bpm(speed(), 33, 6);
  CHECK(b.x100 == 12338);
  CHECK(b.clamp == Clamp::None);
  CHECK(tempo_to_bpm(speed(Clock::Ntsc), 33, 6).x100 == 12452);
  CHECK(std::fabs(33 / 0.264 - 125.0) < 1e-9);
  // BPM mode: 125 BPM, 4 lines per beat, SPD 6: 125.00; 120 BPM, 8 lines per beat, SPD 3: 480
  CHECK(tempo_to_bpm(bpm(4), 125, 6).x100 == 12500);
  CHECK(tempo_to_bpm(bpm(8), 120, 3).x100 == 48000);
  // 8 channels, tempo 4, SPD 6: 141.0
  CHECK(tempo_to_bpm(eight(), 4, 6).x100 == 14100);
  // out of range: SPD 1, 240 BPM, 4 lines per beat is 1,440 BPM and clamps to 800
  b = tempo_to_bpm(bpm(4), 240, 1);
  CHECK(b.x100 == 80000);
  CHECK(b.clamp == Clamp::High);
}

TEST_CASE("med tempo: the 8-channel table, tempos above 10 count as 10") {
  const unsigned bpm8[10] = {179, 164, 152, 141, 131, 123, 116, 110, 104, 99};
  for (unsigned t = 1; t <= 10; ++t) {
    CAPTURE(t);
    // at SPD 6 the line is 6 ticks of 2.5 / BPM8 s = 15 / BPM8 s, so our BPM is BPM8 itself
    CHECK(tempo_to_bpm(eight(), t, 6).x100 == bpm8[t - 1] * 100);
  }
  CHECK(tempo_to_bpm(eight(), 11, 6).x100 == 9900);
  CHECK(tempo_to_bpm(eight(), 240, 6).x100 == 9900);
  CHECK(tempo_to_bpm(eight(), 1, 3).x100 == 35800);
  CHECK(tempo_to_bpm(eight(), 1, 12).x100 == 8950);
}

TEST_CASE("med tempo: speed mode tempos 1 to 10 are the SoundTracker-compatible ones") {
  for (unsigned t = 1; t <= 10; ++t) {
    CAPTURE(t);
    const Bpm b = tempo_to_bpm(speed(), t, 6);
    // T = tempo / 293.7 s: 6 ticks of it is a line of 6 t / 293.7 s
    CHECK(b.clamp == Clamp::None);
    CHECK(std::fabs(b.x100 / 100.0 - 15.0 / (6.0 * t / 293.7)) < 0.006);
  }
  CHECK(tempo_to_bpm(speed(), 1, 6).x100 == 73425);  // 15 x 293.7 / 6 = 734.25 BPM
  CHECK(tempo_to_bpm(speed(), 10, 6).x100 == 7343);   // 440,550 / 60 = 7342.5, rounded up
  // the compatibility tempos are fast ticks: tempo 10 is 34 ms per tick, tempo 11 (the CIA formula) is 61 ms
  CHECK(tempo_to_bpm(speed(), 10, 6).x100 > tempo_to_bpm(speed(), 11, 6).x100);
  CHECK(tempo_to_bpm(speed(), 11, 6).x100 == 4113);  // 1500 x 709,379 x 11 / (474,326 x 6) = 4112.8
}

TEST_CASE("med tempo: mixing songs play tempos below 8 at a fixed 157.86") {
  TempoModel m = speed();
  m.mixing = true;
  CHECK(tempo_to_bpm(m, 1, 6).x100 == 15786);   // BPM_ours = 15 x 157.86 / 2.5 / SPD
  CHECK(tempo_to_bpm(m, 7, 6).x100 == 15786);
  CHECK(tempo_to_bpm(m, 7, 3).x100 == 31572);
  // 8 to 10: the CIA formula, not the compatibility tempos (22,433 against 44,055 at SPD 1)
  CHECK(tempo_to_bpm(m, 10, 1).x100 == 22433);
  CHECK(tempo_to_bpm(speed(), 10, 1).x100 == 44055);
  // 8CHANNEL wins over mixing, BPM mode over nothing
  TempoModel e = eight();
  e.mixing = true;
  CHECK(tempo_to_bpm(e, 4, 6).x100 == 14100);
}

TEST_CASE("med tempo: clamps and degenerate input never divide by zero") {
  Bpm b = tempo_to_bpm(speed(), 0, 6);
  CHECK(b.x100 == kMinBpmX100);
  CHECK(b.clamp == Clamp::BadInput);
  b = tempo_to_bpm(speed(), 33, 0);   // SPD 0 is taken as 1
  CHECK(b.clamp == Clamp::BadInput);
  CHECK(b.x100 == tempo_to_bpm(speed(), 33, 1).x100);
  b = tempo_to_bpm(bpm(4), 0, 0);
  CHECK(b.clamp == Clamp::BadInput);
  CHECK(b.x100 == kMinBpmX100);
  b = tempo_to_bpm(eight(), 0, 6);
  CHECK(b.clamp == Clamp::BadInput);
  // too slow: speed mode 33 at SPD 32 is 23 BPM
  b = tempo_to_bpm(speed(), 33, 32);
  CHECK(b.clamp == Clamp::Low);
  CHECK(b.x100 == kMinBpmX100);
  // too fast
  b = tempo_to_bpm(speed(), 0xFFFF, 1);
  CHECK(b.clamp == Clamp::High);
  CHECK(b.x100 == kMaxBpmX100);
  // huge lines per beat, huge tempo: no overflow
  TempoModel m = bpm(255);
  b = tempo_to_bpm(m, 0xFFFF, 1);
  CHECK(b.clamp == Clamp::High);
  b = tempo_to_bpm(speed(Clock::Ntsc), 0xFFFFFFFFu, 1);
  CHECK(b.clamp == Clamp::High);
  // the model's own limits are the clamp's limits
  CHECK(kMinBpmX100 == 4000);
  CHECK(kMaxBpmX100 == 80000);
}

TEST_CASE("med tempo: every tempo and speed agrees with the formula in double, within a rounding step") {
  const TempoModel models[] = {speed(), speed(Clock::Ntsc), bpm(1), bpm(4), bpm(8), bpm(32), eight(), [] {
                                 TempoModel m = speed();
                                 m.mixing = true;
                                 return m;
                               }()};
  unsigned checked = 0;
  for (const TempoModel& m : models) {
    for (unsigned tempo = 1; tempo <= 255; ++tempo) {
      for (unsigned spd = 1; spd <= 32; ++spd) {
        const Bpm b = tempo_to_bpm(m, tempo, spd);
        const double ideal = ideal_bpm(m, tempo, spd);
        CAPTURE(tempo);
        CAPTURE(spd);
        CAPTURE(ideal);
        if (b.clamp == Clamp::None) {
          CHECK(std::fabs(b.x100 / 100.0 - ideal) <= 0.0051);
          ++checked;
        } else if (b.clamp == Clamp::Low) {
          CHECK(ideal < 40.0 + 0.0051);
          CHECK(b.x100 == kMinBpmX100);
        } else {
          CHECK(b.clamp == Clamp::High);
          CHECK(ideal > 800.0 - 0.0051);
          CHECK(b.x100 == kMaxBpmX100);
        }
        CHECK(b.x100 >= kMinBpmX100);
        CHECK(b.x100 <= kMaxBpmX100);
      }
    }
  }
  CHECK(checked > 20000);
}

TEST_CASE("med tempo: more speed is slower, more tempo is faster, the beat length scales BPM mode") {
  for (unsigned t = 11; t < 240; ++t) {
    CHECK(tempo_to_bpm(speed(), t + 1, 6).x100 >= tempo_to_bpm(speed(), t, 6).x100);
  }
  for (unsigned s = 1; s < 32; ++s) {
    CHECK(tempo_to_bpm(speed(), 33, s + 1).x100 <= tempo_to_bpm(speed(), 33, s).x100);
  }
  CHECK(tempo_to_bpm(bpm(8), 100, 6).x100 == 2 * tempo_to_bpm(bpm(4), 100, 6).x100);
}

TEST_CASE("med tempo: libxmp and OpenMPT time speed mode as 0.66 / tempo, the CIA constants are 1.3 percent slower") {
  // libxmp: tick = 2.64 x 250 / tempo ms = 0.66 / tempo s; OpenMPT: BPM (MOD) = tempo / 0.264, tick = 2.5 / BPM. Both are
  // 0.66 / tempo. Ours is 474,326 / 709,379 = 0.66863 / tempo (PAL).
  for (unsigned t = 20; t <= 200; t += 7) {
    CAPTURE(t);
    const double libxmp_bpm = 15.0 / (6.0 * 0.66 / t);
    const double ours = tempo_to_bpm(speed(), t, 6).x100 / 100.0;
    CHECK(ours < libxmp_bpm);
    CHECK(ours > libxmp_bpm * 0.985);
    CHECK(ours / libxmp_bpm == doctest::Approx(0.66 / 0.668633).epsilon(0.001));
  }
  // the NTSC clock is 0.9 % faster than PAL
  CHECK(tempo_to_bpm(speed(Clock::Ntsc), 100, 6).x100 > tempo_to_bpm(speed(), 100, 6).x100);
  // BPM mode: OpenMPT's BPM x lines per beat / 4 (MOD BPM) with ticks of 2.5 / BPM_MOD s is T = 10 / (BPM x lpb): the same
  const double openmpt = 15.0 / (6.0 * 2.5 / (125.0 * 4 / 4.0));
  CHECK(tempo_to_bpm(bpm(4), 125, 6).x100 / 100.0 == doctest::Approx(openmpt).epsilon(0.0001));
}

TEST_CASE("med tempo: tempo_model reads the song's flags, 8CHANNEL wins over BPM mode") {
  using test::mmd::ModuleSpec;
  ModuleSpec spec = test::mmd::sample_module(1);
  spec.flags = 0;
  spec.flags2 = 0;
  auto model_of = [](const ModuleSpec& s, Clock c) {
    const test::mmd::Bytes bytes = test::mmd::build(s);
    SpanSource src(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
    Reader r;
    CHECK(r.open(src).ok());
    return tempo_model(r.song(), c);
  };
  TempoModel m = model_of(spec, Clock::Pal);
  CHECK(m.mode == TempoMode::Speed);
  CHECK(!m.mixing);
  CHECK(m.lines_per_beat == 4);
  spec.flags2 = 0x20 | 7;
  m = model_of(spec, Clock::Ntsc);
  CHECK(m.mode == TempoMode::Bpm);
  CHECK(m.lines_per_beat == 8);
  CHECK(m.clock == Clock::Ntsc);
  spec.flags = 0x40;
  m = model_of(spec, Clock::Pal);
  CHECK(m.mode == TempoMode::EightChannel);
  spec.flags = 0;
  spec.flags2 = 0x80;
  m = model_of(spec, Clock::Pal);
  CHECK(m.mixing);
  CHECK(m.mode == TempoMode::Speed);
  // a BPM-mode tempo below 7 is flagged as one OctaMED 5 does not play as written
  CHECK(tempo_is_suspect(bpm(4), 6));
  CHECK(!tempo_is_suspect(bpm(4), 7));
  CHECK(!tempo_is_suspect(speed(), 3));
}
