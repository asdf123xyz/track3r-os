// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-UIFIX / OS-SHIFTROW: the Shift row of a screen shows what Shift + F1..F8 really do there (docs/ui/ui-map.md section 5, rules 1-3). Every key is resolved the way the App does, with
// core::resolve against the context chains of the screen (mode, field, screen, Global; a capturing screen ends the chain at itself), so the built-in bindings and the labels cannot drift apart:
//   - Shift + F belongs to the screen (rule 1): a screen's own Shift binding, or nothing. Global binds all eight to the null action (the keymap's `none`), so a Shift + F press never
//     reaches the plain key; a key that resolves to nothing (unbound, captured, or the null action) has the label "-" (rule 2);
//   - a Shift + F binding: the label is not "-", and it is not the plain key's label either (Shift + F8 is REDO only where F8 is UNDO, rule 3: the labels differ by construction).
#include <doctest/doctest.h>

#include <o3os/app/bindings.h>
#include <o3os/app/buttons.h>
#include <o3os/app/model.h>
#include <o3os/app/screens/ids.h>
#include <o3os/app/screens/screen.h>
#include <o3os/app/view_state.h>
#include <o3os/core/binding.h>

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

using namespace o3os;
using app::Button;
using app::Context;
using app::ScreenId;

namespace {

constexpr Button kFKeys[8] = {Button::F1, Button::F2, Button::F3, Button::F4, Button::F5, Button::F6, Button::F7, Button::F8};

// "-" (and "--", the plain rows' filler, and "") is the label of a key that does nothing.
bool blank(const char* label) { return label == nullptr || label[0] == '\0' || std::strcmp(label, "-") == 0 || std::strcmp(label, "--") == 0; }

// The field levels a screen's chain can have: Screen::field_context read on every page of the screen, and after walking the cursor along its second axis (Pattern's columns).
// A field level that depends on other state is not seen here: the sanity check in the test case names the ones that must be.
std::vector<Context> fields_of(const app::Screen& screen, const app::Model& model) {
  std::vector<Context> fields;
  for (unsigned page = 0; page < screen.pages(); ++page) {
    app::ViewState view;
    view.page[app::screen_index(screen.id())] = static_cast<std::uint8_t>(page);
    for (int step = 0; step < 8; ++step) {
      const Context field = screen.field_context(model, view);
      bool seen = field == Context::None;
      for (Context known : fields) {
        seen = seen || known == field;
      }
      if (!seen) {
        fields.push_back(field);
      }
      (void)screen.step(model, view, 1, 0);
    }
  }
  return fields;
}

// What Shift + F`n` does in one chain: nothing, or a Shift binding of the chain. Anything else (the plain key through the resolver's retry) is a failure the caller reports.
struct Expect {
  enum class Kind { Nothing, ShiftBinding, FallThrough } kind = Kind::Nothing;
};

Expect expected(const core::BindingSet& set, const core::ContextChain& chain, unsigned n) {
  const core::Gesture shifted{0, core::GestureKind::Tap, app::button_ref(kFKeys[n]), app::button_ref(Button::Shift), 0};
  const core::Resolution r = core::resolve(set, chain, shifted);
  Expect e;
  if (r.status != core::Resolution::Status::Bound || r.action == core::action_id("ui.none")) {
    return e;  // unbound, captured, or Global's `ui.none`
  }
  bool shift_binding = false;  // a binding of the chain that answers Shift + F`n` itself, and not the plain key through the resolver's retry
  const core::BindingView builtin = set.layers[static_cast<unsigned>(core::Layer::Builtin)];
  for (std::size_t i = 0; i < builtin.count; ++i) {
    for (unsigned level = 0; level < chain.reach(); ++level) {
      shift_binding = shift_binding || (builtin.items[i].context == chain.ids[level] && core::trigger_matches(builtin.items[i].trigger, shifted));
    }
  }
  e.kind = shift_binding ? Expect::Kind::ShiftBinding : Expect::Kind::FallThrough;
  return e;
}

}  // namespace

TEST_CASE("shift labels: every Shift + F key of every screen shows what it does, in every chain the screen can have") {
  const app::ScreenTable table = app::screen_registry();
  CHECK(table.count == app::kScreenCount);
  const core::BindingSet set{{core::BindingView{}, core::BindingView{}, app::builtin_bindings()}};
  app::Model model;
  app::ViewState view;
  for (std::size_t s = 0; s < table.count; ++s) {
    const app::ScreenEntry& e = table.items[s];
    CHECK_MESSAGE(e.screen != nullptr, std::string(e.name) << ": no screen");
    if (e.screen == nullptr) {
      continue;
    }
    const app::Screen& screen = *e.screen;
    const char* const* plain = screen.soft_keys();
    const app::ModeTable modes = screen.modes();
    // Each page is its own set of keys (Record's RADIO page has Shift keys the other pages do not): the row the screen shows on the page, against the chains the page can have.
    for (unsigned page = 0; page < (screen.pages() == 0 ? 1u : screen.pages()); ++page) {
      app::ViewState pview;
      pview.page[app::screen_index(screen.id())] = static_cast<std::uint8_t>(page);
      const char* const* row = screen.shift_row(pview);
      CHECK_MESSAGE(row != nullptr, std::string(e.name) << " page " << page << ": no Shift row");
      if (row == nullptr) {
        continue;
      }
      for (unsigned n = 0; n < 8; ++n) {
        CHECK_MESSAGE(row[n] != nullptr, std::string(e.name) << " Shift + F" << n + 1 << ": no label");
        CHECK_MESSAGE((row[n] == nullptr || std::strlen(row[n]) <= 8), std::string(e.name) << " Shift + F" << n + 1 << ": '" << std::string(row[n]) << "' does not fit a key cap");
      }

      // The mode levels (a mode's context, or none) times the field levels this page can have (a field's context, or none).
      std::vector<Context> mode_levels{Context::None};
      for (unsigned i = 0; i < modes.count; ++i) {
        mode_levels.push_back(modes.items[i].context);
      }
      // The field levels the page really has, walked along the cursor's second axis on a copy (a step that turns the page is not this page's).
      std::vector<Context> field_levels;
      app::ViewState walk = pview;
      for (int step = 0; step < 8 && walk.page[app::screen_index(screen.id())] == page; ++step) {
        const Context field = screen.field_context(model, walk);
        bool seen = false;
        for (Context known : field_levels) {
          seen = seen || known == field;
        }
        if (!seen) {
          field_levels.push_back(field);
        }
        (void)screen.step(model, walk, 1, 0);
      }
      if (field_levels.empty()) {
        field_levels.push_back(Context::None);
      }
      for (Context mode : mode_levels) {
        for (Context field : field_levels) {
          core::ContextChain chain;
          if (mode != Context::None) {
            CHECK(chain.push(app::context_id(mode)));
          }
          if (field != Context::None) {
            CHECK(chain.push(app::context_id(field)));
          }
          CHECK(chain.push(app::context_id(screen.context()), screen.captures()));
          CHECK(chain.push(app::context_id(Context::Global)));
          for (unsigned n = 0; n < 8; ++n) {
            if (row[n] == nullptr) {
              continue;
            }
            const std::string where = std::string(e.name) + " page " + std::to_string(page) + " Shift + F" + std::to_string(n + 1) + " ('" + row[n] + "', plain '" + (plain[n] != nullptr ? plain[n] : "") + "')";
            switch (expected(set, chain, n).kind) {
              case Expect::Kind::Nothing:
                CHECK_MESSAGE(blank(row[n]), where << " does nothing and its label is not '-'");
                break;
              case Expect::Kind::ShiftBinding:
                CHECK_MESSAGE(!blank(row[n]), where << " is bound and its label is '-'");
                CHECK_MESSAGE((blank(plain[n]) || std::strcmp(row[n], plain[n]) != 0), where << " is bound to its own function and its label is the plain key's");
                break;
              case Expect::Kind::FallThrough:
                CHECK_MESSAGE(false, where << " falls through to the plain key (rule 1: Shift + F never does)");
                break;
            }
          }
        }
      }
    }
  }
}

TEST_CASE("shift labels: the field levels the walk finds are the ones the screens have") {
  // The generic walk stands in for a table: if a screen's field_context stops being reachable by pages and the cursor, the chains above lose a level silently.
  app::Model model;
  const app::Screen* pattern = app::find_screen(app::screen_registry(), ScreenId::Pattern)->screen;
  const app::Screen* wave = app::find_screen(app::screen_registry(), ScreenId::Wave)->screen;
  CHECK(fields_of(*pattern, model).size() == 4);  // Note, Instr, Vol and the FX columns
  CHECK(fields_of(*wave, model).size() == 1);     // the CHOP page
}

TEST_CASE("shift labels: Song's Shift + F7 is PADS (OS-SONGPADS), the pads' TRACKS / SLOTS switch, and Shift + F6 is still NAME") {
  const app::Screen* song = app::find_screen(app::screen_registry(), ScreenId::Song)->screen;
  CHECK(song != nullptr);
  if (song == nullptr) {
    return;
  }
  app::ViewState view;
  CHECK(std::string(song->shift_row(view)[6]) == "PADS");
  CHECK(std::string(song->shift_row(view)[5]) == "NAME");
  const core::BindingSet set{{core::BindingView{}, core::BindingView{}, app::builtin_bindings()}};
  core::ContextChain chain;
  CHECK(chain.push(app::context_id(Context::Song)));
  CHECK(chain.push(app::context_id(Context::Global)));
  const core::Gesture shifted{0, core::GestureKind::Tap, app::button_ref(Button::F7), app::button_ref(Button::Shift), 0};
  const core::Resolution r = core::resolve(set, chain, shifted);
  CHECK(r.status == core::Resolution::Status::Bound);
  CHECK(r.action == core::action_id("song.pads"));
}
