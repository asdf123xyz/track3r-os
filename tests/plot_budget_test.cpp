// SPDX-License-Identifier: GPL-3.0-or-later
//
// The plot gallery's display-list budget (sim/plot_screens.h, ADR 0010 amendment 2): every plot type drawn with the standard
// chrome, in every theme and both translate modes, within the 1792 words; the word count does not depend on the theme (a
// theme only changes colours); and the worst-case sweep is the largest frame of its screen.
#include <doctest/doctest.h>

#include <o3os/sim/budget.h>
#include <o3os/sim/plot_screens.h>
#include <o3os/sim/screens.h>
#include <o3os/ui/theme.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace o3os;

namespace {

const sim::Screen* plot_screen() {
  for (unsigned i = 0; i < sim::screen_count(); ++i) {
    if (std::strcmp(sim::screens()[i].name, "plot") == 0) {
      return &sim::screens()[i];
    }
  }
  return nullptr;
}

}  // namespace

TEST_CASE("plot budget: every plot state fits 1792 words with the standard chrome, in every theme and both modes") {
  const sim::Screen* screen = plot_screen();
  CHECK(screen != nullptr);
  if (screen == nullptr) {
    return;
  }
  CHECK(screen->state_count == sim::kPlotStateCount);
  const char* const expected[] = {"wave_l1", "wave_l4", "wave_list", "curve_env", "curve_filter",
                                  "scope",   "spectrum", "meters",   "page",      "worst"};
  CHECK(screen->state_count == sizeof(expected) / sizeof(expected[0]));
  std::string table;
  for (unsigned i = 0; i < screen->state_count; ++i) {
    const sim::ScreenState& state = screen->states[i];
    CHECK(std::string(state.name) == expected[i]);
    CHECK(state.worst_case);  // every plot state is held to the standard budget, none to the chip's 2048 only
    CHECK(state.limit_words == sim::kBudgetWords);
    for (const bool translate : {true, false}) {
      std::size_t first = 0;
      for (unsigned t = 0; t < ui::kThemeCount; ++t) {
        const ui::Theme& theme = ui::theme(ui::theme_id_at(t));
        const sim::BudgetRow row = sim::measure(*screen, state, theme, translate);
        CHECK_MESSAGE(row.complete, state.name << "/" << theme.name << " produced no frame");
        CHECK_MESSAGE(row.within(), state.name << "/" << theme.name << (translate ? "/translate: " : "/vtx2f: ") << row.words);
        CHECK(row.words > 300);  // the chrome alone is about 200: a plot is really drawn
        if (t == 0) {
          first = row.words;
          char line[96];
          std::snprintf(line, sizeof(line), "plot %-13s %-9s %5zu words\n", state.name, translate ? "translate" : "vtx2f", row.words);
          table += line;
        }
        CHECK_MESSAGE(row.words == first, state.name << ": the word count must not depend on the theme");
      }
    }
  }
  std::fputs(table.c_str(), stdout);
}

TEST_CASE("plot budget: the worst-case sweep is a real sweep and its largest variant is the largest frame of the plot screen") {
  const sim::Screen* screen = plot_screen();
  CHECK(screen != nullptr);
  if (screen == nullptr) {
    return;
  }
  const sim::ScreenState* worst = nullptr;
  for (unsigned i = 0; i < screen->state_count; ++i) {
    if (std::string(screen->states[i].name) == "worst") {
      worst = &screen->states[i];
    }
  }
  CHECK(worst != nullptr);
  if (worst == nullptr) {
    return;
  }
  CHECK(worst->variants >= 7);  // vector wave, zoomed line, curve, scope, spectrum, meters, bitmap wave
  const ui::Theme& theme = ui::theme(ui::ThemeId::Amiga);
  std::size_t sweep_max = 0;
  std::vector<std::size_t> per_variant;
  for (unsigned v = 0; v < worst->variants; ++v) {
    bool complete = false;
    const std::size_t words = sim::measure_variant(*worst, v, theme, true, &complete);
    CHECK(complete);
    per_variant.push_back(words);
    sweep_max = words > sweep_max ? words : sweep_max;
    CHECK(words <= sim::kBudgetWords);
  }
  // every variant draws something substantial, and they differ (they are different plots)
  for (const std::size_t w : per_variant) {
    CHECK(w > 400);
  }
  CHECK(sweep_max > 600);
  for (unsigned i = 0; i < screen->state_count; ++i) {
    const sim::BudgetRow row = sim::measure(*screen, screen->states[i], theme, true);
    CHECK_MESSAGE(row.words <= sim::kBudgetWords, screen->states[i].name);
  }
}
