// SPDX-License-Identifier: GPL-3.0-or-later
//
// The wiring of the boot screen on the MK66 (OS-003f): what app_main.cpp does around it, as far as it is logic. IdleHook is the hook
// boot::delay_ms() calls once a millisecond; BootGate decides when the display is serviced from it, when the first light is seen,
// whether the app may draw on a pass, and when the hold is released. The tests run the real display driver on its chip model, with a
// Link whose delay_ms() calls the hook exactly as boot::delay_ms() does, so the re-entry the review found (service() -> a stall
// recovery -> chip init -> delay_ms -> the hook -> service() again) is reproduced here, with and without the flag that stops it.
// Runs on the host and on QEMU's Cortex-M4F (`qemu_mk66`).
#include <doctest/doctest.h>

#include "mk66_display_fakes.h"

#include <o3os/core/boot_hold.h>
#include <o3os/platform/mk66/boot_gate.h>
#include <o3os/platform/mk66/display/chip.h>
#include <o3os/platform/mk66/display/display.h>
#include <o3os/platform/mk66/idle_hook.h>

#include <cstdint>

namespace display = o3os::platform::mk66::display;
using o3os::platform::mk66::BootGate;
using o3os::platform::mk66::IdleHook;
using o3os::platform::mk66::display::fake::FakeBulk;
using o3os::platform::mk66::display::fake::FakeChip;
using Verdict = BootGate::Verdict;

namespace {

// ---- IdleHook ----------------------------------------------------------------------------------------------------------

int g_runs = 0;
IdleHook* g_hook = nullptr;
int g_depth = 0;
int g_max_depth = 0;

void count_run() noexcept { ++g_runs; }
void nest_run() noexcept {
  ++g_runs;
  ++g_depth;
  g_max_depth = g_depth > g_max_depth ? g_depth : g_max_depth;
  g_hook->run();  // a hook that waits, and the wait calls the hook again
  --g_depth;
}

// ---- A screen that counts -----------------------------------------------------------------------------------------------

struct CountingScreen {
  unsigned services = 0;
  std::uint8_t light = 0;
  void service() noexcept { ++services; }
  std::uint8_t backlight() const noexcept { return light; }
};

// ---- The real driver, behind a Link whose delay_ms() calls the idle hook (what boot::delay_ms() does) ----------------------------

class HookedLink final : public display::Link {
 public:
  HookedLink(FakeChip& chip, IdleHook& hook) : chip_(chip), hook_(hook) {}
  void select() noexcept override { chip_.select(); }
  void deselect() noexcept override { chip_.deselect(); }
  std::uint8_t xfer(std::uint8_t out) noexcept override { return chip_.xfer(out); }
  void power_down(bool asserted) noexcept override { chip_.power_down(asserted); }
  void set_speed(display::SpiSpeed speed) noexcept override { chip_.set_speed(speed); }
  void delay_ms(std::uint32_t ms) noexcept override {
    for (std::uint32_t i = 0; i < ms; ++i) {
      chip_.delay_ms(1);
      hook_.run();
    }
  }
  std::uint32_t now_ms() const noexcept override { return chip_.now_ms(); }
  bool take_error() noexcept override { return chip_.take_error(); }

 private:
  FakeChip& chip_;
  IdleHook& hook_;
};

// The display as a Screen the gate drives, counting how deep service() is entered (2 would be the driver re-entered).
struct DriverScreen {
  display::Ft812Display& disp;
  unsigned depth = 0;
  unsigned max_depth = 0;
  unsigned services = 0;
  void service() noexcept {
    ++depth;
    ++services;
    max_depth = depth > max_depth ? depth : max_depth;
    disp.service();
    --depth;
  }
  std::uint8_t backlight() const noexcept { return disp.backlight(); }
};

// The rig of app_main.cpp: the chip model, the hooked link, the driver on it, the gate and the hook installed.
struct Wiring {
  FakeChip chip;
  FakeBulk bulk{chip};
  IdleHook idle;
  HookedLink link{chip, idle};
  display::Chip driver{link};
  display::Ft812Display disp{driver, bulk};
  BootGate gate;
  DriverScreen screen{disp};

  Wiring() {
    CHECK(driver.init(display::InitConfig{}).status == display::InitStatus::Ok);
    disp.start(true);
    g_self = this;
  }
  ~Wiring() {
    g_self = nullptr;
    CHECK(chip.bus_violations() == 0u);
  }
  Wiring(const Wiring&) = delete;
  Wiring& operator=(const Wiring&) = delete;

  static Wiring* g_self;
  static void hook() noexcept { g_self->gate.idle(g_self->screen, g_self->now()); }
  void install() { idle.set(&Wiring::hook); }
  std::uint64_t now() const { return static_cast<std::uint64_t>(chip.now_ms()); }

  // A small frame, committed: the boot frame's stand-in.
  bool commit_frame(unsigned words = 20) {
    if (!disp.begin_frame()) {
      return false;
    }
    std::uint32_t w[64];
    for (unsigned i = 0; i < words && i < 63; ++i) {
      w[i] = 0x04000000u | (i + 1);
    }
    w[words] = 0;  // DISPLAY
    disp.write(w, words + 1);
    disp.end_frame();
    return true;
  }
  // One millisecond of a blocking wait: time passes and the hook runs, as in boot::delay_ms().
  void wait_ms() {
    chip.advance_ms(1);
    idle.run();
  }
  // One pass of the main loop as app_main.cpp's pass() runs it: inside the driver, the display's service, then the gate's verdict.
  Verdict pass(bool in_scope = true) {
    chip.advance_ms(1);
    if (in_scope) {
      BootGate::DriverScope scope(gate);
      screen.service();
      return gate.pass(disp, now());
    }
    screen.service();
    return gate.pass(disp, now());
  }
};
Wiring* Wiring::g_self = nullptr;

}  // namespace

// ---- The hook -----------------------------------------------------------------------------------------------------------

TEST_CASE("idle hook: runs once a call, nothing without one, and removing it stops it") {
  IdleHook h;
  CHECK_FALSE(h.installed());
  g_runs = 0;
  h.run();
  CHECK(g_runs == 0);
  h.set(&count_run);
  CHECK(h.installed());
  h.run();
  h.run();
  h.run();
  CHECK(g_runs == 3);
  h.set(nullptr);
  CHECK_FALSE(h.installed());
  h.run();
  CHECK(g_runs == 3);
}

TEST_CASE("idle hook: it is never entered from itself, and it is armed again when it returns") {
  IdleHook h;
  g_hook = &h;
  g_runs = 0;
  g_depth = 0;
  g_max_depth = 0;
  h.set(&nest_run);
  h.run();  // the hook calls run() from inside: that call does nothing
  CHECK(g_runs == 1);
  CHECK(g_max_depth == 1);
  h.run();  // and the next call, from outside, runs it again
  CHECK(g_runs == 2);
  CHECK(g_max_depth == 1);
  g_hook = nullptr;
}

// ---- The gate, with a screen that counts --------------------------------------------------------------------------------------

TEST_CASE("boot gate: idle services the display and takes the first light as the start of the 800 ms; pass holds, releases once, then runs") {
  BootGate gate;
  CountingScreen screen;
  CHECK_FALSE(gate.active());
  CHECK(gate.pass(screen, 0) == Verdict::Run);  // no hold: the app runs
  gate.shown(1000);
  CHECK(gate.active());
  CHECK(gate.pass(screen, 1001) == Verdict::Hold);
  gate.idle(screen, 1010);  // dark: serviced, no light
  CHECK(screen.services == 1u);
  CHECK(gate.pass(screen, 1011) == Verdict::Hold);
  screen.light = 8;  // the ramp's first step
  gate.idle(screen, 1150);  // the hook sees it first: the hold counts from here
  CHECK(screen.services == 2u);
  screen.light = 127;
  CHECK(gate.pass(screen, 1150 + 799) == Verdict::Hold);
  CHECK(gate.pass(screen, 1150 + 800) == Verdict::Released);  // to the millisecond, and exactly once
  CHECK_FALSE(gate.active());
  CHECK(gate.pass(screen, 1150 + 801) == Verdict::Run);
  CHECK(gate.pass(screen, 1150 + 9000) == Verdict::Run);
}

TEST_CASE("boot gate: a loop that starts after the minimum releases on its first pass, a display that never lights lets go at 3.3 s") {
  CountingScreen screen;
  BootGate late;
  late.shown(0);
  screen.light = 8;
  late.idle(screen, 150);
  CHECK(late.pass(screen, 1100) == Verdict::Released);  // boot work outlasted 150 + 800: no added delay
  BootGate dark;
  screen.light = 0;
  dark.shown(0);
  CHECK(dark.pass(screen, o3os::core::kBootMinMs + o3os::core::kBootLightGraceMs - 1) == Verdict::Hold);
  CHECK(dark.pass(screen, o3os::core::kBootMinMs + o3os::core::kBootLightGraceMs) == Verdict::Released);
  BootGate pass_sees_the_light;  // the first light may be seen by pass() rather than by the hook
  pass_sees_the_light.shown(0);
  screen.light = 5;
  CHECK(pass_sees_the_light.pass(screen, 600) == Verdict::Hold);
  CHECK(pass_sees_the_light.pass(screen, 1399) == Verdict::Hold);
  CHECK(pass_sees_the_light.pass(screen, 1400) == Verdict::Released);
}

TEST_CASE("boot gate: inside the driver the hook does nothing; scopes nest and restore what they found") {
  BootGate gate;
  CountingScreen screen;
  gate.shown(0);
  screen.light = 9;
  CHECK_FALSE(gate.in_driver());
  {
    BootGate::DriverScope outer(gate);
    CHECK(gate.in_driver());
    gate.idle(screen, 100);
    CHECK(screen.services == 0u);  // the display was not serviced, and the light was not noted
    CHECK(gate.pass(screen, 100 + 800) == Verdict::Hold);  // pass() notes it itself from here on (it is the loop, not the hook)
    {
      BootGate::DriverScope inner(gate);
      CHECK(gate.in_driver());
    }
    CHECK(gate.in_driver());  // the inner scope left the flag as it found it: set
    gate.idle(screen, 101);
    CHECK(screen.services == 0u);
  }
  CHECK_FALSE(gate.in_driver());
  gate.idle(screen, 102);  // outside again: serviced
  CHECK(screen.services == 1u);
}

TEST_CASE("boot gate: a display recovery releases the hold at once and the app runs") {
  BootGate gate;
  CountingScreen screen;
  gate.shown(0);
  screen.light = 8;
  gate.idle(screen, 100);
  CHECK(gate.pass(screen, 200) == Verdict::Hold);
  gate.assets_lost();  // the display was re-initialised: the logo went with its memory
  CHECK_FALSE(gate.active());
  CHECK(gate.pass(screen, 201) == Verdict::Run);
  gate.idle(screen, 202);  // the hook (which app_main.cpp removes) would still be harmless: it services, nothing more
  CHECK(screen.services == 2u);
  CHECK(gate.pass(screen, 203) == Verdict::Run);
}

// ---- The real driver ----------------------------------------------------------------------------------------------------

TEST_CASE("boot wiring: the hook's services during a blocking wait swap the boot frame in, the ramp follows it, and the hold ends 800 ms after the light") {
  Wiring w;
  w.install();
  CHECK(w.commit_frame());
  w.gate.shown(w.now());
  const std::uint64_t committed = w.now();
  // The audio start: 600 ms in which main() is blocked and only the hook runs. No light before the frame is on screen.
  std::uint64_t first_light = 0;
  for (int ms = 0; ms < 600; ++ms) {
    w.wait_ms();
    if (w.disp.backlight() != 0) {
      CHECK(w.disp.stats().swaps_done >= 1u);
      if (first_light == 0) {
        first_light = w.now();
      }
    }
  }
  CHECK(w.disp.stats().swaps_done >= 1u);  // the frame was swapped in under the wait, not left for the loop
  CHECK(first_light != 0);
  CHECK(first_light > committed);
  CHECK(w.disp.backlight_on());  // and the ramp ran its course: about 130 ms
  // The main loop starts at 600 ms: held until 800 ms after the first light, released exactly once there.
  std::uint64_t released = 0;
  unsigned holds = 0, runs = 0;
  for (int pass = 0; pass < 2000 && released == 0; ++pass) {
    const Verdict v = w.pass();
    if (v == Verdict::Hold) {
      ++holds;
    } else if (v == Verdict::Released) {
      released = w.now();
    } else {
      ++runs;
    }
  }
  CHECK(runs == 0u);
  CHECK(holds > 0u);
  CHECK(released != 0);
  CHECK(released >= first_light + o3os::core::kBootMinMs);
  CHECK(released <= first_light + o3os::core::kBootMinMs + 4);  // within the fake's clock noise of the register accesses
  CHECK(w.pass() == Verdict::Run);
  CHECK(w.screen.max_depth == 1u);  // the driver was never entered twice
}

TEST_CASE("boot wiring: a stall recovery inside the driver waits with delay_ms, which calls the hook: with the flag the driver is not re-entered, without it it is") {
  for (const bool flagged : {true, false}) {
    Wiring w;
    w.install();
    CHECK(w.commit_frame());
    w.gate.shown(w.now());
    for (int ms = 0; ms < 200; ++ms) {
      w.wait_ms();  // the frame is on screen and the backlight is up
    }
    CHECK(w.disp.stats().swaps_done >= 1u);
    w.chip.swap_stuck_until_reset = true;  // the next swap hangs until the chip is reset: service() will recover it
    CHECK(w.commit_frame(30));
    for (int ms = 0; ms < 700 && w.disp.stats().reinits == 0u; ++ms) {
      w.pass(flagged);  // pass() services the display from inside the driver; the recovery's waits call the hook
    }
    CHECK(w.disp.stats().swap_stalls == 1u);
    CHECK(w.disp.stats().reinits == 1u);
    CHECK_FALSE(w.disp.failed());
    CHECK(w.disp.needs_assets());
    if (flagged) {
      CHECK(w.screen.max_depth == 1u);  // the hook found the flag and let the recovery finish
    } else {
      CHECK(w.screen.max_depth == 2u);  // what the flag is for: the hook serviced the display in the middle of its own init
    }
    // The recovery the pass runs: the hold ends (the logo went with the chip's memory), the hook is removed, the app runs.
    BootGate::DriverScope scope(w.gate);
    w.gate.assets_lost();
    w.idle.set(nullptr);
    CHECK_FALSE(w.idle.installed());
    CHECK(w.gate.pass(w.disp, w.now()) == Verdict::Run);
  }
}
