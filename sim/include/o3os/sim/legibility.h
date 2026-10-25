// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The legibility sweep (OS-E2E-5, docs/specs/OS-E2E.md section 4, `o3sim --legibility`): the rasteriser logs every glyph, tile and icon a frame draws
// (EveRaster::record_glyphs: where, in which colour, under which scissor, which pixels have ink and what was under them), and this judges the log. A
// frame has a finding when
//
//   Clipped   some ink of a bitmap is cut by the scissor or by the screen edge (a label that runs off the panel, a name that is cut short);
//   Overlap   two bitmaps put ink on the same pixel (text over text);
//   Contrast  the ink's colour on the colour it was drawn on is not a pair of the theme's contrast table (ui::kContrastPairs: the pairs that
//             tests/theme_test.cpp holds to their ratios), so a text colour that no theme test has looked at.
//
// Ink that a later opaque panel covers is not on the picture and is not judged (the base screen's text under a dialog; a translucent tint covers nothing).
// Two things are allowed by rule and counted in the run, never silent: a faux bold (the Label face's M or S twice in one colour, a pixel apart, the pattern
// header's marks), and a bitmap that a later opaque panel hides only in part (a Clipped finding with the reason "covered in part"). Any other deliberate
// exception goes in the allow list in legibility.cpp, with the reason; it is printed with the run too. Host only.

#include <o3os/sim/eve_raster.h>
#include <o3os/ui/theme.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace o3os::sim {

enum class LegibilityKind : std::uint8_t { Clipped, Overlap, Contrast };
const char* legibility_kind_name(LegibilityKind kind) noexcept;  // "clipped", "overlap", "contrast"

// An exception: the findings of the frames whose key starts with `shot`, of this kind, whose text contains `what`, are allowed, for `reason`.
struct LegibilityAllow {
  const char* shot;
  LegibilityKind kind;
  const char* what;
  const char* reason;
};

struct LegibilityFinding {
  LegibilityKind kind = LegibilityKind::Clipped;
  std::string shot;               // the frame: its gallery key, "gallery/pattern_worst_amiga"
  std::string what;               // one line: what, where and how much
  const char* allowed = nullptr;  // the reason of the allow-list entry that covers it; nullptr: a finding
};

// The named allow list. Each entry says why; adding one is a decision, not a way to get a run green (docs/specs/OS-E2E.md section 4).
const std::vector<LegibilityAllow>& legibility_allow_list();

// Judges the bitmaps that `raster` logged for the frame it drew last (record_glyphs(ui::kRamGEnd) was on), drawn with `theme`. `shot` is the frame's
// name. The findings are grouped by kind (clipped, overlap, contrast), each in draw order; those the allow list (or the faux-bold rule) covers carry
// the reason in `allowed`.
std::vector<LegibilityFinding> check_legibility(const EveRaster& raster, const ui::Theme& theme, const std::string& shot,
                                                const std::vector<LegibilityAllow>& allow = legibility_allow_list());

}  // namespace o3os::sim
