// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// MIDI controller input on the app side (OS-013a): which control changes the device settings MIDI IN and IN CHANNEL let through to MIDI Learn. The render parses and queues every
// control change of both ports (midi/cc_in.h); the main loop (App::poll_midi_cc) asks this before it runs `midi.cc`, from the settings themselves, so a change of the setting
// takes effect with the next message and nothing is copied to the render. NEW COMMUNITY OS BEHAVIOUR (the settings are OS-011a's).

namespace o3os::app {

// Control changes taken from the ring and handed to `midi.cc` in one pass of the main loop (the ring holds midi::CcInput::kRingSize; the rest waits for the next pass).
inline constexpr unsigned kMidiCcPerPass = 16;

// Note messages taken from the notes' ring in one pass of the main loop (OS-005g; the ring holds midi::CcInput::kNoteRingSize).
inline constexpr unsigned kMidiNotesPerPass = 16;

// `in_port` is the setting MIDI IN (0 DIN, 1 USB, 2 DIN + USB: the numbers of midi::Port for the first two), `in_channel` IN CHANNEL (0 every channel, else 1..16);
// `port` is the port a message arrived on and `channel` its channel 1..16.
constexpr bool midi_in_accepts(unsigned in_port, unsigned in_channel, unsigned port, unsigned channel) noexcept {
  return (in_port >= 2u || in_port == port) && (in_channel == 0u || in_channel == channel);
}

}  // namespace o3os::app
