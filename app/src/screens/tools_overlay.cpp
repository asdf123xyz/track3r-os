// SPDX-License-Identifier: GPL-3.0-or-later
// The TOOLS list of Pattern's F7 (OS-H3A): the OS's own operations (EXPORT PATTERN, and EXPORT SELECTION while there is a block) above a divider, then the tool apps to run on the Pattern's block. Same shape as the engine picker.
#include <o3os/app/key_prompt.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/app/text_format.h>
#include <o3os/app/tools_list.h>
#include <o3os/core/fixed_string.h>

#include "dialog.h"

namespace o3os::app {

using ui::Color;
using ui::Face;
using ui::Rect;

namespace {

constexpr int kToolsW = 520;

Dialog tools_dialog(const ui::Theme& theme) noexcept {
  return make_dialog(theme, kToolsW, (kToolsRows + 2) * theme.metrics.row_pitch);  // a line of help, the rows, the keys
}

}  // namespace

Rect ToolsOverlay::rect(const ui::Theme& theme) const noexcept { return tools_dialog(theme).outer; }

int ToolsOverlay::count(const Model& model, const ViewState& view) const noexcept { return static_cast<int>(tools_count(model, view.edit.selection.active)); }

bool ToolsOverlay::set_position(const Model& model, ViewState& view, int position) const noexcept {
  const int last = count(model, view) - 1;
  const int to = position < 0 ? 0 : (position > last ? (last < 0 ? 0 : last) : position);
  const bool changed = to != view.modules.tools;
  view.modules.tools = static_cast<std::uint8_t>(to);
  return changed;
}

void ToolsOverlay::render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const Dialog d = tools_dialog(t);
  draw_dialog(r, d, "TOOLS");
  const int x = d.body.x + 2 * m.pad;
  const int right = d.body.x + d.body.w - 2 * m.pad;
  const int pitch = m.row_pitch;
  const int cell = t.geometry(Face::Data).cell_w;
  const bool refused = view.modules.tools_msg != nullptr && view.modules.tools_msg[0] != '\0';
  r.text(x, ui::caps_y(t, Face::Label, d.body.y, pitch), refused ? view.modules.tools_msg : "Run a tool on the block (or the cursor).", refused ? Color::ChromeText : Color::ChromeDim, Face::Label);
  const bool block = view.edit.selection.active;  // EXPORT SELECTION is listed while there is a block
  const int total = static_cast<int>(tools_count(model, block));
  const int cursor = view.modules.tools < total ? view.modules.tools : 0;
  const int first = cursor < kToolsRows ? 0 : cursor - kToolsRows + 1;
  if (tools_apps(model) == 0u) {  // (then the list is the fixed entries and `first` is 0)  // no app: one dim line under the fixed entries, with the divider above it
    const int y = d.body.y + (1 + static_cast<int>(tools_fixed(block))) * pitch;
    r.panel(Rect{x, y, right - x, 1}, Color::ChromeDim);
    r.text(x + 2 * cell, ui::caps_y(t, Face::Label, y, pitch), model.modules == nullptr ? "NO MODULE SYSTEM" : "NO TOOL APPS ON THE CARD", Color::ChromeDim, Face::Label);
  }
  for (int k = 0; k < kToolsRows && first + k < total; ++k) {
    ToolItem item;
    if (!tools_item(model, static_cast<unsigned>(first + k), item, block)) {
      continue;
    }
    const int y = d.body.y + (1 + k) * pitch;
    if (item.divider_above) {
      r.panel(Rect{x, y, right - x, 1}, Color::ChromeDim);  // the rule between the OS's entries and the apps, on the top edge of the first app's row
    }
    const Color ink = item.bad ? Color::ChromeDim : Color::ChromeText;
    if (first + k == cursor) {
      r.text(x, ui::caps_y(t, Face::Data, y, pitch), ">", Color::ChromeText, Face::Data);
    }
    r.text(x + 2 * cell, ui::caps_y(t, Face::Data, y, pitch), item.label, ink, Face::Data);
    if (item.bad) {
      r.text(right - r.text_width("QUARANTINED", Face::Label), ui::caps_y(t, Face::Label, y, pitch), "QUARANTINED", ink, Face::Label);
    }
  }
  const int foot = d.body.y + (kToolsRows + 1) * pitch;
  (void)draw_prompt_hints(r, x, foot, pitch, prompt_in_force(model, view), 2);
}

constexpr ToolsOverlay kToolsOverlay{};

}  // namespace o3os::app
