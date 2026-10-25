// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-H3B (new Community OS behaviour): Humanize. The arithmetic (humanize_math.h) against the rules of the spec, and the module's host shared object driven through the ABI as the OS drives it, on a
// fake of the ABI 1.5 host tables (o3m_test::FakeApi, EditRig): TIME and VOL bounds, the Micro-move slot (replace the existing one, else FX2 then FX1, both taken counted and left), the same SEED giving
// the same steps (pinned), VARY, 0/0 leaving no group, the block and the cursor target, the refusals, the page, and the packed .tmod. The undo of a real APPLY (one unit named HUMANIZE) is in
// tests/module_edit_test.cpp. Host only.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../../../tests/o3m_test_support.h"

#include "../../common/o3m_edit.h"
#include "../humanize_math.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

using namespace o3m_test;
using namespace o3os::module;
namespace hu = humanize;
using o3edit::Draw;

namespace {

o3m_step noted(std::uint8_t note, std::uint8_t vol = 255) { return o3m_step{note, 1, vol, 0, 0, 0, 0, 0}; }

// notes on rows 0, 4, 8, 12 of tracks 0 and 1 of a 16-row pattern
void notes(EditRig& r, unsigned tracks = 2) {
  r.api.pat_tracks = tracks;
  for (unsigned t = 0; t < tracks; ++t) {
    for (unsigned row = 0; row < 16; row += 4) {
      r.api.steps[t][row] = noted(static_cast<std::uint8_t>(49 + row));
    }
  }
}

}  // namespace

// ---- the rule ---------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("humanize rule: only steps with a note change, only in VOL, FX1 and FX2; the delay is within TIME and the volume within +/- VOL of the old one") {
  for (unsigned time : {0u, 1u, 30u, 100u}) {
    for (unsigned vol : {0u, 1u, 8u, 64u}) {
      for (unsigned seed : {0u, 1u, 77u, 9999u}) {
        o3m_step cur[128], out[128];
        for (unsigned j = 0; j < 128; ++j) {
          cur[j] = o3m_step{static_cast<std::uint8_t>(j % 3 == 0 ? 0 : 1 + j), static_cast<std::uint8_t>(j % 64), static_cast<std::uint8_t>(j % 5 == 0 ? 255 : j % 128), 0, 0, 0, 0, 0};
        }
        const hu::Result r = hu::humanize_steps<o3m_step, Draw>(hu::Setup{time, vol, seed, 0}, cur, out, 128);
        unsigned noted_steps = 0;
        for (unsigned j = 0; j < 128; ++j) {
          CHECK(out[j].note == cur[j].note);
          CHECK(out[j].instr == cur[j].instr);
          if (cur[j].note == 0) {
            CHECK(std::memcmp(&out[j], &cur[j], sizeof(o3m_step)) == 0);
            continue;
          }
          ++noted_steps;
          const int base = cur[j].vol;
          if (vol == 0 || cur[j].vol == 255) {
            CHECK(out[j].vol == cur[j].vol);  // an unset volume stays unset
          } else {
            CHECK(out[j].vol <= 127);
            CHECK(std::abs(static_cast<int>(out[j].vol) - base) <= static_cast<int>(vol));
          }
          if (time == 0) {
            CHECK((out[j].fx1_type == 0 && out[j].fx2_type == 0));
          } else {
            for (int fx = 0; fx < 2; ++fx) {
              const std::uint8_t type = fx == 0 ? out[j].fx1_type : out[j].fx2_type, value = fx == 0 ? out[j].fx1_value : out[j].fx2_value;
              if (type != 0) {
                CHECK(type == hu::kFxMicroMove);
                CHECK(value <= time);
                CHECK(value >= 1);  // a drawn 0 writes nothing into an empty slot
              }
            }
          }
        }
        CHECK(r.touched == noted_steps);
        CHECK(r.skipped == 0u);
      }
    }
  }
}

TEST_CASE("humanize rule: a step with no volume stays unset (it plays at the instrument's volume, which a written velocity would replace); a volume is jittered and clamps at 127 and at 0") {
  o3m_step cur[3] = {noted(60, 255), noted(61, 2), noted(62, 126)}, out[3];
  bool top = false, low = false, below = false;
  for (unsigned seed = 0; seed < 200; ++seed) {
    (void)hu::humanize_steps<o3m_step, Draw>(hu::Setup{0, 64, seed, 0}, cur, out, 3);
    CHECK(out[0].vol == 255);  // an instrument of volume 50 keeps its 50: nothing was written
    CHECK(out[1].vol <= 66);
    CHECK(out[2].vol <= 127);
    CHECK(out[2].vol >= 62);
    top = top || out[2].vol == 127;  // the clamps are reached
    low = low || out[1].vol == 0;
    below = below || out[2].vol < 126;
  }
  CHECK(top);
  CHECK(low);
  CHECK(below);
}

TEST_CASE("humanize rule: the delay goes into a Micro-move that is there, else into FX2, else into FX1; two taken slots keep the timing and are counted") {
  o3m_step cur[6], out[6];
  for (auto& s : cur) {
    s = noted(60);
  }
  cur[0].fx1_type = hu::kFxMicroMove;  // an existing Micro-move in FX1: replaced
  cur[0].fx1_value = 99;
  cur[1].fx2_type = hu::kFxMicroMove;  // in FX2: replaced
  cur[1].fx2_value = 99;
  cur[2].fx1_type = 8;                 // FX1 taken: FX2 is free
  cur[2].fx1_value = 7;
  cur[3].fx2_type = 8;                 // FX2 taken: FX1 is free
  cur[3].fx2_value = 7;
  cur[4].fx1_type = 8;                 // both taken
  cur[4].fx2_type = 9;
  cur[4].fx1_value = 1;
  cur[4].fx2_value = 2;
  unsigned skipped_total = 0;
  for (unsigned seed = 0; seed < 50; ++seed) {
    const hu::Result r = hu::humanize_steps<o3m_step, Draw>(hu::Setup{100, 0, seed, 0}, cur, out, 6);
    CHECK((out[0].fx1_type == hu::kFxMicroMove && out[0].fx1_value <= 100 && out[0].fx2_type == 0));  // replaced, and a drawn 0 replaces too
    CHECK((out[1].fx2_type == hu::kFxMicroMove && out[1].fx2_value <= 100 && out[1].fx1_type == 0));
    CHECK(out[2].fx1_type == 8);
    CHECK(out[2].fx1_value == 7);
    CHECK((out[2].fx2_type == hu::kFxMicroMove || out[2].fx2_type == 0));
    CHECK(out[3].fx2_type == 8);
    CHECK((out[3].fx1_type == hu::kFxMicroMove || out[3].fx1_type == 0));
    CHECK((out[4].fx1_type == 8 && out[4].fx2_type == 9 && out[4].fx1_value == 1 && out[4].fx2_value == 2));  // untouched
    CHECK((out[5].fx2_type == hu::kFxMicroMove || out[5].fx2_type == 0));  // free: FX2 first
    CHECK(out[5].fx1_type == 0);
    skipped_total += r.skipped;
    CHECK(r.touched == 6u);
    CHECK(r.skipped <= 1u);  // only step 4 can be skipped, and only when it drew a delay above 0
  }
  CHECK(skipped_total > 40u);  // (a delay of 0 is 1 in 101)
}

TEST_CASE("humanize rule: the same SEED gives the same steps, another SEED or another track another ones; the draws do not depend on the other steps' FX") {
  o3m_step cur[16], a[16], b[16], c[16], d[16];
  for (unsigned j = 0; j < 16; ++j) {
    cur[j] = noted(static_cast<std::uint8_t>(40 + j), static_cast<std::uint8_t>(60 + j));
  }
  (void)hu::humanize_steps<o3m_step, Draw>(hu::Setup{50, 20, 7, 0}, cur, a, 16);
  (void)hu::humanize_steps<o3m_step, Draw>(hu::Setup{50, 20, 7, 0}, cur, b, 16);
  CHECK(std::memcmp(a, b, sizeof a) == 0);
  (void)hu::humanize_steps<o3m_step, Draw>(hu::Setup{50, 20, 8, 0}, cur, c, 16);
  (void)hu::humanize_steps<o3m_step, Draw>(hu::Setup{50, 20, 7, 1}, cur, d, 16);
  CHECK(std::memcmp(a, c, sizeof a) != 0);
  CHECK(std::memcmp(a, d, sizeof a) != 0);
  // the volume of step 5 is the same whatever FX another step holds (two draws a noted step, always)
  o3m_step cur2[16];
  std::memcpy(cur2, cur, sizeof cur);
  cur2[2].fx1_type = 8;
  cur2[2].fx2_type = 9;
  o3m_step e[16];
  (void)hu::humanize_steps<o3m_step, Draw>(hu::Setup{50, 20, 7, 0}, cur2, e, 16);
  for (unsigned j = 0; j < 16; ++j) {
    CHECK(e[j].vol == a[j].vol);
  }
}

TEST_CASE("humanize rule: pinned values (a change of the draw is a change of every project's humanize, so it must be deliberate)") {
  o3m_step cur[8], out[8];
  for (unsigned j = 0; j < 8; ++j) {
    cur[j] = noted(static_cast<std::uint8_t>(49 + j), 100);
  }
  (void)hu::humanize_steps<o3m_step, Draw>(hu::Setup{20, 8, 0, 0}, cur, out, 8);
  std::string got;
  for (unsigned j = 0; j < 8; ++j) {
    got += std::to_string(out[j].vol) + "/" + std::to_string(out[j].fx2_value) + " ";
  }
  CHECK(got == "105/1 98/12 95/8 92/20 102/20 105/11 99/16 100/13 ");
}

// ---- the app ----------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("humanize app: descriptor") {
  EditRig r;
  NEED(r.lib.open(O3M_HUMANIZE_HOST_SO));
  const o3m_module& m = *r.lib.entry;
  CHECK((m.abi_major == 1u && m.abi_minor == 10u && m.kind == O3M_KIND_APP));  // (1.10: its three rows are declared and kept with the project, OS-APPUX S3)
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
  CHECK(std::string(m.name) == "Humanize");
  CHECK(m.view_count == 1u);
  CHECK(m.param_count == 3u);  // TIME, VOL, SEED: declared as storage (the page draws them itself)
  CHECK((std::string(m.params[0].name) == "Time" && m.params[0].def == 20.0f && m.params[1].def == 8.0f && m.params[2].max == 9999.0f));
  CHECK(m.set_param != nullptr);
}

TEST_CASE("humanize app: F1 on a block of 2 tracks writes ONE group of two set_steps with the mask VOL | FX1 | FX2; notes and instruments are never in it") {
  EditRig r;
  notes(r);
  for (unsigned t = 0; t < 2; ++t) {
    r.api.steps[t][0].vol = 60;  // rows 0 and 8 carry a volume, rows 4 and 12 do not (an instrument whose own volume is not 127 plays them)
    r.api.steps[t][8].vol = 60;
  }
  r.launch_tools(0, 0, 0, 16, 2);
  NEED(r.start(O3M_HUMANIZE_HOST_SO));
  r.press(O3M_BTN_F1);
  NEED(r.api.groups.size() == 1u);
  CHECK(r.api.groups[0].size() == 2u);
  for (const auto& w : r.api.groups[0]) {
    CHECK(w.fields == (O3M_STEP_VOL | O3M_STEP_FX1 | O3M_STEP_FX2));
    CHECK((w.first == 0u && w.count == 16u && w.pattern == 1u));
  }
  for (unsigned t = 0; t < 2; ++t) {
    for (unsigned row = 0; row < 16; ++row) {
      const o3m_step& s = r.api.steps[t][row];
      if (row % 4 == 0) {
        CHECK((s.note == 49 + row && s.instr == 1));
        if (row % 8 == 0) {
          CHECK((s.vol >= 60 - 8 && s.vol <= 60 + 8));  // defaults: TIME 20, VOL 8 around the volume it had
        } else {
          CHECK(s.vol == 255);  // unset stays unset
        }
        CHECK((s.fx2_type == 0 || (s.fx2_type == 2 && s.fx2_value >= 1 && s.fx2_value <= 20)));
      } else {
        const o3m_step empty{0, 0, 255, 0, 0, 0, 0, 0};
        CHECK(std::memcmp(&s, &empty, sizeof s) == 0);
      }
    }
  }
  CHECK(r.shows(r.draw(), "8 STEPS HUMANIZED"));
}

// One run of the app on the standard pattern: `fn` presses the keys; returns the steps after them and the groups made. (The fake's table functions have no context: one rig at a time.)
struct Run {
  std::vector<o3m_step> steps;
  std::size_t groups = 0;
};
template <class Fn>
Run run_on_notes(Fn fn) {
  EditRig r;
  notes(r);
  r.launch_tools(0, 0, 0, 16, 2);
  Run out;
  if (!r.start(O3M_HUMANIZE_HOST_SO)) {
    return out;
  }
  fn(r);
  out.steps.assign(&r.api.steps[0][0], &r.api.steps[0][0] + FakeApi::kTracks * FakeApi::kRows);
  out.groups = r.api.groups.size();
  return out;
}

bool same(const Run& a, const Run& b) { return a.steps.size() == b.steps.size() && std::memcmp(a.steps.data(), b.steps.data(), a.steps.size() * sizeof(o3m_step)) == 0; }

TEST_CASE("humanize app: the same SEED gives the same steps, VARY is SEED + 1 and another group") {
  const Run a = run_on_notes([](EditRig& r) { r.press(O3M_BTN_F1); });
  const Run b = run_on_notes([](EditRig& r) { r.press(O3M_BTN_F1); });
  NEED(a.steps.size() != 0u);
  CHECK(same(a, b));
  CHECK(a.groups == 1u);
  // VARY: SEED 1, then APPLY, in one press: the same as SEED set by hand to 1 and F1
  const Run vary = run_on_notes([](EditRig& r) { r.press(O3M_BTN_F2); });
  const Run hand = run_on_notes([](EditRig& r) {
    r.down(2);  // the SEED row
    r.turn(1);
    r.press(O3M_BTN_F1);
  });
  CHECK(same(vary, hand));
  CHECK(!same(vary, a));
  // VARY again makes another group on the humanized steps, with SEED 2
  const Run twice = run_on_notes([](EditRig& r) {
    r.press(O3M_BTN_F2);
    r.press(O3M_BTN_F2);
  });
  CHECK(twice.groups == 2u);
  CHECK(!same(twice, vary));
}

TEST_CASE("humanize app: TIME 0 and VOL 0 change nothing and make no group; TIME alone or VOL alone writes only that") {
  EditRig r;
  notes(r);
  r.launch_tools(0, 0, 0, 16, 2);
  NEED(r.start(O3M_HUMANIZE_HOST_SO));
  r.turn(-20);  // TIME 0
  r.down();
  r.turn(-20);  // VOL 0
  r.press(O3M_BTN_F1);
  CHECK((r.api.begins == 0 && r.api.groups.empty()));
  CHECK(r.shows(r.draw(), "TIME AND VOL ARE 0"));
  r.turn(8);  // VOL 8 only
  r.press(O3M_BTN_F1);
  NEED(r.api.groups.size() == 1u);
  for (unsigned row = 0; row < 16; row += 4) {
    CHECK((r.api.steps[0][row].fx1_type == 0 && r.api.steps[0][row].fx2_type == 0));  // no delay written
  }
  EditRig q;
  notes(q);
  q.launch_tools(0, 0, 0, 16, 2);
  NEED(q.start(O3M_HUMANIZE_HOST_SO));
  q.down();
  q.turn(-20);  // VOL 0, TIME 20
  q.press(O3M_BTN_F1);
  for (unsigned row = 0; row < 16; row += 4) {
    CHECK(q.api.steps[0][row].vol == 255);  // untouched
  }
  bool any_fx = false;
  for (unsigned row = 0; row < 16; row += 4) {
    any_fx = any_fx || q.api.steps[0][row].fx2_type == 2;
  }
  CHECK(any_fx);
}

TEST_CASE("humanize app: a step with both slots taken keeps its timing and the page says how many") {
  EditRig r;
  notes(r, 1);
  for (unsigned row = 0; row < 16; row += 4) {
    r.api.steps[0][row].fx1_type = 8;
    r.api.steps[0][row].fx2_type = 9;
  }
  r.launch_tools(0, 0, 0, 16, 1);
  NEED(r.start(O3M_HUMANIZE_HOST_SO));
  r.turn(80);  // TIME 100: a delay of 0 is rare
  r.press(O3M_BTN_F1);
  for (unsigned row = 0; row < 16; row += 4) {
    CHECK((r.api.steps[0][row].fx1_type == 8 && r.api.steps[0][row].fx2_type == 9));
  }
  const Drawing d = r.draw();
  CHECK(r.shows(d, "STEPS: NO FREE FX SLOT"));
}

TEST_CASE("humanize app: with no block it acts on the cursor's track for 16 rows from the cursor row, and on an empty target it says so") {
  EditRig r;
  r.api.pat_tracks = 3;
  for (auto& n : r.api.pat_len) {
    n = 32;
  }
  for (unsigned t = 0; t < 3; ++t) {
    for (unsigned row = 0; row < 32; ++row) {
      r.api.steps[t][row] = noted(60, 90);
    }
  }
  r.launch_tools(1, 0, 8, 0, 0);  // the cursor: track 2, row 9
  NEED(r.start(O3M_HUMANIZE_HOST_SO));
  r.press(O3M_BTN_F1);
  NEED(r.api.groups.size() == 1u);
  CHECK(r.api.groups[0].size() == 1u);
  CHECK((r.api.groups[0][0].track == 1u && r.api.groups[0][0].first == 8u && r.api.groups[0][0].count == 16u));
  for (unsigned t = 0; t < 3; ++t) {
    for (unsigned row = 0; row < 32; ++row) {
      const bool inside = t == 1 && row >= 8 && row < 24;
      const o3m_step& st = r.api.steps[t][row];
      if (inside) {
        CHECK_MESSAGE((st.vol >= 90 - 8 && st.vol <= 90 + 8), "track " << t << " row " << row);
      } else {
        const o3m_step original = noted(60, 90);
        CHECK_MESSAGE(std::memcmp(&st, &original, sizeof st) == 0, "track " << t << " row " << row);
      }
    }
  }
  // a pattern with no note in the target
  EditRig e;
  e.launch_tools(0, 0, 0, 16, 2);
  NEED(e.start(O3M_HUMANIZE_HOST_SO));
  e.press(O3M_BTN_F1);
  CHECK(e.api.groups.size() == 1u);  // a group that wrote nothing: the OS makes no undo unit of it
  CHECK(e.shows(e.draw(), "NO STEP WITH A NOTE"));
}

TEST_CASE("humanize app: it runs on a host of ABI 1.5 (tables that end at set_steps), and the refusals") {
  {
    EditRig r;
    notes(r);
    r.api.make_abi15();
    r.launch_tools(0, 0, 0, 16, 2);
    NEED(r.start(O3M_HUMANIZE_HOST_SO));
    r.press(O3M_BTN_F1);
    CHECK(r.api.groups.size() == 1u);
  }
  {
    EditRig r;  // opened from the app list
    NEED(r.start(O3M_HUMANIZE_HOST_SO));
    r.press(O3M_BTN_F1);
    CHECK(r.api.begins == 0);
    CHECK(r.shows(r.draw(), "OPEN IT FROM PATTERN F7 TOOLS"));
  }
  {
    EditRig r;
    r.launch_tools(0, 0, 0, 16, 2);
    NEED(r.start(O3M_HUMANIZE_HOST_SO, O3M_CAP_READ_PROJECT));  // no edit table
    r.press(O3M_BTN_F1);
    CHECK(r.api.begins == 0);
    CHECK(r.shows(r.draw(), "THIS HOST CANNOT EDIT"));
  }
  {
    EditRig r;
    notes(r);
    r.api.pattern_id = 9;
    r.launch_tools(0, 0, 0, 16, 2);
    NEED(r.start(O3M_HUMANIZE_HOST_SO));
    r.press(O3M_BTN_F1);
    CHECK(r.api.begins == 0);
    CHECK(r.shows(r.draw(), "THE PATTERN DOES NOT EXIST YET"));
  }
  {
    EditRig r;
    notes(r);
    r.api.end_result = O3M_EDIT_LIMIT;
    r.launch_tools(0, 0, 0, 16, 2);
    NEED(r.start(O3M_HUMANIZE_HOST_SO));
    r.press(O3M_BTN_F1);
    CHECK(r.api.groups.empty());
    CHECK(r.shows(r.draw(), "NOT APPLIED  (CODE 5)"));
  }
  {
    EditRig r;
    CHECK(!r.start(O3M_HUMANIZE_HOST_SO, 0));  // no read table: init refuses
  }
}

TEST_CASE("humanize app: Delete puts the default back, the values stop at their ends") {
  EditRig r;
  notes(r);
  r.launch_tools(0, 0, 0, 16, 2);
  NEED(r.start(O3M_HUMANIZE_HOST_SO));
  r.turn(500);
  CHECK(r.shows(r.draw(), "100"));
  r.turn(-500);
  r.press(O3M_BTN_DELETE);
  r.down();
  r.turn(500);
  CHECK(r.shows(r.draw(), "+/- 64"));
  r.press(O3M_BTN_DELETE);
  CHECK(r.shows(r.draw(), "+/- 8"));
  r.down();
  r.turn(100000);
  CHECK(r.shows(r.draw(), "9999"));
}

TEST_CASE("humanize app: the page stays inside the canvas and overlaps nothing, in every state, at the smallest and the real size") {
  for (auto size : {std::pair<int, int>{480, 288}, std::pair<int, int>{800, 330}}) {
    EditRig r;
    notes(r);
    r.launch_tools(0, 0, 0, 16, 2);
    NEED(r.start(O3M_HUMANIZE_HOST_SO));
    for (unsigned row = 0; row < 3; ++row) {
      r.check_layout(size.first, size.second, "fresh");
      r.press(O3M_BTN_DOWN);
    }
    r.press(O3M_BTN_F1);
    r.check_layout(size.first, size.second, "applied");
    r.press(O3M_BTN_F2);
    r.check_layout(size.first, size.second, "varied");
  }
}

TEST_CASE("humanize app: the packed .tmod loads, declares ABI 1.5 with read_project and edit_project, and is small") {
  const Tmod t = load_tmod(O3M_HUMANIZE_TMOD);
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
  std::printf("humanize.tmod: %u bytes, state %u, hot %u\n", t.file_bytes, t.h.state_bytes, t.h.hot_size);
}

TEST_CASE("humanize app: the three rows are kept with set_own_param, handed back through set_param before enter(), and enter() leaves them alone (OS-APPUX S3)") {
  EditRig r;
  notes(r);
  r.launch_tools(0, 0, 0, 16, 2);
  NEED(r.start(O3M_HUMANIZE_HOST_SO));
  CHECK(r.shows(r.draw(), "20"));  // the manifest's defaults, handed back by start (set_param x 3, then enter)
  CHECK(r.api.own_params.empty());
  r.turn(3);  // TIME 23
  NEED(r.api.own_params.size() == 1u);
  CHECK((r.api.own_params[0].index == 0u && r.api.own_params[0].value == 23.0f));
  r.down();
  r.turn(-2);  // VOL 6
  CHECK((r.api.own_params.size() == 2u && r.api.own_params[1].index == 1u && r.api.own_params[1].value == 6.0f));
  r.down();
  r.press(O3M_BTN_F2);  // VARY: SEED + 1, and then APPLY
  CHECK((r.api.own_params.size() == 3u && r.api.own_params[2].index == 2u && r.api.own_params[2].value == 1.0f));
  r.press(O3M_BTN_DELETE);  // the row's default
  CHECK((r.api.own_params.size() == 4u && r.api.own_params[3].index == 2u && r.api.own_params[3].value == 0.0f));
  // a relaunch: the OS hands back what it kept (values outside the range are clamped) and then enters; the page shows them and enter() did not reset them
  r.inst.set(0, 23.0f);
  r.inst.set(1, 6.0f);
  r.inst.set(2, 77.0f);
  r.enter();
  const Drawing d = r.draw();
  CHECK(r.shows(d, "23"));
  CHECK(r.shows(d, "+/- 6"));
  CHECK(r.shows(d, "0077"));
  r.inst.set(0, 1000.0f);  // past its range
  r.inst.set(1, -5.0f);
  r.inst.set(2, 1e9f);
  const Drawing e = r.draw();
  CHECK(r.shows(e, "100"));
  CHECK(r.shows(e, "+/- 0"));
  CHECK(r.shows(e, "9999"));
}
