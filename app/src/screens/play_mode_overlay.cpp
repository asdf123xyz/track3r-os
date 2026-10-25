// SPDX-License-Identifier: GPL-3.0-or-later
// The play mode list (OS-INSTUI S4): the eight play modes (play_mode.h), opened by Shift + F1 on the Wave screen. Same shape as the module picker.
#include <o3os/app/key_prompt.h>
#include <o3os/app/play_mode.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/app/text_format.h>
#include <o3os/core/fixed_string.h>

#include "dialog.h"

namespace o3os::app {

using ui::Color;
using ui::Face;
using ui::Rect;

namespace {

constexpr int kPickW = 520;

Dialog pick_dialog(const ui::Theme& theme) noexcept {
  return make_dialog(theme, kPickW, (static_cast<int>(kPlayModeCount) + 2) * theme.metrics.row_pitch);  // a line of help, the modes, the keys
}

// What each mode plays, in a few words.
constexpr const char* kPlayModeHelp[kPlayModeCount] = {"START to END, once", "loops forward", "loops backward", "loops back and forth", "one slice, picked by Slice",
                                                       "the pads play the slices", "one window as an oscillator", "a grain at a position"};

}  // namespace

Rect PlayModeOverlay::rect(const ui::Theme& theme) const noexcept { return pick_dialog(theme).outer; }

int PlayModeOverlay::count(const Model&, const ViewState&) const noexcept { return static_cast<int>(kPlayModeCount); }

bool PlayModeOverlay::set_position(const Model&, ViewState& view, int position) const noexcept {
  const int to = position < 0 ? 0 : (position >= static_cast<int>(kPlayModeCount) ? static_cast<int>(kPlayModeCount) - 1 : position);
  const bool changed = to != view.wave.mode_pick;
  view.wave.mode_pick = static_cast<std::uint8_t>(to);
  return changed;
}

void PlayModeOverlay::render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const Dialog d = pick_dialog(t);
  core::FixedString<24> title;
  title.assign("PLAY MODE  INSTRUMENT ");
  append_dec(title, static_cast<unsigned>(view.instrument < 1 ? 1 : view.instrument), 2);
  draw_dialog(r, d, title.c_str());
  const int x = d.body.x + 2 * m.pad;
  const int right = d.body.x + d.body.w - 2 * m.pad;
  const int pitch = m.row_pitch;
  const int cell = t.geometry(Face::Data).cell_w;
  r.text(x, ui::caps_y(t, Face::Label, d.body.y, pitch), "How this instrument plays its sample.", Color::ChromeText, Face::Label);
  const model::Instrument* in = model.project != nullptr ? model.project->instrument(view.instrument < 1 ? 0u : static_cast<unsigned>(view.instrument - 1)) : nullptr;
  const int current = in != nullptr && in->kind == model::InstrumentKind::Sample ? static_cast<int>(in->sampler.play_mode & 7u) : -1;
  const int cursor = view.wave.mode_pick < kPlayModeCount ? view.wave.mode_pick : 0;
  for (int k = 0; k < static_cast<int>(kPlayModeCount); ++k) {
    const int y = d.body.y + (1 + k) * pitch;
    if (k == cursor) {
      r.text(x, ui::caps_y(t, Face::Data, y, pitch), ">", Color::ChromeText, Face::Data);
    }
    r.text(x + 2 * cell, ui::caps_y(t, Face::Data, y, pitch), kPlayModeNames[k], Color::ChromeText, Face::Data);
    r.text(x + 14 * cell, ui::caps_y(t, Face::Label, y, pitch), kPlayModeHelp[k], Color::ChromeDim, Face::Label);
    if (k == current) {
      r.text(right - r.text_width("NOW", Face::Label), ui::caps_y(t, Face::Label, y, pitch), "NOW", Color::ChromeText, Face::Label);
    }
  }
  const int foot = d.body.y + (static_cast<int>(kPlayModeCount) + 1) * pitch;
  (void)draw_prompt_hints(r, x, foot, pitch, prompt_in_force(model, view), 2);
}

constexpr PlayModeOverlay kPlayModeOverlay{};

}  // namespace o3os::app
