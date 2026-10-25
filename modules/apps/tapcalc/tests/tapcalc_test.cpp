// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-TAPCALC (new Community OS behaviour): the app's arithmetic (tapcalc_math.h: intervals -> BPM, the 2 s reset, the clamps, the note-length table, the number text) against double
// oracles, and the module's host shared object driven through the ABI as the OS drives it, on a fake of the ABI 1.3 host tables (o3m_test::FakeApi: the clock in milliseconds, the project's
// tempo, set_tempo): taps timed by the host clock, manual BPM, pages, F2 setting the project's tempo and its refusals, the draw budgets, plus the packed .tmod. Host only.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../../../tests/o3m_test_support.h"

#include "../tapcalc_math.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

using namespace o3m_test;
using namespace o3os::module;
using namespace tapcalc;

namespace {

constexpr std::uint32_t kAverage = 0;

// n taps `gap` ms apart, from `t0`; returns the last time
std::uint32_t tap_series(Tapper& t, std::uint32_t t0, std::uint32_t gap, unsigned n) {
  std::uint32_t at = t0;
  for (unsigned i = 0; i < n; ++i, at += gap) {
    t.tap(at);
  }
  return at - gap;
}

}  // namespace

TEST_CASE("tapcalc math: steady taps read back as the tempo, one decimal") {
  Tapper t{};
  CHECK(t.bpm10(6) == 0u);  // no tap
  t.tap(1000);
  CHECK(t.bpm10(6) == 0u);  // one tap is no interval
  for (std::uint32_t gap : {500u, 400u, 600u, 750u, 1000u, 300u, 333u}) {
    Tapper s{};
    tap_series(s, 5000, gap, 9);
    const double bpm = 60000.0 / gap;
    CHECK(s.bpm10(8) == static_cast<std::uint32_t>(std::floor(bpm * 10.0 + 0.5)));
  }
  Tapper s{};
  tap_series(s, 0, 500, 9);
  CHECK(s.bpm10(8) == 1200u);
  CHECK(s.intervals() == 8u);
}

TEST_CASE("tapcalc math: the tempo is the mean of the last N intervals (window 4..8), older taps drop out") {
  Tapper t{};
  // intervals 700, 700, 700, 400, 410, 420, 430, 440, 450, 460, 470 (ms): 12 taps, the first 3 fall out of the 9 held
  std::uint32_t at = 100;
  t.tap(at);
  const std::uint32_t gaps[] = {700, 700, 700, 400, 410, 420, 430, 440, 450, 460, 470};
  std::uint32_t times[12] = {at};
  for (unsigned i = 0; i < 11; ++i) {
    at += gaps[i];
    times[i + 1] = at;
    t.tap(at);
  }
  CHECK(t.n == kMaxIntervals + 1u);
  for (std::uint32_t window = 1; window <= 8; ++window) {
    const double span = static_cast<double>(times[11] - times[11 - window]);
    const double bpm = 60000.0 * window / span;
    CHECK(t.bpm10(window) == static_cast<std::uint32_t>(std::floor(bpm * 10.0 + 0.5)));
  }
  CHECK(t.bpm10(100) == t.bpm10(8));  // the window is clamped to what is held
  CHECK(t.bpm10(0) == 0u);            // (a window of 0 reads no interval)
  // fewer taps than the window: all of them
  Tapper few{};
  tap_series(few, 0, 600, 3);
  CHECK(few.bpm10(8) == 1000u);
}

TEST_CASE("tapcalc math: a pause of more than 2 s starts a new series, exactly 2 s does not") {
  Tapper t{};
  tap_series(t, 0, 500, 5);
  CHECK(t.n == 5u);
  CHECK(t.tap(2000 + kResetMs) == false);  // 2000 ms after the tap at 2000
  CHECK(t.n == 6u);
  CHECK(t.tap(2000 + kResetMs + kResetMs + 1u) == true);  // 2001 ms after the last
  CHECK(t.n == 1u);
  CHECK(t.bpm10(8) == 0u);
  t.tap(t.t[0] + 480);
  CHECK(t.bpm10(8) == 1250u);  // the old series is gone: 480 ms -> 125.0
  // the first tap after clear() is fresh as well
  t.clear();
  CHECK(t.tap(7) == true);
}

TEST_CASE("tapcalc math: the range is 20.0..300.0, equal times and a wrapping clock are safe") {
  Tapper fast{};
  tap_series(fast, 0, 50, 5);  // 1200 BPM
  CHECK(fast.bpm10(8) == kMaxBpm10);
  Tapper same{};
  same.tap(100);
  same.tap(100);
  CHECK(same.bpm10(8) == kMaxBpm10);  // no division by zero
  Tapper slow{};
  tap_series(slow, 0, 2000, 3);  // the slowest a series can run: 30.0
  CHECK(slow.bpm10(8) == 300u);
  CHECK(clamp_bpm10(0) == kMinBpm10);
  CHECK(clamp_bpm10(199) == kMinBpm10);
  CHECK(clamp_bpm10(200) == 200u);
  CHECK(clamp_bpm10(3000) == 3000u);
  CHECK(clamp_bpm10(3001) == kMaxBpm10);
  Tapper wrap{};
  tap_series(wrap, 0xFFFFFFFFu - 1000u, 500, 6);  // the clock wraps inside the series
  CHECK(wrap.n == 6u);
  CHECK(wrap.bpm10(8) == 1200u);
}

TEST_CASE("tapcalc math: the note table against a double oracle over the whole range") {
  const double div_q[kDivisions] = {4, 2, 1, 0.5, 0.25, 0.125};
  const double var_f[kVariants] = {1.0, 1.5, 2.0 / 3.0};
  for (std::uint32_t bpm10 = kMinBpm10; bpm10 <= kMaxBpm10; ++bpm10) {
    for (std::uint32_t d = 0; d < kDivisions; ++d) {
      for (std::uint32_t v = 0; v < kVariants; ++v) {
        const double ms = 600000.0 / bpm10 * div_q[d] * var_f[v];
        const double got = note_tenths(bpm10, d, v);  // (three float roundings: a few parts in 10^7 on top of the rounding to a tenth)
        const double hz = 1000.0 / ms * 100.0;
        if (std::fabs(got - ms * 10.0) > 0.5 + 4e-7 * ms * 10.0 || std::fabs(static_cast<double>(hz_hundredths(bpm10, d, v)) - hz) > 0.5 + 4e-7 * hz) {
          FAIL("bpm10 " << bpm10 << " div " << d << " var " << v << " got " << got << " want " << ms * 10.0);
        }
      }
    }
  }
  // the values a musician knows, at 120 BPM
  CHECK(note_tenths(1200, 0, 0) == 20000u);  // 1/1 2000 ms
  CHECK(note_tenths(1200, 2, 0) == 5000u);   // 1/4 500 ms
  CHECK(note_tenths(1200, 2, 1) == 7500u);   // dotted 1/4 750 ms
  CHECK(note_tenths(1200, 3, 1) == 3750u);   // dotted 1/8 375 ms (the dotted-eighth delay)
  CHECK(note_tenths(1200, 3, 2) == 1667u);   // 1/8 triplet 166.7 ms
  CHECK(note_tenths(1200, 5, 0) == 625u);    // 1/32 62.5 ms
  CHECK(hz_hundredths(1200, 2, 0) == 200u);  // 1/4 = 2 Hz
  // out-of-range arguments are clamped, not indexed
  CHECK(note_tenths(0, 0, 0) == note_tenths(kMinBpm10, 0, 0));
  CHECK(note_tenths(99999, 99, 99) == note_tenths(kMaxBpm10, 5, 0));
}

TEST_CASE("tapcalc math: put_fixed writes <whole>.<fraction>") {
  char b[16];
  auto fmt = [&](std::uint32_t v, std::uint32_t dec) {
    const std::uint32_t n = put_fixed(b, 0, v, dec);
    return std::string(b, n);
  };
  CHECK(fmt(0, 1) == "0.0");
  CHECK(fmt(5, 1) == "0.5");
  CHECK(fmt(1200, 1) == "120.0");
  CHECK(fmt(180000, 1) == "18000.0");  // the longest: 1/1 dotted at 20 BPM
  CHECK(fmt(6, 2) == "0.06");
  CHECK(fmt(200, 2) == "2.00");
  CHECK(fmt(6000, 2) == "60.00");
  CHECK(fmt(4294967295u, 2) == "42949672.95");
}

// ---- the module -----------------------------------------------------------------------------------------------------------------------------------

namespace {

constexpr std::uint32_t kBoth = O3M_CAP_READ_PROJECT | O3M_CAP_EDIT_PROJECT;

struct App {
  Lib lib;
  Instance inst;
  FakeApi api;
  ViewHost views;
  bool ok = false;
  float p[1] = {6.0f};
  explicit App(std::uint32_t caps = kBoth) {
    ViewHost::Metrics m;  // the theme's faces: Display 16 x 32, Data 10 x 20, Label 8 x 16, Micro 6 x 12
    const std::int16_t cw[O3M_FACE_COUNT] = {16, 10, 8, 6}, ch[O3M_FACE_COUNT] = {32, 20, 16, 12};
    for (unsigned f = 0; f < O3M_FACE_COUNT; ++f) {
      m.cell_w[f] = cw[f];
      m.cell_h[f] = ch[f];
    }
    views.set_metrics(m);
    ok = lib.open(O3M_TAPCALC_HOST_SO) && inst.start_with(*lib.entry, make_app_host(api, caps));
    if (ok) {
      inst.m->ui->enter(inst.state);
      p[kAverage] = inst.values[kAverage];
    }
  }
  void event(std::uint32_t kind, std::uint32_t id, std::int32_t value) {
    const o3m_ui_event ev = {kind, id, value, 0};
    CHECK(views.call_input(*lib.entry, inst.state, 1, ev) == ViewStatus::Ok);
  }
  void press(std::uint32_t id) { event(O3M_EV_BUTTON, id, 1); }
  void tap_pad(unsigned pad = 0) {
    event(O3M_EV_PAD, pad, 100);
    event(O3M_EV_PAD, pad, 0);
  }
  // `n` taps, `gap` ms apart, the first at the clock as it is
  void taps(unsigned n, std::uint32_t gap) {
    for (unsigned i = 0; i < n; ++i) {
      if (i != 0) {
        api.now += gap;
      }
      tap_pad(i % 48u);
    }
  }
  Drawing draw(int w = 490, int h = 384, std::uint32_t max_words = kViewMaxWords) {
    Drawing d;
    CHECK(views.draw(*lib.entry, inst.state, 1, 0, w, h, p, 1, max_words) == ViewStatus::Ok);
    views.replay(d);
    return d;
  }
};

bool has(const Drawing& d, const char* s) {
  for (const auto& t : d.texts) {
    if (t.s == s) {
      return true;
    }
  }
  return false;
}
bool starts(const Drawing& d, const char* s) {
  for (const auto& t : d.texts) {
    if (t.s.rfind(s, 0) == 0) {
      return true;
    }
  }
  return false;
}
// The colour the tempo text is drawn in: accent while the last tap is under 2 s old, else the text colour.
std::uint8_t bpm_color(const Drawing& d) {
  for (const auto& t : d.texts) {
    if (t.s.size() > 4 && t.s.compare(t.s.size() - 4, 4, " BPM") == 0 && t.s.rfind("PROJECT", 0) != 0 && t.s.rfind("SET", 0) != 0) {
      return t.color;
    }
  }
  return 255;
}

}  // namespace

TEST_CASE("tapcalc app: descriptor, parameter and an OS that lends no draw table or no clock") {
  App a;
  NEED(a.ok);
  const o3m_module& m = *a.lib.entry;
  CHECK(m.kind == O3M_KIND_APP);
  CHECK(std::string(m.name) == "Tap Calc");
  CHECK((m.abi_major == 1u && m.abi_minor == 10u));  // (1.10: BPM and AVERAGE are the app's own rows, in the grammar, OS-APPUX S3)
  // ABI 1.9 (OS-KEYROWDLG S5): the labels of the page's key row; F8 is the OS's BACK, so the app labels F1..F7 only; Shift + Fn runs Fn (no app reads mods), so the Shift row says the same
  CHECK((m.ui != nullptr && m.ui->keys != nullptr && m.ui->struct_size == O3M_UI_SIZE(9)));
  static const o3m_keys no_keys = {{nullptr}, {nullptr}};
  const o3m_keys* keys = m.ui != nullptr && m.ui->keys != nullptr ? m.ui->keys(nullptr) : nullptr;
  CHECK(keys != nullptr);
  keys = keys != nullptr ? keys : &no_keys;
  CHECK((keys->f[0] != nullptr && std::strcmp(keys->f[0], "TAP") == 0));
  CHECK((keys->shift_f[0] != nullptr && std::strcmp(keys->shift_f[0], "TAP") == 0));  // Shift + Fn runs Fn (no app reads mods): the Shift row says the same
  CHECK((keys->f[1] != nullptr && std::strcmp(keys->f[1], "SET") == 0));
  CHECK((keys->shift_f[1] != nullptr && std::strcmp(keys->shift_f[1], "SET") == 0));
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
  CHECK((m.render == nullptr && m.note_on == nullptr && m.note_off == nullptr));  // no audio work at all
  CHECK(m.param_count == 2u);  // AVERAGE (kept in its place) and BPM, appended
  CHECK(std::string(m.params[0].name) == "Average");
  CHECK((m.params[0].min == 4.0f && m.params[0].max == 8.0f && m.params[0].def == 8.0f));
  alignas(16) unsigned char state[256] = {};
  o3m_host old = a.inst.host;
  old.draw = nullptr;
  CHECK(m.init(state, &old) != 0);
  // an OS of ABI 1.2 (the host table ends at `draw`): no clock, no tempo: it says so instead of timing taps in frames
  o3m_host v12 = a.inst.host;
  v12.size = static_cast<std::uint32_t>(offsetof(o3m_host, read));
  v12.read = nullptr;
  v12.edit = nullptr;
  v12.io = nullptr;
  CHECK(m.init(state, &v12) != 0);
  o3m_host noclock = a.inst.host;
  static o3m_io_api blank;
  blank = a.api.io;
  blank.now_ms = nullptr;
  noclock.io = &blank;
  CHECK(m.init(state, &noclock) != 0);
  noclock.io = nullptr;
  CHECK(m.init(state, &noclock) != 0);
}

TEST_CASE("tapcalc app: taps timed by the host clock give the tempo, to the tenth") {
  App a;
  NEED(a.ok);
  const Drawing idle = a.draw();
  CHECK(has(idle, "120.0 BPM"));  // the manual default
  CHECK(has(idle, "TAPS 0  MANUAL  1/2"));
  a.api.now = 1000;
  a.taps(9, 500);
  const Drawing d = a.draw();
  CHECK(has(d, "120.0 BPM"));
  CHECK(has(d, "500.0"));  // 1/4 in ms
  CHECK(starts(d, "TAPS 9  TAP  1/2"));
  // other tempi, exactly: 450 ms = 133.3, 400 ms = 150.0, 750 ms = 80.0
  for (const auto& c : {std::make_pair(450u, "133.3 BPM"), std::make_pair(400u, "150.0 BPM"), std::make_pair(750u, "80.0 BPM")}) {
    App b;
    NEED(b.ok);
    b.api.now = 5;
    b.taps(9, c.first);
    CHECK(has(b.draw(), c.second));
  }
}

TEST_CASE("tapcalc app: a tap is timed by its own event, so the draws between taps change nothing; the jitter of the taps in the middle cancels, the end taps keep theirs") {
  App a;
  NEED(a.ok);
  // nine taps nominally 500 ms apart, the middle ones early or late by up to 16 ms: only the two end times of the window count (AVERAGE 8, the default: the taps 0 and 8, which are exact)
  const int jitter[9] = {0, 9, 0, 16, -16, 7, -3, 14, 0};
  std::uint32_t t0 = 1000;
  for (int i = 0; i < 9; ++i) {
    a.api.now = static_cast<std::uint32_t>(static_cast<std::int64_t>(t0) + 500 * i + jitter[i]);
    a.tap_pad();
    a.draw();  // the page is redrawn at 30 Hz and after every event: no effect on the tempo
    a.draw();
  }
  CHECK(has(a.draw(), "120.0 BPM"));
  // the end taps are NOT immune: the last tap 33 ms off (the worst case of the main loop's jitter) moves the tempo by about 1 BPM over 8 intervals and about 2 BPM over 4, at 120 BPM
  // (R3 of the OS-H1 review: the comment, the guide and the QA row say so; both ends off in opposite directions double it)
  for (const auto& c : {std::pair<std::uint32_t, double>{8u, 1.0}, std::pair<std::uint32_t, double>{4u, 2.0}}) {
    for (const int skew : {+33, -33}) {
      App e;
      NEED(e.ok);
      e.inst.set(kAverage, static_cast<float>(c.first));
      const int n = static_cast<int>(c.first) + 1;
      for (int i = 0; i < n; ++i) {
        e.api.now = static_cast<std::uint32_t>(1000 + 500 * i + (i == n - 1 ? skew : 0));
        e.tap_pad();
      }
      const double want = 60000.0 * c.first / (500.0 * c.first + skew);
      CHECK(std::fabs(want - 120.0) <= c.second + 0.1);  // the arithmetic of the claim: about +-1 / +-2 BPM
      char text[24];
      std::snprintf(text, sizeof text, "%.1f BPM", want);
      CHECK(has(e.draw(), text));
    }
  }
  // reading the clock at the draw does not move a tap
  App b;
  NEED(b.ok);
  b.api.now = 10;
  b.tap_pad();
  b.api.now = 510;
  for (int i = 0; i < 40; ++i) {
    b.api.now += 1;
    b.draw();
  }
  b.api.now = 510;
  b.tap_pad();
  CHECK(has(b.draw(), "120.0 BPM"));
  // F1 taps like a pad
  App c;
  NEED(c.ok);
  c.api.now = 0;
  c.press(O3M_BTN_F1);
  c.api.now = 600;
  c.press(O3M_BTN_F1);
  CHECK(has(c.draw(), "100.0 BPM"));
  // other keys do not tap
  App d;
  NEED(d.ok);
  d.api.now = 0;
  d.press(O3M_BTN_PLAY);
  d.api.now = 500;
  d.press(O3M_BTN_PLAY);
  d.event(O3M_EV_BUTTON, O3M_BTN_F1, 0);  // a release is no tap
  CHECK(has(d.draw(), "TAPS 0  MANUAL  1/2"));
}

TEST_CASE("tapcalc app: a pause of more than 2 s starts a new series and keeps the last tempo; exactly 2 s does not; Delete forgets the taps") {
  App a;
  NEED(a.ok);
  a.api.now = 100;
  a.taps(5, 500);
  CHECK(has(a.draw(), "120.0 BPM"));
  CHECK(bpm_color(a.draw()) == O3M_COLOR_ACCENT);  // a tap under 2 s ago: orange
  a.api.now += 2000;                                // exactly 2 s after the last tap: still the same series
  CHECK(bpm_color(a.draw()) == O3M_COLOR_ACCENT);
  a.tap_pad();
  CHECK(starts(a.draw(), "TAPS 6  TAP"));
  CHECK(has(a.draw(), "75.0 BPM"));  // the 2000 ms interval is in the mean: (4 x 500 + 2000) / 5 = 800 ms
  a.api.now += 2001;
  CHECK(bpm_color(a.draw()) == O3M_COLOR_TEXT);  // no longer live, the tempo is kept
  CHECK(has(a.draw(), "75.0 BPM"));
  a.tap_pad();  // 2001 ms after the last: a fresh series of one tap, the shown tempo stands
  const Drawing d = a.draw();
  CHECK(starts(d, "TAPS 1  TAP"));
  CHECK(has(d, "75.0 BPM"));
  a.press(O3M_BTN_DELETE);
  CHECK(starts(a.draw(), "TAPS 0  MANUAL"));
  // the clock wraps inside a series
  App w;
  NEED(w.ok);
  w.api.now = 0xFFFFFFFFu - 1000u;
  w.taps(9, 500);
  CHECK(has(w.draw(), "120.0 BPM"));
}

TEST_CASE("tapcalc app: the Average parameter changes the mean, clamped, NaN to the lower bound") {
  App a;
  NEED(a.ok);
  // nine taps; the gaps are 500 ms four times, then 250 ms four times
  a.api.now = 0;
  a.tap_pad();
  for (std::uint32_t g : {500u, 500u, 500u, 500u, 250u, 250u, 250u, 250u}) {
    a.api.now += g;
    a.tap_pad();
  }
  CHECK(has(a.draw(), "160.0 BPM"));  // AVERAGE 8 (the default): all eight intervals, 375 ms
  a.inst.set(kAverage, 6.0f);
  CHECK(has(a.draw(), "180.0 BPM"));  // AVERAGE 6: (2 x 500 + 4 x 250) / 6 = 333.3 ms
  a.inst.set(kAverage, 4.0f);
  CHECK(has(a.draw(), "240.0 BPM"));  // the last four intervals: 250 ms
  a.inst.set(kAverage, 8.0f);
  CHECK(has(a.draw(), "160.0 BPM"));  // all eight: 375 ms
  a.inst.set(kAverage, 99.0f);        // clamped to 8
  CHECK(has(a.draw(), "160.0 BPM"));
  a.inst.set(kAverage, std::nanf(""));  // NaN: the lower bound, 4
  CHECK(has(a.draw(), "240.0 BPM"));
  a.inst.set(kAverage, -3.0f);
  CHECK(has(a.draw(), "240.0 BPM"));
}

TEST_CASE("tapcalc app: manual BPM from the encoder (0.1 a detent, Shift + the encoder 1.0), clamped; Up / Down choose the row; Left / Right turn the page to Hz (OS-APPUX S3)") {
  App a;
  NEED(a.ok);
  const auto shifted = [&](std::int32_t detents) {
    const o3m_ui_event ev = {O3M_EV_ENCODER, 0, detents, O3M_MOD_SHIFT};
    CHECK(a.views.call_input(*a.lib.entry, a.inst.state, 1, ev) == ViewStatus::Ok);
  };
  a.event(O3M_EV_ENCODER, 0, 30);  // BPM row: 0.1 a detent
  CHECK(has(a.draw(), "123.0 BPM"));
  shifted(3);                      // 1.0 a detent
  CHECK(has(a.draw(), "126.0 BPM"));
  a.event(O3M_EV_ENCODER, 0, -300);
  CHECK(has(a.draw(), "96.0 BPM"));
  a.event(O3M_EV_ENCODER, 0, -100000);
  CHECK(has(a.draw(), "20.0 BPM"));
  a.event(O3M_EV_ENCODER, 0, 100000);
  CHECK(has(a.draw(), "300.0 BPM"));
  shifted(-180);  // 120.0
  const Drawing ms = a.draw();
  CHECK(has(ms, "120.0 BPM"));
  CHECK(has(ms, "MS"));
  CHECK(has(ms, "2000.0"));  // 1/1
  CHECK(has(ms, "62.5"));    // 1/32
  CHECK(has(ms, "166.7"));   // 1/8 triplet
  CHECK(has(ms, "750.0"));   // dotted 1/4
  a.event(O3M_EV_BUTTON, O3M_BTN_UP, 1);
  a.event(O3M_EV_BUTTON, O3M_BTN_UP, 0);  // a release does nothing
  CHECK(has(a.draw(), "120.0 BPM"));      // Up / Down no longer move the BPM: they choose the row
  a.event(O3M_EV_BUTTON, O3M_BTN_RIGHT, 1);
  const Drawing hz = a.draw();
  CHECK(has(hz, "HZ"));
  CHECK(has(hz, "2.00"));   // 1/4 = 2 Hz
  CHECK(has(hz, "16.00"));  // 1/32 = 16 Hz
  CHECK(has(hz, "TAPS 0  MANUAL  2/2"));
  a.event(O3M_EV_BUTTON, O3M_BTN_RIGHT, 1);  // (no wrap: the last page stays)
  CHECK(has(a.draw(), "HZ"));
  a.event(O3M_EV_BUTTON, O3M_BTN_LEFT, 1);
  CHECK(has(a.draw(), "MS"));
  // a manual change forgets the taps
  a.api.now = 0;
  a.taps(5, 500);
  CHECK(starts(a.draw(), "TAPS 5  TAP"));
  a.event(O3M_EV_ENCODER, 0, 1);
  CHECK(starts(a.draw(), "TAPS 0  MANUAL"));
}

TEST_CASE("tapcalc app: BPM and AVERAGE are the page's two rows, kept with set_own_param; Delete is the default (AVERAGE 8, BPM 120.0 and the taps forgotten); the pads and F1 still tap (OS-APPUX S3)") {
  App a;
  NEED(a.ok);
  CHECK(a.api.own_params.empty());
  a.event(O3M_EV_ENCODER, 0, 25);  // BPM 122.5
  CHECK(has(a.draw(), "122.5 BPM"));
  NEED(a.api.own_params.size() == 1u);
  CHECK((a.api.own_params[0].index == 1u && a.api.own_params[0].value == 1225.0f));  // declared second: AVERAGE kept its place
  a.event(O3M_EV_BUTTON, O3M_BTN_DOWN, 1);   // the AVERAGE row
  a.event(O3M_EV_ENCODER, 0, -3);
  CHECK((a.api.own_params.size() == 2u && a.api.own_params[1].index == 0u && a.api.own_params[1].value == 5.0f));
  a.event(O3M_EV_ENCODER, 0, -10);  // stops at 4
  CHECK(a.api.own_params.back().value == 4.0f);
  a.event(O3M_EV_BUTTON, O3M_BTN_DELETE, 1);
  CHECK((a.api.own_params.back().index == 0u && a.api.own_params.back().value == 8.0f));
  a.event(O3M_EV_BUTTON, O3M_BTN_UP, 1);  // the BPM row again
  a.api.now = 0;
  a.taps(5, 500);  // a pad tap is still a tap: 120 BPM, kept
  CHECK(starts(a.draw(), "TAPS 5  TAP"));
  CHECK(a.api.own_params.back().index == 1u);
  a.event(O3M_EV_BUTTON, O3M_BTN_DELETE, 1);  // the default and the taps forgotten
  CHECK(starts(a.draw(), "TAPS 0  MANUAL"));
  CHECK(has(a.draw(), "120.0 BPM"));
  CHECK((a.api.own_params.back().index == 1u && a.api.own_params.back().value == 1200.0f));
  // the OS hands back the kept rows before enter(), which forgets the taps and leaves the rows alone
  a.inst.set(kAverage, 6.0f);
  a.inst.set(1, 1875.0f);  // BPM 187.5
  a.inst.m->ui->enter(a.inst.state);
  CHECK(has(a.draw(), "187.5 BPM"));
  CHECK(starts(a.draw(), "TAPS 0  MANUAL"));
  a.inst.set(1, 5.0f);  // below the range: the floor
  CHECK(has(a.draw(), "20.0 BPM"));
}

TEST_CASE("tapcalc app: the PROJECT line follows the project's tempo, F2 sets it through the edit table and says so for 2.5 s") {
  App a;
  NEED(a.ok);
  a.api.tempo = 9850;
  CHECK(has(a.draw(), "PROJECT 98.50 BPM"));
  a.api.now = 100;
  a.taps(9, 500);
  CHECK(a.api.tempo_calls.empty());  // tapping alone changes nothing in the project
  a.press(O3M_BTN_F2);
  NEED(a.api.tempo_calls.size() == 1u);
  CHECK(a.api.tempo_calls[0] == 12000u);  // 120.0 BPM x 10 = BPM x 100
  const Drawing d = a.draw();
  CHECK(has(d, "SET 120.0 BPM  (UNDO TO TAKE BACK)"));
  CHECK(has(d, "PROJECT 120.00 BPM"));  // the fake applies at once; the real host queues and get_tempo returns the queued value
  a.api.now += 2499;
  CHECK(has(a.draw(), "SET 120.0 BPM  (UNDO TO TAKE BACK)"));
  a.api.now += 2;
  CHECK(!starts(a.draw(), "SET "));
  // the tenths survive: 133.3 -> 13330
  a.api.now = 5;
  App b;
  NEED(b.ok);
  b.api.now = 5;
  b.taps(9, 450);
  b.press(O3M_BTN_F2);
  NEED(b.api.tempo_calls.size() == 1u);
  CHECK(b.api.tempo_calls[0] == 13330u);  // 133.3 BPM
  // a manual value
  App c;
  NEED(c.ok);
  c.event(O3M_EV_ENCODER, 0, 51);  // +5.1 BPM (0.1 a detent)
  c.press(O3M_BTN_F2);
  NEED(c.api.tempo_calls.size() == 1u);
  CHECK(c.api.tempo_calls[0] == 12510u);  // 125.1 BPM
  // the message survives a clock wrap
  App w;
  NEED(w.ok);
  w.api.now = 0xFFFFFF00u;
  w.press(O3M_BTN_F2);
  w.api.now += 2000;  // past the wrap, 500 ms before the answer goes
  CHECK(has(w.draw(), "SET 120.0 BPM  (UNDO TO TAKE BACK)"));
  w.api.now += 600;
  CHECK(!starts(w.draw(), "SET "));
}

TEST_CASE("tapcalc app: F2 says why it did not set the tempo") {
  {
    App a;
    NEED(a.ok);
    a.event(O3M_EV_ENCODER, 0, -950);  // 25.0 BPM: the app allows it, the project (40 - 800) does not
    a.api.set_tempo_result = O3M_EDIT_RANGE;
    a.press(O3M_BTN_F2);
    NEED(a.api.tempo_calls.size() == 1u);
    CHECK(a.api.tempo_calls[0] == 2500u);
    CHECK(has(a.draw(), "NOT SET: THE PROJECT TAKES 40 - 800 BPM"));
  }
  {
    App a;
    NEED(a.ok);
    a.api.set_tempo_result = O3M_EDIT_NO_PROJECT;
    a.api.tempo = 0;
    CHECK(has(a.draw(), "NO PROJECT"));
    a.press(O3M_BTN_F2);
    CHECK(has(a.draw(), "NOT SET: NO PROJECT"));
  }
  {
    App a(O3M_CAP_READ_PROJECT);  // asked for no edit capability: no edit table
    NEED(a.ok);
    a.press(O3M_BTN_F2);
    CHECK(a.api.tempo_calls.empty());
    CHECK(has(a.draw(), "NOT SET: THIS APP CANNOT EDIT"));
    CHECK(starts(a.draw(), "PROJECT 120.00"));
  }
  {
    App a(O3M_CAP_EDIT_PROJECT);  // no read table: the project line says it does not know; F2 still works
    NEED(a.ok);
    CHECK(has(a.draw(), "PROJECT TEMPO UNKNOWN"));
    a.press(O3M_BTN_F2);
    CHECK(a.api.tempo_calls.size() == 1u);
  }
  {
    App a(0);  // neither: the clock only
    NEED(a.ok);
    a.press(O3M_BTN_F2);
    CHECK(has(a.draw(), "NOT SET: THIS APP CANNOT EDIT"));
    CHECK(has(a.draw(), "PROJECT TEMPO UNKNOWN"));
  }
}

TEST_CASE("tapcalc app: every page draws inside the draw API's budgets at the page size, at the smallest size and at the extreme tempi, with the answer to F2 on screen") {
  App a;
  NEED(a.ok);
  // the page body is about 490 x 384 under the parameter column; the manifest's minimum is 480 x 288
  const int sizes[][2] = {{490, 384}, {500, 384}, {480, 288}, {800, 480}, {16, 16}};
  a.api.tempo = 80000;
  for (const auto& sz : sizes) {
    for (std::uint32_t page = 0; page < 2; ++page) {
      for (std::int32_t bpm : {-50000, 0, 50000}) {
        a.event(O3M_EV_ENCODER, 0, bpm == 0 ? 1000 : bpm);  // 20 BPM, 120, 300
        for (int result : {O3M_EDIT_OK, O3M_EDIT_RANGE, O3M_EDIT_NO_PROJECT}) {
          a.api.set_tempo_result = result;
          a.press(O3M_BTN_F2);
          const Drawing d = a.draw(sz[0], sz[1], 700);  // one page's 700 display-list words, all of them
          CHECK((sz[0] < 100 || !d.texts.empty()));     // (a canvas of 16 x 16 shows nothing: every text is cut off, and that is no fault)
        }
      }
      a.event(O3M_EV_BUTTON, O3M_BTN_RIGHT, 1);
    }
  }
  // the longest texts together: 1/1 dotted at 20 BPM, 18000.0 ms; the widest project tempo; the widest answer
  a.event(O3M_EV_BUTTON, O3M_BTN_LEFT, 1);  // (back to the ms page: Left / Right do not wrap)
  a.event(O3M_EV_ENCODER, 0, -50000);
  a.api.set_tempo_result = O3M_EDIT_RANGE;
  a.press(O3M_BTN_F2);
  const Drawing d = a.draw(490, 384, 450);  // the budget the manifest aims at
  CHECK(has(d, "18000.0"));
  CHECK(d.texts.size() == 36u);  // nothing is cut off at the page size: tempo + status, MS + 3 heads, 6 x (label + 3 values), the project line, the answer, the two rows (a name and a value each)
  std::size_t chars = 0;
  for (const auto& t : d.texts) {
    chars += t.s.size();
    CHECK(t.s.size() <= 64u);
  }
  CHECK(chars <= O3M_VIEW_MAX_TEXT);
}

TEST_CASE("tapcalc: the packed .tmod passes the loader's checks, asks for read and edit, has no hot code and is small") {
  const Tmod t = load_tmod(O3M_TAPCALC_TMOD);
  if (!t.present) {
    return;
  }
  CHECK(t.header == Status::Ok);
  CHECK(t.crc_ok);
  CHECK(t.relocation == Status::Ok);
  CHECK(t.descriptor == Status::Ok);
  CHECK(t.h.kind == O3M_KIND_APP);
  CHECK(t.h.abi_minor == 10u);
  CHECK(t.h.reserved2[0] == (O3M_CAP_READ_PROJECT | O3M_CAP_EDIT_PROJECT));
  CHECK(t.h.module_version == O3M_VERSION(1, 0, 0));
  CHECK(t.h.param_count == 2u);
  CHECK(t.h.view_count == 1u);
  CHECK(t.h.hot_size == 0u);
  CHECK(t.h.state_bytes <= 256u);
  CHECK(t.file_bytes <= 4096u);
  MESSAGE("tapcalc.tmod: " << t.file_bytes << " B file, hot " << t.h.hot_size << " B, cold " << (t.h.image_size - t.h.hot_size) << " B, state " << t.h.state_bytes << " B");
}
