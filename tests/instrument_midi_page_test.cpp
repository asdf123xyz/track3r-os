// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSTUI S8a + S8b, the pages of a MIDI instrument: F1 MIDI holds CHANNEL (the S8a field, virtual) and, since INST 1.3, VELOCITY, PROGRAM, OUT, PB RANGE and OFFSET; F2 CC holds CC A..F; F3..F5 are
// dim and say why. CHANNEL is edited as one undo unit within 1..16, and O2 holds for a channel that changes under a held pad: the note-off goes on the channel of its note-on (the render's own record,
// MidiLink::live_pad_), also over F6 / F7, a Stop and a project load. The S8b rows are table rows: each edit is one undo unit and reaches the render (PROGRAM). The render side is the S2b rig
// (support/live_midi_rig.h) with the same PreviewSink as inst_live_midi_test.cpp.
#include <doctest/doctest.h>

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/module_ui.h>
#include <o3os/app/screens/params_screen.h>
#include <o3os/core/input_event.h>
#include <o3os/model/editor.h>
#include <o3os/model/params.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/sim/demo_project.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "support/live_midi_rig.h"
#include "support/need.h"
#include "support/project_fixture.h"
#include "support/seq_fixture.h"

using namespace o3os;
using namespace o3os::test;
using app::Button;
using app::DispatchResult;
using core::ActionArg;
namespace ip = model::instrument_param;

namespace {

alignas(8) std::uint8_t g_ring[64 * 1024];

// The platform's LivePreview in the part that matters here (as inst_live_midi_test.cpp): a pad's note-on goes on the live queue as a pad note, its release as an Off, an input reset as an all-notes-off.
struct QueueSink final : app::PreviewSink {
  audio::LiveBox* box = nullptr;
  std::uint64_t held = 0;
  void preview(unsigned, unsigned) noexcept override {}
  void pad_down(unsigned pad, unsigned track, unsigned instrument, unsigned note) noexcept override {
    if (pad >= 48u) {
      return;
    }
    if (box->queue.push(audio::LiveEvent{audio::LiveKind::On, audio::kLiveSourcePad, static_cast<std::uint8_t>(pad), static_cast<std::uint8_t>(note > 127u ? 127u : note),
                                         static_cast<std::uint8_t>(instrument), audio::kLiveVelocityDefault, static_cast<std::uint8_t>(track), 0})) {
      held |= std::uint64_t{1} << pad;
    }
  }
  void pad_up(unsigned pad) noexcept override {
    if (pad >= 48u || (held & (std::uint64_t{1} << pad)) == 0u) {
      return;
    }
    held &= ~(std::uint64_t{1} << pad);
    (void)box->queue.push(audio::LiveEvent{audio::LiveKind::Off, audio::kLiveSourcePad, static_cast<std::uint8_t>(pad), 0, 0, 0, 0, 0});
  }
  void notes_off(bool midi_only) noexcept override {
    if (!midi_only) {
      held = 0;
    }
    (void)box->queue.push(audio::LiveEvent{audio::LiveKind::AllOff, midi_only ? audio::kLiveSourceMidi : audio::kLiveAllSources, 0, 0, 0, 0, 0, 0});
  }
};

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  LiveMidiRig live;
  QueueSink sink;

  Rig() {
    model::Project& p = project();
    sim::fill_demo_project(p);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    make_midi(6, 3);   // instrument 6: channel 3 (wire 2)
    make_midi(7, 16);  // instrument 7: channel 16 (wire 15)
    make_midi(9, 1);   // instrument 9: channel 1 (wire 0)
    live.init(p);
    sink.box = &live.box();
    app->attach_preview(&sink);
    app->attach_param_ring(&live.engine().params());  // OS-INSTUI S8b: PROGRAM reaches the render over the ring
    app->run_once();
    tap(Button::Params);
  }

  model::Project& project() { return test::arena(1).project; }
  void make_midi(unsigned n, unsigned channel) {
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Midi);
    in.midi_channel = static_cast<std::uint8_t>(channel);
    CHECK(project().set_instrument(n - 1u, in) == model::Status::Ok);
  }
  unsigned channel(unsigned n) { return project().instrument(n - 1u)->midi_channel; }
  void press(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true)); }
  void release(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false)); }
  void pass() {
    clock.advance(core::kTicksPerMs);
    app->run_once();
  }
  void expire_notes() {  // the strip shows the oldest word first: let it go before the next one
    for (int i = 0; i < 4; ++i) {
      clock.advance(10000u * core::kTicksPerMs);
      app->run_once();
    }
  }
  void tap(Button b) {
    press(b);
    pass();
    release(b);
    pass();
  }
  void shift_tap(Button b) {
    press(Button::Shift);
    pass();
    tap(b);
    release(Button::Shift);
    pass();
  }
  void pad_down(unsigned index) {
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(index), 127));
    pass();
  }
  void pad_up(unsigned index) {
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(index), 0));
    pass();
  }
  void turn(int detents) {
    input.push(core::make_encoder_event(clock.now(), 0, static_cast<std::int16_t>(detents)));
    pass();
  }
  DispatchResult invoke(const char* name, ActionArg arg = 0) { return app->invoke(core::action_id(name), arg); }
  void select(int n) { (void)invoke("instrument.select", static_cast<ActionArg>(n)); }
  unsigned pad_note(unsigned pad) const { return app->state().edit.pads.note(pad); }
  std::string note() const { return app->state().notify.empty() ? std::string() : std::string(app->state().notify.head().text.c_str()); }
  // The rows the page shows (field ids).
  unsigned rows(std::uint16_t (&fields)[app::ParamView::kMaxRows]) const { return app::params_mod_rows(app->model(), app->state(), fields); }
};

bool dim(const app::ParamsKeyRow& row, unsigned key) { return ((row.dim >> key) & 1u) != 0u; }

}  // namespace

// ---- the page --------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("midi page: a MIDI instrument has two pages: F1 MIDI and F2 CC are lit, F3..F5 are dim, the rows are CHANNEL, VELOCITY, PROGRAM, OUT, PB RANGE, OFFSET") {
  Rig rig;
  rig.select(6);
  app::ParamsKeyRow row;
  app::params_key_row(rig.app->model(), rig.app->state(), row);
  CHECK(std::string(row.label[0]) == "MIDI");
  CHECK(std::string(row.label[1]) == "CC");
  CHECK_FALSE(dim(row, 0));
  CHECK_FALSE(dim(row, 1));
  for (unsigned key = 2; key <= 4; ++key) {
    CHECK(dim(row, key));  // FILTER MOD SENDS do not apply to a MIDI instrument
  }
  CHECK(std::string(row.label[2]) == "FILTER");
  CHECK_FALSE(dim(row, 5));  // <INS and INS> are the neighbours (5 and 7)
  CHECK_FALSE(dim(row, 6));
  std::uint16_t f[app::ParamView::kMaxRows] = {};
  NEED(rig.rows(f) == 6u);
  CHECK((f[0] == ip::MidiChannel && f[1] == ip::MidiVelocity && f[2] == ip::MidiProgram && f[3] == ip::MidiOut && f[4] == ip::MidiPbRange && f[5] == ip::MidiOffset));
  CHECK(app::kParamsScreen.count(rig.app->model(), rig.app->state()) == 6);
  CHECK(app::kParamsScreen.focus_param(rig.app->model(), rig.app->state()) == core::param_id(core::ParamKind::Instrument, 5, ip::MidiChannel));
  // the field is virtual: the tables do not know it, the descriptor is the model's own and CHANNEL cannot be learned
  CHECK(model::param_desc(core::param_id(core::ParamKind::Instrument, 5, ip::MidiChannel)) == nullptr);
  const core::ParamDesc& d = model::midi_channel_desc();
  CHECK(std::string(d.name) == "CHANNEL");
  CHECK((d.min == 1.0f && d.max == 16.0f && d.def == 1.0f));
  CHECK((d.flags & core::kParamLearnable) == 0);
}

TEST_CASE("midi page: the page follows the kind, whatever page the neighbour was left on") {
  Rig rig;
  rig.select(2);
  rig.tap(Button::F5);  // a sample instrument's SENDS page
  std::uint16_t f[app::ParamView::kMaxRows] = {};
  NEED(rig.rows(f) >= 1u);
  CHECK(f[0] == ip::DelaySend);
  rig.select(6);  // the MIDI instrument: its own page, not the SENDS page the view holds
  NEED(rig.rows(f) == 6u);
  CHECK(f[0] == ip::MidiChannel);
  rig.select(2);  // and back: the sample instrument's page is where it was left
  NEED(rig.rows(f) >= 1u);
  CHECK(f[0] == ip::DelaySend);
  rig.select(6);
  rig.expire_notes();
  rig.tap(Button::F4);  // MOD on a MIDI instrument is dim
  CHECK(rig.note() == "NO MOD PAGE ON A MIDI INSTRUMENT");
  NEED(rig.rows(f) == 6u);
  CHECK(f[0] == ip::MidiChannel);
  // the CC page left in the view reads as VOICE on a sample instrument, and the MIDI page again on the next MIDI instrument's
  rig.tap(Button::F2);
  NEED(rig.rows(f) == 6u);
  CHECK(f[0] == ip::midi_cc(0));
  rig.select(2);
  NEED(rig.rows(f) >= 1u);
  CHECK(f[0] == ip::Volume);
  rig.select(7);
  NEED(rig.rows(f) == 6u);
  CHECK(f[0] == ip::midi_cc(0));  // (the view holds the CC page: a MIDI instrument keeps the page of the neighbour that was a MIDI instrument)
}

TEST_CASE("midi page: F1 and F2 turn the two pages, Left / Right go to the other, the Params button is F1; F3..F5 answer with why they are dim and change nothing") {
  Rig rig;
  rig.select(6);
  const char* const why[3] = {"NO FILTER PAGE ON A MIDI INSTRUMENT", "NO MOD PAGE ON A MIDI INSTRUMENT", "NO SENDS PAGE ON A MIDI INSTRUMENT"};
  const unsigned undo_before = rig.journal.depth();
  std::uint16_t f[app::ParamView::kMaxRows] = {};
  for (unsigned key = 2; key <= 4; ++key) {
    rig.expire_notes();
    CHECK(rig.invoke("instrument.page", static_cast<ActionArg>(key)) == DispatchResult::Changed);  // (a notification)
    CHECK(rig.note() == why[key - 2]);
    NEED(rig.rows(f) == 6u);
    CHECK(f[0] == ip::MidiChannel);
  }
  CHECK(rig.invoke("instrument.page", 0) == DispatchResult::Unchanged);  // F1: the page is where it is
  CHECK(rig.invoke("instrument.page", 1) == DispatchResult::Changed);   // F2: the CC page
  NEED(rig.rows(f) == 6u);
  for (unsigned i = 0; i < 6; ++i) {
    CHECK(f[i] == ip::midi_cc(i));
  }
  CHECK(rig.invoke("instrument.page", 1) == DispatchResult::Unchanged);
  CHECK(app::kParamsScreen.focus_param(rig.app->model(), rig.app->state()) == core::param_id(core::ParamKind::Instrument, 5, ip::midi_cc(0)));
  CHECK(rig.invoke("instrument.turn", -1) == DispatchResult::Changed);  // Left on CC: the MIDI page (either direction goes to the other)
  NEED(rig.rows(f) == 6u);
  CHECK(f[0] == ip::MidiChannel);
  CHECK(rig.invoke("instrument.turn", 1) == DispatchResult::Changed);
  CHECK(rig.rows(f) == 6u);
  CHECK(f[0] == ip::midi_cc(0));
  rig.tap(Button::Params);  // the Params button again: the first page
  NEED(rig.rows(f) == 6u);
  CHECK(f[0] == ip::MidiChannel);
  CHECK(rig.journal.depth() == undo_before);  // no key made an undo unit
  CHECK(rig.channel(6) == 3);
}

TEST_CASE("midi page: the chooser's MIDI instrument opens on the MIDI page") {
  Rig rig;
  rig.select(12);  // a free slot: the type chooser
  rig.tap(Button::F2);
  CHECK(rig.note() == "MIDI INSTRUMENT ON CHANNEL 1");
  std::uint16_t f[app::ParamView::kMaxRows] = {};
  NEED(rig.rows(f) == 6u);
  CHECK(f[0] == ip::MidiChannel);
  CHECK(rig.channel(12) == 1);
}

// ---- CHANNEL ---------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("midi page: CHANNEL is one undo unit an edit, held to 1..16, with undo, redo and the default on Delete") {
  Rig rig;
  rig.select(6);
  CHECK(rig.channel(6) == 3);
  rig.turn(1);
  CHECK(rig.channel(6) == 4);
  rig.tap(Button::F8);  // UNDO
  CHECK(rig.channel(6) == 3);
  rig.shift_tap(Button::F8);  // REDO
  CHECK(rig.channel(6) == 4);
  // one turn of many clicks is one unit
  rig.turn(12);
  CHECK(rig.channel(6) == 16);
  rig.tap(Button::F8);
  CHECK(rig.channel(6) == 4);  // back to what it was before that one turn
  rig.shift_tap(Button::F8);
  CHECK(rig.channel(6) == 16);
  // the ends: past 16 and below 1 nothing is written and no empty unit is recorded
  const unsigned units = rig.journal.depth();
  CHECK(rig.invoke("param.edit", 1) == DispatchResult::Unchanged);
  CHECK(rig.channel(6) == 16);
  CHECK(rig.journal.depth() == units);
  rig.turn(-100);
  CHECK(rig.channel(6) == 1);
  CHECK(rig.invoke("param.edit", -1) == DispatchResult::Unchanged);
  CHECK(rig.channel(6) == 1);
  rig.tap(Button::F8);
  CHECK(rig.channel(6) == 16);  // the -100 turn was one unit; the refused one added none
  // Shift + encoder is the coarse step (4), Delete the descriptor's default (1)
  CHECK(rig.invoke("param.reset") == DispatchResult::Changed);
  CHECK(rig.channel(6) == 1);
  CHECK(rig.invoke("param.coarse", 1) == DispatchResult::Changed);
  CHECK(rig.channel(6) == 5);
  CHECK(rig.invoke("param.reset") == DispatchResult::Changed);
  CHECK(rig.invoke("param.reset") == DispatchResult::Unchanged);  // already there
}

TEST_CASE("midi page: an edit changes the channel and nothing else of the instrument") {
  Rig rig;
  rig.select(6);
  const model::Instrument before = *rig.project().instrument(5);
  rig.turn(2);
  const model::Instrument& after = *rig.project().instrument(5);
  CHECK(after.midi_channel == 5);
  CHECK(after.kind == before.kind);
  CHECK(std::strcmp(after.name, before.name) == 0);
  CHECK(std::memcmp(&after.sampler, &before.sampler, sizeof after.sampler) == 0);
  CHECK(rig.channel(7) == 16);  // the neighbours are untouched
  CHECK(rig.channel(9) == 1);
  CHECK(model::validate_instrument(after, 0) == nullptr);
}

TEST_CASE("midi page: INIT keeps the channel") {
  Rig rig;
  rig.select(6);
  rig.turn(3);
  CHECK(rig.channel(6) == 6);
  (void)rig.invoke("instrument.init");
  CHECK(rig.channel(6) == 6);
}

// ---- O2: a channel that changes under a held pad ---------------------------------------------------------------------------------------------

TEST_CASE("midi page O2: CHANNEL edited under a held pad, the note-off goes on the channel of its note-on; the next note uses the new one") {
  Rig rig;
  rig.select(6);  // channel 3 (wire 2)
  rig.pad_down(3);
  rig.live.blocks(3);
  NEED(rig.live.notes().size() == 1u);
  CHECK(rig.live.notes()[0].on);
  CHECK(rig.live.notes()[0].ch == 2);
  rig.turn(5);  // channel 8 (wire 7) while the pad is held
  CHECK(rig.channel(6) == 8);
  rig.live.blocks(3);
  CHECK(rig.live.notes().size() == 1u);  // the edit sends nothing by itself
  rig.pad_up(3);
  rig.live.settle();
  NEED(rig.live.notes().size() == 2u);
  CHECK_FALSE(rig.live.notes()[1].on);
  CHECK(rig.live.notes()[1].ch == 2);  // the channel of the On, not the new 8
  CHECK(rig.live.notes()[1].note == rig.live.notes()[0].note);
  CHECK(LiveMidiRig::balanced(rig.live.notes()));
  rig.pad_down(3);  // the next press is on the new channel, and its release too
  rig.live.blocks(3);
  rig.pad_up(3);
  rig.live.settle();
  NEED(rig.live.notes().size() == 4u);
  CHECK((rig.live.notes()[2].on && rig.live.notes()[2].ch == 7));
  CHECK((!rig.live.notes()[3].on && rig.live.notes()[3].ch == 7));
  CHECK(LiveMidiRig::balanced(rig.live.notes()));
  CHECK(same_notes_on_both_ports(rig.live));
}

TEST_CASE("midi page O2: two pads across an edit and its undo each end on their own channel") {
  Rig rig;
  rig.select(6);  // channel 3
  rig.pad_down(2);
  rig.live.blocks(3);
  rig.turn(1);  // channel 4
  rig.pad_down(9);
  rig.live.blocks(3);
  rig.tap(Button::F8);  // UNDO: channel 3 again, while both pads are held
  CHECK(rig.channel(6) == 3);
  rig.live.blocks(3);
  NEED(rig.live.notes().size() == 2u);
  CHECK(rig.live.notes()[0].ch == 2);
  CHECK(rig.live.notes()[1].ch == 3);
  rig.pad_up(2);
  rig.pad_up(9);
  rig.live.settle();
  NEED(rig.live.notes().size() == 4u);
  for (const WireNote& n : rig.live.notes()) {
    if (n.on) {
      continue;
    }
    // each note-off pairs with the note-on of the same note, on that note-on's channel
    bool paired = false;
    for (const WireNote& on : rig.live.notes()) {
      paired = paired || (on.on && on.note == n.note && on.ch == n.ch);
    }
    CHECK(paired);
  }
  CHECK(LiveMidiRig::balanced(rig.live.notes()));
  CHECK(same_notes_on_both_ports(rig.live));
}

TEST_CASE("midi page O2: F7 after an edit under a held pad ends the note on the channel it started on") {
  Rig rig;
  rig.select(6);  // channel 3 (wire 2); F7 goes to instrument 7, a MIDI instrument on channel 16
  rig.pad_down(4);
  rig.live.blocks(3);
  rig.turn(2);  // channel 5
  rig.tap(Button::F7);
  rig.live.settle();
  CHECK(rig.app->state().instrument == 7);
  NEED(rig.live.notes().size() == 2u);
  CHECK_FALSE(rig.live.notes()[1].on);
  CHECK(rig.live.notes()[1].ch == 2);
  rig.tap(Button::F6);  // back on 6 (channel 5 now): a new press is on the new channel
  rig.pad_up(4);        // the late release sends nothing
  rig.live.settle();
  CHECK(rig.live.notes().size() == 2u);
  rig.pad_down(4);
  rig.live.blocks(3);
  rig.pad_up(4);
  rig.live.settle();
  NEED(rig.live.notes().size() == 4u);
  CHECK(rig.live.notes()[2].ch == 4);
  CHECK(rig.live.notes()[3].ch == 4);
  CHECK(LiveMidiRig::balanced(rig.live.notes()));
}

TEST_CASE("midi page O2: Stop after an edit under a held pad ends the pad's note on its own channel, once") {
  Rig rig;
  CHECK(rig.project().create_pattern(200, 16) == model::Status::Ok);
  for (std::uint32_t k = 0; k < 16; ++k) {
    CHECK(rig.project().set_step(200, 0, k, make_step(static_cast<std::uint8_t>(40 + k), 9)) == model::Status::Ok);  // the sequencer plays MIDI instrument 9 (wire channel 0)
  }
  CHECK(rig.project().append_order(200) == model::Status::Ok);
  rig.live.sequencer().play_pattern(200);
  rig.live.blocks(2);
  rig.select(6);  // channel 3 (wire 2)
  rig.pad_down(4);
  rig.live.blocks(4);
  rig.turn(7);  // channel 10 (wire 9)
  rig.live.blocks(2);
  rig.live.sequencer().stop();
  rig.live.settle();
  CHECK(LiveMidiRig::held(rig.live.notes()) == 0);
  bool pad_off = false;
  for (const WireNote& n : rig.live.notes()) {
    CHECK(n.ch != 9);  // nothing ever went to the new channel: no note was started on it
    pad_off = pad_off || (!n.on && n.ch == 2 && n.note == rig.pad_note(4));
  }
  CHECK(pad_off);
  const std::size_t after_stop = rig.live.notes().size();
  rig.pad_up(4);
  rig.live.settle();
  CHECK(rig.live.notes().size() == after_stop);
  CHECK(LiveMidiRig::balanced(rig.live.notes()));
  CHECK(same_notes_on_both_ports(rig.live));
}

TEST_CASE("midi page O2: a project load after an edit under a held pad still ends the note where it started") {
  Rig rig;
  rig.select(6);  // channel 3 (wire 2)
  rig.pad_down(1);
  rig.live.blocks(3);
  rig.turn(4);  // channel 7
  sim::fill_demo_project(rig.project());  // the load: slot 6 is not a MIDI instrument any more
  rig.app->run_once();
  rig.pad_up(1);
  rig.live.settle();
  NEED(rig.live.notes().size() == 2u);
  CHECK(rig.live.notes()[1].ch == 2);
  CHECK(LiveMidiRig::balanced(rig.live.notes()));
  CHECK(same_notes_on_both_ports(rig.live));
}

TEST_CASE("midi page O2: a step's note that sounds when CHANNEL changes ends on the channel it started on") {
  Rig rig;
  CHECK(rig.project().create_pattern(201, 16) == model::Status::Ok);
  for (std::uint32_t k = 0; k < 16; k += 4) {
    CHECK(rig.project().set_step(201, 0, k, make_step(static_cast<std::uint8_t>(50 + k), 6)) == model::Status::Ok);  // MIDI instrument 6 (channel 3, wire 2)
  }
  CHECK(rig.project().append_order(201) == model::Status::Ok);
  rig.live.sequencer().play_pattern(201);
  rig.live.blocks(40);
  CHECK(LiveMidiRig::held(rig.live.notes()) >= 1);  // a note is sounding
  rig.select(6);
  rig.turn(6);  // channel 9 while it sounds
  rig.live.sequencer().stop();
  rig.live.settle();
  CHECK(LiveMidiRig::held(rig.live.notes()) == 0);
  for (const WireNote& n : rig.live.notes()) {
    CHECK(n.ch == 2);  // (the edit came after the sequencer's last note-on)
  }
  CHECK(LiveMidiRig::balanced(rig.live.notes()));
}

TEST_CASE("midi page O2: DELETE of the MIDI instrument under a held pad, after a channel edit, ends the note on the channel of its note-on") {
  Rig rig;
  rig.select(6);  // channel 3 (wire 2)
  rig.pad_down(3);
  rig.live.blocks(3);
  rig.turn(4);  // channel 7 while held
  rig.shift_tap(Button::F4);  // DELETE: the pads' notes end first
  rig.live.settle();
  CHECK(rig.project().instrument(5) == nullptr);
  NEED(rig.live.notes().size() == 2u);
  CHECK_FALSE(rig.live.notes()[1].on);
  CHECK(rig.live.notes()[1].ch == 2);
  CHECK(rig.live.notes()[1].note == rig.live.notes()[0].note);
  rig.pad_up(3);  // the late release adds nothing
  rig.live.settle();
  CHECK(rig.live.notes().size() == 2u);
  CHECK(LiveMidiRig::balanced(rig.live.notes()));
  rig.tap(Button::F8);  // UNDO brings the instrument back with the edited channel; a new note is on it
  CHECK(rig.channel(6) == 7);
  rig.pad_down(3);
  rig.live.blocks(3);
  rig.pad_up(3);
  rig.live.settle();
  NEED(rig.live.notes().size() == 4u);
  CHECK(rig.live.notes()[2].ch == 6);
  CHECK(rig.live.notes()[3].ch == 6);
  CHECK(LiveMidiRig::balanced(rig.live.notes()));
}

TEST_CASE("midi page O2: INIT of the MIDI instrument under a held pad keeps the channel and the note ends on the channel of its note-on") {
  Rig rig;
  rig.select(6);  // channel 3 (wire 2)
  rig.pad_down(3);
  rig.live.blocks(3);
  rig.turn(4);  // channel 7 while held
  (void)rig.invoke("instrument.init");
  CHECK(rig.channel(6) == 7);  // INIT keeps the channel
  rig.live.blocks(3);
  rig.pad_up(3);
  rig.live.settle();
  NEED(rig.live.notes().size() == 2u);
  CHECK_FALSE(rig.live.notes()[1].on);
  CHECK(rig.live.notes()[1].ch == 2);
  CHECK(rig.live.notes()[1].note == rig.live.notes()[0].note);
  CHECK(LiveMidiRig::balanced(rig.live.notes()));
  CHECK(same_notes_on_both_ports(rig.live));
}

TEST_CASE("midi page: a Learn binding edited by hand onto CHANNEL does not move it (apply_cc wants a learnable descriptor)") {
  Rig rig;
  rig.select(6);
  const core::ParamId id = core::param_id(core::ParamKind::Instrument, 5, ip::MidiChannel);
  core::LearnBinding b{};
  b.param = id;
  b.cc = 20;
  b.channel = 0;
  b.mode = static_cast<std::uint8_t>(core::LearnMode::Jump);
  const model::Status st = rig.editor->set_learn_slot(0, b);
  NEED(st == model::Status::Ok);  // the store takes any parameter id: the guard is apply_cc's
  CHECK(rig.project().settings().learn_count() == 1u);
  CHECK(rig.invoke("midi.cc", static_cast<ActionArg>((0u << 16) | (20u << 8) | 127u)) == DispatchResult::Unchanged);
  CHECK(rig.invoke("midi.cc", static_cast<ActionArg>((0u << 16) | (20u << 8) | 0u)) == DispatchResult::Unchanged);
  CHECK(rig.channel(6) == 3);
}

// ---- S8b: the rows -----------------------------------------------------------------------------------------------------------------------------

namespace {

std::vector<WireMsg> program_changes(const LiveMidiRig& live, std::uint8_t port) {
  std::vector<WireMsg> r;
  for (const WireMsg& m : live.msgs_of(port)) {
    if ((m.b[0] & 0xF0u) == 0xC0u) {
      r.push_back(m);
    }
  }
  return r;
}

}  // namespace

TEST_CASE("midi page S8b: VELOCITY, PROGRAM, OUT, PB RANGE and OFFSET are edited by the encoder, one undo unit each, held to their ranges, with the default on Delete") {
  Rig rig;
  rig.select(6);
  const model::Instrument fresh = *rig.project().instrument(5);
  const auto inst = [&]() -> const model::Instrument& { return *rig.project().instrument(5); };
  struct Row {
    unsigned row;
    int turn;
    std::uint8_t model::Instrument::*field;
    unsigned expect;
    unsigned dflt;
  };
  const Row rows[] = {{1, -27, &model::Instrument::midi_velocity, 100, 127}, {2, 6, &model::Instrument::midi_program, 6, 0}, {3, 2, &model::Instrument::midi_out, 2, 0},
                      {4, 3, &model::Instrument::midi_pb_range, 5, 2}, {5, 10, &model::Instrument::midi_offset, 10, 0}};
  rig.tap(Button::Down);  // VELOCITY
  for (const Row& r : rows) {
    CAPTURE(r.row);
    CHECK(rig.app->state().params.row == static_cast<int>(r.row));
    const unsigned units = rig.journal.depth();
    rig.turn(r.turn);
    CHECK(static_cast<unsigned>(inst().*r.field) == r.expect);
    CHECK(rig.journal.depth() == units + 1u);  // a turn of many clicks is one unit
    rig.tap(Button::F8);
    CHECK(static_cast<unsigned>(inst().*r.field) == r.dflt);
    rig.shift_tap(Button::F8);
    CHECK(static_cast<unsigned>(inst().*r.field) == r.expect);
    CHECK(rig.invoke("param.reset") == DispatchResult::Changed);
    CHECK(static_cast<unsigned>(inst().*r.field) == r.dflt);
    CHECK(rig.invoke("param.reset") == DispatchResult::Unchanged);
    rig.turn(r.turn);  // and the value again, to leave it away from the default
    rig.tap(Button::Down);
  }
  // the ends: OFFSET stops at 100 ms, VELOCITY at 1, PB RANGE at 24, OUT at DIN + USB
  rig.turn(500);
  CHECK(inst().midi_offset == 100);
  rig.tap(Button::Up);
  rig.turn(-500);
  CHECK(inst().midi_pb_range == 0);
  rig.turn(500);
  CHECK(inst().midi_pb_range == 24);
  rig.tap(Button::Up);
  rig.turn(500);
  CHECK(inst().midi_out == 3);
  rig.tap(Button::Up);
  rig.tap(Button::Up);
  rig.turn(-500);
  CHECK(inst().midi_velocity == 1);
  CHECK(model::validate_instrument(inst(), 0) == nullptr);
  CHECK(inst().midi_channel == fresh.midi_channel);
  CHECK(std::memcmp(&inst().sampler, &fresh.sampler, sizeof fresh.sampler) == 0);  // nothing else of the instrument moved
  // INIT puts every row back and keeps the channel
  (void)rig.invoke("instrument.init");
  CHECK(model::inst_v13_default(inst()));
  CHECK(inst().midi_channel == 3);
}

TEST_CASE("midi page S8b: the CC page edits CC A..F, each one undo unit; the text of a slot is CONFIG, CC n, PROG, CH AT or POLY AT") {
  Rig rig;
  rig.select(6);
  rig.tap(Button::F2);
  const core::ParamDesc& d = *model::param_desc(core::param_id(core::ParamKind::Instrument, 5, ip::midi_cc(0)));
  const auto text = [&](unsigned v) {
    char buf[core::kParamTextChars + 1] = {};
    (void)core::param_format(d, static_cast<float>(v), buf, sizeof buf);
    return std::string(buf);
  };
  const unsigned units = rig.journal.depth();
  for (unsigned i = 0; i < 6; ++i) {
    CHECK(rig.app->state().params.row == static_cast<int>(i));
    rig.turn(static_cast<int>(10 * (i + 1)));
    CHECK(rig.project().instrument(5)->midi_cc[i] == 10u * (i + 1u));
    rig.tap(Button::Down);
  }
  CHECK(rig.journal.depth() == units + 6u);
  CHECK(text(rig.project().instrument(5)->midi_cc[0]) == "CC 9");
  rig.tap(Button::F8);
  CHECK(rig.project().instrument(5)->midi_cc[5] == 0);
  CHECK(rig.project().instrument(5)->midi_cc[4] == 50);
  CHECK(text(129) == "PROG");
  // the three options sit at the top of the range: 129 PROG, 130 CH AT, 131 POLY AT
  for (int i = 0; i < 5; ++i) {
    rig.tap(Button::Up);  // CC A
  }
  rig.turn(500);
  CHECK(rig.project().instrument(5)->midi_cc[0] == 131);
  CHECK(text(131) == "POLY AT");
  CHECK(model::validate_instrument(*rig.project().instrument(5), 0) == nullptr);
}

TEST_CASE("midi page S8b: an edit of PROGRAM sends the Program Change on the instrument's channel to both ports; undo and redo send the program they restore; none sends nothing") {
  Rig rig;
  rig.select(6);  // channel 3 (wire 2)
  rig.tap(Button::Down);
  rig.tap(Button::Down);  // PROGRAM
  rig.turn(6);            // program 5
  rig.live.settle();
  for (const std::uint8_t port : {std::uint8_t{0}, std::uint8_t{1}}) {
    const std::vector<WireMsg> pc = program_changes(rig.live, port);
    NEED(pc.size() == 1u);
    CHECK((pc[0].b[0] == 0xC2 && pc[0].b[1] == 5));
  }
  rig.clock.advance(1600u * core::kTicksPerMs);  // (OS-UNDOGRP: clicks within 1500 ms on one row are one undo unit)
  rig.turn(4);  // program 9
  rig.live.settle();
  NEED(program_changes(rig.live, 0).size() == 2u);
  CHECK(program_changes(rig.live, 0)[1].b[1] == 9);
  rig.tap(Button::F8);  // UNDO: program 5 again, sent
  rig.live.settle();
  NEED(program_changes(rig.live, 0).size() == 3u);
  CHECK(program_changes(rig.live, 0)[2].b[1] == 5);
  rig.shift_tap(Button::F8);  // REDO: 9
  rig.live.settle();
  NEED(program_changes(rig.live, 0).size() == 4u);
  CHECK(program_changes(rig.live, 0)[3].b[1] == 9);
  rig.turn(-50);  // none
  rig.live.settle();
  CHECK(program_changes(rig.live, 0).size() == 4u);
  CHECK(rig.project().instrument(5)->midi_program == 0);
  // the first note after the program was set to none sends no program change either
  rig.pad_down(3);
  rig.live.blocks(3);
  rig.pad_up(3);
  rig.live.settle();
  CHECK(program_changes(rig.live, 0).size() == 4u);
  CHECK(LiveMidiRig::balanced(rig.live.notes()));
}

TEST_CASE("midi page S8b O2: OUT changed under a held pad ends the note on the port it started on; the next note uses the new one") {
  Rig rig;
  rig.select(6);  // channel 3 (wire 2)
  for (int i = 0; i < 3; ++i) {
    rig.tap(Button::Down);  // OUT
  }
  rig.turn(2);  // USB only
  CHECK(rig.project().instrument(5)->midi_out == model::kMidiOutUsb);
  rig.pad_down(3);
  rig.live.blocks(3);
  CHECK(rig.live.notes().size() == 1u);  // the USB ring (notes() is the USB capture)
  CHECK(rig.live.din_notes().empty());
  rig.turn(-1);  // DIN only, under the held pad
  CHECK(rig.project().instrument(5)->midi_out == model::kMidiOutDin);
  rig.pad_up(3);
  rig.live.settle();
  NEED(rig.live.notes().size() == 2u);  // USB: on, off
  CHECK_FALSE(rig.live.notes()[1].on);
  CHECK(rig.live.din_notes().empty());   // nothing went to DIN: the off followed its on
  CHECK(LiveMidiRig::balanced(rig.live.notes()));
  CHECK(rig.live.link().out().book().owed() == 0u);
  rig.pad_down(3);  // the next press is on DIN
  rig.live.blocks(3);
  rig.pad_up(3);
  rig.live.settle();
  CHECK(rig.live.notes().size() == 2u);
  CHECK(rig.live.din_notes().size() == 2u);
  CHECK(LiveMidiRig::balanced(rig.live.din_notes()));
}

TEST_CASE("sample page S9: VEL> closes the VOICE page of a sample instrument, edited like the rows above it, one undo unit; a MIDI instrument has no such row") {
  Rig rig;
  rig.select(2);  // a sample instrument
  rig.tap(Button::F2);  // VOICE
  std::uint16_t f[app::ParamView::kMaxRows] = {};
  const unsigned n = rig.rows(f);
  NEED(n == 6u);
  CHECK(f[n - 1] == ip::VelSens);
  for (unsigned i = 0; i + 1 < n; ++i) {
    rig.tap(Button::Down);
  }
  CHECK(rig.project().instrument(1)->vel_sens == 100);
  const unsigned units = rig.journal.depth();
  rig.turn(-30);
  CHECK(rig.project().instrument(1)->vel_sens == 70);
  CHECK(rig.journal.depth() == units + 1u);
  rig.tap(Button::F8);
  CHECK(rig.project().instrument(1)->vel_sens == 100);
  CHECK(rig.invoke("param.reset") == DispatchResult::Unchanged);
  rig.turn(-500);
  CHECK(rig.project().instrument(1)->vel_sens == 0);
  rig.select(6);
  NEED(rig.rows(f) == 6u);
  for (unsigned i = 0; i < 6; ++i) {
    CHECK(f[i] != ip::VelSens);
  }
}
