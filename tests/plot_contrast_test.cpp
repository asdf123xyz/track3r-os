// SPDX-License-Identifier: GPL-3.0-or-later
//
// The plot gallery and the contrast table (ui/theme.h, docs/ui/style-guide.md section 4): the plots add no colour token, but
// they put tokens next to each other in new ways (a wave over the pane, a marker rule over a tint, a meter body in its slot).
// Like the pattern view's, the pairs are derived from pictures: every two tokens that touch, as whole pixels, in a rendered
// plot state (the Amiga theme, whose sixteen colours are all different, so a pixel names its token) must be a pair of the
// contrast table (which the theme tests hold to its role's ratio in every theme) or a decoration named below. Pixels that are
// a blend (a translucent fill, an antialiased edge) are not tokens and are not paired: their ink is the token drawn and its
// background the one beneath, which the table covers.
#include <doctest/doctest.h>

#include <o3os/eve/display_list_builder.h>
#include <o3os/sim/eve_raster.h>
#include <o3os/sim/plot_screens.h>
#include <o3os/sim/raster_display.h>
#include <o3os/sim/screens.h>
#include <o3os/ui/renderer.h>
#include <o3os/ui/theme.h>

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace o3os;
using ui::Color;

namespace {

using Pair = unsigned;
Pair pair_of(Color a, Color b) {
  const unsigned x = static_cast<unsigned>(a), y = static_cast<unsigned>(b);
  return (x < y ? x : y) << 4 | (x < y ? y : x);
}

const char* const kNames[] = {"Chrome", "ChromeText", "ChromeDim", "Pane", "Text", "TextDim", "Empty", "Instr",
                              "Vol",    "Fx",         "Sel",       "SelText", "Accent", "AccentText", "Play", "PlayText"};
std::string name_of(Pair p) { return std::string(kNames[p >> 4]) + " / " + kNames[p & 15]; }

// Decorations of the plots: one-pixel rules, ticks and caps running beside what they divide, never read against it.
//   Empty beside anything               a lane's frame, the grid, a meter's frame, the graticule: a rule beside the data
//   ChromeDim beside Pane               the rule at the edge of a chrome band (the pattern view's own decoration)
//   Text beside Instr, Fx, Play         a slice rule, the zero hairline's opaque neighbour, a hold tick or a clip lamp beside
//                                       a wave, a loop rule or a meter body
//   TextDim beside Instr                the 0 dB reference line of a curve, and a ruler's tick, crossing the line
//   Instr beside Fx, Sel                the wave against a loop rule or the edge of a selection
const Pair kDecorations[] = {
    pair_of(Color::ChromeDim, Color::Pane), pair_of(Color::Text, Color::Instr), pair_of(Color::Text, Color::Fx),
    pair_of(Color::Text, Color::Play),      pair_of(Color::TextDim, Color::Instr), pair_of(Color::Instr, Color::Fx),
    pair_of(Color::Instr, Color::Sel),
};

void collect(const sim::EveRaster& raster, const std::map<std::uint32_t, Color>& token, std::set<Pair>& out) {
  std::vector<std::int8_t> names(800 * 480);
  const std::uint8_t* px = raster.rgba();
  for (std::size_t i = 0; i < names.size(); ++i) {
    const std::uint32_t c = (std::uint32_t{px[i * 4]} << 16) | (std::uint32_t{px[i * 4 + 1]} << 8) | px[i * 4 + 2];
    const auto it = token.find(c);
    names[i] = it != token.end() ? static_cast<std::int8_t>(it->second) : static_cast<std::int8_t>(-1);
  }
  for (int y = 0; y < 480; ++y) {
    for (int x = 0; x < 800; ++x) {
      const std::int8_t here = names[static_cast<std::size_t>(y) * 800 + static_cast<std::size_t>(x)];
      if (here < 0) {
        continue;
      }
      const auto note = [&](std::int8_t other) {
        if (other >= 0 && other != here) {
          out.insert(pair_of(static_cast<Color>(here), static_cast<Color>(other)));
        }
      };
      if (x + 1 < 800) {
        note(names[static_cast<std::size_t>(y) * 800 + static_cast<std::size_t>(x) + 1]);
      }
      if (y + 1 < 480) {
        note(names[static_cast<std::size_t>(y + 1) * 800 + static_cast<std::size_t>(x)]);
      }
    }
  }
}

}  // namespace

TEST_CASE("theme contrast: every pair of tokens that touch in the plot gallery is in the contrast table or a named decoration") {
  const ui::Theme& amiga = ui::theme(ui::ThemeId::Amiga);
  std::map<std::uint32_t, Color> token;
  for (unsigned c = 0; c < ui::kColorCount; ++c) {
    const ui::Rgb& v = amiga.colour[c];
    token.emplace((std::uint32_t{v.r} << 16) | (std::uint32_t{v.g} << 8) | v.b, static_cast<Color>(c));
  }
  const sim::Screen* plot = nullptr;
  for (unsigned i = 0; i < sim::screen_count(); ++i) {
    plot = std::string(sim::screens()[i].name) == "plot" ? &sim::screens()[i] : plot;
  }
  CHECK(plot != nullptr);
  if (plot == nullptr) {
    return;
  }
  std::set<Pair> touching;
  for (unsigned s = 0; s < plot->state_count; ++s) {
    for (unsigned v = 0; v < (plot->states[s].variants < 7 ? plot->states[s].variants : 7); ++v) {
      sim::EveRaster raster;
      sim::RasterDisplay display(raster);
      CHECK(sim::load_assets(display) == hal::UploadResult::Ok);
      auto storage = std::make_unique<eve::DisplayListBuffer>();
      eve::DisplayListBuilder builder(display, storage->words, eve::kMaxDisplayListWords);
      ui::Renderer r(builder, amiga);
      CHECK(r.begin(Color::Pane));
      plot->states[s].draw(r, v);
      CHECK(r.end());
      CHECK(display.render_failures() == 0u);
      collect(raster, token, touching);
    }
  }
  std::set<Pair> table;
  for (unsigned i = 0; i < ui::kContrastPairCount; ++i) {
    table.insert(pair_of(ui::kContrastPairs[i].fg, ui::kContrastPairs[i].bg));
  }
  const std::set<Pair> decoration(std::begin(kDecorations), std::end(kDecorations));
  for (const Pair p : touching) {
    const bool frame = (p >> 4) == static_cast<unsigned>(Color::Empty) || (p & 15) == static_cast<unsigned>(Color::Empty);
    CHECK_MESSAGE((table.count(p) == 1 || decoration.count(p) == 1 || frame),
                  name_of(p) << " touch in a plot picture but are in neither the contrast table nor the decorations");
  }
  for (const Pair p : decoration) {
    CHECK_MESSAGE(touching.count(p) == 1, name_of(p) << " is named a decoration but never touches in a plot picture");
  }
  CHECK(touching.size() >= 10u);
}
