// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/screens/sample_load_screen.h>

#include "../sample_text.h"

#include <o3os/app/chrome.h>
#include <o3os/app/frame.h>
#include <o3os/app/key_prompt.h>
#include <o3os/app/pattern_layout.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/screens/file_screen.h>
#include <o3os/app/text_format.h>
#include <o3os/ui/components.h>
#include <o3os/ui/plot/wave_view.h>

#include <cstring>

namespace o3os::app {

using ui::Color;
using ui::Face;
using ui::Rect;
using ui::Renderer;

namespace {

constexpr int kRows = FileView::kVisibleRows;
constexpr unsigned kCardNameChars = 22;

struct Layout {
  Frame frame;
  PatternLayout grid;  // the cell offset, so the lines of the list sit where the File and Song screens' do
  int marker_x;
  int name_x;
  int name_w;
  int card_x;
  int card_w;
};

Layout make_layout(const ui::Theme& theme) noexcept {
  const ui::Metrics& m = theme.metrics;
  const int cell_w = theme.geometry(Face::Data).cell_w;
  Layout l{};
  l.frame = make_frame(theme, kRows);
  l.grid = make_pattern_layout(theme, kRows, PatternView::kVisibleTracks);
  l.marker_x = (m.index_w - static_cast<int>(ui::kIconSize)) / 2;
  l.name_x = m.index_w + 2 * m.gap;
  l.name_w = kFileNameChars * cell_w;
  l.card_x = l.name_x + l.name_w + 3 * m.gap;
  l.card_w = ui::kScreenW - l.card_x - 2 * m.pad;
  return l;
}

struct Empty {
  const char* headline;
  const char* hint;
};

Empty empty_state(const DirScan* scan, bool pti, bool host) noexcept {
  if (host) {
    return {"CARD ON COMPUTER", "CONFIG USB: F7 EJECT"};  // OS-USBDISK 4.3
  }
  if (scan == nullptr) {
    return {"NO SD CARD", "the card is not mounted"};
  }
  switch (scan->state()) {
    case DirScan::State::Idle: return {"NO SD CARD", "the card is not mounted"};
    case DirScan::State::Scanning: return {"SCANNING", ""};
    case DirScan::State::Done: return pti ? Empty{"NO .PTI FILES", "SHIFT+F5 SAVES AN INSTRUMENT HERE; folders are listed too"} : Empty{"NO WAV FILES", "copy .wav files onto the card; folders are listed too"};
    case DirScan::State::NoCard: return {"NO SD CARD", "the card is not mounted"};
    case DirScan::State::NoFolder: return pti ? Empty{"NO instruments FOLDER", "SHIFT+F5 ON AN INSTRUMENT MAKES IT; F2 goes up"} : Empty{"FOLDER NOT FOUND", "press F2 for the folder above"};
    case DirScan::State::Failed: return {"CARD ERROR", hal::storage::status_name(scan->error())};
    case DirScan::State::Cancelled: return {"SCAN CANCELLED", "press F6 to read the folder again"};
  }
  return {"", ""};
}

// Is the peak cache the picture of exactly this PCM?
bool peaks_match(const ui::plot::PeakCache* peaks, const SampleBank::Pcm& pcm) noexcept {
  return peaks != nullptr && peaks->valid() && peaks->built() && pcm.frames != 0u && peaks->samples() == pcm.data && peaks->frames() == pcm.frames;
}

}  // namespace

bool sample_load_sync(const Model& model, ViewState& view) noexcept {
  if (model.files == nullptr) {
    return view.wav.list.clear();
  }
  return view.wav.list.fit(model.files->index());
}

void SampleLoadScreen::enter(const Model& model, ViewState& view) const noexcept { (void)sample_load_sync(model, view); }

int SampleLoadScreen::count(const Model& model, const ViewState&) const noexcept {
  return model.files != nullptr ? static_cast<int>(model.files->index().count()) : 0;
}

bool SampleLoadScreen::set_position(const Model& model, ViewState& view, int position) const noexcept {
  return model.files != nullptr && view.wav.list.set(position, model.files->index());
}

bool SampleLoadScreen::step(const Model& model, ViewState& view, int dx, int dy) const noexcept {
  if (model.files == nullptr) {
    return false;
  }
  const DirIndex& index = model.files->index();
  if (dy != 0) {
    return dx == 0 && view.wav.list.move(dy, index);
  }
  if (dx == 0) {
    return false;
  }
  const int to = dx > 0 ? index.next_letter(view.wav.list.pos()) : index.prev_letter(view.wav.list.pos());
  return to != DirIndex::kNone && view.wav.list.set(to, index);
}

void SampleLoadScreen::render(const Model& model, const ViewState& view, Renderer& r, Rect covered) const noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const Layout l = make_layout(t);
  const Frame& f = l.frame;
  const DirScan* scan = model.files;
  const DirIndex* index = scan != nullptr ? &scan->index() : nullptr;
  const int length = index != nullptr ? static_cast<int>(index->count()) : 0;
  FileView lv = view.wav.list;
  if (index != nullptr) {
    (void)lv.fit(*index);
  } else {
    lv.reset();
  }
  const int cell_h = t.geometry(Face::Data).cell_h;
  const int cell_w = t.geometry(Face::Data).cell_w;
  const int visible = length - lv.top() < kRows ? length - lv.top() : kRows;
  const int cursor_slot = lv.pos() - lv.top();
  const auto hidden = [&](const Rect& rect) { return covers(covered, rect.x, rect.y, rect.w, rect.h); };
  const auto row_y = [&](int i) { return f.pane.y + i * f.row_pitch; };
  const auto cell_y = [&](int i) { return row_y(i) + l.grid.cell_dy; };
  const bool scanning = scan != nullptr && scan->state() == DirScan::State::Scanning;
  const unsigned slot = view.instrument >= 1 ? static_cast<unsigned>(view.instrument - 1) : 0u;
  const SampleBank* bank = model.samples;
  const SampleBank::Pcm pcm = bank != nullptr ? bank->pcm(slot) : SampleBank::Pcm{};
  const model::Instrument* inst = model.project != nullptr ? model.project->instrument(slot) : nullptr;

  draw_chrome_bands(r, f, covered);
  const Rect row_bar = {0, row_y(cursor_slot < 0 ? 0 : cursor_slot), l.card_x - m.gap, f.row_pitch};
  if (length > 0 && !hidden(row_bar)) {
    r.highlight_row(row_bar);
  }

  // The status strip: the title, the transport badge, the folder, the counts.
  if (!hidden(f.strip)) {
    StatusStrip strip(r, f);
    ui::FieldRow& row = strip.row();
    core::FixedString<24> text;
    row.put(Face::Display, view.wav.pti ? "INSTRUMENT LOAD" : "SAMPLE LOAD", Color::ChromeText);
    strip.divider();
    strip.transport(view.playing);
    strip.divider();
    const char* folder = view.wav.folder;
    const char* leaf = folder;
    for (const char* p = folder; *p != '\0'; ++p) {
      leaf = *p == '/' ? p + 1 : leaf;
    }
    text.assign("/");
    append_clipped(text, leaf, 12);
    row.field("DIR", Face::Data, text.c_str(), Color::ChromeDim, Color::ChromeText);
    text.clear();
    append_uint(text, static_cast<unsigned>(length));
    if (scanning) {
      text.append("..");
    } else if (scan != nullptr && scan->truncated()) {
      text.append("+");
    }
    row.field("ITEMS", Face::Data, text.c_str(), Color::ChromeDim, Color::ChromeText);
    text.clear();
    if (length > 0) {
      append_uint(text, static_cast<unsigned>(lv.pos() + 1));
    } else {
      text.assign("--");
    }
    row.field("POS", Face::Data, text.c_str(), Color::ChromeDim, Color::ChromeText);
    strip.cpu(model.load);
  }

  // The header: the list's column, the target instrument's card and the page.
  if (!hidden(f.header)) {
    const int cap_y = ui::caps_y(t, Face::Label, f.header.y, f.header.h);
    r.panel({l.name_x - l.grid.cursor_pad_x, f.header.y + f.header.h - 2 * m.border, l.name_w + 2 * l.grid.cursor_pad_x, 2 * m.border}, Color::Sel);
    r.text(l.name_x, cap_y, "NAME", Color::ChromeText, Face::Label);
    core::FixedString<24> head;
    head.assign("TARGET INSTRUMENT ");  // OS-SLDTARGET: which instrument F1 loads into (a pad or Shift + F7 / F8 change it)
    append_dec(head, slot + 1u, 2);
    r.text(l.card_x, cap_y, head.c_str(), Color::ChromeText, Face::Label);
    const char* kind = view.wav.pti ? "PTI" : "WAV";
    r.text(ui::kScreenW - 2 * m.pad - r.text_width(kind, Face::Label), cap_y, kind, Color::ChromeText, Face::Label);
  }

  // The info line and the key row.
  if (!hidden(f.info)) {
    core::FixedString<64> text;
    if (length == 0 && !scanning) {
      text.assign(empty_state(scan, view.wav.pti, view.card_on_computer).headline);
    } else if (scanning) {
      text.assign("reading   ");
      append_uint(text, static_cast<unsigned>(length));
      text.append(" so far");
    } else if (scan != nullptr && scan->truncated()) {
      text.assign("list full: the first ");
      append_uint(text, static_cast<unsigned>(length));
      text.append(" are shown");
    } else {
      append_uint(text, static_cast<unsigned>(length));
      text.append(length == 1 ? " entry" : " entries");
    }
    if (!view.wav.pti && view.wav.mark_count() != 0u) {  // OS-AUDIT-A7: how many files F1 / F4 will load
      text.append("   ");
      append_uint(text, view.wav.mark_count());
      text.append(" marked");
    }
    draw_info_line(r, f, text.c_str(), view.wav.pti ? "pad: target   left / right: letter   F3 enter   F1 load" : "pad: target   left / right: letter   F3 enter   F4 add");
  }
  if (!hidden(f.keys) && !view.overlay_keys()) {
    const char* const* row = key_row(*this, view, kSampleLoadSoftKeys);
    const char* keys[8];
    for (unsigned i = 0; i < 8; ++i) {
      keys[i] = row[i];
    }
    core::FixedString<12> load_label;  // OS-AUDIT-A7: with files marked F1 / F4 say how many they load ("LOAD 3", "ADD 3")
    core::FixedString<12> add_label;
    if (!(view.shift && follows_shift(*this, view))) {  // the plain row; the Shift row is the table's (INST - / INST +, OS-SLDTARGET)
      if (view.wav.pti) {  // OS-INSTUI S7: an instrument file has no ADD, no audition, no wavetable mode and no channel
        keys[3] = keys[4] = "--";
      }
      if (!view.wav.pti && view.wav.mark_count() != 0u) {
        load_label.assign("LOAD ");
        append_uint(load_label, view.wav.mark_count());
        add_label.assign("ADD ");
        append_uint(add_label, view.wav.mark_count());
        keys[0] = load_label.c_str();
        keys[3] = add_label.c_str();
      }
      keys[6] = view.wav.pti ? "--" : wavetable_key_label(view.wav.wavetable);  // F7 shows the mode (OS-GRAN)
      const bool file = !view.wav.pti && length > 0;  // F8 (OS-LOADCH): the channel of a stereo file; a mono one says so (OS-QAFIX-11: no key acts unlabelled)
      keys[7] = !file ? "--" : (index->stereo(static_cast<unsigned>(lv.pos())) ? channel_key_label(view.wav.channel) : "CH MONO");
    }
    KeyRow answers{};  // OS-BUG-SLDOVER: F1 reads REPLACE? (inverted) while the question of a load over an instrument is up
    for (unsigned i = 0; i < 8; ++i) {
      answers.label[i] = keys[i];
    }
    (void)patch_row(prompt_in_force(model, view), view.shift && follows_shift(*this, view), answers);
    ui::soft_keys(r, f.keys, answers.label, 8, answers.dim, answers.hot);
  }

  // The card: the target instrument, in the right part of the pane (a caption, then its value 8 cells over).
  {
    const int vx = l.card_x + 8 * cell_w;
    const unsigned room = static_cast<unsigned>((l.card_w - 8 * cell_w) / cell_w);
    const auto line = [&](int i, const char* caption, const char* value, Color c) {
      if (!hidden({l.card_x, cell_y(i), l.card_w, cell_h})) {
        r.text(l.card_x, cell_y(i), caption, Color::TextDim);
        r.text(vx, cell_y(i), value, c);
      }
    };
    core::FixedString<40> text;
    if (inst != nullptr && inst->kind != model::InstrumentKind::None) {  // OS-SLDTARGET: a module or MIDI instrument is named as what it is (a WAV cannot go there)
      text.assign(inst->kind == model::InstrumentKind::Module ? "MODULE " : (inst->kind == model::InstrumentKind::Midi ? "MIDI " : ""));
      const unsigned used = static_cast<unsigned>(text.size());
      const unsigned cap = kCardNameChars < room ? kCardNameChars : room;
      append_clipped(text, inst->name, cap > used ? cap - used : 0u);
      line(0, "NAME", text.c_str(), Color::Text);
    } else {
      line(0, "NAME", "-- empty --", Color::TextDim);
    }
    if (pcm.frames != 0u) {
      text.clear();
      append_seconds(text, pcm.frames, inst != nullptr ? inst->sample_rate : 0u);
      line(1, "LENGTH", text.c_str(), Color::Text);
      text.clear();
      append_uint(text, pcm.frames);
      line(2, "FRAMES", text.c_str(), Color::Text);
      text.clear();
      append_uint(text, inst != nullptr && inst->sample_rate != 0u ? inst->sample_rate : 44100u);
      text.append(" Hz");
      line(3, "RATE", text.c_str(), Color::Text);
    } else {
      line(1, "LENGTH", "--", Color::TextDim);
    }
    // Its waveform (the App builds the cache when a sample has just come in); a lane of four rows.
    const Rect lane = {l.card_x, row_y(5) + l.grid.cell_dy, l.card_w, 4 * f.row_pitch - 2 * l.grid.cell_dy};
    if (!hidden(lane)) {
      r.frame(lane, Color::TextDim);
      if (peaks_match(model.peaks, pcm)) {
        const Rect inner = {lane.x + 1, lane.y + 1, lane.w - 2, lane.h - 2};
        (void)ui::plot::draw_wave(r, inner, *model.peaks, 0, ui::plot::WaveWindow{0, pcm.frames}, ui::plot::WaveMode::Vector, nullptr, 0, 0, ui::plot::WaveMarks{},
                                  ui::plot::WaveStyle{});
      }
    }
    if (bank != nullptr) {
      // OS-AUDIT-A3 (E-LOAD-04): the sample memory as a bar (the used part, in KB) over "N KB USED  M KB FREE".
      const std::uint32_t total = bank->arena_samples();
      const std::uint32_t spare = bank->free_samples();
      const bool low = spare < 2048u;
      const Rect gauge = {vx, cell_y(9) + cell_h / 4, l.card_w - 8 * cell_w, cell_h / 2};
      if (!hidden(gauge)) {
        r.text(l.card_x, cell_y(9), "ARENA", Color::TextDim);
        ui::bar(r, gauge, sample_kb(spare < total ? total - spare : 0u), sample_kb(total) != 0u ? sample_kb(total) : 1u, Color::Empty, low ? Color::Accent : Color::Instr);
      }
      text.clear();
      append_used_free_kb(text, total, spare);
      line(10, "", text.c_str(), low ? Color::Accent : Color::Text);
      if (bank->rebind_pending()) {
        if (!hidden({l.card_x, cell_y(11), l.card_w, cell_h})) {
          r.text(l.card_x, cell_y(11), "SOUNDS WHEN STOPPED", Color::Accent);
        }
      }
    } else {
      line(10, "ARENA", "NO SAMPLE MEMORY", Color::TextDim);
    }
  }

  // Nothing to list: say why, in the middle of the list.
  if (length == 0 || index == nullptr) {
    const Empty e = empty_state(scan, view.wav.pti, view.card_on_computer);
    const int mid = f.pane.y + f.pane.h / 2;
    const int mid_x = l.name_x + l.name_w / 2;
    const int head_w = r.text_width(e.headline, Face::Data);
    const int head_y = mid - cell_h;
    if (!hidden({mid_x - head_w / 2, head_y, head_w, cell_h})) {
      r.text(mid_x - head_w / 2, head_y, e.headline, Color::TextDim, Face::Data);
    }
    const int hint_w = r.text_width(e.hint, Face::Label);
    const int hint_y = ui::caps_y(t, Face::Label, mid + m.pad, f.row_pitch);
    if (e.hint[0] != '\0' && hint_w <= l.name_w + 3 * m.gap && !hidden({mid_x - hint_w / 2, hint_y, hint_w, f.row_pitch})) {
      r.text(mid_x - hint_w / 2, hint_y, e.hint, Color::TextDim, Face::Label);
    }
    return;
  }

  // The rows: the names, one colour pass at a time, a folder icon in front of a folder.
  core::FixedString<kFileNameChars> names[kRows];
  for (int i = 0; i < visible; ++i) {
    const unsigned entry = static_cast<unsigned>(lv.top() + i);
    // (a stereo name leaves the last three cells to the "ST" tag: the cut is made to that width; the stored name is not touched)
    file_display_name(index->name(entry), names[i], index->stereo(entry) ? kFileNameChars - 3 : kFileNameChars);
  }
  static constexpr Color kPasses[] = {Color::Text, Color::SelText};
  for (const Color pass : kPasses) {
    for (int i = 0; i < visible; ++i) {
      const unsigned entry = static_cast<unsigned>(lv.top() + i);
      const bool sel = i == cursor_slot;
      if ((sel ? Color::SelText : Color::Text) != pass) {
        continue;
      }
      const int icon_y = row_y(i) + (f.row_pitch - static_cast<int>(ui::kIconSize)) / 2;
      if (index->is_dir(entry) && !hidden({l.marker_x, icon_y, static_cast<int>(ui::kIconSize), static_cast<int>(ui::kIconSize)})) {
        r.icon(l.marker_x, icon_y, ui::Icon::Folder, pass);
      } else if (view.wav.marked(entry)) {  // OS-AUDIT-A7 (SLD-MULTI): a file marked for the multi-load, a solid square where a folder has its icon
        const int side = static_cast<int>(ui::kIconSize) / 2;
        const Rect box = {l.marker_x + side / 2, icon_y + side / 2, side, side};
        if (!hidden(box)) {
          r.panel(box, pass);
        }
      }
      if (!hidden({l.name_x, cell_y(i), l.name_w, cell_h})) {
        if (index->stereo(entry)) {  // OS-LOADCH: "ST" in the last two cells of the name column; a name that long is cut to leave them
          r.text(l.name_x, cell_y(i), names[i].c_str(), pass);
          r.text(l.name_x + (kFileNameChars - 2) * cell_w, cell_y(i), "ST", sel ? pass : Color::TextDim);
        } else {
          r.text(l.name_x, cell_y(i), names[i].c_str(), pass);
        }
      }
    }
  }
}

constexpr SampleLoadScreen kSampleLoadScreen{};

}  // namespace o3os::app
