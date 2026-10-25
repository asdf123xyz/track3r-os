// SPDX-License-Identifier: GPL-3.0-or-later
#include "dialog.h"

#include <o3os/app/frame.h>

namespace o3os::app {

using ui::Color;
using ui::Face;
using ui::Rect;

Dialog make_dialog(const ui::Theme& theme, int w, int body_h) noexcept {
  const ui::Metrics& m = theme.metrics;
  const int edge = m.border + m.border;  // the frame and the ring of chrome inside it
  const int title_h = m.row_pitch;
  const int content_h = 2 * edge + title_h + m.pad + body_h + m.pad;
  // Whole rows of the pane, top and bottom: the base screen's rows are then cut only at the dialog's sides, never
  // through the middle of a row's glyphs above or below it.
  const int h = (content_h + m.row_pitch - 1) / m.row_pitch * m.row_pitch;
  const Frame f = make_frame(theme, kFrameRows);
  Dialog d{};
  d.outer = {(ui::kScreenW - w) / 2, f.pane.y + (f.pane.h - h) / 2 / m.row_pitch * m.row_pitch, w, h};
  d.title = {d.outer.x + edge, d.outer.y + edge, w - 2 * edge, title_h};
  d.body = {d.title.x, d.title.y + title_h + m.pad, d.title.w, body_h};
  return d;
}

void draw_dialog(ui::Renderer& r, const Dialog& dialog, const char* title) noexcept {
  const ui::Theme& t = r.theme();
  const int b = t.metrics.border;
  r.panel(dialog.outer, Color::ChromeDim);
  r.panel({dialog.outer.x + b, dialog.outer.y + b, dialog.outer.w - 2 * b, dialog.outer.h - 2 * b}, Color::Chrome);
  r.panel(dialog.title, Color::ChromeText);
  r.text(dialog.title.x + 2 * t.metrics.pad, ui::caps_y(t, Face::Data, dialog.title.y, dialog.title.h), title,
         Color::Chrome, Face::Data);
}

int draw_key_hint(ui::Renderer& r, int x, int y, int h, const char* key, const char* label) noexcept {
  const ui::Theme& t = r.theme();
  const ui::FontGeometry g = t.geometry(Face::Label);
  const int pad = t.metrics.pad;
  const int box_w = r.text_width(key, Face::Label) + 2 * pad;
  const int box_h = g.cap_h + 2 * pad;
  const int text_y = ui::caps_y(t, Face::Label, y, h);
  r.panel({x, y + (h - box_h) / 2, box_w, box_h}, Color::ChromeText);
  r.text(x + pad, text_y, key, Color::Chrome, Face::Label);
  r.text(x + box_w + pad, text_y, label, Color::ChromeText, Face::Label);
  return box_w + pad + r.text_width(label, Face::Label);
}

}  // namespace o3os::app
