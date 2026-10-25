// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The registry of screens and the states they are shown in. One table feeds both the display-list budget harness
// (sim/budget.h: every worst-case state must fit its limit) and the gallery (sim/gallery.h, `o3sim --gallery`:
// every state rendered to a PNG), so a screen added here is measured and looked at without further wiring. Both
// run every state in every built-in theme (ui/theme.h): the theme comes with the Renderer a state draws into.
//
// To add a screen: write its states as functions that draw the whole frame content into a ui::Renderer that has
// been begun, and list them in `screens()` (screens.cpp). Nothing else changes.

#include <o3os/ui/renderer.h>

#include <cstddef>

namespace o3os::sim {

// ADR 0010 section 9: the display-list words (DISPLAY included) a screen may need in its worst case, of the
// chip's 2048, in every theme. The remaining 256 are the margin for a longer label, another status field or a wider
// chrome. Every worst-case state is held to it; there is no exception (the pattern view's interim 1920 ended with
// OS-003b, which moved the grid onto tile faces).
inline constexpr std::size_t kBudgetWords = 1792;

struct ScreenState {
  // Letters, digits and '_': it is part of a file name (<screen>_<state>.png) and of a golden key.
  const char* name;
  // A worst-case state is budgeted: the largest frame of its `variants` must fit `limit_words`. Other states
  // (typical, edge positions) only have to fit the chip's 2048 words.
  bool worst_case;
  // A worst case may be a sweep over sub-states (cursor positions, labels, ...): draw() is called with each
  // variant in 0..variants-1. The gallery shows the variant that needs the most words. 1 for the other states.
  unsigned variants;
  // The limit of a worst-case state (ignored for the others, which use the chip's 2048): kBudgetWords.
  std::size_t limit_words;
  // Draws the content of the frame, everything between Renderer::begin() and end(). The renderer's frame is
  // already begun with the screen's background colour.
  void (*draw)(ui::Renderer& r, unsigned variant);
  // Whether `o3sim --gallery` renders it to a PNG (and a golden). The budget harness measures every state either way:
  // false is for a sweep that is there to be measured, not looked at, since a picture costs far more than a measurement.
  bool in_gallery = true;
  // A state that looks the same in every theme (the boot screen, ui/boot_screen.h) names the one theme it is drawn with; the
  // budget harness and the gallery then draw it with that theme whichever one they are sweeping, so its three pictures are the
  // same three pictures (and the golden says so). nullptr: the theme being swept, as for every other state.
  const ui::Theme& (*fixed_theme)() noexcept = nullptr;
};

struct Screen {
  const char* name;  // letters, digits and '_'
  const ScreenState* states;
  unsigned state_count;
};

// The registry: every screen and its states, in the order of the gallery. Static storage, never empty.
const Screen* screens() noexcept;
unsigned screen_count() noexcept;

}  // namespace o3os::sim
