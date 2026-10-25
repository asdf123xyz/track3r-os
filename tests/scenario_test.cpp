// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <o3os/app/actions.h>
#include <o3os/app/buttons.h>
#include <o3os/app/firmware_view.h>
#include <o3os/core/action.h>
#include <o3os/platform/host/fake_storage.h>
#include <o3os/platform/host/sha256.h>
#include <o3os/sim/scenario.h>
#include <o3os/sim/scenario_assert.h>
#include <o3os/sim/simulator.h>
#include <o3os/ui/style.h>
#include <o3os/ui/theme.h>

#include "support/need.h"

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace o3os;
using sim::ScenarioCommand;
using Kind = ScenarioCommand::Kind;

namespace {
bool parse(const std::string& text, std::vector<ScenarioCommand>& out, std::string& error) {
  return sim::parse_scenario(text, out, error);
}
std::string parse_error(const std::string& text) {
  std::vector<ScenarioCommand> out;
  std::string error;
  CHECK_FALSE(parse(text, out, error));
  return error;
}
}  // namespace

TEST_CASE("scenario: the whole grammar parses, comments and blank lines are skipped") {
  const std::string text =
      "# a comment\n"
      "\n"
      "wait 250        # trailing comment\n"
      "press F1\n"
      "release f1\n"
      "TAP Left\n"
      "tap right 3\n"
      "pad 47\n"
      "encoder +8\n"
      "encoder -2\n"
      "snap first_shot\n";
  std::vector<ScenarioCommand> cmds;
  std::string error;
  CHECK(parse(text, cmds, error));
  CHECK(error.empty());
  CHECK(cmds.size() == 9u);
  if (cmds.size() != 9u) {
    return;
  }
  CHECK((cmds[0].kind == Kind::Wait && cmds[0].value == 250u && cmds[0].line == 3));
  CHECK((cmds[1].kind == Kind::Press && cmds[1].button == app::Button::F1));
  CHECK((cmds[2].kind == Kind::Release && cmds[2].button == app::Button::F1));
  CHECK((cmds[3].kind == Kind::Tap && cmds[3].button == app::Button::Left && cmds[3].value == 1u));
  CHECK((cmds[4].kind == Kind::Tap && cmds[4].button == app::Button::Right && cmds[4].value == 3u));
  CHECK((cmds[5].kind == Kind::Pad && cmds[5].value == 47u));
  CHECK((cmds[6].kind == Kind::Encoder && cmds[6].delta == 8));
  CHECK((cmds[7].kind == Kind::Encoder && cmds[7].delta == -2));
  CHECK((cmds[8].kind == Kind::Snap && cmds[8].name == "first_shot" && cmds[8].line == 11));
  CHECK(parse("", cmds, error));
  CHECK(cmds.empty());
}

TEST_CASE("scenario: every button name of the app is accepted") {
  for (const app::ButtonName& b : app::kButtonNames) {
    std::vector<ScenarioCommand> cmds;
    std::string error;
    CHECK(parse(std::string("tap ") + b.name, cmds, error));
    CHECK((cmds.size() == 1u && cmds[0].button == b.button));
  }
}

TEST_CASE("scenario: errors name the line and the problem") {
  CHECK(parse_error("wait 1\nfrobnicate 3\n").find("line 2: unknown command 'frobnicate'") == 0);
  CHECK(parse_error("tap sideways\n").find("line 1: unknown button 'sideways'") == 0);
  CHECK(parse_error("tap left 0\n").find("tap count") != std::string::npos);
  CHECK(parse_error("tap left 1001\n").find("tap count") != std::string::npos);
  CHECK(parse_error("tap left 2 3\n").find("line 1") == 0);
  CHECK(parse_error("press left 2\n").find("line 1") == 0);
  CHECK(parse_error("wait\n").find("wait needs") != std::string::npos);
  CHECK(parse_error("wait -5\n").find("wait needs") != std::string::npos);
  CHECK(parse_error("wait 3600001\n").find("wait needs") != std::string::npos);
  CHECK(parse_error("wait 10x\n").find("wait needs") != std::string::npos);
  CHECK(parse_error("pad 48\n").find("pad needs") != std::string::npos);
  CHECK(parse_error("pad -1\n").find("pad needs") != std::string::npos);
  CHECK(parse_error("encoder 0\n").find("encoder needs") != std::string::npos);
  CHECK(parse_error("encoder 1001\n").find("encoder needs") != std::string::npos);
  CHECK(parse_error("snap\n").find("snap needs") != std::string::npos);
  CHECK(parse_error("snap a/b\n").find("snap needs") != std::string::npos);
  CHECK(parse_error("snap ../x\n").find("snap needs") != std::string::npos);
  CHECK(parse_error(std::string("snap ") + std::string(65, 'a') + "\n").find("snap needs") != std::string::npos);
  CHECK(parse_error("snap a\nsnap a\n").find("line 2: snap name 'a' is used twice") == 0);
}

TEST_CASE("scenario: running is deterministic and snaps are reported in order") {
  std::vector<ScenarioCommand> cmds;
  std::string error;
  CHECK(parse("snap a\ntap right 3\ntap down 5\nsnap b\nencoder 8\ntap f2\nsnap c\nwait 2500\nsnap d\n", cmds, error));

  const auto run_once = [&](std::vector<std::string>& names, std::vector<std::string>& hashes) {
    auto simulator = std::make_unique<sim::Simulator>();
    std::string err;
    const bool ok = sim::run_scenario(
        cmds, *simulator,
        [&](const std::string& name, const sim::Simulator& s) {
          names.push_back(name);
          hashes.push_back(sim::framebuffer_sha256(s.raster()));
          return true;
        },
        err);
    CHECK(ok);
    CHECK(err.empty());
    CHECK(simulator->healthy());
    CHECK(simulator->display().peak_frame_words() <= 2048u);
    CHECK(simulator->app().view().row() == 13);
    CHECK(std::string(simulator->app().message()) == "");  // the F2 label expired during `wait 2500`
  };
  std::vector<std::string> names1, hashes1, names2, hashes2;
  run_once(names1, hashes1);
  run_once(names2, hashes2);
  CHECK(names1 == std::vector<std::string>{"a", "b", "c", "d"});
  CHECK(names1 == names2);
  CHECK(hashes1 == hashes2);
  CHECK(hashes1[0] != hashes1[1]);  // moving the cursor changes the picture
  CHECK(hashes1[1] != hashes1[2]);  // the F2 label changes it again
  CHECK(hashes1[2] != hashes1[3]);
  CHECK(hashes1[1] != hashes1[3]);  // d is c without the label: the cursor is on row 13, not row 5
}

TEST_CASE("scenario: a refused snapshot stops the run and says where") {
  std::vector<ScenarioCommand> cmds;
  std::string error;
  CHECK(parse("snap a\nsnap b\n", cmds, error));
  sim::Simulator simulator;
  int calls = 0;
  CHECK_FALSE(sim::run_scenario(
      cmds, simulator,
      [&](const std::string&, const sim::Simulator&) {
        ++calls;
        return false;
      },
      error));
  CHECK(calls == 1);
  CHECK(error.find("line 1") == 0);
  CHECK(error.find("'a'") != std::string::npos);
}

TEST_CASE("simulator: time only moves when told, one tick per pass") {
  sim::Simulator simulator;
  simulator.step();
  CHECK(simulator.app().frames() == 1u);
  simulator.tap(app::Button::F5);  // PREVIEW (OS-CFGWIRE): the simulator has no engine, and says so
  CHECK(std::string(simulator.app().message()) == "PREVIEW: NO AUDIO OUTPUT");
  simulator.wait_ms(1998);  // the press was handled on a tick of its own; the release took the next one
  CHECK(std::string(simulator.app().message()) == "PREVIEW: NO AUDIO OUTPUT");
  simulator.wait_ms(1);  // 2000 ticks after the press was handled
  CHECK(std::string(simulator.app().message()) == "");
  CHECK(simulator.healthy());
  CHECK(simulator.failure().empty());
}

TEST_CASE("simulator: every pixel of every frame is one of the active theme's colours, in every theme (ADR 0001)") {
  for (unsigned t = 0; t < ui::kThemeCount; ++t) {
    const ui::Theme& theme = ui::theme(ui::theme_id_at(t));
    std::set<std::uint32_t> palette;
    for (const ui::Rgb& c : theme.colour) {
      palette.insert((std::uint32_t{c.r} << 16) | (std::uint32_t{c.g} << 8) | c.b);
    }
    CHECK(palette.size() <= ui::kColorCount);  // the 16 of ADR 0001 and Rec

    auto simulator = std::make_unique<sim::Simulator>();
    simulator->app().set_theme(ui::theme_id_at(t));
    std::set<std::uint32_t> seen;
    bool opaque = true;
    const auto collect = [&] {
      const std::uint8_t* p = simulator->raster().rgba();
      std::uint32_t last = 0xFFFFFFFFu;  // runs of one colour are the rule: insert on change only
      for (std::size_t i = 0; i < sim::EveRaster::kFramebufferBytes; i += 4) {
        const std::uint32_t c = (std::uint32_t{p[i]} << 16) | (std::uint32_t{p[i + 1]} << 8) | p[i + 2];
        if (c != last) {
          seen.insert(c);
          last = c;
        }
        opaque = opaque && p[i + 3] == 255;
      }
    };
    simulator->step();
    collect();
    for (int i = 0; i < 12; ++i) {  // columns, tracks and rows of the grid, with and without a message
      simulator->tap(app::Button::Right);
      simulator->encoder(i % 2 == 0 ? 3 : 1);
      if (i % 12 == 0) {
        simulator->tap(app::Button::F1);
      }
      collect();
    }
    CHECK(opaque);
    for (std::uint32_t c : seen) {
      CHECK_MESSAGE(palette.count(c) == 1u, theme.name << ": a pixel is not one of its colours");  // no antialiasing, no blends
    }
    CHECK_MESSAGE(seen.size() >= 12u, theme.name);  // and the screen really does use most of them
    CHECK(simulator->healthy());
  }
}

TEST_CASE("scenario: the theme command takes a built-in theme's name, any case") {
  std::vector<ScenarioCommand> cmds;
  std::string error;
  CHECK(parse("theme black\nTHEME White\ntheme amiga   # back\n", cmds, error));
  CHECK(cmds.size() == 3u);
  if (cmds.size() != 3u) {
    return;
  }
  CHECK((cmds[0].kind == Kind::Theme && cmds[0].theme == ui::ThemeId::Black && cmds[0].name == "black" && cmds[0].line == 1));
  CHECK((cmds[1].kind == Kind::Theme && cmds[1].theme == ui::ThemeId::White && cmds[1].name == "white"));
  CHECK((cmds[2].kind == Kind::Theme && cmds[2].theme == ui::ThemeId::Amiga));
  CHECK(parse_error("theme\n").find("line 1: theme needs one name") == 0);
  CHECK(parse_error("tap f1\ntheme red\n").find("line 2: unknown theme 'red'") == 0);
  CHECK(parse_error("theme black white\n").find("theme needs one name") != std::string::npos);
  CHECK(parse_error("theme blac\n").find("unknown theme 'blac'") != std::string::npos);  // whole names only
}

TEST_CASE("scenario: the theme command and Shift + Master change the picture, and only the picture") {
  std::vector<ScenarioCommand> cmds;
  std::string error;
  CHECK(parse(
      "tap right\ntap down 3\n"
      "snap amiga_a\n"
      "theme black\nsnap black\n"
      "theme white\nsnap white\n"
      "theme amiga\nsnap amiga_b\n"
      "press shift\ntap master\nrelease shift\nsnap by_key_black\n"
      "press shift\ntap master\nrelease shift\nsnap by_key_white\n"
      "press shift\ntap master\nrelease shift\nsnap by_key_amiga\n"
      "tap master\nsnap plain_master\n",
      cmds, error));
  auto simulator = std::make_unique<sim::Simulator>();
  std::vector<std::string> names, hashes;
  CHECK(sim::run_scenario(
      cmds, *simulator,
      [&](const std::string& name, const sim::Simulator& s) {
        names.push_back(name);
        hashes.push_back(sim::framebuffer_sha256(s.raster()));
        return true;
      },
      error));
  CHECK(error.empty());
  CHECK(names.size() == 8u);
  if (names.size() != 8u) {
    return;
  }
  CHECK(hashes[0] == hashes[3]);   // amiga, then black, white and amiga again: the same picture as the first
  CHECK(hashes[0] != hashes[1]);
  CHECK(hashes[1] != hashes[2]);
  CHECK(hashes[0] != hashes[2]);
  CHECK(hashes[4] == hashes[1]);   // Shift + Master is the next theme: black, white, amiga
  CHECK(hashes[5] == hashes[2]);
  CHECK(hashes[6] == hashes[0]);
  CHECK(hashes[7] != hashes[0]);   // Master alone is the Mixer's button: that screen is not there yet, and the strip says so
  // The cursor did not move through any of it: the model is the same in every theme.
  CHECK((simulator->app().view().row() == 3 && simulator->app().view().column() == app::Column::Instr));
  CHECK(simulator->app().theme() == ui::ThemeId::Amiga);
  CHECK(simulator->healthy());
}

TEST_CASE("scenario: the action command takes a registered name and an optional integer") {
  std::vector<ScenarioCommand> cmds;
  std::string error;
  CHECK(parse("action nav.down 3\nACTION Learn.Toggle\naction nav.scroll -5   # back up\naction instrument.select +7\n", cmds,
              error));
  CHECK(error.empty());
  CHECK(cmds.size() == 4u);
  if (cmds.size() != 4u) {
    return;
  }
  CHECK((cmds[0].kind == Kind::Action && cmds[0].action == core::action_id("nav.down") && cmds[0].arg == 3));
  CHECK((cmds[0].name == "nav.down" && cmds[0].line == 1));
  CHECK((cmds[1].kind == Kind::Action && cmds[1].action == core::action_id("learn.toggle") && cmds[1].arg == 0));
  CHECK(cmds[1].name == "learn.toggle");  // lower-cased
  CHECK((cmds[2].action == core::action_id("nav.scroll") && cmds[2].arg == -5));
  CHECK(cmds[3].arg == 7);

  CHECK(parse("action nav.down 2147483647\naction nav.down -2147483647\n", cmds, error));
  CHECK((cmds[0].arg == 2147483647 && cmds[1].arg == -2147483647));
}

TEST_CASE("scenario: action errors name the line and the problem") {
  CHECK(parse_error("wait 1\naction no.such\n").find("line 2: unknown action 'no.such'") == 0);
  CHECK(parse_error("action\n").find("action needs a name") != std::string::npos);
  CHECK(parse_error("action nav.down 1 2\n").find("action needs a name") != std::string::npos);
  CHECK(parse_error("action nav.down x\n").find("action argument must be an integer") != std::string::npos);
  CHECK(parse_error("action nav.down 1.5\n").find("action argument must be an integer") != std::string::npos);
  CHECK(parse_error("action nav.down 2147483648\n").find("action argument must be an integer") != std::string::npos);
  CHECK(parse_error("action nav.lef\n").find("unknown action 'nav.lef'") != std::string::npos);  // whole names only
  CHECK(parse_error("action none\n").find("unknown action") != std::string::npos);
}

TEST_CASE("scenario: actions run by name give the same state as the keys, and the Learn chord is a scenario too") {
  std::vector<ScenarioCommand> cmds;
  std::string error;
  CHECK(parse(
      "action nav.scroll 5\n"
      "action nav.right\n"
      "action nav.right\n"
      "action pattern.column 3\n"
      "action nav.scroll -2\n"
      "action instrument.select 12\n"
      "snap by_action\n"
      "press vol\n"
      "press fx\n"
      "snap learn_on\n"
      "release fx\n"
      "release vol\n"
      "press fx\n"
      "press vol\n"
      "release vol\n"
      "release fx\n"
      "snap learn_off\n",
      cmds, error));
  std::vector<std::string> names, hashes;
  sim::Simulator simulator;
  bool learn_on_seen = false;
  CHECK(sim::run_scenario(
      cmds, simulator,
      [&](const std::string& name, const sim::Simulator& s) {
        names.push_back(name);
        hashes.push_back(sim::framebuffer_sha256(s.raster()));
        if (name == "by_action") {
          CHECK(s.app().view().row() == 3);                 // 5 steps down, 2 back up
          CHECK(s.app().view().column() == app::Column::Fx1);
          CHECK(s.app().instrument() == 12);
          CHECK_FALSE(s.app().learn());
        } else if (name == "learn_on") {
          learn_on_seen = true;
          CHECK(s.app().learn());
          CHECK(s.app().overlay() == app::OverlayId::Learn);  // an overlay, not a status text
          CHECK(std::string(s.app().message()) == "");
        } else {
          CHECK_FALSE(s.app().learn());
        }
        return true;
      },
      error));
  CHECK(error.empty());
  CHECK(learn_on_seen);
  CHECK(simulator.healthy());
  CHECK(names == std::vector<std::string>{"by_action", "learn_on", "learn_off"});
  CHECK(hashes[0] != hashes[1]);  // the Learn overlay changes the picture
  CHECK(hashes[0] == hashes[2]);  // and leaving it brings the picture back exactly

  CHECK(simulator.action(core::action_id("no.such"), 0) == app::DispatchResult::Unknown);
  CHECK(simulator.healthy());
}

TEST_CASE("scenario: holding a button runs on the simulated clock") {
  // press ... wait ... release: the gesture layer sees timestamps from the fake clock, never a real one.
  sim::Simulator simulator;
  simulator.step();
  const std::string keymap = "o3keymap 1\nglobal hold f4 ui.softkey 7\n";
  CHECK(simulator.app().load_user_keymap(keymap.data(), keymap.size()).ok());
  simulator.press(app::Button::F4);
  simulator.wait_ms(390);
  CHECK(std::string(simulator.app().message()) == "");
  simulator.wait_ms(20);
  CHECK(std::string(simulator.app().message()) == "F8 UNDO");
  simulator.release(app::Button::F4);
  CHECK(std::string(simulator.app().message()) == "F8 UNDO");
  simulator.wait_ms(2000);  // it runs out; a second notification would queue behind it
  CHECK(std::string(simulator.app().message()) == "");

  simulator.tap(app::Button::F4);  // a short press is the tap, delivered at the release (F4 on the Pattern is FILL since OS-PTOOLS: in Browse it says what it needs)
  CHECK(std::string(simulator.app().message()) == "FILL: TURN EDIT ON");
}

// OS-METRO: `playhead <step> [<ustep>] countin <steps>` says the transport is counting in, and the simulator publishes it as the engine does.
TEST_CASE("scenario: playhead takes an optional count-in, and the app reads it as the count-in of the sequencer") {
  std::vector<ScenarioCommand> cmds;
  std::string error;
  NEED(parse("playhead 48 countin 16\nplayhead 5 7 countin 1\nplayhead 0\nplayhead stop\n", cmds, error));
  NEED(cmds.size() == 4u);
  CHECK(cmds[0].kind == Kind::Playhead);
  CHECK(cmds[0].value == 48u);
  CHECK(cmds[0].delta == 0);
  CHECK(cmds[0].arg == 16);
  CHECK(cmds[1].value == 5u);
  CHECK(cmds[1].delta == 7);
  CHECK(cmds[1].arg == 1);
  CHECK(cmds[2].arg == 0);  // no count-in
  CHECK(cmds[3].name == "stop");
  CHECK(cmds[3].arg == 0);
  CHECK(parse_error("playhead 3 countin\n").find("line 1") == 0u);
  CHECK(parse_error("playhead 3 countin 0\n").find("count-in") != std::string::npos);
  CHECK(parse_error("playhead 3 countin 33\n").find("count-in") != std::string::npos);
  CHECK(parse_error("playhead 3 countin -2\n").find("count-in") != std::string::npos);
  CHECK(parse_error("playhead stop countin 4\n").find("line 1") == 0u);
  CHECK(parse_error("playhead countin 4\n").find("line 1") == 0u);
  // The app sees it: the strip counts, and the position carries the steps left.
  sim::Simulator simulator;
  simulator.open_document(false);
  simulator.playhead(true, 48, 0, 16);
  CHECK(simulator.app().state().play.count_in == 16u);
  CHECK(simulator.app().state().count_in.beat == 4u);
  simulator.playhead(true, 0, 0, 0);
  CHECK(simulator.app().state().play.count_in == 0u);
  CHECK(simulator.app().state().count_in.beat == 0u);
  simulator.playhead(true, 52, 0, 12);
  simulator.playhead(false, 0, 0, 12);  // a stopped transport counts nothing in, whatever the word says
  CHECK(simulator.app().state().play.count_in == 0u);
}

// ---- OS-E2E-1a: assert state, assert model, mark model, assert file -----------------------------------------------------------------------------------------

namespace {
// Parses and runs `text` on a fresh simulator; `framebuffer` gets the hash of the last picture the run left. true: the whole script held.
bool run_text(const std::string& text, std::string& error, std::string* framebuffer = nullptr) {
  std::vector<ScenarioCommand> cmds;
  if (!parse(text, cmds, error)) {
    return false;
  }
  auto simulator = std::make_unique<sim::Simulator>();
  const bool ok = sim::run_scenario(cmds, *simulator, [](const std::string&, const sim::Simulator&) { return true; }, error);
  if (framebuffer != nullptr) {
    *framebuffer = sim::framebuffer_sha256(simulator->raster());
  }
  return ok;
}
// The error of a script that must fail to run (it parsed).
std::string run_error(const std::string& text) {
  std::string error;
  CHECK_FALSE(run_text(text, error));
  return error;
}
bool holds(const std::string& text) {
  std::string error;
  const bool ok = run_text(text, error);
  CHECK_MESSAGE(error.empty(), error);
  return ok;
}
}  // namespace

TEST_CASE("scenario: assert and mark parse into commands that keep their words and the line") {
  std::vector<ScenarioCommand> cmds;
  std::string error;
  NEED(parse("# c\n"
             "ASSERT State screen=PATTERN  playing=~^[01]$   # why\n"
             "assert model tempo=13000\n"
             "Mark MODEL first\n"
             "assert model digest=@first\n"
             "assert model digest=0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef\n"
             "assert file \"Projects/Track 01/project.o3pj\" size>100\n"
             "assert file Samples exists\n",
             cmds, error));
  NEED(cmds.size() == 7u);
  CHECK((cmds[0].kind == Kind::Assert && cmds[0].target == ScenarioCommand::Target::State && cmds[0].line == 2));
  CHECK((cmds[0].args == std::vector<std::string>{"screen=PATTERN", "playing=~^[01]$"}));
  CHECK(cmds[0].text == "ASSERT State screen=PATTERN  playing=~^[01]$");  // the line as written, the comment cut
  CHECK((cmds[1].kind == Kind::Assert && cmds[1].target == ScenarioCommand::Target::Model && cmds[1].args == std::vector<std::string>{"tempo=13000"}));
  CHECK((cmds[2].kind == Kind::Mark && cmds[2].name == "first" && cmds[2].line == 4));
  CHECK((cmds[3].target == ScenarioCommand::Target::Model && cmds[3].args[0] == "digest=@first"));
  CHECK(cmds[4].args[0].size() == 7u + 64u);
  CHECK((cmds[5].target == ScenarioCommand::Target::File && cmds[5].args == std::vector<std::string>{"Projects/Track 01/project.o3pj", "size>100"}));  // the quotes make one word
  CHECK(cmds[6].args == std::vector<std::string>{"Samples", "exists"});
}

TEST_CASE("scenario: a wrong assert or mark is refused when the script is read, with the line") {
  CHECK(parse_error("assert\n").find("line 1: assert needs state, model, file or audio") == 0u);
  CHECK(parse_error("assert state\n").find("line 1: assert needs") == 0u);
  CHECK(parse_error("wait 1\nassert colour a=b\n").find("line 2: assert needs state, model, file or audio") == 0u);
  CHECK(parse_error("assert state screen\n").find("wants key=value") != std::string::npos);
  CHECK(parse_error("assert state =x\n").find("wants key=value") != std::string::npos);
  CHECK(parse_error("assert state a=~(b\n").find("not a regular expression") != std::string::npos);
  CHECK(parse_error("assert state screen=~\n").find("line 1: assert state: 'screen=~' has no regular expression after the ~") == 0u);  // an empty expression matches anything: refused
  CHECK(parse_error("assert state screen=PATTERN msg=~\n").find("'msg=~' has no regular expression") != std::string::npos);
  CHECK(parse_error("assert model tempo\n").find("assert model needs one <path>=<value>") != std::string::npos);
  CHECK(parse_error("assert model tempo=\n").find("assert model needs one <path>=<value>") != std::string::npos);
  CHECK(parse_error("assert model tempo=1 order.0=1\n").find("assert model needs one <path>=<value>") != std::string::npos);
  CHECK(parse_error("assert model tem=1\n").find("unknown model path 'tem'") != std::string::npos);
  CHECK(parse_error("assert model order.x=1\n").find("song position") != std::string::npos);
  CHECK(parse_error("assert model order.256=1\n").find("song position") != std::string::npos);
  CHECK(parse_error("assert model pat.0.0.0.note=1\n").find("the pattern must be 1..255") != std::string::npos);
  CHECK(parse_error("assert model pat.256.0.0.note=1\n").find("the pattern must be 1..255") != std::string::npos);
  CHECK(parse_error("assert model pat.1.16.0.note=1\n").find("the track must be 0..15") != std::string::npos);
  CHECK(parse_error("assert model pat.1.0.128.note=1\n").find("the step must be 0..127") != std::string::npos);
  CHECK(parse_error("assert model pat.1.0.0.pitch=1\n").find("the step field must be") != std::string::npos);
  CHECK(parse_error("assert model pat.1.0.0=1\n").find("unknown model path") != std::string::npos);
  CHECK(parse_error("assert model inst.0.name=A\n").find("the instrument must be 1..64") != std::string::npos);
  CHECK(parse_error("assert model inst.65.name=A\n").find("the instrument must be 1..64") != std::string::npos);
  CHECK(parse_error("assert model inst.1.sparkle=1\n").find("no instrument parameter 'sparkle'") != std::string::npos);
  CHECK(parse_error("assert model inst.1.attack=1\n").find("no instrument parameter 'attack'") != std::string::npos);  // the envelope rows are env<e>.attack
  CHECK(parse_error("assert model digest=@nope\n").find("no 'mark model nope' before this line") != std::string::npos);
  CHECK(parse_error("mark model a\nassert model digest=@b\n").find("line 2") == 0u);
  CHECK(parse_error("assert model digest=abc\n").find("digest= wants @<label> or 64 hex digits") != std::string::npos);
  CHECK(parse_error("mark\n").find("mark needs 'model' and a label") != std::string::npos);
  CHECK(parse_error("mark state a\n").find("mark needs 'model'") != std::string::npos);
  CHECK(parse_error("mark model a/b\n").find("mark needs 'model'") != std::string::npos);
  CHECK(parse_error("mark model a\nmark model a\n").find("line 2: mark label 'a' is used twice") == 0u);
  CHECK(parse_error("assert file x\n").find("assert file needs a path") != std::string::npos);
  CHECK(parse_error("assert file x exists now\n").find("assert file needs a path") != std::string::npos);
  CHECK(parse_error("assert file x present\n").find("is not a test") != std::string::npos);
  CHECK(parse_error("assert file x size>abc\n").find("whole number of bytes") != std::string::npos);
  CHECK(parse_error("assert file x size>4294967296\n").find("whole number of bytes") != std::string::npos);
  CHECK(parse_error("assert file x sha=12\n").find("is not a test") != std::string::npos);
  CHECK(parse_error("assert file a:b exists\n").find("is not a path of the card") != std::string::npos);
  CHECK(parse_error("assert file ../x exists\n").find("is not a path of the card") != std::string::npos);
  CHECK(parse_error("assert file \"Projects/x exists\n").find("line 1: a double quote is never closed") == 0u);
}

TEST_CASE("scenario: assert state holds for an exact value, several pairs and a regular expression, and fails with the state line") {
  CHECK(holds("assert state screen=PATTERN\n"));
  CHECK(holds("assert state screen=PATTERN overlay=NONE playing=0 seq=0 pads=NOTES shift=0 order=0 sel=- msg=-\n"));
  CHECK(holds("assert state screen=~^PAT\n"));
  CHECK(holds("assert state screen=~TTER msg=~^-$\n"));
  CHECK(holds("assert state screen=~^(PATTERN|SONG)$\n"));
  CHECK(holds("assert state keys=~^$\n"));  // the pair the line lacks reads as the empty text, as o3hil's search does
  CHECK(holds("tap song\nassert state screen=SONG page=0\ntap song\nassert state screen=SONG page=1\n"));
  CHECK(holds("assert state screen=PATTERN\nassert state screen=PATTERN\n"));  // asserting changes nothing: it can be said again

  std::string e = run_error("wait 3\nassert state screen=SONG\n");
  CHECK(e.find("line 2: assertion failed: assert state screen=SONG: state screen is 'PATTERN', wanted 'SONG'") == 0u);
  e = run_error("assert state screen=PATTERN overlay=LEARN\n");
  CHECK(e.find("state overlay is 'NONE', wanted 'LEARN'") != std::string::npos);  // the first pair that does not hold
  e = run_error("assert state screen=~^SO\n");
  CHECK(e.find("state screen is 'PATTERN', wanted ~'^SO'") != std::string::npos);
  e = run_error("assert state colour=red\n");
  CHECK(e.find("the state line has no colour (screen=PATTERN") != std::string::npos);  // and says what the line is
  e = run_error("assert state screen=PATTER\n");  // exact: a prefix is not the value
  CHECK(e.find("state screen is 'PATTERN'") != std::string::npos);
  e = run_error("assert state screen=pattern\n");  // and the case counts
  CHECK(e.find("state screen is 'PATTERN'") != std::string::npos);
  e = run_error("assert state msg=~^[A-Z]+$\n");
  CHECK(e.find("state msg is '-'") != std::string::npos);

  const std::vector<std::string> words = {"screen=PATTERN", "row=~^[0-9]+$"};
  std::vector<sim::StatePair> pairs;
  std::string error;
  NEED(sim::parse_state_pairs(words, pairs, error));
  CHECK(sim::state_problem("a=1 screen=PATTERN row=12 b=2", pairs).empty());
  CHECK(sim::state_problem("screen=PATTERN row=x", pairs) == "state row is 'x', wanted ~'^[0-9]+$'");
  CHECK(sim::state_problem("screen=PATTERN screen=SONG row=1", pairs).empty());  // the first of a repeated key stays, as o3hil.py's parse_pairs has it
  CHECK(sim::state_problem("", pairs).find("the state line has no screen") == 0u);
}

TEST_CASE("scenario: assert model reads tempo, the song, steps and instruments, and fails with what the document has") {
  const std::string doc = "document\n";
  CHECK(holds(doc + "assert model tempo=13000\nassert model order.len=1\nassert model order.0=1\nassert model order.1=0\n"));
  CHECK(holds(doc + "assert model pat.1.0.0.note=25\nassert model pat.1.0.1.note=0\nassert model pat.1.0.1.instr=0\nassert model pat.1.0.1.vol=255\n"));
  CHECK(holds(doc + "assert model pat.1.0.1.fx1=0:0\nassert model pat.1.0.1.fx2=0:0\nassert model pat.1.0.127.note=0\n"));
  CHECK(holds(doc + "assert model inst.1.name=KICK\nassert model inst.10.name=DRUMS\n"));
  CHECK(holds(doc + "assert model inst.1.panning=26\nassert model inst.1.tune=-3\nassert model inst.1.fine_tune=-20\nassert model inst.2.volume=96\n"));
  CHECK(holds(doc + "assert model inst.1.cutoff=0.3\nassert model inst.1.cutoff=0.3000001\nassert model inst.1.cutoff=.3\n"));  // a number is compared as one
  CHECK(holds(doc + "assert model inst.1.env0.attack=10\nassert model inst.1.env2.enable=1\nassert model inst.1.env2.amount=0.5\n"));
  CHECK(holds(doc + "assert model inst.1.channel=0\n"));
  // an edit by the keys moves the value, and by the hundred-thousandth of a step the tolerance does not hold
  CHECK(holds(doc + "tap rec\ntap down\npad 36\nassert model pat.1.0.1.note=37\nassert model pat.1.0.1.instr=1\n"));

  std::string e = run_error(doc + "assert model tempo=12800\n");
  CHECK(e.find("line 2: assertion failed: assert model tempo=12800: model tempo is '13000', wanted '12800'") == 0u);
  // a whole-number read has no tolerance: one off, or a fraction off, or spelled another way, is a failure (a float parameter keeps its 1e-4, above)
  for (const char* near : {"tempo=13001", "tempo=12999", "tempo=13000.0001", "tempo=13000.0", "tempo=1.3e4", "tempo=+13000", "tempo=0x32c8", "order.0=1.00001", "order.len=1.0", "pat.1.0.0.note=25.00001", "pat.1.0.0.vol=255.0"}) {
    e = run_error(doc + "assert model " + near + "\n");
    CHECK_MESSAGE(e.find("assertion failed: assert model " + std::string(near) + ": model ") != std::string::npos, near << ": " << e);
  }
  CHECK(holds(doc + "assert model tempo=13000\nassert model pat.1.0.0.note=25\nassert model inst.1.channel=0\nassert model inst.1.tune=-3\n"));
  e = run_error(doc + "assert model inst.1.tune=-2\n");  // an Int row is exact too
  CHECK(e.find("model inst.1.tune is '-3', wanted '-2'") != std::string::npos);
  e = run_error(doc + "assert model inst.1.name=kick\n");  // text is exact
  CHECK(e.find("model inst.1.name is 'KICK', wanted 'kick'") != std::string::npos);
  e = run_error(doc + "assert model inst.1.cutoff=0.31\n");
  CHECK(e.find("model inst.1.cutoff is '0.3', wanted '0.31'") != std::string::npos);
  e = run_error(doc + "assert model pat.1.0.0.note=26\n");
  CHECK(e.find("model pat.1.0.0.note is '25', wanted '26'") != std::string::npos);
  e = run_error(doc + "assert model pat.1.0.0.note=abc\n");  // not a number: compared as text
  CHECK(e.find("model pat.1.0.0.note is '25', wanted 'abc'") != std::string::npos);
  e = run_error(doc + "assert model pat.1.0.0.fx1=1:2\n");
  CHECK(e.find("model pat.1.0.0.fx1 is") != std::string::npos);
  e = run_error(doc + "assert model name=Demo\n");  // the project's title (the demo document has none)
  CHECK(e.find("model name is '', wanted 'Demo'") != std::string::npos);
  e = run_error(doc + "assert model pat.9.0.0.note=0\n");
  CHECK(e.find("the document has no pattern 9") != std::string::npos);
  e = run_error(doc + "assert model inst.20.name=A\n");
  CHECK(e.find("instrument 20 is empty") != std::string::npos);
  e = run_error(doc + "assert model inst.20.volume=1\n");
  CHECK(e.find("instrument 20 is empty") != std::string::npos);
  e = run_error("assert model tempo=13000\n");
  CHECK(e.find("line 1: assertion failed: assert model tempo=13000: there is no document") == 0u);
  e = run_error("mark model a\n");
  CHECK(e.find("line 1: mark model 'a': there is no document") == 0u);
}

TEST_CASE("scenario: every instrument parameter has a caption of its own, and the readable ones are in the table") {
  const std::vector<std::string>& names = sim::instrument_param_captions();
  CHECK(names.size() > 80u);
  std::set<std::string> seen;
  for (const std::string& n : names) {
    CHECK_MESSAGE(!n.empty(), "an empty caption");
    CHECK_MESSAGE(seen.insert(n).second, "caption '" << n << "' names two parameters");
  }
  for (const std::string want : {"volume", "panning", "tune", "fine_tune", "cutoff", "resonance", "bit_depth", "env0.attack", "env5.release", "mod2.lfo_speed", "velocity", "pb_range", "cc_a", "vel"}) {
    CHECK_MESSAGE(seen.count(want) == 1u, "no caption '" << want << "'");
  }
  // and each one parses as the tail of a path, to the field its row is
  std::string why;
  for (const std::string& n : names) {
    CHECK_MESSAGE(sim::valid_model_path("inst.7." + n, why), why);
  }
}

TEST_CASE("scenario: the model digest follows the document, byte for byte, and survives an undo") {
  const std::string doc = "document\nmark model start\n";
  CHECK(holds(doc + "assert model digest=@start\nwait 50\nassert model digest=@start\n"));  // time alone changes nothing
  CHECK(holds(doc + "tap rec\ntap down\npad 36\naction edit.undo\nassert model digest=@start\n"));
  CHECK(holds(doc + "tap params\ntap down\nencoder -12\naction edit.undo\nassert model digest=@start\n"));
  CHECK(holds(doc + "tap rec\ntap down\npad 36\nmark model edited\naction edit.undo\nassert model digest=@start\naction edit.redo\nassert model digest=@edited\n"));
  const std::string e = run_error(doc + "tap rec\ntap down\npad 36\nassert model digest=@start\n");
  CHECK(e.find("line 6: assertion failed: assert model digest=@start: the document's digest is ") == 0u);
  CHECK(e.find(", marked as start: ") != std::string::npos);

  // two documents opened the same way have one digest; the digest is the SHA-256 the writer gives: 64 hex digits, a literal can be asserted
  auto a = std::make_unique<sim::Simulator>();
  auto b = std::make_unique<sim::Simulator>();
  a->open_document();
  b->open_document();
  const std::string da = sim::model_digest(*a->project());
  CHECK(da.size() == 64u);
  CHECK(da == sim::model_digest(*b->project()));
  b->open_document(true);  // with the two Learn bindings: another document
  CHECK(da != sim::model_digest(*b->project()));
  CHECK(holds("document\nassert model digest=" + da + "\n"));
  CHECK(holds("document\nassert model digest=" + std::string(da).replace(0, 1, std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(da[0]))))) + "\n"));  // any case
  CHECK_FALSE(run_error("document\nassert model digest=" + std::string(64, '0') + "\n").empty());
}

TEST_CASE("scenario: a run with assertions draws the pictures of the run without them") {
  const std::string base = "document\ntap rec\ntap down\npad 36\nwait 30\nsnap x\n";
  const std::string with = "document\nassert state screen=PATTERN\nmark model m\ntap rec\ntap down\npad 36\nassert model pat.1.0.1.note=37\nwait 30\nassert state row=2\nassert model digest=@m\nsnap x\n";
  // (the digest assertion above must FAIL: the pattern was edited since the mark; take it out for the comparison)
  std::string error;
  CHECK_FALSE(run_text(with, error));
  CHECK(error.find("assert model digest=@m") != std::string::npos);
  const std::string with_ok = "document\nassert state screen=PATTERN\nmark model m\ntap rec\ntap down\npad 36\nassert model pat.1.0.1.note=37\nwait 30\nassert state row=2\nassert model order.0=1\nsnap x\n";
  std::string plain_hash, asserted_hash;
  CHECK(run_text(base, error, &plain_hash));
  CHECK(run_text(with_ok, error, &asserted_hash));
  CHECK(plain_hash == asserted_hash);
}

TEST_CASE("scenario: assert file reads the card by path, and says why it does not hold") {
  platform::host::FakeStorage card;
  const std::vector<std::uint8_t> bytes = {'T', 'r', 'a', 'c', 'k', 'e', 'r', '!'};  // 8 bytes
  card.add_file("Projects/Track 01/project.o3pj", bytes);
  card.add_file("Projects/Track 01/empty.bin", std::vector<std::uint8_t>{});
  card.add_dir("Samples");
  const std::string sha = platform::host::sha256_hex(bytes.data(), bytes.size());
  const auto problem = [&](const std::string& words) {
    // the words after `assert file`, through the same parser the scenario uses
    std::vector<ScenarioCommand> cmds;
    std::string error;
    if (!parse("assert file " + words + "\n", cmds, error) || cmds.size() != 1u) {
      return "(does not parse: " + error + ")";
    }
    std::string path;
    sim::FileWant want;
    if (!sim::parse_file_want(cmds[0].args, path, want, error)) {
      return "(does not parse: " + error + ")";
    }
    return sim::file_problem(&card, path, want);
  };
  CHECK(card.mount() == hal::storage::Status::Ok);

  CHECK(problem("\"Projects/Track 01/project.o3pj\" exists").empty());
  CHECK(problem("\"projects/TRACK 01/Project.o3pj\" exists").empty());  // FAT: case does not count
  CHECK(problem("Samples exists").empty());                              // a folder exists
  CHECK(problem("Settings exists").find("Settings is not on the card (not found)") == 0u);
  CHECK(problem("Settings absent").empty());
  CHECK(problem("\"Projects/Track 01/project.o3pj\" absent") == "Projects/Track 01/project.o3pj exists");
  CHECK(problem("\"Projects/Track 01/project.o3pj/x\" absent").empty());  // a file where a folder was wanted: not there either
  CHECK(problem("\"Projects/Track 01/project.o3pj\" size=8").empty());
  CHECK(problem("\"Projects/Track 01/project.o3pj\" size>7").empty());
  CHECK(problem("\"Projects/Track 01/project.o3pj\" size<9").empty());
  CHECK(problem("\"Projects/Track 01/project.o3pj\" size=7") == "Projects/Track 01/project.o3pj is 8 bytes, wanted = 7");
  CHECK(problem("\"Projects/Track 01/project.o3pj\" size>8") == "Projects/Track 01/project.o3pj is 8 bytes, wanted > 8");
  CHECK(problem("\"Projects/Track 01/project.o3pj\" size<8") == "Projects/Track 01/project.o3pj is 8 bytes, wanted < 8");
  CHECK(problem("\"Projects/Track 01/empty.bin\" size=0").empty());
  CHECK(problem("\"Projects/Track 01/project.o3pj\" sha=" + sha).empty());
  CHECK(problem("\"Projects/Track 01/project.o3pj\" sha=" + std::string(64, '0')).find("has the SHA-256 " + sha + ", wanted 000") != std::string::npos);
  CHECK(problem("\"Projects/Track 01/empty.bin\" sha=e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855").empty());  // the SHA-256 of nothing
  CHECK(problem("Samples size=0") == "Samples is a folder");
  CHECK(problem("\"Projects/Track 01/gone.bin\" size=0").find("gone.bin is not on the card") != std::string::npos);

  card.set_present(false);  // pulled
  CHECK(problem("Samples exists") == "the card is pulled");
  card.set_present(true);
  CHECK(card.mount() == hal::storage::Status::Ok);
  CHECK(problem("Samples exists").empty());
  CHECK(sim::file_problem(nullptr, "Samples", sim::FileWant{}) == "no card");
  CHECK(card.open_dirs() == 0u);  // every handle the check opened is closed
}

// OS-FWCONFIG slice 2b: `firmware on` gives the dialog the fake update port, with the four files of Simulator::firmware_on.
TEST_CASE("scenario: firmware on puts the fake update port behind Config's F8 UPDATE, and its four files end as their scripts say") {
  CHECK(parse_error("firmware\n").find("line 1: firmware needs the word 'on'") == 0);
  CHECK(parse_error("firmware off\n").find("line 1: firmware needs the word 'on'") == 0);
  CHECK(parse_error("firmware on now\n").find("line 1: firmware needs the word 'on'") == 0);
  CHECK(parse_error("firmware on\nfirmware on\n").find("line 2: firmware on: the update port is on already") == 0);

  // Without the command there is no port: INFO labels no key and F8 does nothing.
  {
    std::vector<ScenarioCommand> cmds;
    std::string error;
    NEED(parse("tap config\ntap f4\ntap f8\n", cmds, error));
    sim::Simulator off;
    NEED(sim::run_scenario(cmds, off, [](const std::string&, const sim::Simulator&) { return true; }, error));
    CHECK_FALSE(off.has_firmware());
    CHECK_FALSE(off.app().state().firmware.open());
    CHECK_FALSE(off.app().model().firmware);
  }

  // One run: the list, a check that fails, a check that is cancelled, a good check whose hand-over is not taken, then a hand-over that never ends.
  const auto run = [](const std::string& script, sim::Simulator& s) {
    std::vector<ScenarioCommand> cmds;
    std::string error;
    NEED(parse(script, cmds, error));
    NEED(sim::run_scenario(cmds, s, [](const std::string&, const sim::Simulator&) { return true; }, error));
    CHECK(error.empty());
  };
  sim::Simulator s;
  using Phase = app::FirmwareView::Phase;
  const auto fw = [&s]() -> const app::FirmwareView& { return s.app().state().firmware; };

  run("firmware on\ntap config\ntap f4\ntap f8\n", s);
  CHECK(s.has_firmware());
  CHECK(s.app().model().firmware);
  NEED(fw().phase == Phase::List);
  NEED(fw().n == 4);
  CHECK(std::string(fw().files[0].name) == "TRACKER_1.PTF");
  CHECK(fw().files[0].reset_target);
  CHECK(std::string(fw().files[1].name) == "SLOW_CHECK.PTF");
  CHECK(std::string(fw().files[2].name) == "BAD_IMAGE.PTF");
  CHECK(std::string(fw().files[3].name) == "NEVER_ENDS.PTF");
  CHECK(s.firmware().installs == 0u);

  run("tap down 2\ntap f1\ntap f1\nwait 20\n", s);  // BAD_IMAGE.PTF: the check fails, the list says why, nothing is installed
  CHECK(fw().phase == Phase::List);
  CHECK(std::string(fw().msg.c_str()) == "NOT A VALID IMAGE: FLASH CONFIGURATION FIELD IS NOT THE REQUIRED ONE AT 1104");
  CHECK(s.firmware().last_name == "BAD_IMAGE.PTF");

  run("tap up\ntap f1\ntap f1\nwait 250\n", s);  // SLOW_CHECK.PTF: still reading 250 ms in (600 passes in all), then F8 CANCEL drops it
  CHECK(fw().phase == Phase::Checking);
  CHECK(fw().done > 0u);
  CHECK(fw().done < fw().total);
  run("tap f8\n", s);
  CHECK(fw().phase == Phase::List);
  CHECK(s.firmware().count("cancel") >= 1u);
  run("tap f1\ntap f1\nwait 700\n", s);  // ... and left to run it ends: question 2 with the script's length and CRC
  CHECK(fw().phase == Phase::AskInstall);
  CHECK(fw().image == 0x3E800u);
  CHECK(fw().crc == 0x0BADC0DEu);
  CHECK(s.firmware().installs == 0u);

  run("tap up\ntap f1\ntap f1\nwait 20\ntap f1\nwait 100\n", s);  // TRACKER_1.PTF: question 2 (the other key took the first back), INSTALL, the bootloader does not take over
  CHECK(fw().phase == Phase::List);
  CHECK(std::string(fw().msg.c_str()) == "UPDATE DID NOT START");
  CHECK(s.firmware().installs == 1u);
  CHECK(s.firmware().last_name == "TRACKER_1.PTF");

  run("tap down 3\ntap f1\ntap f1\nwait 20\ntap f1\nwait 100\n", s);  // NEVER_ENDS.PTF: the hand-over never ends, and no key leaves it
  CHECK(fw().phase == Phase::Installing);
  CHECK(s.firmware().installs == 2u);
  run("tap f8\ntap f1\ntap config\nwait 5000\n", s);
  CHECK(fw().phase == Phase::Installing);
  CHECK(s.firmware().installs == 2u);
  CHECK(s.healthy());
}
