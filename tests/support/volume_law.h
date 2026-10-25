// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-BUG-PASTE24 (docs/specs/OS-BUG-PASTE24.md): what the import tests of option C assert, shared by og_volume_law_test.cpp and med_map_test.cpp.

#include <doctest/doctest.h>

#include <o3os/audio/smoothing.h>

#include <cmath>
#include <cstdint>

namespace o3os::test {

// 20 log10 of the Og12 gain of `level`, in dB (the law's own definition: 0.48 dB a step, 50 = 0 dB).
inline double db_of_level(unsigned level) {
  return 20.0 * std::log10(static_cast<double>(audio::level_gain_q16(level, audio::LevelLaw::Og12)) / 65536.0);
}

// Whether `level` is what option C wants for a tracker volume 0..64 on the law: 64 is 0 dB (level 50), 0 is silence, in between the level whose gain is within half a step (0.25 dB) of v / 64
// (levels 1..49), the floor level 1 (-24 dB) for the smallest volumes (those under -24 dB: 1 to 4). Plain logic, so that a plain case can hold the helper itself (a should_fail case that goes through
// it cannot tell a wrong helper from a missing fix).
inline bool import_level_ok(unsigned tracker_volume, unsigned level) {
  if (tracker_volume == 0u) {
    return level == 0u;
  }
  if (tracker_volume >= 64u) {
    return level == 50u;
  }
  if (level < 1u || level > 50u) {
    return false;
  }
  const double want_db = 20.0 * std::log10(static_cast<double>(tracker_volume) / 64.0);
  return want_db > -24.0 ? std::fabs(db_of_level(level) - want_db) <= 0.25 : level == 1u;
}

inline void check_import_level(unsigned tracker_volume, unsigned level) {
  CAPTURE(tracker_volume);
  CAPTURE(level);
  CHECK(import_level_ok(tracker_volume, level));
}

}  // namespace o3os::test
