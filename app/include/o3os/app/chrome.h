// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The chrome every full screen shares (docs/ui/style-guide.md sections 2 and 5): the two chrome bands, the status strip's
// skeleton (a FieldRow on the Display baseline, group dividers, the transport badge, CPU at the right edge) and the info
// line (text at the left, text at the right). The pattern view and the Song screen both draw through it, so a third screen
// has one copy of this to call and a theme's metric that moves a band moves it everywhere (frame.h gives the rectangles).

#include <o3os/app/frame.h>
#include <o3os/app/system_load.h>
#include <o3os/ui/components.h>
#include <o3os/ui/renderer.h>

namespace o3os::app {

// The top band (strip and header, with its rule at the bottom) and the bottom band (info line and key row, with its rule
// at the top), each left out when it lies entirely inside `covered`.
void draw_chrome_bands(ui::Renderer& r, const Frame& f, ui::Rect covered) noexcept;

// A status strip in the making: `row()` is on the Display face's baseline, centred in the strip, for the screen's own
// captions and values; the rest is the same on every screen.
class StatusStrip {
 public:
  StatusStrip(ui::Renderer& r, const Frame& f) noexcept;
  ui::FieldRow& row() noexcept { return row_; }
  void divider() noexcept;                   // a one-pixel ChromeDim rule with a group gap each side
  void transport(bool playing) noexcept;     // the PLAY / STOP badge
  void exporting() noexcept;                 // OS-WAVEXP: the EXPORTING badge, in the place of PLAY / STOP while a WAV export runs
  void rec() noexcept;                       // the REC badge (Rec mode, OS-REVIEW): the record icon and "REC" in SelText on a Rec fill, a group gap before it
  // "CPU 31% MEM 12%" over "VOX 9/16" at the right edge (OS-FU3, OS-VOX; "--" for a figure the platform does not give). Both percent slots reserve "100%", so the strip does
  // not move; the voices line is inverted while SystemLoad::steal is set. OS-GOV: the CPU figure is on an amber chip above 70 % and a red one above 90 % (SystemLoad::level, with hysteresis: system_load.h next_cpu_level).
  void cpu(const SystemLoad& load) noexcept;
  static int load_width(ui::Renderer& r) noexcept;  // the readouts' width: what a field to their left may reach

 private:
  ui::Renderer& r_;
  const Frame& f_;
  ui::FieldRow row_;
};

// The info line: `left` and `right` in the Label face at full chrome text contrast (it is read, so nothing is dim).
void draw_info_line(ui::Renderer& r, const Frame& f, const char* left, const char* right) noexcept;

}  // namespace o3os::app
