// SPDX-License-Identifier: GPL-3.0-or-later
// Navigation actions: moving the cursor of whatever owns the input, and jumping around it (OS-002b). They work through the
// Cursor interface (screens/cursor.h) of the overlay that is up, else of the base screen, and name no screen: a screen
// added later is navigable by implementing the interface, and nothing here changes.
#include "action_handlers.h"

#include <o3os/app/screens/overlay.h>
#include <o3os/app/screens/screen.h>

namespace o3os::app {

namespace {

constexpr int kMaxStep = 1 << 16;  // a move larger than this is clamped before it is added (an encoder fed from data)
constexpr int clamp_int(int v, int lo, int hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

// What owns the input: the modal overlay that is up (Learn has no cursor: its defaults move nothing), else the screen.
const Cursor* focused(const ViewState& view) noexcept {
  if (view.overlay != OverlayId::None) {
    return overlay_of(view.overlay);
  }
  const ScreenEntry* e = find_screen(screen_registry(), view.screen);
  return e != nullptr ? e->screen : nullptr;
}

bool move_by(ActionContext& ctx, const Cursor& c, int delta) noexcept {
  return c.set_position(ctx.model, ctx.view, c.position(ctx.view) + clamp_int(delta, -kMaxStep, kMaxStep));
}

bool step(ActionContext& ctx, int dx, int dy) noexcept {
  const Cursor* c = focused(ctx.view);
  if (c == nullptr) {
    return false;
  }
  // A fresh press may wrap at the end of a list that does so (OS-ROWWRAP: the Pattern's rows); an auto-repeat never does.
  return ctx.repeat ? c->step(ctx.model, ctx.view, dx, dy) : c->step_press(ctx.model, ctx.view, dx, dy);
}

}  // namespace

bool nav_left(ActionContext& ctx, core::ActionArg) noexcept { return step(ctx, -1, 0); }
bool nav_right(ActionContext& ctx, core::ActionArg) noexcept { return step(ctx, 1, 0); }
bool nav_up(ActionContext& ctx, core::ActionArg) noexcept { return step(ctx, 0, -1); }
bool nav_down(ActionContext& ctx, core::ActionArg) noexcept { return step(ctx, 0, 1); }

bool nav_scroll(ActionContext& ctx, core::ActionArg arg) noexcept {
  const Cursor* c = focused(ctx.view);
  if (c == nullptr) {
    return false;
  }
  // OS-AUDIT-28: only a list accelerates (Cursor::accelerates); a pattern's rows, a value and the Shift coarse step (cursor.page) keep a detent a row.
  return move_by(ctx, *c, c->accelerates() ? ctx.view.enc_accel.scale(ctx.now, arg) : arg);
}

// n / 4 of the list: 0, 1/4, 2/4, 3/4 (the end is its own action). A pattern of 64 steps jumps to 0, 16, 32, 48.
bool cursor_jump_fraction(ActionContext& ctx, core::ActionArg arg) noexcept {
  const Cursor* c = focused(ctx.view);
  return c != nullptr && arg >= 0 && arg <= 3 &&
         c->set_position(ctx.model, ctx.view, arg * c->count(ctx.model, ctx.view) / 4);
}

bool cursor_jump_end(ActionContext& ctx, core::ActionArg) noexcept {
  const Cursor* c = focused(ctx.view);
  return c != nullptr && c->count(ctx.model, ctx.view) > 0 && c->set_position(ctx.model, ctx.view, c->count(ctx.model, ctx.view) - 1);
}

bool cursor_page(ActionContext& ctx, core::ActionArg arg) noexcept {
  const Cursor* c = focused(ctx.view);
  return c != nullptr && move_by(ctx, *c, clamp_int(arg, -kMaxStep, kMaxStep) * c->page());
}

bool cursor_track(ActionContext& ctx, core::ActionArg arg) noexcept {
  const Cursor* c = focused(ctx.view);
  return c != nullptr && c->track(ctx.model, ctx.view, arg);
}

bool cursor_home(ActionContext& ctx, core::ActionArg) noexcept {
  const Cursor* c = focused(ctx.view);
  return c != nullptr && c->home(ctx.model, ctx.view);
}

bool cursor_end(ActionContext& ctx, core::ActionArg) noexcept {
  const Cursor* c = focused(ctx.view);
  return c != nullptr && c->end(ctx.model, ctx.view);
}

}  // namespace o3os::app
