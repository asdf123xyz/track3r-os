// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The note a Pattern pad plays and writes for the current instrument (OS-SLICEFIX). A BEAT SLICE instrument (play mode 5) does not play a pitch: the note picks the slice,
// slice = note - audio::kSliceRootNote (24), so pad n must be note 24 + n, as the Wave / Chop screen's pads are (wave_actions.cpp). The pad board's notes (36 and up by default, or whatever the
// scale and the octave make of them) would all land on the last slice. The scale and the PIANO / ISO layouts do not apply to such an instrument. Every other instrument keeps the pad board's
// note, and so does a Pattern with no project.

#include <o3os/app/pad_board.h>
#include <o3os/app/play_mode.h>
#include <o3os/audio/voices.h>
#include <o3os/model/project.h>

namespace o3os::app {

// Is instrument number `instrument` (1..64, ViewState::instrument) a sample instrument in the beat slice play mode?
inline bool beat_slice_instrument(const model::Project* project, int instrument) noexcept {
  if (project == nullptr || instrument < 1) {
    return false;
  }
  const model::Instrument* in = project->instrument(static_cast<std::uint32_t>(instrument - 1));
  return in != nullptr && in->kind == model::InstrumentKind::Sample && in->sampler.play_mode == kPlayModeBeatSlice;
}

// The note (0..120) of pad 0..47 for that instrument: 24 + pad for a beat slice, else the board's.
inline unsigned pad_note(const model::Project* project, int instrument, const PadBoard& board, unsigned pad) noexcept {
  return beat_slice_instrument(project, instrument) ? audio::kSliceRootNote + pad : static_cast<unsigned>(board.note(pad));
}

}  // namespace o3os::app
