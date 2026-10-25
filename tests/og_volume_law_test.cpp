// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-BUG-PASTE24 (docs/specs/OS-BUG-PASTE24.md): the tests of option C. The maintainer's decision of 2026-10-16: an import maps its volume 64 to level 50 (0 dB on the 1.2+ instrument law,
// smoothing.h THE LEVEL LAWS) and the levels below it follow the law's log scale, instead of the linear percent that put 64 at level 100 (+24 dB); and the Amiga engine reads `volume` through the
// same law, not linearly. They were should_fail cases until slices S3 and S4 (the import re-map and the Amiga reading) landed, and are plain cases now.
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/volume_law.h"

#include <o3os/audio/smoothing.h>
#include <o3os/audio/voices.h>
#include <o3os/formats/med/map.h>
#include <o3os/formats/mod/map.h>
#include <o3os/formats/mod/module.h>
#include <o3os/model/instrument.h>

#include <cmath>
#include <cstdlib>
#include <cstdint>


// The MOD / S3M import (formats/mod/src/map.cpp map_sample): SampleInfo::volume 0..64, the same rule.
TEST_CASE("OS-BUG-PASTE24 option C: a MOD / S3M sample volume 64 is level 50 (0 dB) and the rest follow the log law") {
  namespace mod = o3os::formats::mod;
  using o3os::test::check_import_level;
  for (unsigned volume = 0; volume <= 64; ++volume) {
    mod::SampleInfo info;
    info.kind = mod::SampleKind::Pcm;
    info.frames = 64;
    info.volume = static_cast<std::uint8_t>(volume);
    o3os::model::Instrument out{};
    o3os::formats::med::InstReport rep;
    std::uint16_t extra = 0;
    NEED(mod::map_sample(mod::Kind::Mod, 0, info, o3os::formats::med::Clock::Pal, 1u << 20, false, out, rep, extra));
    check_import_level(volume, out.sampler.volume);
  }
}

// The Amiga engine's volume 0..64 (SampleDesc::amiga_vol, audio/src/voices.cpp apply_instrument_param) was linear in the stored level (level 50 read 32). With `volume` a level on the law, the
// Amiga reading is the law's gain, held to Paula's full scale: level 50 and above read 64, level 38 (-5.8 dB) reads 33, and the engine agrees with the Modern one at every level. The tracker
// volume command's inverse (voices.cpp, `level_gain_q16((vol * 100 + 32) / 64, law_)`, OS-COREFX CF2) is the same conversion read the other way and moves with it.
TEST_CASE("OS-BUG-PASTE24 option C: the Amiga engine reads the stored volume through the law, not linearly") {
  using o3os::audio::InstApply;
  using o3os::audio::LevelLaw;
  namespace ip = o3os::audio::inst_param;
  for (unsigned level = 0; level <= 100; ++level) {
    CAPTURE(level);
    o3os::audio::SampleDesc d;
    NEED(o3os::audio::apply_instrument_param(d, ip::Volume, static_cast<float>(level), LevelLaw::Og12) == InstApply::Applied);
    const double gain = static_cast<double>(d.gain) / 65536.0;
    const double want = 64.0 * (gain > 1.0 ? 1.0 : gain);
    CHECK(std::fabs(static_cast<double>(d.amiga_vol) - want) <= 0.5 + 1e-9);
  }
}

// The helper the import cases go through, held by its own case: a failure of those cases is then the importers' (and the Amiga reading's), not the helper's.
TEST_CASE("OS-BUG-PASTE24 option C: the helper accepts exactly the wanted levels") {
  using o3os::test::import_level_ok;
  CHECK(import_level_ok(64, 50));
  CHECK_FALSE(import_level_ok(64, 100));  // the old linear percent
  CHECK_FALSE(import_level_ok(64, 49));
  CHECK(import_level_ok(0, 0));
  CHECK_FALSE(import_level_ok(0, 1));
  CHECK(import_level_ok(32, 37));  // -6.02 dB wanted, level 37 is -6.24 dB
  CHECK_FALSE(import_level_ok(32, 38));  // -5.76 dB is 0.26 dB off: outside half a step
  CHECK_FALSE(import_level_ok(32, 50));  // the old linear percent for half volume
  CHECK(import_level_ok(4, 1));    // -24.08 dB: the floor
  CHECK(import_level_ok(1, 1));
  CHECK_FALSE(import_level_ok(1, 2));
  CHECK_FALSE(import_level_ok(5, 1));  // -22.14 dB is above the floor
  CHECK(import_level_ok(5, 4));  // -22.14 dB wanted, level 4 is -22.08 dB
  // every volume has at least one accepted level, and the accepted level never decreases as the volume rises
  unsigned previous = 0;
  for (unsigned v = 0; v <= 64; ++v) {
    unsigned found = 999;
    for (unsigned level = 0; level <= 50; ++level) {
      if (import_level_ok(v, level)) {
        found = level;
        break;
      }
    }
    CAPTURE(v);
    CHECK(found != 999u);
    CHECK(found >= previous);
    previous = found;
  }
  // the law itself: 50 is exactly unity and a level is 0.48 dB
  CHECK(o3os::audio::level_gain_q16(50, o3os::audio::LevelLaw::Og12) == 65536);
  CHECK(std::fabs(o3os::test::db_of_level(51) - 0.48) < 0.001);
}

// What does not change with option C, true before and after: the Amiga reading of the extremes.
TEST_CASE("OS-BUG-PASTE24 option C: the Amiga volume of level 0 is 0 and of level 100 is the full 64") {
  using o3os::audio::InstApply;
  namespace ip = o3os::audio::inst_param;
  o3os::audio::SampleDesc d;
  CHECK(o3os::audio::apply_instrument_param(d, ip::Volume, 0.0f, o3os::audio::LevelLaw::Og12) == InstApply::Applied);
  CHECK(d.amiga_vol == 0u);
  CHECK(o3os::audio::apply_instrument_param(d, ip::Volume, 100.0f, o3os::audio::LevelLaw::Og12) == InstApply::Applied);
  CHECK(d.amiga_vol == 64u);
}

// The Amiga reading with the 1.1.14 table (our Og1114 law, used for an OG project below version 7 on the Amiga engine): the level is read through that law too (100 is 0 dB there, 50 is -20 dB), held to Paula's 64.
TEST_CASE("OS-BUG-PASTE24 option C: the Amiga engine reads the stored volume through the 1.1.14 table as well") {
  using o3os::audio::InstApply;
  using o3os::audio::LevelLaw;
  namespace ip = o3os::audio::inst_param;
  for (unsigned level = 0; level <= 100; ++level) {
    CAPTURE(level);
    o3os::audio::SampleDesc d;
    NEED(o3os::audio::apply_instrument_param(d, ip::Volume, static_cast<float>(level), LevelLaw::Og1114) == InstApply::Applied);
    const double gain = static_cast<double>(d.gain) / 65536.0;
    CHECK(std::fabs(static_cast<double>(d.amiga_vol) - 64.0 * (gain > 1.0 ? 1.0 : gain)) <= 0.5 + 1e-9);
  }
}

// What must hold before AND after the fix: an imported tracker module plays on the Amiga engine at the volume it was written at, to the quantisation of the law (half a step is 0.24 dB, one tracker
// step at volume 32 is 0.27 dB). Before option C the percent and the Amiga reading were both linear (exact to 1); now the level is on the log law and the reading goes through it.
TEST_CASE("OS-BUG-PASTE24 option C: an imported MOD volume reads on the Amiga engine within 1 of the volume it was written at (59 within 2, 1 and 2 at the floor 4)") {
  namespace mod = o3os::formats::mod;
  using o3os::audio::InstApply;
  namespace ip = o3os::audio::inst_param;
  for (unsigned volume = 0; volume <= 64; ++volume) {
    CAPTURE(volume);
    mod::SampleInfo info;
    info.kind = mod::SampleKind::Pcm;
    info.frames = 64;
    info.volume = static_cast<std::uint8_t>(volume);
    o3os::model::Instrument out{};
    o3os::formats::med::InstReport rep;
    std::uint16_t extra = 0;
    NEED(mod::map_sample(mod::Kind::Mod, 0, info, o3os::formats::med::Clock::Pal, 1u << 20, false, out, rep, extra));
    o3os::audio::SampleDesc d;
    NEED(o3os::audio::apply_instrument_param(d, ip::Volume, static_cast<float>(out.sampler.volume), o3os::audio::LevelLaw::Og12) == InstApply::Applied);
    if (volume == 1u || volume == 2u) {
      CHECK(d.amiga_vol == 4u);  // level 1 is the law's floor, -24 dB = 4 / 64: the volumes under it cannot be told apart (1 and 2 read 4; 3 reads 4, within 1)
    } else {
      // the law's step is 0.48 dB, wider than a tracker step near full scale: every volume reads within 1, except 59 (level 49, -0.48 dB, reads 61)
      CHECK(std::abs(static_cast<int>(d.amiga_vol) - static_cast<int>(volume)) <= (volume == 59u ? 2 : 1));
    }
  }
}
