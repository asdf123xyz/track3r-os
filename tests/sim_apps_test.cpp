// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-APPUX S0: the shipped apps in the simulator's card (sim/include/o3os/sim/sim_apps.h, the scenario command `apps on`). The rig loads the host objects of the eleven apps with a manifest as native modules over
// the card, so MODULES lists them and F1 launches them on their own page; these cases hold down what the QA scenarios (sim/scenarios/qa/qa_modules.txt) rely on: every app is listed, loaded and launches (an app that
// edits the project or writes files asks first, as on the unit), the nine apps of ABI 1.9 declare their key row and Tuner and Scope show the legacy row, F8 and F7 + F8 leave, and the command's refusals.
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3os/app/buttons.h>
#include <o3os/app/module_ui.h>
#include <o3os/app/screens/screen.h>
#include <o3os/sim/scenario.h>
#include <o3os/sim/simulator.h>

#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace o3os;
namespace fs = std::filesystem;

namespace {

bool run_on(sim::Simulator& simulator, const std::string& text, std::string& error) {
  std::vector<sim::ScenarioCommand> cmds;
  if (!sim::parse_scenario(text, cmds, error)) {
    return false;
  }
  return sim::run_scenario(cmds, simulator, [](const std::string&, const sim::Simulator&) { return true; }, error);
}

std::string card_apps_text() { return "document\nwait 20\ncard \"" + std::string(O3_E2E_CARD_DIR) + "\"\napps on\naction nav.screen 12\nwait 50\n"; }

// The nine apps of ABI 1.9 declare their key row; Tuner and Scope are ABI 1.4 and show the legacy row.
bool declares_keys(const std::string& stem) { return stem != "tuner" && stem != "scope"; }

}  // namespace

TEST_CASE("sim apps: apps on lists the eleven shipped apps, all loaded, with a placeholder .tmod for each on the card") {
  sim::Simulator simulator;
  std::string error;
  NEED(run_on(simulator, card_apps_text(), error));
  CHECK_MESSAGE(error.empty(), error);
  NEED(simulator.has_apps());
  const app::ModuleUi* ui = simulator.app().model().modules;
  NEED(ui != nullptr);
  CHECK(ui->app_entry_count() == 11u);
  CHECK(simulator.app().state().screen == app::ScreenId::Modules);
  for (unsigned i = 0; i < ui->app_entry_count(); ++i) {
    const app::CatalogEntry* e = ui->app_entry_at(i);
    NEED(e != nullptr);
    CAPTURE(e->stem);
    CHECK(e->state == app::CatalogEntry::State::Loaded);
    CHECK(simulator.card_storage() != nullptr);
  }
  CHECK(ui->set()->quarantine_count() == 0u);
  std::string script;
  for (unsigned i = 0; i < ui->app_entry_count(); ++i) {
    script += std::string("assert file \"modules/apps/") + ui->app_entry_at(i)->stem + ".tmod\" exists\n";
  }
  CHECK(run_on(simulator, script, error));
  CHECK_MESSAGE(error.empty(), error);
}

TEST_CASE("sim apps: every shipped app launches from MODULES on its own page, the nine of ABI 1.9 with their key row, and F8 leaves") {
  sim::Simulator simulator;
  std::string error;
  NEED(run_on(simulator, card_apps_text(), error));
  app::ModuleUi* ui = simulator.app().model().modules;
  NEED(ui != nullptr);
  const unsigned count = ui->app_entry_count();
  NEED(count == 11u);
  for (unsigned i = 0; i < count; ++i) {
    const std::string stem = ui->app_entry_at(i)->stem;
    CAPTURE(stem);
    simulator.tap(app::Button::F1);
    simulator.wait_ms(100);
    if (simulator.app().state().screen != app::ScreenId::ModuleApp) {  // an app that edits the project or writes files asks first: F1 again
      simulator.tap(app::Button::F1);
      simulator.wait_ms(100);
    }
    CHECK(simulator.app().state().screen == app::ScreenId::ModuleApp);
    CHECK(ui->app_running());
    CHECK(!ui->app_stopped());
    CHECK(stem == ui->app_name());
    CHECK(ui->app_keys_declared() == declares_keys(stem));
    CHECK(ui->app_key_label(7, false)[0] == '\0');  // F8 is the OS's BACK: never a label
    CHECK(simulator.healthy());
    simulator.tap(app::Button::F8);
    simulator.wait_ms(50);
    CHECK(simulator.app().state().screen == app::ScreenId::Modules);
    CHECK(!ui->app_running());
    simulator.tap(app::Button::Down);
  }
}

TEST_CASE("sim apps: MIDI Monitor labels F1 PAUSE and F2 CLEAR (and the Shift row says the same), and F7 + F8 leaves too") {
  sim::Simulator simulator;
  std::string error;
  NEED(run_on(simulator, card_apps_text(), error));
  app::ModuleUi* ui = simulator.app().model().modules;
  NEED(ui != nullptr);
  NEED(run_on(simulator, "tap down 3\ntap f1\nwait 100\n", error));
  NEED(simulator.app().state().screen == app::ScreenId::ModuleApp);
  CHECK(std::strcmp(ui->app_name(), "midimon") == 0);
  CHECK(ui->app_keys_declared());
  CHECK(std::strcmp(ui->app_key_label(0, false), "PAUSE") == 0);
  CHECK(std::strcmp(ui->app_key_label(1, false), "CLEAR") == 0);
  CHECK(std::strcmp(ui->app_key_label(0, true), "PAUSE") == 0);
  CHECK(std::strcmp(ui->app_key_label(1, true), "CLEAR") == 0);
  for (unsigned i = 2; i < 8; ++i) {
    CHECK(std::strcmp(ui->app_key_label(i, false), "") == 0);
  }
  NEED(run_on(simulator, "press f7\nwait 5\npress f8\nwait 5\nrelease f8\nrelease f7\nwait 50\n", error));
  CHECK(simulator.app().state().screen == app::ScreenId::Modules);
  CHECK(!ui->app_running());
}

TEST_CASE("sim apps: the command needs the card, comes once, and excludes the engine") {
  std::string error;
  {
    sim::Simulator simulator;
    CHECK_FALSE(run_on(simulator, "document\napps on\n", error));
    CHECK_MESSAGE(error.find("a card must be in the slot first") != std::string::npos, error);
    CHECK_FALSE(simulator.has_apps());
  }
  {
    sim::Simulator simulator;
    CHECK_FALSE(run_on(simulator, card_apps_text() + "apps on\n", error));
    CHECK_MESSAGE(error.find("the apps are on already") != std::string::npos, error);
  }
  {
    sim::Simulator simulator;
    CHECK_FALSE(run_on(simulator, card_apps_text() + "audio on\n", error));
    CHECK_MESSAGE(error.find("refused after `apps on`") != std::string::npos, error);
  }
  {
    sim::Simulator simulator;
    CHECK_FALSE(run_on(simulator, "document\naudio on\napps on\n", error));  // (the parser or the run says it: either way the pair is refused)
    CHECK_MESSAGE(error.find("apps on") != std::string::npos, error);
  }
  {
    sim::Simulator simulator;
    NEED(run_on(simulator, card_apps_text(), error));
    CHECK_FALSE(run_on(simulator, "document\n", error));  // a document after the card is refused: the apps go with the card
    CHECK(simulator.has_apps());
  }
}
