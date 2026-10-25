// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-QANET D3 (docs/specs/OS-QANET.md section 7 and Q1; new Community OS behaviour, test tooling only): latency budgets as tests. From the key press to what the user can see or hear, on the SLOW CARD (D2:
// platform/host/slow_card.h, the PROVISIONAL defaults taken from the unit's block bench), on a FatFs image (D1) where it fits. The limits are ONE TABLE below (kBudgets); they are open research questions
// until OS-QANET D4 measures the unit (platform/mk66/tools/hil/qanet_latency.json), and the maintainer's Q1 adds two expectations that the table holds in numbers:
//   * the preview STREAMS: it starts at once and does not wait for the whole clip (preview.first_sound on a 1 s clip, preview.long_clip on a 6 s stereo clip: the same 150 ms);
//   * while F5 (the audition key of Sample load: the maintainer's "Play held", app/src/audition.cpp) is held, moving the cursor to another file plays the new file at once (preview.cursor_move: the move to the new file's first sound, 150 ms).
//
// THE MEASURES. Time is the simulator's clock in microseconds (Simulator::uptime_us(): the ticks plus the card's time, and the audio renders through a stretched pass, so the clock and the audio agree).
//   * preview.*: a render window opens at the press (the audio the engine renders from that moment, the master as the codec gets it); the budget is the first sample at or above -50 dB (`onset=` of
//     sim/audio_check.h). The cursor move is told from the file that was already playing by its pitch: the two tones' zero crossings are 2.4 ms and 0.57 ms apart, and the first crossing pair under 1 ms at a level
//     above -50 dB is the new file. The first file's onset is found first, with short windows chained back to back, and the move is made 300 ms after it, while it still sounds (the audition buffer is about 4 s);
//     a first file that never sounds is a test error, not a finding.
//   * screen.switch: the press to the first frame committed by the display after the screen of the state line (`screen=`) has changed (the app draws at the end of the pass that changed it, so this is that pass,
//     plus whatever holds the frame); the slowest of the screen buttons is the measure, each is printed.
//   * listing.*: the press of File and of Sample load to the pass at which the state line's `sel` names an entry (the listing has one under the cursor), on the large generated card of part B
//     (about 2000 files, 52 folders). This is the time to the FIRST ENTRY (DirScan's first step), not the time to the full listing.
// The measures have the 1 ms of the simulator's tick in them (the key is read in the next pass). A measure that never happens within its window is `never`.
//
// THE BASELINE (baseline_budgets.txt: budget | difference | ID or `new`). The test fails on a finding that is not in it, on a line that no longer occurs (a fixed bug leaves the file), and on a line whose measured
// value moved by more than 10 % (a bug that changed is not the bug that was recorded). QANET_WRITE_BASELINE=<path> writes the current findings in that format. Nothing here knows a bug: no budget is skipped or
// adjusted because of one; the IDs in the file were put there by a person who read the finding.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>

#include <o3os/app/buttons.h>
#include <o3os/app/state_line.h>
#include <o3os/platform/host/fat_image.h>
#include <o3os/platform/host/slow_card.h>
#include <o3os/sim/audio_check.h>
#include <o3os/sim/simulator.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

using namespace o3os;
using app::Button;
using platform::host::CardTiming;

namespace {

// ---- the table -------------------------------------------------------------------------------------------------------------------------

struct Budget {
  const char* id;
  const char* what;
  double limit_ms;
};

// OPEN RESEARCH QUESTION (spec Q1): these limits stand until D4 measures the unit.
constexpr Budget kBudgets[] = {
    {"preview.first_sound", "F5 held on Sample load, a 1 s mono clip: the press to the first sample above -50 dB", 150.0},
    {"preview.long_clip", "the same on a 6 s stereo clip: the preview streams, it does not wait for the whole clip", 150.0},
    {"preview.cursor_move", "F5 held, the cursor moves to another file: the move to the new file's first sound", 150.0},
    {"screen.switch", "a screen button to the frame that shows the new screen (the slowest of the buttons)", 100.0},
    {"listing.file", "File entered: the press to the first entry under the cursor (not the full listing)", 500.0},
    {"listing.sample_load", "Sample load entered: the press to the first entry under the cursor (not the full listing)", 500.0},
};

const Budget& budget(const std::string& id) {
  for (const Budget& b : kBudgets) {
    if (id == b.id) {
      return b;
    }
  }
  return kBudgets[0];  // (a typo in this file: the table test below names every id)
}

// ---- the rig ---------------------------------------------------------------------------------------------------------------------------

std::string fmt_ms(double ms) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.0f", ms);
  return buf;
}

struct Measure {
  double ms = -1.0;  // -1: never within the window
  double fast_ms = -1.0;  // preview: the first crossing pair under 1 ms (the 882 Hz tone), -1 when there is none
  std::string note;  // what else was seen (each screen's time, the file under the cursor)
  bool broken = false;  // the rig or the premise failed (no card, no first sound): a test error, never a finding
  bool seen() const { return ms >= 0.0; }
};

std::string state_value(const sim::Simulator& s, const char* key) {
  char buf[640];
  (void)app::describe_state(s.app(), buf, sizeof buf);
  const std::string line = std::string(" ") + buf + " ";
  const std::string at = std::string(" ") + key + "=";
  const std::size_t p = line.find(at);
  if (p == std::string::npos) {
    return std::string();
  }
  const std::size_t from = p + at.size();
  return line.substr(from, line.find(' ', from) - from);
}

struct Rig {
  std::unique_ptr<sim::Simulator> sim = std::make_unique<sim::Simulator>();
  std::string error;
  bool ok = false;

  // The journey's start: an empty document, the card (a FAT32 image), the slow-card model with `timing`, and the engine when `audio`.
  Rig(const std::string& image, bool audio, const CardTiming& timing) {
    sim->open_document(false, false, 0, true);
    sim->wait_ms(20);
    ok = sim->set_card_timing(timing.on, timing.read_bytes_per_ms, timing.latency_us, timing.write_bytes_per_ms, error) && sim->open_card(image, error) && (!audio || sim->audio_on(error));
    sim->wait_ms(20);
  }
  double now_ms() const { return static_cast<double>(sim->uptime_us()) / 1000.0; }
  // Ticks until the state line's `sel` names an entry (a listing is there), at most `max_ms`; the time it took, -1 when it did not.
  double wait_listing(double t0_ms, double max_ms) {
    for (double t = 0.0; t < max_ms; t += 1.0) {
      if (!state_value(*sim, "sel").empty() && state_value(*sim, "sel") != "-") {
        return now_ms() - t0_ms;
      }
      sim->step();
    }
    return -1.0;
  }
};

CardTiming defaults() {
  CardTiming t;
  t.on = true;  // read_bytes_per_ms, latency_us, write_bytes_per_ms: the D2 defaults
  return t;
}

// ---- the audio measures ----------------------------------------------------------------------------------------------------------------

// The first of the zero crossings of the mid channel that starts a half period shorter than `max_half_ms` at a level of at least -50 dB, in ms from the start of the render; -1 when there is none.
double first_fast_crossing_ms(const sim::Signal& s, double max_half_ms) {
  const std::vector<double> x = s.mid();
  const double floor = std::pow(10.0, -50.0 / 20.0);
  std::size_t last = 0;
  bool have_last = false;
  double peak = 0.0;
  int sign = 0;
  for (std::size_t i = 0; i < x.size(); ++i) {
    const int sg = x[i] > 0.0 ? 1 : (x[i] < 0.0 ? -1 : 0);
    if (sg == 0) {
      continue;
    }
    peak = std::max(peak, std::fabs(x[i]));
    if (sign != 0 && sg != sign) {
      if (have_last && static_cast<double>(i - last) / s.rate * 1000.0 < max_half_ms && peak >= floor) {
        return static_cast<double>(last) / s.rate * 1000.0;
      }
      last = i;
      have_last = true;
      peak = 0.0;
    }
    sign = sg;
  }
  return -1.0;
}

// The last frame at or above -50 dB, in ms from the start of the render; -1 for a silent one.
double last_sound_ms(const sim::Signal& s) {
  const double floor = std::pow(10.0, -50.0 / 20.0);
  for (std::size_t i = s.size(); i-- > 0;) {
    if (std::fabs(s.l[i]) >= floor || std::fabs(s.r[i]) >= floor) {
      return static_cast<double>(i) / s.rate * 1000.0;
    }
  }
  return -1.0;
}

// Ticks until the render `name` has ended (its window of ticks), at most `max_ticks`.
const sim::Pcm* finish_render(Rig& rig, const char* name, unsigned max_ticks) {
  for (unsigned i = 0; i < max_ticks && rig.sim->render_result(name) == nullptr; ++i) {
    rig.sim->step();
  }
  return rig.sim->render_result(name);
}

// Sample load on the preview card with the cursor on file number `downs` (0 first): the listing is up and the cursor has settled.
bool open_sample_load(Rig& rig, unsigned downs, std::string& sel) {
  rig.sim->tap(Button::SampleLoad);
  const double t0 = rig.now_ms();
  if (rig.wait_listing(t0, 10000.0) < 0.0) {
    return false;
  }
  rig.sim->wait_ms(300);
  for (unsigned i = 0; i < downs; ++i) {
    rig.sim->tap(Button::Down);
    rig.sim->wait_ms(100);
  }
  sel = state_value(*rig.sim, "sel");
  return true;
}

// The F5 audition: a window of `window_ms` that opens at the press; the onset of the first sound in it.
Measure preview_first_sound(const std::string& image, unsigned downs, double window_ms, const CardTiming& timing) {
  Measure m;
  Rig rig(image, true, timing);
  if (!rig.ok) {
    m.note = "rig: " + rig.error;
    m.broken = true;
    return m;
  }
  std::string sel;
  if (!open_sample_load(rig, downs, sel)) {
    m.note = "the listing of Sample load never came";
    m.broken = true;
    return m;
  }
  m.note = "file " + sel;
  std::string error;
  if (!rig.sim->render_begin("p", static_cast<std::uint32_t>(window_ms), false, error)) {
    m.note += "; render: " + error;
    m.broken = true;
    return m;
  }
  rig.sim->press(Button::F5);
  const sim::Pcm* pcm = finish_render(rig, "p", static_cast<unsigned>(window_ms) + 50u);
  rig.sim->release(Button::F5);
  if (pcm != nullptr) {
    const sim::Signal sig = sim::to_signal(*pcm);
    m.ms = sim::onset_ms(sig);
    m.fast_ms = first_fast_crossing_ms(sig, 1.0);
  }
  return m;
}

// F5 held on file `from` (a 210 Hz tone): the first file's onset is found with short windows chained back to back (no tick between them), the cursor moves one file down (an 882 Hz tone) 300 ms after
// that onset, while the first file still sounds, and the measure is from the Down press to the new file's first sound. F5 held on Sample load is the maintainer's "Play held" (the audition key,
// app/src/audition.cpp). A first file that never sounds is a broken measure, not a finding (`broken`): the test fails on it.
Measure preview_cursor_move(const std::string& image, unsigned from, double window_ms, const CardTiming& timing) {
  Measure m;
  Rig rig(image, true, timing);
  if (!rig.ok) {
    m.note = "rig: " + rig.error;
    m.broken = true;
    return m;
  }
  std::string sel;
  if (!open_sample_load(rig, from, sel)) {
    m.note = "the listing of Sample load never came";
    m.broken = true;
    return m;
  }
  std::string error;
  constexpr unsigned kProbeMs = 20;
  constexpr double kMoveAfterOnsetMs = 300.0;
  constexpr double kFirstSoundLimitMs = 4000.0;  // the audition buffer is about 4 s: a first file that has not sounded by then is not going to
  double elapsed_ms = 0.0;  // the audio the windows have covered since the F5 press
  double a_onset = -1.0;
  bool sounding = false;  // the last window ended in sound (the first file still plays)
  unsigned n = 0;
  while (elapsed_ms < kFirstSoundLimitMs && (a_onset < 0.0 || elapsed_ms < a_onset + kMoveAfterOnsetMs)) {
    const std::string name = "a" + std::to_string(n++);
    if (!rig.sim->render_begin(name, kProbeMs, false, error)) {
      m.note = "render: " + error;
      m.broken = true;
      return m;
    }
    if (n == 1u) {
      rig.sim->press(Button::F5);
    }
    const sim::Pcm* pcm = finish_render(rig, name.c_str(), kProbeMs + 50u);
    if (pcm == nullptr) {
      m.note = "the probe window " + name + " did not finish";
      m.broken = true;
      return m;
    }
    const sim::Signal sig = sim::to_signal(*pcm);
    const double window_len_ms = static_cast<double>(sig.size()) / sig.rate * 1000.0;
    if (a_onset < 0.0) {
      const double o = sim::onset_ms(sig);
      if (o >= 0.0) {
        a_onset = elapsed_ms + o;
      }
    }
    const double last = last_sound_ms(sig);
    sounding = last >= 0.0 && last >= window_len_ms - 10.0;
    elapsed_ms += window_len_ms;
  }
  if (a_onset < 0.0) {
    rig.sim->release(Button::F5);
    m.note = "BROKEN MEASURE: the first file (" + sel + ") never sounded within " + fmt_ms(kFirstSoundLimitMs) + " ms of F5, so a move could not be tried while it plays";
    m.broken = true;
    return m;
  }
  if (!sounding) {
    rig.sim->release(Button::F5);
    m.note = "BROKEN MEASURE: the first file (" + sel + ") had stopped before the move, " + fmt_ms(elapsed_ms - a_onset) + " ms after its first sound";
    m.broken = true;
    return m;
  }
  if (!rig.sim->render_begin("m", static_cast<std::uint32_t>(window_ms), false, error)) {
    m.note = "render: " + error;
    m.broken = true;
    return m;
  }
  rig.sim->press(Button::Down);
  rig.sim->release(Button::Down);
  const sim::Pcm* pcm = finish_render(rig, "m", static_cast<unsigned>(window_ms) + 50u);
  rig.sim->release(Button::F5);
  const std::string moved_to = state_value(*rig.sim, "sel");
  m.note = "from " + sel + " (first sound " + fmt_ms(a_onset) + " ms after F5, still sounding at the move, " + fmt_ms(elapsed_ms - a_onset) + " ms after its onset) to " + moved_to;
  if (pcm != nullptr) {
    const sim::Signal sig = sim::to_signal(*pcm);
    m.ms = first_fast_crossing_ms(sig, 1.0);
    const double last = last_sound_ms(sig);  // the old file's release rings out for a few ms (256 samples); a sound after that is a file that plays on
    m.note += "; after the move the sound " + (last < 0.0 ? std::string("is silent") : "ends at " + fmt_ms(last) + " ms of " + fmt_ms(window_ms));
  }
  return m;
}

// ---- the visual and listing measures ----------------------------------------------------------------------------------------------------

// The press of `button` to the first frame the display commits once the screen of the state line differs from what it was. -1: it did not within `max_ms`; `changed` says whether the screen changed at all.
double switch_ms(Rig& rig, Button button, double max_ms, std::string& from, std::string& to) {
  from = state_value(*rig.sim, "screen");
  std::size_t prev_frames = rig.sim->display().frames();
  const double t0 = rig.now_ms();
  bool switched = false;
  std::size_t base_frames = prev_frames;
  const auto look = [&]() {
    const std::size_t frames = rig.sim->display().frames();
    if (!switched && state_value(*rig.sim, "screen") != from) {
      switched = true;
      base_frames = prev_frames;  // a frame committed in this pass, after the state changed, counts; one of the pass before does not
    }
    prev_frames = frames;
    return switched && frames > base_frames;
  };
  // A tap, as the simulator's `tap` does it (the press, one tick, the release): some screen buttons act on the release (Song: the gesture is a tap, the hold is another thing), so a key held until the screen came
  // would measure the hold's own timer (400 ms), not the screen. The time is from the press.
  rig.sim->press(button);
  bool shown = look();
  rig.sim->release(button);
  shown = look() || shown;
  for (double t = 0.0; !shown && t < max_ms; t += 1.0) {
    rig.sim->step();
    shown = look();
  }
  const double ms = shown ? rig.now_ms() - t0 : -1.0;
  to = state_value(*rig.sim, "screen");
  return switched ? ms : -2.0;  // -2: the screen did not change (the button was a page of the same screen)
}

Measure screen_switch(const std::string& image, const CardTiming& timing) {
  Measure m;
  Rig rig(image, false, timing);
  if (!rig.ok) {
    m.note = "rig: " + rig.error;
    m.broken = true;
    return m;
  }
  rig.sim->wait_ms(2000);  // the boot's own work (the card announcement, the first frames) is over: the first press is a user's, not the start-up's
  // A walk over the screen buttons: each from where the last left the app, then a settle (jobs, the listing).
  const std::pair<Button, const char*> walk[] = {{Button::Song, "Song"},         {Button::Master, "Master"},     {Button::Params, "Params"},   {Button::SamplePlay, "SamplePlay"},
                                                 {Button::SampleEdit, "SampleEdit"}, {Button::SampleLoad, "SampleLoad"}, {Button::SampleRec, "SampleRec"}, {Button::Performance, "Performance"},
                                                 {Button::File, "File"},         {Button::Config, "Config"},     {Button::Pattern, "Pattern"}};
  double worst = 0.0;
  bool never = false;
  std::string note;
  for (const auto& step : walk) {
    std::string from;
    std::string to;
    const double ms = switch_ms(rig, step.first, 3000.0, from, to);
    if (ms == -2.0) {
      note += std::string(step.second) + " (no change) ";
    } else if (ms < 0.0) {
      never = true;
      note += std::string(step.second) + " never ";
    } else {
      worst = std::max(worst, ms);
      char buf[64];
      std::snprintf(buf, sizeof buf, "%s %.0f ", step.second, ms);
      note += buf;
    }
    rig.sim->wait_ms(400);
  }
  m.note = note;
  m.ms = never ? -1.0 : worst;
  return m;
}

Measure listing(const std::string& image, Button button, const CardTiming& timing) {
  Measure m;
  Rig rig(image, false, timing);
  if (!rig.ok) {
    m.note = "rig: " + rig.error;
    m.broken = true;
    return m;
  }
  const double t0 = rig.now_ms();
  rig.sim->press(button);
  m.ms = rig.wait_listing(t0, 10000.0);
  m.note = "entry " + state_value(*rig.sim, "sel");
  rig.sim->release(button);
  return m;
}

// ---- the cards ---------------------------------------------------------------------------------------------------------------------------

std::string sine_wav(unsigned seconds, unsigned period_frames, unsigned channels) {
  const std::uint32_t frames = 44100u * seconds;
  const std::uint32_t bytes = frames * channels * 2u;
  std::string wav = "RIFF";
  const auto le32 = [&](std::uint32_t v) { wav.append({static_cast<char>(v & 255), static_cast<char>((v >> 8) & 255), static_cast<char>((v >> 16) & 255), static_cast<char>((v >> 24) & 255)}); };
  const auto le16 = [&](std::uint32_t v) { wav.append({static_cast<char>(v & 255), static_cast<char>((v >> 8) & 255)}); };
  le32(36u + bytes);
  wav += "WAVEfmt ";
  le32(16);
  le16(1);
  le16(channels);
  le32(44100);
  le32(44100u * channels * 2u);
  le16(channels * 2u);
  le16(16);
  wav += "data";
  le32(bytes);
  for (std::uint32_t i = 0; i < frames; ++i) {
    const auto v = static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(0.5 * 32767.0 * std::sin(6.283185307179586 * static_cast<double>(i % period_frames) / period_frames))));
    for (unsigned c = 0; c < channels; ++c) {
      le16(v);
    }
  }
  return wav;
}

// The preview card, a FAT32 image of the process's own temporary folder (removed when the process ends): four WAVs in the order the listing shows them,
//   a_short.wav   1 s mono, 441 Hz;   b_long.wav   6 s stereo, 441 Hz;   c_tone_a.wav   6 s mono, 210 Hz;   d_tone_b.wav   6 s mono, 882 Hz.
std::string preview_image() {
  namespace fs = std::filesystem;
  static const fs::path dir = fs::temp_directory_path() / ("o3os_qanet_budgets_" + std::to_string(static_cast<unsigned long>(::getpid())));
  struct Cleanup {
    ~Cleanup() {
      std::error_code ec;
      fs::remove_all(dir, ec);
    }
  };
  static const Cleanup cleanup;
  static std::string path;
  if (!path.empty()) {
    return path;
  }
  std::error_code ec;
  fs::create_directories(dir, ec);
  platform::host::FatImage image;
  const std::pair<const char*, std::string> files[] = {{"a_short.wav", sine_wav(1, 100, 1)}, {"b_long.wav", sine_wav(6, 100, 2)}, {"c_tone_a.wav", sine_wav(6, 210, 1)}, {"d_tone_b.wav", sine_wav(6, 50, 1)}};
  for (const auto& f : files) {
    if (!image.add_file(platform::host::FatImage::kRootCluster, f.first, reinterpret_cast<const std::uint8_t*>(f.second.data()), f.second.size())) {
      std::fprintf(stderr, "qanet budgets: the preview card: %s\n", image.error().c_str());
      return std::string();
    }
  }
  std::string error;
  const std::string target = (dir / "preview.img").string();
  if (!image.write_file(target, error)) {
    std::fprintf(stderr, "qanet budgets: the preview card: %s\n", error.c_str());
    return std::string();
  }
  path = target;
  return path;
}

// ---- the findings and the baseline -------------------------------------------------------------------------------------------------------

struct Finding {
  std::string id;
  std::string kind;  // over | never
  std::string diff;  // the text of the line
  double value = 0.0;
};

struct BaseLine {
  std::string id, kind, diff, known;
  double value = 0.0;
  bool used = false;
};

std::string trim(std::string s) {
  const char* ws = " \t\r\n";
  s.erase(0, s.find_first_not_of(ws) == std::string::npos ? s.size() : s.find_first_not_of(ws));
  s.erase(s.find_last_not_of(ws) == std::string::npos ? 0 : s.find_last_not_of(ws) + 1);
  return s;
}

// The verdict of one measure against its budget: no finding when it is inside, `over` with the value when it is not, `never` when it did not happen.
bool judge(const std::string& id, const Measure& m, Finding& out) {
  const Budget& b = budget(id);
  if (!m.seen()) {
    out = {id, "never", "never within the window (limit " + fmt_ms(b.limit_ms) + " ms)", 0.0};
    return true;
  }
  if (m.ms > b.limit_ms) {
    out = {id, "over", "over: " + fmt_ms(m.ms) + " ms against " + fmt_ms(b.limit_ms) + " ms", m.ms};
    return true;
  }
  return false;
}

// "budget | difference | ID or new": a difference begins with its kind (over, never); an `over` has its value in the text (the first number after the colon).
bool parse_baseline(const std::string& text, std::vector<BaseLine>& out, std::string& error) {
  std::istringstream in(text);
  std::string line;
  int n = 0;
  while (std::getline(in, line)) {
    ++n;
    if (trim(line).empty() || trim(line)[0] == '#') {
      continue;
    }
    std::vector<std::string> f;
    std::istringstream ls(line);
    std::string part;
    while (std::getline(ls, part, '|')) {
      f.push_back(trim(part));
    }
    bool known_budget = false;
    for (const Budget& b : kBudgets) {
      known_budget = known_budget || (f.size() == 3u && f[0] == b.id);
    }
    if (f.size() != 3u || !known_budget || f[2].empty() || (f[1].rfind("over", 0) != 0 && f[1].rfind("never", 0) != 0)) {
      error = "baseline line " + std::to_string(n) + ": want 'budget | over: <ms> ms against <ms> ms or never ... | ID or new' with a budget of the table";
      return false;
    }
    BaseLine b{f[0], f[1].substr(0, f[1].find_first_of(": ")), f[1], f[2], 0.0, false};
    const std::size_t at = f[1].find(": ");
    b.value = at == std::string::npos ? 0.0 : std::strtod(f[1].c_str() + at + 2, nullptr);
    out.push_back(b);
  }
  return true;
}

std::vector<std::string> against_baseline(const std::vector<Finding>& found, std::vector<BaseLine>& base) {
  std::vector<std::string> problems;
  for (const Finding& f : found) {
    BaseLine* hit = nullptr;
    for (BaseLine& b : base) {
      if (b.id == f.id && b.kind == f.kind) {
        hit = &b;
      }
    }
    if (hit == nullptr) {
      problems.push_back("NEW FINDING: " + f.id + " | " + f.diff);
      continue;
    }
    hit->used = true;
    if (f.kind == "over" && std::fabs(f.value - hit->value) > std::max(2.0, 0.1 * hit->value)) {
      problems.push_back("CHANGED: " + f.id + " | now " + f.diff + ", the baseline has " + hit->diff);
    }
  }
  for (const BaseLine& b : base) {
    if (!b.used) {
      problems.push_back("STALE BASELINE LINE (no longer occurs; remove it): " + b.id + " | " + b.diff + " | " + b.known);
    }
  }
  return problems;
}

std::string read_file(const char* path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

// ---- the tests ---------------------------------------------------------------------------------------------------------------------------

TEST_CASE("qanet budgets: the table names each budget once, and the baseline's comparison fails on a new finding, a stale line and a finding that moved") {
  for (std::size_t i = 0; i < std::size(kBudgets); ++i) {
    CHECK(kBudgets[i].limit_ms > 0.0);
    for (std::size_t j = i + 1; j < std::size(kBudgets); ++j) {
      CHECK(std::strcmp(kBudgets[i].id, kBudgets[j].id) != 0);
    }
  }
  std::vector<BaseLine> base;
  std::string error;
  CHECK(parse_baseline("# comment\n\npreview.first_sound | over: 700 ms against 150 ms | OS-BUG-PREVLAT\nlisting.file | never within the window (limit 500 ms) | new\n", base, error));
  CHECK(base.size() == 2u);
  const Finding known{"preview.first_sound", "over", "over: 705 ms against 150 ms", 705.0};
  {
    std::vector<BaseLine> b = base;
    CHECK(against_baseline({known}, b).size() == 1u);  // (the listing line is stale: it did not occur)
  }
  {
    std::vector<BaseLine> b = base;
    const std::vector<std::string> p = against_baseline({known, {"listing.file", "never", "never", 0.0}, {"screen.switch", "over", "over: 130 ms against 100 ms", 130.0}}, b);
    REQUIRE(p.size() == 1u);
    CHECK(p[0].find("NEW FINDING: screen.switch") == 0u);
  }
  {
    std::vector<BaseLine> b = base;
    const std::vector<std::string> p = against_baseline({{"preview.first_sound", "over", "over: 400 ms against 150 ms", 400.0}, {"listing.file", "never", "never", 0.0}}, b);
    REQUIRE(p.size() == 1u);
    CHECK(p[0].find("CHANGED: preview.first_sound") == 0u);
  }
  CHECK(against_baseline({}, base).size() == 2u);  // both lines stale
  std::vector<BaseLine> bad;
  CHECK_FALSE(parse_baseline("only | two\n", bad, error));
  CHECK_FALSE(parse_baseline("nothing.like.it | over: 5 ms | new\n", bad, error));
  Finding f;
  CHECK_FALSE(judge("screen.switch", Measure{99.0, -1.0, ""}, f));  // inside
  CHECK(judge("screen.switch", Measure{101.0, -1.0, ""}, f));
  CHECK(f.kind == "over");
  CHECK(judge("screen.switch", Measure{-1.0, -1.0, ""}, f));
  CHECK(f.kind == "never");
}

TEST_CASE("qanet budgets: the measures see what they measure (a planted slow card, a planted late tone, a silent render)") {
  // The crossing detector: a 210 Hz tone is not a new file, an 882 Hz tone is, and where it begins is where it is found.
  const auto tone = [](double hz, double start_ms, double rate, double ms) {
    sim::Signal s;
    s.rate = rate;
    const std::size_t n = static_cast<std::size_t>(ms * rate / 1000.0);
    for (std::size_t i = 0; i < n; ++i) {
      const double t = static_cast<double>(i) / rate * 1000.0;
      const double v = t < start_ms ? 0.0 : 0.5 * std::sin(6.283185307179586 * hz * (t - start_ms) / 1000.0);
      s.l.push_back(v);
      s.r.push_back(v);
    }
    return s;
  };
  CHECK(first_fast_crossing_ms(tone(210.0, 0.0, 44100.0, 100.0), 1.0) < 0.0);
  const double at = first_fast_crossing_ms(tone(882.0, 37.0, 44100.0, 100.0), 1.0);
  CHECK(at >= 37.0);
  CHECK(at < 40.0);
  sim::Signal silent;
  silent.l.assign(4410, 0.0);
  silent.r.assign(4410, 0.0);
  CHECK(first_fast_crossing_ms(silent, 1.0) < 0.0);

  // A planted slow card: 50 bytes a millisecond is far under the unit's: the preview starts once 5.5k frames are read (11 KB, 220 ms of the card alone), far over its budget, and the measure says so (the model is on and charging).
  const std::string image = preview_image();
  REQUIRE(!image.empty());
  CardTiming slow = defaults();
  slow.read_bytes_per_ms = 50;
  const Measure planted = preview_first_sound(image, 0, 3000.0, slow);
  CHECK_MESSAGE(planted.seen(), "the planted slow card: " << planted.note);
  Finding f;
  CHECK(judge("preview.first_sound", planted, f));
  CHECK(planted.ms > 200.0);  // (the streamed start reads rate / 8 frames first: 11 KB at 50 bytes a ms is 220 ms of the card alone)
  // The same card with the model off sounds sooner than the slow one (the model moves the number: the measure is of the card's time).
  CardTiming off;
  const Measure fast = preview_first_sound(image, 0, 3000.0, off);
  CHECK(fast.seen());
  CHECK(fast.ms < planted.ms);
  // The positive control of the cursor-move measure, through the real engine: the 882 Hz file auditioned from the start is found by the crossing detector (and the 441 Hz one is not).
  const Measure tone_b = preview_first_sound(image, 3, 3000.0, defaults());
  CHECK_MESSAGE(tone_b.fast_ms >= 0.0, "the 882 Hz file auditioned: " << tone_b.note);
  CHECK(preview_first_sound(image, 0, 3000.0, defaults()).fast_ms < 0.0);
}

TEST_CASE("qanet budgets: the latency budgets on the slow card") {
  const std::string image = preview_image();
  REQUIRE(!image.empty());
  const char* big_card = O3_QANET_CARD_IMAGE;
  const CardTiming timing = defaults();

  std::vector<std::pair<std::string, Measure>> results;
  results.emplace_back("preview.first_sound", preview_first_sound(image, 0, 3000.0, timing));
  results.emplace_back("preview.long_clip", preview_first_sound(image, 1, 3000.0, timing));
  results.emplace_back("preview.cursor_move", preview_cursor_move(image, 2, 3000.0, timing));
  results.emplace_back("screen.switch", screen_switch(big_card, timing));
  results.emplace_back("listing.file", listing(big_card, Button::File, timing));
  results.emplace_back("listing.sample_load", listing(big_card, Button::SampleLoad, timing));

  std::printf("\nqanet budgets: the slow card is %u bytes/ms read, %u us a call (PROVISIONAL: the unit's block bench, D4 measures)\n", timing.read_bytes_per_ms, timing.latency_us);
  std::printf("%-22s | %6s | %9s | %s\n", "budget", "limit", "measured", "verdict");
  std::vector<Finding> found;
  for (const auto& r : results) {
    Finding f;
    CHECK_MESSAGE(!r.second.broken, r.first << ": " << r.second.note);
    const bool bad = !r.second.broken && judge(r.first, r.second, f);
    std::printf("%-22s | %4s ms | %9s | %s   [%s]\n", r.first.c_str(), fmt_ms(budget(r.first).limit_ms).c_str(), r.second.seen() ? (fmt_ms(r.second.ms) + " ms").c_str() : "never", bad ? f.diff.c_str() : "pass",
                r.second.note.c_str());
    if (bad) {
      found.push_back(f);
    }
  }

  // Not graded: how much the first-sound number depends on the card's numbers. The BACKLOG's estimate of the unit's preview read (OS-BUG-PREVLAT: about 500 KB/s, 2 ms a call) against the D2 defaults.
  CardTiming estimate = timing;
  estimate.read_bytes_per_ms = 500;
  estimate.latency_us = 2000;
  const Measure sens = preview_first_sound(image, 0, 3000.0, estimate);
  std::printf("qanet budgets: for comparison, preview.first_sound on a card of 500 bytes/ms and 2000 us a call (the BACKLOG's estimate): %s\n", sens.seen() ? (fmt_ms(sens.ms) + " ms").c_str() : "never");

  if (const char* out = std::getenv("QANET_WRITE_BASELINE")) {
    std::ofstream file(out, std::ios::binary | std::ios::trunc);
    std::vector<BaseLine> old;
    std::string ignore;
    (void)parse_baseline(read_file(O3_QANET_BUDGETS_BASELINE), old, ignore);
    file << "# OS-QANET D3: the findings of budgets.cpp that are known. One line each: budget | difference | ID or `new`.\n"
            "# The test fails on a finding that is not here, on a line here that no longer occurs, and on a value that moved by more than 10 %; a fixed bug leaves the file.\n";
    for (const Finding& f : found) {
      std::string id = "new";
      for (const BaseLine& b : old) {
        if (b.id == f.id && b.kind == f.kind) {
          id = b.known;
        }
      }
      file << f.id << " | " << f.diff << " | " << id << "\n";
    }
  }

  std::vector<BaseLine> base;
  std::string error;
  REQUIRE_MESSAGE(parse_baseline(read_file(O3_QANET_BUDGETS_BASELINE), base, error), error);
  const std::vector<std::string> problems = against_baseline(found, base);
  for (const std::string& p : problems) {
    std::printf("%s\n", p.c_str());
  }
  CHECK_MESSAGE(problems.empty(), problems.size() << " difference(s) from tests/qanet/baseline_budgets.txt (see above; QANET_WRITE_BASELINE=<path> writes the current list)");
}
