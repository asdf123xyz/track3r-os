// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The items of the Pattern screen's F7 TOOLS list (OS-H3A, ADR 0014 slice H3; docs/specs/OS-H3A.md section 2). One place that the list's overlay (screens/tools_overlay.cpp), the action that runs an
// item (tools_actions.cpp) and the tests ask, so that a new kind of item is one case here and one in tools_run.
//
//   fixed entries   ABOVE a divider: operations of the OS itself, listed with or without a module system or any tool app. EXPORT PATTERN (OS-WAVEXP slice A, always), DUPLICATE x2, EXPAND x2 and SHRINK /2
//                   (PAT-OPS-A, always) and, while a block is active, EXPORT SELECTION (slice B): the count depends on the block (`tools_fixed`), so every function below takes `block`
//                   (ViewState::edit.selection.active); a caller that has none means "no block". A new kind is a Kind and a row of kToolsEntries below, and its case in tools_run (tools_actions.cpp).
//   tool apps       the installed apps whose file asks for edit_project (ModuleUi::scan_tools), sorted by name. They launch with the Pattern's block as their context (o3m_launch_ctx).
//
// Main loop only. No allocation.

#include <o3os/app/model.h>
#include <o3os/app/module_ui.h>

#include <cstdint>

namespace o3os::app {

// How many fixed entries the list always has above the divider, and how many more while a block is active (EXPORT SELECTION).
inline constexpr unsigned kToolsFixed = 4;
inline constexpr unsigned kToolsBlock = 1;
inline constexpr unsigned tools_fixed(bool block) noexcept { return kToolsFixed + (block ? kToolsBlock : 0u); }

struct ToolItem {
  enum class Kind : std::uint8_t { App, ExportPattern, ExportSelection, Duplicate, Expand, Shrink };  // (Duplicate, Expand and Shrink: model::ops::Op 0, 1, 2)
  Kind kind = Kind::App;
  unsigned app_entry = 0;     // App: the index launch_app takes
  const char* label = "";     // the text shown (an app: its file stem)
  bool bad = false;           // shown dim: the app is quarantined (it cannot run this boot)
  bool divider_above = false; // the first item below the fixed ones when there are any
};

// The fixed entries in list order: the always-listed ones, then the one a block adds (OS-WAVEXP slice B, PAT-OPS-A: the labels are D1 of the spec).
struct ToolEntry {
  ToolItem::Kind kind;
  const char* label;
};
inline constexpr ToolEntry kToolsEntries[kToolsFixed + kToolsBlock] = {
    {ToolItem::Kind::ExportPattern, "EXPORT PATTERN"}, {ToolItem::Kind::Duplicate, "DUPLICATE x2"}, {ToolItem::Kind::Expand, "EXPAND x2"}, {ToolItem::Kind::Shrink, "SHRINK /2"},
    {ToolItem::Kind::ExportSelection, "EXPORT SELECTION"}};

// The tool apps in the list (none without a module system).
inline unsigned tools_apps(const Model& model) noexcept { return model.modules != nullptr ? model.modules->tool_count() : 0u; }

// Items in the list: the fixed ones, then the tool apps (the last scan_tools()).
inline unsigned tools_count(const Model& model, bool block = false) noexcept { return tools_fixed(block) + tools_apps(model); }

// Item `i` (false: out of range).
inline bool tools_item(const Model& model, unsigned i, ToolItem& out, bool block = false) noexcept {
  if (i >= tools_count(model, block)) {
    return false;
  }
  const unsigned fixed = tools_fixed(block);
  if (i < fixed) {  // (no module system needed)
    out = ToolItem{};
    out.kind = kToolsEntries[i].kind;
    out.label = kToolsEntries[i].label;
    return true;
  }
  const unsigned entry = model.modules->tool_entry(i - fixed);
  const CatalogEntry* e = model.modules->app_entry_at(entry);
  if (e == nullptr) {
    return false;
  }
  out.kind = ToolItem::Kind::App;
  out.app_entry = entry;
  out.label = e->stem;
  out.bad = e->state == CatalogEntry::State::Quarantined;
  out.divider_above = i == fixed;  // (the fixed entries are never none)
  return true;
}

}  // namespace o3os::app
