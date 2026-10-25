// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-UNDOGRP in the app: encoder clicks on a parameter row are one undo unit, F8 undoes the area of the screen it is pressed on (PATTERN, SONG, SOUND),
// the Mixer has no history, and undoing a pattern that another area made says PATTERN HISTORY CLEARED. The rings, the merge and the fence on their own are
// undo_area_test.cpp; the existing undo behaviour on one ring is history_app_test.cpp.
#include <doctest/doctest.h>

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/pattern_layout.h>
#include <o3os/app/screens/params_screen.h>
#include <o3os/app/screens/screen.h>
#include <o3os/app/view_state.h>
#include <o3os/core/action.h>
#include <o3os/core/input_event.h>
#include <o3os/core/param.h>
#include <o3os/model/editor.h>
#include <o3os/model/params.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/sim/demo_project.h>

#include <cstdint>
#include <memory>
#include <string>

#include "support/need.h"
#include "support/project_fixture.h"

using namespace o3os;
using app::Button;
using app::DispatchResult;
using app::PatternMode;
using core::ActionArg;
using core::ParamId;
using core::ParamKind;
using model::UndoArea;
namespace ip = model::instrument_param;

namespace {

alignas(8) std::uint8_t g_region[model::kUndoRegionBytes];

ParamId inst(unsigned slot, unsigned field) { return core::param_id(ParamKind::Instrument, slot, field); }
ParamId track(unsigned slot, unsigned field) { return core::param_id(ParamKind::Track, slot, field); }
ActionArg cc(unsigned channel, unsigned controller, unsigned value) { return static_cast<ActionArg>((channel << 16) | (controller << 8) | value); }

// An App on the demo project with the device's three-ring history.
struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoHistory history;
  std::unique_ptr<model::Editor> editor;
  core::ParamRing ring;

  Rig() {
    model::Project& p = project();
    sim::fill_demo_project(p);
    (void)p.append_order(1);  // two song positions, so one can go
    history.begin(g_region, sizeof g_region);
    editor = std::make_unique<model::Editor>(p, history);
    app->attach_document(editor.get());
    app->attach_param_ring(&ring);
    app->run_once();
  }
  model::Project& project() { return test::arena(1).project; }
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
  DispatchResult invoke(const char* name, ActionArg arg = 0) { return app->invoke(core::action_id(name), arg); }
  std::string message() const { return app->message(); }
  unsigned depth(UndoArea a) const { return editor->journal(a).depth(); }
  unsigned redo(UndoArea a) const { return editor->journal(a).redo_depth(); }
  float value(ParamId id) { return project().get_param(id); }
  // The Params screen on the FILTER page, the cursor on CUTOFF.
  void on_cutoff() {
    tap(Button::Params);
    (void)invoke("param.group", static_cast<ActionArg>(ip::GroupFilter));
    while (app::kParamsScreen.focus_param(app->model(), app->state()) != inst(0, ip::Cutoff)) {
      tap(Button::Down);
    }
  }
  void on_resonance() {
    tap(Button::Down);
    CHECK(app::kParamsScreen.focus_param(app->model(), app->state()) == inst(0, ip::Resonance));
  }
  // One encoder click of the row under the cursor, 50 ms after the last thing.
  void click(int detents = 1) {
    pass(50);
    CHECK(invoke("param.edit", detents) == DispatchResult::Changed);
  }
  app::ViewState& view() { return const_cast<app::ViewState&>(app->state()); }
  void flush() {
    pass(1);
    pass(10000);
  }
};

}  // namespace

TEST_CASE("undo groups: 48 encoder clicks on the cutoff are one unit; F8 returns to the value before the sweep and Shift + F8 to the last") {
  Rig rig;
  rig.on_cutoff();
  const float start = rig.value(inst(0, ip::Cutoff));
  for (int i = 0; i < 48; ++i) {
    rig.click();
  }
  const float last = rig.value(inst(0, ip::Cutoff));
  CHECK(last > start);
  CHECK(rig.depth(UndoArea::Sound) == 1u);
  CHECK(rig.depth(UndoArea::Pattern) == 0u);
  CHECK(rig.depth(UndoArea::Song) == 0u);
  core::ParamChange c;
  while (rig.ring.pop(c)) {
  }
  rig.tap(Button::F8);
  CHECK(rig.value(inst(0, ip::Cutoff)) == start);
  CHECK(rig.editor->applied_count() == 1u);  // one parameter went to the audio side
  CHECK(rig.ring.pop(c));
  CHECK((c.id == inst(0, ip::Cutoff) && c.value == start));
  CHECK_FALSE(rig.ring.pop(c));
  rig.shift_tap(Button::F8);
  CHECK(rig.value(inst(0, ip::Cutoff)) == last);
  rig.tap(Button::F8);
  rig.tap(Button::F8);
  CHECK(rig.message() == "NOTHING TO UNDO HERE");
}

TEST_CASE("undo groups: rows that alternate make a unit each, the cursor moving alone does not end a group, a sweep back to its start leaves nothing") {
  Rig rig;
  rig.on_cutoff();
  rig.click();
  rig.click();
  rig.on_resonance();
  rig.click();
  rig.tap(Button::Up);  // (navigation: nothing is written)
  rig.click();
  CHECK(rig.depth(UndoArea::Sound) == 3u);  // cutoff, resonance, cutoff again
  rig.click();
  CHECK(rig.depth(UndoArea::Sound) == 3u);
  // the cursor goes away and comes back: still the same group
  rig.tap(Button::Down);
  rig.tap(Button::Up);
  rig.click();
  CHECK(rig.depth(UndoArea::Sound) == 3u);
  // a sweep that comes back to its start
  const float before = rig.value(inst(0, ip::Cutoff));
  rig.pass(2000);
  rig.click(3);
  rig.click(2);
  rig.click(-5);
  CHECK(rig.value(inst(0, ip::Cutoff)) == doctest::Approx(before));
  CHECK(rig.depth(UndoArea::Sound) == 3u);
}

TEST_CASE("undo groups: another undoable action, an undo, a screen change and 1500 ms without a click each end a group") {
  {  // another undoable dispatch (it makes a unit of its own)
    Rig rig;
    rig.on_cutoff();
    rig.click();
    rig.click();
    CHECK(rig.depth(UndoArea::Sound) == 1u);
    CHECK(rig.invoke("project.tempo", 1) == DispatchResult::Changed);
    rig.click();
    CHECK(rig.depth(UndoArea::Sound) == 3u);
  }
  {  // an undo
    Rig rig;
    rig.on_cutoff();
    rig.click();
    rig.on_resonance();
    rig.click();
    rig.click();
    CHECK(rig.depth(UndoArea::Sound) == 2u);
    rig.tap(Button::F8);  // the resonance goes back
    CHECK(rig.depth(UndoArea::Sound) == 1u);
    rig.tap(Button::Up);
    rig.click();  // not part of the unit that was undone, nor of the one before it
    CHECK(rig.depth(UndoArea::Sound) == 2u);
  }
  {  // a screen change, and back
    Rig rig;
    rig.on_cutoff();
    rig.click();
    rig.tap(Button::Pattern);
    rig.tap(Button::Params);
    CHECK(rig.app->state().screen == app::ScreenId::Instrument);
    rig.click();
    CHECK(rig.depth(UndoArea::Sound) == 2u);
  }
  {  // time
    Rig rig;
    rig.on_cutoff();
    rig.click();
    rig.pass(1400);
    rig.click();
    CHECK(rig.depth(UndoArea::Sound) == 1u);
    rig.pass(1400);  // (the timeout counts from the last click: a slow sweep goes on)
    rig.click();
    rig.pass(1600);
    rig.click();
    CHECK(rig.depth(UndoArea::Sound) == 2u);
  }
}

TEST_CASE("undo groups: a coarse click joins the fine ones; a reset does not merge") {
  Rig rig;
  rig.on_cutoff();
  rig.click();
  rig.pass(50);
  CHECK(rig.invoke("param.coarse", 1) == DispatchResult::Changed);
  CHECK(rig.depth(UndoArea::Sound) == 1u);
  rig.pass(50);
  CHECK(rig.invoke("param.reset") == DispatchResult::Changed);
  CHECK(rig.depth(UndoArea::Sound) == 2u);
  rig.click(-1);  // (the default is the top of the range)
  CHECK(rig.depth(UndoArea::Sound) == 3u);
}

TEST_CASE("undo groups: a MIDI CC sweep is one unit by the parameter it resolved to, on whichever screen; a CC on a Mixer parameter writes no record") {
  Rig rig;
  CHECK(rig.editor->set_learn_slot(0, core::LearnBinding{inst(0, ip::Cutoff), 74, 0, static_cast<std::uint8_t>(core::LearnMode::Jump), 0}) == model::Status::Ok);
  CHECK(rig.app->device_settings().set_learn(0, core::LearnBinding{track(2, model::track_param::Volume), 7, 0, static_cast<std::uint8_t>(core::LearnMode::Jump), 0}) == model::Status::Ok);
  rig.editor->history_reset();
  CHECK(rig.app->state().screen == app::ScreenId::Pattern);
  const float start = rig.value(inst(0, ip::Cutoff));
  for (unsigned v = 10; v < 100; v += 5) {
    rig.pass(30);
    (void)rig.invoke("midi.cc", cc(1, 74, v));
  }
  CHECK(rig.value(inst(0, ip::Cutoff)) != start);
  CHECK(rig.depth(UndoArea::Sound) == 1u);
  CHECK(rig.depth(UndoArea::Pattern) == 0u);
  // a CC on the Mixer's parameter is applied, writes nothing, and does not end the group of the sweep
  const float vol = rig.value(track(2, model::track_param::Volume));
  rig.pass(30);
  CHECK(rig.invoke("midi.cc", cc(1, 7, 127)) == DispatchResult::Changed);
  CHECK(rig.value(track(2, model::track_param::Volume)) != vol);
  CHECK(rig.depth(UndoArea::Sound) == 1u);
  rig.pass(30);
  (void)rig.invoke("midi.cc", cc(1, 74, 120));
  CHECK(rig.depth(UndoArea::Sound) == 1u);
  // the same CC from the Params screen
  rig.tap(Button::Params);
  rig.pass(30);
  (void)rig.invoke("midi.cc", cc(1, 74, 30));
  CHECK(rig.depth(UndoArea::Sound) == 2u);  // (the screen change ended the group)
  rig.pass(30);
  (void)rig.invoke("midi.cc", cc(1, 74, 31));
  CHECK(rig.depth(UndoArea::Sound) == 2u);
  // one F8 on Params takes the sweep back, and the Mixer's volume stays where the controller put it
  const float vol_now = rig.value(track(2, model::track_param::Volume));
  rig.tap(Button::F8);
  CHECK(rig.value(track(2, model::track_param::Volume)) == vol_now);
  CHECK(rig.depth(UndoArea::Sound) == 1u);
}

TEST_CASE("undo areas: the Mixer has no undo, its edits write no unit in any area and F8 there stays SOLO") {
  Rig rig;
  rig.tap(Button::Master);
  CHECK(rig.app->state().screen == app::ScreenId::Mixer);
  // (per action, not per screen: an app's tempo asked for while the Mixer is in view is a unit like anywhere)
  CHECK(rig.invoke("module.app_tempo", 15000) == DispatchResult::Changed);
  CHECK(rig.depth(UndoArea::Song) == 1u);
  rig.editor->history_reset();
  float before[4];
  for (unsigned t = 0; t < 4; ++t) {
    before[t] = rig.value(track(t, model::track_param::Volume));
  }
  CHECK(rig.invoke("mixer.edit", -3) == DispatchResult::Changed);
  CHECK(rig.invoke("mixer.coarse", -1) == DispatchResult::Changed);
  float sum = 0.0f;
  for (unsigned t = 0; t < 4; ++t) {
    sum += rig.value(track(t, model::track_param::Volume)) - before[t];
  }
  CHECK(sum < 0.0f);  // the strip under the cursor moved
  CHECK(rig.depth(UndoArea::Pattern) == 0u);
  CHECK(rig.depth(UndoArea::Song) == 0u);
  CHECK(rig.depth(UndoArea::Sound) == 0u);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.message() == "NOTHING TO UNDO HERE");
}

TEST_CASE("undo areas: F8 undoes only its screen's area, so a Song slot delete stays undoable from Song however many Pattern and Params edits follow") {
  Rig rig;
  rig.tap(Button::Song);
  CHECK(rig.project().song_length() == 2u);
  CHECK(rig.invoke("song.delete_slot") == DispatchResult::Changed);
  CHECK(rig.project().song_length() == 1u);
  CHECK(rig.depth(UndoArea::Song) == 1u);
  rig.tap(Button::Pattern);
  (void)rig.invoke("view.mode", static_cast<ActionArg>(PatternMode::Edit));
  CHECK(rig.invoke("step.note", 61) == DispatchResult::Changed);
  CHECK(rig.invoke("step.note", 62) == DispatchResult::Changed);
  rig.on_cutoff();
  rig.click();
  CHECK(rig.depth(UndoArea::Pattern) == 2u);
  CHECK(rig.depth(UndoArea::Song) == 1u);
  CHECK(rig.depth(UndoArea::Sound) == 1u);
  CHECK(rig.app->model().undo_depth == 2u);  // the Pattern info line: the PATTERN area's depth
  // Params: one undo, then nothing more to undo there
  const float cut = rig.value(inst(0, ip::Cutoff));
  rig.tap(Button::F8);
  CHECK(rig.value(inst(0, ip::Cutoff)) != cut);
  rig.tap(Button::F8);
  CHECK(rig.message() == "NOTHING TO UNDO HERE");
  CHECK(rig.project().song_length() == 1u);
  // Pattern: its two notes, and then nothing
  rig.tap(Button::Pattern);
  rig.tap(Button::F8);
  rig.tap(Button::F8);
  rig.flush();
  rig.tap(Button::F8);
  CHECK(rig.message() == "NOTHING TO UNDO HERE");
  CHECK(rig.project().song_length() == 1u);
  // Song: the slot comes back; its redo is the Song's
  rig.tap(Button::Song);
  rig.tap(Button::F8);
  CHECK(rig.project().song_length() == 2u);
  CHECK(rig.redo(UndoArea::Song) == 1u);
  CHECK(rig.redo(UndoArea::Pattern) == 2u);
  rig.shift_tap(Button::F8);
  CHECK(rig.project().song_length() == 1u);
  // a new edit in one area drops that area's redo units only
  rig.tap(Button::Pattern);
  CHECK(rig.invoke("step.note", 63) == DispatchResult::Changed);
  CHECK(rig.redo(UndoArea::Pattern) == 0u);
  CHECK(rig.redo(UndoArea::Sound) == 1u);
}

TEST_CASE("undo fence: undoing a pattern that Song made, after Pattern edits to it, clears the PATTERN history and says so") {
  Rig rig;
  rig.tap(Button::Song);
  const unsigned before = rig.project().pattern_count();
  unsigned fresh = 1;
  while (rig.project().pattern(fresh) != nullptr) {
    ++fresh;  // the lowest free number: the one the copy takes
  }
  CHECK(rig.invoke("song.add_slot", 1) == DispatchResult::Changed);  // NEW: a copy of the cursor's pattern, in a new slot
  CHECK(rig.project().pattern_count() == before + 1u);
  CHECK(rig.depth(UndoArea::Song) == 1u);
  CHECK(rig.project().pattern(fresh) != nullptr);
  rig.tap(Button::Pattern);
  rig.view().edit.pattern = static_cast<std::uint8_t>(fresh);
  rig.pass(1);
  (void)rig.invoke("view.mode", static_cast<ActionArg>(PatternMode::Edit));
  CHECK(rig.invoke("step.note", 64) == DispatchResult::Changed);
  CHECK(rig.depth(UndoArea::Pattern) == 1u);
  rig.tap(Button::Song);
  rig.flush();
  rig.tap(Button::F8);
  CHECK(rig.project().pattern(fresh) == nullptr);
  CHECK(rig.message() == "PATTERN HISTORY CLEARED");
  CHECK(rig.depth(UndoArea::Pattern) == 0u);
  CHECK(rig.redo(UndoArea::Pattern) == 0u);
}

TEST_CASE("undo fence: undoing a Song edit that made no pattern says nothing and leaves the PATTERN history") {
  Rig rig;
  rig.tap(Button::Pattern);
  (void)rig.invoke("view.mode", static_cast<ActionArg>(PatternMode::Edit));
  CHECK(rig.invoke("step.note", 61) == DispatchResult::Changed);
  rig.tap(Button::Song);
  CHECK(rig.invoke("song.add_slot") == DispatchResult::Changed);
  rig.flush();
  rig.tap(Button::F8);
  CHECK(rig.message().empty());
  CHECK(rig.depth(UndoArea::Pattern) == 1u);
}

namespace {

// What the device and the simulator do for File > NEW: the document is replaced, then the history ends.
class NewSink final : public app::ProjectSink {
 public:
  explicit NewSink(model::Editor& editor) : editor_(editor) {}
  Start open(const char*, Format) noexcept override { return Start::NoDocument; }
  Start start_new() noexcept override {
    sim::fill_demo_project(test::arena(1).project);
    editor_.history_reset();
    ++news;
    return Start::Started;
  }
  bool busy() const noexcept override { return false; }
  const char* folder() const noexcept override { return ""; }
  bool loaded() const noexcept override { return true; }
  const char* detail() const noexcept override { return ""; }
  unsigned news = 0;

 private:
  model::Editor& editor_;
};

}  // namespace

TEST_CASE("undo areas: NEW clears the history of every area, and nothing can be undone after it") {
  Rig rig;
  NewSink sink(*rig.editor);
  rig.app->attach_project_sink(&sink);
  rig.tap(Button::Song);
  CHECK(rig.invoke("song.delete_slot") == DispatchResult::Changed);
  rig.tap(Button::Pattern);
  (void)rig.invoke("view.mode", static_cast<ActionArg>(PatternMode::Edit));
  CHECK(rig.invoke("step.note", 61) == DispatchResult::Changed);
  rig.on_cutoff();
  rig.click();
  CHECK(rig.depth(UndoArea::Song) == 1u);
  CHECK(rig.depth(UndoArea::Pattern) == 1u);
  CHECK(rig.depth(UndoArea::Sound) == 1u);
  rig.tap(Button::File);
  (void)rig.invoke("file.new");
  (void)rig.invoke("file.new");
  CHECK(sink.news == 1u);
  CHECK_FALSE(rig.editor->has_history());
  for (const UndoArea a : {UndoArea::Pattern, UndoArea::Song, UndoArea::Sound}) {
    CHECK_FALSE(rig.editor->can_undo(a));
    CHECK_FALSE(rig.editor->can_redo(a));
  }
}
