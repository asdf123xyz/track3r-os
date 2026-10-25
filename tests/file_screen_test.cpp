// SPDX-License-Identifier: GPL-3.0-or-later
// OS-010a: the File screen on the App: the scan runs as a job beside the screen (a bounded amount of card work in a pass, no notification strip), the
// cursor stays on its entry while the listing grows, F1 opens the project under the cursor through the device's load (a ProjectSink) and the result
// is told, F2 reads the folder again, Left / Right jump by first letter, a failed scan is retried when the screen is entered again. The browser's own
// rules are in dir_index_test.cpp and dir_scan_test.cpp.
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/dir_index.h>
#include <o3os/app/text_format.h>
#include <o3os/app/screens/file_screen.h>
#include <o3os/core/action.h>
#include <o3os/core/input_event.h>
#include <o3os/platform/host/fake_storage.h>
#include <o3os/platform/host/fakes.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

using namespace o3os;
using app::Button;
using app::ScreenId;

namespace {

class FakeProject final : public app::ProjectSink {
 public:
  Start result = Start::Started;
  bool is_busy = false;
  bool ok = true;
  std::string words;
  std::string last_folder;
  unsigned opens = 0;
  Format last_format = Format::Og;
  unsigned news = 0;
  Start new_result = Start::Started;
  Start start_new() noexcept override {
    ++news;
    if (new_result == Start::Started) {
      last_folder.clear();
      is_busy = true;
    }
    return new_result;
  }
  Start open(const char* folder, Format format) noexcept override {
    ++opens;
    last_format = format;
    if (result == Start::Started) {
      last_folder = folder;
      is_busy = true;
    }
    return result;
  }
  bool busy() const noexcept override { return is_busy; }
  const char* folder() const noexcept override { return last_folder.c_str(); }
  bool loaded() const noexcept override { return ok; }
  const char* detail() const noexcept override { return words.c_str(); }
};

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  platform::host::FakeStorage card;
  FakeProject project;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);

  Rig() {
    app->attach_storage(&card);
    app->attach_project_sink(&project);
  }
  void pass() {
    clock.advance(16000);
    app->run_once();
  }
  void passes(unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
      pass();
    }
  }
  // Passes until the scan has ended (a bound of the test's own).
  void settle() {
    for (unsigned i = 0; i < 2000; ++i) {
      pass();
      if (app->model().files->state() != app::DirScan::State::Scanning && app->model().files->state() != app::DirScan::State::Idle) {
        return;
      }
    }
    FAIL_CHECK("the scan did not end");
  }
  // Lets the notifications on screen run out (their time is the clock's uptime).
  void expire() {
    for (unsigned i = 0; i < 10 && !std::string(app->message()).empty(); ++i) {
      clock.advance(3000000);
      pass();
    }
  }
  void tap(Button b) {
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true));
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false));
    pass();
  }
  void go(ScreenId id) {
    (void)app->invoke(core::action_id("nav.screen"), static_cast<core::ActionArg>(id));
    pass();
  }
  void add_project(const std::string& name) {
    card.add_dir("Projects/" + name);
    card.add_file("Projects/" + name + "/project.mt", 2320);
  }
  const app::DirIndex& index() const { return app->model().files->index(); }
  std::string name_at_cursor() const { return index().name(static_cast<unsigned>(app->state().file.pos())); }
};

}  // namespace

TEST_CASE("file screen: it is a screen of the registry, reached by its button") {
  Rig rig;
  const app::ScreenEntry* e = app::find_screen(app::screen_registry(), ScreenId::File);
  NEED(e != nullptr);
  CHECK(e->screen == &app::kFileScreen);
  rig.tap(Button::File);
  CHECK(rig.app->state().screen == ScreenId::File);
  CHECK(rig.app->frames() >= 1u);
}

TEST_CASE("file screen: the scan is a job beside the screen: bounded card work a pass, a frame a pass, no strip") {
  Rig rig;
  for (unsigned i = 0; i < 40; ++i) {
    rig.add_project("song-" + std::to_string(100 + i));
  }
  rig.pass();
  CHECK(rig.app->model().files->state() == app::DirScan::State::Idle);  // not in view: nothing read
  CHECK(rig.card.opendirs() == 0u);
  rig.go(ScreenId::File);
  unsigned last = rig.card.readdirs();
  unsigned passes = 0;
  const std::uint32_t frames = rig.app->frames();
  while (rig.app->model().files->state() == app::DirScan::State::Scanning || rig.app->model().files->state() == app::DirScan::State::Idle) {
    CHECK(rig.card.readdirs() - last <= app::DirScan::kMaxUnitsPerStep);
    last = rig.card.readdirs();
    CHECK(std::string(rig.app->message()).empty());  // the scan is quiet: the screen shows it
    rig.pass();
    if (++passes >= 500u) {
      FAIL_CHECK("the scan did not end");
      break;
    }
  }
  CHECK(passes >= 10u);  // the folder was not read in one gulp
  CHECK(rig.app->frames() >= frames + 10u);  // and the screen was drawn as it came in
  CHECK(rig.app->model().files->state() == app::DirScan::State::Done);
  CHECK(rig.index().count() == 40u);
  CHECK(rig.app->session().jobs.occupied() == 0u);  // the slot was released
  CHECK(rig.card.open_dirs() == 0u);
  const unsigned reads = rig.card.readdirs();
  rig.go(ScreenId::Pattern);
  rig.go(ScreenId::File);
  rig.passes(5);
  CHECK(rig.card.readdirs() == reads);  // a listing that was read is kept
}

TEST_CASE("file screen: the cursor stays on its entry while the listing grows around it") {
  Rig rig;
  for (unsigned i = 0; i < 30; ++i) {
    rig.add_project("song-" + std::to_string(999 - i));  // each arrives before the ones already listed
  }
  rig.go(ScreenId::File);
  rig.pass();
  CHECK(rig.app->state().file.pos() == 0);  // at the top it stays at the top: the first entry is whatever sorts first so far
  for (unsigned i = 0; i < 300 && rig.index().count() < 20u; ++i) {  // more than the window holds: from here a line of the screen is a line
    rig.pass();
    CHECK(rig.app->state().file.pos() == 0);
  }
  NEED(rig.index().count() >= 20u);
  NEED(rig.index().count() < 30u);
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  const std::string chosen = rig.name_at_cursor();
  const int line = rig.app->state().file.pos() - rig.app->state().file.top();
  CHECK(line == 2);
  rig.settle();
  CHECK(rig.index().count() == 30u);
  CHECK(rig.name_at_cursor() == chosen);  // the same project, at whatever place it now sorts
  CHECK(rig.app->state().file.pos() - rig.app->state().file.top() == line);  // on the same line of the screen
  CHECK(rig.app->state().file.pos() > 2);
}

TEST_CASE("file screen: Left and Right jump by first letter, Up and Down by one") {
  Rig rig;
  for (const char* n : {"alpha", "Apple", "beta", "Bongo", "cee"}) {
    rig.add_project(n);
  }
  rig.go(ScreenId::File);
  rig.settle();
  NEED(rig.index().count() == 5u);
  CHECK(rig.name_at_cursor() == "alpha");
  rig.tap(Button::Right);
  CHECK(rig.name_at_cursor() == "beta");
  rig.tap(Button::Right);
  CHECK(rig.name_at_cursor() == "cee");
  rig.tap(Button::Right);
  CHECK(rig.name_at_cursor() == "cee");  // the last letter: stays
  rig.tap(Button::Left);
  CHECK(rig.name_at_cursor() == "beta");
  rig.tap(Button::Down);
  CHECK(rig.name_at_cursor() == "Bongo");
  rig.tap(Button::Left);  // mid-letter: the start of its own
  CHECK(rig.name_at_cursor() == "beta");
  rig.tap(Button::Up);
  CHECK(rig.name_at_cursor() == "Apple");
  (void)rig.app->invoke(core::action_id("cursor.end"));
  CHECK(rig.name_at_cursor() == "cee");
  (void)rig.app->invoke(core::action_id("cursor.home"));
  CHECK(rig.name_at_cursor() == "alpha");
}

TEST_CASE("file screen: F1 opens the project under the cursor through the device's load and the result is told") {
  Rig rig;
  rig.add_project("alpha");
  rig.add_project("Beta Song");
  rig.go(ScreenId::File);
  rig.settle();
  rig.tap(Button::Down);
  rig.tap(Button::F1);
  CHECK(rig.project.opens == 1u);
  CHECK(rig.project.last_folder == "Projects/Beta Song");
  CHECK(std::string(rig.app->message()) == "OPENING Beta Song");
  CHECK(rig.app->state().file.opening);
  rig.passes(3);  // the platform's load is running: nothing to tell yet
  CHECK(rig.app->state().file.opening);
  rig.project.is_busy = false;
  rig.project.ok = true;
  rig.pass();
  CHECK_FALSE(rig.app->state().file.opening);
  CHECK(std::string(rig.app->message()) == "PROJECT Beta Song OPENED");

  rig.go(ScreenId::File);  // (the open showed the Pattern view: OS-OPENTOPAT)
  rig.tap(Button::F1);  // a second open, that fails
  rig.project.is_busy = false;
  rig.project.ok = false;
  rig.project.words = "NO PROJECT FILE";
  rig.pass();
  CHECK(std::string(rig.app->message()) == "PROJECT Beta Song FAILED: NO PROJECT FILE");
  rig.tap(Button::F1);
  rig.project.is_busy = false;
  rig.project.words = "i/o error";  // the loader's own words are not always upper case
  rig.pass();
  CHECK(std::string(rig.app->message()) == "PROJECT Beta Song FAILED: I/O ERROR");
}

// OS-OPENTOPAT (maintainer, 2026-10-17; new Community OS behaviour): a project that opened shows the Pattern view; a failed open stays on File with its error.
TEST_CASE("file screen: a project that opened shows the Pattern view, a failed open stays on File with its error (OS-OPENTOPAT)") {
  Rig rig;
  rig.add_project("alpha");
  rig.go(ScreenId::File);
  rig.settle();
  rig.tap(Button::F1);
  CHECK(rig.app->state().screen == ScreenId::File);  // while the platform loads: still on File, the strip says OPENING
  rig.passes(3);
  CHECK(rig.app->state().screen == ScreenId::File);
  rig.project.is_busy = false;
  rig.project.ok = true;
  rig.pass();
  CHECK(rig.app->state().screen == ScreenId::Pattern);
  CHECK(std::string(rig.app->message()) == "PROJECT alpha OPENED");  // (the strip is the same on every screen)
  // A failed open (no document came of it): File, the error on the strip, the cursor where it was.
  rig.go(ScreenId::File);
  rig.settle();
  rig.expire();
  rig.tap(Button::F1);
  rig.project.is_busy = false;
  rig.project.ok = false;
  rig.project.words = "NO PROJECT FILE";
  rig.pass();
  CHECK(rig.app->state().screen == ScreenId::File);
  CHECK(std::string(rig.app->message()) == "PROJECT alpha FAILED: NO PROJECT FILE");
  // A refused start (busy, no card) never leaves File either.
  rig.expire();
  rig.project.result = app::ProjectSink::Start::NoCard;
  rig.tap(Button::F1);
  CHECK(rig.app->state().screen == ScreenId::File);
  CHECK(rig.project.opens == 3u);  // (a refused start is still a call to the sink)
}

TEST_CASE("file screen: a recovered project shows the Pattern view too, and the screen the user went to meanwhile is not taken back (OS-OPENTOPAT)") {
  Rig rig;
  rig.card.add_dir("Projects/Cut");
  rig.card.add_file("Projects/Cut/project.o3pj.bak", 4000);
  rig.go(ScreenId::File);
  rig.settle();
  rig.tap(Button::F1);
  CHECK(rig.project.last_format == app::ProjectSink::Format::NativeBackup);
  rig.project.is_busy = false;
  rig.pass();
  CHECK(rig.app->state().screen == ScreenId::Pattern);
  CHECK(std::string(rig.app->message()) == "PROJECT Cut RECOVERED");
  // The open was asked on File, but the user is on the Song screen when it ends: it stays there.
  rig.go(ScreenId::File);
  rig.settle();
  rig.expire();
  rig.tap(Button::F1);
  NEED(rig.app->state().file.opening);
  rig.go(ScreenId::Song);
  rig.project.is_busy = false;
  rig.pass();
  CHECK(rig.app->state().screen == ScreenId::Song);
  CHECK(std::string(rig.app->message()) == "PROJECT Cut RECOVERED");  // (still told: only the screen is left alone)
}

TEST_CASE("file screen: native and OG projects are listed and opened as what they are (OS-007o)") {
  Rig rig;
  rig.add_project("Beta");  // OG
  rig.card.add_dir("Projects/Alpha");
  rig.card.add_file("Projects/Alpha/project.o3pj", 4000);
  rig.go(ScreenId::File);
  rig.settle();
  NEED(rig.index().count() == 2u);
  CHECK(std::string(rig.index().name(0)) == "Alpha");
  CHECK(rig.index().tag(0) == app::EntryTag::Native);
  CHECK(rig.index().tag(1) == app::EntryTag::Og);
  rig.tap(Button::F1);  // the cursor is on Alpha
  CHECK(rig.project.last_format == app::ProjectSink::Format::Native);
  CHECK(rig.project.last_folder == "Projects/Alpha");
  rig.project.is_busy = false;
  rig.pass();
  rig.expire();
  rig.go(ScreenId::File);  // (the open showed the Pattern view: OS-OPENTOPAT)
  rig.tap(Button::Down);
  rig.tap(Button::F1);
  CHECK(rig.project.last_format == app::ProjectSink::Format::Og);
  CHECK(rig.project.last_folder == "Projects/Beta");
}

TEST_CASE("file screen: a folder left by an interrupted save is RECOVER, opens as the backup and is told as a recovery (OS-007o)") {
  Rig rig;
  rig.card.add_dir("Projects/Cut");
  rig.card.add_file("Projects/Cut/project.o3pj.bak", 4000);
  rig.go(ScreenId::File);
  rig.settle();
  NEED(rig.index().count() == 1u);
  CHECK(rig.index().tag(0) == app::EntryTag::Recover);
  rig.tap(Button::F1);
  CHECK(rig.project.last_format == app::ProjectSink::Format::NativeBackup);
  CHECK(std::string(rig.app->message()) == "RECOVERING Cut");
  CHECK(rig.app->state().file.pending == app::FileView::Pending::Recover);
  rig.project.is_busy = false;
  rig.pass();
  CHECK(std::string(rig.app->message()) == "PROJECT Cut RECOVERED");
  CHECK(rig.app->state().file.pending == app::FileView::Pending::Open);
  // The next open of an ordinary project is told as one.
  rig.go(ScreenId::File);  // (the recovery showed the Pattern view: OS-OPENTOPAT)
  rig.add_project("Plain");
  rig.tap(Button::F2);
  rig.settle();
  rig.expire();
  rig.tap(Button::F1);  // the cursor is back on the top: Cut
  rig.project.is_busy = false;
  rig.expire();
  rig.go(ScreenId::File);  // (the recovery showed the Pattern view again: OS-OPENTOPAT)
  rig.tap(Button::Down);
  rig.tap(Button::F1);
  CHECK(rig.project.last_format == app::ProjectSink::Format::Og);
  CHECK(rig.app->state().file.pending == app::FileView::Pending::Open);
  rig.project.is_busy = false;
  rig.pass();
  CHECK(std::string(rig.app->message()) == "PROJECT Plain OPENED");
}

TEST_CASE("file screen: opening says why it could not") {
  Rig rig;
  rig.go(ScreenId::File);
  rig.settle();
  rig.tap(Button::F1);  // an empty list
  CHECK(std::string(rig.app->message()) == "NOTHING TO OPEN");
  CHECK(rig.project.opens == 0u);
  rig.add_project("one");
  rig.tap(Button::F2);
  rig.settle();
  rig.expire();
  rig.project.result = app::ProjectSink::Start::Busy;
  rig.tap(Button::F1);
  CHECK(std::string(rig.app->message()) == "A LOAD IS ALREADY RUNNING");
  CHECK_FALSE(rig.app->state().file.opening);
  rig.expire();
  rig.project.result = app::ProjectSink::Start::NoCard;
  rig.tap(Button::F1);
  CHECK(std::string(rig.app->message()) == "THE CARD IS NOT MOUNTED");
  rig.expire();
  rig.project.result = app::ProjectSink::Start::NoDocument;
  rig.tap(Button::F1);
  CHECK(std::string(rig.app->message()) == "NO DOCUMENT TO LOAD INTO");
  CHECK(rig.project.opens == 3u);

  Rig bare;  // no load attached (the simulator): the action is disabled and starts nothing
  bare.app->attach_project_sink(nullptr);
  bare.add_project("one");
  bare.go(ScreenId::File);
  bare.settle();
  CHECK(bare.app->invoke(core::action_id("file.open")) == app::DispatchResult::Disabled);
}

TEST_CASE("file screen: F2 reads the folder again, and the cursor goes back to the top") {
  Rig rig;
  rig.add_project("a");
  rig.add_project("b");
  rig.go(ScreenId::File);
  rig.settle();
  rig.tap(Button::Down);
  CHECK(rig.app->state().file.pos() == 1);
  rig.add_project("c");  // appeared on the card since
  rig.tap(Button::F2);
  CHECK(rig.app->model().files->state() == app::DirScan::State::Scanning);
  CHECK(rig.app->state().file.pos() == 0);
  rig.settle();
  CHECK(rig.index().count() == 3u);
  CHECK(std::string(rig.app->message()) == "READ AGAIN: 3 ITEMS");  // OS-QAFIX-RESCAN
}

TEST_CASE("file screen: RESCAN says the count when the scan has ended, once, and says nothing when the screen was left (OS-QAFIX-RESCAN)") {
  Rig rig;
  rig.add_project("a");
  rig.go(ScreenId::File);
  rig.settle();
  rig.expire();  // (the first read is not a RESCAN: nothing was said, and whatever is on the strip has run out)
  CHECK(std::string(rig.app->message()).empty());
  rig.tap(Button::F2);
  CHECK(std::string(rig.app->message()).empty());  // (not before the scan has ended)
  rig.settle();
  CHECK(std::string(rig.app->message()) == "READ AGAIN: 1 ITEM");
  rig.expire();
  rig.tap(Button::F2);
  rig.go(ScreenId::Pattern);  // left before the scan ended
  rig.passes(60);
  CHECK(std::string(rig.app->message()).empty());
}

TEST_CASE("file screen: the RESCAN count text: NO ITEMS, 1 ITEM, n ITEMS, n+ ITEMS when the list filled up (OS-QAFIX-RESCAN)") {
  core::FixedString<core::kNotifyTextChars> s;
  app::rescan_text(s, 0, false, "ITEM", "ITEMS");
  CHECK(std::string(s.c_str()) == "READ AGAIN: NO ITEMS");
  app::rescan_text(s, 1, false, "ITEM", "ITEMS");
  CHECK(std::string(s.c_str()) == "READ AGAIN: 1 ITEM");
  app::rescan_text(s, 12, false, "ITEM", "ITEMS");
  CHECK(std::string(s.c_str()) == "READ AGAIN: 12 ITEMS");
  app::rescan_text(s, 256, true, "ITEM", "ITEMS");
  CHECK(std::string(s.c_str()) == "READ AGAIN: 256+ ITEMS");
  app::rescan_text(s, 1, true, "ITEM", "ITEMS");
  CHECK(std::string(s.c_str()) == "READ AGAIN: 1+ ITEMS");  // (a cut list is never "1 ITEM")
  app::rescan_text(s, 0, false, "APP", "APPS");
  CHECK(std::string(s.c_str()) == "READ AGAIN: NO APPS");
}

TEST_CASE("file screen: RESCAN of a list that filled up says the count with a plus (OS-QAFIX-RESCAN)") {
  Rig rig;
  for (unsigned i = 0; i < app::DirIndex::kMaxEntries + 10; ++i) {
    char name[8];
    std::snprintf(name, sizeof name, "p%03u", i);
    rig.add_project(name);
  }
  rig.go(ScreenId::File);
  rig.settle();
  NEED(rig.app->model().files->state() == app::DirScan::State::Done);
  NEED(rig.app->model().files->truncated());
  rig.expire();
  rig.tap(Button::F2);
  rig.settle();
  CHECK(std::string(rig.app->message()) == "READ AGAIN: 256+ ITEMS");
}

TEST_CASE("file screen: RESCAN of an empty folder says NO ITEMS (OS-QAFIX-RESCAN)") {
  Rig rig;
  rig.card.add_dir("Projects");  // the folder is there, with nothing in it
  rig.go(ScreenId::File);
  rig.settle();
  NEED(rig.app->model().files->state() == app::DirScan::State::Done);
  rig.expire();
  rig.tap(Button::F2);
  rig.settle();
  NEED(rig.app->model().files->state() == app::DirScan::State::Done);
  CHECK(rig.index().count() == 0u);
  CHECK(std::string(rig.app->message()) == "READ AGAIN: NO ITEMS");
}

TEST_CASE("file screen: a RESCAN that fails (no card, a read error) stays silent: no count (OS-QAFIX-RESCAN)") {
  Rig rig;
  rig.add_project("a");
  rig.add_project("b");
  rig.go(ScreenId::File);
  rig.settle();
  NEED(rig.index().count() == 2u);

  rig.card.set_present(false);  // the card is gone
  rig.expire();
  rig.tap(Button::F2);
  rig.settle();
  CHECK(rig.app->model().files->state() == app::DirScan::State::NoCard);
  CHECK(std::string(rig.app->message()).find("READ AGAIN") == std::string::npos);
  rig.passes(60);
  CHECK(std::string(rig.app->message()).find("READ AGAIN") == std::string::npos);

  rig.card.set_present(true);  // back, but a read that fails half way
  rig.card.fail_readdir_after(1, hal::storage::Status::Io);
  rig.expire();
  rig.tap(Button::F2);
  rig.settle();
  CHECK(rig.app->model().files->state() == app::DirScan::State::Failed);
  CHECK(std::string(rig.app->message()).find("READ AGAIN") == std::string::npos);
}

TEST_CASE("file screen: a scan that found no folder is tried again when the screen is entered again") {
  Rig rig;
  rig.card.add_dir("Workspace");
  rig.go(ScreenId::File);
  rig.settle();
  CHECK(rig.app->model().files->state() == app::DirScan::State::NoFolder);
  rig.passes(5);
  CHECK(rig.card.opendirs() == 1u);  // it does not try again on its own, every pass
  rig.go(ScreenId::Pattern);
  rig.add_project("late");
  rig.go(ScreenId::File);
  rig.settle();
  CHECK(rig.app->model().files->state() == app::DirScan::State::Done);
  CHECK(rig.index().count() == 1u);
}

TEST_CASE("file screen: a failing card and a missing one leave the screen alive") {
  Rig rig;
  for (unsigned i = 0; i < 10; ++i) {
    rig.add_project("p" + std::to_string(i));
  }
  rig.card.fail_readdir_after(4, hal::storage::Status::Io);
  rig.go(ScreenId::File);
  rig.settle();
  CHECK(rig.app->model().files->state() == app::DirScan::State::Failed);
  CHECK(rig.index().count() == 4u);
  CHECK(rig.card.open_dirs() == 0u);
  rig.tap(Button::F2);  // and the next scan, with the card well again, reads it all
  rig.settle();
  CHECK(rig.index().count() == 10u);

  Rig none;
  none.app->attach_storage(nullptr);  // no card attached at all
  none.go(ScreenId::File);
  none.passes(3);
  CHECK(none.app->model().files->state() == app::DirScan::State::Idle);
  CHECK(none.app->frames() >= 1u);
}

// OS-SDSWAP: the platform tells the App when the card was taken out and put back; the strip says so and the one listing the browser screens share is read again.
TEST_CASE("file screen: a card taken out and put back is said on the strip and the listing is read again (OS-SDSWAP)") {
  Rig rig;
  rig.add_project("alpha");
  rig.add_project("beta");
  rig.go(ScreenId::File);
  rig.settle();
  NEED(rig.app->model().files->state() == app::DirScan::State::Done);
  CHECK(rig.index().count() == 2u);

  rig.card.set_present(false);
  rig.app->card_changed(app::CardEvent::Removed);
  rig.pass();
  CHECK(std::string(rig.app->message()) == "CARD REMOVED");
  CHECK(rig.index().count() == 0u);  // the listing was the gone card's
  rig.settle();
  CHECK(rig.app->model().files->state() == app::DirScan::State::NoCard);
  CHECK(rig.card.open_dirs() == 0u);

  rig.card.set_present(true);
  rig.app->card_changed(app::CardEvent::Reading);
  rig.pass();
  CHECK(std::string(rig.app->message()) == "READING CARD");
  CHECK(rig.app->model().files->state() == app::DirScan::State::NoCard);  // the listing waits for the mount
  rig.app->card_changed(app::CardEvent::Ready);
  rig.pass();
  CHECK(std::string(rig.app->message()) == "CARD READY");
  rig.settle();
  CHECK(rig.app->model().files->state() == app::DirScan::State::Done);
  CHECK(rig.index().count() == 2u);

  rig.card.set_present(false);  // a card that goes in and does not mount
  rig.app->card_changed(app::CardEvent::Removed);
  rig.pass();
  rig.card.set_present(true);
  rig.app->card_changed(app::CardEvent::NotReadable);
  rig.pass();
  CHECK(std::string(rig.app->message()) == "CARD NOT READABLE");
  CHECK(rig.app->frames() >= 1u);
}

TEST_CASE("file screen: the Sample load listing is read again after a card swap too (OS-SDSWAP)") {
  Rig rig;
  rig.card.add_file("Kick.wav", 1000);
  rig.card.add_file("Snare.wav", 1000);
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  NEED(rig.app->model().files->state() == app::DirScan::State::Done);
  CHECK(rig.index().count() == 2u);
  rig.card.set_present(false);
  rig.app->card_changed(app::CardEvent::Removed);
  rig.settle();
  CHECK(rig.app->model().files->state() == app::DirScan::State::NoCard);
  rig.card.set_present(true);
  rig.app->card_changed(app::CardEvent::Ready);
  rig.settle();
  CHECK(rig.app->model().files->state() == app::DirScan::State::Done);
  CHECK(rig.index().count() == 2u);
}

TEST_CASE("file screen: the App gives back the directory of a scan it did not finish") {
  Rig rig;
  for (unsigned i = 0; i < 40; ++i) {
    rig.add_project("song-" + std::to_string(100 + i));
  }
  rig.go(ScreenId::File);
  rig.passes(2);
  NEED(rig.app->model().files->state() == app::DirScan::State::Scanning);
  CHECK(rig.card.open_dirs() == 1u);
  rig.app.reset();  // a recovery builds the App again
  CHECK(rig.card.open_dirs() == 0u);
}

TEST_CASE("file screen: what the list shows of a name") {
  const auto show = [](const char* name) {
    core::FixedString<app::kFileNameChars> out;
    app::file_display_name(name, out);
    return std::string(out.c_str());
  };
  CHECK(show("t-v16") == "t-v16");
  CHECK(show("Breaks 2024") == "Breaks 2024");
  CHECK(show(std::string(32, 'a').c_str()) == std::string(32, 'a'));  // 32 characters: whole
  CHECK(show((std::string(31, 'a') + "b").c_str()) == std::string(31, 'a') + "b");
  CHECK(show((std::string(26, 'a') + "123456").c_str()) == std::string(26, 'a') + "123456");  // exactly the width: whole
  // OS-AUDIT-37: one over the width: the head (25), '~', the last six characters; 32 in all
  CHECK(show((std::string(27, 'a') + "123456").c_str()) == std::string(25, 'a') + "~123456");
  CHECK(show((std::string(40, 'a') + "KICK.WAV").c_str()) == std::string(25, 'a') + "~CK.WAV");
  CHECK(show((std::string(40, 'a') + "KICK.WAV").c_str()).size() == 32u);
  CHECK(show(std::string(33, 'a').c_str()) == std::string(25, 'a') + "~" + std::string(6, 'a'));  // no extension: the last six
  CHECK(show(std::string(300, 'a').c_str()).size() == 32u);
  CHECK(show("caf\xC3\xA9") == "caf?");                // a UTF-8 character is one '?'
  CHECK(show("\xE2\x82\xAC" "uro") == "?uro");
  CHECK(show("\x80\x80") == "??");                     // continuation bytes with no lead: each its own
  CHECK(show("\xFF") == "?");
  CHECK(show("a\tb") == "a?b");
  CHECK(show("") == "");
  std::string wide;
  for (int i = 0; i < 20; ++i) {
    wide += "\xC3\xA9";  // twenty accents, forty bytes
  }
  CHECK(show(wide.c_str()) == std::string(20, '?'));
  wide += std::string(20, 'z');
  CHECK(show(wide.c_str()) == std::string(20, '?') + std::string(5, 'z') + "~" + std::string(6, 'z'));  // 40 characters: the cut counts what is drawn
  // A multi-byte character is never split by the cut: the head and the tail are made of drawn characters.
  std::string accents;
  for (int i = 0; i < 40; ++i) {
    accents += "\xC3\xA9";
  }
  CHECK(show(accents.c_str()) == std::string(25, '?') + "~" + std::string(6, '?'));
  CHECK(show((accents + ".wav").c_str()) == std::string(25, '?') + "~" + std::string(2, '?') + ".wav");
  // The width argument (the Sample load list leaves three cells to the "ST" tag): the same cut at 29.
  core::FixedString<app::kFileNameChars> narrow;
  app::file_display_name((std::string(40, 'a') + "KICK.WAV").c_str(), narrow, 29);
  CHECK(std::string(narrow.c_str()) == std::string(22, 'a') + "~CK.WAV");
  app::file_display_name(std::string(29, 'a').c_str(), narrow, 29);
  CHECK(std::string(narrow.c_str()) == std::string(29, 'a'));
  app::file_display_name(std::string(30, 'a').c_str(), narrow, 29);
  CHECK(narrow.size() == 29u);
}

TEST_CASE("file screen: the open project's row is told by its folder (OS-AUDIT-19)") {
  CHECK(app::file_is_open_project("Projects/My Song", "My Song"));
  CHECK_FALSE(app::file_is_open_project("Projects/My Song", "My Song 2"));
  CHECK_FALSE(app::file_is_open_project("Projects/My Song", "My"));
  CHECK_FALSE(app::file_is_open_project("Projects/My Song", "my song"));  // the card's names are compared as they are
  CHECK_FALSE(app::file_is_open_project("", "My Song"));                  // a new or imported project: no row
  CHECK_FALSE(app::file_is_open_project("", ""));
  CHECK_FALSE(app::file_is_open_project("Other/My Song", "My Song"));
  CHECK_FALSE(app::file_is_open_project("ProjectsX/My Song", "My Song"));
  CHECK_FALSE(app::file_is_open_project("Projects", "Projects"));
}
