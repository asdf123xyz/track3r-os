// SPDX-License-Identifier: GPL-3.0-or-later
// OS-HELP (docs/specs/OS-HELP.md): Shift + Config opens the help page of the screen in view. New Community OS behaviour; nothing is written to the card. Closing is ui.back, scrolling is nav.up / nav.down /
// nav.scroll and cursor.page through the Cursor of HelpOverlay (screens/help_overlay.cpp), so this is the one action.
#include "action_handlers.h"

#include <o3os/app/help_text.h>

namespace o3os::app {

// Global (and the app page's context, which captures): the page of the base screen, at the scroll it was left at (ViewState::help keeps one for each page). Not reached while another modal overlay is up
// (its context captures the chord); the focus flag keeps a direct call from opening it over one.
bool help_open(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.view.overlay != OverlayId::None) {
    return false;
  }
  ctx.view.help.page = static_cast<std::uint8_t>(help_page_of(ctx.view.screen, ctx.view.dj_page()));
  ctx.view.overlay = OverlayId::Help;
  return true;
}

}  // namespace o3os::app
