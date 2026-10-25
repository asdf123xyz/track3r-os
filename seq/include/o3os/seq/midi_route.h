// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Where and how a MIDI instrument's output goes (OS-INSTUI S8b + S9; NEW COMMUNITY OS BEHAVIOUR, docs/formats/INST-1.3-diff.md for the stored fields). Pure functions of an instrument, shared by the
// sequencer (the notes and FX of the steps) and the MIDI link (the notes of the pads), so that the two cannot disagree.
//
//   CHANNEL   Instrument::midi_channel 1..16, the wire's 0..15.
//   OUT       Instrument::midi_out: 0 the device's MIDI OUT port (Config), 1 DIN, 2 USB, 3 both; the numbers are the router's port bits (audio::kRoutePorts), so the value goes into an event's `flags` as it is.
//   OFFSET    Instrument::midi_offset 0..100 ms: the delay the router adds to everything this instrument sends (the event's `value`, microseconds). The delay is applied by MidiOut (the render side, one add to
//             the due time of an item it stages anyway), not by the sequencer: the sequencer only labels the event, so no queue of the sequencer's grows and nothing waits in a loop.
//   VELOCITY  Instrument::midi_velocity 1..127, the velocity of a note that names none (a pad, a step with no volume).
//   PB RANGE  Instrument::midi_pb_range 0..24 semitones, default 2 (the bend range of 2 semitones every receiver has until told otherwise; the OS sends no RPN): the Micro-tune FX (0..198, 99 = in tune, +-99 cents) is sent as a pitch bend of cents / (100 x range) of the full travel; range 0 sends no bend.
//   CC A..F   Instrument::midi_cc[]: 0 the Config page's number for the slot; 1..128 controller 0..127; then PROG, channel aftertouch and polyphonic aftertouch (cc_target).

#include <o3os/audio/events.h>
#include <o3os/model/instrument.h>

#include <cstdint>

namespace o3os::seq {

struct MidiRoute {
  std::uint8_t ch;         // 0..15
  std::uint8_t ports;      // audio::kRoutePorts bits, 0: the device's MIDI OUT
  std::uint8_t delay_ms;   // 0..100
};

constexpr MidiRoute midi_route(const model::Instrument& in) noexcept {
  return MidiRoute{static_cast<std::uint8_t>(in.midi_channel > 0u ? (in.midi_channel - 1u) & 0x0Fu : 0u), static_cast<std::uint8_t>(in.midi_out & audio::kRoutePorts),
                   in.midi_offset > model::kMidiOffsetMaxMs ? model::kMidiOffsetMaxMs : in.midi_offset};
}
constexpr std::int32_t route_delay_us(const MidiRoute& r) noexcept { return static_cast<std::int32_t>(r.delay_ms) * 1000; }

constexpr std::uint8_t midi_velocity_of(const model::Instrument& in) noexcept {
  return in.midi_velocity < 1u ? std::uint8_t{model::kMidiVelocityDefault} : (in.midi_velocity > 127u ? std::uint8_t{127} : in.midi_velocity);
}

inline constexpr std::uint16_t kBendCentre = 8192;

// The Micro-tune FX value (0..198, 99 in tune) as cents, -99..+99.
constexpr int microtune_cents(std::uint8_t value) noexcept { return static_cast<int>(value > 198u ? 198u : value) - 99; }

// A pitch bend value 0..16383 (8192 the centre) for `cents` when the receiver bends `range` semitones each way: round(cents x 8192 / (100 x range)), held to the travel; no bend for a range of 0.
constexpr std::uint16_t pitch_bend_value(int cents, unsigned range) noexcept {
  if (range == 0u || cents == 0) {
    return kBendCentre;
  }
  const long n = static_cast<long>(cents) * 8192L;
  const long d = 100L * static_cast<long>(range);
  const long q = n >= 0 ? (n + d / 2) / d : -((-n + d / 2) / d);
  const long v = static_cast<long>(kBendCentre) + q;
  return static_cast<std::uint16_t>(v < 0 ? 0 : (v > 16383 ? 16383 : v));
}

// What a CC slot of an instrument sends.
enum class CcKind : std::uint8_t { Config, Controller, Program, ChannelAt, PolyAt };
struct CcTarget {
  CcKind kind;
  std::uint8_t number;  // Controller: 0..127
};
constexpr CcTarget cc_target(std::uint8_t stored) noexcept {
  if (stored == model::kMidiCcConfig || stored > model::kMidiCcMax) {
    return CcTarget{CcKind::Config, 0};
  }
  if (stored <= model::kMidiCcNumberMax) {
    return CcTarget{CcKind::Controller, static_cast<std::uint8_t>(stored - 1u)};
  }
  return CcTarget{stored == model::kMidiCcProg ? CcKind::Program : (stored == model::kMidiCcChanAt ? CcKind::ChannelAt : CcKind::PolyAt), 0};
}

}  // namespace o3os::seq
