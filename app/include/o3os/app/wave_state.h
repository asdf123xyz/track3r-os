// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/app/wave_math.h>

#include <cstdint>

namespace o3os::app {

// The Wave / Chop screen's state (OS-011a): the window on the sample (zoom), the point or slice being edited, the options, and the two hand-offs of the AUTO
// slicer (the action asks, the App's pass runs the job and leaves the answer here for the internal action wave.auto_apply, which is an undo unit). The sample and
// the instrument are the ones in view (ViewState::instrument).
struct WaveState {
  // The window: `span` frames from `first`; 0 is the whole sample (fit), which stays the whole sample when the sample changes.
  std::uint32_t first = 0;
  std::uint32_t span = 0;
  std::uint8_t point = 0;   // WAVE page: the point being edited (PointId)
  std::uint8_t slice = 0;   // CHOP page: the selected slice (0..count - 1)
  std::uint8_t even = 16;   // the number of slices EVEN makes (2..48)
  bool zc = true;           // the points and slices snap to a zero crossing when the encoder moves them
  bool auto_go = false;     // the AUTO key was pressed: the App starts the job
  bool auto_ready = false;  // the job is done: the slices wait (SampleBank::auto_slices, in SDRAM) wait for wave.auto_apply
  std::uint8_t auto_count = 0;
  std::uint8_t auto_slot = 0;  // the instrument slot the answer is for
  std::uint8_t mode_pick = 0;  // OS-INSTUI S4: the play mode list's cursor (play_mode.h 0..7)
};

}  // namespace o3os::app
