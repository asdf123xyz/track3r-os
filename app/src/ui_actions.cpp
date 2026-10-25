// SPDX-License-Identifier: GPL-3.0-or-later
// Interface actions that belong to no screen: the soft keys, Learn and the colour theme.
#include "action_handlers.h"

#include <o3os/model/params.h>

namespace o3os::app {

// F1..F8: the soft keys do nothing else yet, so each posts its label as an info notification (the screen's own table:
// the Song screen's F1 is not the pattern screen's). A key whose label is "-" or "--" has no function and posts nothing.
bool ui_softkey(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (arg < 0 || arg > 7) {
    return false;
  }
  const char* label = ctx.soft_keys[arg];
  if (label == nullptr || label[0] == '\0' || (label[0] == '-' && (label[1] == '\0' || (label[1] == '-' && label[2] == '\0')))) {
    return false;  // a key with no function ("-", "--"): it says nothing (OS-CFGWIRE)
  }
  core::FixedString<core::kNotifyTextChars> text;
  const char key[4] = {'F', static_cast<char>('1' + arg), ' ', '\0'};
  text.assign(key);
  text.append(ctx.soft_keys[arg]);
  return ctx.view.notify.post(core::Severity::Info, text.c_str(), "ui", ViewState::kMessageMs);
}

// Shift + F1..F8 where the screen has no function on the key (OS-SHIFTROW): the walk of the chain ends here, so the press neither reaches the plain key nor does anything.
// The do-nothing action. A key bound to it still counts as "another key" for the File screen's questions (NEW, DELETE, PURGE, the OG PROJECT row's): they are taken back, the strip follows (file_question_sync).
bool ui_none(ActionContext& ctx, core::ActionArg) noexcept {
  ctx.view.file.ask_clear();
  return false;
}

// FX1 + FX2 together: the Learn overlay on, or off. It does not open over the Navigator (and the Navigator's context
// does not bind the chord, so it cannot be asked to).
bool learn_toggle(ActionContext& ctx, core::ActionArg) noexcept {
  ctx.view.learn = LearnView{};  // opened or closed: the list starts at the top, nothing armed
  if (ctx.view.overlay == OverlayId::Learn) {
    ctx.view.overlay = OverlayId::None;
    return true;
  }
  if (ctx.view.overlay != OverlayId::None) {
    return false;
  }
  ctx.view.overlay = OverlayId::Learn;
  return true;
}

// Shift + Master (provisional: Config does not exist yet): the next built-in theme, wrapping. Switching is at once, no
// reboot (ADR 0010 amendment 1).
bool ui_theme_next(ActionContext& ctx, core::ActionArg) noexcept {
  ctx.view.theme = ui::next_theme(ctx.view.theme);
  (void)ctx.model.device.set(model::device_setting::Theme, static_cast<float>(ctx.view.theme));  // the Config screen's THEME row says the same (OS-011a)
  return true;
}

}  // namespace o3os::app
