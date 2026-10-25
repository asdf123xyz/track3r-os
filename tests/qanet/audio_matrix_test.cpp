// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-QANET C1 (docs/specs/OS-QANET.md part C; new Community OS behaviour, test tooling only): a differential audio matrix. Class 3 of the spec: two paths reach one voice pool, each is tested alone, nobody
// compares them. Here the same note on the same instrument is played by a PAD and by a Pattern STEP, in the real engine inside the simulator (`audio on`, `render`), and the two renders must agree in pitch
// (5 cents) and in onset (2 ms), the tolerances of sim/include/o3os/sim/audio_check.h's `pitch=` and `onset=`. The reference is the Pattern render (the spec: note 60 plays the sample at its own rate).
//
// ROWS (instrument kind x play mode): a plain sample (one shot, loop forward, loop backward, ping-pong), a sample on the Amiga engine, a chopped sample (BEAT SLICE made by ADD, by EVEN and by AUTO, and the
// explicit SLICE mode), WAVETABLE, GRANULAR, and the TB-303 module. MIDI out is not a row: the simulator has no MIDI link and the instrument is silent in it (sim_audio.h, "Not wired"), so there is nothing
// to hear; the test says so in its log.
// COLUMNS (the path): the pad of the Pattern screen (Browse), the pad of the Instrument screen (instrument.pad), and, on a chopped sample, the pad of the Wave screen (wave.pad); C2 adds MIDI note in
// (the scenario word `midinote`, which feeds midi::CcInput and polls the app as the platform does), EXPORT (File > EXPORT, the WAV read back, its first 400 ms; no cell without a card) and, in the row
// file.preview only, the F5 preview of Sample load (a WAV of a card the test generates; it plays the file at its own rate, so the step it is compared with is the root note's, and its onset is a latency, reported). The step is written by
// the Pattern pad in Edit (the note is whatever that pad plays), so "the same note" is the pad index. Every cell runs the journey a person does: open the Kit on the card, pick the instrument, set the mode or
// chop it on the Wave screen, go to Pattern, write the step, then press the pad and Play, each in a render window of its own.
//
// C3 (cells `edit.*`, and `.fresh` on the chop cells): every edit that rebinds the instrument's descriptor is played twice, the pad pressed straight after the edit (no settle wait; the key of the edit is the last
// line of the setup, and an edit answered by a job, as AUTO's slicer, is waited for tick by tick with `@until <path>=<value>`: the pad is the next thing in the pass after the apply) and after the sound has
// settled (the cell's own run, the step written meanwhile). The step is the settled reference of both. Edits: ADD, EVEN, AUTO, slice delete, a change of play mode (loop backward, wavetable, granular), a sample
// load over an empty slot and over a chopped one, and the undo of each of ADD, EVEN, AUTO, slice delete and the mode change. NOT PLAYED: the undo of a load (F8 after a load leaves the instrument as the load made it,
// so there is nothing to undo to hear), and the loop direction of ping_pong (not an edit cell). Two cells a sine cannot fail play a generated two-pitch WAV that Sample load puts in (make_card): edit.slice_del (qasplit: the 441 Hz half
// and the 882 Hz half, so which slice a pad plays shows in the pitch) and edit.mode_loop_bwd (qaloop: 160 ms of 441 Hz, then 120 ms of a higher tone; a backward loop turns at the end of the first pass, so the
// ORDER of the tones shows, read as a tone track of 20 ms bins because the pitch reading of a two-tone render is an octave off on some windows; a self-test holds that forward and backward differ in it).
// THE LAG SIZED: a fresh run that finds something is played again with the pad 5, 20 and 50 ms after the edit (`lag:` lines of the log, the first delay that agrees with the settled step); a lag the 20 ms rung does not
// cure fails the cell (it is another bug than the 5 ms wait). edit.load_empty presses the pad at the instrument's name, which the import's job sets some passes before the bank binds the audio: the slot is empty for the
// engine and the note is dropped for 5 ms (silent, a `new` finding; `edit.load_empty.fresh` lines).
// C3 MORE, the ring cells (`ring.<kind>_<edit>`, a test case of their own, `-tc=*voice still sounding*`): the same edits made while the voice pool is not quiet (a note still sounding, a held pad, pads played in a row,
// the transport playing), the age of the stale descriptor measured from the engine and a pad probe heard (the block above run_ring).
// Sharded: one ctest a cell (QANET_ONLY, tests/qanet/audio_matrix.cmake), each under a minute.
//
// The findings are compared with baseline_audio.txt (cell | paths | difference | ID or `new`). The test fails on a finding that is not in it and on a baseline line that no longer occurs, so a fixed bug has
// to leave the file. QANET_ONLY=<cell id> runs that one cell (a quick look at one row; the baseline comparison is then incomplete). QANET_WRITE_BASELINE=<path> writes the current findings in that format (the IDs of the lines that exist are kept, a new one is marked `new`). Nothing here knows a bug: no cell is
// skipped or adjusted because of one. The self-tests plant a detune and a delay in a pad render and the checks must see them.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>

#include <o3os/app/pad_board.h>
#include <o3os/audio/voices.h>
#include <o3os/sim/audio_check.h>
#include <o3os/sim/scenario.h>
#include <o3os/sim/scenario_assert.h>
#include <o3os/sim/sim_audio.h>
#include <o3os/sim/simulator.h>

#include "audio_probe.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

using namespace o3os;
using namespace o3os::qanet_audio;

namespace {

// ---- the measurements: tests/qanet/audio_probe.cpp (shared with the random walk's probe, C4) ----

// ---- the baseline ----------------------------------------------------------------------------------------------------------------------

struct BaseLine {
  std::string cell, paths, what, diff, id;
  double value = 0.0;
  bool used = false;
};

std::string trim(std::string s) {
  const char* ws = " \t\r\n";
  s.erase(0, s.find_first_not_of(ws) == std::string::npos ? s.size() : s.find_first_not_of(ws));
  s.erase(s.find_last_not_of(ws) == std::string::npos ? 0 : s.find_last_not_of(ws) + 1);
  return s;
}

// "cell | paths | difference | id": a difference begins with its kind (pitch, onset, tones, silent, nopitch, dead); a pitch or onset has its value in the text (the first number).
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
    if (f.size() != 4u || f[0].empty() || f[3].empty()) {
      error = "baseline line " + std::to_string(n) + ": want 'cell | paths | difference | ID or new'";
      return false;
    }
    BaseLine b{f[0], f[1], f[2].substr(0, f[2].find(' ')), f[2], f[3], 0.0, false};
    const std::size_t at = f[2].find_first_of("+-0123456789");
    b.value = at == std::string::npos ? 0.0 : std::strtod(f[2].c_str() + at, nullptr);
    out.push_back(b);
  }
  return true;
}

// The findings against the baseline: a finding with no line, a line with no finding, and a line whose measured value moved by more than a tolerance (a bug that changed is not the bug that was recorded).
std::vector<std::string> against_baseline(const std::vector<Finding>& found, std::vector<BaseLine>& base) {
  std::vector<std::string> problems;
  for (const Finding& f : found) {
    BaseLine* hit = nullptr;
    for (BaseLine& b : base) {
      if (b.cell == f.cell && b.paths == f.paths && b.what == f.what) {
        hit = &b;
      }
    }
    if (hit == nullptr) {
      problems.push_back("NEW FINDING: " + f.cell + " | " + f.paths + " | " + f.diff);
      continue;
    }
    hit->used = true;
    // (`stale`, a descriptor's age in ms, is held to 10 % like a long latency)
    // cents, ms: sanitizers and compilers move the engine's phase a little, not an octave; a latency of hundreds of ms (the preview's read, OS-BUG-PREVLAT) is held to 10 %
    const double tol = f.what == "pitch" ? 50.0 : std::max(1.5, 0.1 * std::fabs(hit->value));
    if ((f.what == "pitch" || f.what == "onset" || f.what == "stale") && std::fabs(f.value - hit->value) > tol) {
      problems.push_back("CHANGED: " + f.cell + " | " + f.paths + " | now " + f.diff + ", the baseline has " + hit->diff);
    }
  }
  for (const BaseLine& b : base) {
    if (!b.used) {
      problems.push_back("STALE BASELINE LINE (no longer occurs; remove it): " + b.cell + " | " + b.paths + " | " + b.diff + " | " + b.id);
    }
  }
  return problems;
}

std::string read_file(const char* path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// ---- the cells -------------------------------------------------------------------------------------------------------------------------

struct Cell {
  std::string id;          // row: <kind>.<mode>
  unsigned instrument = 1;  // 1..64 (the Kit's: 1 SINE loop, 2 SAW on the Amiga engine, 3 PLUCK one shot)
  unsigned pad = 16;       // the pad both paths use (16: E-5 on the default board, four semitones over the root)
  bool tb303 = false;      // `document tb303` (the module and no card)
  bool chopped = false;    // the Wave screen's pad is a column too
  bool fresh = false;      // also run with the pad pressed straight after the edit (no wait: a person's next key), one run a pad
  std::string setup;       // scenario lines run on the Wave screen: the mode, the chop. The last line of a fresh cell is the key that edits (nothing waits after it)
  int mode = -1;           // the play mode the setup must have made (PLAY MODE 0..7), asserted on the model right after it; -1: no model row (the TB-303 module)
  int expect_slices = -1;  // the slice count the chop must have made (SLICES), asserted the same way; -1: not a chopped cell
  bool preview = false;    // the Sample load journey instead of the Kit: a WAV on a generated card, F5 held (the preview column), then F1 loads it into instrument 1 and the other paths play that
  double abs_hz = 0.0;     // > 0: the Pattern step must also sound at this pitch (an absolute check: a bug common to the pad and the step would pass the comparison); 0: not checked
  bool gen_card = false;   // C3: the Sample load journey's document and generated card (one WAV, qatone) instead of the Kit; the pad is the root pad and the instrument starts empty (the preview cell is one too)
  std::string expect_name;  // C3: the instrument's name after the setup (a load took: the file's name); empty: not asserted
  int tone = 0;            // C3 follow-up: the WAV of the generated card (gen_card): 0 a sine (qatone), 1 two halves of two pitches (qasplit: slice identity shows in pitch), 2 a short two-pitch sample (qaloop: loop direction shows)
  Cell(std::string id_, unsigned instrument_, unsigned pad_, bool tb303_, bool chopped_, bool fresh_, std::string setup_, int mode_, int expect_slices_, bool preview_, double abs_hz_, bool gen_card_ = false,
       std::string expect_name_ = std::string(), int tone_ = 0)
      : id(std::move(id_)), instrument(instrument_), pad(pad_), tb303(tb303_), chopped(chopped_), fresh(fresh_), setup(std::move(setup_)), mode(mode_), expect_slices(expect_slices_), preview(preview_), abs_hz(abs_hz_), gen_card(gen_card_),
        expect_name(std::move(expect_name_)), tone(tone_) {}
};

// The recipes of the Wave screen's edits (pick_mode, kChop, kEven, kAdd, kAuto) are in audio_probe.h: the random walk's probe makes the same edits.

// Sample load, F1: the one WAV of the generated card goes into the instrument; then the WAV is in, the engine has it (`bound.1`), the sound has died and the Pattern screen is up.
// The Kit's instruments 1 and 2 are there, so the load asks REPLACE first (OS-BUG-SLDOVER): the second F1 answers it.
const std::string kLoad = "tap sampleload\nwait 500\ntap f1\nwait 50\ntap f1\n";
std::string loaded_wav(const char* name) { return kLoad + "@until inst.1.name=" + name + "\n@until bound.1\nwait 600\ntap pattern\n"; }

// The Kit's sine loop is 441.18 Hz at its root (C-5, note 60), and pad 16 is four semitones over it (the default board's E-5): 441.18 * 2^(4/12) = 555.8 Hz, the spec's "note 60 plays the sample at its own rate".
const double kPad16Hz = 441.18 * std::pow(2.0, 4.0 / 12.0);

// The pad of the default board that plays note 60, the root: the preview plays a WAV at its own rate, so the step it is compared with is the root note.
unsigned root_pad() {
  const app::PadBoard board;
  std::uint8_t pads[app::PadBoard::kPads] = {};
  board.pads_with_note(audio::kRootNote, pads);
  for (unsigned i = 0; i < app::PadBoard::kPads; ++i) {
    if (pads[i] != 0u) {
      return i;
    }
  }
  return 0;
}

// The slice counts the chops make on the Kit's sine loop (observed, and asserted by the cells): EVEN of two makes 2; ADD on an unchopped sample puts a slice at the start first (wave_actions.cpp, slice_add) and
// one at the middle, so 2; AUTO finds no transient in a sine and keeps one slice. Pad 1 plays the first slice in every one.
std::vector<Cell> cells() {
  std::vector<Cell> c;
  c.push_back({"sample.one_shot", 3, 16, false, false, false, "", 0, -1, false, 0.0});
  c.push_back({"sample.loop_fwd", 1, 16, false, false, false, "", 1, -1, false, kPad16Hz});
  c.push_back({"sample.loop_bwd", 1, 16, false, false, false, pick_mode(2), 2, -1, false, kPad16Hz});
  c.push_back({"sample.ping_pong", 1, 16, false, false, false, pick_mode(3), 3, -1, false, 0.0});
  c.push_back({"sample.amiga_engine", 2, 16, false, false, false, "", 1, -1, false, 0.0});
  c.push_back({"slice.beat_even", 1, 1, false, true, true, kEven, 5, 2, false, 0.0});
  c.push_back({"slice.beat_add", 1, 1, false, true, true, kAdd, 5, 2, false, 0.0});
  c.push_back({"slice.beat_auto", 1, 1, false, true, true, kAuto, 5, 1, false, 0.0});
  c.push_back({"slice.slice_mode", 1, 16, false, false, false, kEven + "wait 50\n" + pick_mode(4), 4, 2, false, 0.0});
  c.push_back({"wavetable.wavetable", 1, 16, false, false, false, pick_mode(6), 6, -1, false, 0.0});
  c.push_back({"grain.granular", 1, 16, false, false, false, pick_mode(7), 7, -1, false, 0.0});
  c.push_back({"module.tb303", 1, 16, true, false, false, "", -1, -1, false, 0.0});
  c.push_back({"file.preview", 1, root_pad(), false, false, false, "", 0, -1, true, 441.18});
  // C3: the edits that rebind the instrument's descriptor, each played straight after (fresh) and after settling (the run itself). The last line of the setup is the key that edits; an undo is that key.
  const std::string del = kEven + "wait 50\ntap f5\n";
  // The two cells a sine cannot fail (loop direction, which slice is slice 0) play a generated two-pitch WAV that Sample load puts into the instrument (tone 2 and 1, make_card).
  const std::string load = kLoad;
  auto loaded = loaded_wav;
  c.push_back({"edit.mode_loop_bwd", 1, root_pad(), false, false, true, loaded("qaloop") + pick_mode(2, false), 2, -1, false, 0.0, true, "qaloop", 2});
  c.push_back({"edit.mode_wavetable", 1, 16, false, false, true, pick_mode(6, false), 6, -1, false, 0.0});
  c.push_back({"edit.mode_granular", 1, 16, false, false, true, pick_mode(7, false), 7, -1, false, 0.0});
  c.push_back({"edit.slice_del", 1, 0, false, true, true, loaded("qasplit") + del, 5, 1, false, 0.0, true, "qasplit", 1});
  c.push_back({"edit.undo_add", 1, 16, false, false, true, kAdd + "wait 50\ntap f8\n", 1, 0, false, kPad16Hz});
  c.push_back({"edit.undo_even", 1, 16, false, false, true, kEven + "wait 50\ntap f8\n", 1, 0, false, kPad16Hz});
  c.push_back({"edit.undo_auto", 1, 16, false, false, true, kChop + "tap f1\n@until inst.1.play_mode=5\nwait 100\ntap f8\n", 1, 0, false, kPad16Hz});
  c.push_back({"edit.undo_slice_del", 1, 1, false, true, true, del + "wait 50\ntap f8\n", 5, 2, false, 0.0});
  c.push_back({"edit.undo_mode", 1, 16, false, false, true, pick_mode(2) + "tap f8\n", 1, -1, false, kPad16Hz});
  // The name is set when the import's job ends and the bank is told then, but the rebind lands only in a quiet window some passes later: the pad is pressed when the voices have the audio (`bound.1`), not at the name.
  c.push_back({"edit.load_empty", 1, root_pad(), false, false, true, load + "@until inst.1.name=qatone\n", -1, -1, false, 441.18, true, "qatone"});
  const std::string sliced = load + "@until inst.1.name=qatone\nwait 600\ntap pattern\n" + kEven + "wait 50\n";  // a WAV loaded and chopped in two (the slot to load over)
  c.push_back({"edit.load_over_sliced", 1, root_pad(), false, false, true, sliced + load + "@until inst.1.slices=0\n", -1, -1, false, 441.18, true, "qatone"});
  return c;
}

// The surfaces that play the note: path name, the button that brings the screen up, and whether it is a MIDI key (else a pad).
struct Surface {
  const char* path;
  const char* screen;
  bool midi;
};
constexpr Surface kPattern{"pad:pattern", "tap pattern", false};
constexpr Surface kInstrument{"pad:instrument", "tap params", false};
constexpr Surface kWave{"pad:wave", "tap sampleplay", false};  // on the Wave screen already after a chop: the button would turn the page
// MIDI note in (C2): `midinote` of the simulator (sim/README.md) feeds the bytes to midi::CcInput and calls App::poll_midi_notes, the platform's own path (app_main.cpp), the way OS-E2E E2E-6 puts a
// message through the render's parser. The keyboard plays the selected track's instrument. The Pattern screen, not in Edit: only the live note sounds.
constexpr Surface kMidi{"midi:in", "tap pattern", true};
constexpr Surface kPreview{"preview:f5", "tap sampleload", false};  // (only named here: its window is the cell's prologue)

// The MIDI note of the cell's pad: the board's (a beat slice instrument: 24 + pad, app/pad_note.h, as the pad itself plays it).
unsigned midi_note_of(const Cell& c) {
  const app::PadBoard board;
  return c.mode == 5 ? audio::kSliceRootNote + c.pad : static_cast<unsigned>(board.note(c.pad));
}

// The WAVs of the generated cards, 16 bit mono at 44.1 kHz, in runs of a sine of exactly `period` frames (100: 441.0 Hz at 44.1 kHz, 441.18 Hz at the engine's rate: the Kit's own sine; 50: one octave up).
// tone 0, qatone: one second of the 100-frame sine (the preview cell, the loads). tone 1, qasplit: two halves of a second, 100 then 50 (C3 follow-up: EVEN makes the halves two slices, and which slice a pad plays
// shows in its pitch: a sine cannot tell slice 0 from slice 1). tone 2, qaloop: 150 ms of 100 then 100 ms of 50 (a loop over it turns at 250 ms: the first pass is forward in every loop mode, so the sample is
// short and the loop direction shows in what plays in the 400 ms of a render; tests below hold that fwd and bwd differ).
// tone 3 and 4 (the ring cells): a card of two WAVs, a_quiet.wav (the first file, so F1 loads it: silence, 3000 ms for tone 3, 100 ms for tone 4; the voice that plays it sounds for its length and is not heard)
// and qatone.wav (the second file: the sine that is chopped). The silent one is how a long sample tail is made without mixing into the probe's pitch.
struct Run {
  std::uint32_t frames;
  std::uint32_t period;  // 0: silence
};
std::vector<Run> tone_runs(int tone) {
  switch (tone) {
    case 1:
      return {{22050, 100}, {22050, 50}};
    case 2:
      return {{7056, 100}, {5292, 64}};
    default:
      return {{44100, 100}};
  }
}
const char* tone_name(int tone) { return tone == 1 ? "qasplit" : (tone == 2 ? "qaloop" : (tone == 3 ? "qaring3000" : (tone == 4 ? "qaring100" : "qatone"))); }
std::string wav_bytes(const std::vector<Run>& runs) {
  std::uint32_t frames = 0;
  for (const Run& r : runs) {
    frames += r.frames;
  }
  std::string wav = "RIFF";
  auto le32 = [&](std::uint32_t v) { wav.append({static_cast<char>(v & 255), static_cast<char>((v >> 8) & 255), static_cast<char>((v >> 16) & 255), static_cast<char>((v >> 24) & 255)}); };
  auto le16 = [&](std::uint32_t v) { wav.append({static_cast<char>(v & 255), static_cast<char>((v >> 8) & 255)}); };
  le32(36u + frames * 2u);
  wav += "WAVEfmt ";
  le32(16);
  le16(1);
  le16(1);
  le32(44100);
  le32(88200);
  le16(2);
  le16(16);
  wav += "data";
  le32(frames * 2u);
  for (const Run& r : runs) {
    for (std::uint32_t i = 0; i < r.frames; ++i) {
      le16(r.period == 0u ? 0u : static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(0.5 * 32767.0 * std::sin(6.283185307179586 * static_cast<double>(i % r.period) / static_cast<double>(r.period))))));
    }
  }
  return wav;
}
std::string make_card(int tone) {
  namespace fs = std::filesystem;
  // Named by the process id (two runs at once do not share a folder); removed when the process ends, whatever the tests did (a static's destructor runs when main returns).
  static const fs::path root = fs::temp_directory_path() / ("o3os_qanet_card_" + std::to_string(static_cast<unsigned long>(::getpid())));
  struct Cleanup {
    ~Cleanup() {
      std::error_code ec;
      fs::remove_all(root, ec);
    }
  };
  static const Cleanup cleanup;
  const fs::path dir = root / tone_name(tone);  // one WAV a card (F1 of Sample load takes the first file); the ring cards hold two
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string wav = wav_bytes(tone_runs(tone));
  const std::string file = std::string(tone_name(tone == 3 || tone == 4 ? 0 : tone)) + ".wav";
  std::ofstream(dir / file, std::ios::binary).write(wav.data(), static_cast<std::streamsize>(wav.size()));
  if (tone == 3 || tone == 4) {
    const std::string quiet = wav_bytes({{tone == 3 ? 132300u : 4410u, 0}});
    std::ofstream(dir / "a_quiet.wav", std::ios::binary).write(quiet.data(), static_cast<std::streamsize>(quiet.size()));
  }
  return dir.string();
}
std::string make_preview_card() { return make_card(0); }

// Where the export lands (File > EXPORT, SONG, F1): EXPORT/<project>/<project>_SONG0001.WAV, the Kit's name, or UNTITLED for a document with no name (sim/scenarios/e2e/e2e_ref_dsp_export.txt).
const char* kExportKit = "EXPORT/KIT/KIT_SONG0001.WAV";
const char* kExportUntitled = "EXPORT/UNTITLED/UNTITLED_SONG0001.WAV";

// The journey of one run: the scenario text and the names of its renders (path name -> render name). `fresh` (a surface of a fresh cell): the pad is pressed first, straight after the edit; else every surface
// of the cell is pressed after the step is written and the sound has died away.
std::string script(const Cell& c, const Surface* fresh, std::map<std::string, std::string>& renders, unsigned delay_ms = 0) {
  std::string s = c.tb303 ? "document tb303\nwait 20\naudio on\nwait 20\n" : "document\nwait 20\ncard e2e_pro\naudio on\nwait 20\ntap file\nwait 300\ntap f1\nwait 1000\n";
  if (c.preview) {
    // Sample load on a card with one WAV: F5 held auditions it (the first path), then F1 loads it into the instrument and the other paths play that instrument at its root note. The window is 1.5 s: the
    // audition reads the whole clip before it sounds (OS-BUG-PREVLAT), the onset says how long that takes.
    s = "document sound\nwait 20\ncard " + make_preview_card() + "\naudio on\nwait 20\ntap pattern\naction instrument.select " + std::to_string(c.instrument) + "\ntap sampleload\nwait 500\n";
    renders[kPreview.path] = "r_preview";
    s += "render r_preview 1500\npress f5\nwait 1200\nrelease f5\nwait 298\ntap f1\nwait 50\ntap f1\nwait 600\nassert model inst." + std::to_string(c.instrument) + ".name=qatone\n";  // (the load took: the instrument has the file's name)
  }
  if (c.gen_card && !c.preview) {
    s = "document sound\nwait 20\ncard " + make_card(c.tone) + "\naudio on\nwait 20\n";
  }
  s += "tap pattern\naction instrument.select " + std::to_string(c.instrument) + "\n";
  s += c.setup;
  // Did the setup take? The model must show the mode and the slice count the cell is about: a wrong one is a test error, not a finding (the Shift+F1 list is walked blind and play_mode_set can refuse
  // with a notify). An assertion takes no tick, so the key of a fresh cell is still the last thing that happened.
  const std::string inst = "assert model inst." + std::to_string(c.instrument) + ".";
  if (c.mode >= 0) {
    s += inst + "play_mode=" + std::to_string(c.mode) + "\n";
  }
  if (c.expect_slices >= 0) {
    s += inst + "slices=" + std::to_string(c.expect_slices) + "\n";
  }
  if (!c.expect_name.empty()) {
    s += inst + "name=" + c.expect_name + "\n";
  }
  const std::string pad = std::to_string(c.pad);
  // 400 ms a window: a pad held 200 ms is 202 ticks; Play and Stop are 2 ticks each.
  auto pad_window = [&](const Surface& sf, bool go_to_screen) {
    std::string name = std::string("r_") + sf.path;
    std::replace(name.begin(), name.end(), ':', '_');  // a render's name is letters, digits and _
    renders[sf.path] = name;
    const std::string note = std::to_string(midi_note_of(c));
    s += (go_to_screen ? std::string(sf.screen) + "\nwait 100\n" : std::string()) + "render " + name + " 400\n" +
         (sf.midi ? "midinote 1 " + note + " 100\nwait 200\nmidinote 1 " + note + " off\nwait 198\n" : "pad " + pad + " hold 200\nwait 198\n");
  };
  if (fresh != nullptr) {
    // the key of the edit was the last line of the setup: the screen of the pad and the pad, no wait between (`delay_ms`, the lag sizing: that long a wait after the edit, then the same)
    if (delay_ms > 0u) {
      s += "wait " + std::to_string(delay_ms) + "\n";
    }
    if (fresh != &kWave) {
      s += std::string(fresh->screen) + "\n";
    }
    pad_window(*fresh, false);
  }
  // The step: the Pattern pad in Edit writes its note on the first step of the cursor's track (the pad sounds too: let it die).
  s += "tap pattern\nwait 50\ntap rec\ntap note\naction cursor.jump_fraction 0\npad " + pad + "\ntap rec\nwait 600\n";
  if (fresh == nullptr) {
    pad_window(kPattern, true);
    pad_window(kInstrument, true);
    if (c.chopped) {
      pad_window(kWave, true);
    }
    pad_window(kMidi, true);
  }
  renders["step"] = "r_step";
  s += "tap pattern\nwait 100\nrender r_step 400\ntap play\nwait 200\ntap play\nwait 196\n";
  if (fresh == nullptr && !c.tb303) {
    // EXPORT: File > EXPORT (F6 twice ends on PROJECTS, F7 the EXPORT page, SONG the first row, F1 starts it) writes the song, the first pattern from its first step, as a WAV; `wav` reads it back. No `null=`
    // against the live render (the limiter is not deterministic, design review 2026-10-21): the same pitch and onset as the Pattern render, the first 400 ms of the file (run_cell cuts it).
    renders["export"] = "r_export";
    s += std::string("tap file\nwait 50\ntap f6\ntap f6\ntap f7\nwait 50\ntap f1\nwait 50\nwait 2000\nwav r_export ") + (c.preview || c.gen_card ? kExportUntitled : kExportKit) + "\n";
  }
  return s;
}

struct Result {
  std::vector<Finding> findings;
  std::string error;
  std::string log;  // what each path measured, one phrase a path
  double preview_onset = -1.0;  // ms from the start of the render to the first sound of the F5 preview (-1: no preview column)
  std::map<std::string, sim::Pcm> pcm;  // the renders by path name, `step` the reference (for the self-test that plants a fault in one)
};

// Runs `text` on `simulator`, in pieces at its `@until <path>=<value>` lines (C3): each is a `wait 0` to the parser (so that the whole script is checked as one: `audio on` before `render`), and the commands are cut there.
// After the piece before it the test steps the simulator one tick at a time until the model has that value (an edit whose answer is a job's, as AUTO's slicer: the apply lands in some pass, not at a time a
// script can name) and the next piece begins in the very next pass, the first one a person could have pressed a pad in. `log` gets one `[path after n ticks]` a wait. false: `error` says why.
bool run_text(sim::Simulator& simulator, const std::string& text, std::string& error, std::string& log) {
  std::vector<std::string> conds;
  std::string joined;
  {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
      if (line.compare(0, 7, "@until ") == 0) {
        conds.push_back(line.substr(7));
        joined += "wait 0\n";
      } else {
        joined += line + "\n";
      }
    }
  }
  std::vector<sim::ScenarioCommand> all;
  if (!sim::parse_scenario(joined, all, error)) {
    return false;
  }
  std::vector<std::vector<sim::ScenarioCommand>> pieces(1);
  for (const sim::ScenarioCommand& cmd : all) {
    pieces.back().push_back(cmd);
    if (cmd.kind == sim::ScenarioCommand::Kind::Wait && cmd.value == 0u) {
      pieces.emplace_back();
    }
  }
  for (std::size_t k = 0; k < pieces.size(); ++k) {
    if (!sim::run_scenario(pieces[k], simulator, [](const std::string&, const sim::Simulator&) { return true; }, error)) {
      if (error.find("assertion failed: assert model inst.") != std::string::npos) {
        error = "SETUP DID NOT TAKE (a test error, not a product finding): " + error;
      }
      return false;
    }
    if (k < conds.size()) {
      const std::size_t eq = conds[k].find('=');
      const std::string path = conds[k].substr(0, eq);
      const std::string want = eq == std::string::npos ? std::string() : conds[k].substr(eq + 1u);
      unsigned ticks = 0;
      // `bound.<n>`: the voices' descriptor of instrument n has audio (the bank's rebind has landed: a note on it plays what the edit made). The model has no such row (the bank is not in it); the engine's pool says.
      const bool bound_wait = path.compare(0, 6, "bound.") == 0;
      const unsigned bound_slot = bound_wait ? static_cast<unsigned>(std::atoi(path.c_str() + 6)) - 1u : 0u;
      auto reached = [&]() {
        if (bound_wait) {
          return simulator.audio() != nullptr && simulator.audio()->voices().sample(bound_slot).length != 0u;
        }
        return simulator.project() != nullptr && sim::model_problem(*simulator.project(), path, want).empty();
      };
      while (!reached()) {
        if (++ticks > 5000u) {
          error = "SETUP DID NOT TAKE (a test error, not a product finding): " + conds[k] + " did not come within 5 s";
          return false;
        }
        simulator.step();
      }
      char b[64];
      std::snprintf(b, sizeof b, " [%s after %u ticks]", path.c_str(), ticks);
      log += b;
    }
  }
  return true;
}

Result run_cell(const Cell& c, const Surface* fresh, unsigned delay_ms = 0) {
  Result r;
  std::map<std::string, std::string> renders;
  const std::string text = script(c, fresh, renders, delay_ms);
  const std::string id = fresh != nullptr ? c.id + ".fresh" : c.id;
  auto simulator = std::make_unique<sim::Simulator>();
  simulator->set_scenario_dir(O3_QANET_CARDS_DIR);
  if (!run_text(*simulator, text, r.error, r.log)) {
    return r;
  }
  const sim::Pcm* ref = simulator->render_result(renders["step"]);
  if (ref == nullptr) {
    r.error = "no render of the step";
    return r;
  }
  auto say = [&](const char* name, const sim::Pcm& p) {
    double spread = 0.0;
    const Measure m = measure(p, &spread);
    char b[112];
    std::snprintf(b, sizeof b, " %s %.1f Hz (spread %.0f c) @%.1f ms;", name, m.hz, spread, m.onset);
    r.log += b;
  };
  say("step", *ref);
  if (c.abs_hz > 0.0) {
    const double hz = measure(*ref).hz;
    if (!(hz > 0.0) || std::fabs(cents(hz, c.abs_hz)) > kPitchCents) {
      char b[128];
      std::snprintf(b, sizeof b, "ABSOLUTE PITCH: the step plays %.1f Hz, the Kit's sample at pad %u is %.1f Hz (both paths may be wrong together)", hz, c.pad, c.abs_hz);
      r.error = b;
      return r;
    }
  }
  r.pcm["step"] = *ref;
  for (const auto& kv : renders) {
    if (kv.first == "step") {
      continue;
    }
    const sim::Pcm* got = simulator->render_result(kv.second);
    if (got == nullptr) {
      r.error = "no render of " + kv.first;
      return r;
    }
    sim::Pcm cut = *got;
    if (kv.first == "export") {
      cut.lr.resize(std::min(cut.lr.size(), 2u * static_cast<std::size_t>(0.4 * cut.rate)));  // the first 400 ms of the file: the window of the other renders
    }
    say(kv.first.c_str(), cut);
    if (kv.first == "preview:f5") {
      r.preview_onset = measure(cut).onset;
    }
    r.pcm[kv.first] = cut;
    for (Finding& f : compare(id, kv.first, cut, *ref, kv.first != "preview:f5", c.tone == 2)) {
      r.findings.push_back(std::move(f));
    }
  }
  return r;
}

// ---- C3 more: a voice still sounding does not keep the stale descriptor (regression of OS-BUG-SLICEPITCH) ----------------------------------------------------------------------------
// SampleBank::poll rebinds the voices' descriptor only in a quiet window (transport stopped, no live note, NO VOICE SOUNDS, kSettleMs), and before OS-BUG-SLICEPITCH a pad pressed meanwhile played the old
// descriptor for as long as a voice sounded (0.9 s to 4 s). Since the fix a note started after the edit reads the staged descriptor at once; these cells hold that. The fresh cells above
// press the pad in a quiet pool, where the lag is the 5 ms. Here the pool is not quiet when the edit is made, and the question is how long the pad keeps the stale descriptor:
//   tail    a note of another instrument (a_quiet.wav, 3 s of silence: a long sample tail that is not heard) was struck just before the edit and still sounds
//   held    the same on a 100 ms sample, whose pad is held down for 1 s after the edit (a held pad: a live note)
//   series  pads of the chopped instrument itself, one every 100 ms for 2 s, from the edit on (a person playing the chop)
//   play    the transport is playing (an empty pattern: nothing sounds) and is stopped 4 s after the edit
// The edits are ADD, EVEN and AUTO (the three that make a beat slice). TWO MEASUREMENTS, in two runs of the same journey (a probe note is itself a voice and would hold the pool):
//   STALE (the engine's truth): the ticks from the edit until the voices' descriptor of the instrument says beat slice, counted from the apply of the edit (the model's play mode; AUTO's slicer is a job), at most 5 s.
//   AUDIBLE: the pad of the Wave screen 100 ms after the edit, 400 ms of render, against the settled step of the same edit (pitch and onset, the C1 tolerances). The probe is the first of the pads in the series.
// A stale longer than 20 ms is a finding of kind `stale` (the value is the milliseconds; at the bound it is the bound, and the text says `or more`).
struct RingCase {
  std::string id;    // ring.<kind>_<edit>
  std::string kind;  // tail | held | series | play
  std::string edit;  // even | add | auto
};

std::vector<RingCase> ring_cases() {
  std::vector<RingCase> v;
  for (const char* kind : {"tail", "held", "series", "play"}) {
    for (const char* edit : {"even", "add", "auto"}) {
      v.push_back({std::string("ring.") + kind + "_" + edit, kind, edit});
    }
  }
  return v;
}

// Sample load of the card's two WAVs: the silent one into instrument 2 (F1 takes the first file, as a one shot: its default), the sine into instrument 1 (the second file: Down, F1). Ends on Pattern, instrument 1 selected.
std::string ring_load() {
  return "action instrument.select 2\ntap sampleload\nwait 500\ntap f1\nwait 50\ntap f1\n@until inst.2.name=a_quiet\n@until bound.2\nwait 600\ntap pattern\nassert model inst.2.play_mode=0\n"
         "action instrument.select 1\ntap sampleload\nwait 500\ntap down\ntap f1\nwait 50\ntap f1\n@until inst.1.name=qatone\n@until bound.1\nwait 600\ntap pattern\n";
}
// The navigation of the edit up to its key, and the key (AUTO's is answered by a job: the key line is followed by the wait for the apply in the settled run, by the clock of the ring run).
std::string ring_pre(const std::string& edit) { return edit == "even" ? kEven.substr(0, kEven.rfind("tap f2\n")) : kChop; }
const char* ring_key(const std::string& edit) { return edit == "even" ? "tap f2" : (edit == "add" ? "tap f4" : "tap f1"); }
int ring_slices(const std::string& edit) { return edit == "auto" ? 1 : 2; }

// The settled step of the edit: the same journey with the pad 500 ms after the key and the step written (the cells' own machinery).
sim::Pcm ring_reference(const RingCase& rc, std::string& error) {
  const std::string key = std::string(ring_key(rc.edit)) + "\n" + (rc.edit == "auto" ? "@until inst.1.play_mode=5\n" : "");
  const Cell c{"ring.ref_" + rc.edit, 1, 1, false, true, true, ring_load() + ring_pre(rc.edit) + key, 5, ring_slices(rc.edit), false, 0.0, true, "qatone", rc.kind == "held" ? 4 : 3};
  const Result r = run_cell(c, &kWave, 500);
  error = r.error;
  return r.pcm.count("step") != 0u ? r.pcm.at("step") : sim::Pcm();
}

struct RingRun {
  std::string error;
  int apply_ms = -1;   // the edit reached the model (AUTO: the slicer's answer)
  int rebind_ms = -1;  // the voices' descriptor became a beat slice; -1: not within the bound
  int quiet_ms = -1;   // the first moment the voice pool had no voice (voices_in_use() == 0) after the key; -1: never within the bound
  sim::Pcm probe;
  bool probed = false;
};

constexpr int kRingBoundMs = 5000;

// `probe` false: the engine's truth only. true: the audible probe too (series: in the same run, which goes on to the bound; the others stop when the probe's window has closed).
RingRun run_ring(const RingCase& rc, bool probe) {
  RingRun out;
  auto simulator = std::make_unique<sim::Simulator>();
  simulator->set_scenario_dir(O3_QANET_CARDS_DIR);
  sim::Simulator& sm = *simulator;
  const unsigned ring_pad = root_pad();
  std::string text = "document sound\nwait 20\ncard " + make_card(rc.kind == "held" ? 4 : 3) + "\naudio on\nwait 20\ntap pattern\naction instrument.select 1\n" + ring_load();
  if (rc.kind == "tail") {
    text += "action instrument.select 2\npad " + std::to_string(ring_pad) + "\naction instrument.select 1\n";
  } else if (rc.kind == "held") {
    text += "action instrument.select 2\npad " + std::to_string(ring_pad) + " down\naction instrument.select 1\n";
  } else if (rc.kind == "play") {
    text += "tap play\nwait 50\n";
  }
  text += ring_pre(rc.edit);
  std::string log;
  if (!run_text(sm, text, out.error, log)) {
    return out;
  }
  std::vector<sim::ScenarioCommand> key;
  if (!sim::parse_scenario(std::string(ring_key(rc.edit)) + "\n", key, out.error) || key.size() != 1u) {
    return out;
  }
  std::vector<sim::ScenarioCommand> play_key;
  if (!sim::parse_scenario("tap play\n", play_key, out.error) || play_key.size() != 1u) {
    return out;
  }
  const audio::SampleDesc pre = sm.audio()->voices().sample(0);
  if (pre.beat_slice) {
    out.error = "SETUP DID NOT TAKE (a test error, not a product finding): the instrument is a beat slice before the edit";
    return out;
  }
  int now = 0;  // ms since the key went down
  auto look = [&]() {
    const audio::SampleDesc& d = sm.audio()->voices().sample(0);
    if (out.quiet_ms < 0 && sm.audio()->voices().voices_in_use() == 0u) {
      out.quiet_ms = now;
    }
    if (out.apply_ms < 0 && sm.project() != nullptr && sim::model_problem(*sm.project(), "inst.1.play_mode", "5").empty()) {
      out.apply_ms = now;
    }
    if (out.rebind_ms < 0 && (d.beat_slice != pre.beat_slice || d.slice_count != pre.slice_count)) {
      out.rebind_ms = now;
    }
  };
  auto tick = [&](unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
      sm.step();
      ++now;
      look();
    }
  };
  const unsigned wave_pad = 1;
  sm.tap(key[0].button);
  now += 2;
  look();
  std::string why;
  const bool series = rc.kind == "series";
  // The events, in time: {at ms, what}. A pad press is a tick, as is its release.
  struct Ev {
    int at;
    int what;  // 0 probe window and pad down, 1 probe pad up, 2 ring pad up (held), 3 stop (play), 4 series pad down, 5 series pad up
  };
  std::vector<Ev> evs;
  if (series) {
    for (int k = 0; k < 20; ++k) {
      evs.push_back({k * 100, 4});
      evs.push_back({k * 100 + 20, 5});
    }
  } else {
    if (probe) {
      evs.push_back({100, 0});
      evs.push_back({300, 1});
    }
    if (rc.kind == "held") {
      evs.push_back({1000, 2});
    }
    if (rc.kind == "play") {
      evs.push_back({4000, 3});
    }
  }
  std::sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.at < b.at; });
  const int end_ms = (probe && !series) ? 520 : kRingBoundMs;
  std::size_t next = 0;
  while (now < end_ms && (out.rebind_ms < 0 || (probe && now < 520))) {  // the truth run stops at the rebind; a probe run, when its window has closed
    while (next < evs.size() && evs[next].at <= now) {
      switch (evs[next].what) {
        case 0:
          if (!sm.render_begin("r_probe", 400, false, why)) {
            out.error = "render: " + why;
            return out;
          }
          out.probed = true;
          sm.pad_down(wave_pad);
          ++now;
          break;
        case 1: sm.pad_up(wave_pad); ++now; break;
        case 2: sm.pad_up(ring_pad); ++now; break;
        case 3: sm.tap(play_key[0].button); now += 2; break;
        case 4:
          if (evs[next].at == 0 && probe && !sm.render_begin("r_probe", 400, false, why)) {
            out.error = "render: " + why;
            return out;
          }
          out.probed = out.probed || (evs[next].at == 0 && probe);
          sm.pad_down(wave_pad);
          ++now;
          break;
        default: sm.pad_up(wave_pad); ++now; break;
      }
      look();
      ++next;
    }
    tick(1);
  }
  if (probe) {
    tick(10);
    const sim::Pcm* got = sm.render_result("r_probe");
    if (got == nullptr) {
      out.error = "no render of the probe";
      return out;
    }
    out.probe = *got;
  }
  return out;
}

// ---- synthetic renders for the self-tests ------------------------------------------------------------------------------------------------

sim::Pcm tone(double hz, double delay_ms, double ms = 400.0) {
  sim::Pcm p;
  p.rate = 44117.647;
  const std::size_t n = static_cast<std::size_t>(ms * p.rate / 1000.0);
  const std::size_t start = static_cast<std::size_t>(delay_ms * p.rate / 1000.0);
  p.lr.assign(2u * n, 0);
  for (std::size_t i = start; i < n; ++i) {
    const double v = 0.4 * std::sin(6.283185307179586 * hz * static_cast<double>(i - start) / p.rate);
    p.lr[2u * i] = p.lr[2u * i + 1u] = static_cast<std::int16_t>(std::lround(v * 32767.0));
  }
  return p;
}

// `p` played `late_ms` later (silence in front, the length kept).
sim::Pcm delayed(const sim::Pcm& p, double late_ms) {
  sim::Pcm q = p;
  const std::size_t shift = static_cast<std::size_t>(late_ms * p.rate / 1000.0);
  for (std::size_t i = p.frames(); i-- > 0;) {
    for (std::size_t ch = 0; ch < 2u; ++ch) {
      q.lr[2u * i + ch] = i >= shift ? p.lr[2u * (i - shift) + ch] : std::int16_t{0};
    }
  }
  return q;
}

}  // namespace

// ---- the self-tests: the matrix can fail ------------------------------------------------------------------------------------------------

TEST_CASE("qanet audio matrix: equal renders agree, a planted detune and a planted delay are findings") {
  const sim::Pcm ref = tone(441.18, 2.0);
  CHECK(compare("t", "pad:pattern", tone(441.18, 2.0), ref).empty());
  CHECK(compare("t", "pad:pattern", tone(441.18 * std::pow(2.0, 3.0 / 1200.0), 2.0), ref).empty());  // 3 cents: inside the tolerance
  const std::vector<Finding> detuned = compare("t", "pad:pattern", tone(441.18 * std::pow(2.0, 12.0 / 1200.0), 2.0), ref);  // 12 cents
  REQUIRE(detuned.size() == 1u);
  CHECK(detuned[0].what == "pitch");
  CHECK(detuned[0].value == doctest::Approx(12.0).epsilon(0.2));
  const std::vector<Finding> octave = compare("t", "pad:pattern", tone(441.18 / 8.0, 2.0), ref);  // three octaves down
  REQUIRE(octave.size() == 1u);
  CHECK(octave[0].value == doctest::Approx(-3600.0).epsilon(0.02));
  const std::vector<Finding> late = compare("t", "pad:pattern", tone(441.18, 6.0), ref);  // 4 ms late
  REQUIRE(late.size() == 1u);
  CHECK(late[0].what == "onset");
  CHECK(late[0].value == doctest::Approx(4.0).epsilon(0.2));
  CHECK(compare("t", "preview:f5", tone(441.18, 40.0), ref, false).empty());  // a preview's latency is reported, not compared
  const sim::Pcm silent = tone(441.18, 0.0);
  sim::Pcm mute = silent;
  std::fill(mute.lr.begin(), mute.lr.end(), std::int16_t{0});
  const std::vector<Finding> none = compare("t", "pad:pattern", mute, ref);
  REQUIRE(none.size() == 1u);
  CHECK(none[0].what == "silent");
  CHECK(compare("t", "pad:pattern", mute, mute)[0].what == "dead");
}

TEST_CASE("qanet audio matrix: the pitch of a render is not moved by a phase slip, and a detune over most of the note is a finding") {
  // 555.8 Hz, 250 ms, a 0.9 ms gap in the phase at 100 ms: a reading of the whole run (one YIN window of 8192 samples) is 15 cents flat, what the engine's loop_bwd showed in the C2 review.
  sim::Pcm p = tone(555.8, 0.0, 250.0);
  const std::size_t at = static_cast<std::size_t>(0.100 * p.rate);
  const std::size_t slip = static_cast<std::size_t>(0.0009 * p.rate);
  for (std::size_t i = p.frames() - 1u; i >= at + slip; --i) {
    p.lr[2u * i] = p.lr[2u * i + 1u] = p.lr[2u * (i - slip)];
  }
  for (std::size_t i = at; i < at + slip; ++i) {
    p.lr[2u * i] = p.lr[2u * i + 1u] = 0;
  }
  const double whole = sim::pitch_hz(sim::to_signal(p).mid(), p.rate);
  CHECK_MESSAGE(std::fabs(cents(whole, 555.8)) > kPitchCents, "the precondition: the whole-run reading is " << whole << " Hz, it must be off by more than 5 cents or this test proves nothing");
  const Measure m = measure(p);
  CHECK_MESSAGE(std::fabs(cents(m.hz, 555.8)) < kPitchCents, "steady_pitch reads " << m.hz << " Hz");
  CHECK(compare("t", "pad:pattern", p, tone(555.8, 0.0, 250.0)).empty());

  // A detune that covers 70 % of the note is the pitch most windows agree on, so it is the note's pitch: against the true one it is a finding.
  auto partly = [](double from, double to, double hz_a, double hz_b) {
    sim::Pcm q = tone(hz_a, 0.0, 400.0);
    const sim::Pcm other = tone(hz_b, 0.0, 400.0);
    const std::size_t n = q.frames();
    for (std::size_t i = static_cast<std::size_t>(from * static_cast<double>(n)); i < static_cast<std::size_t>(to * static_cast<double>(n)); ++i) {
      q.lr[2u * i] = q.lr[2u * i + 1u] = other.lr[2u * i];
    }
    return q;
  };
  const double sharp = 555.8 * std::pow(2.0, 20.0 / 1200.0);
  const std::vector<Finding> late = compare("t", "pad:pattern", partly(0.0, 0.7, 555.8, sharp), tone(555.8, 0.0, 400.0));
  REQUIRE(late.size() == 1u);
  CHECK(late[0].what == "pitch");
  CHECK(late[0].value == doctest::Approx(20.0).epsilon(0.25));
  // The stated limit: a detune over the first 30 % only (a glide in the first 120 ms) is the minority, and is not seen. The printed spread of the windows (max minus min, in cents) is what shows it.
  CHECK(compare("t", "pad:pattern", partly(0.0, 0.3, 555.8, sharp), tone(555.8, 0.0, 400.0)).empty());
  double spread = 0.0;
  const Measure glide = measure(partly(0.0, 0.3, 555.8, sharp), &spread);
  CHECK(std::fabs(cents(glide.hz, 555.8)) < kPitchCents);
  CHECK(spread > kPitchCents);
}

TEST_CASE("qanet audio matrix: a fault planted in a real pad render of the simulator is a finding") {
  const Cell loop{"sample.loop_fwd", 1, 16, false, false, false, "", 1, -1, false, 0.0};
  const Result r = run_cell(loop, nullptr);
  REQUIRE_MESSAGE(r.error.empty(), r.error);
  REQUIRE(r.pcm.count("pad:pattern") == 1u);
  const sim::Pcm& pad = r.pcm.at("pad:pattern");
  const sim::Pcm& step = r.pcm.at("step");
  CHECK(r.findings.empty());
  const std::vector<Finding> d = compare("t", "pad:pattern", detuned(pad, 20.0), step);  // the pad's pitch 20 cents sharp
  REQUIRE(d.size() == 1u);
  CHECK(d[0].what == "pitch");
  CHECK(d[0].value == doctest::Approx(20.0).epsilon(0.25));
  const std::vector<Finding> l = compare("t", "pad:pattern", delayed(pad, 5.0), step);  // the pad's note 5 ms late
  REQUIRE(l.size() == 1u);
  CHECK(l[0].what == "onset");
  CHECK(l[0].value == doctest::Approx(5.0).epsilon(0.2));
}

TEST_CASE("qanet audio matrix: a setup that did not take and a wrong absolute pitch fail the cell, they are not findings") {
  Cell wrong_mode{"sample.loop_fwd", 1, 16, false, false, false, "", 5, -1, false, 0.0};  // the instrument is in loop forward (1), not in beat slice
  const Result m = run_cell(wrong_mode, nullptr);
  CHECK(m.error.find("SETUP DID NOT TAKE") == 0u);
  CHECK(m.error.find("play_mode=5") != std::string::npos);
  Cell wrong_slices{"sample.loop_fwd", 1, 16, false, false, false, "", 1, 3, false, 0.0};  // not chopped: no slices
  CHECK(run_cell(wrong_slices, nullptr).error.find("slices=3") != std::string::npos);
  Cell wrong_hz{"sample.loop_fwd", 1, 16, false, false, false, "", 1, -1, false, kPad16Hz * 1.02};  // 34 cents off
  const Result a = run_cell(wrong_hz, nullptr);
  CHECK(a.error.find("ABSOLUTE PITCH") == 0u);
  Cell right{"sample.loop_fwd", 1, 16, false, false, false, "", 1, -1, false, kPad16Hz};
  const Result ok = run_cell(right, nullptr);
  CHECK_MESSAGE(ok.error.empty(), ok.error);
}

TEST_CASE("qanet audio matrix: the two-pitch WAVs make slice identity and loop direction audible (the cells that a sine could not fail)") {
  // Slice identity: EVEN of qasplit makes two slices, the 441 Hz half and the 882 Hz half; pad 0 plays the first, pad 1 the second (a pad past the last slice plays the last) (the step must sound at that pitch: run_cell's absolute check).
  const std::string chop = loaded_wav("qasplit") + kEven + "wait 50\n";
  for (const auto& pad_hz : {std::make_pair(0u, 441.18), std::make_pair(1u, 882.36)}) {
    Cell c{"x", 1, pad_hz.first, false, true, false, chop, 5, 2, false, pad_hz.second, true, "qasplit", 1};
    const Result r = run_cell(c, nullptr);
    CHECK_MESSAGE(r.error.empty(), "slice " << pad_hz.first << ": " << r.error);
  }
  // Loop direction: the same sample in loop forward (1) and loop backward (2) differs in the order of the tones (the first pass is forward in both; the turn is at 280 ms), so a pad that plays one while the
  // step plays the other is a finding of kind `tones`, and two renders of one mode are not.
  auto step_of = [&](unsigned mode) {
    Cell c{"x", 1, root_pad(), false, false, false, loaded_wav("qaloop") + pick_mode(mode), static_cast<int>(mode), -1, false, 0.0, true, "qaloop", 2};
    const Result r = run_cell(c, nullptr);
    CHECK_MESSAGE(r.error.empty(), "mode " << mode << ": " << r.error);
    return r.pcm.count("step") != 0u ? r.pcm.at("step") : sim::Pcm();
  };
  const sim::Pcm fwd = step_of(1);
  const sim::Pcm bwd = step_of(2);
  REQUIRE(fwd.frames() > 0u);
  REQUIRE(bwd.frames() > 0u);
  const std::vector<int> tf = tone_track(fwd);
  const std::vector<int> tb = tone_track(bwd);
  std::string tfs, tbs;
  for (int v : tf) {
    tfs += static_cast<char>('0' + v);
  }
  for (int v : tb) {
    tbs += static_cast<char>('0' + v);
  }
  std::printf("  loop direction, the tone of each 20 ms: forward %s, backward %s\n", tfs.c_str(), tbs.c_str());
  CHECK(compare("t", "pad:pattern", fwd, fwd, true, true).empty());
  const std::vector<Finding> d = compare("t", "pad:pattern", bwd, fwd, true, true);
  bool seen = false;
  for (const Finding& f : d) {
    seen = seen || f.what == "tones";
  }
  CHECK_MESSAGE(seen, "a backward loop and a forward one must differ in the tone track, or the cell is blind to the direction: forward " << tfs << ", backward " << tbs);
}

TEST_CASE("qanet audio matrix: the midinote word of the scenario grammar") {
  std::vector<sim::ScenarioCommand> cmds;
  std::string error;
  CHECK(sim::parse_scenario("midinote 1 64 100\nmidinote 16 0 1\nmidinote 1 127 off\n", cmds, error));
  REQUIRE(cmds.size() == 3u);
  CHECK(cmds[0].kind == sim::ScenarioCommand::Kind::MidiNote);
  CHECK(cmds[0].mode == 1u);
  CHECK(cmds[0].value == 64u);
  CHECK(cmds[0].delta == 100);
  CHECK(cmds[2].delta == 0);  // off
  for (const char* bad : {"midinote 0 64 100\n", "midinote 17 64 100\n", "midinote 1 128 100\n", "midinote 1 64 0\n", "midinote 1 64 128\n", "midinote 1 64\n", "midinote 1 -1 100\n", "midinote 1 64 +5\n"}) {
    CHECK_MESSAGE(!sim::parse_scenario(bad, cmds, error), bad);
  }
}

TEST_CASE("qanet audio matrix: every cell has its shard in audio_matrix.cmake") {
  std::string listed = " " O3_QANET_CELLS " ";
  for (const Cell& c : cells()) {
    CHECK_MESSAGE(listed.find(" " + c.id + " ") != std::string::npos, c.id << " has no ctest (add it to the list in tests/qanet/audio_matrix.cmake)");
    std::size_t at = listed.find(" " + c.id + " ");
    if (at != std::string::npos) {
      listed.erase(at, c.id.size() + 1u);
    }
  }
  CHECK_MESSAGE(trim(listed).empty(), "a ctest of audio_matrix.cmake names no cell:" << listed);
}

TEST_CASE("qanet audio matrix: the baseline's comparison fails on a new finding, on a stale line and on a finding that moved") {
  std::vector<BaseLine> base;
  std::string error;
  REQUIRE(parse_baseline("# comment\n\nslice.x | pad:pattern vs step | pitch -3600 c (a against b) | OS-BUG-SLICEPITCH\nsample.y | pad:wave vs step | onset +3.0 ms | new\n", base, error));
  REQUIRE(base.size() == 2u);
  CHECK(base[0].what == "pitch");
  std::vector<BaseLine> silent;
  REQUIRE(parse_baseline("e.x | pad:pattern vs step | silent - the path is silent, the step sounds | new\n", silent, error));
  CHECK(silent[0].what == "silent");
  CHECK(base[0].value == doctest::Approx(-3600.0));
  const Finding known{"slice.x", "pad:pattern vs step", "pitch", "pitch -3590 c", -3590.0};
  const Finding other{"sample.z", "pad:pattern vs step", "pitch", "pitch +40 c", 40.0};
  {
    auto b = base;
    const std::vector<std::string> p = against_baseline({known}, b);
    REQUIRE(p.size() == 1u);  // the second line is stale
    CHECK(p[0].find("STALE") == 0u);
  }
  {
    auto b = base;
    const std::vector<std::string> p = against_baseline({known, other, {"sample.y", "pad:wave vs step", "onset", "onset +3.2 ms", 3.2}}, b);
    REQUIRE(p.size() == 1u);
    CHECK(p[0].find("NEW FINDING: sample.z") == 0u);
  }
  {
    auto b = base;
    const std::vector<std::string> p = against_baseline({{"slice.x", "pad:pattern vs step", "pitch", "pitch -1200 c", -1200.0}, {"sample.y", "pad:wave vs step", "onset", "onset +3.2 ms", 3.2}}, b);
    REQUIRE(p.size() == 1u);
    CHECK(p[0].find("CHANGED") == 0u);
  }
  std::vector<BaseLine> bad;
  CHECK_FALSE(parse_baseline("only | three | fields\n", bad, error));
}

// The findings of a run against the baseline file: `ring` selects the lines of the ring cells (cell ids `ring.*`) or the others, so that each test case answers for its own.
static void finish(const std::vector<Finding>& found, const char* only, bool ring) {
  std::vector<BaseLine> base;
  std::string error;
  REQUIRE_MESSAGE(parse_baseline(read_file(O3_QANET_AUDIO_BASELINE), base, error), error);
  if (const char* out = std::getenv("QANET_WRITE_BASELINE")) {
    std::ofstream f(out);
    f << "# OS-QANET C1: the findings of audio_matrix_test.cpp that are known. One line each: cell | paths | difference | ID or `new`.\n"
         "# The test fails on a finding that is not here and on a line here that no longer occurs; a fixed bug leaves the file. A pitch or onset value may move by 50 cents or 1.5 ms.\n";
    for (const Finding& x : found) {
      std::string id = "new";
      for (const BaseLine& b : base) {
        if (b.cell == x.cell && b.paths == x.paths && b.what == x.what) {
          id = b.id;
        }
      }
      f << x.cell << " | " << x.paths << " | " << (x.diff.compare(0, x.what.size(), x.what) == 0 ? x.diff : x.what + " - " + x.diff) << " | " << id << "\n";  // a difference begins with its kind (pitch, onset, silent, ...)
    }
  }
  // A shard (QANET_ONLY=<cell>, the ctest of that cell) answers for its own lines of the baseline only: the cell and its `.fresh` runs.
  std::vector<BaseLine> mine;
  for (const BaseLine& b : base) {
    if ((b.cell.compare(0, 5, "ring.") == 0) != ring) {
      continue;
    }
    if (only == nullptr || b.cell == only || b.cell.compare(0, std::strlen(only) + 1u, std::string(only) + ".") == 0) {
      mine.push_back(b);
    }
  }
  const std::vector<std::string> problems = against_baseline(found, mine);
  for (const std::string& p : problems) {
    std::printf("  %s\n", p.c_str());
  }
  CHECK_MESSAGE(problems.empty(), problems.size() << " difference(s) from tests/qanet/baseline_audio.txt (see above; QANET_WRITE_BASELINE=<path> writes the current list)");
}

// ---- the matrix ---------------------------------------------------------------------------------------------------------------------------

TEST_CASE("qanet audio matrix: pad against Pattern step for every instrument kind") {
  const auto t0 = std::chrono::steady_clock::now();
  std::vector<Finding> found;
  std::size_t runs = 0;
  double preview_onset = -1.0;
  struct Lag {
    std::string cell, path, first, line;
  };
  std::vector<Lag> lags;
  auto one = [&](const Cell& c, const Surface* fresh) {
    const Result r = run_cell(c, fresh);
    const std::string id = fresh != nullptr ? c.id + ".fresh" : c.id;
    CHECK_MESSAGE(r.error.empty(), id << ": " << r.error);
    std::printf("  %-26s%s\n", id.c_str(), r.log.c_str());
    runs += fresh != nullptr ? 1u : (c.chopped ? 3u : 2u) + 1u + (c.tb303 ? 0u : 1u) + (c.preview ? 1u : 0u);
    for (const Finding& f : r.findings) {
      found.push_back(f);
    }
    // THE LAG SIZED: a fresh run that found something is played again with the pad pressed 5, 20 and 50 ms after the edit (the first of them that agrees with the settled step ends the ladder). The answer is a
    // line of the log, `lag:`, one a surface, and not a finding (the baseline is the lag at 0 ms); `> 50 ms` is a difference that the first 50 ms do not cure.
    if (fresh != nullptr && !r.findings.empty()) {
      std::string line = c.id + " " + fresh->path + ": 0 ms " + r.findings.front().diff;
      std::string first = "> 50 ms";
      for (const unsigned d : {5u, 20u, 50u}) {
        const Result later = run_cell(c, fresh, d);
        CHECK_MESSAGE(later.error.empty(), c.id << " +" << d << " ms: " << later.error);
        if (later.findings.empty()) {
          first = std::to_string(d) + " ms";
          line += "; " + first + " matches";
          break;
        }
        line += "; " + std::to_string(d) + " ms " + later.findings.front().diff;
      }
      lags.push_back({c.id, fresh->path, first, line});
      // A lag of the 5 ms settle is the known one; one that the 20 ms rung does not cure is another bug (the ring cells size the long one: a voice that still sounds).
      const bool lag_ok = first == "5 ms" || first == "20 ms";
      CHECK_MESSAGE(lag_ok, "LAG ABOVE 20 ms (a different bug from the 5 ms rebind wait): " << line);
    }
    if (r.preview_onset >= 0.0) {
      preview_onset = r.preview_onset;
    }
  };
  const char* only = std::getenv("QANET_ONLY");
  for (const Cell& c : cells()) {
    if (only != nullptr && c.id != only) {
      continue;
    }
    one(c, nullptr);
    if (c.fresh) {
      one(c, &kPattern);
      one(c, &kInstrument);
      if (c.chopped) {
        one(c, &kWave);
      }
      one(c, &kMidi);
    }
  }
  REQUIRE_MESSAGE(runs > 0u, "no cell is named " << (only != nullptr ? only : "(all)"));
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("qanet audio matrix: %zu cells, %zu pad/step pairs, %zu findings, %.1f s. MIDI out: not a row (the simulator has no MIDI link, the instrument is silent).\n", cells().size(), runs, found.size(), secs);
  std::printf("qanet audio matrix: the F5 preview's first sound comes %.1f ms after the render starts (the render starts one tick before the key goes down; OS-QANET D3's budget is 150 ms)\n", preview_onset);
  for (const Finding& f : found) {
    std::printf("  finding: %s | %s | %s\n", f.cell.c_str(), f.paths.c_str(), f.diff.c_str());
  }
  for (const Lag& l : lags) {
    std::printf("  lag: %s\n", l.line.c_str());
  }

  finish(found, only, false);
}

// ---- C3 more: the ring cells ---------------------------------------------------------------------------------------------------------------

TEST_CASE("qanet audio matrix: a voice still sounding does not keep the stale descriptor (regression of OS-BUG-SLICEPITCH)") {
  const char* only = std::getenv("QANET_ONLY");
  std::vector<Finding> found;
  std::size_t ran = 0;
  for (const RingCase& rc : ring_cases()) {
    if (only != nullptr && rc.id != only) {
      continue;
    }
    ++ran;
    std::string error;
    const sim::Pcm ref = ring_reference(rc, error);
    REQUIRE_MESSAGE(error.empty(), rc.id << " (the settled step): " << error);
    const bool series = rc.kind == "series";
    const RingRun truth = run_ring(rc, series);
    REQUIRE_MESSAGE(truth.error.empty(), rc.id << ": " << truth.error);
    const RingRun heard = series ? truth : run_ring(rc, true);
    REQUIRE_MESSAGE(heard.error.empty(), rc.id << " (the probe): " << heard.error);
    // The age of the stale descriptor: from the apply of the edit (the model's play mode) to the rebind, at the bound when it never came.
    const int from = std::max(0, truth.apply_ms);
    const bool bounded = truth.rebind_ms < 0;
    const int stale = bounded ? kRingBoundMs : truth.rebind_ms - from;
    std::printf("  %-18s apply at %d ms, the pool first has no voice at %d ms, descriptor rebound %s; probe pad on the Wave screen at 100 ms:", rc.id.c_str(), truth.apply_ms, truth.quiet_ms,
                bounded ? "NOT within 5 s" : (std::to_string(truth.rebind_ms) + " ms after the key (stale " + std::to_string(stale) + " ms)").c_str());
    if (stale > 20) {
      char b[160];
      std::snprintf(b, sizeof b, "stale %d ms%s: the voices' descriptor is not the edit's while %s", stale, bounded ? " or more" : "",
                    rc.kind == "tail" ? "a long note of another instrument sounds" : rc.kind == "held" ? "a pad is held" : rc.kind == "series" ? "pads are played one after another" : "the transport plays");
      found.push_back({rc.id, "descriptor rebind", "stale", b, static_cast<double>(stale)});
    }
    const Measure pm = measure(heard.probe);
    std::printf(" %.1f Hz @%.1f ms against the settled step's %.1f Hz @%.1f ms\n", pm.hz, pm.onset, measure(ref).hz, measure(ref).onset);
    for (Finding& f : compare(rc.id, "pad:wave", heard.probe, ref)) {
      found.push_back(std::move(f));
    }
  }
  REQUIRE_MESSAGE(ran > 0u, "no ring cell is named " << (only != nullptr ? only : "(all)"));
  for (const Finding& f : found) {
    std::printf("  finding: %s | %s | %s\n", f.cell.c_str(), f.paths.c_str(), f.diff.c_str());
  }
  finish(found, only, true);
}

TEST_CASE("qanet audio matrix: every ring cell has its shard in audio_matrix.cmake") {
  std::string listed = " " O3_QANET_RING_CELLS " ";
  for (const RingCase& rc : ring_cases()) {
    CHECK_MESSAGE(listed.find(" " + rc.id + " ") != std::string::npos, rc.id << " has no ctest (add it to the ring list in tests/qanet/audio_matrix.cmake)");
    const std::size_t at = listed.find(" " + rc.id + " ");
    if (at != std::string::npos) {
      listed.erase(at, rc.id.size() + 1u);
    }
  }
  CHECK_MESSAGE(trim(listed).empty(), "a ring ctest of audio_matrix.cmake names no cell:" << listed);
}
