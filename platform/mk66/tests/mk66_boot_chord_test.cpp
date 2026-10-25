// SPDX-License-Identifier: GPL-3.0-or-later
//
// The INPUT TEST boot chord (OS-DIAG1, INP-8): the bounded poll of boot_chord.h on its own and over the real scanner and modelled TCA8418 chips.
// The unit did not open INPUT TEST from Shift + F1 + F8 held at power-up with the first read (a fixed 40 ms scan); these tests pin what the poll
// guarantees: a chord that reaches the scanner inside the window is found and ends the poll at once, a chord that never arrives costs exactly the
// window and reads as not held, and a partial chord is never taken for the whole. Runs on the host and on QEMU's Cortex-M4F (`qemu_mk66`).
#include <doctest/doctest.h>

#include "mk66_input_fakes.h"

#include <o3os/platform/mk66/boot_chord.h>

#include <cstdint>
#include <initializer_list>

namespace input = o3os::platform::mk66::input;
namespace fake = o3os::platform::mk66::input::fake;
namespace mk66 = o3os::platform::mk66;

namespace {

constexpr std::uint8_t kAddress = 0x34;
// app_main.cpp kChordButtons: Shift, F1, F8 in that order (bit 0, 1, 2). The button codes are app::Button's (Shift 29, F1 0, F8 7, F2 1); a platform test does not
// include the app layer (check_layers), and the numbers are the wiring the INP rows of the README name.
constexpr unsigned kShift = 29;
constexpr unsigned kF1 = 0;
constexpr unsigned kF2 = 1;
constexpr unsigned kF8 = 7;
constexpr unsigned kChord[mk66::kBootChordKeys] = {kShift, kF1, kF8};

struct Rig {
  fake::ManualClock clock;
  fake::Bus pad_bus;
  fake::Bus control_bus;
  fake::KeyChip pad_chip;
  fake::KeyChip button_chip;
  fake::Hardware hw{pad_chip, button_chip};
  input::Scanner scanner{pad_bus, control_bus, hw, clock};
  std::uint32_t at_ms = 0;  // the poll's own clock: the step count

  Rig() {
    pad_bus.attach(kAddress, pad_chip);
    control_bus.attach(kAddress, button_chip);
  }
  void hold_chord() {
    for (const unsigned code : kChord) {
      button_chip.press(input::key_of_button(code));
    }
  }
  unsigned held() const {
    unsigned bits = 0;
    for (unsigned i = 0; i < mk66::kBootChordKeys; ++i) {
      bits |= scanner.button_held(kChord[i]) ? 1u << i : 0u;
    }
    return bits;
  }
  // What app_main.cpp does: step = a millisecond and a scan pass. `hook` runs before each step with the step number (the chip reporting late).
  template <typename Hook>
  std::uint32_t poll(unsigned& out, Hook&& hook) {
    at_ms = 0;
    return mk66::poll_boot_chord(
        [&] {
          hook(at_ms);
          ++at_ms;
          clock.advance_ms(1);
          scanner.service();
        },
        [&] { return held(); }, out);
  }
};

}  // namespace

TEST_CASE("boot chord: the three keys are Shift, F1 and F8 and the full mask has three bits") {
  CHECK(kChord[0] == 29u);
  CHECK(kChord[1] == 0u);
  CHECK(kChord[2] == 7u);
  CHECK(mk66::kBootChordAll == 0x7u);
  CHECK(mk66::boot_chord_complete(0x7u));
  for (unsigned partial = 0; partial < 7u; ++partial) {
    CHECK_FALSE(mk66::boot_chord_complete(partial));
  }
  CHECK(mk66::kBootChordWindowMs >= 100u);  // the task's 100..200 ms
  CHECK(mk66::kBootChordWindowMs <= 200u);
}

TEST_CASE("boot chord: poll_boot_chord on its own (the decision, without a scanner)") {
  unsigned held = 99;
  std::uint32_t reads = 0;
  // Already complete: no step at all.
  CHECK(mk66::poll_boot_chord([] { FAIL_CHECK("no step expected"); }, [&] { return mk66::kBootChordAll; }, held) == 0u);
  CHECK(held == mk66::kBootChordAll);
  // Never complete: exactly the window, and the last reading is returned.
  std::uint32_t steps = 0;
  CHECK(mk66::poll_boot_chord([&] { ++steps; }, [&] { ++reads; return 0x5u; }, held) == mk66::kBootChordWindowMs);
  CHECK(steps == mk66::kBootChordWindowMs);
  CHECK(reads == mk66::kBootChordWindowMs + 1u);  // once before the first step and once after each
  CHECK(held == 0x5u);
  // Complete at step 70: stops there.
  steps = 0;
  CHECK(mk66::poll_boot_chord([&] { ++steps; }, [&] { return steps >= 70u ? mk66::kBootChordAll : 0x3u; }, held) == 70u);
  CHECK(held == mk66::kBootChordAll);
  // Complete on the very last step the window allows: found. One later: not.
  steps = 0;
  CHECK(mk66::poll_boot_chord([&] { ++steps; }, [&] { return steps >= mk66::kBootChordWindowMs ? mk66::kBootChordAll : 0u; }, held) ==
        mk66::kBootChordWindowMs);
  CHECK(mk66::boot_chord_complete(held));
  steps = 0;
  CHECK(mk66::poll_boot_chord([&] { ++steps; }, [&] { return steps > mk66::kBootChordWindowMs ? mk66::kBootChordAll : 0u; }, held) ==
        mk66::kBootChordWindowMs);
  CHECK_FALSE(mk66::boot_chord_complete(held));
}

TEST_CASE("boot chord: keys held at power-up, already in the chip's FIFO when the scanner starts, are found within a few passes") {
  Rig r;
  r.hold_chord();
  r.scanner.begin();
  unsigned held = 0;
  const std::uint32_t steps = r.poll(held, [](std::uint32_t) {});
  CHECK(mk66::boot_chord_complete(held));
  CHECK(steps <= 5u);  // read at the first pass, accepted at once (a key that never changed is not locked out)
}

TEST_CASE("boot chord: a chip that reports the held keys late is found anywhere inside the window, and not after it") {
  // The hook runs at the start of step `ms` (0..window-1): 149 is the last step the poll takes.
  for (const std::uint32_t late_ms : std::initializer_list<std::uint32_t>{1u, 39u, 40u, 41u, 100u, 140u, mk66::kBootChordWindowMs - 1u}) {  // typed: std::uint32_t is unsigned long on the target
    INFO(late_ms);
    Rig r;
    r.scanner.begin();
    unsigned held = 0;
    const std::uint32_t steps = r.poll(held, [&](std::uint32_t ms) {
      if (ms == late_ms) {
        r.hold_chord();
      }
    });
    CHECK(mk66::boot_chord_complete(held));
    CHECK(steps <= mk66::kBootChordWindowMs);
    CHECK(steps >= late_ms + 1u);
    if (late_ms < mk66::kBootChordWindowMs - 1u) {
      CHECK(steps <= late_ms + 12u);  // the safety poll (10 ms) would find it if INT were not seen; here INT is
    }
  }
  // After the window: the poll has already given up (steps = the window, nothing read: the log line shows it). The chord pressed then reaches the
  // scanner later, which is what the late pass() is not for; this pins that the poll's own answer is "not held".
  Rig r;
  r.scanner.begin();
  unsigned held = 99;
  bool fired = false;
  const std::uint32_t steps = r.poll(held, [&](std::uint32_t) { fired = true; });
  CHECK(fired);
  CHECK(steps == mk66::kBootChordWindowMs);
  CHECK(held == 0u);
  r.hold_chord();  // one step after the window
  r.clock.advance_ms(1);
  r.scanner.service();
  CHECK(mk66::boot_chord_complete(r.held()));  // the scanner has it; the poll's decision was already taken
  CHECK_FALSE(mk66::boot_chord_complete(held));
}

TEST_CASE("boot chord: a key released during the poll is not part of the chord") {
  Rig r;
  r.scanner.begin();
  unsigned held = 0;
  const std::uint32_t steps = r.poll(held, [&](std::uint32_t ms) {
    if (ms == 0u) {
      r.button_chip.press(input::key_of_button(kShift));
      r.button_chip.press(input::key_of_button(kF1));
    } else if (ms == 20u) {
      r.button_chip.release(input::key_of_button(kShift));  // Shift let go ...
    } else if (ms == 40u) {
      r.button_chip.press(input::key_of_button(kF8));  // ... before F8 went down
    }
  });
  CHECK(steps == mk66::kBootChordWindowMs);
  CHECK_FALSE(mk66::boot_chord_complete(held));
  CHECK(held == ((1u << 1) | (1u << 2)));  // bit 1 F1, bit 2 F8; bit 0 (Shift) is up
}

TEST_CASE("boot chord: a partial chord, other buttons and pads are not the chord") {
  const unsigned partial[][2] = {{kShift, kF1}, {kShift, kF8}, {kF1, kF8}};  // Shift+F1, Shift+F8, F1+F8
  for (const auto& pair : partial) {
    Rig r;
    r.button_chip.press(input::key_of_button(pair[0]));
    r.button_chip.press(input::key_of_button(pair[1]));
    r.scanner.begin();
    unsigned held = 0;
    const std::uint32_t steps = r.poll(held, [](std::uint32_t) {});
    CHECK(steps == mk66::kBootChordWindowMs);
    CHECK_FALSE(mk66::boot_chord_complete(held));
    CHECK(held != 0u);
  }
  Rig r;
  r.pad_chip.press(0);  // pads are the other chip: pad 0 is not Shift
  r.button_chip.press(input::key_of_button(kF2));
  r.scanner.begin();
  unsigned held = 99;
  CHECK(r.poll(held, [](std::uint32_t) {}) == mk66::kBootChordWindowMs);
  CHECK(held == 0u);
  CHECK(r.scanner.pad_held(0));
}

TEST_CASE("boot chord: a control chip that answers late (configure fails at first) is still read inside the window") {
  Rig r;
  r.control_bus.dead = true;  // the chip is not there for the boot's three configure attempts
  r.scanner.begin();
  CHECK_FALSE((r.scanner.stats().online & o3os::hal::InputStats::kButtonsOnline) != 0u);
  r.hold_chord();
  unsigned held = 0;
  const std::uint32_t steps = r.poll(held, [&](std::uint32_t ms) {
    if (ms == 20u) {
      r.control_bus.dead = false;  // answers from 20 ms on; the scanner's backoff (10, 20, 40 ms) retries it
    }
  });
  CHECK(mk66::boot_chord_complete(held));
  CHECK(steps < mk66::kBootChordWindowMs);
}

TEST_CASE("boot chord: the boot disk chord is Shift + F1 + F7 with F8 up, and INPUT TEST keeps precedence (OS-USBDISK U1c)") {
  constexpr unsigned kShiftF1 = 0x3u;  // the INPUT TEST poll's bits 0 (Shift) and 1 (F1)
  CHECK(mk66::kBootDiskMask == kShiftF1);
  CHECK(mk66::boot_disk_chord(kShiftF1, true));
  CHECK_FALSE(mk66::boot_disk_chord(kShiftF1, false));  // F7 up
  for (unsigned held = 0; held < 8u; ++held) {
    if (held != kShiftF1) {
      CHECK_FALSE(mk66::boot_disk_chord(held, true));  // partial chords, F1 + F7, Shift + F7, and Shift + F1 + F8 (+ F7): never the disk
    }
  }
  CHECK(mk66::boot_chord_complete(0x7u));  // Shift + F1 + F8 (+ F7) is INPUT TEST, decided first in app_main
  // Over the scanner: the chord is held, the INPUT TEST poll runs its whole window (not complete), and F7 reads down at its end.
  Rig r;
  constexpr unsigned kF7 = 6;
  r.button_chip.press(input::key_of_button(kShift));
  r.button_chip.press(input::key_of_button(kF1));
  r.button_chip.press(input::key_of_button(kF7));
  r.scanner.begin();
  unsigned held = 0;
  CHECK(r.poll(held, [](std::uint32_t) {}) == mk66::kBootChordWindowMs);
  CHECK_FALSE(mk66::boot_chord_complete(held));
  CHECK(mk66::boot_disk_chord(held, r.scanner.button_held(kF7)));
}
