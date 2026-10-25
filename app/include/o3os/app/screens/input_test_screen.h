// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/app/screens/screen.h>

namespace o3os::app {

// F1..F8 of the INPUT TEST screen (OS-KEYROWDLG): its context binds two chords and nothing else, so F2 + F3 read CLEAR (the counts go) and F4 + F5 read EXIT (back to Pattern), the same word on both keys of a
// chord as an app page does for F7 + F8; the other keys are blank (the screen sees them raw and lights their cell, nothing else happens). It is a screen, not an overlay: the row is drawn by its own render.
inline constexpr const char* kInputTestSoftKeys[8] = {"", "CLEAR", "CLEAR", "EXIT", "EXIT", "", "", ""};

// The hidden diagnostic of the hardware session (OS-007d), in the spirit of the hidden diagnostic screens that the 1.7.2 release notes mention (PAR-132: documented in the release notes, not verified): all 33 buttons, all 48 pads and
// the encoder live, from the raw events (InputTestState, input_test.h). A control that is down is the selection blue, one that has been
// pressed since the last clear shows its press count (a bounce reads as 2 for one touch), the encoder shows its detents, and the
// strips show the input path's health (the key scanners answering, errors, refused events) and the scan-to-app age of the last event.
//
// Hidden: not in the Navigator, no screen button. It is opened by the platform from a boot chord (Shift + F1 + F8 held at power-up,
// platform/mk66 app_main.cpp) and left with F4 + F5 together. Its context captures: no control acts while it is up.
// Everything is drawn from theme tokens and the pairs of ui/theme.h's contrast table (Text and TextDim on Pane, SelText on Sel,
// ChromeText on Chrome).
class InputTestScreen final : public Screen {
 public:
  constexpr InputTestScreen() noexcept = default;
  void enter(const Model& model, ViewState& view) const noexcept override;
  void render(const Model& model, const ViewState& view, ui::Renderer& r, ui::Rect covered) const noexcept override;
  ScreenId id() const noexcept override { return ScreenId::InputTest; }
  Context context() const noexcept override { return Context::InputTest; }
  bool captures() const noexcept override { return true; }
  const char* const* soft_keys() const noexcept override { return kInputTestSoftKeys; }
};
extern const InputTestScreen kInputTestScreen;

// The short name a button's cell shows (at most five characters), by button code: "F1", "SHIFT", "RIGHT"; "?" for a code that is no button.
const char* input_test_button_name(unsigned code) noexcept;

}  // namespace o3os::app
