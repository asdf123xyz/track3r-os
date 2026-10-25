// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-H3B (new Community OS behaviour): Mutate. The arithmetic (mutate_math.h) against a double-precision reckoning of the rule, and the module's host shared object driven through the ABI as the OS
// drives it, on a fake of the ABI 1.6 host tables (EditRig: instruments with parameters, set_instrument_param inside a group): bounds, GROUPs, AMOUNT 0 changing nothing, the same SEED giving the same
// result (pinned), a parameter getting the same new value whichever GROUP is chosen, non-writable parameters never touched, STEPPED and ENUM moving by whole steps, a module instrument, the refusals,
// the page and the packed .tmod. The undo of a real APPLY (every float bit back) is in tests/module_edit_test.cpp. Host only.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../../../tests/o3m_test_support.h"

#include "../../common/o3m_edit.h"
#include "../mutate_math.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using namespace o3m_test;
using namespace o3os::module;
namespace mu = mutate;

namespace {

constexpr unsigned kRowAmount = 0, kRowGroup = 1, kRowSeed = 2;

void go_to(EditRig& r, unsigned row) {
  for (unsigned i = 0; i < 3; ++i) {
    r.press(O3M_BTN_UP);
  }
  r.down(row);
}

// A rig with a sample instrument on slot 3 (the 0-based slot 2, the context's), AMOUNT 100.
struct Rig : EditRig {
  explicit Rig(bool started = true, float k = 1.0f) {
    api.add_sample(3, 0, k);
    launch_tools(0, 2, 0, 0, 0);
    if (started) {
      NEED(start(O3M_MUTATE_HOST_SO));
      turn(16);  // AMOUNT 100
    }
  }
  std::vector<float> values(unsigned n = 3) const {
    std::vector<float> v;
    for (const auto& p : api.instr[n].params) {
      v.push_back(p.value);
    }
    return v;
  }
};

}  // namespace

// ---- the rule ---------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("mutate rule: new = old + (r - 0.5) x AMOUNT x range, clamped; a STEPPED or ENUM parameter moves by the rounded delta; AMOUNT 0 and r = 0.5 change nothing") {
  for (float mn : {0.0f, -24.0f, 4.0f}) {
    for (float span : {1.0f, 4.3f, 48.0f, 10000.0f}) {
      const float mx = mn + span;
      for (unsigned amount : {0u, 1u, 20u, 100u}) {
        for (unsigned i = 0; i <= 16; ++i) {
          const float r = static_cast<float>(i) / 16.0f * 0.999999f;
          for (float old : {mn, mn + 0.25f * span, mn + 0.5f * span, mx}) {
            const float got = mu::mutate_value(old, mn, mx, false, amount, r);
            const double delta = (static_cast<double>(r) - 0.5) * (amount * 0.01) * static_cast<double>(span);
            const double want_d = static_cast<double>(old) + delta;
            const double want = want_d < mn ? mn : (want_d > mx ? mx : want_d);
            CHECK(std::fabs(static_cast<double>(got) - want) <= 1e-4 * span + 1e-4);
            CHECK(got >= mn);
            CHECK(got <= mx);
            const float whole = mu::mutate_value(old, mn, mx, true, amount, r);
            const double step = std::floor(std::fabs(delta) + 0.5) * (delta < 0 ? -1.0 : 1.0);
            const double want_w = static_cast<double>(old) + step;
            CHECK(std::fabs(static_cast<double>(whole) - (want_w < mn ? mn : (want_w > mx ? mx : want_w))) <= 1e-3 * span + 1e-4);
            CHECK(whole >= mn);
            CHECK(whole <= mx);
          }
        }
        CHECK(mu::mutate_value(mn + 0.3f * span, mn, mx, false, 0, 0.99f) == mn + 0.3f * span);
        CHECK(mu::mutate_value(mn + 0.3f * span, mn, mx, false, amount, 0.5f) == mn + 0.3f * span);
      }
    }
  }
}

// ---- the app ----------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("mutate app: descriptor") {
  EditRig r;
  NEED(r.lib.open(O3M_MUTATE_HOST_SO));
  const o3m_module& m = *r.lib.entry;
  CHECK((m.abi_major == 1u && m.abi_minor == 10u && m.kind == O3M_KIND_APP));  // (1.10: its rows are declared and kept with the project, OS-APPUX S3)
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
  CHECK(std::string(m.name) == "Mutate");
  CHECK((m.view_count == 1u && m.param_count == 3u && m.set_param != nullptr));  // Amount, Group, Seed: declared as storage (the page draws them itself)
}

TEST_CASE("mutate app: F1 writes the changes of the writable parameters as ONE group, each new value is the rule's with r from (SEED, id), inside its range; the others are never written") {
  Rig r;
  NEED(r.ok);
  const std::vector<float> before = r.values();
  r.press(O3M_BTN_F1);
  NEED(r.api.param_groups.size() == 1u);
  CHECK(r.api.groups.size() == 1u);  // (the fake lists every group that ended: the group has no step writes)
  const auto& g = r.api.param_groups[0];
  CHECK(!g.empty());
  for (const auto& w : g) {
    CHECK(w.instrument == 3u);
    const auto* p = r.api.find_param(3, w.id);
    NEED(p != nullptr);
    CHECK((p->flags & O3M_PD_APP_WRITABLE) != 0u);
    CHECK(w.id != 13);
    CHECK(w.id != 0x14);
    CHECK(w.value >= p->min);
    CHECK(w.value <= p->max);
  }
  std::size_t i = 0;
  unsigned moved = 0;
  for (const auto& p : r.api.instr[3].params) {
    const double was = before[i++];
    if ((p.flags & O3M_PD_APP_WRITABLE) == 0u) {
      CHECK(p.value == static_cast<float>(was));  // not writable: untouched
      continue;
    }
    const bool whole = (p.flags & (O3M_PD_STEPPED | O3M_PD_ENUM)) != 0u;
    double delta = (static_cast<double>(o3edit::unit_of(0, p.id)) - 0.5) * 1.0 * (p.max - p.min);
    if (whole) {
      delta = std::floor(std::fabs(delta) + 0.5) * (delta < 0 ? -1.0 : 1.0);
    }
    double want = was + delta;
    want = want < p.min ? p.min : (want > p.max ? p.max : want);
    if (whole) {
      want = std::floor(want + 0.5);
    }
    CHECK_MESSAGE(std::fabs(p.value - want) <= 1e-3 * (p.max - p.min) + 1e-4, "id " << p.id);
    moved += static_cast<float>(was) != p.value ? 1u : 0u;
  }
  CHECK(moved >= 6u);
  CHECK(r.shows(r.draw(), "PARAMETERS CHANGED"));
}

// One run: a fresh rig (AMOUNT 100), `fn` presses the keys, the instrument's values after them. (The fake's table functions have no context: one rig at a time.)
template <class Fn>
std::vector<float> run(Fn fn, std::size_t* groups = nullptr) {
  Rig r;
  fn(r);
  if (groups != nullptr) {
    *groups = r.api.param_groups.size();
  }
  return r.values();
}

TEST_CASE("mutate app: the same SEED gives the same result; VARY is SEED + 1 and another group") {
  const std::vector<float> a = run([](Rig& r) { r.press(O3M_BTN_F1); });
  const std::vector<float> b = run([](Rig& r) { r.press(O3M_BTN_F1); });
  CHECK(a == b);
  std::size_t groups = 0;
  const std::vector<float> vary = run([](Rig& r) { r.press(O3M_BTN_F2); }, &groups);
  CHECK(groups == 1u);
  CHECK(a != vary);
  const std::vector<float> hand = run([](Rig& r) {  // SEED 1 by hand
    go_to(r, kRowSeed);
    r.turn(1);
    r.press(O3M_BTN_F1);
  });
  CHECK(hand == vary);
  (void)run([](Rig& r) {  // and VARY again is another unit
    r.press(O3M_BTN_F2);
    r.press(O3M_BTN_F2);
  }, &groups);
  CHECK(groups == 2u);
}

TEST_CASE("mutate app: a GROUP limits it to that page, and a parameter gets the same new value whichever GROUP is chosen") {
  Rig all;
  all.press(O3M_BTN_F1);
  const std::vector<float> all_values = all.values();
  const std::vector<float> start = Rig().values();
  struct G {
    unsigned steps;
    std::uint8_t group;
    const char* name;
  };
  for (const G& g : {G{1, O3M_PGROUP_VOICE, "VOICE"}, G{2, O3M_PGROUP_FILTER, "FILTER"}, G{3, O3M_PGROUP_ENV, "ENV"}, G{4, O3M_PGROUP_SENDS, "SENDS"}, G{5, O3M_PGROUP_MOD, "MOD"}}) {
    Rig r;
    go_to(r, kRowGroup);
    r.turn(static_cast<int>(g.steps));
    CHECK_MESSAGE(r.shows(r.draw(), g.name), g.name);
    r.press(O3M_BTN_F1);
    for (std::size_t i = 0; i < r.api.instr[3].params.size(); ++i) {
      const auto& p = r.api.instr[3].params[i];
      if (p.group == g.group && (p.flags & O3M_PD_APP_WRITABLE) != 0u) {
        CHECK_MESSAGE(p.value == all_values[i], g.name << " id " << p.id);  // the ALL run's value for it
      } else {
        CHECK_MESSAGE(p.value == start[i], g.name << " id " << p.id);       // outside the group: not touched
      }
    }
  }
  Rig e;  // the source page holds nothing writable, so it is not offered: the encoder stops after MOD
  go_to(e, kRowGroup);
  e.turn(40);
  CHECK(e.shows(e.draw(), "MOD"));
  CHECK(!e.shows(e.draw(), "SOURCE"));
}

TEST_CASE("mutate app: AMOUNT 0 changes nothing and makes no group; a small AMOUNT moves a little; a STEPPED or ENUM parameter only moves by whole steps, an ENUM of two values never") {
  Rig r;
  r.turn(-30);  // AMOUNT 0
  r.press(O3M_BTN_F1);
  CHECK((r.api.begins == 0 && r.api.param_groups.empty()));
  CHECK(r.shows(r.draw(), "AMOUNT IS 0"));
  const std::vector<float> start = r.values();
  r.turn(2);  // AMOUNT 10
  r.press(O3M_BTN_F1);
  const std::vector<float> after = r.values();
  for (std::size_t i = 0; i < start.size(); ++i) {
    const auto& p = r.api.instr[3].params[i];
    CHECK(std::fabs(after[i] - start[i]) <= 0.5f * 0.1f * (p.max - p.min) + 1e-3f + ((p.flags & (O3M_PD_STEPPED | O3M_PD_ENUM)) != 0u ? 0.5f : 0.0f));
  }
  // whole parameters stay whole over many seeds
  for (unsigned seed = 0; seed < 60; ++seed) {
    Rig q;
    go_to(q, kRowSeed);
    q.turn(static_cast<int>(seed));
    q.press(O3M_BTN_F1);
    for (const auto& p : q.api.instr[3].params) {
      if ((p.flags & (O3M_PD_STEPPED | O3M_PD_ENUM)) != 0u) {
        CHECK(p.value == std::floor(p.value));
      }
      if (p.id == 5) {
        CHECK(p.value == q.values()[2]);  // (FILTER, 0..1)
        CHECK(p.value == 0.0f);           // a span of 1 moves by at most half a step: rounds to nothing
      }
    }
  }
}

TEST_CASE("mutate app: pinned values (a change of the draw changes every mutation, so it must be deliberate)") {
  Rig r;
  r.press(O3M_BTN_F1);
  std::string got;
  for (const auto& w : r.api.param_groups[0]) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%u=%.3f ", w.id, static_cast<double>(w.value));
    got += buf;
  }
  CHECK(got == "0=10.777 2=-24.000 7=0.679 8=0.000 9=0.000 33=0.800 37=4298.908 97=3.000 ");
}

TEST_CASE("mutate app: a module instrument: its parameters are all writable, the groups are its pages of eight, and every value stays in range") {
  EditRig r;
  r.api.add_module(3, 0xC0FFEEu, 12, 1.0f);
  r.launch_tools(0, 2, 0, 0, 0);
  NEED(r.start(O3M_MUTATE_HOST_SO));
  r.turn(16);
  go_to(r, kRowGroup);
  r.turn(1);
  CHECK(r.shows(r.draw(), "PAGE 1"));
  CHECK(r.shows(r.draw(), "8 PARAMETERS IN THE GROUP"));
  r.turn(1);
  CHECK(r.shows(r.draw(), "PAGE 2"));
  CHECK(r.shows(r.draw(), "4 PARAMETERS IN THE GROUP"));
  r.turn(1);
  CHECK(r.shows(r.draw(), "PAGE 2"));  // only two pages
  r.press(O3M_BTN_F1);
  NEED(r.api.param_groups.size() == 1u);
  for (const auto& w : r.api.param_groups[0]) {
    CHECK(w.id >= 0x108);  // the second page only
    const auto* p = r.api.find_param(3, w.id);
    NEED(p != nullptr);
    CHECK(w.value >= p->min);
    CHECK(w.value <= p->max);
  }
  CHECK(r.api.param_groups[0].size() <= 4u);
}

TEST_CASE("mutate app: the refusals: not a sample or module instrument, opened from the app list, no edit table, an edit_end that fails, an OS of 1.5") {
  for (std::uint32_t kind : {std::uint32_t{O3M_INSTRUMENT_EMPTY}, std::uint32_t{O3M_INSTRUMENT_MIDI}}) {
    EditRig r;
    r.api.instr[3].kind = kind;
    r.launch_tools(0, 2, 0, 0, 0);
    NEED(r.start(O3M_MUTATE_HOST_SO));
    r.press(O3M_BTN_F1);
    r.press(O3M_BTN_F2);
    CHECK((r.api.begins == 0 && r.api.param_groups.empty()));
    CHECK(r.shows(r.draw(), "SAMPLE OR MODULE INSTRUMENTS ONLY"));
  }
  {
    Rig r(false);  // opened from the app list
    r.api.launch = {sizeof(o3m_launch_ctx), 0, 0, 0, 0, 0, 0, 0, {0, 0, 0}};
    NEED(r.start(O3M_MUTATE_HOST_SO));
    r.press(O3M_BTN_F1);
    CHECK(r.api.begins == 0);
    CHECK(r.shows(r.draw(), "OPEN IT FROM PATTERN F7 TOOLS"));
  }
  {
    Rig r(false);
    NEED(r.start(O3M_MUTATE_HOST_SO, O3M_CAP_READ_PROJECT));
    r.turn(16);
    r.press(O3M_BTN_F1);
    CHECK(r.api.begins == 0);
    CHECK(r.shows(r.draw(), "THIS HOST CANNOT EDIT"));
  }
  {
    Rig r;
    r.api.end_result = O3M_EDIT_LIMIT;
    r.press(O3M_BTN_F1);
    CHECK(r.api.param_groups.empty());
    CHECK(r.shows(r.draw(), "NOT APPLIED  (CODE 5)"));
  }
  {
    Rig r(false);
    r.api.make_abi15();
    CHECK(!r.start(O3M_MUTATE_HOST_SO));  // an OS of 1.5 has no instrument parameters
  }
  {
    EditRig r;
    CHECK(!r.start(O3M_MUTATE_HOST_SO, 0));
  }
}

TEST_CASE("mutate app: the page stays inside the canvas and overlaps nothing, in every state, at the smallest and the real size") {
  for (auto size : {std::pair<int, int>{480, 288}, std::pair<int, int>{800, 330}}) {
    Rig r;
    for (unsigned row = 0; row < 3; ++row) {
      r.check_layout(size.first, size.second, "fresh");
      r.press(O3M_BTN_DOWN);
    }
    r.press(O3M_BTN_F1);
    r.check_layout(size.first, size.second, "applied");
    r.press(O3M_BTN_F2);
    r.check_layout(size.first, size.second, "varied");
    EditRig k;
    k.api.instr[3].kind = O3M_INSTRUMENT_MIDI;
    k.launch_tools(0, 2, 0, 0, 0);
    NEED(k.start(O3M_MUTATE_HOST_SO));
    k.check_layout(size.first, size.second, "refused");
  }
}

TEST_CASE("mutate app: the packed .tmod loads, declares ABI 1.6 with read_project and edit_project, and is small") {
  const Tmod t = load_tmod(O3M_MUTATE_TMOD);
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
  CHECK(t.h.state_bytes <= 2048u);
  CHECK(t.file_bytes < 14u * 1024u);  // the module slab is 14 KiB
  std::printf("mutate.tmod: %u bytes, state %u, hot %u\n", t.file_bytes, t.h.state_bytes, t.h.hot_size);
}

TEST_CASE("mutate app: AMOUNT, GROUP and SEED are kept with set_own_param, handed back through set_param before enter(), and enter() leaves them alone (OS-APPUX S3)") {
  EditRig r;
  r.launch_tools(0, 0, 0, 16, 1);
  NEED(r.start(O3M_MUTATE_HOST_SO));
  CHECK(r.shows(r.draw(), "20"));  // AMOUNT's default, handed back by start (set_param x 3, then enter)
  CHECK(r.api.own_params.empty());
  r.turn(3);  // AMOUNT 20 + 3 x 5
  NEED(r.api.own_params.size() == 1u);
  CHECK((r.api.own_params[0].index == 0u && r.api.own_params[0].value == 35.0f));
  r.down(2);
  r.turn(7);  // SEED 7
  CHECK((r.api.own_params.size() == 2u && r.api.own_params[1].index == 2u && r.api.own_params[1].value == 7.0f));
  r.press(O3M_BTN_DELETE);
  CHECK((r.api.own_params.size() == 3u && r.api.own_params[2].index == 2u && r.api.own_params[2].value == 0.0f));
  r.press(O3M_BTN_F2);  // VARY: SEED + 1
  CHECK((r.api.own_params.size() == 4u && r.api.own_params[3].index == 2u && r.api.own_params[3].value == 1.0f));
  // a relaunch: what the OS kept comes back before enter(), which resets none of it; values past a range are clamped, a GROUP past the list reads as ALL
  r.inst.set(0, 55.0f);
  r.inst.set(1, 5.0f);
  r.inst.set(2, 321.0f);
  r.enter();
  const Drawing d = r.draw();
  CHECK(r.shows(d, "55"));
  CHECK(r.shows(d, "ALL"));
  CHECK(r.shows(d, "0321"));
  r.inst.set(0, 9999.0f);
  r.inst.set(2, -4.0f);
  const Drawing e = r.draw();
  CHECK(r.shows(e, "100"));
  CHECK(r.shows(e, "0000"));
}

TEST_CASE("mutate app: the INSTRUMENT row moves the instrument it works on, APPLY writes there, Delete is the launch instrument, and nothing is kept (OS-APPUX S5)") {
  Rig r;
  NEED(r.ok);
  r.api.add_sample(5, 0, 1.0f);
  go_to(r, 3);  // INSTRUMENT, after AMOUNT, GROUP and SEED
  r.turn(2);    // 3 -> 5
  r.press(O3M_BTN_F1);
  NEED(r.api.param_groups.size() == 1u);
  CHECK(!r.api.param_groups[0].empty());
  for (const auto& w : r.api.param_groups[0]) {
    CHECK(w.instrument == 5u);
  }
  r.press(O3M_BTN_DELETE);  // the launch instrument again
  r.press(O3M_BTN_F1);
  NEED(r.api.param_groups.size() == 2u);
  CHECK(!r.api.param_groups[1].empty());
  for (const auto& w : r.api.param_groups[1]) {
    CHECK(w.instrument == 3u);
  }
  // an instrument that is not a sample or a module says so, the row still moves, and the next one that is works again
  r.turn(3);  // 3 -> 6, an empty slot
  CHECK(r.shows(r.draw(), "SAMPLE OR MODULE INSTRUMENTS ONLY"));
  r.press(O3M_BTN_F1);
  CHECK(r.api.param_groups.size() == 2u);  // (nothing to apply to)
  r.turn(-1);                              // 5
  CHECK(!r.shows(r.draw(), "SAMPLE OR MODULE INSTRUMENTS ONLY"));
  // not declared, not kept: a new launch starts on the cursor's instrument
  r.inst.m->ui->enter(r.inst.state);
  r.press(O3M_BTN_F1);
  NEED(r.api.param_groups.size() == 3u);
  for (const auto& w : r.api.param_groups[2]) {
    CHECK(w.instrument == 3u);
  }
}
