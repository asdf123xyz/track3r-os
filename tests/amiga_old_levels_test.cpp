// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-BUG-PASTE24 slice S4 (docs/specs/OS-BUG-PASTE24.md sections 5 "D4" and 13): the fixture of the projects that an EARLIER build saved. The level is the only datum the file holds, so nothing converts them
// (no loader, none wanted: the maintainer accepted the numbers, Q2), and this test pins what changes for them, as a visible and deliberate line of the suite. It is the test behind the changelog entry
// of docs/CHANGELOG.md, "Instrument volume, imports and the Amiga engine".
//
//  - Project A, an IMPORT saved by an earlier build: instruments at the linear percents 100, 50, 25 and 0 (the importer's old rule, volume 64 -> 100). Loaded, the levels are untouched and the law is the
//    1.2+ law. The Modern engine plays 100 at +24 dB as stored. The Amiga engine now reads the level through the law: 64, 64, 16, 0 (it read 64, 32, 16, 0). Re-importing the module gives 50, 37, 25.
//  - Project B, native instruments that were never imports (levels 80 and 30), one note each with C40 and C20 on a note that names no velocity. The Modern engine plays level 80 at +14.4 dB and level 30
//    at -9.6 dB. The Cxx command changed its REFERENCE: it was "level 100 through the law" (+24 dB at C40 under the 1.2+ law, whatever the instrument's own level), it is now "unity" (C40 is 0 dB, C20
//    is -6 dB): both instruments now play 0 dB at C40, 24 dB lower than before. The Amiga engine reads 64 for level 80 and 21 for level 30.
// Built in code from this description: no binary in git.
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/project_fixture.h"
#include "support/static_slot.h"

#include <o3os/audio/events.h>
#include <o3os/audio/smoothing.h>
#include <o3os/audio/tracker_fx.h>
#include <o3os/audio/voices.h>
#include <o3os/formats/med/map.h>
#include <o3os/formats/mod/map.h>
#include <o3os/formats/mod/module.h>
#include <o3os/model/instrument.h>
#include <o3os/hal/audio.h>
#include <o3os/model/project.h>

#include <cmath>
#include <cstdint>
#include <vector>

namespace {

using o3os::audio::InstApply;
using o3os::audio::LevelLaw;
using o3os::audio::SampleDesc;
using o3os::audio::VoicePool;
namespace ip = o3os::audio::inst_param;
namespace haudio = o3os::hal::audio;

// The project an earlier build saved: sample instruments at `levels` (slots 0..), the law absent; written, then loaded into a document that held the 1.1.14 table before (the law must not leak).
void saved_and_loaded(const unsigned (&levels)[4], unsigned count, o3os::model::Project& loaded) {
  o3os::model::Project& src = o3os::test::arena(2).project;
  src.reset();
  for (unsigned i = 0; i < count; ++i) {
    o3os::model::Instrument inst{};
    o3os::model::init_instrument(inst, o3os::model::InstrumentKind::Sample);
    inst.sampler.volume = static_cast<std::uint8_t>(levels[i]);
    NEED(src.set_instrument(i, inst) == o3os::model::Status::Ok);
  }
  std::vector<std::uint8_t> bytes;
  NEED(o3os::test::save(src, bytes) == o3os::formats::native::Progress::Done);
  loaded.set_volume_law(1);
  NEED(o3os::test::load(o3os::test::arena(1), bytes) == o3os::formats::native::Progress::Done);
}

SampleDesc desc_of(unsigned level, LevelLaw law = LevelLaw::Og12) {
  SampleDesc d;
  CHECK(o3os::audio::apply_instrument_param(d, ip::Volume, static_cast<float>(level), law) == InstApply::Applied);
  return d;
}

double db_of(std::int32_t gain_q16) { return 20.0 * std::log10(static_cast<double>(gain_q16) / 65536.0); }

// 40 samples a period, amplitude 20000, looped over the whole sample.
std::int16_t g_sine[4000];
SampleDesc looped(unsigned level) {
  for (unsigned i = 0; i < 4000; ++i) {
    g_sine[i] = static_cast<std::int16_t>(std::lround(20000.0 * std::sin(2.0 * 3.14159265358979 * static_cast<double>(i) / 40.0)));
  }
  SampleDesc d = desc_of(level);
  d.data = g_sine;
  d.length = 4000;
  d.loop_start = 0;
  d.loop_end = 4000;
  return d;
}

// The peak of the voice on track 0 after `blocks` blocks (the gain ramp has landed by the last).
double peak_after(VoicePool& p, unsigned blocks = 16) {
  double peak = 0.0;
  for (unsigned b = 0; b < blocks; ++b) {
    p.begin_block();
    p.render(0, haudio::kBlock);
    p.end_block();
    const std::int16_t* f = p.feed(0);
    if (b + 4u >= blocks && f != nullptr) {
      for (unsigned i = 0; i < haudio::kBlock; ++i) {
        peak = std::fmax(peak, std::fabs(static_cast<double>(f[i])));
      }
    }
  }
  return peak;
}

// A note that names no velocity on an instrument of `level`, then Cxx `vol` (0..64) when it is not negative; the peak it plays.
double note_with_cxx(unsigned level, int vol) {
  static o3os::test::StaticSlot<VoicePool> slot;
  VoicePool& p = slot.make();
  p.set_law(LevelLaw::Og12);
  p.set_sample(0, looped(level));
  CHECK(p.note_on(0, 60, 0, o3os::audio::kVelocityDefault));
  if (vol >= 0) {
    p.tracker_set(0, VoicePool::kTrackerKeep, vol);
  }
  return peak_after(p);
}

}  // namespace

TEST_CASE("amiga old levels, project A: an import saved by an earlier build keeps its levels (no loader); the Amiga engine reads them through the law, the Modern engine plays 100 at +24 dB") {
  const unsigned levels[4] = {100, 50, 25, 0};  // the importer's linear percents of the module volumes 64, 32, 16, 0
  o3os::model::Project& loaded = o3os::test::arena(1).project;
  saved_and_loaded(levels, 4, loaded);
  CHECK(loaded.volume_law() == 0);  // the law absent in the file: the 1.2+ law
  for (unsigned i = 0; i < 4; ++i) {
    CAPTURE(i);
    const o3os::model::Instrument* inst = loaded.instrument(i);
    NEED(inst != nullptr);
    CHECK(inst->sampler.volume == levels[i]);  // untouched
  }
  // the Modern engine: as stored, level 100 is +24 dB
  CHECK(std::fabs(db_of(desc_of(100).gain) - 24.0) < 0.05);
  CHECK(desc_of(50).gain == 65536);
  // the Amiga engine reads the gain against unity, held to 64: 100 and 50 are full volume (the change: 50 read 32), 25 (-12 dB) reads round(64 x gain), 0 is 0
  const unsigned want_amiga[4] = {64, 64, static_cast<unsigned>(std::lround(64.0 * o3os::audio::level_gain_q16(25, LevelLaw::Og12) / 65536.0)), 0};
  CHECK(want_amiga[2] == 16);
  for (unsigned i = 0; i < 4; ++i) {
    CAPTURE(i);
    CHECK(desc_of(levels[i]).amiga_vol == want_amiga[i]);
  }
  // re-importing the module (volumes 64, 32, 16) gives levels 50, 37, 25: the changelog's advice is checked
  namespace mod = o3os::formats::mod;
  const unsigned volumes[3] = {64, 32, 16};
  const unsigned want_levels[3] = {50, 37, 25};
  for (unsigned i = 0; i < 3; ++i) {
    CAPTURE(volumes[i]);
    mod::SampleInfo info;
    info.kind = mod::SampleKind::Pcm;
    info.frames = 64;
    info.volume = static_cast<std::uint8_t>(volumes[i]);
    o3os::model::Instrument out{};
    o3os::formats::med::InstReport rep;
    std::uint16_t extra = 0;
    NEED(mod::map_sample(mod::Kind::Mod, 0, info, o3os::formats::med::Clock::Pal, 1u << 20, false, out, rep, extra));
    CHECK(out.sampler.volume == want_levels[i]);
    CHECK(std::abs(static_cast<int>(desc_of(out.sampler.volume).amiga_vol) - static_cast<int>(volumes[i])) <= 1);
  }
}

TEST_CASE("amiga old levels, project B: native instruments at levels 80 and 30: the Cxx command is vol / 64 of unity (it was level 100 through the law, +24 dB), the Amiga engine reads 64 and 21") {
  const unsigned levels[4] = {80, 30, 0, 0};
  o3os::model::Project& loaded = o3os::test::arena(1).project;
  saved_and_loaded(levels, 2, loaded);
  CHECK(loaded.volume_law() == 0);
  CHECK(loaded.instrument(0)->sampler.volume == 80);
  CHECK(loaded.instrument(1)->sampler.volume == 30);
  // the Modern engine plays the levels as stored: +14.4 dB and -9.6 dB (0.48 dB a step from 50)
  CHECK(std::fabs(db_of(desc_of(80).gain) - 14.4) < 0.05);
  CHECK(std::fabs(db_of(desc_of(30).gain) + 9.6) < 0.05);
  // a note on level 30 alone plays 20000 x 0.331; with C40 it plays unity (it played +24 dB: the reference was level 100 through the law), with C20 half of unity (-6 dB)
  const double own30 = 20000.0 * desc_of(30).gain / 65536.0;
  CHECK(std::fabs(note_with_cxx(30, -1) - own30) <= own30 * 0.02 + 20.0);
  CHECK(std::fabs(note_with_cxx(30, 64) - 20000.0) <= 400.0 + 20.0);
  CHECK(std::fabs(note_with_cxx(30, 32) - 10000.0) <= 200.0 + 20.0);
  // and level 80 (+14.4 dB on its own: the note alone clips) plays 0 dB at C40 and -6 dB at C20: the same as level 30, whatever the instrument's own level
  CHECK(std::fabs(note_with_cxx(80, 64) - 20000.0) <= 400.0 + 20.0);
  CHECK(std::fabs(note_with_cxx(80, 32) - 10000.0) <= 200.0 + 20.0);
  // the Amiga engine: level 80 is over unity, 64; level 30 (-9.6 dB, gain 0.331) reads round(64 x gain) = 21
  CHECK(desc_of(80).amiga_vol == 64);
  CHECK(desc_of(30).amiga_vol == static_cast<unsigned>(std::lround(64.0 * o3os::audio::level_gain_q16(30, LevelLaw::Og12) / 65536.0)));
  CHECK(desc_of(30).amiga_vol == 21);
}

// Above level 50 (over unity) the instrument's own tracker volume is held at Paula's 64, and the processor's volumes are vol / 64 of UNITY: the first tick that sets a volume drops the note to the unity
// scale (the changelog line). A slide down of 4 on a note of level 55 (+2.4 dB, peak 26,360) asks for 60 / 64 at its first tick and the note lands on 60 / 64 of unity (18,750), not 0.56 dB under its own level.
TEST_CASE("amiga old levels: a volume slide on an instrument above level 50 takes the note down to the unity scale at its first tick (up to 14 dB at level 80)") {
  static o3os::test::StaticSlot<VoicePool> slot;
  VoicePool& p = slot.make();
  o3os::audio::TrackerFx t;
  t.attach(nullptr);
  t.reset(p);
  p.set_law(LevelLaw::Og12);
  p.set_sample(0, looped(55));
  CHECK(p.note_on(0, 60, 0, o3os::audio::kVelocityDefault));
  const double own = 20000.0 * desc_of(55).gain / 65536.0;
  CHECK(std::fabs(peak_after(p, 8) - own) <= own * 0.02 + 20.0);
  o3os::audio::SeqEvent e{};
  e.kind = o3os::audio::EventKind::TrkFx;
  e.track = 0;
  e.a = static_cast<std::uint8_t>(o3os::audio::kFxTrkVolSlide);
  e.b = 0x04;  // down 4 a tick, from tick 1
  t.event(e, p);
  double heard[3] = {0.0, 0.0, 0.0};
  for (unsigned i = 0; i < 3; ++i) {
    o3os::audio::SeqEvent k{};
    k.kind = o3os::audio::EventKind::Tick;
    k.track = o3os::audio::kNoTrack;
    k.a = static_cast<std::uint8_t>(i);
    k.b = 6;
    t.event(k, p);
    heard[i] = peak_after(p, 8);
  }
  CHECK(std::fabs(heard[0] - own) <= own * 0.02 + 20.0);  // tick 0 does not slide
  CHECK(std::fabs(heard[1] - 20000.0 * 60.0 / 64.0) <= 400.0);
  CHECK(std::fabs(heard[2] - 20000.0 * 56.0 / 64.0) <= 400.0);
  CHECK(t.volume(0) == 56u);
}
