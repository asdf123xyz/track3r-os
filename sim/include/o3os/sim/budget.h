// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The display-list budget harness (ADR 0010 section 9, OS-003a, per theme and per translate mode since OS-003b). It
// renders each screen state of the registry (sim/screens.h) in each built-in theme through the real renderer into a
// builder, in both ways the renderer can reach x >= 512 (VERTEX_TRANSLATE_X, the default, and the fallback that needs
// nothing of it: BITMAP_HANDLE + CELL + VERTEX2F, ui::Renderer::use_translate(false)), counts the words of the frame
// (DISPLAY included) and compares the largest of a state's variants with its limit: kBudgetWords for a worst case, the
// chip's 2048 for the others. The fallback is the plan B for a chip that does not translate VERTEX2II: it must fit too. A screen is measured in a list of 8192 words, so one that does not fit the
// chip reports its real size, not just "overflow". It needs no rasteriser: it is cheap enough for ctest.
//
//   const auto rows = sim::measure_all();
//   CHECK(sim::all_within(rows));  std::fputs(sim::format_table(rows).c_str(), stdout);

#include <o3os/sim/screens.h>
#include <o3os/ui/theme.h>

#include <cstddef>
#include <string>
#include <vector>

namespace o3os::sim {

struct BudgetRow {
  const char* screen = "";
  const char* state = "";
  const char* theme = "";      // the theme it was drawn in (ui::Theme::name)
  bool translate = true;       // the renderer's mode: VERTEX_TRANSLATE_X (true) or the VERTEX2F fallback (false)
  unsigned variants = 1;       // sub-states measured
  std::size_t words = 0;       // the largest frame of any variant, DISPLAY included
  unsigned worst_variant = 0;  // the variant that needs them (the first one, on a tie)
  bool budgeted = false;       // a worst-case state, held to `limit`
  std::size_t limit = 0;       // words allowed: the state's limit if budgeted, else the chip's 2048
  bool complete = true;        // every variant produced a frame (false: the builder or the sink refused one)

  bool within() const { return complete && words <= limit; }
};

// Words of one variant of `state` in `theme`, DISPLAY included; 0 and *complete = false if no frame came out.
std::size_t measure_variant(const ScreenState& state, unsigned variant, const ui::Theme& theme, bool translate = true,
                            bool* complete = nullptr);

BudgetRow measure(const Screen& screen, const ScreenState& state, const ui::Theme& theme, bool translate = true);
// Every state of every screen of the registry in every built-in theme and both modes, in registry order, themes next
// to innermost and the two modes (translate first) innermost. `screen_prefix`: only the screens whose registry name starts with it.
std::vector<BudgetRow> measure_all(const std::string& screen_prefix = std::string());

bool all_within(const std::vector<BudgetRow>& rows);

// The largest `words` of the budgeted (worst-case) rows drawn in `theme` in the given mode; 0 if there is none.
std::size_t worst_words(const std::vector<BudgetRow>& rows, const char* theme, bool translate = true);

// The table, one row per state, theme and mode, and a line with the worst case of each theme in each mode:
//   screen   state       theme    mode    variants  words   limit  verdict
std::string format_table(const std::vector<BudgetRow>& rows);

}  // namespace o3os::sim
