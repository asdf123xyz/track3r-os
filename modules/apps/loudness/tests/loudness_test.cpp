// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-H2a (new Community OS behaviour): the Loudness VU app driven through the ABI as the OS drives it, on a fake of the ABI 1.4 host tables (o3m_test::FakeApi: the tap's select point and the
// levels it answers): the descriptor, an OS that lends no tap, the tap point (selected once, re-selected when the parameter moves), the peak with its fall and hold, the RMS average, the clip
// lamp and counter, Delete, the history line, the dBFS readouts against the clock (including a wrap), hostile levels, the draw budgets, and the packed .tmod with its capability word. Host only.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../../../tests/o3m_test_support.h"

#include <cstring>
#include <cmath>
#include <string>

using namespace o3m_test;
using namespace o3os::module;

namespace {

constexpr std::uint32_t kSource = 0, kHold = 1;

struct Meter {
  Lib lib;
  Instance inst;
  FakeApi api;
  ViewHost views;
  bool ok = false;
  float p[2] = {2.0f, 3.0f};
  explicit Meter(std::uint32_t caps = O3M_CAP_AUDIO_TAP) {
    ViewHost::Metrics m;  // the theme's faces: Display 16 x 32, Data 10 x 20, Label 8 x 16, Micro 6 x 12
    const std::int16_t cw[O3M_FACE_COUNT] = {16, 10, 8, 6}, ch[O3M_FACE_COUNT] = {32, 20, 16, 12};
    for (unsigned f = 0; f < O3M_FACE_COUNT; ++f) {
      m.cell_w[f] = cw[f];
      m.cell_h[f] = ch[f];
    }
    views.set_metrics(m);
    ok = lib.open(O3M_LOUDNESS_HOST_SO) && inst.start_with(*lib.entry, make_app_host(api, caps));
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
  // The tap answers `frames` frames with these figures to the next frame's tap_levels call.
  void answer(float peak, float rms, std::uint32_t clips = 0, std::uint32_t frames = 64) {
    o3m_tap_levels lv{};
    lv.size = sizeof lv;
    lv.frames = frames;
    lv.peak = peak;
    lv.rms = rms;
    lv.clip_blocks = clips;
    api.tap_levels.push_back(lv);
  }
  // A frame of 33 ms with the levels asked for.
  Drawing frame(float peak, float rms, std::uint32_t clips = 0, std::uint32_t ms = 33) {
    api.now += ms;
    answer(peak, rms, clips);
    return draw();
  }
};

bool has(const Drawing& d, const std::string& s) {
  for (const auto& t : d.texts) {
    if (t.s == s) {
      return true;
    }
  }
  return false;
}
// The text that starts with `prefix`, or "".
std::string starts(const Drawing& d, const std::string& prefix) {
  for (const auto& t : d.texts) {
    if (t.s.rfind(prefix, 0) == 0) {
      return t.s;
    }
  }
  return "";
}
// The readout right of the PEAK / RMS bars: the texts at the readout column are 6 characters wide ("-12.0"), identified by their y.
std::string at_y(const Drawing& d, int y) {
  for (const auto& t : d.texts) {
    if (t.y == y && t.x > 300) {
      return t.s;
    }
  }
  return "";
}
std::string peak_readout(const Drawing& d) { return at_y(d, 46); }
std::string rms_readout(const Drawing& d) { return at_y(d, 82); }
// The filled width of the bar that starts at y (the PLAY and ACCENT rects of that row).
int bar_fill(const Drawing& d, int y) {
  int w = 0;
  for (const auto& r : d.rects) {
    if (r.y == y && r.h == 24 && (r.color == O3M_COLOR_PLAY || r.color == O3M_COLOR_ACCENT)) {
      w += r.w;
    }
  }
  return w;
}
bool lamp_lit(const Drawing& d) {
  for (const auto& r : d.rects) {
    if (r.w == 18 && r.h == 18) {
      return r.color == O3M_COLOR_ACCENT;
    }
  }
  return false;
}
// The value of a row of the page (o3edit::draw_rows: the name at the left, the value on the same line): "MASTER" of SOURCE, "3 S" of HOLD.
std::string row_value(const Drawing& d, const std::string& name) {
  for (const auto& n : d.texts) {
    if (n.s != name) {
      continue;
    }
    for (const auto& v : d.texts) {
      if (v.y == n.y && v.x > n.x) {
        return v.s;
      }
    }
  }
  return "";
}
// The HOLD and MAX figures of the line "HOLD  -6.0   MAX  -6.0" (6 columns each).
std::string hold_of(const Drawing& d) { return starts(d, "HOLD ").substr(4, 6); }
std::string max_of(const Drawing& d) { return starts(d, "HOLD ").substr(16, 6); }
std::string trim(const std::string& s) {
  const std::size_t a = s.find_first_not_of(' ');
  return a == std::string::npos ? "" : s.substr(a);
}

}  // namespace

TEST_CASE("loudness: descriptor, capability and an OS that lends no draw table") {
  Meter a;
  NEED(a.ok);
  const o3m_module& m = *a.lib.entry;
  CHECK(m.kind == O3M_KIND_APP);
  CHECK(std::string(m.name) == "Loudness VU");
  CHECK((m.abi_major == 1u && m.abi_minor == 10u));  // (1.10: SOURCE and HOLD are the app's own rows, in the grammar, OS-APPUX S3)
  // ABI 1.9 (OS-KEYROWDLG S5): the labels of the page's key row; F8 is the OS's BACK, so the app labels F1..F7 only; Shift + Fn runs Fn (no app reads mods), so the Shift row says the same
  CHECK((m.ui != nullptr && m.ui->keys != nullptr && m.ui->struct_size == O3M_UI_SIZE(9)));
  static const o3m_keys no_keys = {{nullptr}, {nullptr}};
  const o3m_keys* keys = m.ui != nullptr && m.ui->keys != nullptr ? m.ui->keys(nullptr) : nullptr;
  CHECK(keys != nullptr);
  keys = keys != nullptr ? keys : &no_keys;
  CHECK((keys->f[0] != nullptr && std::strcmp(keys->f[0], "CLEAR") == 0));
  CHECK((keys->shift_f[0] != nullptr && std::strcmp(keys->shift_f[0], "CLEAR") == 0));  // Shift + Fn runs Fn (no app reads mods): the Shift row says the same
  CHECK(keys->f[1] == nullptr);
  CHECK(keys->shift_f[1] == nullptr);
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
  CHECK(std::string(m.params[0].name) == "Source");
  CHECK((m.params[0].min == 1.0f && m.params[0].max == 2.0f && m.params[0].def == 2.0f && (m.params[0].flags & O3M_PARAM_INT) != 0u));
  CHECK(std::string(m.params[1].name) == "Hold");
  CHECK((m.params[1].min == 1.0f && m.params[1].max == 10.0f && m.params[1].def == 3.0f && O3M_PARAM_UNIT(m.params[1].flags) == O3M_UNIT_SECONDS));
  alignas(16) unsigned char state[1024] = {};
  o3m_host old = a.inst.host;
  old.draw = nullptr;
  CHECK(m.init(state, &old) != 0);
}

TEST_CASE("loudness: an OS that lends no tap says so instead of empty meters, whatever table is missing") {
  {
    Meter a(0);  // the file did not ask: the four tap entries are NULL
    NEED(a.ok);
    const Drawing d = a.draw();
    CHECK(has(d, "NO AUDIO TAP FOR THIS APP"));
    CHECK(a.api.tap_selects.empty());
  }
  {
    Meter a;
    NEED(a.ok);
    // an OS of ABI 1.3: its io table ends at midi_lost, whatever the pointer bytes after it are
    o3m_io_api v13 = a.api.io;
    v13.size = static_cast<std::uint32_t>(offsetof(o3m_io_api, tap_select));
    o3m_host h = a.inst.host;
    h.io = &v13;
    Instance b;
    NEED(b.start_with(*a.lib.entry, h));
    b.m->ui->enter(b.state);
    Drawing d;
    CHECK(a.views.draw(*a.lib.entry, b.state, 1, 0, 490, 384, a.p, 2) == ViewStatus::Ok);
    a.views.replay(d);
    CHECK(has(d, "NO AUDIO TAP FOR THIS APP"));
    CHECK(a.api.tap_selects.empty());
    // an OS with no io table at all (ABI 1.2)
    o3m_host v12 = a.inst.host;
    v12.size = static_cast<std::uint32_t>(offsetof(o3m_host, read));
    v12.io = nullptr;
    Instance c;
    NEED(c.start_with(*a.lib.entry, v12));
    c.m->ui->enter(c.state);
    Drawing e;
    CHECK(a.views.draw(*a.lib.entry, c.state, 1, 0, 490, 384, a.p, 2) == ViewStatus::Ok);
    a.views.replay(e);
    CHECK(has(e, "NO AUDIO TAP FOR THIS APP"));
  }
}

TEST_CASE("loudness: the tap point is selected once from the parameter, and again only when it changes") {
  Meter a;
  NEED(a.ok);
  CHECK(row_value(a.draw(), "SOURCE") == "MASTER");
  NEED(a.api.tap_selects.size() == 1u);
  CHECK(a.api.tap_selects[0] == O3M_TAP_MASTER);
  (void)a.draw();
  (void)a.draw();
  CHECK(a.api.tap_selects.size() == 1u);
  a.set(kSource, 1.0f);
  CHECK(row_value(a.draw(), "SOURCE") == "INPUT");
  NEED(a.api.tap_selects.size() == 2u);
  CHECK(a.api.tap_selects[1] == O3M_TAP_INPUT);
  a.set(kSource, 7.0f);  // clamped to the top
  CHECK(row_value(a.draw(), "SOURCE") == "MASTER");
  a.set(kSource, std::nanf(""));  // NaN falls to the lower bound
  CHECK(row_value(a.draw(), "SOURCE") == "INPUT");
  CHECK(a.api.tap_source == O3M_TAP_INPUT);
  // entering the page again selects again
  a.inst.m->ui->enter(a.inst.state);
  (void)a.draw();
  CHECK(a.api.tap_selects.back() == O3M_TAP_INPUT);
}

TEST_CASE("loudness: peak and RMS in dBFS, the bars follow them") {
  Meter a;
  NEED(a.ok);
  Drawing d = a.draw();
  CHECK(trim(peak_readout(d)) == "-INF");
  CHECK(trim(rms_readout(d)) == "-INF");
  CHECK(bar_fill(d, 44) == 0);
  // a steady -6 dBFS peak / -12 dBFS RMS signal: the peak reads at once, the RMS settles within a second (300 ms average)
  d = a.frame(0.5f, 0.25f);
  CHECK(trim(peak_readout(d)) == "-6.0");
  CHECK(trim(rms_readout(d)) != "-12.0");  // the first frame has only 33 ms of weight
  for (int i = 0; i < 120; ++i) {
    d = a.frame(0.5f, 0.25f);
  }
  CHECK(trim(rms_readout(d)) == "-12.0");
  CHECK(trim(peak_readout(d)) == "-6.0");
  // the bars: 60 dB across the width, so -6 dB is 90 % and -12 dB 80 % of it
  const int bw = 490 - 8 - 60 - 12 - 72;
  CHECK(bar_fill(d, 44) == static_cast<int>(0.9f * static_cast<float>(bw)));
  CHECK(std::abs(bar_fill(d, 80) - static_cast<int>(0.8f * static_cast<float>(bw))) <= 1);
  // full scale is 0.0, the top of the scale; the part above -6 dB is the accent colour
  d = a.frame(1.0f, 1.0f);
  CHECK(trim(peak_readout(d)) == "0.0");
  CHECK(bar_fill(d, 44) == bw);
  bool accent = false;
  for (const auto& r : d.rects) {
    accent = accent || (r.y == 44 && r.h == 24 && r.color == O3M_COLOR_ACCENT);
  }
  CHECK(accent);
  // dBFS of a very quiet signal and of nothing
  for (int i = 0; i < 160; ++i) {  // let the falling peak reach the floor (24 dB a second: 4 s from full scale)
    d = a.frame(0.0f, 0.0f);
  }
  CHECK(trim(peak_readout(d)) == "-INF");
  d = a.frame(4.0f / 32768.0f, 0.0f);
  CHECK(trim(peak_readout(d)) == "-78.3");
  for (int i = 0; i < 160; ++i) {
    d = a.frame(0.0f, 0.0f);
  }
  d = a.frame(1.0f / 32768.0f, 0.0f);  // one LSB is -90.3 dBFS: below the readout's floor
  CHECK(trim(peak_readout(d)) == "-INF");
}

TEST_CASE("loudness: the peak falls 24 dB a second and the hold marker keeps the highest peak for HOLD seconds") {
  Meter a;
  NEED(a.ok);
  Drawing d = a.frame(0.5f, 0.1f);  // -6.0 dBFS
  CHECK(trim(peak_readout(d)) == "-6.0");
  CHECK(trim(hold_of(d)) == "-6.0");
  // half a second of silence (15 frames of 33 ms = 495 ms): -6 - 12 = about -18
  for (int i = 0; i < 15; ++i) {
    d = a.frame(0.0f, 0.0f);
  }
  const float fell = std::stof(trim(peak_readout(d)));
  CHECK(fell < -17.0f);
  CHECK(fell > -19.5f);
  CHECK(trim(hold_of(d)) == "-6.0");  // HOLD 3 s: still held
  // after 3 s the hold lets go and follows the falling peak
  for (int i = 0; i < 80; ++i) {
    d = a.frame(0.0f, 0.0f);
  }
  CHECK(trim(hold_of(d)) != "-6.0");
  CHECK(trim(max_of(d)) == "-6.0");
  // the hold time is the parameter: 1 s
  Meter b;
  NEED(b.ok);
  b.set(kHold, 1.0f);
  d = b.frame(0.5f, 0.1f);
  for (int i = 0; i < 20; ++i) {  // 660 ms
    d = b.frame(0.0f, 0.0f);
  }
  CHECK(trim(hold_of(d)) == "-6.0");
  for (int i = 0; i < 20; ++i) {  // 1.3 s in all
    d = b.frame(0.0f, 0.0f);
  }
  CHECK(trim(hold_of(d)) != "-6.0");
  // the maximum stays until Delete
  CHECK(trim(max_of(d)) == "-6.0");
  b.press(O3M_BTN_F1);  // CLEAR (Delete is the row's default since OS-APPUX S3)
  d = b.frame(0.0f, 0.0f);
  CHECK(trim(max_of(d)) == "-INF");
  CHECK(trim(hold_of(d)) == "-INF");
  // a stall of seconds is one step of the fall, not a plunge to the floor on the next frame
  Meter c;
  NEED(c.ok);
  d = c.frame(0.5f, 0.1f);
  d = c.frame(0.0f, 0.0f, 0, 5000);
  CHECK(std::stof(trim(peak_readout(d))) > -6.0f - 24.0f * 0.26f);
}

TEST_CASE("loudness: the clip lamp lights for two seconds, CLIPS counts the frames, F1 clears") {
  Meter a;
  NEED(a.ok);
  Drawing d = a.draw();
  CHECK(!lamp_lit(d));
  CHECK(has(d, "CLIPS 0"));
  d = a.frame(1.0f, 0.5f, 3);  // three blocks of that frame had a full-scale sample: one frame with a clip
  CHECK(lamp_lit(d));
  CHECK(has(d, "CLIPS 1"));
  d = a.frame(0.2f, 0.1f);
  CHECK(lamp_lit(d));
  d = a.frame(0.2f, 0.1f, 1);
  CHECK(has(d, "CLIPS 2"));
  for (int i = 0; i < 59; ++i) {  // 1.95 s after the last clip
    d = a.frame(0.2f, 0.1f);
  }
  CHECK(lamp_lit(d));
  d = a.frame(0.2f, 0.1f);
  d = a.frame(0.2f, 0.1f);
  CHECK(!lamp_lit(d));
  CHECK(has(d, "CLIPS 2"));  // the count stays
  bool accent = false;
  for (const auto& t : d.texts) {
    accent = accent || (t.s == "CLIPS 2" && t.color == O3M_COLOR_ACCENT);
  }
  CHECK(accent);  // and says so
  a.press(O3M_BTN_F1);  // CLEAR
  d = a.draw();
  CHECK(has(d, "CLIPS 0"));
  for (const auto& t : d.texts) {
    CHECK(!(t.s == "CLIPS 0" && t.color == O3M_COLOR_ACCENT));
  }
  // other keys and the release of Delete do nothing
  a.frame(1.0f, 0.5f, 1);
  a.event(O3M_EV_BUTTON, O3M_BTN_DELETE, 0);
  a.press(O3M_BTN_INSERT);
  a.event(O3M_EV_PAD, 3, 100);
  a.event(O3M_EV_ENCODER, 0, 5);
  CHECK(has(a.draw(), "CLIPS 1"));
}

TEST_CASE("loudness: F1 CLEAR, Shift + F1 too, and the other keys do nothing (the key row's label, OS-KEYROWDLG S5)") {
  Meter a;
  NEED(a.ok);
  a.frame(1.0f, 0.5f, 3);
  CHECK(has(a.draw(), "CLIPS 1"));
  a.press(O3M_BTN_F1);
  Drawing d = a.draw();
  CHECK(has(d, "CLIPS 0"));
  for (const auto& t : d.texts) {
    CHECK(!(t.s == "CLIPS 0" && t.color == O3M_COLOR_ACCENT));
  }
  a.frame(1.0f, 0.5f, 1);
  a.event(O3M_EV_BUTTON, O3M_BTN_F1, 0);  // the release does nothing
  for (std::uint32_t id : {O3M_BTN_F2, O3M_BTN_F3, O3M_BTN_F4, O3M_BTN_F5, O3M_BTN_F6, O3M_BTN_F7}) {  // blank on the row (F8 never reaches an app)
    a.press(id);
  }
  CHECK(has(a.draw(), "CLIPS 1"));
  const o3m_ui_event shifted = {O3M_EV_BUTTON, O3M_BTN_F1, 1, O3M_MOD_SHIFT};  // Shift + F1 reaches the app as F1 with the Shift flag
  CHECK(a.views.call_input(*a.lib.entry, a.inst.state, 1, shifted) == ViewStatus::Ok);
  CHECK(has(a.draw(), "CLIPS 0"));
}

TEST_CASE("loudness: the history is the RMS every 250 ms, newest at the right, at most 96 points") {
  Meter a;
  NEED(a.ok);
  CHECK(a.draw().polys.empty());  // nothing yet
  // 3 s at -20 dB RMS (0.1): about 12 points once the average has settled
  for (int i = 0; i < 91; ++i) {
    (void)a.frame(0.2f, 0.1f);
  }
  Drawing d = a.draw();
  NEED(d.polys.size() == 1u);
  const auto& xy = d.polys[0].xy;
  const std::size_t n = xy.size() / 2u;
  CHECK((n >= 11u && n <= 13u));
  CHECK(xy[2 * (n - 1)] == 8 + 474 - 1);  // the newest is at the right edge of the frame
  for (std::size_t i = 1; i < n; ++i) {
    CHECK(xy[2 * i] > xy[2 * (i - 1)]);  // left to right
  }
  // 60 s: the line is full (96 points) and still inside the frame, within the draw budgets
  for (int i = 0; i < 1800; ++i) {
    (void)a.frame(0.2f, 0.1f);
  }
  d = a.draw(490, 384, kViewMaxWords);
  NEED(d.polys.size() == 1u);
  CHECK(d.polys[0].xy.size() == 2u * 96u);
  for (std::size_t i = 0; i < d.polys[0].xy.size(); i += 2) {
    CHECK((d.polys[0].xy[i] >= 8 && d.polys[0].xy[i] < 8 + 474));
    CHECK((d.polys[0].xy[i + 1] >= 170 && d.polys[0].xy[i + 1] < 170 + (384 - 170 - 56)));
  }
  // F1 empties it
  a.press(O3M_BTN_F1);
  CHECK(a.draw().polys.empty());
}

TEST_CASE("loudness: the clock wraps, a long stall counts as one step, a silent tap with no frames decays, hostile levels draw something sane") {
  Meter a;
  NEED(a.ok);
  a.api.now = 0xFFFFFFE0u;
  Drawing d = a.frame(0.5f, 0.25f);
  CHECK(trim(peak_readout(d)) == "-6.0");
  d = a.frame(0.5f, 0.25f, 0, 64);  // 32 ms across the wrap
  CHECK(trim(peak_readout(d)) == "-6.0");
  CHECK(trim(hold_of(d)) == "-6.0");
  // the tap answers nothing (frames 0: no new block yet) is not silence: the peak, the hold and the RMS stay as they were
  a.api.tap_levels.clear();
  const std::string peak0 = peak_readout(d), rms0 = rms_readout(d), hold0 = hold_of(d);
  for (int i = 0; i < 40; ++i) {
    a.api.now += 33;
    d = a.draw();
  }
  CHECK(peak_readout(d) == peak0);
  CHECK(rms_readout(d) == rms0);
  CHECK(hold_of(d) == hold0);
  // and the next real frame carries on from there: silence now falls from the held figures
  for (int i = 0; i < 40; ++i) {
    d = a.frame(0.0f, 0.0f);
  }
  CHECK(std::stof(trim(peak_readout(d))) < -30.0f);
  // NaN, infinity, a negative level and an absurd one never break the drawing or the arithmetic of the next frame
  for (const float bad : {std::nanf(""), INFINITY, -INFINITY, -3.0f, 1.0e30f}) {
    d = a.frame(bad, bad);
    CHECK(d.texts.size() > 5u);
  }
  d = a.frame(0.5f, 0.25f);
  d = a.frame(0.5f, 0.25f);
  CHECK(peak_readout(d).size() == 6u);
}

TEST_CASE("loudness: a small page still draws within the budgets, a tiny one draws nothing that breaks") {
  Meter a;
  NEED(a.ok);
  for (int i = 0; i < 400; ++i) {
    (void)a.frame(0.7f, 0.3f, i % 50 == 0 ? 1u : 0u);
  }
  const Drawing small = a.draw(480, 288);
  CHECK(has(small, "LOUDNESS VU"));
  CHECK(!small.polys.empty());
  (void)a.draw(800, 480);
  (void)a.draw(16, 16);
  (void)a.draw(100, 100);
  // the budget the manifest aims at
  const Drawing worst = a.draw(490, 384, 450);
  CHECK(!worst.texts.empty());
}

TEST_CASE("loudness: the packed .tmod passes the loader's checks, carries the audio_tap capability, has no hot code and is small") {
  const Tmod t = load_tmod(O3M_LOUDNESS_TMOD);
  if (!t.present) {
    return;
  }
  CHECK(t.header == Status::Ok);
  CHECK(t.crc_ok);
  CHECK(t.relocation == Status::Ok);
  CHECK(t.descriptor == Status::Ok);
  CHECK(t.h.kind == O3M_KIND_APP);
  CHECK(t.h.abi_minor == 10u);
  CHECK(t.h.reserved2[0] == O3M_CAP_AUDIO_TAP);
  CHECK(t.h.reserved2[1] == 0u);
  CHECK(t.h.module_version == O3M_VERSION(1, 0, 0));
  CHECK(t.h.param_count == 2u);
  CHECK(t.h.view_count == 1u);
  CHECK(t.h.hot_size == 0u);
  CHECK(t.h.state_bytes <= 768u);
  CHECK(t.file_bytes <= 8192u);
  MESSAGE("loudness.tmod: " << t.file_bytes << " B file, hot " << t.h.hot_size << " B, cold " << (t.h.image_size - t.h.hot_size) << " B, state " << t.h.state_bytes << " B");
}

TEST_CASE("loudness: SOURCE and HOLD are the page's two rows: Up / Down, the encoder, Shift + the encoder, Delete (the default), each kept with set_own_param; Delete does not clear (OS-APPUX S3)") {
  Meter a;
  NEED(a.ok);
  CHECK(row_value(a.draw(), "SOURCE") == "MASTER");
  CHECK(row_value(a.draw(), "HOLD") == "3 S");
  CHECK(a.api.own_params.empty());
  a.event(O3M_EV_ENCODER, 0, -1);  // SOURCE: INPUT
  CHECK(row_value(a.draw(), "SOURCE") == "INPUT");
  NEED(a.api.own_params.size() == 1u);
  CHECK((a.api.own_params[0].index == 0u && a.api.own_params[0].value == 1.0f));
  a.event(O3M_EV_ENCODER, 0, 9);  // stops at MASTER
  CHECK(row_value(a.draw(), "SOURCE") == "MASTER");
  a.press(O3M_BTN_DOWN);  // HOLD
  a.event(O3M_EV_ENCODER, 0, 2);
  CHECK(row_value(a.draw(), "HOLD") == "5 S");
  const o3m_ui_event coarse = {O3M_EV_ENCODER, 0, 1, O3M_MOD_SHIFT};  // five seconds a detent
  CHECK(a.views.call_input(*a.lib.entry, a.inst.state, 1, coarse) == ViewStatus::Ok);
  CHECK(row_value(a.draw(), "HOLD") == "10 S");
  CHECK((a.api.own_params.back().index == 1u && a.api.own_params.back().value == 10.0f));
  a.press(O3M_BTN_DELETE);
  CHECK(row_value(a.draw(), "HOLD") == "3 S");
  CHECK((a.api.own_params.back().index == 1u && a.api.own_params.back().value == 3.0f));
  // the tap follows the row (SOURCE INPUT selects the input), Delete does not clear the figures
  a.press(O3M_BTN_UP);
  a.event(O3M_EV_ENCODER, 0, -1);
  (void)a.draw();
  CHECK(a.api.tap_source == O3M_TAP_INPUT);
  a.frame(1.0f, 0.5f, 3);
  CHECK(has(a.draw(), "CLIPS 1"));
  a.press(O3M_BTN_DELETE);
  CHECK(has(a.draw(), "CLIPS 1"));
  // the OS hands the kept rows back before enter(), which clears the transient figures and leaves the rows alone
  a.set(kSource, 2.0f);
  a.set(kHold, 7.0f);
  a.inst.m->ui->enter(a.inst.state);
  CHECK(row_value(a.draw(), "SOURCE") == "MASTER");
  CHECK(row_value(a.draw(), "HOLD") == "7 S");
  CHECK(has(a.draw(), "CLIPS 0"));
}
