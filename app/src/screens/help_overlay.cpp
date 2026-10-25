// SPDX-License-Identifier: GPL-3.0-or-later
// The help overlay (OS-HELP, help_text.h): HELP: <SCREEN> and n/15 in the title bar, kHelpRows lines of the page in the Label face, a scroll bar on the right when the page is longer than the body. Colours are the Chrome
// pairs of the other dialogs (ChromeText on Chrome, the title bar inverted); the section labels are key-caps. Only the lines in view are drawn.
// The page covers the whole pane, not only the dialog's width: it declares the full width of the dialog's rows, so a base screen leaves out everything wholly inside it, and the pane is painted Chrome first. The unit's
// fills do not hide what lies under them (OS-SIMKEY, as OS-BUG-OGOVERLAY), and the Perform slots and a module app's page read through the dialog's 10 px sides and its fill.
#include <o3os/app/help_text.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/app/text_format.h>
#include <o3os/core/fixed_string.h>

#include "dialog.h"

namespace o3os::app {

using ui::Color;
using ui::Face;
using ui::Rect;

namespace {

constexpr int kHelpW = 780;

int help_pitch(const ui::Theme& theme) noexcept { return theme.geometry(Face::Label).cell_h + 2 * theme.metrics.border; }

Dialog help_dialog(const ui::Theme& theme) noexcept { return make_dialog(theme, kHelpW, kHelpRows * help_pitch(theme)); }

unsigned page_in_view(const ViewState& view) noexcept { return view.help.page < kHelpPages ? view.help.page : kHelpPages - 1u; }

// The dialog is the height of the pane (kHelpRows): the rectangle is its rows at the full width of the screen.
Rect cover_of(const ui::Theme& theme) noexcept {
  const Rect outer = help_dialog(theme).outer;
  return {0, outer.y, ui::kScreenW, outer.h};
}

}  // namespace

Rect HelpOverlay::rect(const ui::Theme& theme) const noexcept { return cover_of(theme); }

int HelpOverlay::count(const Model&, const ViewState& view) const noexcept { return help_page(page_in_view(view)).count; }

int HelpOverlay::position(const ViewState& view) const noexcept { return view.help.scroll[page_in_view(view)]; }

// The scroll stops at the top and when the last line is at the bottom; a page shorter than the body does not scroll.
bool HelpOverlay::set_position(const Model& model, ViewState& view, int position) const noexcept {
  const int last = count(model, view) - kHelpRows;
  const int to = position < 0 ? 0 : (position > last ? (last < 0 ? 0 : last) : position);
  std::uint16_t& at = view.help.scroll[page_in_view(view)];
  const bool changed = to != at;
  at = static_cast<std::uint16_t>(to);
  return changed;
}

// Left / Right turn the pages, with no wrap at the ends (Q2); each page shows at the scroll it was left at. Up / Down are one line.
bool HelpOverlay::step(const Model& model, ViewState& view, int dx, int dy) const noexcept {
  if (dx != 0) {
    const int to = static_cast<int>(page_in_view(view)) + dx;
    if (to < 0 || to >= static_cast<int>(kHelpPages)) {
      return false;
    }
    view.help.page = static_cast<std::uint8_t>(to);
    return true;
  }
  return dy != 0 && set_position(model, view, position(view) + dy);
}

void HelpOverlay::render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const Dialog d = help_dialog(t);
  const unsigned index = page_in_view(view);
  const HelpPage& page = help_page(index);
  r.panel(cover_of(t), Color::Chrome);  // (the base screen drew nothing wholly inside it: Screen::render's `covered`)
  draw_dialog(r, d, page.title);
  core::FixedString<8> number;  // "3/15", at the right end of the title bar
  append_uint(number, index + 1u);
  number.append("/");
  append_uint(number, kHelpPages);
  r.text(d.title.x + d.title.w - 2 * m.pad - r.text_width(number.c_str(), Face::Data), ui::caps_y(t, Face::Data, d.title.y, d.title.h), number.c_str(), Color::Chrome, Face::Data);

  const int cell = t.geometry(Face::Label).cell_w;
  const int pitch = help_pitch(t);
  const int x = d.body.x + 2 * m.pad;
  const int text_x = x + kHelpKeyCols * cell;
  const int total = count(model, view);
  const int first = position(view);
  for (int k = 0; k < kHelpRows && first + k < total; ++k) {
    const HelpLine& line = page.lines[first + k];
    const int y = d.body.y + k * pitch;
    if (line.key == nullptr) {
      r.text(x, ui::caps_y(t, Face::Label, y, pitch), line.text, Color::ChromeText, Face::Label);
    } else if (line.text[0] == '\0') {
      if (line.key[0] != '\0') {
        (void)draw_key_hint(r, x, y, pitch, line.key, "");  // a section label
      }
    } else {
      r.text(x, ui::caps_y(t, Face::Label, y, pitch), line.key, Color::ChromeText, Face::Label);
      r.text(text_x, ui::caps_y(t, Face::Label, y, pitch), line.text, Color::ChromeText, Face::Label);
    }
  }
  if (total > kHelpRows) {  // the scroll bar: the track in ChromeDim, the thumb in ChromeText, in proportion to the lines in view
    const int bar_w = 2 * m.border + m.pad / 2;
    const int bar_x = d.body.x + d.body.w - m.pad - bar_w;
    const int bar_h = kHelpRows * pitch;
    const int thumb_h = bar_h * kHelpRows / total < 2 * m.pad ? 2 * m.pad : bar_h * kHelpRows / total;
    const int travel = bar_h - thumb_h;
    r.panel({bar_x, d.body.y, bar_w, bar_h}, Color::ChromeDim);
    r.panel({bar_x, d.body.y + travel * first / (total - kHelpRows), bar_w, thumb_h}, Color::ChromeText);
  }
}

constexpr HelpOverlay kHelpOverlay{};

}  // namespace o3os::app
