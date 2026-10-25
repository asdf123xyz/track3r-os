// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-H3A (new Community OS behaviour): Rhythm Gen. The arithmetic (rhythmgen_math.h) against the Fill dialog's Euclid (model::fill, the oracle of the rule), and the module's host shared object
// driven through the ABI as the OS drives it, on a fake of the ABI 1.5 host tables (o3m_test::FakeApi: a launch context, a small project, edit_begin / set_steps / edit_end): the lanes of a
// block, ROTATE, PROB 0 and 100, the same SEED giving the same steps, ADD keeping notes, FX untouched, one group per APPLY and per VARY, four lanes with different LENGTHs, the refusals, the
// draw budgets on every page, plus the packed .tmod. The undo of a real APPLY (one unit) is in tests/module_edit_test.cpp. Host only.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../../../tests/o3m_test_support.h"

#include "../rhythmgen_math.h"

#include <o3os/model/pattern_fill.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

using namespace o3m_test;
using namespace o3os::module;
namespace rg = rhythmgen;
namespace fill = o3os::model::fill;

namespace {

constexpr std::uint32_t kBoth = O3M_CAP_READ_PROJECT | O3M_CAP_EDIT_PROJECT;

struct App {
  Lib lib;
  Instance inst;
  FakeApi api;
  ViewHost views;
  bool ok = false;
  App(std::uint32_t caps = kBoth, bool tools = true, std::uint32_t tracks = 2, std::uint32_t block_tracks = 2, std::uint32_t block_len = 16, std::uint32_t row0 = 0, std::uint32_t pat_rows = 16) {
    ViewHost::Metrics m;  // the theme's faces: Display 16 x 32, Data 10 x 20, Label 8 x 16, Micro 6 x 12
    const std::int16_t cw[O3M_FACE_COUNT] = {16, 10, 8, 6}, ch[O3M_FACE_COUNT] = {32, 20, 16, 12};
    for (unsigned f = 0; f < O3M_FACE_COUNT; ++f) {
      m.cell_w[f] = cw[f];
      m.cell_h[f] = ch[f];
    }
    views.set_metrics(m);
    api.pat_tracks = tracks;
    for (auto& n : api.pat_len) {
      n = static_cast<std::uint8_t>(pat_rows);
    }
    for (auto& t : api.steps) {
      for (auto& s : t) {
        s = o3m_step{0, 0, 255, 0, 0, 0, 0, 0};
      }
    }
    api.launch = {sizeof(o3m_launch_ctx), tools ? O3M_LAUNCH_FROM_TOOLS : 0u, 1, 0, 2, row0, block_len, static_cast<std::uint8_t>(block_tracks), {0, 0, 0}};
    ok = lib.open(O3M_RHYTHMGEN_HOST_SO) && inst.start_with(*lib.entry, make_app_host(api, caps));
    if (ok) {
      inst.m->ui->enter(inst.state);
    }
  }
  void event(std::uint32_t kind, std::uint32_t id, std::int32_t value) {
    const o3m_ui_event ev = {kind, id, value, 0};
    CHECK(views.call_input(*lib.entry, inst.state, 1, ev) == ViewStatus::Ok);
  }
  void press(std::uint32_t id) { event(O3M_EV_BUTTON, id, 1); }
  void turn(int n) { event(O3M_EV_ENCODER, 0, n); }
  void down(unsigned n = 1) { for (unsigned i = 0; i < n; ++i) press(O3M_BTN_DOWN); }
  void right(unsigned n = 1) { for (unsigned i = 0; i < n; ++i) press(O3M_BTN_RIGHT); }
  void left(unsigned n = 1) { for (unsigned i = 0; i < n; ++i) press(O3M_BTN_LEFT); }
  Drawing draw(int w = 800, int h = 330) {
    Drawing d;
    CHECK(views.draw(*lib.entry, inst.state, 1, 0, w, h, nullptr, 0, kViewMaxWords) == ViewStatus::Ok);
    views.replay(d);
    return d;
  }
  bool shows(const Drawing& d, const char* s) {
    for (const auto& t : d.texts) {
      if (t.s.find(s) != std::string::npos) {
        return true;
      }
    }
    return false;
  }
  // the notes of a lane's 16 rows, 0 = empty
  std::string notes(unsigned track, unsigned rows = 16) const {
    std::string s;
    for (unsigned r = 0; r < rows; ++r) {
      s += api.steps[track][r].note != 0 ? 'X' : '.';
    }
    return s;
  }
};

std::string cycle(unsigned hits, unsigned length, unsigned rotate, unsigned rows) {
  std::string s;
  for (unsigned j = 0; j < rows; ++j) {
    s += rg::cycle_hit(hits, length, rotate, j) ? 'X' : '.';
  }
  return s;
}

}  // namespace

// ---- the rule ---------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("rhythm gen rule: the hits are the Euclid rule of the Fill dialog, for every hits and length") {
  for (unsigned length = 1; length <= rg::kMaxLength; ++length) {
    for (unsigned hits = 0; hits <= length + 2; ++hits) {  // (more hits than steps is clamped by both)
      for (unsigned i = 0; i < length; ++i) {
        CHECK_MESSAGE(rg::euclid_hit(hits, length, i) == fill::euclid_hit(hits, length, i), hits << "/" << length << " row " << i);
      }
    }
  }
}

TEST_CASE("rhythm gen rule: 3/8 is the Fill dialog's Euclid fill of eight empty steps") {
  fill::Plan plan;
  plan.type = fill::Type::Euclid;
  plan.what = fill::What::Note;
  plan.events = 3;
  o3os::model::Step steps[8];
  for (auto& s : steps) {
    s = o3os::model::kEmptyStep;
  }
  fill::Rng rng;
  o3os::model::edit::Last last{1, 0};
  (void)fill::apply(plan, steps, 8, rng, last);
  std::string fill_hits;
  for (const auto& s : steps) {
    fill_hits += s.note != 0 ? 'X' : '.';
  }
  CHECK(fill_hits == "X..X..X.");
  CHECK(cycle(3, 8, 0, 8) == fill_hits);
  CHECK(cycle(3, 8, 0, 16) == "X..X..X.X..X..X.");  // the cycle repeats over the block
}

TEST_CASE("rhythm gen rule: ROTATE moves the cycle later, wrapping, and 0 or LENGTH is the plain cycle") {
  CHECK(cycle(3, 8, 0, 8) == "X..X..X.");
  CHECK(cycle(3, 8, 1, 8) == ".X..X..X");
  CHECK(cycle(3, 8, 2, 8) == "X.X..X..");
  CHECK(cycle(3, 8, 8, 8) == cycle(3, 8, 0, 8));
  for (unsigned r = 0; r < 8; ++r) {
    CHECK(cycle(3, 8, r, 8).find('X') != std::string::npos);
    unsigned n = 0;
    for (char c : cycle(3, 8, r, 8)) n += c == 'X';
    CHECK(n == 3u);  // rotation never changes how many
  }
}

TEST_CASE("rhythm gen rule: the draw is deterministic, one per hit, and a higher PROB only adds hits") {
  rg::Draw a, b;
  a.seed(7, 1);
  b.seed(7, 1);
  for (int i = 0; i < 100; ++i) {
    CHECK(a.next() == b.next());
  }
  rg::Draw c;
  c.seed(7, 2);
  rg::Draw d;
  d.seed(8, 1);
  a.seed(7, 1);
  CHECK(a.s != c.s);  // another lane, another sequence
  CHECK(a.s != d.s);  // another seed too
  for (unsigned s = 0; s <= 9999; s += 997) {
    for (unsigned lane = 0; lane < 4; ++lane) {
      rg::Draw z;
      z.seed(s, lane);
      CHECK(z.s != 0u);
    }
  }
  // monotone in PROB: the same draws, so every hit kept at 30 % is kept at 60 %
  rg::LaneSetup l{16, 32, 0, 49, 30, 1, 255, true, 5, 0};
  struct St { std::uint8_t note, instr, vol; };
  St cur[64] = {}, lo[64], hi[64];
  rg::lane_steps(l, cur, lo, 64);
  l.prob = 60;
  rg::lane_steps(l, cur, hi, 64);
  unsigned nlo = 0, nhi = 0;
  for (unsigned i = 0; i < 64; ++i) {
    CHECK((lo[i].note == 0 || hi[i].note != 0));
    nlo += lo[i].note != 0;
    nhi += hi[i].note != 0;
  }
  CHECK(nlo < nhi);
  l.prob = 0;
  CHECK(rg::lane_steps(l, cur, lo, 64) == 0u);
  l.prob = 100;
  CHECK(rg::lane_steps(l, cur, hi, 64) == 32u);  // 16 hits of a cycle of 32, over 64 rows
}

// ---- the module ---------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("rhythm gen app: descriptor") {
  App a;
  NEED(a.ok);
  const o3m_module& m = *a.lib.entry;
  CHECK(m.kind == O3M_KIND_APP);
  CHECK(std::string(m.name) == "Rhythm Gen");
  CHECK((m.abi_major == 1u && m.abi_minor == 10u));  // (1.10: its 24 rows are declared and kept with the project, and it plays a lane's hits: OS-APPUX S3)
  // ABI 1.9 (OS-KEYROWDLG S5): the labels of the page's key row; F8 is the OS's BACK, so the app labels F1..F7 only; Shift + Fn runs Fn (no app reads mods), so the Shift row says the same
  CHECK((m.ui != nullptr && m.ui->keys != nullptr && m.ui->struct_size == O3M_UI_SIZE(9)));
  static const o3m_keys no_keys = {{nullptr}, {nullptr}};
  const o3m_keys* keys = m.ui != nullptr && m.ui->keys != nullptr ? m.ui->keys(nullptr) : nullptr;
  CHECK(keys != nullptr);
  keys = keys != nullptr ? keys : &no_keys;
  CHECK((keys->f[0] != nullptr && std::strcmp(keys->f[0], "APPLY") == 0));
  CHECK((keys->shift_f[0] != nullptr && std::strcmp(keys->shift_f[0], "APPLY") == 0));  // Shift + Fn runs Fn (no app reads mods): the Shift row says the same
  CHECK((keys->f[1] != nullptr && std::strcmp(keys->f[1], "VARY") == 0));
  CHECK((keys->shift_f[1] != nullptr && std::strcmp(keys->shift_f[1], "VARY") == 0));
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
  CHECK(m.param_count == 24u);  // 4 lanes x (HITS LENGTH ROTATE NOTE PROB), then INSTR VOL MODE SEED: declared as storage (the page draws them itself)
  CHECK((m.render == nullptr && m.note_on == nullptr && m.note_off == nullptr && m.set_param != nullptr));
}

TEST_CASE("rhythm gen app: a block of 2 tracks x 16 rows, F1 writes both lanes as ONE group, the notes of the Euclid rule, FX and untouched fields as they were") {
  App a;
  NEED(a.ok);
  for (unsigned t = 0; t < 2; ++t) {
    for (unsigned r = 0; r < 16; ++r) {
      a.api.steps[t][r].fx1_type = 7;  // an FX on every step
      a.api.steps[t][r].fx1_value = static_cast<std::uint8_t>(r);
    }
  }
  a.press(O3M_BTN_F1);
  CHECK((a.api.begins == 1 && a.api.ends == 1));
  NEED(a.api.groups.size() == 1u);
  NEED(a.api.groups[0].size() == 2u);  // one set_steps per lane
  for (unsigned l = 0; l < 2; ++l) {
    const FakeApi::Write& w = a.api.groups[0][l];
    CHECK((w.pattern == 1u && w.track == l && w.first == 0u && w.count == 16u));
    CHECK(w.fields == (O3M_STEP_NOTE | O3M_STEP_INSTR | O3M_STEP_VOL));  // the FX are never in the mask
  }
  // default LENGTH is the block's 16, HITS a quarter of it: 4/16 = rows 0, 4, 8, 12; the note is C-4 (the step byte 49), the instrument the context's slot + 1
  CHECK(a.notes(0) == "X...X...X...X...");
  CHECK(a.notes(1) == "X...X...X...X...");
  for (unsigned r = 0; r < 16; ++r) {
    CHECK(a.api.steps[0][r].fx1_type == 7);
    CHECK(a.api.steps[0][r].fx1_value == r);
    CHECK(a.api.steps[1][r].fx1_type == 7);
    if (a.api.steps[0][r].note != 0) {
      CHECK(a.api.steps[0][r].note == 49);
      CHECK(a.api.steps[0][r].instr == 3);  // slot 2, as the Pattern shows it
      CHECK(a.api.steps[0][r].vol == 255);  // VOL is unset: the volume is left alone
    }
  }
  // rows past the block are not touched (the block here is the whole pattern; a longer pattern is checked below)
}

TEST_CASE("rhythm gen app: HITS, LENGTH, ROTATE, NOTE and PROB are edited with Up / Down and the encoder, and Delete puts the default back") {
  App a;
  NEED(a.ok);
  a.turn(-1);  // HITS 4 -> 3
  a.down();
  a.turn(-8);  // LENGTH 16 -> 8
  a.down();
  a.turn(2);   // ROTATE 0 -> 2
  a.down();
  a.turn(3);   // NOTE 49 -> 52
  a.press(O3M_BTN_F1);
  CHECK(a.notes(0) == "X.X..X..X.X..X..");  // 3/8 rotated by 2, repeated over 16 rows
  CHECK(a.api.steps[0][0].note == 52);
  a.press(O3M_BTN_DELETE);                   // NOTE back to the lane's default (nothing in the block yet: C-4)
  a.press(O3M_BTN_F1);
  CHECK(a.api.steps[0][0].note == 49);
  // HITS cannot pass LENGTH, ROTATE stays below it
  a.press(O3M_BTN_UP);
  a.press(O3M_BTN_UP);
  a.press(O3M_BTN_UP);  // HITS
  a.turn(100);
  a.press(O3M_BTN_F1);
  CHECK(a.notes(0) == "XXXXXXXXXXXXXXXX");  // 8 of 8
  a.down();             // LENGTH
  a.turn(-100);         // 1
  a.down();
  a.turn(5);            // ROTATE stays under LENGTH 1: 0
  a.press(O3M_BTN_F1);
  CHECK(a.notes(0) == "XXXXXXXXXXXXXXXX");
  // PROB moves by 5 and is held to 0..100
  a.press(O3M_BTN_DOWN);
  a.press(O3M_BTN_DOWN);
  a.turn(1000);
  a.turn(-3);  // 100 - 15
  const Drawing d = a.draw();
  CHECK(a.shows(d, "85 %"));
}

TEST_CASE("rhythm gen app: PROB 0 keeps no hit, PROB 100 keeps them all, REPLACE empties the notes that were there") {
  App a;
  NEED(a.ok);
  for (unsigned r = 0; r < 16; ++r) {
    a.api.steps[0][r].note = 61;  // a full lane
  }
  a.down(4);
  a.turn(-20);  // PROB 0
  a.press(O3M_BTN_F1);
  CHECK(a.notes(0) == "................");  // REPLACE: every row that is not a (kept) hit is emptied
  a.turn(20);                                 // PROB 100
  a.press(O3M_BTN_F1);
  CHECK(a.notes(0) == "X...X...X...X...");
}

TEST_CASE("rhythm gen app: the same SEED gives the same steps, VARY is SEED + 1 and another group, and a new variation") {
  App a(kBoth, true, 1, 1, 64, 0, 64);
  NEED(a.ok);
  a.press(O3M_BTN_UP);  // (already on the first row)
  a.turn(8);            // HITS 16 -> 24 of 64... LENGTH is 64
  a.down(4);
  a.turn(-10);          // PROB 50
  a.press(O3M_BTN_F1);
  const std::string first = a.notes(0, 64);
  a.press(O3M_BTN_F1);
  CHECK(a.notes(0, 64) == first);  // again: the same
  CHECK(a.api.groups.size() == 2u);
  a.press(O3M_BTN_F2);  // VARY
  CHECK(a.api.groups.size() == 3u);  // one group each
  const std::string second = a.notes(0, 64);
  CHECK(second != first);
  CHECK(second.find('X') != std::string::npos);
  // the shared page shows SEED 0001 now
  a.right(4);
  CHECK(a.shows(a.draw(), "0001"));
  // back to seed 0 by Delete on the SEED row: the first pattern again
  a.press(O3M_BTN_UP);  // (the cursor kept its row 4, which on the shared page is PATTERN now: SEED is the row above)
  a.press(O3M_BTN_DELETE);
  a.press(O3M_BTN_F1);
  CHECK(a.notes(0, 64) == first);
}

TEST_CASE("rhythm gen app: ADD keeps the notes that are there, VOL and INSTR are written on a hit") {
  App a;
  NEED(a.ok);
  a.api.steps[0][1].note = 70;  // not a hit of 4/16
  a.api.steps[0][4].note = 55;  // a hit: replaced
  a.api.steps[0][4].vol = 33;
  a.right(4);           // the shared page
  a.turn(5);            // INSTR 3 -> 8
  a.down();
  a.turn(11);           // VOL: UNSET -> 10 (the row counts 0 = unset)
  a.down();
  a.turn(-1);           // MODE ADD
  a.press(O3M_BTN_F1);
  CHECK(a.api.steps[0][1].note == 70);  // kept
  CHECK(a.api.steps[0][4].note == 49);
  CHECK(a.api.steps[0][4].instr == 8);
  CHECK(a.api.steps[0][4].vol == 10);
  CHECK(a.api.steps[0][0].vol == 10);
  CHECK(a.api.steps[0][2].vol == 255);  // not a hit: untouched
  CHECK(a.notes(0) == "XX..X...X...X...");  // (row 1 kept its note)
}

TEST_CASE("rhythm gen app: four lanes with different LENGTHs are a polyrhythm; the fifth track of the block is left alone") {
  App a(kBoth, true, 6, 6, 24, 0, 24);
  NEED(a.ok);
  CHECK(a.shows(a.draw(), "LANES 4 OF 6"));
  const unsigned lengths[4] = {3, 5, 7, 8};
  for (unsigned l = 0; l < 4; ++l) {
    a.right(4);
    for (unsigned i = 0; i < 4; ++i) a.press(O3M_BTN_LEFT);
    a.right(l);
    for (unsigned i = 0; i < 5; ++i) a.press(O3M_BTN_UP);  // HITS
    a.down();                                                // LENGTH
    a.turn(static_cast<int>(lengths[l]) - 24);
    a.press(O3M_BTN_UP);
    a.turn(-100);
    a.turn(1);  // HITS 1
  }
  a.press(O3M_BTN_F1);
  NEED(a.api.groups.size() == 1u);
  CHECK(a.api.groups[0].size() == 4u);  // lanes 5 and 6 are not written
  for (unsigned l = 0; l < 4; ++l) {
    CHECK(a.notes(l, 24) == cycle(1, lengths[l], 0, 24));
  }
  CHECK(a.notes(4, 24) == std::string(24, '.'));
  CHECK(a.notes(5, 24) == std::string(24, '.'));
}

TEST_CASE("rhythm gen app: with no block there is one lane of 16 rows from the cursor row, cut at the track's end; the default note is the first one already there") {
  App a(kBoth, true, 3, 0, 0, 10);  // no block, cursor on track 0, row 10 of 16
  NEED(a.ok);
  a.api.steps[0][12].note = 80;
  a.api.launch.track = 1;  // the cursor's track
  a.inst.m->ui->enter(a.inst.state);
  a.api.steps[1][12].note = 80;
  a.inst.m->ui->enter(a.inst.state);  // (enter again: the notes are read at enter)
  const Drawing d = a.draw();
  CHECK(a.shows(d, "LANES 1"));
  CHECK(a.shows(d, "AT THE CURSOR  TRACK 2"));
  CHECK(a.shows(d, "ROWS 11-16"));  // 1-based, as the Pattern counts steps
  a.press(O3M_BTN_F1);
  NEED(a.api.groups.size() == 1u);
  CHECK(a.api.groups[0].size() == 1u);
  CHECK((a.api.groups[0][0].track == 1u && a.api.groups[0][0].first == 10u && a.api.groups[0][0].count == 6u));  // 16 - 10 rows left on the track
  CHECK(a.api.steps[1][10].note == 80);  // LENGTH 16 on 6 rows, 4 hits: row 10 is the first hit, written with the note found at row 12
  CHECK(a.api.steps[1][11].note == 0);
}

TEST_CASE("rhythm gen app: nothing to write when the block is past the track's end; a track shorter than the block is cut") {
  App a(kBoth, true, 2, 2, 16, 20);
  a.api.pat_len[0] = 16;
  a.api.pat_len[1] = 32;
  NEED(a.ok);
  a.api.launch.block_start = 20;
  a.inst.m->ui->enter(a.inst.state);
  a.press(O3M_BTN_F1);
  NEED(a.api.groups.size() == 1u);
  CHECK(a.api.groups[0].size() == 1u);  // track 0 ends at 16: only track 1 (rows 20..31) is written
  CHECK(a.api.groups[0][0].track == 1u);
  CHECK(a.api.groups[0][0].count == 12u);
}

TEST_CASE("rhythm gen app: the target rows PATTERN, TRACK, ROW and ROWS move the block; the AUTO LENGTH, HITS and NOTE follow it, a LENGTH chosen by hand stays; APPLY writes ROWS rows (OS-APPUX S5)") {
  App a;  // a block of 2 tracks x 16 rows from row 1: L1 4/16 and L2 4/16
  NEED(a.ok);
  const auto row_to = [&a](unsigned r) {
    for (unsigned i = 0; i < 12; ++i) {
      a.press(O3M_BTN_UP);
    }
    a.down(r);
  };
  a.right(4);  // the shared page: INSTR VOL MODE SEED PATTERN TRACK ROW ROWS
  CHECK(a.shows(a.draw(), "L1  4/16"));
  CHECK(a.shows(a.draw(), "L2  4/16"));
  row_to(7);   // ROWS
  a.turn(-8);  // 16 -> 8
  Drawing d = a.draw();
  CHECK(a.shows(d, "L1  2/8"));  // AUTO LENGTH follows the rows, AUTO HITS follow the length
  CHECK(a.shows(d, "L2  2/8"));
  a.press(O3M_BTN_F1);
  NEED(a.api.groups.size() == 1u);
  NEED(a.api.groups[0].size() == 2u);
  CHECK((a.api.groups[0][0].count == 8u && a.api.groups[0][1].count == 8u));  // ROWS rows, not the 16 of the launch
  a.press(O3M_BTN_DELETE);  // the launch's rows
  CHECK(a.shows(a.draw(), "L1  4/16"));
  // a LENGTH chosen by hand stays when the rows move; the lane that is still AUTO follows
  a.left(4);
  row_to(1);   // LENGTH of lane 1
  a.turn(-4);  // 12, by hand
  a.right(4);
  row_to(7);
  a.turn(4);   // ROWS 20
  d = a.draw();
  CHECK(a.shows(d, "L1  3/12"));  // by hand: stays (its AUTO hits follow it)
  CHECK(a.shows(d, "L2  5/20"));  // AUTO: follows
}

TEST_CASE("rhythm gen app: moving PATTERN to one that exists reads the first note of each lane again for the AUTO NOTE; TRACK moves the lanes (OS-APPUX S5)") {
  App a;
  NEED(a.ok);
  a.api.pattern_id = 3;  // the project has pattern 3 only: the launch (pattern 1) is empty
  a.api.pat_tracks = 3;
  a.api.steps[0][2].note = 80;  // G-6 on track 1
  a.api.steps[1][5].note = 60;  // B-4 on track 2
  a.inst.m->ui->enter(a.inst.state);
  a.right(4);
  Drawing d = a.draw();
  CHECK(a.shows(d, "1 (EMPTY)"));
  CHECK(a.shows(d, "L1  4/16  ROT 0  C-4"));  // nothing is known of the pattern: the default note
  a.down(4);  // PATTERN
  a.turn(2);  // 1 -> 3
  d = a.draw();
  CHECK_FALSE(a.shows(d, "(EMPTY)"));
  CHECK(a.shows(d, "L1  4/16  ROT 0  G-6"));  // the first note in lane 1's rows
  CHECK(a.shows(d, "L2  4/16  ROT 0  B-4"));
  a.down();   // TRACK 1 -> 2: the lanes are tracks 2 and 3
  a.turn(1);
  d = a.draw();
  CHECK(a.shows(d, "L1  4/16  ROT 0  B-4"));
  CHECK(a.shows(d, "L2  4/16  ROT 0  C-4"));
}

TEST_CASE("rhythm gen app: the refusals: opened from the app list, a pattern that does not exist, no edit table, an edit_end that fails") {
  {
    App a(kBoth, false);  // opened from the MODULES list: no context
    NEED(a.ok);
    a.press(O3M_BTN_F1);
    CHECK((a.api.begins == 0 && a.api.groups.empty()));
    CHECK(a.shows(a.draw(), "OPEN IT FROM PATTERN F7 TOOLS"));
  }
  {
    App a;
    a.api.pattern_id = 9;  // the pattern in the context is not there
    a.inst.m->ui->enter(a.inst.state);
    a.press(O3M_BTN_F1);
    CHECK((a.api.begins == 0 && a.api.groups.empty()));
    CHECK(a.shows(a.draw(), "THE PATTERN DOES NOT EXIST YET"));
  }
  {
    App a(O3M_CAP_READ_PROJECT);  // asked for read only: no edit table
    NEED(a.ok);
    a.press(O3M_BTN_F1);
    CHECK((a.api.begins == 0 && a.api.groups.empty()));
    CHECK(a.shows(a.draw(), "THIS HOST CANNOT EDIT"));
  }
  {
    App a;
    a.api.end_result = O3M_EDIT_LIMIT;  // the OS dropped the group
    a.press(O3M_BTN_F1);
    CHECK(a.api.groups.empty());
    CHECK(a.notes(0) == "................");
    CHECK(a.shows(a.draw(), "NOT APPLIED  (CODE 5)"));
    a.api.end_result = O3M_EDIT_OK;
    a.press(O3M_BTN_F1);  // and the next key works again
    CHECK(a.notes(0) == "X...X...X...X...");
  }
  {
    App a(0);  // no read table at all: init refuses
    CHECK(!a.ok);
  }
}

TEST_CASE("rhythm gen app: init refuses an OS whose read table is the 1.4 one") {
  FakeApi api;
  o3m_host h = make_app_host(api, kBoth);
  o3m_read_api old = api.read;
  old.size = static_cast<std::uint32_t>(offsetof(o3m_read_api, pattern_info));  // ends after launch_ctx
  h.read = &old;
  Lib lib;
  NEED(lib.open(O3M_RHYTHMGEN_HOST_SO));
  Instance inst;
  CHECK(!inst.start_with(*lib.entry, h));
}

TEST_CASE("rhythm gen app: every page draws within the view budgets, on the smallest page and the widest, with and without lanes") {
  for (int w : {480, 800}) {
    for (int h : {288, 330, 384}) {
      App a(kBoth, true, 6, 6, 64, 0, 64);
      NEED(a.ok);
      for (unsigned p = 0; p < 5; ++p) {
        for (unsigned r = 0; r < 5; ++r) {
          const Drawing d = a.draw(w, h);
          CHECK(!d.texts.empty());
          a.press(O3M_BTN_DOWN);
        }
        a.press(O3M_BTN_RIGHT);
      }
    }
  }
  App b(kBoth, true, 1, 1, 16);  // one lane: the pages of lanes 2..4 are dim and cannot be edited
  NEED(b.ok);
  b.right(2);
  b.turn(5);
  b.press(O3M_BTN_F1);
  CHECK(b.api.groups.size() == 1u);
  CHECK(b.api.groups[0].size() == 1u);
  (void)b.draw();
}

TEST_CASE("rhythm gen app: nothing the page draws leaves the canvas or overlaps another text, on every page, at the smallest and the real size") {
  const int cw[O3M_FACE_COUNT] = {16, 10, 8, 6}, ch[O3M_FACE_COUNT] = {32, 20, 16, 12};  // the faces as the test's ViewHost has them
  for (auto size : {std::pair<int, int>{480, 288}, std::pair<int, int>{800, 330}}) {
    for (unsigned lanes : {1u, 2u, 6u}) {
      App a(kBoth, true, 6, lanes, 64, 0, 64);
      NEED(a.ok);
      for (unsigned p = 0; p < 5; ++p) {
        for (unsigned row = 0; row < 5; ++row) {
          const Drawing d = a.draw(size.first, size.second);
          for (std::size_t i = 0; i < d.texts.size(); ++i) {
            const auto& t = d.texts[i];
            const int w = static_cast<int>(t.s.size()) * cw[t.face], h = ch[t.face];
            CHECK_MESSAGE((t.x >= 0 && t.y >= 0 && t.x + w <= size.first && t.y + h <= size.second), "page " << p << " '" << t.s << "' at " << t.x << "," << t.y << " " << w << "x" << h << " in " << size.first << "x" << size.second);
            for (std::size_t j = i + 1; j < d.texts.size(); ++j) {
              const auto& u = d.texts[j];
              const int uw = static_cast<int>(u.s.size()) * cw[u.face], uh = ch[u.face];
              const bool overlap = t.x < u.x + uw && u.x < t.x + w && t.y < u.y + uh && u.y < t.y + h;
              CHECK_MESSAGE(!overlap, "page " << p << " '" << t.s << "' overlaps '" << u.s << "'");
            }
          }
          a.press(O3M_BTN_DOWN);
        }
        a.press(O3M_BTN_RIGHT);
      }
    }
  }
}

TEST_CASE("rhythm gen app: the packed .tmod loads, declares ABI 1.5 with read_project and edit_project, and is small") {
  const Tmod t = load_tmod(O3M_RHYTHMGEN_TMOD);
  if (!t.present) {
    return;
  }
  CHECK(t.header == Status::Ok);
  CHECK(t.crc_ok);
  CHECK(t.relocation == Status::Ok);
  CHECK(t.descriptor == Status::Ok);
  CHECK((t.h.abi_major == 1u && t.h.abi_minor == 10u));
  CHECK(t.h.kind == O3M_KIND_APP);
  CHECK(t.h.reserved2[0] == (O3M_CAP_READ_PROJECT | O3M_CAP_EDIT_PROJECT | O3M_CAP_PLAY_NOTES));  // (play_notes: it plays a lane's hits when the encoder turns)
  CHECK(t.h.module_version == O3M_VERSION(1, 0, 0));
  CHECK(t.h.state_bytes <= 1024u);
  CHECK(t.file_bytes < 14u * 1024u);  // the module slab is 14 KiB
  std::printf("rhythmgen.tmod: %u bytes, state %u, hot %u\n", t.file_bytes, t.h.state_bytes, t.h.hot_size);
}

TEST_CASE("rhythm gen app: the 24 rows are kept with set_own_param (AUTO as 0, HITS as hits + 1), handed back through set_param before enter(), and enter() resolves the AUTOs from the block (OS-APPUX S3)") {
  EditRig r;
  r.launch_tools(0, 4, 0, 32, 1);  // a block of 32 rows on one track, instrument 5
  NEED(r.start(O3M_RHYTHMGEN_HOST_SO));
  Drawing d = r.draw();
  CHECK(r.shows(d, "32"));  // LENGTH follows the block (AUTO)
  CHECK(r.api.own_params.empty());
  r.down();  // LENGTH
  r.turn(-4);  // 28
  NEED(r.api.own_params.size() == 1u);
  CHECK((r.api.own_params[0].index == 1u && r.api.own_params[0].value == 28.0f));
  r.press(O3M_BTN_DELETE);  // back to AUTO: kept as 0
  CHECK((r.api.own_params.size() == 2u && r.api.own_params[1].index == 1u && r.api.own_params[1].value == 0.0f));
  r.up();  // HITS (AUTO: LENGTH / 4 = 8): a change is kept as hits + 1
  r.turn(1);
  CHECK((r.api.own_params.size() == 3u && r.api.own_params[2].index == 0u && r.api.own_params[2].value == 10.0f));  // 9 hits
  r.press(O3M_BTN_DELETE);
  CHECK((r.api.own_params.size() == 4u && r.api.own_params[3].index == 0u && r.api.own_params[3].value == 0.0f));
  r.right(4);  // the shared page: INSTR (AUTO: the context's 5), VOL, MODE, SEED
  r.turn(2);
  CHECK((r.api.own_params.size() == 5u && r.api.own_params[4].index == 20u && r.api.own_params[4].value == 7.0f));
  r.press(O3M_BTN_DELETE);
  CHECK((r.api.own_params.size() == 6u && r.api.own_params[5].index == 20u && r.api.own_params[5].value == 0.0f));
  r.down(3);
  r.turn(3);
  CHECK((r.api.own_params.size() == 7u && r.api.own_params[6].index == 23u && r.api.own_params[6].value == 3.0f));
  // a relaunch with the kept values: explicit ones stay (clamped to their ranges), the AUTOs follow a different block, and nothing is reset by enter()
  r.launch_tools(0, 8, 0, 20, 1);
  r.inst.set(1, 12.0f);   // lane 1 LENGTH 12
  r.inst.set(0, 6.0f);    // HITS 5
  r.inst.set(2, 3.0f);    // ROTATE 3
  r.inst.set(4, 85.0f);   // PROB
  r.inst.set(20, 0.0f);   // INSTR AUTO
  r.inst.set(21, 40.0f);  // VOL
  r.inst.set(23, 123.0f);
  r.enter();
  d = r.draw();
  CHECK(r.shows(d, "12"));
  CHECK(r.shows(d, "85"));
  r.right(4);
  d = r.draw();
  CHECK(r.shows(d, "09"));  // INSTR AUTO: the new context's instrument (slot 8 + 1)
  CHECK(r.shows(d, "0123"));
  // a value past its range is clamped, an AUTO LENGTH on a 20-row block is 20
  r.inst.set(1, 0.0f);
  r.inst.set(0, 1000.0f);
  r.enter();
  r.left(4);
  d = r.draw();
  CHECK(r.shows(d, "20"));
}

TEST_CASE("rhythm gen app: turning the encoder on a lane plays its hits once through, eight a second, on the instrument INSTR names; one note at a time, the last stopped; a lane page and play_notes are needed (OS-APPUX S3)") {
  const std::uint32_t caps = kReadEdit | O3M_CAP_PLAY_NOTES;
  EditRig r;
  r.launch_tools(0, 4, 0, 16, 1);
  NEED(r.start(O3M_RHYTHMGEN_HOST_SO, caps));
  r.draw();
  CHECK(r.api.plays.empty());  // nothing sounds until the encoder turns
  r.down(2);
  r.turn(1);  // ROTATE 1: the lane's hits are the rule's, rotated (4 hits in 16: rows 0 4 8 12, rotated to 1 5 9 13)
  r.draw();   // the first frame after the turn plays step 0 (not a hit under rotation 1)
  std::vector<std::uint32_t> hit_steps;
  for (unsigned step = 0; step < 24; ++step) {
    const std::size_t before = r.api.plays.size();
    r.api.now += 125;
    r.draw();
    for (std::size_t i = before; i < r.api.plays.size(); ++i) {
      if (r.api.plays[i].on) {
        hit_steps.push_back(step);
        CHECK(r.api.plays[i].instrument == 5u);          // the context's instrument 4 + 1
        CHECK(r.api.plays[i].note == 49u - 1u);          // the lane's NOTE (step byte 49, C-4) as a live note
        CHECK(r.api.plays[i].velocity == 100u);
      }
    }
  }
  CHECK(hit_steps == std::vector<std::uint32_t>({0, 4, 8, 12}));  // (the frame after the first: steps 1 5 9 13 of the cycle; step counted from that frame)
  CHECK(r.api.plays.back().on == false);  // the cycle is over: the last note was stopped, nothing rings
  // a frame without time passing plays nothing; a new turn starts again
  const std::size_t n = r.api.plays.size();
  r.draw();
  CHECK(r.api.plays.size() == n);
  r.turn(1);  // ROTATE 2: it starts again
  for (unsigned step = 0; step < 24; ++step) {
    r.api.now += 125;
    r.draw();
  }
  CHECK(r.api.plays.size() >= n + 8u);  // four hits, each a note on and a note off
  // no sound on the shared page (no lane), and none without play_notes
  r.right(4);
  const std::size_t m = r.api.plays.size();
  r.turn(1);
  r.api.now += 500;
  r.draw();
  CHECK(r.api.plays.size() == m + 0u);
  EditRig quiet;
  quiet.launch_tools(0, 4, 0, 16, 1);
  NEED(quiet.start(O3M_RHYTHMGEN_HOST_SO));  // kReadEdit: no play_notes, the entries are NULL
  quiet.draw();
  quiet.down(2);
  quiet.turn(1);
  quiet.api.now += 500;
  quiet.draw();
  CHECK(quiet.api.plays.empty());
}
