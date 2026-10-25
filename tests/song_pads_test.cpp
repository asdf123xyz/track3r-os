// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-SONGPADS (docs/specs/SONG-UX.md section 2; new Community OS behaviour): the pads on the Song screen. Shift + F7 PADS switches TRACKS (the layer of pad_layer.h: mute, solo, mute mode) and
// SLOTS (48 pads are 48 slots of the order list: a tap queues the slot while the song plays, moves the cursor when it is stopped); the LEDs; a lone Shift hold shows TRACKS in SLOTS.
// The row 1 move of the track cursor belongs to OS-SONGMAP (song_map_edit_test.cpp).
#include <doctest/doctest.h>

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/mix_board.h>
#include <o3os/app/pad_lights.h>
#include <o3os/app/state_line.h>
#include <o3os/app/track_state.h>
#include <o3os/audio/play_position.h>
#include <o3os/core/input_event.h>
#include <o3os/model/editor.h>
#include <o3os/platform/host/fakes.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#include "support/need.h"
#include "support/project_fixture.h"

using namespace o3os;
using app::Button;
using app::DispatchResult;
using app::PadRole;

namespace {

alignas(8) std::uint8_t g_ring[256 * 1024];

struct JumpLog final : app::TransportSink {
  unsigned jumps = 0;
  unsigned jump_row = 0;
  bool accept = true;
  bool queue_jump(unsigned row) noexcept override {
    if (!accept) {
      return false;
    }
    ++jumps;
    jump_row = row;
    return true;
  }
  bool play(unsigned) noexcept override { return true; }
  bool play_song(unsigned) noexcept override { return true; }
  bool stop() noexcept override { return true; }
};

// The sequencer's side as the TRACKS pads see it (the same fake as pad_modes_test.cpp's).
struct FakeSink final : app::TrackSink {
  bool playing() const noexcept override { return false; }
  bool mute_defer(unsigned, app::MuteMode, bool) noexcept override { return true; }
  bool mute_cancel(unsigned) noexcept override { return true; }
  std::uint16_t mute_pending() const noexcept override { return 0; }
  std::uint8_t note_count(unsigned) const noexcept override { return 0; }
};

// An App on the Song screen with a document: `slots` slots over the patterns 1..3 (16 steps), 12 active tracks.
struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  std::unique_ptr<app::MixBoard> board = std::make_unique<app::MixBoard>();
  FakeSink sink;
  JumpLog log;
  audio::PlayPosition position;

  explicit Rig(unsigned slots = 6) {
    model::Project& p = test::arena(0).project;
    p.reset();
    for (unsigned id = 1; id <= 3; ++id) {
      NEED(p.create_pattern(id, 16) == model::Status::Ok);
    }
    for (unsigned i = 0; i < slots; ++i) {
      NEED(p.append_order(1 + i % 3) == model::Status::Ok);
    }
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(p, journal);
    board->tracks = &sink;
    app->attach_document(editor.get());
    app->attach_mix(board.get());
    app->attach_transport(&log);
    app->attach_play_position(&position);
    app->run_once();
    tap(Button::Song);
  }
  ~Rig() { app->attach_mix(nullptr); }
  model::Project& project() { return test::arena(0).project; }
  const app::ViewState& state() const { return app->state(); }
  void press(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true)); }
  void release(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false)); }
  void pass(unsigned ms = 0) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  void tap(Button b) {
    press(b);
    pass(1);
    release(b);
    pass(1);
  }
  void shift_tap(Button b) {
    press(Button::Shift);
    pass(1);
    tap(b);
    release(Button::Shift);
    pass(1);
  }
  void pad(unsigned index) {
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(index), 100));
    pass(1);
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(index), 0));
    pass(1);
  }
  bool lights(PadRole (&roles)[48]) const { return app->pad_lights(roles); }
  std::uint8_t flags(unsigned t) { return project().track(t)->flags; }
  std::string message() const { return app->message(); }
  void play() {
    position.publish(true, 1, 0, 12000, 0);
    pass(1);
  }
  std::string line() const {
    char buf[512];
    (void)app::describe_state(*app, buf, sizeof buf);
    return buf;
  }
};

constexpr unsigned kRowMute = 12, kRowMode = 24, kRowSolo = 36;

}  // namespace

TEST_CASE("song pads: Shift + F7 switches TRACKS and SLOTS, the strip and the state line follow, the mode starts as TRACKS") {
  Rig rig;
  CHECK_FALSE(rig.state().padmode.song_slots());
  CHECK(rig.line().find("pads=TRACKS") != std::string::npos);
  rig.shift_tap(Button::F7);
  CHECK(rig.state().padmode.song_slots());
  CHECK(rig.message() == "PADS: SLOTS");
  CHECK(rig.line().find("pads=SLOTS") != std::string::npos);
  rig.shift_tap(Button::F7);
  CHECK_FALSE(rig.state().padmode.song_slots());
  CHECK(rig.message() == "PADS: TRACKS");
  // The mode is the Song screen's own: another screen keeps its pads' mode, and the Song mode is still there on the way back.
  rig.shift_tap(Button::F7);
  rig.tap(Button::Pattern);
  CHECK(rig.state().padmode.fixed() == app::PadMode::Notes);
  CHECK(rig.line().find("pads=NOTES") != std::string::npos);
  rig.tap(Button::Song);
  CHECK(rig.state().padmode.song_slots());
}

TEST_CASE("song pads: TRACKS on Song mutes, solos and cycles the mute mode, shared with the Pattern screen's flags") {
  Rig rig;
  rig.pad(kRowMute + 2);
  CHECK(rig.flags(2) == model::kTrackMute);
  rig.pad(kRowMute + 2);
  CHECK(rig.flags(2) == 0);
  rig.pad(kRowSolo + 4);
  CHECK(rig.flags(4) == model::kTrackSolo);
  rig.pad(kRowMode + 1);
  CHECK(rig.board->mute_mode[1] == static_cast<std::uint8_t>(app::MuteMode::Bar));
  rig.pad(kRowMute + 5);
  rig.tap(Button::Pattern);  // the same project state on the other screen
  CHECK(rig.flags(5) == model::kTrackMute);
  // the 12th column is the last track the pads reach (they have 12 columns): with 12 active tracks it mutes; a track the project does not have is refused
  rig.tap(Button::Song);
  rig.pad(kRowMute + 11);
  CHECK(rig.flags(11) == model::kTrackMute);
  NEED(rig.editor->set_active_tracks(6) == model::Status::Ok);
  rig.pass(1);
  rig.pad(kRowMute + 8);
  CHECK(rig.flags(8) == 0);
  // a mute is a unit of the SONG area while Song is in view (the Pattern screen's is the PATTERN area's)
  CHECK(rig.editor->journal(model::UndoArea::Song).depth() >= 1u);
}

TEST_CASE("song pads: the TRACKS layer reaches the Song screen only, not the screens that have no such layer") {
  Rig rig;
  rig.tap(Button::Master);  // the Mixer
  CHECK(rig.state().screen == app::ScreenId::Mixer);
  CHECK(rig.app->invoke(core::action_id("track.mute"), 2) == DispatchResult::Disabled);
  CHECK(rig.app->invoke(core::action_id("track.solo"), 2) == DispatchResult::Disabled);
  CHECK(rig.app->invoke(core::action_id("track.mute_mode"), 2) == DispatchResult::Disabled);
  CHECK(rig.flags(2) == 0);
  rig.tap(Button::File);
  CHECK(rig.app->invoke(core::action_id("track.mute"), 2) == DispatchResult::Disabled);
  rig.tap(Button::Song);
  CHECK(rig.app->invoke(core::action_id("track.mute"), 2) != DispatchResult::Disabled);
}

TEST_CASE("song pads: SLOTS, stopped, a tap moves the cursor to the slot and collapses the selection; a pad past the end does nothing") {
  Rig rig(6);
  rig.shift_tap(Button::F7);
  rig.press(Button::Shift);
  rig.pass(1);
  rig.tap(Button::Down);  // Shift + Down: a range
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.state().song.selecting());
  rig.pad(4);
  CHECK(rig.state().song.pos() == 4);
  CHECK_FALSE(rig.state().song.selecting());
  CHECK(rig.log.jumps == 0u);
  rig.pad(6);  // the song has slots 0..5
  CHECK(rig.state().song.pos() == 4);
  rig.pad(47);
  CHECK(rig.state().song.pos() == 4);
  CHECK(rig.log.jumps == 0u);
  // In TRACKS mode the same pad is a mute, not a slot
  rig.shift_tap(Button::F7);
  rig.pad(kRowMute + 3);
  CHECK(rig.state().song.pos() == 4);
  CHECK(rig.flags(3) == model::kTrackMute);
}

TEST_CASE("song pads: SLOTS, playing, a tap queues the slot, a tap on the queued slot cancels it, the cursor stays") {
  Rig rig(6);
  rig.shift_tap(Button::F7);
  rig.tap(Button::Down);  // the cursor is on slot 1
  rig.play();
  rig.pad(3);
  CHECK(rig.log.jumps == 1u);
  CHECK(rig.log.jump_row == 3u);
  CHECK(rig.state().song.pos() == 1);
  rig.position.publish_queued(3);
  rig.pass(1);
  rig.pad(3);  // the queued slot: cancel
  CHECK(rig.log.jumps == 2u);
  CHECK(rig.log.jump_row == 0xFFFFu);
  rig.position.publish_queued(audio::PlayPosition::kNoOrder);
  rig.pass(1);
  rig.pad(5);
  CHECK(rig.log.jump_row == 5u);
  CHECK(rig.state().song.pos() == 1);
  // a pad past the end queues nothing; a sequencer that refuses (no ring room) changes nothing
  const unsigned before = rig.log.jumps;
  rig.pad(9);
  CHECK(rig.log.jumps == before);
  rig.log.accept = false;
  rig.pad(2);
  CHECK(rig.log.jumps == before);
}

TEST_CASE("song pads: the LEDs of SLOTS: existing slots Back, the playing slot Front, the queued slot blinks, the cursor's slot is Front when stopped, past the end Off") {
  Rig rig(6);
  PadRole roles[48];
  rig.shift_tap(Button::F7);
  rig.tap(Button::Down);
  rig.tap(Button::Down);  // the cursor is on slot 2
  NEED(rig.lights(roles));
  CHECK(roles[0] == PadRole::Back);
  CHECK(roles[2] == PadRole::Front);  // stopped: the cursor's slot
  CHECK(roles[5] == PadRole::Back);
  CHECK(roles[6] == PadRole::Off);
  CHECK(roles[47] == PadRole::Off);
  rig.position.publish(true, 2, 0, 12000, 4);  // the song plays slot 4
  rig.pass(1);
  NEED(rig.lights(roles));
  CHECK(roles[4] == PadRole::Front);
  CHECK(roles[2] == PadRole::Back);  // playing: the cursor's slot is not lit any more
  rig.position.publish_queued(1);
  rig.pass(1);
  PadRole first[48];
  NEED(rig.lights(first));
  CHECK((first[1] == PadRole::Front || first[1] == PadRole::Back));
  rig.pass(app::kBlinkMs);  // half a period of the blink: the phase flips whatever the clock's start
  PadRole half[48];
  NEED(rig.lights(half));
  CHECK(half[1] != first[1]);
  rig.pass(app::kBlinkMs);  // a whole period: back where it was
  NEED(rig.lights(roles));
  CHECK(roles[1] == first[1]);
  CHECK(roles[4] == PadRole::Front);
}

TEST_CASE("song pads: the SLOTS page is the 48 slots that hold the cursor and follows it") {
  Rig rig(100);
  rig.shift_tap(Button::F7);
  PadRole roles[48];
  NEED(rig.lights(roles));
  CHECK(roles[0] == PadRole::Front);  // slot 0 is the cursor's
  for (int i = 0; i < 60; ++i) {
    rig.tap(Button::Down);
  }
  CHECK(rig.state().song.pos() == 60);
  NEED(rig.lights(roles));
  CHECK(roles[12] == PadRole::Front);  // page 2 starts at slot 48: slot 60 is its pad 12
  CHECK(roles[47] == PadRole::Back);   // slot 95
  rig.pad(3);                          // slot 51
  CHECK(rig.state().song.pos() == 51);
  for (int i = 0; i < 60; ++i) {
    rig.tap(Button::Down);
  }
  CHECK(rig.state().song.pos() == 99);
  NEED(rig.lights(roles));
  CHECK(roles[3] == PadRole::Front);  // page 3: slots 96..99 only
  CHECK(roles[4] == PadRole::Off);
  rig.pad(2);
  CHECK(rig.state().song.pos() == 98);
}

TEST_CASE("song pads: a lone Shift hold shows the TRACKS layer in SLOTS for as long as it is held; a chord ends it") {
  Rig rig(6);
  rig.shift_tap(Button::F7);
  PadRole roles[48];
  NEED(rig.lights(roles));
  CHECK(roles[0] == PadRole::Front);
  CHECK(roles[kRowMute + 1] == PadRole::Off);  // slot 12 does not exist: SLOTS lights
  rig.press(Button::Shift);
  rig.pass(1);
  CHECK(rig.state().padmode.momentary());
  rig.pad(kRowMute + 2);  // the layer: a mute, the cursor stays
  CHECK(rig.flags(2) == model::kTrackMute);
  CHECK(rig.state().song.pos() == 0);
  NEED(rig.lights(roles));
  CHECK(roles[kRowMute + 2] == PadRole::Front);  // the TRACKS lights: muted by you
  rig.release(Button::Shift);
  rig.pass(1);
  rig.pad(3);  // SLOTS again
  CHECK(rig.state().song.pos() == 3);
  // a chord (an arrow under Shift) makes the layer yield for the rest of the hold: the pads are SLOTS then
  rig.press(Button::Shift);
  rig.pass(1);
  rig.tap(Button::Down);
  rig.pad(1);
  CHECK(rig.state().song.pos() == 1);
  CHECK(rig.flags(1) == 0);
  rig.release(Button::Shift);
  rig.pass(1);
}

TEST_CASE("song pads: the actions refuse outside Song and in the wrong mode") {
  Rig rig(6);
  CHECK(rig.app->invoke(core::action_id("song.pad"), 2) == DispatchResult::Unchanged);  // TRACKS mode: the layer has the pads, the SLOTS action does nothing
  CHECK(rig.state().song.pos() == 0);
  rig.tap(Button::Pattern);
  CHECK(rig.app->invoke(core::action_id("song.pads"), 0) == DispatchResult::Disabled);
  CHECK(rig.app->invoke(core::action_id("song.pad"), 2) == DispatchResult::Disabled);
}
