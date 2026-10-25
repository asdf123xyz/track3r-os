// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSTUI S8b + S9 on the wire: the stored fields of a MIDI instrument (INST 1.3) as the two ports see them, through the real engine, MidiLink, MidiOut, the DIN emitter and the USB ring of the
// S2b rig (support/live_midi_rig.h): Program Change on an edit (the ParamChange the app sends) and before the first note after a load, OUT routing of notes and CCs, the Micro-tune FX as a pitch
// bend, CC slots as controller / Program Change / aftertouch, OFFSET as a delay in cycles on both ports, and O2: no note hangs when OUT, the channel or OFFSET change under a held pad or a
// sounding step, nor under a stop. VEL> (S9) is the app's: app/vel_scale.h and the keyboard path of pattern_edit_app_test.cpp. Host only (the rig keeps vectors).
#include <doctest/doctest.h>

#include <o3os/app/vel_scale.h>
#include <o3os/audio/live_notes.h>
#include <o3os/model/instrument.h>
#include <o3os/seq/fx.h>

#include "support/live_midi_rig.h"
#include "support/need.h"
#include "support/seq_fixture.h"

#include <cstdint>
#include <vector>

using namespace o3os;
using namespace o3os::test;
using audio::LiveEvent;
using audio::LiveKind;
namespace fx = o3os::seq::fx;

namespace {

LiveMidiRig& rig() {
  static LiveMidiRig r;
  return r;
}

LiveEvent pad_on(std::uint8_t pad, std::uint8_t note, std::uint8_t instrument) {
  return LiveEvent{LiveKind::On, audio::kLiveSourcePad, pad, note, instrument, audio::kLiveVelocityDefault, 0, 0};
}
LiveEvent pad_off(std::uint8_t pad) { return LiveEvent{LiveKind::Off, audio::kLiveSourcePad, pad, 0, 0, 0, 0, 0}; }
void push(const LiveEvent& e) { CHECK(rig().box().queue.push(e)); }

// Instruments (1-based, as a pad names them): 1..2 samples; 11 MIDI channel 3, 12 channel 1, 13 channel 16.
void midi_slot(std::uint32_t slot, std::uint8_t channel, void (*fn)(model::Instrument&) = nullptr) {
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Midi);
  in.midi_channel = channel;
  if (fn != nullptr) {
    fn(in);
  }
  CHECK(seq_project().set_instrument(slot, in) == model::Status::Ok);
}
void edit(std::uint32_t slot, void (*fn)(model::Instrument&)) {
  model::Instrument in = *seq_project().instrument(slot);
  fn(in);
  CHECK(seq_project().set_instrument(slot, in) == model::Status::Ok);
}

void setup(std::uint8_t ports = midi::kPortDin | midi::kPortUsb) {
  fresh_project(2, 2);
  midi_slot(11, 1);
  midi_slot(12, 16);
  rig().init(seq_project(), ports);
}

void tell_program(unsigned slot) {
  CHECK(rig().engine().params().push(core::ParamChange{core::param_id(core::ParamKind::Project, 0, seq::seq_param::MidiProgram), static_cast<float>(slot), 0u}));
}

// The messages of one port, with a status of `hi` (0xC0 program change, 0xB0 control, 0xE0 bend, 0xD0 channel pressure, 0xA0 poly pressure, 0x90 / 0x80 notes) on channel `ch`.
std::vector<WireMsg> of(std::uint8_t port, std::uint8_t hi, int ch = -1) {
  std::vector<WireMsg> r;
  for (const WireMsg& m : rig().msgs_of(port)) {
    if ((m.b[0] & 0xF0u) == hi && (ch < 0 || (m.b[0] & 0x0Fu) == static_cast<unsigned>(ch))) {
      r.push_back(m);
    }
  }
  return r;
}

// A pattern of one MIDI track for the sequencer: id 1, `steps` long, plays on a 120 BPM clock (a step is 5,515 samples, 86 blocks).
void pattern(unsigned steps = 16) {
  CHECK(seq_project().set_tempo_x100(12000) == model::Status::Ok);
  CHECK(seq_project().create_pattern(1, steps) == model::Status::Ok);
}
void play_blocks(unsigned n) {
  rig().sequencer().play_pattern(1);
  rig().blocks(n);
}

}  // namespace

// ---- PROGRAM ---------------------------------------------------------------------------------------------------------------------------

TEST_CASE("midi fields: an edit of PROGRAM sends a Program Change on the channel, to both ports; a program of none sends nothing") {
  setup();
  edit(11, [](model::Instrument& i) { i.midi_program = 6; });  // program 5 on channel 1 (wire 0)
  tell_program(11);
  rig().settle();
  for (const std::uint8_t port : {std::uint8_t{0}, std::uint8_t{1}}) {
    const std::vector<WireMsg> pc = of(port, 0xC0);
    NEED(pc.size() == 1u);
    CHECK(pc[0].b[0] == 0xC0);
    CHECK(pc[0].b[1] == 5);
    CHECK(pc[0].len == 2);
  }
  rig().clear_wire();
  edit(11, [](model::Instrument& i) { i.midi_program = 128; });
  tell_program(11);
  rig().settle();
  NEED(of(0, 0xC0).size() == 1u);
  CHECK(of(0, 0xC0)[0].b[1] == 127);
  rig().clear_wire();
  edit(11, [](model::Instrument& i) { i.midi_program = 0; });
  tell_program(11);
  rig().settle();
  CHECK(rig().msgs().empty());
  // a sample instrument's slot, or a free one, sends nothing
  tell_program(0);
  tell_program(30);
  rig().settle();
  CHECK(rig().msgs().empty());
}

TEST_CASE("midi fields: the first note after a project load is preceded by the program; a second note of the instrument is not; another instrument on the channel changes it") {
  setup();
  midi_slot(10, 3, [](model::Instrument& i) { i.midi_program = 4; });  // slot 10 = instrument 11: channel 3, program 3
  push(pad_on(1, 60, 11));
  rig().settle();
  NEED(of(0, 0xC0).size() == 1u);
  CHECK((of(0, 0xC0)[0].b[0] == 0xC2 && of(0, 0xC0)[0].b[1] == 3));
  // the program change is on the wire before the note (each port keeps its own order)
  for (const std::uint8_t port : {std::uint8_t{0}, std::uint8_t{1}}) {
    const std::vector<WireMsg> all = rig().msgs_of(port);
    NEED(all.size() >= 2u);
    CHECK(all[0].b[0] == 0xC2);
    CHECK(all[1].b[0] == 0x92);
  }
  push(pad_off(1));
  push(pad_on(2, 62, 11));
  rig().settle();
  CHECK(of(0, 0xC0).size() == 1u);  // the receiver has it: no second program change
  push(pad_off(2));
  rig().settle();
  // another instrument takes the channel's program, and the first one puts its own back
  midi_slot(11, 3, [](model::Instrument& i) { i.midi_program = 9; });  // instrument 12: channel 3 as well
  push(pad_on(3, 64, 12));
  rig().settle();
  push(pad_off(3));
  push(pad_on(4, 65, 11));
  rig().settle();
  push(pad_off(4));
  rig().settle();
  const std::vector<WireMsg> pcs = of(0, 0xC0);
  NEED(pcs.size() == 3u);
  CHECK(pcs[1].b[1] == 8);
  CHECK(pcs[2].b[1] == 3);
  // a project load (reset(), the same instruments back): the first note puts the receiver on the program again
  rig().clear_wire();
  seq_project().reset();
  midi_slot(10, 3, [](model::Instrument& i) { i.midi_program = 4; });
  push(pad_on(5, 67, 11));
  rig().settle();
  push(pad_off(5));
  rig().settle();
  NEED(of(0, 0xC0).size() == 1u);
  CHECK(of(0, 0xC0)[0].b[1] == 3);
  CHECK(LiveMidiRig::balanced(rig().notes()));
}

// ---- OUT ----------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("midi fields: OUT routes the notes of a pad: DIN, USB, both, or the device's port") {
  const struct {
    std::uint8_t out;
    std::uint8_t device_ports;
    unsigned din;
    unsigned usb;
  } cases[] = {{model::kMidiOutDin, midi::kPortUsb, 2, 0}, {model::kMidiOutUsb, midi::kPortDin, 0, 2}, {model::kMidiOutBoth, midi::kPortDin, 2, 2}, {model::kMidiOutConfig, midi::kPortUsb, 0, 2},
               {model::kMidiOutConfig, midi::kPortDin, 2, 0}, {model::kMidiOutConfig, midi::kPortDin | midi::kPortUsb, 2, 2}};
  for (const auto& c : cases) {
    setup(c.device_ports);
    static std::uint8_t out;
    out = c.out;
    edit(10, [](model::Instrument& i) { i.midi_out = out; });
    push(pad_on(1, 60, 11));
    rig().blocks(2);
    push(pad_off(1));
    rig().settle();
    CHECK_MESSAGE(of(0, 0x90).size() + of(0, 0x80).size() == c.din, "out " << int(c.out) << " device " << int(c.device_ports));
    CHECK_MESSAGE(of(1, 0x90).size() + of(1, 0x80).size() == c.usb, "out " << int(c.out) << " device " << int(c.device_ports));
    CHECK(rig().link().out().book().owed() == 0u);
  }
}

TEST_CASE("midi fields: OUT routes the CCs of a step, and the instrument's controller names the number") {
  setup(midi::kPortDin);  // the device's MIDI OUT is DIN
  pattern();
  edit(10, [](model::Instrument& i) {
    i.midi_out = model::kMidiOutUsb;
    i.midi_cc[0] = 75;  // CC A: controller 74
  });
  CHECK(seq_project().set_step(1, 0, 0, make_step(60, 11, fx::kCcA, 99)) == model::Status::Ok);
  play_blocks(20);
  const std::vector<WireMsg> usb = of(1, 0xB0, 2);
  NEED(usb.size() == 1u);
  CHECK((usb[0].b[1] == 74 && usb[0].b[2] == 99));
  CHECK(of(0, 0xB0).empty());  // nothing on DIN: the instrument says USB
  CHECK(of(0, 0x90).empty());
  CHECK(of(1, 0x90, 2).size() == 1u);
}

// ---- the Micro-tune FX as a pitch bend -------------------------------------------------------------------------------------------------------

TEST_CASE("midi fields: the Micro-tune FX is a pitch bend before the note, sized by PB RANGE, and the centre again after") {
  setup();
  pattern();
  edit(10, [](model::Instrument& i) { i.midi_pb_range = 2; });
  CHECK(seq_project().set_step(1, 0, 0, make_step(60, 11, fx::kMicroTune, 198)) == model::Status::Ok);  // +99 cents
  CHECK(seq_project().set_step(1, 0, 1, make_step(62, 11)) == model::Status::Ok);
  play_blocks(2 * 86 + 40);
  const std::vector<WireMsg> bend = of(0, 0xE0, 2);
  NEED(bend.size() == 2u);
  CHECK((bend[0].b[1] == (12247 & 0x7F) && bend[0].b[2] == (12247 >> 7)));  // 12,247
  CHECK((bend[1].b[1] == 0 && bend[1].b[2] == 0x40));                       // 8,192, the centre
  // the order on the wire: bend, note, bend, note
  std::vector<std::uint8_t> order;  // the bends and note-ons, in the order DIN saw them
  for (const WireMsg& m : rig().msgs_of(0)) {
    if (m.b[0] == 0xE2 || m.b[0] == 0x92) {
      order.push_back(m.b[0]);
    }
  }
  CHECK(order == std::vector<std::uint8_t>({0xE2, 0x92, 0xE2, 0x92}));
  CHECK(of(1, 0xE0).size() == 2u);  // both ports
  // a range of 0: no bend at all
  setup();
  pattern();
  edit(10, [](model::Instrument& i) { i.midi_pb_range = 0; });
  CHECK(seq_project().set_step(1, 0, 0, make_step(60, 11, fx::kMicroTune, 198)) == model::Status::Ok);
  play_blocks(86 + 40);
  CHECK(of(0, 0xE0).empty());
  CHECK(of(0, 0x90).size() == 1u);
}

TEST_CASE("midi fields: a stop within an OFFSET drops the bend that was staged behind it: the last bend on the wire is the centre, on both ports") {
  setup();
  pattern();
  edit(10, [](model::Instrument& i) {
    i.midi_pb_range = 2;
    i.midi_offset = 100;  // the bend and the note are staged 100 ms ahead
  });
  CHECK(seq_project().set_step(1, 0, 0, make_step(60, 11, fx::kMicroTune, 198)) == model::Status::Ok);  // +99 cents
  play_blocks(5);
  rig().sequencer().stop();  // within the 100 ms: nothing of the step was heard yet
  rig().settle(150);          // past the delay: a bend that was only staged would have landed by now
  for (const std::uint8_t port : {std::uint8_t{0}, std::uint8_t{1}}) {
    const std::vector<WireMsg> bend = of(port, 0xE0, 2);
    NEED(!bend.empty());
    CHECK((bend.back().b[1] == 0 && bend.back().b[2] == 0x40));  // the last one is the centre ...
    for (const WireMsg& m : bend) {
      CHECK((m.b[1] == 0 && m.b[2] == 0x40));  // ... and no other bend was heard at all
    }
    CHECK(of(port, 0x90).empty());
  }
}

TEST_CASE("midi fields: a restart within an OFFSET also drops the bend that was staged behind it: the last bend on the wire is the centre") {
  setup();
  pattern();
  edit(10, [](model::Instrument& i) {
    i.midi_pb_range = 2;
    i.midi_offset = 100;
  });
  CHECK(seq_project().set_step(1, 0, 1, make_step(60, 11, fx::kMicroTune, 198)) == model::Status::Ok);  // step 1: +99 cents, staged 100 ms ahead
  play_blocks(86 + 3);         // the bent step has been made (its bend due about 97 ms from now) ...
  play_blocks(1);              // ... and Play again while playing: a restart, no Stopped event
  rig().settle(110);           // past the old bend's due time, before the restarted run reaches step 1 again
  for (const std::uint8_t port : {std::uint8_t{0}, std::uint8_t{1}}) {
    const std::vector<WireMsg> bend = of(port, 0xE0, 2);
    NEED(!bend.empty());
    for (const WireMsg& m : bend) {
      CHECK((m.b[1] == 0 && m.b[2] == 0x40));  // only the centre was heard
    }
  }
}

// ---- CC slots as Program Change and aftertouch --------------------------------------------------------------------------------------------

TEST_CASE("midi fields: CC slots set to PROG, channel AT and poly AT send those messages with the FX value") {
  setup();
  pattern();
  edit(10, [](model::Instrument& i) {
    i.midi_cc[0] = model::kMidiCcProg;
    i.midi_cc[1] = model::kMidiCcChanAt;
    i.midi_cc[2] = model::kMidiCcPolyAt;
  });
  CHECK(seq_project().set_step(1, 0, 0, make_step(60, 11, fx::kCcA, 12, fx::kCcA + 1, 34)) == model::Status::Ok);
  CHECK(seq_project().set_step(1, 0, 1, fx_step(fx::kCcA + 2, 56)) == model::Status::Ok);
  play_blocks(3 * 86);
  for (const std::uint8_t port : {std::uint8_t{0}, std::uint8_t{1}}) {
    const std::vector<WireMsg> pc = of(port, 0xC0, 2);
    NEED(pc.size() == 1u);
    CHECK(pc[0].b[1] == 12);
    const std::vector<WireMsg> cat = of(port, 0xD0, 2);
    NEED(cat.size() == 1u);
    CHECK(cat[0].b[1] == 34);
    const std::vector<WireMsg> pat = of(port, 0xA0, 2);
    NEED(pat.size() == 1u);
    CHECK((pat[0].b[1] == 60 && pat[0].b[2] == 56));  // the note the track sounds
  }
}

// ---- OFFSET --------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("midi fields: OFFSET delays everything the instrument sends by that many milliseconds, on both ports; a delay of 0 is no delay") {
  setup();
  midi_slot(11, 2, [](model::Instrument& i) { i.midi_offset = 50; });  // instrument 12: channel 2, 50 ms
  push(pad_on(1, 60, 11));  // instrument 11 (channel 3): no offset
  push(pad_on(2, 61, 12));  // instrument 12: 50 ms
  rig().blocks(2);
  push(pad_off(1));
  push(pad_off(2));
  rig().settle(60);
  const std::vector<WireMsg> din = of(0, 0x90);
  const std::vector<WireMsg> usb = of(1, 0x90);
  NEED(din.size() == 2u);
  NEED(usb.size() == 2u);
  const auto note = [](const std::vector<WireMsg>& v, std::uint8_t n) {
    for (const WireMsg& m : v) {
      if (m.b[1] == n) {
        return m.at;
      }
    }
    return std::uint64_t{0};
  };
  const std::uint64_t d = note(din, 61) - note(din, 60);
  CHECK(d >= 9000000u);              // 50 ms is 9,000,000 cycles at 180 MHz
  CHECK(d <= 9000000u + 3u * 57600u);  // plus what the wire took for the first message
  const std::uint64_t u = note(usb, 61) - note(usb, 60);
  CHECK(u >= 9000000u - 180000u);   // the bus has 1 ms frames
  CHECK(u <= 9000000u + 180000u);
  // the offs are as late as their ons, so the notes keep their length
  const std::vector<WireMsg> offs = of(0, 0x80);
  NEED(offs.size() == 2u);
  CHECK(LiveMidiRig::balanced(rig().notes()));
}

// ---- O2: nothing hangs when the route changes under a held note -------------------------------------------------------------------------

TEST_CASE("midi fields: OUT, the channel and OFFSET changed under a held pad end the note where it began; nothing hangs, on either port") {
  setup();
  edit(10, [](model::Instrument& i) {
    i.midi_out = model::kMidiOutUsb;
    i.midi_offset = 20;
  });
  push(pad_on(1, 60, 11));  // USB only, channel 3, 20 ms late
  rig().blocks(40);
  CHECK(of(1, 0x90, 2).size() == 1u);
  edit(10, [](model::Instrument& i) {  // under the held pad: DIN, channel 7, no delay
    i.midi_out = model::kMidiOutDin;
    i.midi_channel = 7;
    i.midi_offset = 0;
  });
  push(pad_off(1));
  rig().settle(40);
  CHECK(of(1, 0x80, 2).size() == 1u);  // the off went where the on went: USB, channel 3
  CHECK(of(0, 0x90).empty());
  CHECK(of(0, 0x80).empty());
  CHECK(of(1, 0x80, 6).empty());
  CHECK(LiveMidiRig::balanced(rig().notes()));
  CHECK(rig().link().out().book().owed() == 0u);
  // a pad that is released within its own delay: the off follows the on (both 20 ms late), so the receiver sees on then off
  setup();
  edit(10, [](model::Instrument& i) { i.midi_offset = 20; });
  push(pad_on(1, 60, 11));
  rig().blocks(1);
  edit(10, [](model::Instrument& i) { i.midi_offset = 0; });  // the edit under the held pad
  push(pad_off(1));
  rig().settle(40);
  const std::vector<WireMsg> all = rig().msgs_of(0);
  NEED(all.size() == 2u);
  CHECK(all[0].b[0] == 0x92);
  CHECK(all[1].b[0] == 0x82);
  CHECK(all[1].at >= all[0].at);
  CHECK(LiveMidiRig::balanced(rig().din_notes()));
}

TEST_CASE("midi fields: a stop ends a sounding step note on the route of its on, after an OUT and OFFSET edit; a stop within the delay drops the note that never sounded") {
  setup();
  pattern();
  edit(10, [](model::Instrument& i) {
    i.midi_out = model::kMidiOutUsb;
    i.midi_offset = 30;
  });
  CHECK(seq_project().set_step(1, 0, 0, make_step(60, 11)) == model::Status::Ok);
  play_blocks(60);  // the note is on (the first block started it, 30 ms = 21 blocks later it is heard)
  CHECK(of(1, 0x90, 2).size() == 1u);
  edit(10, [](model::Instrument& i) {
    i.midi_out = model::kMidiOutDin;
    i.midi_offset = 0;
  });
  rig().sequencer().stop();
  rig().settle(60);
  CHECK(of(1, 0x80, 2).size() == 1u);
  CHECK(of(0, 0x80).empty());
  CHECK(LiveMidiRig::balanced(rig().notes()));
  CHECK(rig().link().out().book().owed() == 0u);
  // a stop inside the delay: the note-on was staged, not handed over; it is dropped and no off is owed
  setup();
  pattern();
  edit(10, [](model::Instrument& i) { i.midi_offset = 100; });
  CHECK(seq_project().set_step(1, 0, 0, make_step(60, 11)) == model::Status::Ok);
  play_blocks(5);
  rig().sequencer().stop();
  rig().settle(100);
  CHECK(of(0, 0x90).empty());  // it never sounded ...
  CHECK(of(1, 0x90).empty());
  CHECK(rig().link().out().book().owed() == 0u);  // ... so nothing is owed. (The step's own off, staged with the same delay, still goes out after it: a note-off for a note that is not on is harmless, MIDI 1.0.)
  CHECK(LiveMidiRig::held(rig().notes()) <= 0);
}

// ---- VEL> (S9), the rule of the keyboard's velocity ---------------------------------------------------------------------------------------

TEST_CASE("vel>: VEL> blends the keyboard's velocity with none: 100 is the velocity as it is, 0 plays every note as a velocity of 127, between is linear") {
  // VEL> 100 (the default): exactly the law the keyboard has always had, played = velocity, for every velocity
  for (unsigned v = 1; v <= 127; ++v) {
    CHECK(app::vel_scaled_velocity(v, 100) == v);
  }
  // VEL> 0: the velocity is ignored, every note plays as a velocity of 127 does
  for (unsigned v = 1; v <= 127; ++v) {
    CHECK(app::vel_scaled_velocity(v, 0) == 127);
  }
  // VEL> 50: half way between the two: velocity 64 -> 127 - 0.5 x 63 = 95.5, rounded to 96; velocity 1 -> 64; velocity 127 is the same at every VEL>
  CHECK(app::vel_scaled_velocity(64, 50) == 96);
  CHECK(app::vel_scaled_velocity(1, 50) == 64);
  CHECK(app::vel_scaled_velocity(127, 50) == 127);
  // linear in VEL> (to the rounding): velocity 1, 126 below full: 127 - VEL> x 1.26
  CHECK(app::vel_scaled_velocity(1, 25) == 96);   // 127 - 31.5 = 95.5
  CHECK(app::vel_scaled_velocity(1, 75) == 33);   // 127 - 94.5 = 32.5, rounded up
  // monotone in both: a larger velocity never plays lower, a larger VEL> never plays higher (except at velocity 127, which is constant)
  for (unsigned a = 0; a <= 100; ++a) {
    unsigned last = 0;
    for (unsigned v = 1; v <= 127; ++v) {
      const unsigned out = app::vel_scaled_velocity(v, a);
      CHECK((out >= last && out >= 1u && out <= 127u));
      last = out;
    }
  }
  // the range of the field is 0..100, so there is no extension: a larger value is held to 100 (never a negative gain, never above the velocity), and a velocity out of range is held to 1..127
  CHECK(app::vel_scaled_velocity(64, 101) == 64);
  CHECK(app::vel_scaled_velocity(64, 255) == 64);
  CHECK(app::vel_scaled_velocity(64, 4000000000u) == 64);
  CHECK(app::vel_scaled_velocity(0, 100) == 1);
  CHECK(app::vel_scaled_velocity(500, 100) == 127);
  CHECK(app::vel_scaled_velocity(4000000000u, 0) == 127);
}
