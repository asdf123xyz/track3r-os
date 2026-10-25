// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-H3B (new Community OS behaviour): Chords. The music (chords_math.h) against the OS's own chord table (seq/fx.h kChordMask, the oracle of the app's copy of the index map) and against an independent
// reckoning of the scales, and the module's host shared object driven through the ABI as the OS drives it, on a fake of the host tables (EditRig): I-V-vi-IV in C gives C, G, Am and F with the right
// FX 14 values, SEVENTHS, the RATE split, SPREAD voices, the default MODE by the instrument's kind (MIDI: CHORD FX; any other, and an OS of 1.5: SPREAD), the FX slots, the refusals, the page and the
// packed .tmod. The undo of a real APPLY (one unit named CHORDS) is in tests/module_edit_test.cpp. Host only.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../../../tests/o3m_test_support.h"

#include "../chords_math.h"

#include <cstring>
#include <o3os/seq/fx.h>

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

using namespace o3m_test;
using namespace o3os::module;
namespace ch = chords;
namespace sfx = o3os::seq;

namespace {

constexpr unsigned kRowMode = 7, kRowSevenths = 5, kRowRate = 3, kRowProg = 2, kRowScale = 1, kRowKey = 0, kRowOctave = 4, kRowInstr = 6;

void go_to(EditRig& r, unsigned row) {
  for (unsigned i = 0; i < 8; ++i) {
    r.press(O3M_BTN_UP);
  }
  r.down(row);
}

// A rig on a 4-track, 16-row pattern with the block over `tracks` tracks and `rows` rows, the context instrument 3 (the 0-based slot 2).
void setup(EditRig& r, unsigned block_tracks = 1, unsigned rows = 16) {
  r.api.pat_tracks = 4;
  r.launch_tools(0, 2, 0, rows, block_tracks);
}

std::string notes(const EditRig& r, unsigned track, unsigned rows = 16) {
  std::string s;
  for (unsigned j = 0; j < rows; ++j) {
    s += std::to_string(r.api.steps[track][j].note) + (j + 1 < rows ? "," : "");
  }
  return s;
}

}  // namespace

// ---- the music --------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("chords map: the app's index of each chord quality is the entry of the OS's 30 masks with exactly that chord's tones") {
  for (unsigned q = 0; q < ch::kQualityCount; ++q) {
    std::uint32_t want = 0;
    for (unsigned t = 0; t < ch::tone_count(q); ++t) {
      want |= 1u << ch::kTones[q][t];
    }
    CHECK_MESSAGE(sfx::kChordMask[ch::kChordIndex[q]] == want, "quality " << q << " index " << int(ch::kChordIndex[q]));
    CHECK(ch::kChordIndex[q] < sfx::kChordCount);
    // the sequencer's own reading of the value: the chord_mask accessor
    CHECK(sfx::chord_mask(ch::kChordIndex[q]) == want);
    // and no other entry of the table is the same chord
    unsigned same = 0;
    for (unsigned i = 0; i < sfx::kChordCount; ++i) {
      same += sfx::kChordMask[i] == want ? 1u : 0u;
    }
    CHECK(same == 1u);
  }
}

TEST_CASE("chords music: I-V-vi-IV in C is C, G, A minor and F; with sevenths Cmaj7, G7, Am7, Fmaj7; in C minor Cm, Gm, Ab, Fm") {
  ch::Setup u{0, 0, 0, 4, 4, 0, 1};
  struct W {
    unsigned root, quality, fx;
  };
  const W triads[4] = {{49, ch::kMaj, 5}, {56, ch::kMaj, 5}, {58, ch::kMin, 4}, {54, ch::kMaj, 5}};
  for (unsigned k = 0; k < 4; ++k) {
    const ch::Chord c = ch::chord_of(u, k);
    CHECK((c.root == triads[k].root && c.quality == triads[k].quality && ch::kChordIndex[c.quality] == triads[k].fx));
  }
  u.sevenths = 1;
  const W sevenths[4] = {{49, ch::kMaj7, 24}, {56, ch::kDom7, 23}, {58, ch::kMin7, 20}, {54, ch::kMaj7, 24}};
  for (unsigned k = 0; k < 4; ++k) {
    const ch::Chord c = ch::chord_of(u, k);
    CHECK((c.root == sevenths[k].root && c.quality == sevenths[k].quality && ch::kChordIndex[c.quality] == sevenths[k].fx));
  }
  u.sevenths = 0;
  u.minor = 1;
  const W minor[4] = {{49, ch::kMin, 4}, {56, ch::kMin, 4}, {57, ch::kMaj, 5}, {54, ch::kMin, 4}};
  for (unsigned k = 0; k < 4; ++k) {
    const ch::Chord c = ch::chord_of(u, k);
    CHECK((c.root == minor[k].root && c.quality == minor[k].quality && ch::kChordIndex[c.quality] == minor[k].fx));
  }
}

TEST_CASE("chords music: every key, scale, progression and octave gives chords whose tones are the scale's own notes (an independent reckoning) and stay in range") {
  const int major[7] = {0, 2, 4, 5, 7, 9, 11}, minor[7] = {0, 2, 3, 5, 7, 8, 10};
  for (unsigned prog = 0; prog < ch::kProgCount; ++prog) {
    for (unsigned key = 0; key < 12; ++key) {
      for (unsigned mn = 0; mn < 2; ++mn) {
        for (unsigned sev = 0; sev < 2; ++sev) {
          for (unsigned oct = ch::kMinOctave; oct <= ch::kMaxOctave; ++oct) {
            const ch::Setup u{key, mn, prog, 1, oct, sev, 1};
            for (unsigned k = 0; k < ch::chord_count(u); ++k) {
              const ch::Chord c = ch::chord_of(u, k);
              const int degree = ch::kProgs[prog].degree[k] - 1;
              const int root_pc = (static_cast<int>(key) + (mn ? minor : major)[degree]) % 12;
              CHECK(c.root == 12 * oct + root_pc + 1);
              // the chord's tones, as pitch classes, must all be notes of the scale (diatonic chords)
              for (unsigned t = 0; t < ch::tone_count(c.quality); ++t) {
                const int pc = (root_pc + ch::kTones[c.quality][t]) % 12;
                bool in_scale = false;
                for (int d = 0; d < 7; ++d) {
                  in_scale = in_scale || (static_cast<int>(key) + (mn ? minor : major)[d]) % 12 == pc;
                }
                CHECK_MESSAGE(in_scale, "prog " << prog << " key " << key << " minor " << mn << " chord " << k << " tone " << t);
              }
              CHECK(c.root + 11 <= 127);
              CHECK(ch::kChordIndex[c.quality] < sfx::kChordCount);
              CHECK((sfx::kChordMask[ch::kChordIndex[c.quality]] & 1u) == 1u);  // the root is in the mask
            }
          }
        }
      }
    }
  }
}

TEST_CASE("chords music: the default RATE divides the rows by the chords, the progression plays once, and a RATE that is too large drops the chords that do not fit") {
  ch::Setup u{0, 0, 0, 1, 4, 0, 1};  // 4 chords
  CHECK(ch::default_rate(u, 16) == 4u);
  CHECK(ch::default_rate(u, 3) == 1u);
  u.prog = 2;  // ii-V-I
  CHECK(ch::default_rate(u, 16) == 5u);
  u.prog = 7;  // the blues, 12 chords
  CHECK(ch::default_rate(u, 16) == 1u);
  CHECK(ch::default_rate(u, 64) == 5u);
  u.prog = 0;
  u.rate = 4;
  CHECK(ch::chords_fitting(u, 16) == 4u);
  CHECK(ch::chords_fitting(u, 13) == 4u);  // the fourth starts on row 12
  CHECK(ch::chords_fitting(u, 12) == 3u);
  u.rate = 8;
  CHECK(ch::chords_fitting(u, 16) == 2u);
  CHECK(ch::chord_at_row(u, 16, 0) == 0);
  CHECK(ch::chord_at_row(u, 16, 8) == 1);
  CHECK(ch::chord_at_row(u, 16, 4) == -1);
  CHECK(ch::chord_at_row(u, 32, 24) == 3);
  CHECK(ch::chord_at_row(u, 32, 32) == -1);
}

// ---- the app ----------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("chords app: descriptor") {
  EditRig r;
  NEED(r.lib.open(O3M_CHORDS_HOST_SO));
  const o3m_module& m = *r.lib.entry;
  CHECK((m.abi_major == 1u && m.abi_minor == 10u && m.kind == O3M_KIND_APP));  // (1.10: its eight rows are declared and kept with the project, OS-APPUX S3)
  // ABI 1.9 (OS-KEYROWDLG S5): the labels of the page's key row; F8 is the OS's BACK, so the app labels F1..F7 only; Shift + Fn runs Fn (no app reads mods), so the Shift row says the same
  CHECK((m.ui != nullptr && m.ui->keys != nullptr && m.ui->struct_size == O3M_UI_SIZE(9)));
  static const o3m_keys no_keys = {{nullptr}, {nullptr}};
  const o3m_keys* keys = m.ui != nullptr && m.ui->keys != nullptr ? m.ui->keys(nullptr) : nullptr;
  CHECK(keys != nullptr);
  keys = keys != nullptr ? keys : &no_keys;
  CHECK((keys->f[0] != nullptr && std::strcmp(keys->f[0], "APPLY") == 0));
  CHECK((keys->shift_f[0] != nullptr && std::strcmp(keys->shift_f[0], "APPLY") == 0));  // Shift + Fn runs Fn (no app reads mods): the Shift row says the same
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
  CHECK(std::string(m.name) == "Chords");
  CHECK((m.view_count == 1u && m.param_count == 8u && m.set_param != nullptr));  // KEY SCALE PROG RATE OCTAVE SEVENTHS INSTR MODE: declared as storage (the page draws them itself)
}

TEST_CASE("chords app: on a MIDI instrument the default is CHORD FX: C, G, Am, F on rows 0, 4, 8, 12 with the instrument and FX 14 values 5, 5, 4, 5 in FX2, and nothing on the other rows") {
  EditRig r;
  setup(r);
  r.api.instr[3].kind = O3M_INSTRUMENT_MIDI;
  for (unsigned j = 0; j < 16; ++j) {
    r.api.steps[0][j].note = 70;  // old notes: the rows between the chords are emptied
  }
  NEED(r.start(O3M_CHORDS_HOST_SO));
  CHECK(EditRig::shows(r.draw(), "CHORD FX AUTO"));
  r.press(O3M_BTN_F1);
  NEED(r.api.groups.size() == 1u);
  NEED(r.api.groups[0].size() == 1u);
  CHECK(r.api.groups[0][0].fields == (O3M_STEP_NOTE | O3M_STEP_INSTR | O3M_STEP_FX1 | O3M_STEP_FX2));
  CHECK((r.api.groups[0][0].track == 0u && r.api.groups[0][0].first == 0u && r.api.groups[0][0].count == 16u));
  CHECK(notes(r, 0) == "49,0,0,0,56,0,0,0,58,0,0,0,54,0,0,0");
  const unsigned fx[4] = {5, 5, 4, 5};
  for (unsigned k = 0; k < 4; ++k) {
    const o3m_step& s = r.api.steps[0][4 * k];
    CHECK((s.instr == 3 && s.fx2_type == 14 && s.fx2_value == fx[k] && s.fx1_type == 0));
  }
  CHECK(EditRig::shows(r.draw(), "4 CHORDS WRITTEN"));
}

TEST_CASE("chords app: the default MODE follows the instrument's kind: MIDI is CHORD FX, a sample, a module, an empty slot and an OS that cannot say are SPREAD; moving MODE stops it following") {
  struct Case {
    std::uint32_t kind;
    const char* mode;
  };
  for (const Case& c : {Case{O3M_INSTRUMENT_MIDI, "CHORD FX AUTO"}, Case{O3M_INSTRUMENT_SAMPLE, "SPREAD AUTO"}, Case{O3M_INSTRUMENT_MODULE, "SPREAD AUTO"}, Case{O3M_INSTRUMENT_EMPTY, "SPREAD AUTO"}}) {
    EditRig r;
    setup(r);
    r.api.instr[3].kind = c.kind;
    NEED(r.start(O3M_CHORDS_HOST_SO));
    CHECK_MESSAGE(EditRig::shows(r.draw(), c.mode), c.mode);
  }
  {
    EditRig r;  // an OS of 1.5: no instrument_info
    setup(r);
    r.api.instr[3].kind = O3M_INSTRUMENT_MIDI;
    r.api.make_abi15();
    NEED(r.start(O3M_CHORDS_HOST_SO));
    CHECK(EditRig::shows(r.draw(), "SPREAD AUTO"));
    CHECK(r.api.info_reads == 0);
  }
  {
    EditRig r;
    setup(r, 3);
    r.api.instr[3].kind = O3M_INSTRUMENT_SAMPLE;
    r.api.instr[5].kind = O3M_INSTRUMENT_MIDI;
    NEED(r.start(O3M_CHORDS_HOST_SO));
    go_to(r, kRowMode);
    r.turn(-1);  // CHORD FX by hand
    CHECK(EditRig::shows(r.draw(), "CHORD FX"));
    CHECK(!EditRig::shows(r.draw(), "CHORD FX AUTO"));
    go_to(r, kRowInstr);
    r.turn(2);  // instrument 5, a MIDI instrument: the default would be CHORD FX; and 3 -> 5 leaves it as chosen
    CHECK(EditRig::shows(r.draw(), "CHORD FX"));
    go_to(r, kRowMode);
    r.press(O3M_BTN_DELETE);  // back to the default
    CHECK(EditRig::shows(r.draw(), "CHORD FX AUTO"));
    go_to(r, kRowInstr);
    r.turn(-2);  // instrument 3 again, a sample: the default follows
    CHECK(EditRig::shows(r.draw(), "SPREAD AUTO"));
  }
}

TEST_CASE("chords app: SEVENTHS writes the seventh chords' FX values 24, 23, 20, 24; KEY, SCALE and OCTAVE move the roots; the second slot goes to FX1 when FX2 is taken") {
  EditRig r;
  setup(r);
  r.api.instr[3].kind = O3M_INSTRUMENT_MIDI;
  r.api.steps[0][4].fx2_type = 8;  // FX2 taken on the second chord's row
  r.api.steps[0][4].fx2_value = 9;
  NEED(r.start(O3M_CHORDS_HOST_SO));
  go_to(r, kRowSevenths);
  r.turn(1);
  r.press(O3M_BTN_F1);
  NEED(r.api.groups.size() == 1u);
  CHECK(r.api.steps[0][0].fx2_value == 24);
  CHECK((r.api.steps[0][4].fx1_type == 14 && r.api.steps[0][4].fx1_value == 23 && r.api.steps[0][4].fx2_type == 8 && r.api.steps[0][4].fx2_value == 9));
  CHECK(r.api.steps[0][8].fx2_value == 20);
  CHECK(r.api.steps[0][12].fx2_value == 24);
  // D minor in octave 3
  go_to(r, kRowKey);
  r.turn(2);
  go_to(r, kRowScale);
  r.turn(1);
  go_to(r, kRowOctave);
  r.turn(-1);
  r.press(O3M_BTN_F1);
  CHECK(r.api.steps[0][0].note == 12 * 3 + 2 + 1);  // D-3
  CHECK(r.api.steps[0][0].fx2_value == 20);         // Dm7
  CHECK(EditRig::shows(r.draw(), "Dm7"));
}

TEST_CASE("chords app: a row whose two FX slots hold other FX gets the root without the Chord FX and the page says so; a Chord FX that is there is replaced in place") {
  EditRig r;
  setup(r);
  r.api.instr[3].kind = O3M_INSTRUMENT_MIDI;
  r.api.steps[0][0].fx1_type = 8;
  r.api.steps[0][0].fx2_type = 9;
  r.api.steps[0][4].fx1_type = 14;  // an old chord in FX1: replaced there
  r.api.steps[0][4].fx1_value = 29;
  NEED(r.start(O3M_CHORDS_HOST_SO));
  r.press(O3M_BTN_F1);
  CHECK((r.api.steps[0][0].note == 49 && r.api.steps[0][0].fx1_type == 8 && r.api.steps[0][0].fx2_type == 9));
  CHECK((r.api.steps[0][4].fx1_type == 14 && r.api.steps[0][4].fx1_value == 5 && r.api.steps[0][4].fx2_type == 0));
  CHECK(EditRig::shows(r.draw(), "1 WITHOUT FX: NO FREE SLOT"));
}

TEST_CASE("chords app: the RATE split: ii-V-I on 16 rows is 5 rows each; a RATE of 8 keeps the first two chords; RATE follows the progression until it is set by hand, Delete gives AUTO back") {
  EditRig r;
  setup(r);
  r.api.instr[3].kind = O3M_INSTRUMENT_MIDI;
  NEED(r.start(O3M_CHORDS_HOST_SO));
  go_to(r, kRowProg);
  r.turn(2);  // ii-V-I
  CHECK(EditRig::shows(r.draw(), "5 AUTO"));
  r.press(O3M_BTN_F1);
  CHECK(notes(r, 0) == "51,0,0,0,0,56,0,0,0,0,49,0,0,0,0,0");  // Dm (ii), G (V), C (I) in octave 4
  go_to(r, kRowRate);
  r.turn(3);  // 8
  CHECK(EditRig::shows(r.draw(), "8"));
  CHECK(!EditRig::shows(r.draw(), "8 AUTO"));
  r.press(O3M_BTN_F1);
  CHECK(notes(r, 0) == "51,0,0,0,0,0,0,0,56,0,0,0,0,0,0,0");
  go_to(r, kRowProg);
  r.turn(-2);  // I-V-vi-IV: RATE stays at the hand-set 8
  CHECK(!EditRig::shows(r.draw(), "8 AUTO"));
  go_to(r, kRowRate);
  r.press(O3M_BTN_DELETE);
  CHECK(EditRig::shows(r.draw(), "4 AUTO"));
}

TEST_CASE("chords app: SPREAD puts the root, third, fifth (and seventh) on the first 3 (4) tracks of the block, same rows, no FX, and says when the block has too few tracks") {
  EditRig r;
  setup(r, 4);
  r.api.instr[3].kind = O3M_INSTRUMENT_SAMPLE;
  for (unsigned t = 0; t < 4; ++t) {
    r.api.steps[t][2].note = 90;  // an old note between the chords
    r.api.steps[t][0].fx1_type = 8;
    r.api.steps[t][0].fx1_value = 5;
  }
  NEED(r.start(O3M_CHORDS_HOST_SO));
  r.press(O3M_BTN_F1);
  NEED(r.api.groups.size() == 1u);
  CHECK(r.api.groups[0].size() == 3u);  // three tracks of the four
  for (const auto& w : r.api.groups[0]) {
    CHECK(w.fields == (O3M_STEP_NOTE | O3M_STEP_INSTR));
  }
  CHECK(notes(r, 0) == "49,0,0,0,56,0,0,0,58,0,0,0,54,0,0,0");  // roots C G A F
  CHECK(notes(r, 1) == "53,0,0,0,60,0,0,0,61,0,0,0,58,0,0,0");  // thirds: C maj E (53), G maj B (60), A min C (61), F maj A (58)
  CHECK(notes(r, 2) == "56,0,0,0,63,0,0,0,65,0,0,0,61,0,0,0");  // fifths: G, D, E, C
  CHECK(r.api.steps[3][2].note == 90);                            // the fourth track is not part of a triad's spread
  CHECK((r.api.steps[0][0].fx1_type == 8 && r.api.steps[0][0].fx1_value == 5));  // FX untouched
  CHECK(r.api.steps[2][4].instr == 3);
  // SEVENTHS: four tracks, the seventh on the fourth
  go_to(r, kRowSevenths);
  r.turn(1);
  r.press(O3M_BTN_F1);
  CHECK(r.api.groups.back().size() == 4u);
  CHECK(notes(r, 3) == "60,0,0,0,66,0,0,0,68,0,0,0,65,0,0,0");  // sevenths: Cmaj7 B (60), G7 F (66), Am7 G (68), Fmaj7 E (65)
}

TEST_CASE("chords app: SPREAD on a block of two tracks writes two tones and the page says how many were left out; with no block CHORD FX and SPREAD use the cursor's track") {
  EditRig r;
  setup(r, 2);
  r.api.instr[3].kind = O3M_INSTRUMENT_SAMPLE;
  NEED(r.start(O3M_CHORDS_HOST_SO));
  r.press(O3M_BTN_F1);
  CHECK(r.api.groups.back().size() == 2u);
  CHECK(EditRig::shows(r.draw(), "1 TONES LEFT OUT: TOO FEW TRACKS"));
  // no block: the cursor's track from the cursor row to the end of the track (rows 6..15 here)
  EditRig q;
  q.api.pat_tracks = 4;
  q.launch_tools(1, 2, 6, 0, 0);
  q.api.instr[3].kind = O3M_INSTRUMENT_MIDI;
  NEED(q.start(O3M_CHORDS_HOST_SO));
  q.press(O3M_BTN_F1);
  NEED(q.api.groups.size() == 1u);
  CHECK((q.api.groups[0][0].track == 1u && q.api.groups[0][0].first == 6u && q.api.groups[0][0].count == 10u));
  CHECK(q.api.steps[1][6].note == 49);
  CHECK(q.api.steps[1][6 + 2].note == 56);  // 10 rows / 4 chords = 2
  CHECK(q.api.steps[1][5].note == 0);
}

TEST_CASE("chords app: a launch with no block on a pattern that does not exist, then PATTERN moved to one that does: ROWS is the end of the track, APPLY writes it, and Delete on ROWS gives it back (OS-APPUX S5)") {
  EditRig q;
  q.api.pat_tracks = 4;
  q.api.pattern_id = 3;  // the project has pattern 3 only; the cursor was on pattern 1, track 2, row 7
  q.launch_tools(1, 2, 6, 0, 0);
  q.api.instr[3].kind = O3M_INSTRUMENT_MIDI;
  NEED(q.start(O3M_CHORDS_HOST_SO));
  q.right();  // the target page: PATTERN TRACK ROW ROWS TRACKS
  CHECK(EditRig::shows(q.draw(), "1 (EMPTY)"));
  q.turn(2);  // PATTERN 3
  CHECK_FALSE(EditRig::shows(q.draw(), "(EMPTY)"));
  q.press(O3M_BTN_F1);
  NEED(q.api.groups.size() == 1u);
  NEED(q.api.groups[0].size() == 1u);
  CHECK((q.api.groups[0][0].track == 1u && q.api.groups[0][0].first == 6u && q.api.groups[0][0].count == 10u));  // 16 - 6 rows to the end of the track, not nothing
  CHECK(q.api.steps[1][6].note == 49);
  q.down(3);   // ROWS
  q.turn(-6);  // 10 -> 4
  q.press(O3M_BTN_F1);
  NEED(q.api.groups.size() == 2u);
  CHECK(q.api.groups[1][0].count == 4u);
  q.press(O3M_BTN_DELETE);  // the launch's "to the end of the track", not 1
  q.press(O3M_BTN_F1);
  NEED(q.api.groups.size() == 3u);
  CHECK(q.api.groups[2][0].count == 10u);
}

TEST_CASE("chords app: the refusals: opened from the app list, a pattern that does not exist, no edit table, an edit_end that fails, an OS below 1.5") {
  {
    EditRig r;
    NEED(r.start(O3M_CHORDS_HOST_SO));
    r.press(O3M_BTN_F1);
    CHECK(r.api.begins == 0);
    CHECK(EditRig::shows(r.draw(), "OPEN IT FROM PATTERN F7 TOOLS"));
  }
  {
    EditRig r;
    setup(r);
    r.api.pattern_id = 9;
    NEED(r.start(O3M_CHORDS_HOST_SO));
    r.press(O3M_BTN_F1);
    CHECK(r.api.begins == 0);
    CHECK(EditRig::shows(r.draw(), "THE PATTERN DOES NOT EXIST YET"));
  }
  {
    EditRig r;
    setup(r);
    NEED(r.start(O3M_CHORDS_HOST_SO, O3M_CAP_READ_PROJECT));
    r.press(O3M_BTN_F1);
    CHECK(r.api.begins == 0);
    CHECK(EditRig::shows(r.draw(), "THIS HOST CANNOT EDIT"));
  }
  {
    EditRig r;
    setup(r);
    r.api.end_result = O3M_EDIT_LIMIT;
    NEED(r.start(O3M_CHORDS_HOST_SO));
    r.press(O3M_BTN_F1);
    CHECK(r.api.groups.empty());
    CHECK(EditRig::shows(r.draw(), "NOT APPLIED  (CODE 5)"));
  }
  {
    EditRig r;
    CHECK(!r.start(O3M_CHORDS_HOST_SO, 0));
  }
}

TEST_CASE("chords app: the page stays inside the canvas and overlaps nothing on every row, in both modes, at the smallest and the real size") {
  for (auto size : {std::pair<int, int>{480, 288}, std::pair<int, int>{800, 330}}) {
    for (bool blues : {false, true}) {
      EditRig r;
      setup(r, 4, 32);
      for (auto& n : r.api.pat_len) {
        n = 32;
      }
      r.api.instr[3].kind = O3M_INSTRUMENT_MIDI;
      NEED(r.start(O3M_CHORDS_HOST_SO));
      if (blues) {
        go_to(r, kRowProg);
        r.turn(7);
      }
      for (unsigned row = 0; row < 8; ++row) {
        r.check_layout(size.first, size.second, blues ? "blues" : "fresh");
        r.press(O3M_BTN_DOWN);
      }
      r.press(O3M_BTN_F1);
      r.check_layout(size.first, size.second, "applied");
      go_to(r, kRowMode);
      r.turn(1);
      r.press(O3M_BTN_F1);
      r.check_layout(size.first, size.second, "spread applied");
    }
  }
}

TEST_CASE("chords app: the packed .tmod loads, declares ABI 1.5 with read_project and edit_project, and is small") {
  const Tmod t = load_tmod(O3M_CHORDS_TMOD);
  if (!t.present) {
    return;
  }
  CHECK(t.header == Status::Ok);
  CHECK(t.crc_ok);
  CHECK(t.relocation == Status::Ok);
  CHECK(t.descriptor == Status::Ok);
  CHECK((t.h.abi_major == 1u && t.h.abi_minor == 10u));
  CHECK(t.h.kind == O3M_KIND_APP);
  CHECK(t.h.reserved2[0] == (O3M_CAP_READ_PROJECT | O3M_CAP_EDIT_PROJECT));
  CHECK(t.h.module_version == O3M_VERSION(1, 0, 0));
  CHECK(t.h.state_bytes <= 1024u);
  CHECK(t.file_bytes < 14u * 1024u);  // the module slab is 14 KiB
  std::printf("chords.tmod: %u bytes, state %u, hot %u\n", t.file_bytes, t.h.state_bytes, t.h.hot_size);
}

TEST_CASE("chords app: the eight rows are kept with set_own_param (RATE, INSTR and MODE with an AUTO written 0), handed back through set_param before enter(), and enter() leaves them alone (OS-APPUX S3)") {
  EditRig r;
  r.launch_tools(0, 4, 0, 16, 1);
  NEED(r.start(O3M_CHORDS_HOST_SO));
  CHECK(r.api.own_params.empty());
  r.turn(2);  // KEY
  NEED(r.api.own_params.size() == 1u);
  CHECK((r.api.own_params[0].index == 0u && r.api.own_params[0].value == 2.0f));
  r.down();
  r.turn(1);  // SCALE: MINOR
  CHECK((r.api.own_params.size() == 2u && r.api.own_params[1].index == 1u && r.api.own_params[1].value == 1.0f));
  r.down(2);  // RATE: AUTO, a change makes it a number, Delete makes it AUTO again (0)
  r.turn(1);
  CHECK((r.api.own_params.size() == 3u && r.api.own_params[2].index == 3u && r.api.own_params[2].value >= 2.0f));
  r.press(O3M_BTN_DELETE);
  CHECK((r.api.own_params.size() == 4u && r.api.own_params[3].index == 3u && r.api.own_params[3].value == 0.0f));
  r.down(3);  // INSTR: AUTO is the context's instrument (5)
  r.turn(1);
  CHECK((r.api.own_params.size() == 5u && r.api.own_params[4].index == 6u && r.api.own_params[4].value == 6.0f));
  r.press(O3M_BTN_DELETE);
  CHECK((r.api.own_params.size() == 6u && r.api.own_params[5].index == 6u && r.api.own_params[5].value == 0.0f));
  r.down();  // MODE: AUTO, then SPREAD (2), Delete is AUTO (0)
  r.turn(1);
  CHECK((r.api.own_params.size() == 7u && r.api.own_params[6].index == 7u && r.api.own_params[6].value == 2.0f));
  r.press(O3M_BTN_DELETE);
  CHECK((r.api.own_params.size() == 8u && r.api.own_params[7].index == 7u && r.api.own_params[7].value == 0.0f));
  // a relaunch: the kept values come back before enter() (KEY 7, MINOR, PROG 3, RATE 6, OCTAVE 5, SEVENTHS, INSTR 12, MODE FX), enter() resets none, and a different block moves only the AUTOs
  r.launch_tools(0, 20, 0, 8, 1);
  const float kept[8] = {7, 1, 3, 6, 5, 1, 12, 1};
  for (unsigned i = 0; i < 8; ++i) {
    r.inst.set(i, kept[i]);
  }
  r.enter();
  Drawing d = r.draw();
  CHECK(r.shows(d, "G"));      // KEY 7 is G
  CHECK(r.shows(d, "MINOR"));
  CHECK(r.shows(d, "12"));     // INSTR 12
  CHECK(r.shows(d, "FX"));     // MODE CHORD FX
  r.inst.set(3, 0.0f);  // RATE AUTO, INSTR AUTO
  r.inst.set(6, 0.0f);
  r.enter();
  d = r.draw();
  CHECK(r.shows(d, "21"));  // INSTR AUTO: the new context's instrument (slot 20 + 1)
  CHECK(r.shows(d, "AUTO"));
}
