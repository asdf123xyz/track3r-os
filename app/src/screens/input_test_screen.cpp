// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/screens/input_test_screen.h>

#include <o3os/app/frame.h>
#include <o3os/app/text_format.h>
#include <o3os/core/fixed_string.h>
#include <o3os/ui/components.h>

#include <initializer_list>

namespace o3os::app {

using ui::Color;
using ui::Face;
using ui::Rect;
using ui::Renderer;

namespace {

// Short names of the 33 buttons, by button code (the numbering of buttons.h): at most five characters, what a cell has room for.
constexpr const char* kShortNames[kButtonCount] = {
    "F1",   "F2",   "F3",    "F4",    "F5",   "F6",    "F7",   "F8",    // 0..7: soft keys
    "PARAM", "PERF", "FILE", "CONF",  "MSTR",                          // 8..12: screen buttons
    "SLOAD", "SPLAY", "SEDIT", "SREC", "SONG",                         // 13..17
    "NOTE", "INSTR", "VOL",  "FX",    "PAT",                           // 18..22: step column buttons, Pattern
    "PLAY", "COPY", "INS",   "UP",    "DEL",                           // 23..27
    "REC",  "SHIFT", "LEFT", "DOWN",  "RIGHT",                         // 28..32
};

constexpr int kButtonColumns = 11;  // 33 buttons: 11 x 3
constexpr int kPadColumns = 12;     // 48 pads: 12 x 4, pad 0 at the top left
constexpr int kPadRows = 4;

enum class Cell : std::uint8_t { Idle, Seen, Down };

// A line of text: the widest (every counter at 4,294,967,295) is about 85 characters.
using Line = core::FixedString<96>;

struct Layout {
  int margin;
  int caption_h;
  Rect strip;
  Rect footer;
  int buttons_y;  // top of the button grid
  int button_w;
  int button_h;
  int pads_y;
  int pad_w;
  int pad_h;
  int encoder_y;
  int gap;
};

Layout make_layout(const ui::Theme& t) noexcept {
  const ui::Metrics& m = t.metrics;
  Layout l{};
  l.margin = 3 * m.gap;
  l.gap = m.gap;
  l.caption_h = t.geometry(Face::Label).cell_h;
  l.strip = {0, 0, ui::kScreenW, m.strip_h};
  l.footer = {0, ui::kScreenH - m.row_pitch, ui::kScreenW, m.row_pitch};
  const int inner = ui::kScreenW - 2 * l.margin;
  l.button_w = (inner - (kButtonColumns - 1) * m.gap) / kButtonColumns;
  l.button_h = t.geometry(Face::Label).cell_h + 2 * m.pad;
  l.pad_w = (inner - (kPadColumns - 1) * m.gap) / kPadColumns;
  l.pad_h = t.geometry(Face::Data).cell_h + t.geometry(Face::Label).cell_h + 3 * m.pad;
  l.buttons_y = l.strip.y + l.strip.h + m.pad + l.caption_h + m.gap;
  const int buttons_h = 3 * l.button_h + 2 * m.gap;
  l.pads_y = l.buttons_y + buttons_h + 2 * m.gap + l.caption_h + m.gap;
  const int pads_h = kPadRows * l.pad_h + (kPadRows - 1) * m.gap;
  l.encoder_y = l.pads_y + pads_h + 2 * m.gap;
  return l;
}

Cell kind_of(bool down, std::uint8_t presses) noexcept { return down ? Cell::Down : (presses > 0u ? Cell::Seen : Cell::Idle); }

Color ink_of(Cell c) noexcept { return c == Cell::Down ? Color::SelText : (c == Cell::Seen ? Color::Text : Color::TextDim); }

// `n` as one or two digits (the counts stop at 99).
void put_count(core::FixedString<4>& s, unsigned n) noexcept {
  char buf[2];
  if (n >= 10u) {
    put_dec(buf, n, 2);
    s.append(buf, 2);
  } else {
    put_dec(buf, n, 1);
    s.append(buf, 1);
  }
}

// A signed number with an explicit sign, up to ten digits.
void append_signed(Line& s, std::int32_t v) noexcept {
  const bool negative = v < 0;
  const unsigned magnitude = negative ? static_cast<unsigned>(-(v + 1)) + 1u : static_cast<unsigned>(v);
  s.append(negative ? "-" : "+");
  append_uint(s, magnitude);
}

// Milliseconds with one decimal: "1.2".
void append_ms(Line& s, std::uint32_t us) noexcept {
  append_uint(s, us / 1000u);
  s.append(".");
  append_dec(s, (us % 1000u) / 100u, 1);
}

}  // namespace

const InputTestScreen kInputTestScreen{};

const char* input_test_button_name(unsigned code) noexcept { return code < kButtonCount ? kShortNames[code] : "?"; }

void InputTestScreen::enter(const Model&, ViewState& view) const noexcept { view.input_test.reset(); }

void InputTestScreen::render(const Model&, const ViewState& view, Renderer& r, Rect) const noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const Layout l = make_layout(t);
  const InputTestState& s = view.input_test;
  const int label_h = t.geometry(Face::Label).cell_h;

  // ---- The bands: the title and the health of the input path at the top, the keys and the latency at the bottom ----------------
  r.panel(l.strip, Color::Chrome);
  r.panel(l.footer, Color::Chrome);
  r.text(l.margin, ui::caps_y(t, Face::Data, l.strip.y, l.strip.h), "INPUT TEST", Color::ChromeText, Face::Data);
  {
    Line line;
    line.append((s.stats.online & hal::InputStats::kPadsOnline) != 0u ? "PADS OK  " : "PADS NO  ");
    line.append((s.stats.online & hal::InputStats::kButtonsOnline) != 0u ? "KEYS OK  " : "KEYS NO  ");
    line.append("ERR ");
    append_uint(line, s.stats.errors);
    line.append("  DROP ");
    append_uint(line, s.stats.dropped);
    line.append("  FLT ");
    append_uint(line, s.stats.filtered);
    line.append("  RSYNC ");
    append_uint(line, s.stats.resyncs);
    const int w = r.text_width(line.c_str(), Face::Label);
    r.text(ui::kScreenW - l.margin - w, ui::caps_y(t, Face::Label, l.strip.y, l.strip.h), line.c_str(), Color::ChromeText, Face::Label);
  }
  ui::soft_keys(r, l.footer, kInputTestSoftKeys, 8);  // the footer is the key row now: F2 + F3 CLEAR, F4 + F5 EXIT (OS-KEYROWDLG)
  {  // the latency readout, on the caption row of the buttons (the eight keys have the footer to themselves)
    Line line;
    line.append("AGE ");
    append_ms(line, s.last_age_us);
    line.append(" MS  MAX ");
    append_ms(line, s.max_age_us);
    line.append(" MS  EVENTS ");
    append_uint(line, s.events);
    const int w = r.text_width(line.c_str(), Face::Label);
    r.text(ui::kScreenW - l.margin - w, l.buttons_y - l.gap - l.caption_h, line.c_str(), Color::Text, Face::Label);
  }

  // ---- Captions ---------------------------------------------------------------------------------------------------------------
  r.text(l.margin, l.buttons_y - l.gap - l.caption_h, "BUTTONS", Color::TextDim, Face::Label);
  r.text(l.margin, l.pads_y - l.gap - l.caption_h, "PADS  (0 = top left, 11 = top right)", Color::TextDim, Face::Label);

  // ---- The cells: 33 buttons (11 x 3, by code) and 48 pads (12 x 4, by index) -------------------------------------------------
  // Drawn in passes that group the draws by colour, because the display list is words: first every box in its edge colour (the selection
  // blue when down, the dim or the data ink for a frame), then the interiors of the framed cells in the pane colour (a frame is a fill
  // inside a fill, two words, not the four of a drawn outline), then the text by ink.
  const auto button_box = [&](unsigned i) {
    return Rect{l.margin + static_cast<int>(i % kButtonColumns) * (l.button_w + l.gap),
                l.buttons_y + static_cast<int>(i / kButtonColumns) * (l.button_h + l.gap), l.button_w, l.button_h};
  };
  const auto pad_box = [&](unsigned i) {
    return Rect{l.margin + static_cast<int>(i % kPadColumns) * (l.pad_w + l.gap),
                l.pads_y + static_cast<int>(i / kPadColumns) * (l.pad_h + l.gap), l.pad_w, l.pad_h};
  };
  const auto button_cell = [&](unsigned i) { return kind_of(((s.buttons_down >> i) & 1u) != 0u, s.button_presses[i]); };
  const auto pad_cell = [&](unsigned i) { return kind_of(((s.pads_down >> i) & 1u) != 0u, s.pad_presses[i]); };
  const int b = m.border;

  for (const Cell c : {Cell::Idle, Cell::Seen, Cell::Down}) {  // pass 1: the boxes
    const Color edge = c == Cell::Down ? Color::Sel : (c == Cell::Seen ? Color::TextDim : Color::Empty);
    for (unsigned i = 0; i < kButtonCount; ++i) {
      if (button_cell(i) == c) {
        r.panel(button_box(i), edge);
      }
    }
    for (unsigned i = 0; i < kInputTestPads; ++i) {
      if (pad_cell(i) == c) {
        r.panel(pad_box(i), edge);
      }
    }
  }
  for (unsigned i = 0; i < kButtonCount; ++i) {  // pass 2: the interiors of the framed cells
    if (button_cell(i) != Cell::Down) {
      const Rect box = button_box(i);
      r.panel({box.x + b, box.y + b, box.w - 2 * b, box.h - 2 * b}, Color::Pane);
    }
  }
  for (unsigned i = 0; i < kInputTestPads; ++i) {
    if (pad_cell(i) != Cell::Down) {
      const Rect box = pad_box(i);
      r.panel({box.x + b, box.y + b, box.w - 2 * b, box.h - 2 * b}, Color::Pane);
    }
  }
  for (const Cell c : {Cell::Idle, Cell::Seen, Cell::Down}) {  // pass 3: the text
    const Color ink = ink_of(c);
    for (unsigned i = 0; i < kButtonCount; ++i) {
      if (button_cell(i) != c) {
        continue;
      }
      const Rect box = button_box(i);
      r.text(box.x + m.pad / 2, ui::caps_y(t, Face::Label, box.y, box.h), kShortNames[i], ink, Face::Label);
      if (s.button_presses[i] > 0u) {
        core::FixedString<4> n;
        put_count(n, s.button_presses[i]);
        r.text(box.x + box.w - m.pad / 2 - r.text_width(n.c_str(), Face::Label), ui::caps_y(t, Face::Label, box.y, box.h), n.c_str(),
               ink, Face::Label);
      }
    }
    for (unsigned i = 0; i < kInputTestPads; ++i) {
      if (pad_cell(i) != c) {
        continue;
      }
      const Rect box = pad_box(i);
      char index[2];
      put_dec(index, i, 2);
      const char text[3] = {index[0], index[1], '\0'};
      r.text(box.x + m.pad, box.y + m.pad, text, ink, Face::Data);
      if (s.pad_presses[i] > 0u) {
        core::FixedString<4> n;
        put_count(n, s.pad_presses[i]);
        r.text(box.x + box.w - m.pad - r.text_width(n.c_str(), Face::Label), box.y + box.h - m.pad - label_h, n.c_str(), ink,
               Face::Label);
      }
    }
  }

  // ---- The encoder ---------------------------------------------------------------------------------------------------------------
  Line enc;
  enc.append("ENCODER  TOTAL ");
  append_signed(enc, s.encoder_total);
  enc.append("  LAST ");
  append_signed(enc, s.encoder_last);
  enc.append("  EVENTS ");
  append_uint(enc, s.encoder_events);
  r.text(l.margin, l.encoder_y, enc.c_str(), s.encoder_events > 0u ? Color::Text : Color::TextDim, Face::Data);
}

}  // namespace o3os::app
