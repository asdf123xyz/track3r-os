// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The runtime side of the project-level state (OS-004d; model/project_state.h, docs/formats/o3-project.md "The project-level state") and of the session state (OS-PERSIST2, "The session state"):
// what the Mixer, the Perform screen, the SD apps, the MIDI link and the USB driver hold that a project now keeps, and the note-entry settings, the MUTE MODEs, the send effects' controls
// and the Perform scenes. The pieces here are pure (host-tested): the conversions between a runtime object and the model's plain state, and the little
// state machine that applies a project's USB mode after a load. The App wires the mixer, Perform and apps (App::sync_project_state, App::apply_project_state); the platform wires MIDI and USB
// (app_main.cpp) with these and seq::project_midi_state / apply_project_midi.
//
// Two directions, both main loop only:
//   RUNTIME -> PROJECT, whenever the runtime changed: Editor::set_*_state (not journalled, the undo history stays, the project becomes unsaved). Compared with what the project holds, so
//     nothing is written, and the project does not become unsaved, by a value that is already there.
//   PROJECT -> RUNTIME, when a project has been loaded (or a new one made): the mixer and Perform always (a project without the part sets the defaults, so the last project's routing does
//     not leak), MIDI and USB only when the project has the part. USB is a re-enumeration when the mode or the profile changes, so it is asked of the driver from the main loop, once, after the load.

#include <o3os/app/mix_board.h>
#include <o3os/app/pattern_edit.h>
#include <o3os/audio/stem_map.h>
#include <o3os/core/param.h>
#include <o3os/model/editor.h>
#include <o3os/model/project.h>
#include <o3os/model/project_state.h>
#include <o3os/usb/usb_types.h>

#include <cstdint>

namespace o3os::app {

class ModuleUi;

// ---- the mixer: the engine fields of the board (routing matrix, return routes, OUT mode, CUE and input levels)
model::MixState mix_state_of(const MixBoard& board) noexcept;
// Puts `s` in the board and tells the engine what changed, as ParamChange on `ring` (nullptr: the board only). Returns how many fields changed. Main loop (the ring's one producer).
unsigned mix_apply(MixBoard& board, const model::MixState& s, core::ParamRing* ring) noexcept;

// ---- the tracks' mute and solo (OS-MUTE), and the project's SENDS setting (OS-SENDFX)
// Tells the engine every track's mute and solo (and the project's SENDS setting, and the forgetting of the last project's effect tails and instrument sends: four ParamChanges more): after a load the graph's strips must hold the project's TrackSlot flags (the flag IS the Track parameter Mute / Solo; the sequencer no longer
// skips a muted track, the strip is the mute), and a project that sets none must clear the last project's. One ParamChange per track and flag on `ring` (nullptr: nothing), with the engine's own smoothing (ramp 0). Never an edit of the project: a loaded flag is already where it belongs, so the baseline of unsaved changes is unaffected. Returns how many were sent.
unsigned tracks_apply(const model::Project& project, core::ParamRing* ring) noexcept;

// ---- the session state (OS-PERSIST2; the Perform scenes are PerformBank::scenes_state / load_scenes)
// The note-entry settings (root, scale, octave, SCALE LOCK) and JUMP (the edit step) of the Pattern screen's session state. note_state_of() is what the runtime holds, held to the ranges of the
// format (a value out of them reads as its default, so what is stored is always valid); note_apply() puts `s` in and lays the pad board out again. True when something changed.
model::NoteState note_state_of(const PatternEdit& edit) noexcept;
bool note_apply(PatternEdit& edit, const model::NoteState& s) noexcept;
// Each track's MUTE MODE. mute_apply() returns how many changed; it tells nobody (the mode only says when a later toggle takes effect).
model::MuteState mute_state_of(const MixBoard& board) noexcept;
unsigned mute_apply(MixBoard& board, const model::MuteState& s) noexcept;
// The send effects' controls of the MASTER FX page. send_fx_apply() puts `s` in the board and tells the engine each value that changed, as the ParamChange of kind Master that the page itself
// sends (ramp 0: the effects' own smoothing), on `ring` (nullptr: the board only). Returns how many changed. Main loop (the ring's one producer).
model::SendFxState send_fx_state_of(const MixBoard& board) noexcept;
unsigned send_fx_apply(MixBoard& board, const model::SendFxState& s, core::ParamRing* ring) noexcept;

// ---- USB: the stem map of the stream and the descriptor set
model::UsbState usb_state_of(const audio::StemMap& map, usb::Mode mode) noexcept;
// Sets the slots of `map` from `s`; false (nothing changed) when `s` is not for the map's profile.
bool stem_map_apply(audio::StemMap& map, const model::UsbState& s) noexcept;

// OS-USBAUDIO: what a loaded project's USB part asks of the driver. The MODE is not the project's: the device setting USB AUDIO decided it at boot, and a project must not switch the unit to the
// other one behind the setting's back (Off would not persist, On would enumerate twice), nor undo a runtime `usb mode`; so the target keeps the mode the unit presents now (or is changing to:
// the platform passes `change_phase() != 0 ? pending_mode() : mode()`) and the project brings the profile and the stem map.
inline model::UsbState usb_apply_target(const model::UsbState& project, usb::Mode driver_mode) noexcept {
  model::UsbState s = project;
  s.mode = static_cast<std::uint8_t>(driver_mode);
  return s;
}

// Applies a project's USB part to the driver without ever blocking: the mode and profile first (a re-enumeration when they change: usb0::request_mode), then, once the new descriptor set
// is attached, the stem masks (a profile change resets the map to the factory slots, so the project's map has to come after it). Every pass of the main loop while pending():
//   switch (apply.step(usb0::mode(), usb0::profile(), usb0::change_phase())) {
//     case Request: if (!usb0::request_mode(mode, profile)) apply.abandon(); break;      // the driver is not running, or refused the profile
//     case Masks:   stem_map_apply(usb0::stem_map(), apply.target()); usb0::apply_stem_map(); break;
//     case None:    break;
//   }
// It asks once; a change that does not end in the project's mode and profile is given up (the runtime stays as the driver left it).
class UsbApply {
 public:
  enum class Act : std::uint8_t { None, Request, Masks };
  void begin(const model::UsbState& target) noexcept {
    target_ = target;
    pending_ = true;
    requested_ = false;
  }
  void abandon() noexcept { pending_ = false; }
  bool pending() const noexcept { return pending_; }
  const model::UsbState& target() const noexcept { return target_; }
  Act step(usb::Mode mode, usb::Profile profile, std::uint8_t change_phase) noexcept {
    if (!pending_ || change_phase != 0u) {
      return Act::None;  // nothing to do, or a mode change is running: wait for it
    }
    if (static_cast<std::uint8_t>(mode) == target_.mode && static_cast<std::uint8_t>(profile) == target_.profile) {
      pending_ = false;
      return Act::Masks;
    }
    if (requested_) {
      pending_ = false;  // asked once and the driver did not get there
      return Act::None;
    }
    requested_ = true;
    return Act::Request;
  }

 private:
  model::UsbState target_{};
  bool pending_ = false;
  bool requested_ = false;
};

// ---- SD apps: the parameter values of the running app
// Applies the project's remembered values (same name, same ABI major) to the app that has just been launched; returns how many it set. Without an entry nothing changes.
unsigned app_params_apply(ModuleUi& ui, const model::Project& project) noexcept;
// Stores the running app's values in the project when they differ from what it remembers (or, with no entry, from the module's defaults). True when it wrote.
bool app_params_sync(const ModuleUi& ui, model::Editor& editor) noexcept;

}  // namespace o3os::app
