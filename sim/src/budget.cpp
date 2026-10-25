// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/sim/budget.h>

#include <o3os/eve/display_list_builder.h>
#include <o3os/eve/dl.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/ui/renderer.h>
#include <o3os/ui/style.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace o3os::sim {

namespace {

// Measuring happens in a list four times the chip's, so an oversize screen reports how big it is.
constexpr std::size_t kMeasureWords = 4 * eve::kMaxDisplayListWords;

// The measuring list, with its alignment stated like every display-list buffer (eve::DisplayListBuffer). Static: the
// harness is not re-entrant, which is fine for a test and a command-line tool.
struct alignas(8) MeasureBuffer {
  std::uint32_t words[kMeasureWords];
};
static_assert(alignof(MeasureBuffer) >= 8, "an explicitly aligned display-list buffer");

MeasureBuffer& measure_buffer() {
  static MeasureBuffer buffer;
  return buffer;
}

}  // namespace

std::size_t measure_variant(const ScreenState& state, unsigned variant, const ui::Theme& theme, bool translate,
                            bool* complete) {
  platform::host::FakeDisplay sink(kMeasureWords);
  eve::DisplayListBuilder builder(sink, measure_buffer().words, kMeasureWords);
  ui::Renderer renderer(builder, state.fixed_theme != nullptr ? state.fixed_theme() : theme);
  renderer.use_translate(translate);
  bool ok = renderer.begin(ui::Color::Pane);
  if (ok) {
    state.draw(renderer, variant);
    ok = renderer.end();
  }
  if (complete != nullptr) {
    *complete = ok;
  }
  return ok ? sink.last_frame().size() : 0;
}

BudgetRow measure(const Screen& screen, const ScreenState& state, const ui::Theme& theme, bool translate) {
  BudgetRow row;
  row.screen = screen.name;
  row.state = state.name;
  row.theme = theme.name;
  row.translate = translate;
  row.variants = state.variants;
  row.budgeted = state.worst_case;
  row.limit = state.worst_case ? state.limit_words : eve::kMaxDisplayListWords;
  for (unsigned v = 0; v < state.variants; ++v) {
    bool complete = false;
    const std::size_t words = measure_variant(state, v, theme, translate, &complete);
    row.complete = row.complete && complete;
    if (words > row.words) {
      row.words = words;
      row.worst_variant = v;
    }
  }
  return row;
}

std::vector<BudgetRow> measure_all(const std::string& screen_prefix) {
  std::vector<BudgetRow> rows;
  for (unsigned s = 0; s < screen_count(); ++s) {
    const Screen& screen = screens()[s];
    if (std::strncmp(screen.name, screen_prefix.c_str(), screen_prefix.size()) != 0) {
      continue;
    }
    for (unsigned i = 0; i < screen.state_count; ++i) {
      for (unsigned t = 0; t < ui::kThemeCount; ++t) {
        for (const bool translate : {true, false}) {
          rows.push_back(measure(screen, screen.states[i], ui::theme(ui::theme_id_at(t)), translate));
        }
      }
    }
  }
  return rows;
}

bool all_within(const std::vector<BudgetRow>& rows) {
  for (const BudgetRow& row : rows) {
    if (!row.within()) {
      return false;
    }
  }
  return true;
}

std::size_t worst_words(const std::vector<BudgetRow>& rows, const char* theme, bool translate) {
  std::size_t worst = 0;
  for (const BudgetRow& row : rows) {
    if (row.budgeted && row.translate == translate && std::strcmp(row.theme, theme) == 0 && row.words > worst) {
      worst = row.words;
    }
  }
  return worst;
}

std::string format_table(const std::vector<BudgetRow>& rows) {
  std::string out = "display-list budget (words, DISPLAY included; chip 2048, worst-case budget " +
                    std::to_string(kBudgetWords) + ", every theme, translate = VERTEX_TRANSLATE_X, vtx2f = the fallback)\n";
  char line[200];
  std::snprintf(line, sizeof(line), "%-10s %-12s %-7s %-9s %8s %7s %7s  %s\n", "screen", "state", "theme", "mode",
                "variants", "words", "limit", "verdict");
  out += line;
  for (const BudgetRow& row : rows) {
    const char* verdict = !row.complete ? "FAIL: no frame" : (row.within() ? "ok" : "OVER");
    char extra[96] = "";
    if (!row.within() && row.complete) {
      std::snprintf(extra, sizeof(extra), " (%zu words over)", row.words - row.limit);
    }
    std::snprintf(line, sizeof(line), "%-10s %-12s %-7s %-9s %8u %7zu %7s  %s%s", row.screen, row.state, row.theme,
                  row.translate ? "translate" : "vtx2f", row.variants, row.words,
                  row.budgeted ? std::to_string(row.limit).c_str() : "-", verdict, extra);
    out += line;
    if (row.variants > 1) {
      std::snprintf(line, sizeof(line), "  [largest: variant %u]", row.worst_variant);
      out += line;
    }
    out += '\n';
  }
  for (const bool translate : {true, false}) {
    out += translate ? "worst case per theme, translate:" : "worst case per theme, vtx2f fallback:";
    for (unsigned t = 0; t < ui::kThemeCount; ++t) {
      const char* name = ui::theme(ui::theme_id_at(t)).name;
      std::snprintf(line, sizeof(line), " %s %zu", name, worst_words(rows, name, translate));
      out += line;
    }
    out += '\n';
  }
  return out;
}

}  // namespace o3os::sim
