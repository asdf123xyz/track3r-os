// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Screens (ADR 0009 rev 1, mechanism 3; OS-002b). A Screen draws one full-panel view from the model and the ViewState,
// says which binding contexts it lives in, and nothing else: actions are in the feature files (there is no `on_action`),
// and a screen switches on no action. Screens are static instances; the registry is one constexpr table.
//
// Adding a screen:
//   1. a class derived from Screen (id, render, context, soft_keys; enter / leave / modes / pages when it has them, and the
//      Cursor interface when it has a list or a grid to move: count, position, page, set_position, step, and optionally
//      track, home, end), its static instance next to it (see pattern_screen.h);
//   2. a ScreenId (ids.h) if it has none, and one row in the registry (registry.cpp) with its name, screen button, icon;
//   3. its default bindings in app/src/bindings.cpp: the screen button's `nav.screen`, and its own context's keys.
// The Navigator, the soft-key labels and the gallery pick it up from the registry; the unit tests check ids, names and
// that every row's screen button has its `nav.screen` binding.
//
// Navigation. The navigation actions (nav.*, cursor.*) move whatever owns the input through the Cursor interface that a
// Screen (and an Overlay) is: they never name a screen, so a new screen is navigable by implementing it and by nothing else.
//
// Modes. A screen may declare modes (Pattern: Edit and Browse; Follow and Select are retired): one enum, one ModeDef row each, naming the binding
// context of that mode, and `default_mode()` for the one it starts in. The mode is the "mode" level of the context chain (overlay,
// mode, field, screen, global), so the same key can do something else in another mode. `view.mode` / `view.mode_next` choose it;
// the mode is kept per screen. A screen without modes returns an empty table and its chain has no mode level.
//
// Fields. A screen may name the field under its cursor (OS-009 D3, `field_context`): Pattern's Note, Instr, Vol and FX columns.
// The field is the level between the mode and the screen: a mode's row overrides, a field's row specialises the screen's default.
// Moving the cursor from one field to another does not reset the gesture layer (App::sync_context ignores this level), or a
// button held across the move would be forgotten.
//
// Pages. `pages()` is how many a screen has; pressing the current screen's own button again cycles them.

#include <o3os/app/bindings.h>
#include <o3os/app/buttons.h>
#include <o3os/app/model.h>
#include <o3os/app/pad_palette.h>
#include <o3os/app/screens/cursor.h>
#include <o3os/app/screens/ids.h>
#include <o3os/app/view_state.h>
#include <o3os/core/param.h>
#include <o3os/ui/assets.h>
#include <o3os/ui/renderer.h>

#include <cstddef>

namespace o3os::app {

struct ModeDef {
  const char* name;  // lower case: shown on the info line ("edit ON")
  Context context;   // the binding context of the mode
  // OS-004d: a retired mode keeps its row (so the mode numbers stay what they were) but is no longer entered: `view.mode_next` skips it, `view.mode` refuses it.
  bool retired = false;
};

struct ModeTable {
  const ModeDef* items;
  unsigned count;
};

// The Shift rows (Screen::soft_keys_shifted): the default is nothing (OS-SHIFTROW rule 1: Shift + F belongs to the screen, Global binds it to ui.none). kJumpShiftKeys is the quarter jumps
// Shift + F1..F4 and the end Shift + F5, which Song, File and Sample load bind in their own contexts.
inline constexpr const char* kJumpShiftKeys[8] = {"0/4", "1/4", "2/4", "3/4", "END", "-", "-", "-"};
inline constexpr const char* kNoShiftKeys[8] = {"-", "-", "-", "-", "-", "-", "-", "-"};

class Screen : public Cursor {
 public:
  // Called by navigation: `leave` on the screen being left, then `enter` on the one being entered (view.screen is
  // already the new one). They put the screen's own state in order (a cursor inside a list that changed); they draw nothing.
  virtual void enter(const Model& model, ViewState& view) const noexcept {
    (void)model;
    (void)view;
  }
  virtual void leave(const Model& model, ViewState& view) const noexcept {
    (void)model;
    (void)view;
  }

  // Appends the whole screen to the frame in progress. `covered` is the overlay's rectangle (empty without one): what
  // lies entirely inside it is not drawn, because the overlay paints over it; what it only partly covers is drawn.
  virtual void render(const Model& model, const ViewState& view, ui::Renderer& r, ui::Rect covered) const noexcept = 0;

  // Its stable identity (ids.h): the registry row that lists it carries the same one.
  virtual ScreenId id() const noexcept = 0;
  virtual Context context() const noexcept = 0;
  virtual ModeTable modes() const noexcept { return ModeTable{nullptr, 0}; }
  // The mode a screen starts in (an index into modes()): Pattern's is Browse (the pads preview until Rec arms editing).
  virtual unsigned default_mode() const noexcept { return 0; }
  // The context of the field under the cursor, Context::None for a screen with no field level.
  virtual Context field_context(const Model& model, const ViewState& view) const noexcept {
    (void)model;
    (void)view;
    return Context::None;
  }
  virtual unsigned pages() const noexcept { return 1; }
  // The screen's own button pressed again (navigate_to): by default the next page, wrapping. A screen whose F-keys already go to every page overrides it (the Instrument screen goes to its first
  // page: UI map rule 3, no cycle inside a cycle). True when something changed.
  virtual bool repress(const Model& model, ViewState& view) const noexcept {
    (void)model;
    const unsigned count = pages();
    if (count < 2) {
      return false;
    }
    std::uint8_t& page = view.page[screen_index(id())];
    page = static_cast<std::uint8_t>((page + 1u) % count);
    view.params.discard_pending();
    return true;
  }
  // A screen whose context captures: the walk of the binding chain stops at it, so no global binding (a screen button, a soft key)
  // acts while it is in view. Only the hidden INPUT TEST screen does: it is there to see every control, not to use one.
  virtual bool captures() const noexcept { return false; }
  // Whether the key row can show a Shift layer of its own while Shift is held (follows_shift): by default every screen that does not capture. An app's page captures but, when the app declares its keys (ABI 1.9,
  // OS-KEYROWDLG S5), draws the app's Shift + F1..F7 labels, so it answers by the view (ModulesView::app_keys).
  virtual bool shows_shift_layer(const ViewState& view) const noexcept {
    (void)view;
    return !captures();
  }
  // The parameter the cursor is on (core::ParamId::None for a screen without parameters): what `param.edit` changes, what MIDI Learn
  // binds a controller to (the Learn overlay's target). OS-002d.
  virtual core::ParamId focus_param(const Model& model, const ViewState& view) const noexcept {
    (void)model;
    (void)view;
    return core::ParamId::None;
  }
  // The roles of the 48 pads' LEDs while this screen is in view (OS-011a: Chop lights the pads of its slices, the selected one brighter). False: the screen
  // has no opinion and the platform's own policy stands (the pads that are held light up).
  virtual bool pad_roles(const Model& model, const ViewState& view, PadRole (&roles)[48]) const noexcept {
    (void)model;
    (void)view;
    (void)roles;
    return false;
  }
  // Eight labels, F1..F8, owned by the screen; what the key row shows and what an F-key's placeholder says.
  virtual const char* const* soft_keys() const noexcept = 0;
  // The labels the key row shows while Shift is held: what Shift + F1..F8 really do on this screen (the test in shift_labels_test.cpp resolves every key against the screen's
  // chains). A key with a Shift binding shows its label; "-" where the key does nothing: Shift + F never falls through to the plain key (OS-SHIFTROW), so the default is all "-".
  virtual const char* const* soft_keys_shifted() const noexcept { return kNoShiftKeys; }
  // The Shift row in `view`: the same for most screens; a screen whose page has Shift keys of its own (Record's RADIO page) answers by the page. The test resolves each page's keys the same way.
  virtual const char* const* shift_row(const ViewState& view) const noexcept {
    (void)view;
    return soft_keys_shifted();
  }

 protected:
  Screen() = default;
  ~Screen() = default;  // never deleted through the interface: static instances, no heap
};

// Whether the key row of `screen` can show the Shift layer in `view`: not while a dialog owns the row (its keys are the dialog's: the Navigator's own Shift layer is its prompt's, key_prompt.h) and not on a
// capturing screen (it has no Shift bindings and draws no layer; an app's page that declares its keys does, see shows_shift_layer). The App redraws on a Shift edge by it and key_row picks the row by it, so the two cannot drift apart: it asks the one predicate the row sites ask.
inline bool follows_shift(const Screen& screen, const ViewState& view) noexcept {
  return !view.overlay_keys() && screen.shows_shift_layer(view);
}

// The F-key row a screen draws: its Shift row while Shift is held and the screen follows it, else `plain`. Pattern and Song pick theirs the same way.
inline const char* const* key_row(const Screen& screen, const ViewState& view, const char* const* plain) noexcept {
  return view.shift && follows_shift(screen, view) ? screen.shift_row(view) : plain;
}

// One row of the registry. `screen` is null for a screen that is not implemented yet: the Navigator shows it disabled and
// nav.screen refuses it.
struct ScreenEntry {
  ScreenId id;
  const char* name;      // upper case, up to 11 characters: the Navigator's cell and the notifications
  const Screen* screen;  // nullptr: not implemented
  Button button;         // the screen button that reaches it
  const char* key;       // that button's name on the device, upper case, up to 11 characters
  ui::Icon icon;         // the Navigator's icon
  // Not in the Navigator and not reached by a screen button (its `button` and `key` are placeholders): a diagnostic. Hidden rows are
  // the last ones of the table (screens_valid); screen_registry() lists them, navigator_screens() does not.
  bool hidden = false;
  // In the Navigator but with no screen button of its own (OS-012b: MODULES; `button` is a placeholder, `key` says how it is reached): it has no `nav.screen`
  // binding and takes no part in the uniqueness of the buttons.
  bool buttonless = false;
};

struct ScreenTable {
  const ScreenEntry* items;
  std::size_t count;
};

// The registry: every screen of this build, in the Navigator's order, the hidden ones last.
ScreenTable screen_registry() noexcept;
// The same without the hidden screens: what the Navigator shows and chooses from.
ScreenTable navigator_screens() noexcept;

// nullptr when there is no such id. Linear: the table is a dozen rows.
const ScreenEntry* find_screen(ScreenTable table, ScreenId id) noexcept;
// The row at an index of the table (the Navigator's chosen cell), nullptr if out of range.
const ScreenEntry* screen_at(ScreenTable table, int index) noexcept;
// Index of an id in the table, -1 if it is not there.
int screen_position(ScreenTable table, ScreenId id) noexcept;

// Moves the base screen to `target`: leave on the current one, then enter on the target, in that order (view.screen
// changes in between). False, nothing done: the target is already current, unknown or not implemented. Overlays are not
// touched. `table` is a parameter so a test can hand it screens of its own.
bool switch_screen(ScreenTable table, const Model& model, ViewState& view, ScreenId target) noexcept;

// What a screen button does: another screen is switched to; the current one's own button cycles its pages. A screen that
// is not implemented posts an info notification and does nothing else. Returns true if anything visible changed.
bool navigate_to(ScreenTable table, const Model& model, ViewState& view, ScreenId target) noexcept;

constexpr bool same_text(const char* a, const char* b) noexcept {
  while (*a != '\0' && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}

// Compile-time sanity of a registry: ids valid and unique, names and keys non-empty and unique, names short enough for a
// Navigator cell, the hidden rows last (a screen button is unique among the rows that have one). (The screens are checked at run time by the tests: GCC cannot compare a pointer to an extern object.)
constexpr bool screens_valid(const ScreenEntry* items, std::size_t count) noexcept {
  for (std::size_t i = 0; i < count; ++i) {
    const ScreenEntry& e = items[i];
    if (!screen_id_valid(static_cast<std::int32_t>(e.id)) || e.name == nullptr || e.name[0] == '\0' || e.key == nullptr ||
        e.key[0] == '\0') {
      return false;
    }
    std::size_t n = 0;
    while (e.name[n] != '\0') {
      ++n;
    }
    std::size_t k = 0;
    while (e.key[k] != '\0') {
      ++k;
    }
    if (n > 11 || k > 11) {
      return false;
    }
    if (i > 0 && items[i - 1].hidden && !e.hidden) {
      return false;  // a hidden row before a visible one: navigator_screens() is a prefix of the table
    }
    for (std::size_t j = 0; j < i; ++j) {
      if (items[j].id == e.id || same_text(items[j].name, e.name) ||
          (!items[j].hidden && !e.hidden && !items[j].buttonless && !e.buttonless && items[j].button == e.button)) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace o3os::app
