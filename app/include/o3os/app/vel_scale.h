// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-INSTUI S9, VEL> (INST 1.3; NEW COMMUNITY OS BEHAVIOUR): how much the velocity of a note from the MIDI keyboard shapes a SAMPLE instrument's level. A pad has no velocity (its note is the instrument's own
// level, audio::kLiveVelocityDefault) and is not touched by this.
//
// The voices play a note that names a velocity at velocity / 127 of unity (audio/voices.h), and that is the law the keyboard has always had. VEL> is a continuous blend between that law and no velocity at all:
//
//     played = 127 - VEL> / 100 x (127 - velocity)        VEL> 0..100 %, velocity 1..127, the result 1..127 (rounded to nearest)
//
//     VEL> 100  (the default)  played = velocity: exactly the law of every earlier build, so an old project and a new instrument sound as they did
//     VEL> 0                   played = 127: the velocity is ignored, every note plays as a velocity of 127 does
//     VEL> 1..99               linear between the two (VEL> 50 on velocity 64 is 95.5, rounded to 96)
//
// The range of the field is 0..100 (a percentage of the velocity's effect), so there is no extension above 100; a value above 100 given to this function is held to 100, and a velocity out of 1..127 is held
// to its range, so the result is never below 1 (no silent note, no negative gain) nor above 127. The scaling is made here, in the main loop, before the note is queued: the engine's live path (hot code)
// keeps its one law.

#include <cstdint>

namespace o3os::app {

// The velocity to queue, 1..127.
constexpr unsigned vel_scaled_velocity(unsigned velocity, unsigned sens) noexcept {
  const unsigned v = velocity < 1u ? 1u : (velocity > 127u ? 127u : velocity);
  const unsigned a = sens > 100u ? 100u : sens;
  const unsigned x100 = 12700u - a * (127u - v);  // 100 x the played velocity: an integer, at least 100 x v
  return (x100 + 50u) / 100u;
}

}  // namespace o3os::app
