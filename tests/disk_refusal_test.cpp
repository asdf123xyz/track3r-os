// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-USBDISK 4.3 (U2b-2), the matrix of the refusals the user sees while the computer has the card. The card is the platform's own stack over a counting BlockDevice (the sparse `Medium` of the
// MK66 storage tests): FatStorage, CardSwap and the FatStorageWriter whose gate answers Host, under the real SaveService and App. For each refused action: the medium's block counters do not
// move, and the strip's head is the refusal (not the persistent CARD ON COMPUTER strip, which the platform rewrites every second: the test rewrites it before each action). The texts the platform
// says itself (DiskShare::refusal_text, fail_text) are the Config row's own words, and fit the strip.
#include <doctest/doctest.h>

#include "../platform/mk66/tests/mk66_storage_fakes.h"
#include "support/need.h"
#include "support/project_fixture.h"
#include "support/static_slot.h"

#include <o3os/app/app.h>
#include <o3os/app/export_source.h>
#include <o3os/app/module_ui.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/save_service.h>
#include <o3os/audio/offline.h>
#include <o3os/audio/voices.h>
#include <o3os/core/input_event.h>
#include <o3os/core/notify.h>
#include <o3os/model/editor.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/platform/mk66/disk_share.h>
#include <o3os/platform/mk66/save/fat_storage_writer.h>
#include <o3os/platform/mk66/storage/card_swap.h>
#include <o3os/platform/mk66/storage/fat_storage.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace o3os;
using app::ScreenId;
namespace st = o3os::platform::mk66::storage;
namespace sv = o3os::platform::mk66::save;
namespace mk = o3os::platform::mk66;
using fakes::Fat32Builder;
using fakes::Medium;

namespace {

alignas(8) std::uint8_t g_undo[64 * 1024];

audio::VoicePool& make_pool() {
  static o3os::test::StaticSlot<audio::VoicePool> slot;
  return slot.make();
}

// An export source that is never opened when the refusal does its job.
class NoRenderer final : public audio::OfflineRenderer {
 public:
  unsigned opens = 0;
  bool open(const audio::OfflineRequest&) noexcept override {
    ++opens;
    return false;
  }
  Step next(std::int16_t*) noexcept override { return Step::Failed; }
  bool in_tail() const noexcept override { return false; }
  unsigned progress() const noexcept override { return 0; }
  void close() noexcept override {}
  const char* failure() const noexcept override { return ""; }
};

// A project sink that has a loaded document and no job: file.open then gets as far as the listing (empty while the computer has the card).
class FakeProject final : public app::ProjectSink {
 public:
  Start open(const char*, Format) noexcept override { return Start::NoDocument; }
  Start start_new() noexcept override { return Start::NoDocument; }
  bool busy() const noexcept override { return false; }
  const char* folder() const noexcept override { return ""; }
  bool loaded() const noexcept override { return true; }
  const char* detail() const noexcept override { return ""; }
};

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  Medium medium{200'000};
  std::unique_ptr<st::FatStorage> fat;
  std::unique_ptr<st::CardSwap> swap;
  std::unique_ptr<sv::FatStorageWriter> writer;
  app::SaveService saves;
  NoRenderer renderer;
  alignas(4) std::uint8_t ring[app::ExportSource::kRingBytes];
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  std::unique_ptr<app::SampleBankMem> bank_mem = std::make_unique<app::SampleBankMem>();
  std::vector<std::int16_t> memory = std::vector<std::int16_t>(40000);
  app::SampleBank bank;
  FakeProject project;
  std::unique_ptr<app::ModuleUi> modules = std::make_unique<app::ModuleUi>();

  Rig() {
    Fat32Builder builder(medium);
    builder.add_file(Fat32Builder::kRootCluster, "README.TXT", std::vector<std::uint8_t>(40, 1));
    fat = std::make_unique<st::FatStorage>(medium);
    swap = std::make_unique<st::CardSwap>(*fat, medium);
    writer = std::make_unique<sv::FatStorageWriter>(*fat, medium);
    writer->attach_owner(*swap);
    NEED(swap->reacquire() == hal::storage::Status::Ok);
    swap->begin();
    model::ModelArena& arena = test::arena(1);
    test::fill_small(arena.project);
    journal.begin(g_undo, sizeof g_undo);
    editor = std::make_unique<model::Editor>(arena.project, journal);
    app->attach_document(editor.get());
    bank.init(make_pool(), arena, *bank_mem, memory.data(), static_cast<std::uint32_t>(memory.size()));
    app->attach_samples(&bank);
    app->attach_storage(fat.get());
    saves.attach(*fat, *writer);
    app::ExportSource::Wiring w;
    w.renderer = &renderer;
    w.ring = ring;
    w.ring_bytes = sizeof ring;
    saves.attach_export(w);
    app->attach_saves(&saves);
    app->attach_project_sink(&project);
    app->attach_modules(modules.get());
    pass(16);
  }
  void pass(unsigned ms) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  void go(ScreenId id) {
    (void)app->invoke(core::action_id("nav.screen"), static_cast<core::ActionArg>(id));
    pass(16);
  }
  // The computer has the card: the hand-over, then what the platform tells the app.
  void card_out() {
    NEED(swap->hand_to_host() == st::CardSwap::HandOver::Ok);
    app->set_card_on_computer(true);
  }
  // The flag alone, the hand-over not done: the card still reads, so the listing the screen shows is Done, not NoCard (the state that decides what an unguarded action would say).
  void flag_only() {
    app->set_card_on_computer(true);
    for (unsigned i = 0; i < 20 && app->model().files->state() != app::DirScan::State::Done; ++i) {
      pass(16);
    }
    NEED(app->model().files->state() == app::DirScan::State::Done);
  }
  // The platform's second-by-second rewrite of the persistent strip (source "disk", Info): it is the head unless something asked goes ahead of it.
  void strip() {
    app->disk_notice(app::App::kCardOnComputerStrip, 60000);
    pass(16);
  }
  void tap(app::Button b) {
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true));
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false));
    pass(16);
  }
  void expire() {
    for (unsigned i = 0; i < 10 && !std::string(app->message()).empty(); ++i) {
      clock.advance(5000000);
      pass(16);
    }
  }
  // One refused action: the strip is up, `act` runs it, and the head is the refusal with no block written.
  template <class Act>
  void refused_by(const char* action, Act&& act, const char* text) {
    INFO(action);
    strip();
    CHECK(std::string(app->message()) == app::App::kCardOnComputerStrip);
    const std::uint64_t written = medium.blocks_written;
    const unsigned write_calls = medium.write_calls;
    act();
    pass(16);
    CHECK(std::string(app->message()) == text);
    for (unsigned i = 0; i < 5; ++i) {
      pass(16);  // (a job that was started would write on the next passes)
    }
    CHECK(medium.blocks_written == written);
    CHECK(medium.write_calls == write_calls);
    CHECK(app->session().jobs.occupied() == 0u);
    CHECK_FALSE(saves.busy());
    CHECK(renderer.opens == 0u);
    expire();
  }
  void refused(const char* action, core::ActionArg arg, const char* text) {
    refused_by(action, [&] { (void)app->invoke(core::action_id(action), arg); }, text);
  }
  // The same through the key itself (the binding of the screen's context, as the unit's press goes).
  void refused_key(const char* action, app::Button b, const char* text) {
    refused_by(action, [&] { tap(b); }, text);
  }
};

constexpr const char* kSaveRefusal = "CARD ON COMPUTER - EJECT FIRST";

}  // namespace

TEST_CASE("disk refusals: every card action of 4.3 is refused while the card is the computer's, shows ahead of the persistent strip, and writes no block") {
  Rig rig;
  rig.card_out();
  CHECK(rig.writer->gate() == hal::storage::WriteStatus::Host);
  CHECK(std::string(app::SaveService::gate_text(rig.writer->gate())) == kSaveRefusal);  // the platform writer's gate says the same words as the app's refusal
  rig.go(ScreenId::File);
  rig.refused("file.save", 0, kSaveRefusal);
  rig.refused("file.save_as", 0, kSaveRefusal);  // (before the keyboard opens: nobody types a name for a save that cannot go)
  CHECK(rig.app->state().overlay == app::OverlayId::None);
  rig.refused("sample.load", 0, "CARD ON COMPUTER");
  rig.refused("sample.load", 1, "CARD ON COMPUTER");  // F4 ADD
  rig.go(ScreenId::Pattern);
  rig.refused("export.pattern", 0, kSaveRefusal);
  rig.refused("export.pattern", 0, kSaveRefusal);  // (the strip comes back behind it and the next refusal is the head again)
  CHECK(rig.medium.blocks_written == 0u);  // not a block since the volume was built (the builder writes the medium directly)
}

TEST_CASE("disk refusals: the File screen's open, the project operations and the module picker ask their refusals ahead of the strip") {
  Rig rig;
  rig.card_out();
  rig.go(ScreenId::File);
  rig.refused("file.open", 0, kSaveRefusal);  // (file_actions: the listing is dropped while the computer has the card, which is the news, not "nothing to open")
  rig.refused("file.delete", 0, kSaveRefusal);  // (file_ops_actions: the list is empty because the card is away, which is the news, not "no project under the cursor")
  rig.go(ScreenId::Instrument);
  (void)rig.app->invoke(core::action_id("module.engine"), 0);
  rig.pass(16);
  NEED(rig.app->state().overlay == app::OverlayId::ModulePicker);
  rig.strip();  // (the strip is not shown under the picker: the news waits behind it; the pick closes the picker, and its refusal is asked ahead of the strip)
  const std::uint64_t written = rig.medium.blocks_written;
  (void)rig.app->invoke(core::action_id("module.pick"), 0);  // module_actions: ModuleUi::pick says OnComputer
  rig.pass(16);
  CHECK(rig.app->state().overlay == app::OverlayId::None);
  CHECK(std::string(rig.app->message()) == "CARD ON COMPUTER");
  CHECK(rig.medium.blocks_written == written);
  rig.strip();  // the platform's rewrite, a second later: the refusal is still the head
  CHECK(std::string(rig.app->message()) == "CARD ON COMPUTER");
}

TEST_CASE("disk refusals: the strip comes back behind a refusal, and a refusal is shown for its normal time") {
  Rig rig;
  rig.card_out();
  rig.go(ScreenId::File);
  rig.strip();
  (void)rig.app->invoke(core::action_id("file.save"), 0);
  rig.pass(16);
  CHECK(std::string(rig.app->message()) == kSaveRefusal);
  rig.strip();  // the platform's rewrite, a second later: the refusal is still the head
  CHECK(std::string(rig.app->message()) == kSaveRefusal);
  rig.clock.advance(core::kTicksPerMs * (app::ViewState::kMessageMs + 100u));
  rig.pass(16);
  CHECK(std::string(rig.app->message()) == app::App::kCardOnComputerStrip);  // and then the strip
}

TEST_CASE("disk refusals: after the eject the same actions are not refused for the card") {
  Rig rig;
  rig.card_out();
  rig.go(ScreenId::File);
  NEED(rig.swap->take_back() == hal::storage::Status::Ok);
  rig.app->set_card_on_computer(false);
  rig.app->card_changed(app::CardEvent::Ready);
  CHECK(rig.writer->gate() == hal::storage::WriteStatus::Ok);
  (void)rig.app->invoke(core::action_id("file.save"), 0);
  rig.pass(16);
  CHECK(std::string(rig.app->message()).find("CARD ON COMPUTER") == std::string::npos);
  CHECK(rig.app->session().jobs.occupied() == 1u);  // the save started
  rig.go(ScreenId::File);  // and the keys that were refused open their pages again
  rig.tap(app::Button::F7);
  CHECK(rig.app->state().xport.page);
  rig.tap(app::Button::F7);
  CHECK_FALSE(rig.app->state().xport.page);
}

TEST_CASE("disk refusals: the platform's own words are the Config row's, and every one fits the strip") {
  using Refusal = mk::DiskShare::Refusal;
  using Fail = mk::DiskShare::Fail;
  CHECK(std::string(mk::DiskShare::refusal_text(Refusal::None)).empty());
  CHECK(std::string(mk::DiskShare::refusal_text(Refusal::NoCard)) == "NO CARD");
  CHECK(std::string(mk::DiskShare::refusal_text(Refusal::Busy)) == "STOP THE EXPORT FIRST");
  CHECK(std::string(mk::DiskShare::refusal_text(Refusal::SaveBusy)) == "CARD BUSY: WAIT FOR THE SAVE");
  CHECK(std::string(mk::DiskShare::refusal_text(Refusal::NotIdle)) == "THE CARD IS CHANGING HANDS: WAIT");
  for (const Refusal r : {Refusal::NoCard, Refusal::Busy, Refusal::SaveBusy, Refusal::NotIdle}) {
    const std::string text = mk::DiskShare::refusal_text(r);
    CHECK_FALSE(text.empty());
    CHECK(text.size() <= core::kNotifyTextChars);
  }
  for (const Fail f : {Fail::NoCard, Fail::Busy, Fail::SaveBusy, Fail::Layout, Fail::NoHost, Fail::Owner, Fail::Open}) {
    const std::string text = mk::DiskShare::fail_text(f);
    CHECK_FALSE(text.empty());
    CHECK(text.size() <= core::kNotifyTextChars);
  }
  CHECK(std::string(app::SaveService::gate_text(hal::storage::WriteStatus::Host)).size() <= core::kNotifyTextChars);
  CHECK(std::string(app::App::kCardOnComputerStrip).size() <= core::kNotifyTextChars);
}

// The four that said nothing on the unit (int.16): the key itself, the refusal is the head, no block and no page.
TEST_CASE("disk refusals: File EXPORT and Delete, Sample load ADD and the chooser's instrument LOAD say the card is on the computer") {
  Rig rig;
  rig.card_out();
  rig.go(ScreenId::File);
  rig.refused_key("file.engine (F7 EXPORT)", app::Button::F7, kSaveRefusal);
  CHECK_FALSE(rig.app->state().xport.page);  // the EXPORT page does not open for a card that is away
  rig.refused_key("file.delete (Delete)", app::Button::Delete, kSaveRefusal);
  rig.go(ScreenId::SampleLoad);
  rig.refused_key("sample.load (F4 ADD)", app::Button::F4, "CARD ON COMPUTER");
  rig.refused_key("sample.load (F1 LOAD)", app::Button::F1, "CARD ON COMPUTER");
  rig.go(ScreenId::Instrument);
  (void)rig.app->invoke(core::action_id("instrument.select"), 40);
  rig.pass(16);
  NEED(rig.editor->project().instrument(static_cast<std::uint32_t>(rig.app->state().instrument - 1)) == nullptr);  // an empty slot: the type chooser, whose F5 is LOAD
  rig.refused_key("instrument.load (F5 LOAD)", app::Button::F5, "CARD ON COMPUTER");
  CHECK(rig.app->state().screen == ScreenId::Instrument);  // the listing does not open
  CHECK_FALSE(rig.app->state().wav.pti);
}

// The refusals that sat behind other answers (rev-u2-silent). Each case fails without its guard: the guard is the first check, and the answer it shadows is the one the rig reaches.
TEST_CASE("disk refusals: OPEN and the IMPORT page's F1 say the card is on the computer, not that there is nothing to open or import") {
  Rig rig;
  rig.go(ScreenId::File);
  (void)rig.app->invoke(core::action_id("file.page"), 0);  // F6: the IMPORT page, open when the card goes out
  rig.pass(16);
  NEED(rig.app->state().import.page);
  rig.card_out();
  rig.refused_key("file.open (F1 IMPORT)", app::Button::F1, kSaveRefusal);  // was "NOTHING TO IMPORT"
  (void)rig.app->invoke(core::action_id("file.page"), 0);  // and back on the PROJECTS page
  rig.pass(16);
  NEED(!rig.app->state().import.page);
  rig.refused_key("file.open (F1 OPEN)", app::Button::F1, kSaveRefusal);  // was "NOTHING TO OPEN"
}

TEST_CASE("disk refusals: Sample load ADD, LOAD and PLAY say the card is on the computer while the listing still reads") {
  Rig rig;
  rig.go(ScreenId::SampleLoad);
  rig.flag_only();  // the scan is Done, not NoCard: without the guards ADD and LOAD say NO FILE TO LOAD, and PLAY says NO PREVIEW MEMORY
  rig.refused_key("sample.load (F4 ADD)", app::Button::F4, "CARD ON COMPUTER");
  rig.refused_key("sample.load (F1 LOAD)", app::Button::F1, "CARD ON COMPUTER");
  rig.refused_key("sample.play (F5 PLAY)", app::Button::F5, "CARD ON COMPUTER");
}

TEST_CASE("disk refusals: the EXPORT page closes when the card goes out, and F7 then refuses instead of closing it") {
  Rig rig;
  rig.go(ScreenId::File);
  rig.tap(app::Button::F7);
  NEED(rig.app->state().xport.page);
  rig.card_out();
  CHECK_FALSE(rig.app->state().xport.page);  // no stale export mode survives the share
  rig.refused_key("file.engine (F7 EXPORT)", app::Button::F7, kSaveRefusal);
  CHECK_FALSE(rig.app->state().xport.page);
}
