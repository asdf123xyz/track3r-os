// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-002c in the app: undoable actions, F8 / Shift + F8 (undo and redo) on Pattern and Song, the editor that only an undoable
// action is handed, undo units named by the action, the macro recorder. The journal and the Editor themselves are undo_test.cpp.
#include <doctest/doctest.h>

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/model.h>
#include <o3os/app/session.h>
#include <o3os/app/view_state.h>
#include <o3os/core/action.h>
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
using core::ActionArg;
using core::ActionId;

namespace {

alignas(8) std::uint8_t g_ring[64 * 1024];

// An App with a document attached: the editor over a project in a static arena and a journal in a static ring.
struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;

  explicit Rig(bool with_document = true) {
    model::Project& p = test::arena(0).project;
    p.reset();
    if (with_document) {
      journal.begin(g_ring, sizeof g_ring);
      editor = std::make_unique<model::Editor>(p, journal);
      app->attach_document(editor.get());
    }
    app->run_once();
  }
  model::Project& project() { return test::arena(0).project; }
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
  // Lets every notification run out (the first pass starts the clock of the one that was just posted).
  void flush() {
    pass(1);
    pass(10000);
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
  std::uint32_t tempo() { return project().tempo_x100(); }
};

}  // namespace

TEST_CASE("history: project.tempo is undoable: F8 undoes it and Shift + F8 redoes it, on Pattern and on Song") {
  Rig rig;
  CHECK(rig.tempo() == model::kDefaultTempoX100);
  CHECK(rig.invoke("project.tempo", 20) == DispatchResult::Changed);
  CHECK(rig.tempo() == model::kDefaultTempoX100 + 2000u);
  CHECK(rig.editor->can_undo());

  rig.tap(Button::F8);  // Pattern
  CHECK(rig.tempo() == model::kDefaultTempoX100);
  CHECK(rig.editor->can_redo());
  CHECK(rig.message() == "");  // a successful undo is silent: the screen says it
  rig.shift_tap(Button::F8);
  CHECK(rig.tempo() == model::kDefaultTempoX100 + 2000u);

  rig.tap(Button::Song);  // and Song: the same keys, the same document
  rig.tap(Button::F8);
  CHECK(rig.tempo() == model::kDefaultTempoX100);
  rig.shift_tap(Button::F8);
  CHECK(rig.tempo() == model::kDefaultTempoX100 + 2000u);

  // Nothing left to redo, and then nothing left to undo: said once, and counted when repeated.
  rig.shift_tap(Button::F8);
  CHECK(rig.message() == "NOTHING TO REDO HERE");
  rig.tap(Button::F8);
  rig.tap(Button::F8);
  rig.pass(2000);
  rig.tap(Button::F8);
  CHECK(rig.message() == "NOTHING TO UNDO HERE");
  CHECK(rig.tempo() == model::kDefaultTempoX100);
}

TEST_CASE("history: the editing is clamped to the model and a change of nothing leaves no unit") {
  Rig rig;
  CHECK(rig.invoke("project.tempo", 100000) == DispatchResult::Changed);
  CHECK(rig.tempo() == model::kMaxTempoX100);
  CHECK(rig.invoke("project.tempo", 5) == DispatchResult::Unchanged);  // already at the top
  CHECK(rig.editor->journal().depth() == 1u);                          // the second call left nothing
  CHECK(rig.invoke("project.tempo", -2147483647 - 1) == DispatchResult::Changed);
  CHECK(rig.tempo() == model::kMinTempoX100);
  CHECK(rig.editor->journal().depth() == 2u);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.tempo() == model::kMaxTempoX100);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.tempo() == model::kDefaultTempoX100);
}

TEST_CASE("history: without a document an undoable action is Disabled and undo says there is nothing to undo") {
  Rig rig(false);
  CHECK(rig.invoke("project.tempo", 5) == DispatchResult::Disabled);
  CHECK(rig.tempo() == model::kDefaultTempoX100);  // nothing was edited
  rig.tap(Button::F8);
  CHECK(rig.message() == "NOTHING TO UNDO HERE");
  rig.shift_tap(Button::F8);
  rig.pass(2000);
  CHECK(rig.message() == "NOTHING TO REDO HERE");

  // Attaching later works; detaching again returns to the bare state.
  model::UndoJournal journal;
  journal.begin(g_ring, sizeof g_ring);
  model::Editor editor(rig.project(), journal);
  rig.app->attach_document(&editor);
  CHECK(rig.invoke("project.tempo", 5) == DispatchResult::Changed);
  rig.app->attach_document(nullptr);
  CHECK(rig.invoke("project.tempo", 5) == DispatchResult::Disabled);
  rig.app->attach_document(&editor);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.tempo() == model::kDefaultTempoX100);
}

TEST_CASE("history: the overlays keep F8 for Back: it never undoes under Learn or the Navigator") {
  Rig rig;
  CHECK(rig.invoke("project.tempo", 10) == DispatchResult::Changed);
  rig.invoke("learn.toggle");
  rig.pass(1);
  CHECK(rig.app->learn());
  rig.tap(Button::F8);  // Back
  CHECK_FALSE(rig.app->learn());
  CHECK(rig.tempo() == model::kDefaultTempoX100 + 1000u);  // not undone
  rig.tap(Button::F8);
  CHECK(rig.tempo() == model::kDefaultTempoX100);          // with nothing over it, undone

  // The history actions themselves need the base screen's focus.
  rig.invoke("learn.toggle");
  rig.pass(1);
  CHECK(rig.invoke("edit.redo") == DispatchResult::Disabled);
  CHECK(rig.tempo() == model::kDefaultTempoX100);
}

TEST_CASE("history: a project changed behind the document's back: undo says so and does nothing") {
  Rig rig;
  CHECK(rig.invoke("project.tempo", 10) == DispatchResult::Changed);
  CHECK(rig.project().set_swing(60) == model::Status::Ok);  // not through the editor
  rig.tap(Button::F8);
  CHECK(rig.message() == "UNDO HISTORY RESET");
  CHECK(rig.app->state().notify.head().severity == core::Severity::Warn);
  CHECK(rig.tempo() == model::kDefaultTempoX100 + 1000u);  // what no longer matches was not applied
  CHECK_FALSE(rig.editor->can_undo());
}

// ---- the dispatcher: who gets the editor, and what is one unit ---------------------------------------------------------------

namespace {

model::Editor* g_seen_by_plain = reinterpret_cast<model::Editor*>(1);
model::Editor* g_seen_by_undoable = nullptr;
const ActionId kThree = core::action_id("t.three");

bool plain_action(app::ActionContext& ctx, ActionArg) noexcept {
  g_seen_by_plain = ctx.edit;
  return false;
}
bool three_edits(app::ActionContext& ctx, ActionArg arg) noexcept {
  g_seen_by_undoable = ctx.edit;
  if (ctx.edit == nullptr) {
    return false;
  }
  (void)ctx.edit->create_pattern(1, 16);
  (void)ctx.edit->set_step(1, 0, 0, model::Step{static_cast<std::uint8_t>(60 + arg), 1, 1, {{0, 0}, {0, 0}}});
  (void)ctx.edit->append_order(1);
  return true;
}
bool nothing_action(app::ActionContext& ctx, ActionArg) noexcept {
  return ctx.edit != nullptr && ctx.edit->set_tempo_x100(1) == model::Status::BadValue;  // rejected: no edit
}
bool outer_action(app::ActionContext& ctx, ActionArg) noexcept {
  // An undoable action that dispatches another one: still one unit, named by the outer.
  static const app::ActionDef inner[] = {app::make_action("t.three", three_edits, core::kActionUndoable)};
  (void)app::dispatch(app::ActionTable{inner, 1}, kThree, 5, ctx);
  (void)ctx.edit->set_tempo_x100(15000);
  return true;
}

}  // namespace

TEST_CASE("dispatch: only an undoable action is handed the editor, its edits are one unit, named by the action") {
  static const app::ActionDef table[] = {
      app::make_action("t.plain", plain_action),
      app::make_action("t.three", three_edits, core::kActionUndoable),
      app::make_action("t.nothing", nothing_action, core::kActionUndoable),
      app::make_action("t.outer", outer_action, core::kActionUndoable),
  };
  const app::ActionTable t{table, 4};
  CHECK(app::registry_valid(table, 4));

  Rig rig;
  auto model = std::make_unique<app::Model>();
  app::ViewState view;
  app::ActionContext ctx{*model, view, app::App::kSoftKeyLabels, 1000, true, &rig.app->session()};

  CHECK(app::dispatch(t, core::action_id("t.plain"), 0, ctx) == DispatchResult::Unchanged);
  CHECK(g_seen_by_plain == nullptr);  // the session has an editor, and the plain action was not given it
  CHECK(ctx.edit == nullptr);          // and nothing is left in the context afterwards

  CHECK(app::dispatch(t, kThree, 7, ctx) == DispatchResult::Changed);
  CHECK(g_seen_by_undoable == rig.editor.get());
  CHECK(ctx.edit == nullptr);
  CHECK(rig.editor->journal().depth() == 1u);  // three edits, one unit
  ActionId id = ActionId::None;
  ActionArg arg = 0;
  CHECK(rig.editor->journal().newest_undo(id, arg));
  CHECK(id == kThree);  // the unit remembers what the user did, and with what
  CHECK(arg == 7);
  CHECK(rig.project().pattern(1)->steps[0][0].note == 67);
  CHECK(rig.editor->undo() == model::UndoResult::Ok);
  CHECK(rig.project().pattern_count() == 0u);
  CHECK(rig.project().song_length() == 0u);

  // An undoable action whose edits are all rejected leaves nothing, and does not end the redo history.
  CHECK(rig.editor->can_redo());
  CHECK(app::dispatch(t, core::action_id("t.nothing"), 0, ctx) == DispatchResult::Changed);  // (its handler says so)
  CHECK(rig.editor->journal().depth() == 0u);
  CHECK(rig.editor->can_redo());

  // Nested: the inner action's edits and the outer's are the outer's one unit.
  CHECK(app::dispatch(t, core::action_id("t.outer"), 3, ctx) == DispatchResult::Changed);
  CHECK(rig.editor->journal().depth() == 1u);
  CHECK(rig.editor->journal().newest_undo(id, arg));
  CHECK(id == core::action_id("t.outer"));
  CHECK(rig.project().tempo_x100() == 15000u);
  CHECK(rig.editor->undo() == model::UndoResult::Ok);
  CHECK(rig.project().tempo_x100() == model::kDefaultTempoX100);
  CHECK(rig.project().pattern_count() == 0u);

  // With no editor in the session an undoable action is Disabled and a plain one still runs.
  app::Session bare;
  app::ActionContext ctx2{*model, view, app::App::kSoftKeyLabels, 1000, true, &bare};
  CHECK(app::dispatch(t, kThree, 0, ctx2) == DispatchResult::Disabled);
  CHECK(app::dispatch(t, core::action_id("t.plain"), 0, ctx2) == DispatchResult::Unchanged);
  app::ActionContext ctx3{*model, view, app::App::kSoftKeyLabels, 1000, true};  // and with no session at all
  CHECK(app::dispatch(t, kThree, 0, ctx3) == DispatchResult::Disabled);
  CHECK(app::dispatch(t, core::action_id("t.plain"), 0, ctx3) == DispatchResult::Unchanged);
}

// ---- macros -------------------------------------------------------------------------------------------------------------------

TEST_CASE("macro: record what was dispatched, play it back as one undo unit") {
  Rig rig;
  const app::Macro& m = rig.app->session().macro;
  CHECK(rig.invoke("macro.play") == DispatchResult::Changed);  // nothing recorded yet
  CHECK(rig.message() == "NO MACRO");
  rig.flush();

  CHECK(rig.invoke("macro.record") == DispatchResult::Changed);
  CHECK(m.recording);
  CHECK(rig.message() == "MACRO RECORDING");
  rig.invoke("project.tempo", 1);
  rig.invoke("nav.down");
  rig.invoke("nav.down");
  rig.invoke("edit.undo");     // the history and the macro's own controls are not recorded
  rig.invoke("edit.redo");
  rig.invoke("macro.play");    // (refused while recording)
  CHECK(m.count == 3u);
  CHECK(rig.invoke("macro.record") == DispatchResult::Changed);  // stop
  CHECK_FALSE(m.recording);
  CHECK(m.count == 3u);
  CHECK(m.steps[0].action == core::action_id("project.tempo"));
  CHECK(m.steps[0].arg == 1);
  CHECK(m.steps[1].action == core::action_id("nav.down"));
  CHECK(rig.tempo() == model::kDefaultTempoX100 + 100u);
  const int row = rig.app->view().row();
  rig.flush();
  CHECK(rig.message() == "");  // (the strip of "MACRO: 3 STEPS" has run out)

  // Play: the tempo goes up again, the cursor goes down again, and one undo takes the whole macro back.
  rig.pass(0);
  const std::size_t depth = rig.editor->journal().depth();
  CHECK(rig.invoke("macro.play") == DispatchResult::Changed);
  CHECK(rig.tempo() == model::kDefaultTempoX100 + 200u);
  CHECK(rig.app->view().row() == row + 2);
  CHECK(rig.editor->journal().depth() == depth + 1u);
  ActionId id = ActionId::None;
  ActionArg arg = 0;
  CHECK(rig.editor->journal().newest_undo(id, arg));
  CHECK(id == core::action_id("macro.play"));
  CHECK(rig.invoke("macro.play") == DispatchResult::Changed);
  CHECK(rig.tempo() == model::kDefaultTempoX100 + 300u);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.tempo() == model::kDefaultTempoX100 + 200u);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.tempo() == model::kDefaultTempoX100 + 100u);
  CHECK(m.count == 3u);  // playing and undoing do not touch the recording

  // Recording again starts from empty.
  rig.flush();
  rig.invoke("macro.record");
  CHECK(m.count == 0u);
  rig.invoke("nav.up");
  rig.flush();  // ("MACRO RECORDING" has run out)
  rig.invoke("macro.record");
  CHECK(m.count == 1u);
  CHECK(rig.message() == "MACRO: 1 STEP");
}

TEST_CASE("macro: it stops by itself when it is full, and a macro replayed without a document skips its edits") {
  Rig rig;
  const app::Macro& m = rig.app->session().macro;
  rig.invoke("macro.record");
  for (unsigned i = 0; i < app::kMacroSteps; ++i) {
    rig.invoke("nav.down");
  }
  CHECK(m.recording);
  CHECK(m.count == app::kMacroSteps);
  rig.invoke("nav.down");  // one too many
  CHECK_FALSE(m.recording);
  CHECK(m.full);
  CHECK(m.count == app::kMacroSteps);
  CHECK(rig.app->state().notify.size() >= 1u);
  rig.flush();

  // A short macro with an edit in it, played with the document detached: the edit is skipped, the rest still runs.
  rig.invoke("macro.record");
  rig.invoke("project.tempo", 3);
  rig.invoke("nav.up");
  rig.invoke("macro.record");
  CHECK(m.count == 2u);
  const std::uint32_t tempo = rig.tempo();
  rig.app->attach_document(nullptr);
  rig.invoke("cursor.jump_fraction", 1);  // off the first row: a fresh Up there wraps (OS-ROWWRAP)
  const int row = rig.app->view().row();
  CHECK(row > 0);
  CHECK(rig.invoke("macro.play") == DispatchResult::Changed);
  CHECK(rig.tempo() == tempo);
  CHECK(rig.app->view().row() == row - 1);
}
