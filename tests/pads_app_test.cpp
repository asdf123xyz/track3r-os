// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-012b: the example app of the SDK (modules/apps/pads, ABI 1.1): the host shared object driven through the ABI (init, set_param, enter, input events, view_draw
// under the ViewHost's budgets), and the .tmod the packer makes of it (an app has no hot code: kind 4, no render) through the real loader. Host only.
#include <doctest/doctest.h>

#include <o3os/module/loader.h>
#include <o3os/module/view.h>
#include <o3os/platform/host/dir_storage.h>

#include <dlfcn.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "support/need.h"

namespace {

using namespace o3os::module;
namespace fs = std::filesystem;

struct Loaded {
  void* handle = nullptr;
  const o3m_module* entry = nullptr;
  ~Loaded() {
    if (handle != nullptr) {
      dlclose(handle);
    }
  }
};

bool open_pads(Loaded& out) {
  out.handle = dlopen(O3M_PADS_HOST_SO, RTLD_NOW | RTLD_LOCAL);
  if (out.handle == nullptr) {
    return false;
  }
  out.entry = static_cast<const o3m_module*>(dlsym(out.handle, "o3m_entry"));
  return out.entry != nullptr;
}

struct Recorder final : ViewSink {
  std::vector<std::uint8_t> fills;  // the colour of each filled rectangle
  std::vector<std::string> texts;
  unsigned outlines_as_fills = 0;
  void fill(int, int, int, int, std::uint8_t color) noexcept override { fills.push_back(color); }
  void text(int, int, const char* s, std::uint8_t, std::uint8_t) noexcept override { texts.emplace_back(s); }
  void polyline(const std::int16_t*, std::uint32_t, std::uint8_t) noexcept override {}
  void curve(const CurveCall&) noexcept override {}
  unsigned count(std::uint8_t color) const {
    unsigned n = 0;
    for (std::uint8_t f : fills) {
      n += f == color ? 1u : 0u;
    }
    return n;
  }
};

struct App {
  Loaded lib;
  alignas(16) std::uint8_t state[256] = {};
  o3m_host host{};
  ViewHost views;
  float params[2] = {0.0f, 0.0f};
  bool ready = false;

  App() {
    ready = open_pads(lib);
    if (!ready) {
      return;
    }
    host.abi_major = O3M_ABI_MAJOR;
    host.abi_minor = O3M_ABI_MINOR;
    host.size = sizeof host;
    host.draw = draw_api();
    ready = lib.entry->state_bytes <= sizeof state && lib.entry->init(state, &host) == 0;
  }
  void input(std::uint32_t kind, std::uint32_t id, std::int32_t value) {
    const o3m_ui_event ev = {kind, id, value, 0};
    CHECK(views.call_input(*lib.entry, state, 1, ev) == ViewStatus::Ok);
  }
  Recorder draw(int w = 640, int h = 384) {
    CHECK(views.draw(*lib.entry, state, 1, 0, w, h, params, 2) == ViewStatus::Ok);
    Recorder r;
    views.replay(r);
    return r;
  }
};

}  // namespace

TEST_CASE("pads app: the descriptor is an app of the SDK's ABI with one view and two parameters") {
  App a;
  NEED(a.ready);
  const o3m_module& m = *a.lib.entry;
  CHECK(m.kind == O3M_KIND_APP);
  CHECK((m.abi_major == 1u && m.abi_minor == 2u));  // the example declares ABI 1.2 on purpose (OS-H1): an app packed before 1.3 must keep running
  CHECK(std::string(m.name) == "Pads");
  CHECK(m.view_count == 1u);
  CHECK((m.ui != nullptr && m.ui->enter != nullptr && m.ui->input != nullptr && m.ui->view_draw != nullptr && m.ui->exit != nullptr));
  CHECK((m.render == nullptr && m.note_on == nullptr && m.note_off == nullptr));
  CHECK(m.param_count == 2u);
  CHECK(std::string(m.params[0].name) == "Colour");
  CHECK((m.params[0].flags & O3M_PARAM_INT) != 0u);
  CHECK((m.params[1].flags & O3M_PARAM_BOOL) != 0u);
  ModuleSet set;
  CHECK(set.add_native("pads", m));  // the set takes an app: no sound entries, a view
  // an OS that lends no draw table (ABI 1.0) cannot run it: init says so
  alignas(16) std::uint8_t state[256] = {};
  o3m_host old = a.host;
  old.draw = nullptr;
  CHECK(m.init(state, &old) != 0);
  // an OS of ABI 1.2 (a host table that ends at `draw`) still starts it, and the OS of 1.3 with every new table NULL does too
  o3m_host v12 = a.host;
  v12.abi_minor = 2;
  v12.size = static_cast<std::uint32_t>(offsetof(o3m_host, read));
  v12.read = nullptr;
  v12.edit = nullptr;
  v12.io = nullptr;
  alignas(16) std::uint8_t state12[256] = {};
  CHECK(m.init(state12, &v12) == 0);
  CHECK(!O3M_HOST_HAS(&v12, io));
  CHECK(O3M_HOST_HAS(&a.host, io));
}

TEST_CASE("pads app: pads light while held, the encoder moves the bar, the buttons are named, Delete clears") {
  App a;
  NEED(a.ready);
  const o3m_module& m = *a.lib.entry;
  m.ui->enter(a.state);
  Recorder dark = a.draw();
  CHECK(dark.count(O3M_COLOR_ACCENT) == 0u);
  CHECK(dark.texts.size() == 2u);
  CHECK(dark.texts[0] == "BUTTON -");
  CHECK(dark.texts[1] == "EVENTS 0");
  a.input(O3M_EV_PAD, 5, 100);
  a.input(O3M_EV_PAD, 40, 64);
  Recorder lit = a.draw();
  CHECK(lit.count(O3M_COLOR_ACCENT) == 2u);
  CHECK(lit.texts[1] == "EVENTS 2");
  a.input(O3M_EV_PAD, 5, 0);
  CHECK(a.draw().count(O3M_COLOR_ACCENT) == 1u);  // released: dark again (HOLD is off)
  // HOLD: the OS tells the app through set_param and in the canvas
  m.set_param(a.state, 1, 1.0f);
  a.params[1] = 1.0f;
  a.input(O3M_EV_PAD, 5, 90);
  a.input(O3M_EV_PAD, 5, 0);
  CHECK(a.draw().count(O3M_COLOR_ACCENT) == 2u);  // held on after release
  a.input(O3M_EV_BUTTON, O3M_BTN_DELETE, 1);
  CHECK(a.draw().count(O3M_COLOR_ACCENT) == 0u);
  Recorder named = a.draw();
  CHECK(named.texts[0] == "BUTTON 27");
  a.input(O3M_EV_BUTTON, O3M_BTN_F3, 0);  // a release is not a press: not named
  CHECK(a.draw().texts[0] == "BUTTON 27");
  // pads outside the 48 are ignored
  a.input(O3M_EV_PAD, 48, 100);
  a.input(O3M_EV_PAD, 255, 100);
  CHECK(a.draw().count(O3M_COLOR_ACCENT) == 0u);
  // the colour parameter picks the role
  a.params[0] = 1.0f;
  a.input(O3M_EV_PAD, 7, 100);
  Recorder green = a.draw();
  CHECK(green.count(O3M_COLOR_PLAY) == 1u);
  a.params[0] = 3.0f;
  CHECK(a.draw().count(O3M_COLOR_FX) == 1u);
  a.params[0] = 9.0f;  // beyond the table: held to the last
  CHECK(a.draw().count(O3M_COLOR_FX) == 1u);
  // the encoder: the bar grows to the right with turns up and to the left with turns down; held to +-100
  a.input(O3M_EV_ENCODER, 0, 40);
  a.input(O3M_EV_ENCODER, 0, 500);
  a.input(O3M_EV_ENCODER, 0, -1000);
  a.input(O3M_EV_ENCODER, 0, -50);
  CHECK(a.draw().texts[1] == "EVENTS 14");
  m.ui->enter(a.state);  // a new launch: dark, the bar in the middle
  CHECK(a.draw().count(O3M_COLOR_FX) == 0u);
}

TEST_CASE("pads app: the view is inside the draw budgets at every size a page gives it") {
  App a;
  NEED(a.ready);
  for (int i = 0; i < 48; ++i) {
    a.input(O3M_EV_PAD, static_cast<std::uint32_t>(i), 100);  // every pad lit: the dearest picture
  }
  const int sizes[][2] = {{800, 384}, {500, 384}, {300, 100}, {16, 16}, {640, 200}};
  for (const auto& s : sizes) {
    CAPTURE(s[0]);
    CAPTURE(s[1]);
    CHECK(a.views.draw(*a.lib.entry, a.state, 1, 0, s[0], s[1], a.params, 2) == ViewStatus::Ok);
    CHECK(a.views.calls() <= kViewMaxCalls);
    CHECK(a.views.words() <= kViewMaxWords);
  }
  // the first view is 48 rectangles, the bar, the text: well inside 96 calls
  CHECK(a.views.draw(*a.lib.entry, a.state, 1, 0, 640, 384, a.params, 2) == ViewStatus::Ok);
  CHECK(a.views.calls() < 70u);
}

namespace {

std::vector<std::uint8_t> slurp(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("pads app: the packed .tmod loads (no hot part, kind 4) and reads back as the source says") {
#ifdef O3M_FIXTURE_DIR
  const std::vector<std::uint8_t> file = slurp(std::string(O3M_FIXTURE_DIR) + "/pads.tmod");
#else
  const std::vector<std::uint8_t> file;
#endif
  if (file.empty()) {
    const char* ci = std::getenv("O3_CI");
    CHECK_MESSAGE(!(ci != nullptr && ci[0] == '1'), "the module fixtures were not built and O3_CI=1");
    MESSAGE("module fixtures not built (no arm-none-eabi-g++): skipped");
    return;
  }
  const fs::path root = fs::temp_directory_path() / ("o3m_pads_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(&file)));
  fs::remove_all(root);
  fs::create_directories(root / "modules" / "apps");
  {
    std::ofstream f(root / "modules" / "apps" / "pads.tmod", std::ios::binary);
    f.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
  }
  o3os::platform::host::DirStorage storage(root.string());
  NEED(storage.mount() == o3os::hal::storage::Status::Ok);
  ModuleSet set;
  ModuleLoader loader;
  alignas(64) static std::uint8_t sdram[8192];
  set.cold().init(sdram, 0x70000000u, sizeof sdram);  // no slab: an app has nothing to put in it
  NEED(loader.begin(storage, set, "pads", nullptr));
  for (unsigned n = 0; n < 1000 && loader.state() == ModuleLoader::State::Running; ++n) {
    loader.step(512);
  }
  CHECK(loader.state() == ModuleLoader::State::Done);
  CHECK(loader.status() == Status::Ok);
  NEED(set.count() == 1u);
  const LoadedModule* m = set.at(0);
  CHECK(m->header.kind == O3M_KIND_APP);
  CHECK(m->header.hot_size == 0u);
  CHECK(m->header.abi_minor == 2);  // a 1.2 file, loaded by an OS of 1.3 (kOsAbiMinor): no capability word, nothing new to ask of it
  CHECK(m->header.reserved2[0] == 0u);
  CHECK(m->desc.view_count == 1u);
  CHECK(m->desc.param_count == 2u);
  CHECK(m->desc.render == 0u);
  CHECK(m->desc.init != 0u);
  CHECK(m->place.hot_mem == nullptr);
  Param32 p;
  NEED(read_param(m->header, m->place, m->desc, 1, p));
  CHECK((p.flags & O3M_PARAM_BOOL) != 0u);
  CHECK(!m->hot_in_sdram);
  fs::remove_all(root);
}
