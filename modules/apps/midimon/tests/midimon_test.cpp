// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-H1 (new Community OS behaviour): the MIDI Monitor app driven through the ABI as the OS drives it, on a fake of the ABI 1.3 host tables (o3m_test::FakeApi): the descriptor, an OS
// that lends no MIDI, the lines it shows (every message kind, the note names, the channel and port), the order and the number of rows, the filters, pause and clear, the counters, the
// channel strip against the clock, hostile events, the draw budgets, and the packed .tmod with its capability word. Host only.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../../../tests/o3m_test_support.h"

#include <cstring>
#include <string>

using namespace o3m_test;
using namespace o3os::module;

namespace {

constexpr std::uint32_t kChannel = 0, kType = 1;

struct Monitor {
  Lib lib;
  Instance inst;
  FakeApi api;
  ViewHost views;
  bool ok = false;
  float p[2] = {0.0f, 0.0f};
  explicit Monitor(std::uint32_t caps = O3M_CAP_MIDI_IN) {
    ViewHost::Metrics m;  // the theme's faces: Display 16 x 32, Data 10 x 20, Label 8 x 16, Micro 6 x 12
    const std::int16_t cw[O3M_FACE_COUNT] = {16, 10, 8, 6}, ch[O3M_FACE_COUNT] = {32, 20, 16, 12};
    for (unsigned f = 0; f < O3M_FACE_COUNT; ++f) {
      m.cell_w[f] = cw[f];
      m.cell_h[f] = ch[f];
    }
    views.set_metrics(m);
    ok = lib.open(O3M_MIDIMON_HOST_SO) && inst.start_with(*lib.entry, make_app_host(api, caps));
    if (ok) {
      inst.m->ui->enter(inst.state);
    }
  }
  void event(std::uint32_t kind, std::uint32_t id, std::int32_t value) {
    const o3m_ui_event ev = {kind, id, value, 0};
    CHECK(views.call_input(*lib.entry, inst.state, 1, ev) == ViewStatus::Ok);
  }
  void press(std::uint32_t id) { event(O3M_EV_BUTTON, id, 1); }
  void set(std::uint32_t id, float v) {
    inst.set(id, v);
    p[id] = inst.values[id];
  }
  Drawing draw(int w = 490, int h = 384, std::uint32_t max_words = kViewMaxWords) {
    Drawing d;
    CHECK(views.draw(*lib.entry, inst.state, 1, 0, w, h, p, 2, max_words) == ViewStatus::Ok);
    views.replay(d);
    return d;
  }
};

// The lines of the list (the texts that start with a port), top to bottom.
std::vector<std::string> lines(const Drawing& d) {
  std::vector<std::string> out;
  for (const auto& t : d.texts) {
    if (t.s.rfind("DIN ", 0) == 0 || t.s.rfind("USB ", 0) == 0) {
      out.push_back(t.s);
    }
  }
  return out;
}
// The filter as the page's two rows say it (CHANNEL and TYPE, o3edit::draw_rows: a name at the left and its value on the same line), as "CH <channel>  <type>".
std::string filter_of(const Drawing& d) {
  std::string channel, type;
  for (const auto& n : d.texts) {
    if (n.s != "CHANNEL" && n.s != "TYPE") {
      continue;
    }
    for (const auto& v : d.texts) {
      if (v.y == n.y && v.x > n.x) {
        (n.s == "CHANNEL" ? channel : type) = v.s;
      }
    }
  }
  return "CH " + channel + "  " + type;
}
bool has(const Drawing& d, const std::string& s) {
  for (const auto& t : d.texts) {
    if (t.s == s) {
      return true;
    }
  }
  return false;
}
unsigned lit(const Drawing& d) {
  unsigned n = 0;
  for (const auto& r : d.rects) {
    n += r.color == O3M_COLOR_PLAY ? 1u : 0u;
  }
  return n;
}

}  // namespace

TEST_CASE("midimon: descriptor, capability and an OS that lends no draw table") {
  Monitor a;
  NEED(a.ok);
  const o3m_module& m = *a.lib.entry;
  CHECK(m.kind == O3M_KIND_APP);
  CHECK(std::string(m.name) == "MIDI Monitor");
  CHECK((m.abi_major == 1u && m.abi_minor == 10u));  // (1.10: its two rows are the app's own, in the grammar, OS-APPUX S3)
  // ABI 1.9 (OS-KEYROWDLG S5): the labels of the page's key row; F8 is the OS's BACK, so the app labels F1..F7 only; Shift + Fn runs Fn (no app reads mods), so the Shift row says the same
  CHECK((m.ui != nullptr && m.ui->keys != nullptr && m.ui->struct_size == O3M_UI_SIZE(9)));
  static const o3m_keys no_keys = {{nullptr}, {nullptr}};
  const o3m_keys* keys = m.ui != nullptr && m.ui->keys != nullptr ? m.ui->keys(nullptr) : nullptr;
  CHECK(keys != nullptr);
  keys = keys != nullptr ? keys : &no_keys;
  CHECK((keys->f[0] != nullptr && std::strcmp(keys->f[0], "PAUSE") == 0));
  CHECK((keys->shift_f[0] != nullptr && std::strcmp(keys->shift_f[0], "PAUSE") == 0));  // Shift + Fn runs Fn (no app reads mods): the Shift row says the same
  CHECK((keys->f[1] != nullptr && std::strcmp(keys->f[1], "CLEAR") == 0));
  CHECK((keys->shift_f[1] != nullptr && std::strcmp(keys->shift_f[1], "CLEAR") == 0));
  CHECK(keys->f[2] == nullptr);
  CHECK(keys->shift_f[2] == nullptr);
  CHECK(keys->f[3] == nullptr);
  CHECK(keys->shift_f[3] == nullptr);
  CHECK(keys->f[4] == nullptr);
  CHECK(keys->shift_f[4] == nullptr);
  CHECK(keys->f[5] == nullptr);
  CHECK(keys->shift_f[5] == nullptr);
  CHECK(keys->f[6] == nullptr);
  CHECK(keys->shift_f[6] == nullptr);
  CHECK(keys->f[7] == nullptr);
  CHECK(keys->shift_f[7] == nullptr);
  CHECK(m.view_count == 1u);
  CHECK((m.render == nullptr && m.note_on == nullptr && m.note_off == nullptr));
  CHECK(m.param_count == 2u);
  CHECK(std::string(m.params[0].name) == "Channel");
  CHECK((m.params[0].min == 0.0f && m.params[0].max == 16.0f && m.params[0].def == 0.0f && (m.params[0].flags & O3M_PARAM_INT) != 0u));
  CHECK(std::string(m.params[1].name) == "Type");
  CHECK((m.params[1].min == 0.0f && m.params[1].max == 3.0f));
  alignas(16) unsigned char state[512] = {};
  o3m_host old = a.inst.host;
  old.draw = nullptr;
  CHECK(m.init(state, &old) != 0);
}

TEST_CASE("midimon: an OS that lends no MIDI says so instead of an empty list, whatever table is missing") {
  {
    Monitor a(0);  // the io table without midi_read: the file did not ask (or the build has no ring)
    NEED(a.ok);
    CHECK(has(a.draw(), "NO MIDI INPUT FOR THIS APP"));
  }
  {
    Monitor a;
    NEED(a.ok);
    // an OS of ABI 1.2: the host table ends at `draw`
    o3m_host v12 = a.inst.host;
    v12.size = static_cast<std::uint32_t>(offsetof(o3m_host, read));
    v12.io = nullptr;
    Instance b;
    NEED(b.start_with(*a.lib.entry, v12));
    b.m->ui->enter(b.state);
    Drawing d;
    CHECK(a.views.draw(*a.lib.entry, b.state, 1, 0, 490, 384, a.p, 2) == ViewStatus::Ok);
    a.views.replay(d);
    CHECK(has(d, "NO MIDI INPUT FOR THIS APP"));
    // the same with a table whose size says the io member is not there at all, even if the pointer bytes are not zero
    o3m_host junk = v12;
    junk.io = reinterpret_cast<const o3m_io_api*>(0x1000);
    Instance c;
    NEED(c.start_with(*a.lib.entry, junk));
    c.m->ui->enter(c.state);
    Drawing e;
    CHECK(a.views.draw(*a.lib.entry, c.state, 1, 0, 490, 384, a.p, 2) == ViewStatus::Ok);
    a.views.replay(e);
    CHECK(has(e, "NO MIDI INPUT FOR THIS APP"));
  }
}

TEST_CASE("midimon: every kind of channel message is a line with its port, channel and values") {
  Monitor a;
  NEED(a.ok);
  const Drawing empty = a.draw();
  CHECK(has(empty, "WAITING FOR MIDI ON DIN OR USB"));
  CHECK(has(empty, "IN 0  LOST 0  SYSEX 0"));
  CHECK(filter_of(empty) == "CH ALL  ALL");
  CHECK(has(empty, "MIDI MONITOR"));
  a.api.push(0, 0x90, 60, 100);   // note on, channel 1
  a.api.push(0, 0x80, 61, 0);     // note off
  a.api.push(0, 0x93, 62, 0);     // note on with velocity 0 is a note off
  a.api.push(1, 0xBF, 74, 127);   // USB, channel 16, controller
  a.api.push(0, 0xC2, 9, 0);      // program change, channel 3
  a.api.push(0, 0xD0, 33, 0);     // channel pressure
  a.api.push(1, 0xA5, 64, 20);    // polyphonic pressure
  a.api.push(0, 0xE0, 0x00, 0x40);  // pitch bend: centre
  a.api.push(0, 0xE1, 0x7F, 0x7F);  // pitch bend: the top
  a.api.push(0, 0xE1, 0x00, 0x00);  // pitch bend: the bottom
  const Drawing d = a.draw();
  const std::vector<std::string> got = lines(d);
  // newest first; eight rows at this page size, so the two oldest are gone
  NEED(got.size() == 8u);
  CHECK(got[0] == "DIN 02 PITCH    -8192");
  CHECK(got[1] == "DIN 02 PITCH    +8191");
  CHECK(got[2] == "DIN 01 PITCH    +0");
  CHECK(got[3] == "USB 06 POLY AT  E4   64 P 20");
  CHECK(got[4] == "DIN 01 CH AT     33");
  CHECK(got[5] == "DIN 03 PROGRAM    9");
  CHECK(got[6] == "USB 16 CC        74 = 127");
  CHECK(got[7] == "DIN 04 NOTE OFF D4   62 V  0");
  CHECK(has(d, "IN 10  LOST 0  SYSEX 0"));
  // the two that scrolled out, on a page tall enough to hold them: not available (the list keeps eight)
  CHECK(a.draw().texts.size() == d.texts.size());
}

TEST_CASE("midimon: notes are named from MIDI 60 = C4, down to C-1 and up to G9") {
  Monitor a;
  NEED(a.ok);
  a.api.push(0, 0x90, 0, 1);
  a.api.push(0, 0x90, 60, 100);
  a.api.push(0, 0x90, 61, 100);
  a.api.push(0, 0x90, 127, 127);
  const std::vector<std::string> got = lines(a.draw());
  NEED(got.size() == 4u);
  CHECK(got[0] == "DIN 01 NOTE ON  G9  127 V127");
  CHECK(got[1] == "DIN 01 NOTE ON  C#4  61 V100");
  CHECK(got[2] == "DIN 01 NOTE ON  C4   60 V100");
  CHECK(got[3] == "DIN 01 NOTE ON  C-1   0 V  1");
}

TEST_CASE("midimon: eight lines at the page size, fewer on a small page, the oldest scroll out") {
  Monitor a;
  NEED(a.ok);
  for (std::uint8_t n = 0; n < 12; ++n) {
    a.api.push(0, 0x90, static_cast<std::uint8_t>(40 + n), 100);
  }
  const std::vector<std::string> big = lines(a.draw());
  NEED(big.size() == 8u);
  CHECK(big.front().find(" 51 V") != std::string::npos);  // the newest
  CHECK(big.back().find(" 44 V") != std::string::npos);   // the eighth
  CHECK(lines(a.draw(480, 288)).size() == 6u);            // the smallest page the manifest allows
  CHECK(lines(a.draw(800, 480)).size() == 8u);
  CHECK(lines(a.draw(16, 16)).empty());                   // nothing fits: nothing is drawn (and nothing breaks)
}

TEST_CASE("midimon: the filters by channel and type, applied as the message arrives") {
  Monitor a;
  NEED(a.ok);
  a.set(kChannel, 3.0f);
  a.api.push(0, 0x90, 60, 100);  // channel 1
  a.api.push(0, 0x92, 61, 100);  // channel 3
  a.api.push(0, 0xB2, 7, 1);     // channel 3, controller
  CHECK(lines(a.draw()).size() == 2u);
  CHECK(filter_of(a.draw()) == "CH 3  ALL");
  CHECK(has(a.draw(), "IN 3  LOST 0  SYSEX 0"));  // all three were heard, two were kept
  a.set(kType, 2.0f);
  a.api.push(0, 0x92, 62, 100);
  a.api.push(0, 0xB2, 8, 2);
  a.api.push(0, 0xC2, 4, 0);
  const std::vector<std::string> cc = lines(a.draw());
  NEED(cc.size() == 3u);  // the two kept before and the new controller; the new note and the program change did not pass
  CHECK(cc[0].find("CC") != std::string::npos);
  CHECK(filter_of(a.draw()) == "CH 3  CONTROL");
  a.set(kChannel, 0.0f);
  a.set(kType, 1.0f);
  a.api.push(0, 0x80, 60, 0);
  a.api.push(0, 0xE0, 0, 64);
  a.api.push(0, 0xA0, 1, 1);
  CHECK(lines(a.draw()).size() == 4u);  // + the note off only: pitch bend and poly pressure are "other"
  CHECK(filter_of(a.draw()) == "CH ALL  NOTES");
  a.set(kType, 3.0f);
  a.api.push(0, 0xE0, 0, 64);
  a.api.push(0, 0xA0, 1, 1);
  a.api.push(0, 0xD0, 1, 0);
  a.api.push(0, 0xB0, 1, 1);
  CHECK(lines(a.draw()).size() == 7u);
  CHECK(filter_of(a.draw()) == "CH ALL  OTHER");
  // the parameters are clamped, and NaN falls to the lower bound
  a.set(kChannel, 99.0f);
  CHECK(filter_of(a.draw()) == "CH 16  OTHER");
  a.set(kChannel, std::nanf(""));
  a.set(kType, -5.0f);
  a.set(kType, std::nanf(""));
  CHECK(filter_of(a.draw()) == "CH ALL  ALL");
  a.set(kChannel, 2.4f);
  CHECK(filter_of(a.draw()) == "CH 2  ALL");
}

TEST_CASE("midimon: Insert pauses the list, Delete clears it, other keys and releases do nothing") {
  Monitor a;
  NEED(a.ok);
  a.api.push(0, 0x90, 60, 100);
  CHECK(lines(a.draw()).size() == 1u);
  a.press(O3M_BTN_INSERT);
  a.api.push(0, 0x90, 61, 100);
  a.api.push(0, 0x90, 62, 100);
  const Drawing paused = a.draw();
  CHECK(lines(paused).size() == 1u);         // frozen
  CHECK(has(paused, "PAUSED"));
  CHECK(has(paused, "IN 3  LOST 0  SYSEX 0"));  // but still counted
  a.event(O3M_EV_BUTTON, O3M_BTN_INSERT, 0);  // the release does not toggle
  CHECK(has(a.draw(), "PAUSED"));
  a.press(O3M_BTN_INSERT);
  a.api.push(0, 0x90, 63, 100);
  const Drawing run = a.draw();
  CHECK(lines(run).size() == 2u);
  CHECK(!has(run, "PAUSED"));
  a.press(O3M_BTN_PLAY);
  a.event(O3M_EV_PAD, 3, 100);  // (a pad does nothing here; the encoder is the filter's row now, below)
  CHECK(lines(a.draw()).size() == 2u);
  a.press(O3M_BTN_F2);  // CLEAR (it was Delete: Delete is the row's default now)
  const Drawing cleared = a.draw();
  CHECK(lines(cleared).empty());
  CHECK(has(cleared, "IN 0  LOST 0  SYSEX 0"));
  CHECK(has(cleared, "WAITING FOR MIDI ON DIN OR USB"));
  // pausing an empty list says so
  a.press(O3M_BTN_INSERT);
  CHECK(has(a.draw(), "PAUSED"));
  // entering the page again starts clean and running
  a.inst.m->ui->enter(a.inst.state);
  CHECK(!has(a.draw(), "PAUSED"));
}

TEST_CASE("midimon: F1 PAUSE and F2 CLEAR do what Insert and Delete do, with Shift held too (the key row's labels, OS-KEYROWDLG S5)") {
  Monitor a;
  NEED(a.ok);
  const auto shifted = [&](std::uint32_t id, std::int32_t value) {  // Shift + Fn reaches the app as Fn with the Shift flag
    const o3m_ui_event ev = {O3M_EV_BUTTON, id, value, O3M_MOD_SHIFT};
    CHECK(a.views.call_input(*a.lib.entry, a.inst.state, 1, ev) == ViewStatus::Ok);
  };
  a.api.push(0, 0x90, 60, 100);
  CHECK(lines(a.draw()).size() == 1u);
  a.press(O3M_BTN_F1);  // PAUSE
  a.api.push(0, 0x90, 61, 100);
  a.api.push(0, 0x90, 62, 100);
  const Drawing paused = a.draw();
  CHECK(lines(paused).size() == 1u);             // frozen
  CHECK(has(paused, "PAUSED"));
  CHECK(has(paused, "IN 3  LOST 0  SYSEX 0"));   // but still counted
  a.event(O3M_EV_BUTTON, O3M_BTN_F1, 0);         // the release does not toggle
  CHECK(has(a.draw(), "PAUSED"));
  a.press(O3M_BTN_F1);                           // the second press resumes
  a.api.push(0, 0x90, 63, 100);
  const Drawing run = a.draw();
  CHECK(!has(run, "PAUSED"));
  CHECK(lines(run).size() == 2u);
  shifted(O3M_BTN_F1, 1);                        // Shift + F1 is F1: the Shift row says PAUSE too
  CHECK(has(a.draw(), "PAUSED"));
  shifted(O3M_BTN_F1, 1);
  CHECK(!has(a.draw(), "PAUSED"));
  for (std::uint32_t id : {O3M_BTN_F3, O3M_BTN_F4, O3M_BTN_F5, O3M_BTN_F6, O3M_BTN_F7}) {  // the keys the row leaves blank do nothing (F8 never reaches an app)
    a.press(id);
  }
  CHECK(lines(a.draw()).size() == 2u);
  CHECK(!has(a.draw(), "PAUSED"));
  a.press(O3M_BTN_F2);                           // CLEAR
  const Drawing cleared = a.draw();
  CHECK(lines(cleared).empty());
  CHECK(has(cleared, "IN 0  LOST 0  SYSEX 0"));
  CHECK(has(cleared, "WAITING FOR MIDI ON DIN OR USB"));
  a.api.push(0, 0x90, 64, 100);
  CHECK(lines(a.draw()).size() == 1u);
  shifted(O3M_BTN_F2, 1);                        // Shift + F2 is F2
  CHECK(lines(a.draw()).empty());
}

TEST_CASE("midimon: the counters of what the host lost, and the channel strip against the clock") {
  Monitor a;
  NEED(a.ok);
  a.api.overflow = 6;
  a.api.sysex = 2;
  Drawing d = a.draw();
  CHECK(has(d, "IN 0  LOST 6  SYSEX 2"));
  CHECK(lit(d) == 0u);
  a.api.now = 1000;
  a.api.push(0, 0x90, 60, 100);  // channel 1
  a.api.push(1, 0xB9, 1, 1);     // channel 10
  d = a.draw();
  CHECK(lit(d) == 2u);
  a.api.now = 1250;
  CHECK(lit(a.draw()) == 2u);
  a.api.now = 1300;  // 300 ms: dark
  CHECK(lit(a.draw()) == 0u);
  a.api.now = 1400;
  a.api.push(0, 0x95, 60, 100);
  CHECK(lit(a.draw()) == 1u);
  // the newest line is the accent colour while it is recent, then the kind's own
  bool accent = false;
  for (const auto& t : a.draw().texts) {
    accent = accent || (t.s.rfind("DIN 06", 0) == 0 && t.color == O3M_COLOR_ACCENT);
  }
  CHECK(accent);
  a.api.now = 5000;
  for (const auto& t : a.draw().texts) {
    CHECK(!(t.s.rfind("DIN 06", 0) == 0 && t.color == O3M_COLOR_ACCENT));
  }
  // a clock that wraps: the strip works on differences
  a.api.now = 0xFFFFFFF0u;
  a.api.push(0, 0x90, 60, 100);
  CHECK(lit(a.draw()) == 1u);
  a.api.now = 0x00000010u;  // 32 ms later across the wrap
  CHECK(lit(a.draw()) == 1u);
  a.api.now = 0x00000200u;
  CHECK(lit(a.draw()) == 0u);
}

TEST_CASE("midimon: events that are not channel messages are ignored, and a host that over-reports cannot make it read past its buffer") {
  Monitor a;
  NEED(a.ok);
  a.api.push(0, 0xF8, 0, 0);  // real-time (the host never delivers it)
  a.api.push(0, 0xF0, 0, 0);
  a.api.push(0, 0x40, 1, 1);  // no status bit
  a.api.push(0, 0x00, 0, 0);
  CHECK(lines(a.draw()).empty());
  CHECK(has(a.draw(), "IN 0  LOST 0  SYSEX 0"));
  a.api.push(0, 0x90, 0xFF, 0xFF);  // data bytes with the top bit set are masked to 7 bits
  const std::vector<std::string> got = lines(a.draw());
  NEED(got.size() == 1u);
  CHECK(got[0] == "DIN 01 NOTE ON  G9  127 V127");
  // a flood far above the host's ring in one frame is drained in bounded calls
  for (unsigned i = 0; i < 500; ++i) {
    a.api.push(0, 0x90, static_cast<std::uint8_t>(i & 0x7F), 1);
  }
  a.api.reads = 0;
  (void)a.draw();
  CHECK(a.api.reads <= 8u);
  CHECK(a.api.midi.size() >= 500u - 8u * 16u);  // what is beyond the bound waits for the next frame
}

TEST_CASE("midimon: every state draws inside the draw API's budgets at the page size, the smallest size and the largest") {
  Monitor a;
  NEED(a.ok);
  const int sizes[][2] = {{490, 384}, {500, 384}, {480, 288}, {800, 480}, {16, 16}};
  // the widest line of every kind, all channels lit, a paused flag, a filter, counters in the thousands
  a.api.overflow = 123456;
  a.api.sysex = 9999;
  a.api.now = 100;
  for (std::uint8_t k = 0; k < 16; ++k) {
    a.api.push(1, static_cast<std::uint8_t>(0x80 | ((k % 7) << 4) | k), 127, 127);
  }
  a.set(kChannel, 16.0f);
  a.set(kType, 0.0f);
  for (const auto& sz : sizes) {
    Drawing d = a.draw(sz[0], sz[1], 700);
    std::size_t chars = 0;
    for (const auto& t : d.texts) {
      chars += t.s.size();
      CHECK(t.s.size() <= 64u);
    }
    CHECK(chars <= O3M_VIEW_MAX_TEXT);
  }
  a.press(O3M_BTN_INSERT);
  a.set(kChannel, 0.0f);
  for (std::uint8_t k = 0; k < 16; ++k) {
    a.api.push(0, static_cast<std::uint8_t>(0x90 | k), 127, 127);
  }
  Drawing worst = a.draw(490, 384, 700);
  std::size_t chars = 0;
  for (const auto& t : worst.texts) {
    chars += t.s.size();
  }
  CHECK(chars <= O3M_VIEW_MAX_TEXT);
  a.press(O3M_BTN_INSERT);
  a.press(O3M_BTN_F2);
  for (std::uint8_t k = 0; k < 16; ++k) {
    a.api.push(1, static_cast<std::uint8_t>(0x90 | k), 127, 127);
  }
  worst = a.draw(490, 384, 450);  // the budget the manifest aims at
  CHECK(lit(worst) == 16u);
  CHECK(lines(worst).size() == 8u);
}

TEST_CASE("midimon: the packed .tmod passes the loader's checks, carries the midi_in capability, has no hot code and is small") {
  const Tmod t = load_tmod(O3M_MIDIMON_TMOD);
  if (!t.present) {
    return;
  }
  CHECK(t.header == Status::Ok);
  CHECK(t.crc_ok);
  CHECK(t.relocation == Status::Ok);
  CHECK(t.descriptor == Status::Ok);
  CHECK(t.h.kind == O3M_KIND_APP);
  CHECK(t.h.abi_minor == 10u);
  CHECK(t.h.reserved2[0] == O3M_CAP_MIDI_IN);
  CHECK(t.h.reserved2[1] == 0u);
  CHECK(t.h.module_version == O3M_VERSION(1, 0, 0));
  CHECK(t.h.param_count == 2u);
  CHECK(t.h.view_count == 1u);
  CHECK(t.h.hot_size == 0u);
  CHECK(t.h.state_bytes <= 512u);
  CHECK(t.file_bytes <= 6144u);
  MESSAGE("midimon.tmod: " << t.file_bytes << " B file, hot " << t.h.hot_size << " B, cold " << (t.h.image_size - t.h.hot_size) << " B, state " << t.h.state_bytes << " B");
}

TEST_CASE("midimon: CHANNEL and TYPE are the page's two rows: Up / Down, the encoder, Shift + the encoder, Delete (the default), each kept with set_own_param; F2 clears, Delete does not (OS-APPUX S3)") {
  Monitor a;
  NEED(a.ok);
  CHECK(filter_of(a.draw()) == "CH ALL  ALL");
  CHECK(a.api.own_params.empty());
  a.event(O3M_EV_ENCODER, 0, 3);  // CHANNEL 3
  CHECK(filter_of(a.draw()) == "CH 3  ALL");
  NEED(a.api.own_params.size() == 1u);
  CHECK((a.api.own_params[0].index == 0u && a.api.own_params[0].value == 3.0f));
  const o3m_ui_event coarse = {O3M_EV_ENCODER, 0, 2, O3M_MOD_SHIFT};  // Shift + the encoder: four channels a detent
  CHECK(a.views.call_input(*a.lib.entry, a.inst.state, 1, coarse) == o3os::module::ViewStatus::Ok);
  CHECK(filter_of(a.draw()) == "CH 11  ALL");
  a.press(O3M_BTN_DOWN);  // TYPE
  a.event(O3M_EV_ENCODER, 0, 2);
  CHECK(filter_of(a.draw()) == "CH 11  CONTROL");
  CHECK((a.api.own_params.size() == 3u && a.api.own_params[2].index == 1u && a.api.own_params[2].value == 2.0f));
  a.event(O3M_EV_ENCODER, 0, 50);  // stops at the last
  CHECK(filter_of(a.draw()) == "CH 11  OTHER");
  a.press(O3M_BTN_DELETE);  // the row's default, kept
  CHECK(filter_of(a.draw()) == "CH 11  ALL");
  CHECK((a.api.own_params.size() == 5u && a.api.own_params[4].index == 1u && a.api.own_params[4].value == 0.0f));
  a.press(O3M_BTN_UP);
  a.press(O3M_BTN_UP);  // no wrap above the first row
  a.press(O3M_BTN_DELETE);
  CHECK(filter_of(a.draw()) == "CH ALL  ALL");
  // Delete does not clear the list any more: F2 does
  a.api.push(1, 0x90, 60, 100);
  CHECK(lines(a.draw()).size() == 1u);
  a.press(O3M_BTN_DELETE);
  CHECK(lines(a.draw()).size() == 1u);
  a.press(O3M_BTN_F2);
  CHECK(lines(a.draw()).empty());
  // the OS hands the kept values back before enter(); enter() clears the transient list and leaves the filter alone
  a.set(0, 5.0f);
  a.set(1, 3.0f);
  a.inst.m->ui->enter(a.inst.state);
  CHECK(filter_of(a.draw()) == "CH 5  OTHER");
}
