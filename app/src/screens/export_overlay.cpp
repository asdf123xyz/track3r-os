// SPDX-License-Identifier: GPL-3.0-or-later
// The progress of a running WAV export (OS-WAVEXP; export_view.h): what is being made, which of the two passes it is in (the first measures the length of the file, the second writes it),
// a bar, and the one key that works, F8 CANCEL. The F-key row is the dialog's own, painted on the frame band (ViewState::overlay_keys).
#include <o3os/app/frame.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/app/text_format.h>
#include <o3os/core/fixed_string.h>
#include <o3os/ui/components.h>

#include "dialog.h"

namespace o3os::app {

using ui::Color;
using ui::Face;
using ui::Rect;

namespace {

constexpr int kExportW = 560;
constexpr int kExportRows = 5;  // the file, the pass, the bar, the percent and the hint, the keys

Dialog export_dialog(const ui::Theme& theme) noexcept { return make_dialog(theme, kExportW, kExportRows * theme.metrics.row_pitch); }

}  // namespace

Rect ExportOverlay::rect(const ui::Theme& theme) const noexcept { return export_dialog(theme).outer; }

void ExportOverlay::render(const Model&, const ViewState& view, ui::Renderer& r) const noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const Dialog d = export_dialog(t);
  const ExportView& e = view.xport;
  const bool song = e.request.what == audio::OfflineRequest::What::Song;
  const bool og = e.kind == ExportView::Og;  // OS-FILES-C C2c: the whole project as an OG folder: files, not passes
  draw_dialog(r, d, og ? "EXPORTING THE PROJECT AS AN OG FOLDER" : e.kind == ExportView::Stems ? (song ? "EXPORTING THE SONG'S STEMS" : "EXPORTING A PATTERN'S STEMS") : (e.kind == ExportView::Selection ? "EXPORTING THE SELECTION" : (song ? (e.request.order_last != audio::OfflineRequest::kToEnd ? "EXPORTING THE SLOTS" : "EXPORTING THE SONG") : "EXPORTING A PATTERN")));
  const int x = d.body.x + 2 * m.pad;
  const int w = d.body.w - 4 * m.pad;
  const int pitch = m.row_pitch;
  // the file being made (the Data face's cells are wide: the longest name is cut by the dialog's width, not wrapped)
  r.text(x, ui::caps_y(t, Face::Data, d.body.y, pitch), e.armed ? "STOPPING THE TRANSPORT" : e.file, Color::ChromeText, Face::Data);
  core::FixedString<48> pass;
  if (e.armed) {
    pass.assign("waiting for the sequencer to stop");
  } else if (og) {
    pass.assign("FILE ");
    append_uint(pass, e.og_total != 0u && e.og_done < e.og_total ? e.og_done + 1u : e.og_done);
    pass.append(" OF ");
    append_uint(pass, e.og_total);
    pass.append("   writing the project's files");
  } else if (e.kind == ExportView::Stems && e.stems != 0u) {  // a set: which file of how many, and what it is doing (the first file measures the MIX's length first)
    pass.assign("FILE ");
    append_uint(pass, e.stem);
    pass.append(" OF ");
    append_uint(pass, e.stems);
    pass.append(e.pass == 0u ? "   measuring the length" : "   writing ");
    if (e.pass != 0u) {
      pass.append(e.part);
    }
  } else {
    pass.assign(e.pass == 0u ? "PASS 1 OF 2   measuring the length" : "PASS 2 OF 2   writing the file");
  }
  r.text(x, ui::caps_y(t, Face::Label, d.body.y + pitch, pitch), pass.c_str(), Color::ChromeDim, Face::Label);
  const Rect bar_area{x, d.body.y + 2 * pitch + m.gap, w, pitch - 2 * m.gap};
  ui::bar(r, bar_area, e.permille, 1000u, Color::ChromeDim, Color::ChromeText);
  core::FixedString<8> pct;
  append_uint(pct, static_cast<unsigned>(e.permille / 10u));
  pct.append("%");
  r.text(x, ui::caps_y(t, Face::Data, d.body.y + 3 * pitch, pitch), pct.c_str(), Color::ChromeText, Face::Data);
  r.text(x, ui::caps_y(t, Face::Label, d.body.y + 4 * pitch, pitch), og ? "F8 stops it: finished files stay, no project.mt." : e.kind == ExportView::Stems ? "Nothing else works until it ends. F8 stops it; finished files stay." : "Nothing else works until it ends. A cancelled export leaves no file.", Color::ChromeDim, Face::Label);
  // (The F-key row is the dialog's prompt, painted by render_scene: F8 is CANCEL, not the screen's.)
}

constexpr ExportOverlay kExportOverlay{};

}  // namespace o3os::app
