// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/screens/sample_edit_screen.h>

#include "../sample_text.h"

#include <o3os/app/chrome.h>
#include <o3os/app/frame.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/screens/wave_screen.h>
#include <o3os/app/text_format.h>
#include <o3os/ui/components.h>
#include <o3os/ui/plot/wave_view.h>

namespace o3os::app {

using ui::Color;
using ui::Face;
using ui::Rect;
using ui::Renderer;

namespace {

constexpr int kLaneX = 16;
constexpr int kLaneW = 768;
constexpr int kLaneTop = 18;
constexpr int kLaneH = 224;
constexpr int kLaneHFx = 96;  // the FX page: the waveform and its selection are smaller, the rows of the parameters take the room (OS-AUDIT-15)

// A signed number (the FX page's dB).
void append_signed(core::FixedString<40>& s, int v) noexcept {
  if (v < 0) {
    s.append("-");
  }
  append_uint(s, static_cast<unsigned>(v < 0 ? -v : v));
}

// tenths as "n.t".
void append_tenths_text(core::FixedString<40>& s, unsigned tenths) noexcept {
  append_uint(s, tenths / 10u);
  s.append(".");
  append_uint(s, tenths % 10u);
}

// The value of FX row `row` as the page shows it.
void fx_value_text(core::FixedString<40>& s, unsigned row, int v) noexcept {
  switch (row) {
    case kFxType: s.append(v == 0 ? "LOW PASS" : (v == 1 ? "HIGH PASS" : "BAND PASS")); break;
    case kFxCutoff: append_uint(s, static_cast<unsigned>(v)); s.append(" HZ"); break;
    case kFxQ: append_tenths_text(s, static_cast<unsigned>(v)); break;
    case kFxThreshold: append_signed(s, v); s.append(" DB"); break;
    case kFxRatio: append_tenths_text(s, static_cast<unsigned>(v)); s.append(" : 1"); break;
    case kFxAttack: append_tenths_text(s, static_cast<unsigned>(v)); s.append(" MS"); break;
    case kFxRelease: append_uint(s, static_cast<unsigned>(v)); s.append(" MS"); break;
    case kFxMakeup:
      if (v < 0) {
        s.append("AUTO");
      } else {
        append_uint(s, static_cast<unsigned>(v));
        s.append(" DB");
      }
      break;
    default: s.append(v == 0 ? "PEAK" : "RMS"); break;
  }
}

int lane_x(const Window& w, std::uint32_t frame) noexcept {
  if (frame < w.first || frame > w.first + w.span || w.span == 0u) {
    return -1;
  }
  return kLaneX + static_cast<int>(static_cast<std::uint64_t>(frame - w.first) * static_cast<std::uint64_t>(kLaneW - 1) / w.span);
}

}  // namespace

Window sedit_window(const SampleEditState& state, std::uint32_t frames) noexcept {
  return state.span == 0u ? window_fit(frames) : window_clamp(Window{state.first, state.span}, frames);
}

bool sedit_zoom(const Model& model, ViewState& view, int dir) noexcept {
  const SampleBank::Pcm pcm = model.samples != nullptr ? model.samples->pcm(wave_slot(view)) : SampleBank::Pcm{};
  if (pcm.frames == 0u || dir == 0) {
    return false;
  }
  const SampleEditState& st = view.sedit;
  const Window w = sedit_window(st, pcm.frames);
  const std::uint32_t edge = st.b > st.a ? (st.edge == 0u ? st.a : st.b) : w.first + w.span / 2u;
  const Window z = window_zoom(w, pcm.frames, dir, edge >= w.first && edge <= w.first + w.span ? edge : w.first + w.span / 2u);
  if (z.first == w.first && z.span == w.span) {
    return false;
  }
  view.sedit.first = z.first;
  view.sedit.span = z.span >= pcm.frames ? 0u : z.span;
  return true;
}

bool sedit_sync(const Model& model, ViewState& view) noexcept {
  const SampleBank::Pcm pcm = model.samples != nullptr ? model.samples->pcm(wave_slot(view)) : SampleBank::Pcm{};
  SampleEditState& st = view.sedit;
  const SampleEditState before = st;
  if (pcm.frames == 0u) {
    st.a = st.b = 0;
    st.first = st.span = 0;
  } else {
    if (st.b > pcm.frames) {
      st.b = pcm.frames;
    }
    if (st.a >= st.b) {
      st.a = st.b = 0;  // an empty selection is none
    }
    if (st.span >= pcm.frames) {
      st.first = st.span = 0;
    } else if (st.span != 0u) {
      const Window w = window_clamp(Window{st.first, st.span}, pcm.frames);
      st.first = w.first;
      st.span = w.span;
    }
  }
  st.edge = st.edge > 1u ? 0u : st.edge;
  return st.a != before.a || st.b != before.b || st.first != before.first || st.span != before.span || st.edge != before.edge;
}

void SampleEditScreen::enter(const Model& model, ViewState& view) const noexcept { (void)sedit_sync(model, view); }

bool sedit_fx_page(const ViewState& view) noexcept { return view.page[screen_index(ScreenId::SampleEdit)] != 0u; }

Context SampleEditScreen::field_context(const Model&, const ViewState& view) const noexcept { return sedit_fx_page(view) ? Context::SampleEditFx : Context::None; }

bool SampleEditScreen::set_position(const Model&, ViewState& view, int position) const noexcept {
  if (sedit_fx_page(view)) {  // the FX page's rows
    const std::uint8_t row = static_cast<std::uint8_t>(position < 0 ? 0 : (position >= static_cast<int>(kFxCount) ? kFxCount - 1 : position));
    const bool moved = view.sedit.fx_sel != row;
    view.sedit.fx_sel = row;
    return moved;
  }
  const std::uint8_t to = position <= 0 ? 0u : 1u;
  const bool changed = view.sedit.edge != to;
  view.sedit.edge = to;
  return changed;
}

bool SampleEditScreen::step(const Model& model, ViewState& view, int dx, int dy) const noexcept {
  if (sedit_fx_page(view)) {  // a list of rows: any arrow moves along it, round
    const int d = dy != 0 ? dy : dx;
    const int rows = static_cast<int>(kFxCount);
    return d != 0 && set_position(model, view, (view.sedit.fx_sel + (d < 0 ? rows - 1 : 1)) % rows);
  }
  if (dy != 0) {
    return dx == 0 && sedit_zoom(model, view, dy < 0 ? 1 : -1);
  }
  return dx != 0 && set_position(model, view, dx < 0 ? 0 : 1);
}

void SampleEditScreen::render(const Model& model, const ViewState& view, Renderer& r, Rect covered) const noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const Frame f = make_frame(t, kFrameRows);
  const unsigned slot = wave_slot(view);
  const model::Instrument* inst = model.project != nullptr ? model.project->instrument(slot) : nullptr;
  const SampleBank::Pcm pcm = model.samples != nullptr ? model.samples->pcm(slot) : SampleBank::Pcm{};
  const bool valid = inst != nullptr && inst->kind == model::InstrumentKind::Sample && pcm.frames != 0u;
  const SampleEditState& st = view.sedit;
  const std::uint32_t frames = pcm.frames;
  const Window w = sedit_window(st, frames);
  const bool has_sel = valid && st.b > st.a;
  const unsigned rate = inst != nullptr ? inst->sample_rate : 0u;
  const auto hidden = [&](const Rect& rect) { return covers(covered, rect.x, rect.y, rect.w, rect.h); };
  const bool fx = sedit_fx_page(view);
  const Rect lane = {kLaneX, f.pane.y + kLaneTop, kLaneW, fx ? kLaneHFx : kLaneH};

  draw_chrome_bands(r, f, covered);

  if (!hidden(f.strip)) {
    StatusStrip strip(r, f);
    ui::FieldRow& row = strip.row();
    core::FixedString<24> text;
    row.put(Face::Display, "SAMPLE EDIT", Color::ChromeText);
    strip.divider();
    strip.transport(view.playing);
    strip.divider();
    append_dec(text, slot + 1u, 2);
    if (inst != nullptr && inst->name[0] != '\0') {
      text.append(" ");
      append_clipped(text, inst->name, 10);
    }
    row.field("INS", Face::Data, text.c_str(), Color::ChromeDim, Color::ChromeText);
    text.clear();
    append_seconds(text, frames, rate);
    row.field("LENGTH", Face::Data, text.c_str(), Color::ChromeDim, Color::ChromeText);
    strip.cpu(model.load);
  }
  if (!hidden(f.header)) {
    const int cap_y = ui::caps_y(t, Face::Label, f.header.y, f.header.h);
    r.text(kLaneX, cap_y, "SELECTION", Color::ChromeText, Face::Label);
    r.panel({kLaneX, f.header.y + f.header.h - 2 * m.border, r.text_width("SELECTION", Face::Label), 2 * m.border}, Color::Sel);
    // OS-QAFIX-27: the instrument being edited, in words, and how to change it (the strip's INS field is small).
    core::FixedString<40> head;
    head.assign("INSTRUMENT ");
    append_dec(head, slot + 1u, 2);
    if (inst != nullptr && inst->name[0] != '\0') {
      head.append(" ");
      append_clipped(head, inst->name, 12);
    }
    const char* hint = "pad / Shift+F5: change";
    const int hint_x = ui::kScreenW - 2 * m.pad - r.text_width(hint, Face::Label);
    r.text(hint_x, cap_y, hint, Color::ChromeDim, Face::Label);
    r.text(hint_x - 3 * m.gap - r.text_width(head.c_str(), Face::Label), cap_y, head.c_str(), Color::ChromeText, Face::Label);
  }

  if (!hidden(lane)) {
    r.frame({lane.x - 1, lane.y - 1, lane.w + 2, lane.h + 2}, Color::TextDim);
    if (!valid) {
      const char* none = frames == 0u ? "NO SAMPLE IN THIS INSTRUMENT" : "NOT A SAMPLE INSTRUMENT";
      r.text(lane.x + (lane.w - r.text_width(none, Face::Data)) / 2, lane.y + lane.h / 2 - 10, none, Color::TextDim, Face::Data);
    } else if (model.peaks == nullptr || !model.peaks->valid() || !model.peaks->built() || model.peaks->samples() != pcm.data || model.peaks->frames() != frames) {
      const char* wait = "BUILDING THE WAVEFORM";
      r.text(lane.x + (lane.w - r.text_width(wait, Face::Data)) / 2, lane.y + lane.h / 2 - 10, wait, Color::TextDim, Face::Data);
    } else {
      ui::plot::WaveMarks marks;
      marks.zero_line = true;
      marks.clip_tint = true;
      if (model.head.on && model.head.slot == wave_slot(view)) {  // the playhead of the last live voice (OS-FU3)
        marks.has_playhead = true;
        marks.playhead = model.head.frame;
      }
      marks.has_selection = has_sel;
      marks.sel_begin = st.a;
      marks.sel_end = st.b;
      (void)ui::plot::draw_wave(r, lane, *model.peaks, 0, ui::plot::WaveWindow{w.first, w.span}, ui::plot::WaveMode::Vector, nullptr, 0, 0, marks,
                                ui::plot::WaveStyle{});
      if (has_sel) {
        for (unsigned e = 0; e < 2; ++e) {
          const int x = lane_x(w, e == 0u ? st.a : st.b);
          if (x >= 0) {
            const bool sel = e == st.edge;
            r.panel({x - (sel ? 2 : 1), lane.y, sel ? 4 : 2, lane.h}, sel ? Color::Play : Color::Accent);
          }
        }
      }
    }
  }
  if (valid && !hidden({kLaneX, lane.y + lane.h + 3, kLaneW, 12})) {
    core::FixedString<16> a;
    core::FixedString<16> b;
    append_seconds(a, w.first, rate);
    append_seconds(b, w.first + w.span, rate);
    r.text(kLaneX, lane.y + lane.h + 3, a.c_str(), Color::TextDim, Face::Micro);
    r.text(kLaneX + kLaneW - r.text_width(b.c_str(), Face::Micro), lane.y + lane.h + 3, b.c_str(), Color::TextDim, Face::Micro);
  }

  // The readout: the selection, the amplify factor, the history.
  const int cell_h = t.geometry(Face::Data).cell_h;
  const int cell_w = t.geometry(Face::Data).cell_w;
  const int ry = lane.y + lane.h + 22;
  for (unsigned e = 0; e < 2; ++e) {
    const Rect cell = {kLaneX + static_cast<int>(e) * 392, ry, 376, f.row_pitch};
    if (hidden(cell)) {
      continue;
    }
    const bool sel = has_sel && e == st.edge;
    if (sel) {
      r.highlight_row(cell);
    }
    const int ty = cell.y + (f.row_pitch - cell_h) / 2;
    r.text(cell.x + 6, ty, e == 0u ? "START" : "END", sel ? Color::SelText : Color::TextDim);
    core::FixedString<40> text;
    if (valid) {
      const std::uint32_t at = has_sel ? (e == 0u ? st.a : st.b) : (e == 0u ? 0u : frames);
      append_uint(text, at);
      text.append(" fr  ");
      append_seconds(text, at, rate);
      r.text(cell.x + 6 + 7 * cell_w, ty, text.c_str(), sel ? Color::SelText : (has_sel ? Color::Text : Color::TextDim));
    }
  }
  if (fx && valid) {
    // OS-AUDIT-15: the FX page. FILTER's three rows on the left under its caption, COMPRESSOR's six on the right; the history under the filter's rows.
    const int top = ry + f.row_pitch + 8;
    for (unsigned row = 0; row < kFxCount; ++row) {
      const bool comp = row >= kFxThreshold;
      const unsigned idx = comp ? row - kFxThreshold : row;
      const Rect cell = {kLaneX + (comp ? 392 : 0), top + static_cast<int>(idx + 1u) * f.row_pitch, 376, f.row_pitch};
      if (hidden(cell)) {
        continue;
      }
      const bool sel = st.fx_sel == row;
      if (sel) {
        r.highlight_row(cell);
      }
      const int ty = cell.y + (f.row_pitch - cell_h) / 2;
      r.text(cell.x + 6, ty, kFxParams[row].name, sel ? Color::SelText : Color::TextDim);
      core::FixedString<40> text;
      fx_value_text(text, row, st.fx[row]);
      r.text(cell.x + 6 + 11 * cell_w, ty, text.c_str(), sel ? Color::SelText : Color::Text);
    }
    for (unsigned c = 0; c < 2; ++c) {
      const char* cap = c == 0u ? "FILTER" : "COMPRESSOR";
      r.text(kLaneX + static_cast<int>(c) * 392 + 6, top + (f.row_pitch - cell_h) / 2, cap, Color::Accent);
    }
    core::FixedString<80> hist;
    hist.assign("UNDO ");
    append_uint(hist, model.samples != nullptr ? model.samples->undo_depth(slot) : 0u);
    hist.append("  REDO ");
    append_uint(hist, model.samples != nullptr ? model.samples->redo_depth(slot) : 0u);
    r.text(kLaneX + 6, top + 5 * f.row_pitch + (f.row_pitch - cell_h) / 2, hist.c_str(), Color::Text);
    if (model.samples != nullptr) {
      core::FixedString<80> room;
      append_free_kb(room, model.samples->free_samples());
      r.text(kLaneX + 6, top + 6 * f.row_pitch + (f.row_pitch - cell_h) / 2, room.c_str(), Color::Text);
    }
  }
  if (!fx && valid && !hidden({kLaneX, ry + f.row_pitch + 8, kLaneW, f.row_pitch})) {
    core::FixedString<80> text;
    text.assign("AMP ");
    append_uint(text, st.amp);
    text.append("%     SELECTED ");
    append_seconds(text, has_sel ? st.b - st.a : frames, rate);
    text.append("     UNDO ");
    append_uint(text, model.samples != nullptr ? model.samples->undo_depth(slot) : 0u);
    text.append("  REDO ");
    append_uint(text, model.samples != nullptr ? model.samples->redo_depth(slot) : 0u);
    if (model.samples != nullptr) {  // OS-AUDIT-A3 (E-EDIT-08): what is left of the sample memory, for the next effect or paste
      text.append("     ");
      append_free_kb(text, model.samples->free_samples());
    }
    r.text(kLaneX + 6, ry + f.row_pitch + 8 + (f.row_pitch - cell_h) / 2, text.c_str(), Color::Text);
  }

  // OS-AMG1: the AMIGA conversion's keys and its 8-bit option (a line of its own: the info line is full).
  if (!fx && valid && !hidden({kLaneX, ry + 2 * f.row_pitch + 14, kLaneW, f.row_pitch})) {
    core::FixedString<96> text;
    text.assign("AMIGA   Shift+F1: 8287 HZ   Shift+F2: 16574 HZ   Shift+F3: 8-BIT ");
    text.append(st.q8 ? "ON" : "OFF");
    r.text(kLaneX + 6, ry + 2 * f.row_pitch + 14 + (f.row_pitch - cell_h) / 2, text.c_str(), Color::TextDim);
  }

  if (!hidden(f.info)) {
    core::FixedString<64> left;
    if (!valid) {
      left.assign("no sample");
    } else if (model.samples != nullptr && model.samples->rebind_pending()) {
      left.assign("SOUNDS WHEN STOPPED");
    } else if (!has_sel) {
      left.assign("no selection: effects act on all");
    } else {
      left.assign("selection: ");
      append_uint(left, st.b - st.a);
      left.append(" frames");
    }
    draw_info_line(r, f, left.c_str(), fx ? "up / down: row   encoder: value (shift: coarse)   Sample edit: back" : "left / right: edge   up / down: zoom   shift + up / down: AMP %");
  }
  if (!hidden(f.keys) && !view.overlay_keys()) {
    ui::soft_keys(r, f.keys, key_row(*this, view, st.running ? (fx ? kSampleEditFxBusySoftKeys : kSampleEditBusySoftKeys) : (fx ? kSampleEditFxSoftKeys : kSampleEditSoftKeys)), 8);
  }
}

constexpr SampleEditScreen kSampleEditScreen{};

}  // namespace o3os::app
