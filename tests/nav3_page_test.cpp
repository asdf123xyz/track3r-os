// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-NAV3: Shift + Up / Down move a list by one page (the rows it shows), clamped, never wrapping, in the File screen (PROJECTS and IMPORT), Sample load, Learn and the step popup's
// instrument list; the Modules screen and the engine picker are in module_ui_test.cpp (they need its module rig). The bindings themselves, and that every other Shift + arrow keeps
// its meaning, are builtin_bindings_test.cpp. Keys are pressed for real, through the App.
// OS-SHIFTENC: Shift + encoder is the same page, a detent a page (the second half of the file: the same contract through the encoder).
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/file_view.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/core/action.h>
#include <o3os/core/input_event.h>
#include <o3os/core/learn.h>
#include <o3os/model/params.h>
#include <o3os/platform/host/fake_storage.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/ui/theme.h>

#include <functional>
#include <memory>
#include <string>

using namespace o3os;
using app::Button;
using app::ScreenId;

namespace {

// What the File screen's F1 needs to be available (it enters a folder on the IMPORT page; nothing here opens or imports).
class NullSink final : public app::ProjectSink {
 public:
  Start open(const char*, Format) noexcept override { return Start::NoCard; }
  Start start_new() noexcept override { return Start::NoCard; }
  bool busy() const noexcept override { return false; }
  const char* folder() const noexcept override { return ""; }
  bool loaded() const noexcept override { return true; }
  const char* detail() const noexcept override { return ""; }
};

struct Rig {
  NullSink project;
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  platform::host::FakeStorage card;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);

  Rig() {
    app->attach_storage(&card);
    app->attach_project_sink(&project);
  }
  void pass() {
    clock.advance(16000);
    app->run_once();
  }
  void settle() {
    for (unsigned i = 0; i < 2000; ++i) {
      pass();
      const app::DirScan::State st = app->model().files->state();
      if (st != app::DirScan::State::Scanning && st != app::DirScan::State::Idle) {
        return;
      }
    }
    FAIL_CHECK("the scan did not end");
  }
  void tap(Button b) {
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true));
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false));
    pass();
  }
  // Shift held, `b` tapped, Shift let go.
  void shift_tap(Button b) {
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(Button::Shift), true));
    pass();
    tap(b);
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(Button::Shift), false));
    pass();
  }
  void go(ScreenId id) {
    (void)app->invoke(core::action_id("nav.screen"), static_cast<core::ActionArg>(id));
    pass();
  }
  // OS-SHIFTENC: Shift held, the encoder turned `detents`, Shift let go.
  void shift_turn(int detents) {
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(Button::Shift), true));
    pass();
    input.push(core::make_encoder_event(clock.now(), 0, static_cast<std::int16_t>(detents)));
    pass();
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(Button::Shift), false));
    pass();
  }
  // One page down (+1) or up (-1) by Shift + Down / Up, or by Shift + the encoder.
  void page(int dir, bool encoder) {
    if (encoder) {
      shift_turn(dir);
    } else {
      shift_tap(dir > 0 ? Button::Down : Button::Up);
    }
  }
  const app::ViewState& state() const { return app->state(); }
};

// The contract of OS-NAV3 section 5 for a list of `n` entries, `page` rows, whose cursor `at` reads (0-based): Shift + Down from the first entry lands on the first of the next page, near the
// end stops on the last entry, Shift + Up stops on the first, a list shorter than a page goes to its ends, and an empty list does nothing.
void check_paging(Rig& rig, int n, int page, const std::function<int()>& at, const char* what, bool encoder = false) {
  CAPTURE(what);
  CHECK(at() == 0);
  if (n == 0) {
    rig.page(1, encoder);
    CHECK(at() == 0);
    rig.page(-1, encoder);
    CHECK(at() == 0);
    return;
  }
  if (n <= page) {
    rig.page(1, encoder);
    CHECK(at() == n - 1);
    rig.page(1, encoder);
    CHECK(at() == n - 1);  // (clamps)
    rig.page(-1, encoder);
    CHECK(at() == 0);
    rig.page(-1, encoder);
    CHECK(at() == 0);
    return;
  }
  NEED(n > 2 * page);  // (the tests below use a list of more than two pages)
  rig.page(1, encoder);
  CHECK(at() == page);  // the first entry of the next page
  rig.page(1, encoder);
  CHECK(at() == 2 * page);
  for (int guard = 0; guard < n && at() != n - 1; ++guard) {
    rig.page(1, encoder);
  }
  CHECK(at() == n - 1);  // stops on the last entry
  rig.page(1, encoder);
  CHECK(at() == n - 1);  // and does not wrap
  const int back = n - 1 - page;
  rig.page(-1, encoder);
  CHECK(at() == back);
  for (int guard = 0; guard < n && at() != 0; ++guard) {
    rig.page(-1, encoder);
  }
  CHECK(at() == 0);  // stops on the first entry
  rig.page(-1, encoder);
  CHECK(at() == 0);
}

void add_projects(Rig& rig, unsigned n) {
  for (unsigned i = 0; i < n; ++i) {
    const std::string name = "P" + std::to_string(100 + i);
    rig.card.add_dir("Projects/" + name);
    rig.card.add_file("Projects/" + name + "/project.mt", 2320);
  }
}

}  // namespace

TEST_CASE("nav3: Shift + Up / Down page the File screen's PROJECTS list, and keep the window on the cursor") {
  Rig rig;
  add_projects(rig, 40);
  rig.go(ScreenId::File);
  rig.settle();
  NEED(rig.app->model().files->index().count() == 40u);
  check_paging(rig, 40, app::FileView::kVisibleRows, [&] { return rig.state().file.pos(); }, "projects");
  // the window follows: the cursor is one of the rows shown
  rig.shift_tap(Button::Down);
  rig.shift_tap(Button::Down);
  rig.shift_tap(Button::Down);
  CHECK(rig.state().file.pos() == 39);
  CHECK(rig.state().file.top() <= 39);
  CHECK(rig.state().file.top() + app::FileView::kVisibleRows > 39);
}

TEST_CASE("nav3: a PROJECTS list shorter than a page goes to its ends, an empty one does nothing") {
  Rig few;
  add_projects(few, 5);
  few.go(ScreenId::File);
  few.settle();
  check_paging(few, 5, app::FileView::kVisibleRows, [&] { return few.state().file.pos(); }, "5 projects");
  Rig none;  // no Projects folder at all
  none.go(ScreenId::File);
  none.settle();
  NEED(none.app->model().files->index().count() == 0u);
  check_paging(none, 0, app::FileView::kVisibleRows, [&] { return none.state().file.pos(); }, "empty");
}

TEST_CASE("nav3: Shift + Up / Down page the IMPORT page, in a folder of module files") {
  Rig rig;
  rig.card.add_dir("MOD");
  for (unsigned i = 0; i < 40; ++i) {
    rig.card.add_file("MOD/m" + std::to_string(100 + i) + ".mod", 2000);
  }
  rig.go(ScreenId::File);
  rig.settle();
  rig.tap(Button::F6);  // IMPORT, at the root of the card
  rig.settle();
  NEED(rig.state().import.page);
  NEED(rig.app->model().files->index().count() == 1u);  // the folder MOD
  rig.tap(Button::F1);                                   // enter it
  rig.settle();
  NEED(rig.app->model().files->index().count() == 40u);
  check_paging(rig, 40, app::FileView::kVisibleRows, [&] { return rig.state().file.pos(); }, "import");
}

TEST_CASE("nav3: Shift + Up / Down page the Sample load list") {
  Rig rig;
  for (unsigned i = 0; i < 40; ++i) {
    rig.card.add_file("s" + std::to_string(100 + i) + ".wav", 1000);
  }
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  NEED(rig.app->model().files->index().count() == 40u);
  check_paging(rig, 40, app::FileView::kVisibleRows, [&] { return rig.state().wav.list.pos(); }, "sample load");
  Rig few;
  few.card.add_file("a.wav", 1000);
  few.card.add_file("b.wav", 1000);
  few.go(ScreenId::SampleLoad);
  few.settle();
  check_paging(few, 2, app::FileView::kVisibleRows, [&] { return few.state().wav.list.pos(); }, "2 samples");
  Rig none;
  none.go(ScreenId::SampleLoad);
  none.settle();
  check_paging(none, 0, app::FileView::kVisibleRows, [&] { return none.state().wav.list.pos(); }, "no samples");
}

TEST_CASE("nav3: Shift + Up / Down page the Learn list, five rows at a time") {
  Rig rig;
  const auto bind = [&](unsigned slot, unsigned track) {
    CHECK(rig.app->device_settings().set_learn(slot, core::LearnBinding{core::param_id(core::ParamKind::Track, track, model::track_param::Volume), static_cast<std::uint8_t>(20 + slot), 0,
                                                                        static_cast<std::uint8_t>(core::LearnMode::Jump), 0}) == model::Status::Ok);
  };
  for (unsigned i = 0; i < 12; ++i) {
    bind(i, i);
  }
  (void)rig.app->invoke(core::action_id("learn.toggle"));
  rig.pass();
  NEED(rig.state().overlay == app::OverlayId::Learn);
  check_paging(rig, 12, 5, [&] { return static_cast<int>(rig.state().learn.cursor); }, "learn");
  CHECK(rig.state().overlay == app::OverlayId::Learn);
}

TEST_CASE("nav3: Shift + Up / Down page the step popup's instrument list by the rows it shows, and never wrap") {
  Rig rig;
  rig.tap(Button::Params);
  NEED(rig.state().screen == ScreenId::Instrument);
  (void)rig.app->invoke(core::action_id("popup.open"), app::StepPopupView::Instr);
  rig.pass();
  NEED(rig.state().overlay == app::OverlayId::StepPopup);
  const int rows = app::step_popup_list_rows(ui::theme(rig.state().theme), app::StepPopupView::Instr);
  CHECK((rows >= 8 && rows <= 16));
  const int first = rig.state().instrument;
  CHECK(first == 1);
  rig.shift_tap(Button::Down);
  CHECK(rig.state().instrument == first + rows);
  rig.shift_tap(Button::Up);
  CHECK(rig.state().instrument == first);
  rig.shift_tap(Button::Up);
  CHECK(rig.state().instrument == first);  // clamps at the first
  for (int i = 0; i < 64; ++i) {
    rig.shift_tap(Button::Down);
  }
  CHECK(rig.state().instrument == 64);  // the last of the 64
  rig.shift_tap(Button::Down);
  CHECK(rig.state().instrument == 64);  // and no wrap
  rig.shift_tap(Button::Up);
  CHECK(rig.state().instrument == 64 - rows);
  CHECK(rig.state().overlay == app::OverlayId::StepPopup);
}

TEST_CASE("nav3: the FX list pages too, by the rows left over by its description, and the Note panel has no list") {
  Rig rig;
  rig.tap(Button::Pattern);
  (void)rig.app->invoke(core::action_id("popup.open"), app::StepPopupView::Fx);
  rig.pass();
  NEED(rig.state().overlay == app::OverlayId::StepPopup);
  const int rows = app::step_popup_list_rows(ui::theme(rig.state().theme), app::StepPopupView::Fx);
  CHECK((rows >= 4 && rows < app::step_popup_list_rows(ui::theme(rig.state().theme), app::StepPopupView::Instr)));
  const int at = rig.state().popup.item;
  CHECK(at >= 0);
  rig.shift_tap(Button::Down);
  CHECK(rig.state().popup.item == at + rows);
  rig.shift_tap(Button::Up);
  CHECK(rig.state().popup.item == at);
  CHECK(app::step_popup_list_rows(ui::theme(rig.state().theme), app::StepPopupView::Note) == 0);
}

// ---- OS-SHIFTENC: Shift + encoder pages the same lists ----------------------------------------------------------------------------------------------------------------------------

TEST_CASE("OS-SHIFTENC: Shift + encoder pages the PROJECTS list, the Sample load list and Learn, as Shift + Down / Up do, and clamps at both ends") {
  Rig rig;
  add_projects(rig, 40);
  rig.go(ScreenId::File);
  rig.settle();
  NEED(rig.app->model().files->index().count() == 40u);
  check_paging(rig, 40, app::FileView::kVisibleRows, [&] { return rig.state().file.pos(); }, "projects", true);
  // two detents are two pages
  rig.shift_turn(-3);
  CHECK(rig.state().file.pos() == 0);
  rig.shift_turn(2);
  CHECK(rig.state().file.pos() == 2 * app::FileView::kVisibleRows);
  rig.shift_turn(-1);
  CHECK(rig.state().file.pos() == app::FileView::kVisibleRows);

  Rig sl;
  for (unsigned i = 0; i < 40; ++i) {
    sl.card.add_file("s" + std::to_string(100 + i) + ".wav", 1000);
  }
  sl.go(ScreenId::SampleLoad);
  sl.settle();
  NEED(sl.app->model().files->index().count() == 40u);
  check_paging(sl, 40, app::FileView::kVisibleRows, [&] { return sl.state().wav.list.pos(); }, "sample load", true);

  Rig lr;
  for (unsigned i = 0; i < 12; ++i) {
    CHECK(lr.app->device_settings().set_learn(i, core::LearnBinding{core::param_id(core::ParamKind::Track, i, model::track_param::Volume), static_cast<std::uint8_t>(20 + i), 0,
                                                                    static_cast<std::uint8_t>(core::LearnMode::Jump), 0}) == model::Status::Ok);
  }
  (void)lr.app->invoke(core::action_id("learn.toggle"));
  lr.pass();
  NEED(lr.state().overlay == app::OverlayId::Learn);
  check_paging(lr, 12, 5, [&] { return static_cast<int>(lr.state().learn.cursor); }, "learn", true);
}

TEST_CASE("OS-SHIFTENC: Shift + encoder pages the Navigator (a page is the whole grid), and a plain turn still steps by one") {
  Rig rig;
  rig.go(ScreenId::Pattern);
  (void)rig.app->invoke(core::action_id("navigator.open"));
  rig.pass();
  NEED(rig.state().overlay == app::OverlayId::Navigator);
  const int plain0 = rig.state().navigator;
  rig.input.push(core::make_encoder_event(rig.clock.now(), 0, 1));
  rig.pass();
  CHECK(rig.state().navigator == plain0 + 1);  // unchanged: one a detent
  rig.shift_turn(1000);                              // a huge turn only clamps
  const int last = rig.state().navigator;
  CHECK(last > plain0 + 1);
  rig.shift_turn(1);
  CHECK(rig.state().navigator == last);  // the end of the list: no wrap
  rig.shift_turn(-1000);
  CHECK(rig.state().navigator == 0);
  rig.shift_turn(-1);
  CHECK(rig.state().navigator == 0);
  CHECK(rig.state().overlay == app::OverlayId::Navigator);
}

TEST_CASE("OS-SHIFTENC: Shift + encoder in the step popup's lists is a page, the Note panel has none") {
  Rig rig;
  rig.tap(Button::Params);
  NEED(rig.state().screen == ScreenId::Instrument);
  (void)rig.app->invoke(core::action_id("popup.open"), app::StepPopupView::Instr);
  rig.pass();
  NEED(rig.state().overlay == app::OverlayId::StepPopup);
  const int rows = app::step_popup_list_rows(ui::theme(rig.state().theme), app::StepPopupView::Instr);
  rig.input.push(core::make_encoder_event(rig.clock.now(), 0, 1));
  rig.pass();
  CHECK(rig.state().instrument == 2);  // the plain turn: one
  rig.shift_turn(1);
  CHECK(rig.state().instrument == 2 + rows);
  rig.shift_turn(-1000);
  CHECK(rig.state().instrument == 1);  // the top
  rig.shift_turn(1000);
  CHECK(rig.state().instrument == 64);  // the bottom
  rig.shift_turn(1);
  CHECK(rig.state().instrument == 64);
  CHECK(rig.state().overlay == app::OverlayId::StepPopup);
}
