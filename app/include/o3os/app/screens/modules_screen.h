// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/app/frame.h>
#include <o3os/app/screens/screen.h>

namespace o3os::app {

// F1..F8 of the MODULES screen: LAUNCH the app under the cursor, RESCAN the card.
inline constexpr const char* kModulesSoftKeys[8] = {"LAUNCH", "RESCAN", "--", "--", "--", "--", "--", "--"};
// The app page's keys when the app declares none (ABI 1.2-1.8): F8 is BACK (OS-KEYROWDLG S5), the OS's, whatever the app is. The exit chord F7 + F8 stays the way out of a hung app. An app that declares its keys
// (ABI 1.9) draws its own labels on F1..F7 and Shift + F1..F7 (ModuleAppScreen::render); its blank keys read "-", its Shift + F8 reads nothing.
inline constexpr const char* kModuleAppSoftKeys[8] = {"--", "--", "--", "--", "--", "--", "--", "BACK"};

// "MODULES" (OS-012b, ADR 0002 amendment 1; new Community OS behaviour): the app modules on the card (modules/apps/*.tmod), in the Navigator. Up / Down and the encoder choose one,
// F1 launches it (the OS loads it, gives it its state and opens its page), F2 reads the card again. A list of what this boot has quarantined (a file that failed validation, a module
// that went over a budget or crashed the device) is on the right, with the reason: the one place a user sees why a module is not there. Native modules, not apps, are bound to instruments
// on the Instrument page (F1 ENGINE).
class ModulesScreen final : public Screen {
 public:
  constexpr ModulesScreen() noexcept = default;
  void enter(const Model& model, ViewState& view) const noexcept override;
  void render(const Model& model, const ViewState& view, ui::Renderer& r, ui::Rect covered) const noexcept override;
  ScreenId id() const noexcept override { return ScreenId::Modules; }
  Context context() const noexcept override { return Context::Modules; }
  const char* const* soft_keys() const noexcept override { return kModulesSoftKeys; }
  int count(const Model& model, const ViewState& view) const noexcept override;
  int position(const ViewState& view) const noexcept override { return view.modules.apps; }
  int page() const noexcept override { return kFrameRows; }  // the rows of the pane (the list is not windowed; OS-NAV3)
  bool accelerates() const noexcept override { return true; }  // OS-AUDIT-28
  bool set_position(const Model& model, ViewState& view, int position) const noexcept override;
};
extern const ModulesScreen kModulesScreen;

// An app's page (OS-012b): the OS owns it, the module draws its body. The page is the status strip, the app's parameters as the standard controls on the left (Shift + Up / Down chooses
// one, Shift + the encoder edits it, Shift + Delete is its default) and the module's view in the rest of the pane; everything else (buttons, pads, the encoder) goes to the app. The context
// captures, so no OS binding acts but BACK (F8), the transport's Play, the exit chord F7 + F8 and the Shift layer of the parameters. An app that went over a budget is stopped and the body says so.
class ModuleAppScreen final : public Screen {
 public:
  constexpr ModuleAppScreen() noexcept = default;
  void leave(const Model& model, ViewState& view) const noexcept override;  // leaving the page, however, ends the app
  void render(const Model& model, const ViewState& view, ui::Renderer& r, ui::Rect covered) const noexcept override;
  ScreenId id() const noexcept override { return ScreenId::ModuleApp; }
  Context context() const noexcept override { return Context::ModuleApp; }
  bool captures() const noexcept override { return true; }
  bool shows_shift_layer(const ViewState& view) const noexcept override { return view.modules.app_keys; }
  const char* const* soft_keys() const noexcept override { return kModuleAppSoftKeys; }
  int count(const Model& model, const ViewState& view) const noexcept override;
  int position(const ViewState& view) const noexcept override { return view.modules.app_row; }
  bool set_position(const Model& model, ViewState& view, int position) const noexcept override;
};
extern const ModuleAppScreen kModuleAppScreen;

}  // namespace o3os::app
