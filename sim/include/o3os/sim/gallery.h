// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The gallery (`o3sim --gallery <dir>`): every screen x state x theme of the registry (sim/screens.h, ui/theme.h)
// rendered through the real renderer and the display-list rasteriser, one PNG per state and theme, contact sheets,
// and SHA-256 hashes that the command compares with the goldens (goldens.txt, keys
// "gallery/<screen>_<state>_<theme>"). The point is to look at the screens after every UI change, in every theme, and
// to see which ones changed.

#include <o3os/sim/eve_raster.h>
#include <o3os/ui/theme.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace o3os::sim {

struct GalleryShot {
  std::string key;      // "gallery/pattern_worst_amiga": the golden key
  std::string file;     // "pattern_worst_amiga.png": relative to the gallery directory
  std::string theme;    // "amiga": ui::Theme::name
  std::string hash;     // lower-case hex SHA-256 of the 800 x 480 RGBA bytes
  std::size_t words;    // display-list words of the frame, DISPLAY included
  unsigned variant;     // the variant drawn (a worst case shows the one that needs the most words)
  // Filled by the caller when goldens are given (it decides the frame colour on the contact sheet):
  enum class Verdict : std::uint8_t { Unchecked, Ok, Changed, New } verdict = Verdict::Unchecked;
  std::vector<std::uint8_t> thumb;  // kThumbW x kThumbH RGBA, a quarter-size copy for the contact sheet
};

inline constexpr int kThumbW = 200;
inline constexpr int kThumbH = 120;

// Which part of the gallery a run renders (the whole of it by default). Applied in this order: `theme`, then `prefix`, then
// the shard of what is left.
struct GalleryFilter {
  const ui::ThemeId* theme = nullptr;  // one theme only (null: every built-in theme)
  std::string prefix;                  // only the screens whose registry name starts with it (empty: every screen)
  unsigned shard = 0;                  // shard `shard` of `shards`: every shards-th shot of the sequence, from shot `shard`
  unsigned shards = 1;                 // (a split into parallel runs whose union is the whole sequence, none twice)
};

// Looks at every frame as it is drawn (the legibility sweep, sim/legibility.h): the raster has recorded its bitmaps (EveRaster::record_glyphs
// below `record_glyphs_below`, a RAM_G address) when `frame` is called, with the shot just made (every field is set, the thumbnail too; it is
// added to the returned `shots` after the call) and the theme the state was drawn with. The PNG, if there is one, is written after the call.
struct GalleryObserver {
  std::uint32_t record_glyphs_below = 0;
  std::function<void(const GalleryShot&, const EveRaster&, const ui::Theme&)> frame;
};

// Renders every state of every screen in every built-in theme, or the part that `filter` names. The PNGs are written to
// `dir` only if it is non-empty (it must exist); the shots are returned in registry order, the themes innermost, so a
// contact sheet of three columns has one state in the three themes on each row. false: a frame could not be built or
// drawn (`error` says which) or a file could not be written.
bool render_gallery(const std::string& dir, std::vector<GalleryShot>& shots, std::string& error,
                    const GalleryFilter& filter = GalleryFilter(), const GalleryObserver* observer = nullptr);

// The golden keys ("gallery/<screen>_<state>_<theme>") of the shots that `filter` selects, in render order: nothing is drawn.
// The stale-golden check, the check that a prefix names a screen and the shard cover test use it. (An invalid shard: none.)
std::vector<std::string> gallery_keys(const GalleryFilter& filter = GalleryFilter());

// Writes a contact sheet: the thumbnails in a grid, each under its name, words and, if the verdict is known, a
// frame in the colour of the verdict (green unchanged, red changed, amber new). false: file error.
inline constexpr int kSheetColumns = 3;
bool write_contact_sheet(const std::string& path, const std::vector<GalleryShot>& shots, int columns = kSheetColumns);

// The contact sheet's size for `count` thumbnails (for tests): kSheetColumns per row, or `columns`.
int contact_sheet_width(int columns = kSheetColumns);
int contact_sheet_height(std::size_t count, int columns = kSheetColumns);

}  // namespace o3os::sim
