// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-H2b (new Community OS behaviour): the Tuner app driven through the ABI as the OS drives it, on a fake of the ABI 1.4 host tables (o3m_test::FakeApi: tap_stream answers the chunks the test
// queues, one a 33 ms frame): the descriptor, an OS that lends no tap, the tap point, the pitch of synthetic sines (55, 110, 440, 1000, 1760 Hz and the ends of the range) within 2 cents, the
// A4 reference, harmonic-rich waves with no octave error, silence / DC / noise / a quiet or out-of-range tone giving NO SIGNAL, a held note staying put under noise, the note hysteresis, a
// `lost` gap and a stalled tap restarting the window, a hostile host, the clock wrapping, the draw budgets, and the packed .tmod. Host only.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../../../tests/o3m_test_support.h"

#include <chrono>
#include <cmath>
#include <functional>
#include <string>

using namespace o3m_test;
using namespace o3os::module;

namespace {

constexpr std::uint32_t kA4 = 0, kSource = 1;
constexpr std::uint32_t kFrame = 1456;  // input frames of a 33 ms app frame at 44.1 kHz (even)

using Wave = std::function<double(std::uint64_t)>;  // a sample, in int16 units, at the frame index

Wave sine(double hz, double amp, double phase = 0.3) {
  return [=](std::uint64_t i) { return amp * std::sin(2.0 * kPi * hz * static_cast<double>(i) / 44100.0 + phase); };
}
Wave saw(double hz, double amp) {
  return [=](std::uint64_t i) {
    const double ph = std::fmod(hz * static_cast<double>(i) / 44100.0, 1.0);
    return amp * (2.0 * ph - 1.0);
  };
}
Wave square(double hz, double amp) {
  return [=](std::uint64_t i) { return std::fmod(hz * static_cast<double>(i) / 44100.0, 1.0) < 0.5 ? amp : -amp; };
}
Wave noise(double amp) {
  return [=](std::uint64_t i) {  // a deterministic hash of the index: white noise, uniform
    std::uint64_t x = i * 0x9E3779B97F4A7C15ull + 0x1234567ull;
    x ^= x >> 29;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 32;
    return amp * (static_cast<double>(x & 0xFFFF) / 32768.0 - 1.0);
  };
}
Wave mix(Wave a, Wave b) {
  return [=](std::uint64_t i) { return a(i) + b(i); };
}
Wave dc(double v) {
  return [=](std::uint64_t) { return v; };
}
Wave clipped(Wave w) {
  return [=](std::uint64_t i) { return std::fmax(-32768.0, std::fmin(32767.0, w(i))); };
}

struct Tuner {
  Lib lib;
  Instance inst;
  FakeApi api;
  ViewHost views;
  bool ok = false;
  float p[2] = {440.0f, 1.0f};
  std::uint64_t at = 0;  // frames of the wave fed so far
  explicit Tuner(std::uint32_t caps = O3M_CAP_AUDIO_TAP) {
    ViewHost::Metrics m;
    const std::int16_t cw[O3M_FACE_COUNT] = {16, 10, 8, 6}, ch[O3M_FACE_COUNT] = {32, 20, 16, 12};
    for (unsigned f = 0; f < O3M_FACE_COUNT; ++f) {
      m.cell_w[f] = cw[f];
      m.cell_h[f] = ch[f];
    }
    views.set_metrics(m);
    ok = lib.open(O3M_TUNER_HOST_SO) && inst.start_with(*lib.entry, make_app_host(api, caps));
    if (ok) {
      inst.m->ui->enter(inst.state);
    }
  }
  void set(std::uint32_t id, float v) {
    inst.set(id, v);
    p[id] = inst.values[id];
  }
  void event(std::uint32_t kind, std::uint32_t id, std::int32_t value, std::uint32_t mods = 0) {
    const o3m_ui_event ev = {kind, id, value, mods};
    CHECK(views.call_input(*lib.entry, inst.state, 1, ev) == ViewStatus::Ok);
  }
  Drawing draw(int w = 490, int h = 384, std::uint32_t max_words = kViewMaxWords) {
    Drawing d;
    CHECK(views.draw(*lib.entry, inst.state, 1, 0, w, h, p, 2, max_words) == ViewStatus::Ok);
    views.replay(d);
    return d;
  }
  // Queues `frames` frames of the wave as one chunk (the tap's backlog of one app frame).
  void queue(const Wave& w, std::uint32_t frames = kFrame, std::uint32_t lost = 0) {
    FakeApi::TapChunk c;
    c.lost = lost;
    c.s.resize(frames);
    for (std::uint32_t i = 0; i < frames; ++i, ++at) {
      c.s[i] = static_cast<std::int16_t>(std::lrint(std::fmax(-32768.0, std::fmin(32767.0, w(at)))));
    }
    api.tap_chunks.push_back(std::move(c));
  }
  // One 33 ms app frame carrying the wave.
  Drawing frame(const Wave& w, std::uint32_t ms = 33) {
    api.now += ms;
    queue(w);
    return draw();
  }
  Drawing run(const Wave& w, int frames) {
    Drawing d;
    for (int i = 0; i < frames; ++i) {
      d = frame(w);
    }
    return d;
  }
};

// The two rows of the page as "A4 440 HZ   INPUT" (o3edit::draw_rows: a name at the left, its value on the same line).
std::string row_text(const Drawing& d, const std::string& name) {
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
std::string rows_of(const Drawing& d) { return "A4 " + row_text(d, "A4") + "   " + row_text(d, "SOURCE"); }
bool has(const Drawing& d, const std::string& s) {
  for (const auto& t : d.texts) {
    if (t.s == s) {
      return true;
    }
  }
  return false;
}
std::string starts(const Drawing& d, const std::string& prefix) {
  for (const auto& t : d.texts) {
    if (t.s.rfind(prefix, 0) == 0) {
      return t.s;
    }
  }
  return "";
}
// The note name: the Display face text under the title that is not the title or NO SIGNAL.
std::string note_of(const Drawing& d) {
  for (const auto& t : d.texts) {
    if (t.face == O3M_FACE_DISPLAY && t.s != "TUNER") {
      return t.s;
    }
  }
  return "";
}
bool no_signal(const Drawing& d) { return has(d, "NO SIGNAL"); }
double hz_of(const Drawing& d) { return std::atof(starts(d, "FREQ ").c_str() + 5); }
int cents_of(const Drawing& d) { return std::atoi(starts(d, "CENTS ").c_str() + 6); }

struct Expect {
  std::string name;
  double cents;  // exact
};
// What a perfect tuner shows for `hz` against `a4`.
Expect expect(double hz, double a4 = 440.0) {
  const double x = 69.0 + 12.0 * std::log2(hz / a4);
  const long n = std::lround(x);
  static const char* names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  return Expect{std::string(names[((n % 12) + 12) % 12]) + std::to_string(n / 12 - 1), (x - static_cast<double>(n)) * 100.0};
}

// Runs a wave until the reading settles and checks the note and the cents (the readout is a whole number of cents: it may differ from the exact figure by the 0.5 of its rounding).
void check_tone(Tuner& t, const Wave& w, double hz, double a4 = 440.0, int frames = 40) {
  const Drawing d = t.run(w, frames);
  const Expect e = expect(hz, a4);
  CHECK_MESSAGE(!no_signal(d), "NO SIGNAL at " << hz << " Hz");
  CHECK_MESSAGE(note_of(d) == e.name, hz << " Hz shows " << note_of(d) << ", expected " << e.name);
  CHECK_MESSAGE(std::abs(cents_of(d) - e.cents) <= 1.0, hz << " Hz: " << cents_of(d) << " cents, expected " << e.cents);
  CHECK_MESSAGE(std::abs(hz_of(d) - hz) <= 0.06 + hz * 0.0003, hz << " Hz reads " << hz_of(d));
}

}  // namespace

TEST_CASE("tuner: descriptor, capability and an OS that lends no draw table") {
  Tuner a;
  NEED(a.ok);
  const o3m_module& m = *a.lib.entry;
  CHECK(m.kind == O3M_KIND_APP);
  CHECK(std::string(m.name) == "Tuner");
  CHECK((m.abi_major == 1u && m.abi_minor == 10u));  // (1.10: A4 and SOURCE are the app's own rows, with an input() of their own, OS-APPUX S3)
  CHECK(m.view_count == 1u);
  CHECK((m.render == nullptr && m.note_on == nullptr && m.note_off == nullptr));
  CHECK(m.param_count == 2u);
  CHECK(std::string(m.params[0].name) == "A4");
  CHECK((m.params[0].min == 415.0f && m.params[0].max == 466.0f && m.params[0].def == 440.0f && (m.params[0].flags & O3M_PARAM_INT) != 0u));
  CHECK(O3M_PARAM_UNIT(m.params[0].flags) == O3M_UNIT_HZ);
  CHECK(std::string(m.params[1].name) == "Source");
  CHECK((m.params[1].min == 1.0f && m.params[1].max == 2.0f && m.params[1].def == 1.0f && (m.params[1].flags & O3M_PARAM_INT) != 0u));
  alignas(16) static unsigned char state[65536] = {};
  o3m_host old = a.inst.host;
  old.draw = nullptr;
  CHECK(m.init(state, &old) != 0);
}

TEST_CASE("tuner: an OS that lends no tap says so, whatever table is missing") {
  {
    Tuner a(0);
    NEED(a.ok);
    CHECK(has(a.draw(), "NO AUDIO TAP FOR THIS APP"));
    CHECK(a.api.tap_selects.empty());
  }
  Tuner a;
  NEED(a.ok);
  o3m_io_api v13 = a.api.io;  // an OS of ABI 1.3: its io table ends at midi_lost
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
  o3m_host v12 = a.inst.host;  // ABI 1.2: no io table
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

TEST_CASE("tuner: the tap point defaults to the input and follows the parameter; the A4 reference is clamped") {
  Tuner a;
  NEED(a.ok);
  CHECK(rows_of(a.draw()) == "A4 440 HZ   INPUT");
  NEED(a.api.tap_selects.size() == 1u);
  CHECK(a.api.tap_selects[0] == O3M_TAP_INPUT);
  (void)a.draw();
  CHECK(a.api.tap_selects.size() == 1u);
  a.set(kSource, 2.0f);
  CHECK(rows_of(a.draw()) == "A4 440 HZ   MASTER");
  CHECK(a.api.tap_selects.back() == O3M_TAP_MASTER);
  a.set(kA4, 100.0f);
  CHECK(rows_of(a.draw()) == "A4 415 HZ   MASTER");
  a.set(kA4, 9000.0f);
  CHECK(rows_of(a.draw()) == "A4 466 HZ   MASTER");
  a.set(kA4, std::nanf(""));
  CHECK(rows_of(a.draw()) == "A4 415 HZ   MASTER");
  a.set(kA4, 432.4f);
  CHECK(rows_of(a.draw()) == "A4 432 HZ   MASTER");
}

TEST_CASE("tuner: sines at 55, 110, 440, 1000 and 1760 Hz read within 2 cents (and the ends of the range)") {
  for (const double hz : {55.0, 110.0, 440.0, 1000.0, 1760.0, 30.87, 82.41, 261.63, 1318.51, 2000.0}) {
    Tuner a;
    NEED(a.ok);
    check_tone(a, sine(hz, 12000.0), hz);
  }
  // the needle and the confidence: 440 Hz is in tune, the needle at the middle and the colour of an A4 that is in tune
  Tuner a;
  NEED(a.ok);
  const Drawing d = a.run(sine(440.0, 12000.0), 40);
  CHECK(note_of(d) == "A4");
  CHECK(has(d, "CENTS 0"));
  bool needle = false;
  for (const auto& r : d.rects) {
    if (r.w == 5 && r.h == 52 && r.color == O3M_COLOR_PLAY) {
      needle = needle || std::abs(r.x + 2 - (12 + (490 - 24) / 2)) <= 3;
    }
  }
  CHECK(needle);
  CHECK(std::atoi(starts(d, "FREQ ").c_str() + 5) == 440);
  CHECK(starts(d, "FREQ ") == "FREQ 440.0 HZ");
  std::string conf;
  for (const auto& t : d.texts) {
    if (t.s.size() > 2u && t.s.compare(t.s.size() - 2u, 2u, " %") == 0) {
      conf = t.s;
    }
  }
  CHECK((conf == "99 %" || conf == "100 %" || conf == "98 %"));
}

TEST_CASE("tuner: the A4 reference moves the note arithmetic") {
  {
    Tuner a;
    NEED(a.ok);
    a.set(kA4, 432.0f);
    check_tone(a, sine(432.0, 10000.0), 432.0, 432.0);  // A4 at 432: dead centre
  }
  {
    Tuner a;
    NEED(a.ok);
    a.set(kA4, 432.0f);
    const Drawing d = a.run(sine(440.0, 10000.0), 40);  // 440 Hz is 32 cents sharp of A4 = 432
    CHECK(note_of(d) == "A4");
    CHECK(std::abs(cents_of(d) - 32) <= 1);
  }
  {
    Tuner a;
    NEED(a.ok);
    a.set(kA4, 415.0f);
    check_tone(a, sine(415.3, 10000.0), 415.3, 415.0);
  }
  {
    Tuner a;
    NEED(a.ok);
    a.set(kA4, 466.0f);
    check_tone(a, sine(220.0, 10000.0), 220.0, 466.0);  // 220 against A4 = 466: a flat A#3
  }
  {
    Tuner a;  // the reference can change while a note is held: the reading re-reads, no new analysis is needed
    NEED(a.ok);
    Drawing d = a.run(sine(440.0, 10000.0), 40);
    CHECK(note_of(d) == "A4");
    a.set(kA4, 432.0f);
    d = a.frame(sine(440.0, 10000.0));
    CHECK(std::abs(cents_of(d) - 32) <= 1);
  }
}

TEST_CASE("tuner: harmonic-rich notes read their fundamental, not an octave away") {
  for (const double hz : {82.41, 110.0, 220.0, 440.0, 880.0}) {
    Tuner a;
    NEED(a.ok);
    check_tone(a, saw(hz, 12000.0), hz);
  }
  for (const double hz : {110.0, 330.0}) {
    Tuner a;
    NEED(a.ok);
    check_tone(a, square(hz, 20000.0), hz);
  }
  // a note whose second harmonic is louder than the fundamental (a guitar's open string, a reed)
  for (const double hz : {110.0, 196.0}) {
    Tuner a;
    NEED(a.ok);
    const Wave w = [=](std::uint64_t i) { return 4000.0 * std::sin(2.0 * kPi * hz * static_cast<double>(i) / 44100.0) + 9000.0 * std::sin(4.0 * kPi * hz * static_cast<double>(i) / 44100.0 + 1.0); };
    check_tone(a, w, hz);
  }
  // a full-scale square and a hard-clipped sine
  {
    Tuner a;
    NEED(a.ok);
    check_tone(a, square(220.0, 32767.0), 220.0);
  }
  {
    Tuner a;
    NEED(a.ok);
    check_tone(a, clipped(sine(146.83, 90000.0)), 146.83);
  }
  // an offset: the DC is removed
  {
    Tuner a;
    NEED(a.ok);
    check_tone(a, mix(sine(440.0, 8000.0), dc(15000.0)), 440.0);
  }
}

TEST_CASE("tuner: silence, DC, noise, a quiet tone and a tone out of range are NO SIGNAL") {
  const Wave quiet = sine(440.0, 60.0);  // -55 dBFS peak
  const Wave low_hum = sine(20.0, 12000.0);
  const Wave below = sine(27.0, 12000.0);   // under the 30 Hz floor of the longest lag searched: refused, not read as 30.0 Hz
  const Wave edge = sine(29.0, 12000.0);
  const Wave shrill = sine(5000.0, 12000.0);
  const std::pair<const char*, Wave> cases[] = {{"silence", dc(0.0)},          {"DC", dc(20000.0)},         {"full-scale DC", dc(32767.0)}, {"noise", noise(8000.0)},
                                                {"loud noise", noise(32000.0)}, {"quiet tone", quiet},       {"20 Hz hum", low_hum},         {"5 kHz", shrill},
                                                {"27 Hz", below},               {"29 Hz", edge},
                                                {"4 kHz", sine(4000.0, 12000.0)}};
  for (const auto& c : cases) {
    Tuner a;
    NEED(a.ok);
    const Drawing d = a.run(c.second, 40);
    CHECK_MESSAGE(no_signal(d), std::string(c.first) << " must be NO SIGNAL");
    CHECK_MESSAGE(has(d, "CENTS --"), std::string(c.first));
    CHECK_MESSAGE(has(d, "FREQ --"), std::string(c.first));
    for (const auto& r : d.rects) {
      CHECK(!(r.w == 5 && r.color == O3M_COLOR_ACCENT));  // no needle
    }
  }
  // the same tone just above the level threshold reads
  Tuner a;
  NEED(a.ok);
  check_tone(a, sine(440.0, 400.0), 440.0);
  // a tone with a lot of noise on it still reads (10 dB signal-to-noise)
  Tuner b;
  NEED(b.ok);
  check_tone(b, mix(sine(440.0, 8000.0), noise(2600.0)), 440.0, 440.0, 60);
}

TEST_CASE("tuner: a held note does not jitter under noise, and the note name has a hysteresis") {
  Tuner a;
  NEED(a.ok);
  const Wave w = mix(sine(440.0, 8000.0), noise(900.0));
  (void)a.run(w, 30);
  int lo = 99, hi = -99;
  for (int i = 0; i < 120; ++i) {
    const Drawing d = a.frame(w);
    CHECK(note_of(d) == "A4");
    lo = std::min(lo, cents_of(d));
    hi = std::max(hi, cents_of(d));
  }
  CHECK(hi - lo <= 3);
  // a pitch that creeps past the half-way point (50 cents) keeps the old name until it is 58 cents away
  Tuner b;
  NEED(b.ok);
  const double up45 = 440.0 * std::pow(2.0, 45.0 / 1200.0), up54 = 440.0 * std::pow(2.0, 54.0 / 1200.0), up66 = 440.0 * std::pow(2.0, 66.0 / 1200.0);
  Drawing d = b.run(sine(up45, 9000.0), 40);
  CHECK(note_of(d) == "A4");
  CHECK(std::abs(cents_of(d) - 45) <= 1);
  d = b.run(sine(up54, 9000.0), 40);
  CHECK(note_of(d) == "A4");
  CHECK(std::abs(cents_of(d) - 54) <= 1);
  d = b.run(sine(up66, 9000.0), 40);
  CHECK(note_of(d) == "A#4");
  CHECK(std::abs(cents_of(d) - -34) <= 1);
  // a cold start at 54 cents has no old note to keep: it is A#4
  Tuner c;
  NEED(c.ok);
  d = c.run(sine(up54, 9000.0), 40);
  CHECK(note_of(d) == "A#4");
}

TEST_CASE("tuner: a note change settles within a second; a gap, a source change and a stalled tap restart the window") {
  Tuner a;
  NEED(a.ok);
  Drawing d = a.run(sine(440.0, 10000.0), 40);
  CHECK(note_of(d) == "A4");
  d = a.run(sine(329.63, 10000.0), 30);  // 1 s
  CHECK(note_of(d) == "E4");
  // a `lost` gap: the next draw has no stale note, and the new tone reads once the window is whole again
  a.api.now += 33;
  a.queue(sine(110.0, 10000.0), kFrame, 3000);
  d = a.draw();
  CHECK(no_signal(d));
  d = a.run(sine(110.0, 10000.0), 40);
  CHECK(note_of(d) == "A2");
  // a source change restarts too
  a.set(kSource, 2.0f);
  d = a.frame(sine(110.0, 10000.0));
  CHECK(no_signal(d));
  CHECK(a.api.tap_selects.back() == O3M_TAP_MASTER);
  d = a.run(sine(110.0, 10000.0), 40);
  CHECK(note_of(d) == "A2");
  // frames == 0: nothing new for a while (no chunk queued) keeps the note for a moment, then drops it
  for (int i = 0; i < 8; ++i) {
    a.api.now += 33;
    d = a.draw();
  }
  CHECK(note_of(d) == "A2");
  for (int i = 0; i < 12; ++i) {
    a.api.now += 33;
    d = a.draw();
  }
  CHECK(no_signal(d));
  // and carries on when the samples come back
  d = a.run(sine(110.0, 10000.0), 40);
  CHECK(note_of(d) == "A2");
}

TEST_CASE("tuner: short, odd and huge chunks, a hostile host and the clock wrapping") {
  Tuner a;
  NEED(a.ok);
  a.api.now = 0xFFFFFF00u;
  Drawing d;
  for (int i = 0; i < 60; ++i) {  // 33 ms frames across the wrap of the clock, chunks of every size
    a.api.now += 33;
    const std::uint32_t n = i % 5 == 0 ? 64u : (i % 7 == 0 ? 2046u : kFrame);
    a.queue(sine(440.0, 10000.0), n);
    d = a.draw();
  }
  CHECK(note_of(d) == "A4");
  CHECK(std::abs(cents_of(d)) <= 1);
  // a chunk of 3 samples, an empty chunk, a chunk of 2048 (the most one call takes)
  a.queue(sine(440.0, 10000.0), 2);
  a.queue(sine(440.0, 10000.0), 0);
  a.queue(sine(440.0, 10000.0), 2048);
  for (int i = 0; i < 3; ++i) {
    a.api.now += 33;
    d = a.draw();
  }
  CHECK(note_of(d) == "A4");
  // a host that claims more frames than it was given or asked for: not believed, nothing breaks
  Tuner b;
  NEED(b.ok);
  b.api.tap_overreturn = 0xFFFFFFFFu;
  for (int i = 0; i < 80; ++i) {
    b.api.now += 33;
    d = b.draw();
  }
  CHECK(no_signal(d));
  b.api.tap_overreturn = 1000000u;
  d = b.draw();
  CHECK(no_signal(d));
  // full-scale alternating samples (the most violent input for the filter) and ten seconds of every wave: the arithmetic stays finite
  Tuner c;
  NEED(c.ok);
  const Wave nyq = [](std::uint64_t i) { return (i & 1u) != 0u ? 32767.0 : -32768.0; };
  d = c.run(nyq, 60);
  CHECK(no_signal(d));
  d = c.run(sine(440.0, 12000.0), 40);
  CHECK(note_of(d) == "A4");
}

TEST_CASE("tuner: the clock is checked before every chunk, so a frame is bounded; a reading still completes over later frames") {
  const Wave w = sine(440.0, 12000.0);
  struct Run {
    std::uint32_t worst_ms = 0, worst_calls = 0;
    bool engaged = false;  // a frame stopped by the guard (2 ms of the fake clock)
    Drawing last;
  };
  auto run = [&](Tuner& a, std::uint32_t tick_every, int frames) {
    Run r;
    a.api.tick_every = tick_every;
    for (int i = 0; i < frames; ++i) {
      a.api.now += 33;
      a.queue(w);
      const std::uint32_t n0 = a.api.now, c0 = a.api.now_calls;
      r.last = a.draw();
      r.worst_ms = std::max(r.worst_ms, a.api.now - n0);
      r.worst_calls = std::max(r.worst_calls, a.api.now_calls - c0);
      r.engaged = r.engaged || a.api.now - n0 >= 2u;
    }
    return r;
  };
  {  // a clock that never moves: the caps alone bound the frame (the filter's 16 chunks, 20 chunks of analysis, a few looks)
    Tuner a;
    NEED(a.ok);
    const Run r = run(a, 0, 60);
    CHECK(r.worst_calls <= 45u);
    CHECK(note_of(r.last) == "A4");
  }
  {  // a clock that runs 1 ms every 16 looks: the 2 ms guard stops the analysis before its cap, every frame stays inside it, and the readings still come, over more frames
    Tuner a;
    NEED(a.ok);
    const Run r = run(a, 16, 500);
    CHECK(r.engaged);
    CHECK(r.worst_ms <= 3u);
    CHECK(r.worst_calls <= 45u);
    CHECK(note_of(r.last) == "A4");
    CHECK(std::abs(cents_of(r.last)) <= 1);
  }
  {  // a clock that is already past the guard when the work would start (every look is a millisecond): not a chunk of filter or analysis runs, the frame costs a few looks, nothing is read
    Tuner a;
    NEED(a.ok);
    const Run r = run(a, 1, 40);
    CHECK(r.worst_calls <= 4u);
    CHECK(no_signal(r.last));
  }
  {  // a unit whose filter cannot keep up inside its millisecond: it degrades (the stream restarts as a gap, nothing is read) and every frame is still bounded; it never overruns
    Tuner a;
    NEED(a.ok);
    const Run r = run(a, 6, 200);
    CHECK(r.worst_ms <= 3u);
    CHECK(r.worst_calls <= 45u);
  }
  {  // what the filter had no time for waits for the next frame: the signal stays gapless, and the note is the same as with all the time in the world
    Tuner a;
    NEED(a.ok);
    const Run r = run(a, 40, 400);
    CHECK(r.worst_ms <= 3u);
    CHECK(note_of(r.last) == "A4");
    CHECK(std::abs(cents_of(r.last)) <= 1);
  }
}

TEST_CASE("tuner: the page draws within the budgets at any size and the host cost per frame is reported") {
  Tuner a;
  NEED(a.ok);
  for (int i = 0; i < 40; ++i) {
    (void)a.frame(sine(440.0, 12000.0));
  }
  const Drawing d490 = a.draw(490, 384, 450);
  CHECK(has(d490, "TUNER"));
  CHECK(d490.polys.empty());
  (void)a.draw(480, 288);
  (void)a.draw(800, 480);
  (void)a.draw(16, 16);
  (void)a.draw(100, 100);
  // wall-clock of a frame on this host (sanitised debug build in CI: only a coarse indication; the unit's figure is owed): the worst frame is a 33 ms frame with samples and the analysis units
  Tuner b;
  NEED(b.ok);
  double worst = 0.0, total = 0.0;
  const int frames = 120;
  for (int i = 0; i < frames; ++i) {
    b.api.now += 33;
    b.queue(sine(440.0, 12000.0));
    const auto t0 = std::chrono::steady_clock::now();
    (void)b.draw();
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    worst = std::max(worst, us);
    total += us;
  }
  MESSAGE("tuner view_draw on this host: mean " << total / frames << " us, worst " << worst << " us per 33 ms frame");
  CHECK(worst < 200000.0);  // (a sanity bound for a sanitised build, not a claim about the unit)
}

TEST_CASE("tuner: the packed .tmod passes the loader's checks, carries the audio_tap capability, has no hot code and is small") {
  const Tmod t = load_tmod(O3M_TUNER_TMOD);
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
  CHECK(t.h.state_bytes <= 40000u);
  CHECK(t.file_bytes <= 8192u);
  MESSAGE("tuner.tmod: " << t.file_bytes << " B file, hot " << t.h.hot_size << " B, cold " << (t.h.image_size - t.h.hot_size) << " B, state " << t.h.state_bytes << " B");
}

TEST_CASE("tuner: A4 and SOURCE are the page's two rows: Up / Down, the encoder, Shift + the encoder, Delete (the default), each kept with set_own_param (OS-APPUX S3)") {
  Tuner a;
  NEED(a.ok);
  CHECK(a.inst.m->ui->input != nullptr);
  CHECK(rows_of(a.draw()) == "A4 440 HZ   INPUT");
  CHECK(a.api.own_params.empty());
  a.event(O3M_EV_ENCODER, 0, 3);  // A4 443
  CHECK(rows_of(a.draw()) == "A4 443 HZ   INPUT");
  NEED(a.api.own_params.size() == 1u);
  CHECK((a.api.own_params[0].index == 0u && a.api.own_params[0].value == 443.0f));
  a.event(O3M_EV_ENCODER, 0, 2, O3M_MOD_SHIFT);  // five Hz a detent
  CHECK(rows_of(a.draw()) == "A4 453 HZ   INPUT");
  a.event(O3M_EV_ENCODER, 0, 100);  // stops at 466
  CHECK(rows_of(a.draw()) == "A4 466 HZ   INPUT");
  a.event(O3M_EV_BUTTON, O3M_BTN_DOWN, 1);  // SOURCE
  a.event(O3M_EV_ENCODER, 0, 1);
  CHECK(rows_of(a.draw()) == "A4 466 HZ   MASTER");
  CHECK((a.api.own_params.back().index == 1u && a.api.own_params.back().value == 2.0f));
  a.event(O3M_EV_BUTTON, O3M_BTN_DELETE, 1);
  CHECK(rows_of(a.draw()) == "A4 466 HZ   INPUT");
  CHECK((a.api.own_params.back().index == 1u && a.api.own_params.back().value == 1.0f));
  a.event(O3M_EV_BUTTON, O3M_BTN_UP, 1);
  a.event(O3M_EV_BUTTON, O3M_BTN_DELETE, 1);
  CHECK(rows_of(a.draw()) == "A4 440 HZ   INPUT");
  CHECK((a.api.own_params.back().index == 0u && a.api.own_params.back().value == 440.0f));
  // the tap follows the SOURCE row
  a.event(O3M_EV_BUTTON, O3M_BTN_DOWN, 1);
  a.event(O3M_EV_ENCODER, 0, 1);
  (void)a.draw();
  CHECK(a.api.tap_source == O3M_TAP_MASTER);
  // the OS hands the kept rows back before enter(), which restarts the measurement and leaves the rows alone
  a.set(0, 432.0f);
  a.set(1, 2.0f);
  a.inst.m->ui->enter(a.inst.state);
  CHECK(rows_of(a.draw()) == "A4 432 HZ   MASTER");
}
