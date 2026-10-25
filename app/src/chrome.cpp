// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/chrome.h>

namespace o3os::app {

using ui::Color;
using ui::Face;

void draw_chrome_bands(ui::Renderer& r, const Frame& f, ui::Rect covered) noexcept {
  const ui::Rect top = {0, 0, ui::kScreenW, f.strip.h + f.header.h};
  const ui::Rect bottom = {0, f.info.y, ui::kScreenW, f.info.h + f.keys.h};
  if (!rect_inside(top, covered)) {
    ui::strip(r, top, ui::Edge::Bottom);
  }
  if (!rect_inside(bottom, covered)) {
    ui::strip(r, bottom, ui::Edge::Top);
  }
}

StatusStrip::StatusStrip(ui::Renderer& r, const Frame& f) noexcept
    : r_(r),
      f_(f),
      row_(r, 2 * r.theme().metrics.pad,
           f.strip.y + (f.strip.h - r.theme().geometry(Face::Display).cap_h) / 2 + r.theme().geometry(Face::Display).cap_h) {}

void StatusStrip::divider() noexcept {
  const ui::Metrics& m = r_.theme().metrics;
  row_.skip(2 * m.gap);
  r_.panel({row_.x(), f_.divider_y, m.border, f_.divider_h}, Color::ChromeDim);
  row_.skip(m.border + 2 * m.gap);
}

void StatusStrip::transport(bool playing) noexcept {
  row_.skip(ui::transport_badge(r_, row_.x(), f_.badge_y, f_.badge_h, playing));
}

// OS-WAVEXP: the transport is stopped for an export, and the badge says what the unit is doing instead; the look of PLAY (it is the state the strip is in, not a stop).
void StatusStrip::exporting() noexcept { row_.skip(ui::transport_badge(r_, row_.x(), f_.badge_y, f_.badge_h, true, "EXPORTING")); }

void StatusStrip::rec() noexcept {
  const ui::Theme& t = r_.theme();
  const ui::Metrics& m = t.metrics;
  const int icon = static_cast<int>(ui::kIconSize);
  const int w = m.pad + icon + m.pad + r_.text_width("REC", Face::Label) + m.pad;
  row_.skip(m.gap);
  r_.panel({row_.x(), f_.badge_y, w, f_.badge_h}, Color::Rec);
  r_.icon(row_.x() + m.pad, f_.badge_y + (f_.badge_h - icon) / 2, ui::Icon::Record, Color::SelText);
  r_.text(row_.x() + m.pad + icon + m.pad, ui::caps_y(t, Face::Label, f_.badge_y, f_.badge_h), "REC", Color::SelText, Face::Label);
  row_.skip(w);
}

// The load group (OS-FU3, OS-VOX): two lines of the Label face, right-aligned in the strip's two halves. Above, "CPU 31% MEM 12%" (both slots reserve "100%", so the strip does
// not move); below, "VOX 9/16": the voices in use of the pool (the peak of a half second) and, inverted for half a second after a voice was stolen, the warning.
// The Data face's 20-pixel cells made the group 144 pixels wide and the Pattern strip's instrument name 4 characters; the Label face makes it 136 with the third figure.
int StatusStrip::load_width(ui::Renderer& r) noexcept { return r.text_width("CPU 100% MEM 100%", Face::Label); }

void StatusStrip::cpu(const SystemLoad& load) noexcept {
  const ui::Theme& t = r_.theme();
  const ui::Metrics& m = t.metrics;
  const int cell = t.geometry(Face::Label).cell_w;
  const int x0 = ui::kScreenW - 2 * m.pad - load_width(r_);
  const int half = f_.strip.h / 2;
  const int y1 = ui::caps_y(t, Face::Label, f_.strip.y, half);
  const int y2 = ui::caps_y(t, Face::Label, f_.strip.y + half, half);
  const auto percent_text = [](char (&text)[5], std::int8_t percent) {
    char* p = text;
    if (percent < 0) {
      *p++ = '-';
      *p++ = '-';
    } else {
      if (percent >= 100) {
        *p++ = '1';
        *p++ = '0';
      } else if (percent >= 10) {
        *p++ = static_cast<char>('0' + percent / 10);
      }
      *p++ = percent >= 100 ? '0' : static_cast<char>('0' + percent % 10);
      *p++ = '%';
    }
    *p = '\0';
  };
  char cpu[5];
  char mem[5];
  percent_text(cpu, load.cpu);
  percent_text(mem, load.mem);
  // OS-GOV: above 70 % the CPU figure sits on an amber chip (Accent), above 90 % on a red one (Rec), both with the text colours the theme pairs with them; the chip is the VOX steal chip's
  // shape (a border of padding round the cap height), so the strip does not move. Normal: as before.
  const CpuLevel level = load.level;  // (with hysteresis: the App keeps it)
  if (level != CpuLevel::Normal) {
    const int chip_h = t.geometry(Face::Label).cap_h + 2 * m.border;
    r_.panel({x0 - m.border, f_.strip.y + (half - chip_h) / 2, 4 * cell + r_.text_width(cpu, Face::Label) + 2 * m.border, chip_h}, level == CpuLevel::Alert ? Color::Rec : Color::Accent);
  }
  const Color cpu_label = level == CpuLevel::Alert ? Color::SelText : (level == CpuLevel::Warn ? Color::AccentText : Color::ChromeDim);
  const Color cpu_ink = level == CpuLevel::Alert ? Color::SelText : (level == CpuLevel::Warn ? Color::AccentText : Color::ChromeText);
  r_.text(x0, y1, "CPU", cpu_label, Face::Label);
  r_.text(x0 + 4 * cell, y1, cpu, cpu_ink, Face::Label);
  r_.text(x0 + 9 * cell, y1, "MEM", Color::ChromeDim, Face::Label);
  r_.text(x0 + 13 * cell, y1, mem, Color::ChromeText, Face::Label);

  // VOX n/pool: the figure is "--" without a source. A steal inverts the whole indicator (the chip of the message and the mode: ChromeText fill, Chrome text).
  char vox[8] = "--";
  if (load.voices >= 0 && load.pool != 0u) {
    char* p = vox;
    const unsigned v = static_cast<unsigned>(load.voices);
    if (v >= 10u) {
      *p++ = static_cast<char>('0' + v / 10u);
    }
    *p++ = static_cast<char>('0' + v % 10u);
    *p++ = '/';
    if (load.pool >= 10u) {
      *p++ = static_cast<char>('0' + load.pool / 10u);
    }
    *p++ = static_cast<char>('0' + load.pool % 10u);
    *p = '\0';
  }
  const int vox_w = 4 * cell + r_.text_width(vox, Face::Label);
  if (load.steal) {
    const int chip_h = t.geometry(Face::Label).cap_h + 2 * m.border;
    r_.panel({x0 - m.border, f_.strip.y + half + (half - chip_h) / 2, vox_w + 2 * m.border, chip_h}, Color::ChromeText);
    r_.text(x0, y2, "VOX", Color::Chrome, Face::Label);
    r_.text(x0 + 4 * cell, y2, vox, Color::Chrome, Face::Label);
  } else {
    r_.text(x0, y2, "VOX", Color::ChromeDim, Face::Label);
    r_.text(x0 + 4 * cell, y2, vox, Color::ChromeText, Face::Label);
  }
}

void draw_info_line(ui::Renderer& r, const Frame& f, const char* left, const char* right) noexcept {
  const ui::Theme& t = r.theme();
  const int pad = t.metrics.pad;
  const int y = ui::caps_y(t, Face::Label, f.info.y, f.info.h);
  r.text(2 * pad, y, left, Color::ChromeText, Face::Label);
  r.text(ui::kScreenW - 2 * pad - r.text_width(right, Face::Label), y, right, Color::ChromeText, Face::Label);
}

}  // namespace o3os::app
