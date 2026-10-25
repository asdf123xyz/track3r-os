// SPDX-License-Identifier: GPL-3.0-or-later
// Pattern F7 TOOLS (OS-H3A, ADR 0014 slice H3; docs/specs/OS-H3A.md section 2): the list opens over the Pattern screen, F1 runs the item under the cursor. An app is launched with the Pattern's
// block as its launch context (ModuleUi::launch_app's `ctx`, its second caller after MODULES) and its page opens; its exit chord comes back to Pattern with the cursor and the block as they were
// (module_actions.cpp: ViewState::modules.from_tools). NEW COMMUNITY OS BEHAVIOUR.
#include "action_handlers.h"
#include "param_access.h"
#include "pattern_edit_util.h"

#include <o3os/app/module_ui.h>
#include <o3os/app/project_state.h>
#include <o3os/app/screens/screen.h>
#include <o3os/app/session.h>
#include <o3os/app/tools_list.h>

namespace o3os::app {

// F7. Refused while the view follows the play line (LIVE, or WATCH while playing): the cursor is not the user's then, and the tools start from it. It says so (OS-BUG-TOOLSPLAY, finding 13 of the random
// walk: the key is labelled and was silent). Under an overlay or on another screen the key is not this one's. Anything else that stops it says why.
bool tools_open(ActionContext& ctx, core::ActionArg) noexcept {
  ViewState& v = ctx.view;
  if (v.overlay != OverlayId::None || v.screen != ScreenId::Pattern) {
    return false;
  }
  if (pe::select_follows(ctx)) {
    return pe::say(ctx, core::Severity::Info, "STOP PLAYING TO OPEN TOOLS");
  }
  if (ctx.model.project == nullptr || ctx.session == nullptr || ctx.session->document() == nullptr) {  // the tools edit the project: with none open (the demo) there is nothing to run them on
    return pe::say(ctx, core::Severity::Info, "NO PROJECT");
  }
  if (ctx.model.modules != nullptr) {  // (the fixed entries need no module system: without one the list holds only them)
    ctx.model.modules->scan_tools();  // the card may have changed since the last look: one directory read and the 64-byte header of each app not loaded yet
  }
  v.modules.tools = 0;
  v.modules.tools_msg = "";
  v.overlay = OverlayId::Tools;
  return true;
}

// The launch context of the Pattern as it is: the pattern in view, the block's first track, rows and track count (the cursor's track and row, and a block of 0, when nothing is selected), the
// instrument in use (0-based slot).
static o3m_launch_ctx tools_context(const ViewState& v) noexcept {
  const Selection& sel = v.edit.selection;
  const Selection::Block b = sel.block(v.pattern.row(), v.pattern.track());  // (OS-AUDIT-56: an app gets whole steps, never a column range; the app's screen covers the strip, so no notice)
  o3m_launch_ctx lc{};
  lc.size = sizeof lc;
  lc.flags = O3M_LAUNCH_FROM_TOOLS;
  lc.pattern = v.edit.pattern;
  lc.track = static_cast<std::uint32_t>(b.track0 < 0 ? 0 : b.track0);
  lc.instrument = v.instrument >= 1 ? static_cast<std::uint32_t>(v.instrument - 1) : 0u;
  lc.block_start = static_cast<std::uint32_t>(b.row0 < 0 ? 0 : b.row0);
  if (sel.active) {
    lc.block_length = static_cast<std::uint32_t>(b.row1 - b.row0 + 1);
    lc.block_tracks = static_cast<std::uint8_t>(b.track1 - b.track0 + 1);
  }
  return lc;
}

// F1 in the list: a fixed entry runs its action, an app is launched. A refused launch keeps the list up and says why inside it (the strip waits behind a modal overlay): the first launch of an app that edits the project or writes files answers "F1 AGAIN".
bool tools_run(ActionContext& ctx, core::ActionArg) noexcept {
  ViewState& v = ctx.view;
  if (v.overlay != OverlayId::Tools) {
    return false;
  }
  ToolItem item;
  if (!tools_item(ctx.model, v.modules.tools, item, v.edit.selection.active)) {
    return false;  // an empty list: nothing to run
  }
  if (item.kind == ToolItem::Kind::ExportPattern || item.kind == ToolItem::Kind::ExportSelection) {  // the list closes first: the export's own refusals (no card, a save running, ...) say why on the strip, and its progress overlay opens over Pattern
    v.overlay = OverlayId::None;
    v.modules.tools_msg = "";
    return item.kind == ToolItem::Kind::ExportPattern ? export_pattern(ctx, 0) : export_selection(ctx, 0);
  }
  if (item.kind != ToolItem::Kind::App) {  // PAT-OPS-A: DUPLICATE, EXPAND, SHRINK. The list closes first (the strip waits behind a modal overlay) and a redraw is due whatever happens; Edit must be on, as for a paste; the edit is
                                             // the undoable pattern.op, one unit (it says why when it does nothing)
    v.overlay = OverlayId::None;
    v.modules.tools_msg = "";
    if (!pe::edit_enabled(ctx)) {
      (void)pe::say(ctx, core::Severity::Info, pe::kEditOff);
    } else {
      const core::ActionArg op = static_cast<core::ActionArg>(item.kind) - static_cast<core::ActionArg>(ToolItem::Kind::Duplicate);  // (Duplicate, Expand, Shrink: model::ops::Op 0, 1, 2)
      (void)dispatch(action_registry(), core::action_id("pattern.op"), op, ctx);
    }
    return true;
  }
  if (ctx.model.modules == nullptr) {
    return false;  // (an App item exists only with a module system)
  }
  ModuleUi& mu = *ctx.model.modules;
  const o3m_launch_ctx lc = tools_context(v);
  const ModuleUi::LaunchResult res = mu.launch_app(item.app_entry, ctx.model.project, &lc);
  if (res != ModuleUi::LaunchResult::Ok) {
    v.modules.tools_msg = mu.launch_message(res);  // (OS-H4: the confirm names what the app does)
    return true;
  }
  v.overlay = OverlayId::None;
  v.modules.tools_msg = "";
  if (ctx.model.project != nullptr) {
    (void)app_params_apply(mu, *ctx.model.project);  // OS-004d: the values this project remembers for the app
  }
  v.modules.app_row = 0;
  v.modules.from_tools = true;
  (void)switch_screen(screen_registry(), ctx.model, ctx.view, ScreenId::ModuleApp);
  return true;
}

// The edit group of a running app, applied as ONE undo unit (ModuleUi::apply_group, run by the sink App::attach_modules gave the module system): dispatch() opened the unit and named it with this
// action and the app's catalog entry, and the unit ends with it. Without a group closed by edit_end (a macro replaying this action, a stray call) it does nothing.
// OS-H3B: the steps and the tempo are written by apply_group (which also checks the parameter writes, so a bad group writes nothing); the parameter writes then go through write_param, the Params page's own
// path, so the engine hears them and the Editor journals each one inside this unit.
bool module_app_group(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.edit == nullptr || ctx.model.modules == nullptr) {
    return false;
  }
  ModuleUi& mu = *ctx.model.modules;
  bool changed = mu.apply_group(*ctx.edit);
  if (mu.apply_result() != O3M_EDIT_OK) {
    return false;
  }
  unsigned op = 0, at = 0;
  core::ParamId id = core::ParamId::None;
  float value = 0.0f;
  while (mu.next_param_op(op, at, id, value)) {
    changed = write_param(ctx, id, value) == WriteResult::Changed || changed;
  }
  // OS-APPUX S5: the Pattern opens on what the tool wrote: the cursor on the first written step and the written block selected (the view state's, no ABI). The unit's own redo and undo leave the cursor alone.
  const ModuleUi::GroupExtent& e = mu.group_extent();
  if (e.valid) {
    ViewState& v = ctx.view;
    v.edit.pattern = e.pattern;
    const PatternView::Dims d = pattern_dims(ctx.model, v);
    (void)v.pattern.scroll_rows(static_cast<int>(e.row0) - v.pattern.row(), d);
    (void)v.pattern.move_track(static_cast<int>(e.track0) - v.pattern.track(), d);
    Selection& sel = v.edit.selection;
    sel.start_row = static_cast<std::int16_t>(e.row0);
    sel.end_row = static_cast<std::int16_t>(e.row1);
    sel.start_track = static_cast<std::int8_t>(e.track0);
    sel.end_track = static_cast<std::int8_t>(e.track1);
    sel.active = true;
    sel.extending = false;
    sel.column = false;
  }
  return changed;
}

}  // namespace o3os::app
