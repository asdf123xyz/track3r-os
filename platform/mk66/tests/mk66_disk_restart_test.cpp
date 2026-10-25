// SPDX-License-Identifier: GPL-3.0-or-later
// OS-USBDISK U1c, the writable step: what a key does on the boot disk's strip (disk_restart.h). One press restarts the unit when nothing can be lost; while a computer may be writing it is a
// warning first and a restart on the second press inside the window; the restart waits for the command in flight, for a bounded time. Runs on the host and on QEMU's Cortex-M4F.
#include <doctest/doctest.h>

#include <o3os/platform/mk66/disk_restart.h>

#include <cstdint>

using o3os::platform::mk66::DiskRestart;

namespace {
using In = DiskRestart::Input;
In idle_keys() { return In{false, true, false}; }
In key(bool at_risk = true, bool busy = false) { return In{true, at_risk, busy}; }
In up(bool at_risk = true, bool busy = false) { return In{false, at_risk, busy}; }
}  // namespace

TEST_CASE("disk restart: the chord still held at power-up is not a press") {
  DiskRestart r;
  CHECK_FALSE(r.update(0, key()));
  CHECK_FALSE(r.update(100, key()));
  CHECK(r.prompt(100) == DiskRestart::Prompt::None);
  CHECK_FALSE(r.update(200, up()));    // every key up: now armed
  CHECK_FALSE(r.update(300, key()));   // a real press while a computer may be writing: the warning, not the restart
  CHECK(r.prompt(300) == DiskRestart::Prompt::Confirm);
}

TEST_CASE("disk restart: nothing at risk, one press restarts at once") {
  // (no computer has configured the disk, no card, the computer ejected it, or the disk is exported read-only: the platform says at_risk == false for all of them)
  DiskRestart r;
  (void)r.update(0, idle_keys());
  CHECK(r.update(10, key(false)));
  CHECK(r.prompt(10) == DiskRestart::Prompt::Finishing);
}

TEST_CASE("disk restart: at risk, the first press warns, the second inside the window restarts") {
  DiskRestart r;
  (void)r.update(0, up());
  CHECK_FALSE(r.update(1000, key()));
  CHECK(r.prompt(1000) == DiskRestart::Prompt::Confirm);
  CHECK_FALSE(r.update(1100, key()));  // the same press, still held: spent, not a second one
  CHECK(r.prompt(1100) == DiskRestart::Prompt::Confirm);
  CHECK_FALSE(r.update(1200, up()));
  CHECK(r.prompt(1000 + DiskRestart::kConfirmMs - 1u) == DiskRestart::Prompt::Confirm);
  CHECK(r.update(3000, key()));  // the second press
  CHECK(r.prompt(3000) == DiskRestart::Prompt::Finishing);
}

TEST_CASE("disk restart: the warning runs out, and the next press warns again") {
  DiskRestart r;
  (void)r.update(0, up());
  CHECK_FALSE(r.update(1000, key()));
  (void)r.update(1500, up());
  CHECK(r.prompt(1000 + DiskRestart::kConfirmMs) == DiskRestart::Prompt::None);
  CHECK_FALSE(r.update(1000 + DiskRestart::kConfirmMs, key()));  // too late: a first press again
  CHECK(r.prompt(1000 + DiskRestart::kConfirmMs) == DiskRestart::Prompt::Confirm);
  (void)r.update(7000, up());
  CHECK(r.update(7500, key()));
}

TEST_CASE("disk restart: the restart waits for the command in flight, and no longer than kFinishMs") {
  DiskRestart r;
  (void)r.update(0, up());
  CHECK_FALSE(r.update(100, key(true, true)));
  (void)r.update(150, up(true, true));
  CHECK_FALSE(r.update(200, key(true, true)));  // second press: asked, but a WRITE(10) is in flight
  CHECK(r.prompt(200) == DiskRestart::Prompt::Finishing);
  CHECK_FALSE(r.update(300, up(true, true)));
  CHECK_FALSE(r.update(200 + DiskRestart::kFinishMs - 1u, up(true, true)));
  CHECK(r.update(300 + 100, up(true, false)));  // the command ended: go
}

TEST_CASE("disk restart: a command that never ends does not hold the restart for ever") {
  DiskRestart r;
  (void)r.update(0, up());
  (void)r.update(10, key(false, true));  // nothing at risk but a command in flight: asked at once, waits
  CHECK(r.prompt(10) == DiskRestart::Prompt::Finishing);
  CHECK_FALSE(r.update(10 + DiskRestart::kFinishMs - 1u, up(false, true)));
  CHECK(r.update(10 + DiskRestart::kFinishMs, up(false, true)));
}

TEST_CASE("disk restart: the clock may wrap") {
  DiskRestart r;
  const std::uint32_t t = 0xFFFFFF00u;
  (void)r.update(t, up());
  CHECK_FALSE(r.update(t + 10u, key()));
  (void)r.update(t + 20u, up());
  CHECK(r.prompt(t + 10u + 100u) == DiskRestart::Prompt::Confirm);  // wraps past zero
  CHECK(r.update(t + 10u + 500u, key()));
}
