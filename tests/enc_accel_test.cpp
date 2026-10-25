// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-AUDIT-28 (E-GLOB-11, E-LOAD-09): encoder acceleration in lists only (app/enc_accel.h). The curve with an injected clock, then the real thing through the App: the File list
// (accelerates), the Pattern's cursor (does not), `off` (a row a detent), the end of the list (never past it) and Shift + encoder (a page, untouched).
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/enc_accel.h>
#include <o3os/app/file_view.h>
#include <o3os/core/action.h>
#include <o3os/core/input_event.h>
#include <o3os/core/tick.h>
#include <o3os/platform/host/fake_storage.h>
#include <o3os/platform/host/fakes.h>

#include <memory>
#include <string>

using namespace o3os;
using app::EncAccel;

namespace {

constexpr core::Tick ms(std::uint32_t n) { return core::ticks_from_ms(n); }

// A spin: `count` reports of one detent, `every_ms` apart, from `t`; the rows moved in all.
int spin(EncAccel& a, core::Tick& t, int count, std::uint32_t every_ms, int detents = 1) {
  int rows = 0;
  for (int i = 0; i < count; ++i) {
    t += ms(every_ms);
    rows += a.scale(t, detents);
  }
  return rows;
}

}  // namespace

TEST_CASE("enc accel: the curve is a row a detent until a run has t1 detents, then x4, then x8 from t2") {
  EncAccel a;  // 6 16 60
  core::Tick t = 1000000;
  int per_report[24] = {};
  for (int i = 0; i < 24; ++i) {
    t += ms(15);
    per_report[i] = a.scale(t, 1);
  }
  for (int i = 0; i < 6; ++i) {
    CHECK(per_report[i] == 1);  // the run before the report had 0..5 detents
  }
  for (int i = 6; i < 16; ++i) {
    CHECK(per_report[i] == 4);  // 6..15
  }
  for (int i = 16; i < 24; ++i) {
    CHECK(per_report[i] == 8);  // 16 and more
  }
}

TEST_CASE("enc accel: a lone report, a slow turn and a pause never accelerate; the sign is kept") {
  EncAccel a;
  core::Tick t = 5000000;
  CHECK(a.scale(t, 20) == 20);  // a batch of 20 at once (the console's `enc 20`): x1, the run starts after it
  CHECK(a.scale(t + ms(500), -3) == -3);
  core::Tick u = t + ms(1000);
  CHECK(spin(a, u, 40, 100) == 40);  // a report every 100 ms (> the 60 ms gap): a row a detent however long it goes on
  CHECK(a.scale(u + ms(1), 0) == 0);
  // a pause ends the run
  CHECK(spin(a, u, 12, 20) > 12);
  u += ms(200);
  CHECK(a.scale(u, 1) == 1);
}

TEST_CASE("enc accel: a reversal starts a new run; a batch counts all its detents toward the run") {
  EncAccel a;
  core::Tick t = 0;
  CHECK(spin(a, t, 8, 10) > 8);  // up to x4
  t += ms(10);
  CHECK(a.scale(t, -1) == -1);  // the other way: x1 again
  t += ms(10);
  CHECK(a.scale(t, 5) == 5);  // (and another reversal)
  t += ms(10);
  CHECK(a.scale(t, 3) == 3);  // run is 5 now, < t1 = 6
  t += ms(10);
  CHECK(a.scale(t, 2) == 8);  // run is 8: x4
  t += ms(10);
  CHECK(a.scale(t, -2) == -2);
}

TEST_CASE("enc accel: off is a row a detent, the numbers are the console's, an absurd report is bounded") {
  EncAccel a;
  a.cfg.on = false;
  core::Tick t = 0;
  CHECK(spin(a, t, 50, 5) == 50);
  a.cfg.on = true;
  a.cfg.t1 = 2;
  a.cfg.t2 = 3;
  a.cfg.gap_ms = 10;
  CHECK(EncAccel::valid(a.cfg));
  a.reset();
  CHECK(a.scale(1000000, 1) == 1);
  CHECK(a.scale(1000000 + ms(5), 1) == 1);  // run 1 < 2
  CHECK(a.scale(1000000 + ms(10), 1) == 4);  // run 2: x4
  CHECK(a.scale(1000000 + ms(15), 1) == 8);  // run 3: x8
  CHECK(a.scale(1000000 + ms(40), 1) == 1);  // a gap of 25 ms > 10: a new run
  EncAccel::Config bad;
  bad.t1 = 0;
  CHECK_FALSE(EncAccel::valid(bad));
  bad = EncAccel::Config{};
  bad.t2 = 3;  // t2 < t1
  CHECK_FALSE(EncAccel::valid(bad));
  bad = EncAccel::Config{};
  bad.gap_ms = 4;
  CHECK_FALSE(EncAccel::valid(bad));
  EncAccel b;
  CHECK(b.scale(1, 1 << 30) == (1 << 15));  // clamped to the input's range, x1 for a lone report
  CHECK(b.scale(2, -(1 << 30)) == -(1 << 15));
  CHECK(b.scale(3, -(1 << 30)) == -(1 << 15) * EncAccel::kMult2);
  CHECK(b.scale(4, -(1 << 30)) == -(1 << 15) * EncAccel::kMult2);  // (the run saturates at 65535, it does not wrap back to x1)
  CHECK(b.scale(5, -(1 << 30)) == -(1 << 15) * EncAccel::kMult2);
}

namespace {

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  platform::host::FakeStorage card;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);

  Rig() { app->attach_storage(&card); }
  void pass(std::uint32_t micros = 16000) {
    clock.advance(micros);
    app->run_once();
  }
  void go(app::ScreenId id) {
    (void)app->invoke(core::action_id("nav.screen"), static_cast<core::ActionArg>(id));
    pass();
  }
  void settle() {
    for (unsigned i = 0; i < 2000; ++i) {
      pass();
      const app::DirScan::State st = app->model().files->state();
      if (st != app::DirScan::State::Scanning && st != app::DirScan::State::Idle) {
        return;
      }
    }
    FAIL_CHECK("the scan did not end");
  }
  // One report of `detents`, then a pass; `micros` is how long the pass takes (the gap to the next report).
  void turn(int detents, std::uint32_t micros = 16000) {
    input.push(core::make_encoder_event(clock.now(), 0, static_cast<std::int16_t>(detents)));
    pass(micros);
  }
  int file_pos() const { return app->state().file.pos(); }
};

void add_samples(Rig& rig, unsigned n) {
  for (unsigned i = 0; i < n; ++i) {
    rig.card.add_file("s" + std::to_string(1000 + i) + ".wav", 1000);
  }
}

}  // namespace

TEST_CASE("OS-AUDIT-28: a fast spin moves the Sample load list more rows a detent, a slow turn one, off one") {
  Rig rig;
  add_samples(rig, 200);
  rig.go(app::ScreenId::SampleLoad);
  rig.settle();
  NEED(rig.app->model().files->index().count() == 200u);
  auto pos = [&] { return rig.app->state().wav.list.pos(); };
  CHECK(pos() == 0);
  for (int i = 0; i < 6; ++i) {
    rig.turn(1);  // a report every 16 ms: one run
  }
  CHECK(pos() == 6);  // the first six: a row each
  rig.turn(1);
  CHECK(pos() == 10);  // the seventh: x4
  for (int i = 0; i < 9; ++i) {
    rig.turn(1);
  }
  CHECK(pos() == 46);  // (reports 7..16 at x4)
  rig.turn(1);
  CHECK(pos() == 54);  // the 17th: x8
  rig.turn(-1);
  CHECK(pos() == 53);  // the other way: a new run, one row
  // a slow turn: one report every 100 ms, a row each
  rig.turn(-1, 100000);
  rig.turn(-1, 100000);
  rig.pass(100000);
  const int before = pos();
  for (int i = 0; i < 20; ++i) {
    rig.turn(1, 100000);
  }
  CHECK(pos() == before + 20);
  // off: a row a detent whatever the speed
  rig.app->enc_accel().cfg.on = false;
  rig.app->enc_accel().reset();
  const int at = pos();
  for (int i = 0; i < 30; ++i) {
    rig.turn(1);
  }
  CHECK(pos() == at + 30);
}

TEST_CASE("OS-AUDIT-28: the spin stops at the end of the list, never past it") {
  Rig rig;
  add_samples(rig, 30);
  rig.go(app::ScreenId::SampleLoad);
  rig.settle();
  NEED(rig.app->model().files->index().count() == 30u);
  for (int i = 0; i < 40; ++i) {
    rig.turn(1);
  }
  CHECK(rig.app->state().wav.list.pos() == 29);
  for (int i = 0; i < 40; ++i) {
    rig.turn(-1);
  }
  CHECK(rig.app->state().wav.list.pos() == 0);
  rig.turn(1000);  // one big batch from the end of the report range
  CHECK(rig.app->state().wav.list.pos() == 29);
}

TEST_CASE("OS-AUDIT-28: the Pattern's cursor and Shift + encoder are not accelerated") {
  Rig rig;
  rig.go(app::ScreenId::Pattern);
  const int row0 = rig.app->state().pattern.row();
  for (int i = 0; i < 12; ++i) {
    rig.turn(1);  // Browse: nav.scroll moves the step row, a row a detent however fast
  }
  CHECK(rig.app->state().pattern.row() == row0 + 12);
  // Shift + encoder in a list: a page a detent, however many in a run (cursor.page)
  Rig lst;
  add_samples(lst, 200);
  lst.go(app::ScreenId::SampleLoad);
  lst.settle();
  lst.input.push(core::make_button_event(lst.clock.now(), static_cast<std::uint8_t>(app::Button::Shift), true));
  lst.pass();
  for (int i = 0; i < 12; ++i) {
    lst.turn(1);
  }
  CHECK(lst.app->state().wav.list.pos() == 12 * app::FileView::kVisibleRows);
}
