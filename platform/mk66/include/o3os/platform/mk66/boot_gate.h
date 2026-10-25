// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What app_main.cpp does around the boot screen (OS-003f), as a pure object so that the wiring is tested on the host and on QEMU
// (tests/mk66_boot_gate_test.cpp, against the real display driver over its chip model): when the display is serviced from the
// idle hook, when the first light is seen, whether the app may draw on this pass, and when the hold is released. app_main.cpp keeps
// what only the hardware can do (the hook's installation, the app's frame timers) and asks this.
//
//   shown(now)      the boot frame was committed: the hold starts (core::BootHold, 800 ms from the first light)
//   idle(screen)    the body of the idle hook, called by boot::delay_ms() once a millisecond: services the display and notes the
//                   first light; does nothing while the main loop is inside the driver (DriverScope)
//   pass(screen)    one pass of the main loop, after the display's own service(): Hold (skip the app's frame), Released (the
//                   minimum has passed on this very pass: the app draws now; the caller resets its frame timers and removes the
//                   hook), or Run (no hold)
//   assets_lost()   the display was re-initialised (its RAM_G, and the logo with it, is gone): the hold is over
//
// The driver is not re-entrant: Ft812Display::service() can find the swap stalled, re-initialise the chip and wait with
// boot::delay_ms(), which calls the idle hook, which must not service the display again in the middle of that. So every entry of
// the main loop into the driver (the pass, the app's frame, the asset upload) sits inside a DriverScope, and idle() honours it.
// `Screen` is anything with service() and backlight() (the display driver; a counting fake in the tests).

#include <o3os/core/boot_hold.h>

#include <cstdint>

namespace o3os::platform::mk66 {

class BootGate {
 public:
  enum class Verdict : std::uint8_t { Run, Hold, Released };

  // The boot frame was committed at `now_ms`.
  void shown(std::uint64_t now_ms) noexcept { hold_.start(now_ms); }
  bool active() const noexcept { return hold_.active(); }
  bool in_driver() const noexcept { return in_driver_; }

  // The main loop is inside the driver for the lifetime of the object (scopes nest and restore what they found).
  class DriverScope {
   public:
    explicit DriverScope(BootGate& gate) noexcept : gate_(gate), was_(gate.in_driver_) { gate.in_driver_ = true; }
    ~DriverScope() { gate_.in_driver_ = was_; }
    DriverScope(const DriverScope&) = delete;
    DriverScope& operator=(const DriverScope&) = delete;

   private:
    BootGate& gate_;
    bool was_;
  };

  // The idle hook's body. Services the display and, while a hold is on, takes the first light (the ramp's first step) as its start.
  template <typename Screen>
  void idle(Screen& screen, std::uint64_t now_ms) noexcept {
    if (in_driver_) {
      return;
    }
    DriverScope scope(*this);  // the service may wait, and the wait calls the hook again: it finds the flag
    screen.service();
    note_light(screen, now_ms);
  }

  // One pass of the main loop (after the display's service()). `Released` is returned once.
  template <typename Screen>
  Verdict pass(const Screen& screen, std::uint64_t now_ms) noexcept {
    if (!hold_.active()) {
      return Verdict::Run;
    }
    note_light(screen, now_ms);
    if (hold_.holding(now_ms)) {
      return Verdict::Hold;
    }
    hold_.release();
    return Verdict::Released;
  }

  void assets_lost() noexcept { hold_.release(); }

 private:
  template <typename Screen>
  void note_light(const Screen& screen, std::uint64_t now_ms) noexcept {
    if (hold_.active() && screen.backlight() != 0) {
      hold_.lit(now_ms);
    }
  }

  core::BootHold hold_;
  bool in_driver_ = false;
};

}  // namespace o3os::platform::mk66
