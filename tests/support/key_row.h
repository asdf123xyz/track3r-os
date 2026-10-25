// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-KEYROWDLG: what the tests read of an F-key row, and what a key does. The row is the screen's own labels with the prompt in force patched over them (the part of the picture that is not pixels);
// resolve_key() walks the same context chain the App does, so a label and a binding are compared as the unit meets them.

#include <o3os/app/app.h>
#include <o3os/app/bindings.h>
#include <o3os/app/buttons.h>
#include <o3os/app/key_prompt.h>
#include <o3os/app/screens/screen.h>
#include <o3os/core/binding.h>

#include <cstdint>
#include <string>

namespace o3os::test {

inline constexpr app::Button kFKeys[8] = {app::Button::F1, app::Button::F2, app::Button::F3, app::Button::F4, app::Button::F5, app::Button::F6, app::Button::F7, app::Button::F8};

struct RowRead {
  std::string label[8];
  unsigned hot = 0;      // bit n = F(n+1) is drawn inverted
  bool marked = false;   // a prompt changed the screen's row
};

// The row the screen in view draws in `shifted` or not. `plain` stands in for the screen's own row (File swaps some labels by page inside render; the keys a question marks are the same).
inline RowRead read_row(const app::Model& model, const app::ViewState& view, const app::Screen& screen, bool shifted = false) {
  const char* const* base = shifted ? screen.shift_row(view) : screen.soft_keys();
  app::KeyRow row{};
  for (unsigned i = 0; i < 8; ++i) {
    row.label[i] = base[i];
  }
  RowRead out;
  out.marked = app::patch_row(app::prompt_in_force(model, view), shifted, row);
  for (unsigned i = 0; i < 8; ++i) {
    out.label[i] = row.label[i] != nullptr ? row.label[i] : "";
  }
  out.hot = row.hot;
  return out;
}

// The row of the dialog that owns it (a modal overlay is up): `marked` says a dialog owns one.
inline RowRead read_owner_row(const app::Model& model, const app::ViewState& view, bool shifted = false) {
  app::KeyRow row{};
  RowRead out;
  out.marked = app::owner_row(app::prompt_in_force(model, view), shifted, row);
  for (unsigned i = 0; i < 8; ++i) {
    out.label[i] = row.label[i] != nullptr ? row.label[i] : "";
  }
  out.hot = row.hot;
  return out;
}

inline RowRead read_row(const app::App& application, bool shifted = false) {
  const app::ScreenEntry* entry = app::find_screen(app::screen_registry(), application.state().screen);
  return read_row(application.model(), application.state(), *entry->screen, shifted);
}

// What the key (0..7 = F1..F8, 8..15 = Shift + F1..F8) resolves to on `screen` in `view`: the chain of the field level, the screen and Global, as shift_labels_test builds it.
struct KeyBinding {
  bool bound = false;
  core::ActionId action = core::ActionId::None;
};
inline KeyBinding resolve_key(const app::Model& model, const app::ViewState& view, const app::Screen& screen, std::uint8_t key) {
  const core::BindingSet set{{core::BindingView{}, core::BindingView{}, app::builtin_bindings()}};
  core::ContextChain chain;
  const app::Context field = screen.field_context(model, view);
  if (field != app::Context::None) {
    (void)chain.push(app::context_id(field));
  }
  (void)chain.push(app::context_id(screen.context()), screen.captures());
  (void)chain.push(app::context_id(app::Context::Global));
  const core::InputRef shift = key >= 8u ? app::button_ref(app::Button::Shift) : core::kNoInput;
  const core::Gesture tap{0, core::GestureKind::Tap, app::button_ref(kFKeys[key & 7u]), shift, 0};
  const core::Resolution r = core::resolve(set, chain, tap);
  KeyBinding out;
  out.bound = r.status == core::Resolution::Status::Bound && r.action != core::action_id("ui.none");
  out.action = r.action;
  return out;
}

}  // namespace o3os::test
