// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-H3B (new Community OS behaviour): Morph. The arithmetic (morph_math.h) against a double-precision reckoning, and the module's host shared object driven through the ABI as the OS drives it, on a
// fake of the ABI 1.6 host tables (EditRig): POS 0 is A0 and POS 100 is B exactly, a continuous parameter blends and a STEPPED or ENUM one switches at POS 50, A0 is the instrument as it was when the
// page opened (so a second APPLY blends from it, not from the first APPLY), RESET puts it back bit for bit, only instruments of the same kind and engine are offered, non-writable parameters are never
// touched, no live sweep (turning POS writes nothing), a module instrument, the refusals, the page and the packed .tmod. The undo of a real APPLY is in tests/module_edit_test.cpp. Host only.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../../../tests/o3m_test_support.h"

#include "../../common/o3m_edit.h"
#include "../morph_math.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using namespace o3m_test;
using namespace o3os::module;
namespace mo = morph;

namespace {

constexpr unsigned kRowSource = 0, kRowPos = 1;

void go_to(EditRig& r, unsigned row) {
  r.press(O3M_BTN_UP);
  r.press(O3M_BTN_UP);
  r.down(row);
}

// A = instrument 3 (the context's), B = 5 (the same engine), 11 (the same engine again), 7 (another play engine), 9 (a module).
struct Rig : EditRig {
  explicit Rig(bool started = true) {
    api.add_sample(3, 0, 0.0f);
    api.add_sample(5, 0, 3.0f);
    api.add_sample(11, 0, 2.0f);
    api.add_sample(7, 2, 1.0f);
    api.add_module(9, 0x1234u, 12, 1.0f);
    api.find_param(5, 13)->value = 4;  // B's play engine and play mode differ from A's: they are not writable, so A keeps its own
    api.find_param(5, 0x14)->value = 6;
    launch_tools(0, 2, 0, 0, 0);
    if (started) {
      NEED(start(O3M_MORPH_HOST_SO));
    }
  }
  float a(unsigned id) { return api.find_param(3, id)->value; }
  float b(unsigned id) { return api.find_param(5, id)->value; }
  std::vector<float> values(unsigned n) const {
    std::vector<float> v;
    for (const auto& p : api.instr[n].params) {
      v.push_back(p.value);
    }
    return v;
  }
  void pos(int percent) { go_to(*this, kRowPos), turn(percent / 5 - 10); }  // from the default 50
};

}  // namespace

// ---- the rule ---------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("morph rule: POS 0 is A0 and POS 100 is B to the last bit; between them a continuous parameter blends linearly and a STEPPED or ENUM one switches at POS 50") {
  for (float mn : {0.0f, -24.0f}) {
    for (float mx : {1.0f, 4.3f, 10000.0f}) {
      for (float a0 : {mn, mn + 0.1f * (mx - mn), mn + 0.77f * (mx - mn), mx}) {
        for (float b : {mn, mn + 0.33f * (mx - mn), mx}) {
          CHECK(mo::morph_value(a0, b, mn, mx, false, 0) == a0);
          CHECK(mo::morph_value(a0, b, mn, mx, false, 100) == b);
          CHECK(mo::morph_value(a0, b, mn, mx, true, 0) == a0);
          CHECK(mo::morph_value(a0, b, mn, mx, true, 100) == b);
          for (unsigned pos = 1; pos < 100; ++pos) {
            const double want = static_cast<double>(a0) + (static_cast<double>(b) - static_cast<double>(a0)) * pos / 100.0;
            CHECK(std::fabs(static_cast<double>(mo::morph_value(a0, b, mn, mx, false, pos)) - want) <= 1e-5 * (mx - mn) + 1e-6);
            CHECK(mo::morph_value(a0, b, mn, mx, true, pos) == (pos >= 50 ? b : a0));
          }
        }
      }
    }
  }
}

// ---- the app ----------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("morph app: descriptor") {
  EditRig r;
  NEED(r.lib.open(O3M_MORPH_HOST_SO));
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
  CHECK(keys->f[1] == nullptr);
  CHECK(keys->shift_f[1] == nullptr);
  CHECK((keys->f[2] != nullptr && std::strcmp(keys->f[2], "RESET") == 0));
  CHECK((keys->shift_f[2] != nullptr && std::strcmp(keys->shift_f[2], "RESET") == 0));
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
  CHECK(std::string(m.name) == "Morph");
  CHECK((m.view_count == 1u && m.param_count == 2u && m.set_param != nullptr));  // Source, Pos: declared as storage (the page draws them itself)
}

TEST_CASE("morph app: SOURCE lists only the instruments of the same kind and engine; the encoder walks them; none says NO MATCHING INSTRUMENT") {
  Rig r;
  NEED(r.ok);
  CHECK(r.shows(r.draw(), "05"));
  CHECK(r.shows(r.draw(), "2 MATCHING INSTRUMENTS"));  // 5 and 11; not 7 (engine 2), 9 (a module) or A itself
  r.turn(1);
  CHECK(r.shows(r.draw(), "11"));
  r.turn(5);  // stops at the last
  CHECK(r.shows(r.draw(), "11"));
  r.turn(-5);
  CHECK(r.shows(r.draw(), "05"));
  EditRig n;
  n.api.add_sample(3, 0, 0.0f);
  n.api.add_sample(7, 2, 1.0f);
  n.launch_tools(0, 2, 0, 0, 0);
  NEED(n.start(O3M_MORPH_HOST_SO));
  CHECK(n.shows(n.draw(), "NO MATCHING INSTRUMENT"));
  n.press(O3M_BTN_F1);
  n.press(O3M_BTN_F3);
  CHECK((n.api.begins == 0 && n.api.param_groups.empty()));
}

TEST_CASE("morph app: POS 100 puts B's values in A exactly (writable parameters only), as ONE group; B and the non-writable parameters are untouched") {
  Rig r;
  const std::vector<float> a0 = r.values(3), b0 = r.values(5), c0 = r.values(11);
  r.pos(100);
  r.press(O3M_BTN_F1);
  NEED(r.api.param_groups.size() == 1u);
  CHECK(r.values(5) == b0);
  CHECK(r.values(11) == c0);
  for (std::size_t i = 0; i < a0.size(); ++i) {
    const auto& p = r.api.instr[3].params[i];
    if ((p.flags & O3M_PD_APP_WRITABLE) != 0u) {
      CHECK_MESSAGE(p.value == b0[i], "id " << p.id);
    } else {
      CHECK_MESSAGE(p.value == a0[i], "id " << p.id);  // PLAY ENGINE and PLAY MODE: B's differ, A keeps its own
    }
  }
  for (const auto& w : r.api.param_groups[0]) {
    CHECK(w.instrument == 3u);
    CHECK(w.id != 13);
    CHECK(w.id != 0x14);
  }
  CHECK(r.shows(r.draw(), "PARAMETERS CHANGED"));
}

TEST_CASE("morph app: POS 50 blends the continuous parameters half way and switches the STEPPED and ENUM ones to B; POS 45 keeps those on A0; another SOURCE morphs towards it") {
  Rig r;
  const std::vector<float> a0 = r.values(3), b0 = r.values(5);
  r.pos(50);
  r.press(O3M_BTN_F1);
  for (std::size_t i = 0; i < a0.size(); ++i) {
    const auto& p = r.api.instr[3].params[i];
    if ((p.flags & O3M_PD_APP_WRITABLE) == 0u) {
      continue;
    }
    if ((p.flags & (O3M_PD_STEPPED | O3M_PD_ENUM)) != 0u) {
      CHECK_MESSAGE(p.value == b0[i], "id " << p.id);
    } else {
      CHECK_MESSAGE(std::fabs(p.value - 0.5 * (a0[i] + b0[i])) <= 1e-5 * (p.max - p.min) + 1e-6, "id " << p.id);
    }
  }
  Rig q;
  q.pos(45);
  q.press(O3M_BTN_F1);
  for (std::size_t i = 0; i < a0.size(); ++i) {
    const auto& p = q.api.instr[3].params[i];
    if ((p.flags & (O3M_PD_STEPPED | O3M_PD_ENUM)) != 0u) {
      CHECK_MESSAGE(p.value == a0[i], "id " << p.id);
    }
  }
  CHECK(q.a(7) != a0[4]);  // CUTOFF did blend
  Rig s;
  s.turn(1);  // SOURCE 11
  s.pos(100);
  s.press(O3M_BTN_F1);
  CHECK(s.a(0) == s.api.find_param(11, 0)->value);
  CHECK(s.a(0) != s.b(0));
}

TEST_CASE("morph app: POS 0 right after the page opens is A0: nothing moves and no group is made; A0 is the instrument as it was when the page opened, so APPLY twice blends from A0 each time") {
  Rig r;
  const std::vector<float> a0 = r.values(3), b0 = r.values(5);
  r.pos(0);
  r.press(O3M_BTN_F1);
  CHECK((r.api.begins == 1 && r.api.groups.size() == 1u));  // the group is made and closed empty: the OS makes no unit of it
  CHECK(r.api.param_groups.back().empty());
  CHECK(r.values(3) == a0);
  CHECK(r.shows(r.draw(), "NOTHING MOVED"));
  // 100 %, then 25 %: the second is a blend of A0 and B, not of the first result
  r.turn(20);
  r.press(O3M_BTN_F1);
  CHECK(r.values(3) != a0);
  go_to(r, kRowPos);
  r.turn(-15);  // 25
  r.press(O3M_BTN_F1);
  for (std::size_t i = 0; i < a0.size(); ++i) {
    const auto& p = r.api.instr[3].params[i];
    if ((p.flags & O3M_PD_APP_WRITABLE) != 0u && (p.flags & (O3M_PD_STEPPED | O3M_PD_ENUM)) == 0u) {
      CHECK_MESSAGE(std::fabs(p.value - (a0[i] + (b0[i] - a0[i]) * 0.25)) <= 1e-5 * (p.max - p.min) + 1e-6, "id " << p.id);
    }
  }
}

TEST_CASE("morph app: F3 RESET puts A back to A0 bit for bit, as ONE group, and sets POS to 0; there is no live sweep: turning POS or SOURCE writes nothing") {
  Rig r;
  const std::vector<float> a0 = r.values(3);
  r.pos(100);
  r.press(O3M_BTN_F1);
  CHECK(r.values(3) != a0);
  const std::size_t groups = r.api.param_groups.size();
  go_to(r, kRowPos);
  r.turn(-3);
  go_to(r, kRowSource);
  r.turn(1);
  r.turn(-1);
  CHECK((r.api.param_groups.size() == groups && r.api.begins == static_cast<int>(groups)));  // nothing written by turning
  r.press(O3M_BTN_F3);
  CHECK(r.api.param_groups.size() == groups + 1u);
  CHECK(r.values(3) == a0);
  CHECK(r.shows(r.draw(), "PARAMETERS PUT BACK"));
  CHECK(r.shows(r.draw(), "0 %"));
  r.press(O3M_BTN_F3);  // again: nothing to put back
  CHECK(r.values(3) == a0);
  CHECK(r.shows(r.draw(), "NOTHING MOVED"));
}

TEST_CASE("morph app: module instruments: only the same module is offered (the engine value is the module's); stepped parameters switch, the others blend") {
  EditRig r;
  r.api.add_module(3, 0xAAAAu, 12, 0.0f);
  r.api.add_module(4, 0xBBBBu, 12, 3.0f);  // another module
  r.api.add_module(6, 0xAAAAu, 12, 4.0f);  // the same module
  r.api.add_sample(8, 0, 1.0f);
  r.launch_tools(0, 2, 0, 0, 0);
  NEED(r.start(O3M_MORPH_HOST_SO));
  CHECK(r.shows(r.draw(), "1 MATCHING INSTRUMENT"));
  CHECK(r.shows(r.draw(), "06"));
  const std::vector<float> keep = [&] {
    std::vector<float> v;
    for (const auto& p : r.api.instr[3].params) v.push_back(p.value);
    return v;
  }();
  go_to(r, kRowPos);
  r.turn(10);
  r.press(O3M_BTN_F1);
  NEED(r.api.param_groups.size() == 1u);
  for (std::size_t i = 0; i < keep.size(); ++i) {
    const auto& p = r.api.instr[3].params[i];
    const float b = r.api.instr[6].params[i].value;
    CHECK_MESSAGE(p.value == b, "id " << p.id);
  }
}

TEST_CASE("morph app: the refusals: not a sample or module instrument, opened from the app list, no edit table, an edit_end that fails, an OS of 1.5") {
  for (std::uint32_t kind : {std::uint32_t{O3M_INSTRUMENT_EMPTY}, std::uint32_t{O3M_INSTRUMENT_MIDI}}) {
    EditRig r;
    r.api.instr[3].kind = kind;
    r.launch_tools(0, 2, 0, 0, 0);
    NEED(r.start(O3M_MORPH_HOST_SO));
    r.press(O3M_BTN_F1);
    r.press(O3M_BTN_F3);
    CHECK((r.api.begins == 0 && r.api.param_groups.empty()));
    CHECK(r.shows(r.draw(), "SAMPLE OR MODULE INSTRUMENTS ONLY"));
  }
  {
    Rig r(false);
    r.api.launch = {sizeof(o3m_launch_ctx), 0, 0, 0, 0, 0, 0, 0, {0, 0, 0}};
    NEED(r.start(O3M_MORPH_HOST_SO));
    r.press(O3M_BTN_F1);
    CHECK(r.api.begins == 0);
    CHECK(r.shows(r.draw(), "OPEN IT FROM PATTERN F7 TOOLS"));
  }
  {
    Rig r(false);
    NEED(r.start(O3M_MORPH_HOST_SO, O3M_CAP_READ_PROJECT));
    r.pos(100);
    r.press(O3M_BTN_F1);
    CHECK(r.api.begins == 0);
    CHECK(r.shows(r.draw(), "THIS HOST CANNOT EDIT"));
  }
  {
    Rig r;
    r.api.end_result = O3M_EDIT_LIMIT;
    r.pos(100);
    r.press(O3M_BTN_F1);
    CHECK(r.api.param_groups.empty());
    CHECK(r.shows(r.draw(), "NOT APPLIED  (CODE 5)"));
  }
  {
    Rig r(false);
    r.api.make_abi15();
    CHECK(!r.start(O3M_MORPH_HOST_SO));
  }
  {
    EditRig r;
    CHECK(!r.start(O3M_MORPH_HOST_SO, 0));
  }
}

TEST_CASE("morph app: the page stays inside the canvas and overlaps nothing, in every state, at the smallest and the real size") {
  for (auto size : {std::pair<int, int>{480, 288}, std::pair<int, int>{800, 330}}) {
    Rig r;
    for (unsigned row = 0; row < 2; ++row) {
      r.check_layout(size.first, size.second, "fresh");
      r.press(O3M_BTN_DOWN);
    }
    r.pos(100);
    r.press(O3M_BTN_F1);
    r.check_layout(size.first, size.second, "applied");
    r.press(O3M_BTN_F3);
    r.check_layout(size.first, size.second, "reset");
    EditRig n;
    n.api.add_sample(3, 0, 0.0f);
    n.launch_tools(0, 2, 0, 0, 0);
    NEED(n.start(O3M_MORPH_HOST_SO));
    n.check_layout(size.first, size.second, "no match");
  }
}

TEST_CASE("morph app: the packed .tmod loads, declares ABI 1.6 with read_project and edit_project, and is small") {
  const Tmod t = load_tmod(O3M_MORPH_TMOD);
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
  std::printf("morph.tmod: %u bytes, state %u, hot %u\n", t.file_bytes, t.h.state_bytes, t.h.hot_size);
}

TEST_CASE("morph app: SOURCE and POS are kept with set_own_param (SOURCE as the instrument number), handed back through set_param before enter(), and enter() leaves them alone (OS-APPUX S3)") {
  EditRig r;
  r.launch_tools(0, 0, 0, 16, 1);
  NEED(r.start(O3M_MORPH_HOST_SO));
  CHECK(r.shows(r.draw(), "50"));  // POS's default, handed back by start (set_param x 2, then enter)
  CHECK(r.api.own_params.empty());
  r.down();
  r.turn(2);  // POS 50 + 2 x 5
  NEED(r.api.own_params.size() == 1u);
  CHECK((r.api.own_params[0].index == 1u && r.api.own_params[0].value == 60.0f));
  r.press(O3M_BTN_DELETE);
  CHECK((r.api.own_params.size() == 2u && r.api.own_params[1].index == 1u && r.api.own_params[1].value == 50.0f));
  r.press(O3M_BTN_F3);  // RESET: A back, POS 0
  CHECK((r.api.own_params.size() == 3u && r.api.own_params[2].index == 1u && r.api.own_params[2].value == 0.0f));
  // SOURCE kept as an instrument number (0 = the first candidate): none to choose here (no instrument), so the row keeps 0
  r.up();
  r.turn(1);
  CHECK((r.api.own_params.size() == 4u && r.api.own_params[3].index == 0u && r.api.own_params[3].value == 0.0f));
  // a relaunch: POS comes back before enter(), which resets nothing; a value past the range is clamped
  r.inst.set(1, 35.0f);
  r.enter();
  CHECK(r.shows(r.draw(), "35"));
  r.inst.set(1, 1e6f);
  CHECK(r.shows(r.draw(), "100"));
}

TEST_CASE("morph app: the INSTRUMENT A row moves A, its sources follow, APPLY writes into the new A, Delete is the launch instrument and nothing is kept (OS-APPUX S5)") {
  Rig r;
  NEED(r.ok);
  go_to(r, 2);  // INSTRUMENT A, after SOURCE and POS
  r.turn(2);    // 3 -> 5
  r.press(O3M_BTN_F1);
  NEED(r.api.param_groups.size() == 1u);
  CHECK(!r.api.param_groups[0].empty());
  for (const auto& w : r.api.param_groups[0]) {
    CHECK(w.instrument == 5u);
  }
  r.press(O3M_BTN_DELETE);
  r.press(O3M_BTN_F1);
  NEED(r.api.param_groups.size() == 2u);
  for (const auto& w : r.api.param_groups[1]) {
    CHECK(w.instrument == 3u);
  }
  r.inst.m->ui->enter(r.inst.state);  // a new launch: the cursor's instrument again
  r.press(O3M_BTN_F1);
  for (const auto& w : r.api.param_groups.back()) {
    CHECK(w.instrument == 3u);
  }
}

TEST_CASE("morph app: an INSTRUMENT A move that leaves A where it is (Delete on the launch instrument, a detent at 64) keeps A0, so RESET still puts the first sound back (OS-APPUX S5)") {
  Rig r;
  const std::vector<float> a0 = r.values(3);
  r.pos(100);
  r.press(O3M_BTN_F1);
  CHECK(r.values(3) != a0);
  go_to(r, 2);              // INSTRUMENT A
  r.press(O3M_BTN_DELETE);  // already the launch instrument: A did not change
  r.press(O3M_BTN_F3);
  CHECK(r.values(3) == a0);  // (an A0 read again from the blend would have left it blended)
  // the same at the end of the row: A is 64 and a detent up stays there
  EditRig q;
  q.api.add_sample(64, 0, 0.0f);
  q.api.add_sample(5, 0, 3.0f);
  q.launch_tools(0, 63, 0, 0, 0);
  NEED(q.start(O3M_MORPH_HOST_SO));
  std::vector<float> z0;
  for (const auto& p : q.api.instr[64].params) {
    z0.push_back(p.value);
  }
  go_to(q, kRowPos);
  q.turn(10);  // POS 100
  q.press(O3M_BTN_F1);
  bool moved = false;
  for (std::size_t i = 0; i < z0.size(); ++i) {
    moved = moved || q.api.instr[64].params[i].value != z0[i];
  }
  CHECK(moved);
  go_to(q, 2);
  q.turn(5);  // past the last instrument
  q.press(O3M_BTN_F3);
  for (std::size_t i = 0; i < z0.size(); ++i) {
    CHECK_MESSAGE(q.api.instr[64].params[i].value == z0[i], "param " << i << " after RESET");
  }
}

TEST_CASE("morph app: the SOURCE the user chose survives an A that is B for a while: A moved onto B and back finds B again (OS-APPUX S5)") {
  Rig r;
  const std::vector<float> c0 = r.values(11);
  r.turn(1);                      // SOURCE: 5 -> 11
  CHECK(r.shows(r.draw(), "11"));
  go_to(r, 2);
  r.turn(8);                      // A: 3 -> 11, B can no longer be 11
  r.turn(-8);                     // A: back on 3
  go_to(r, kRowPos);
  r.turn(10);                     // POS 100
  r.press(O3M_BTN_F1);
  for (std::size_t i = 0; i < c0.size(); ++i) {
    const auto& p = r.api.instr[3].params[i];
    if ((p.flags & O3M_PD_APP_WRITABLE) != 0u) {
      CHECK_MESSAGE(p.value == c0[i], "id " << p.id << ": A holds 11 (the SOURCE), not 5 (the first candidate)");
    }
  }
}
