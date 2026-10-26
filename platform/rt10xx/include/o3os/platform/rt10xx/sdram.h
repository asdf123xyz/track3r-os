// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The SDRAM bring-up's outcome (OS-PORT-RT10XX phase 2, src/sdram.cpp), kept for the console: which step the bring-up reached, the test
// results, how long the tests took. The boot never halts on an SDRAM failure: the OCRAM and the DTCM carry the console, which reports
// this. Nothing of the OS is placed in the SDRAM until the console has shown it working on a unit (the linker asserts the slack window
// empty).

#include <o3os/platform/mk66/memtest.h>

#include <cstdint>

namespace o3os::platform::rt10xx::sdram {

namespace memtest = o3os::platform::mk66::memtest;

enum class Step : std::uint8_t {
  NotRun = 0,
  Clock,          // the SEMC clock root and gate
  Pads,           // the 40 pads muxed
  Controller,     // the SEMC registers written, the software reset done
  PrechargeAll,   // the IP commands, in order
  AutoRefresh,
  ModeSet,
  RefreshOn,
  WiringTest,     // the data bus, byte lane and address bus tests
  CapacityTest,   // the fill test (sparse by default)
  Done,
};

struct Status {
  Step reached = Step::NotRun;    // the last step that was started
  bool ok = false;                // every step completed and every test passed
  bool full_test = false;         // the capacity test covered every word (else every semc::kSparseStride-th)
  std::uint32_t failed_command = 0;  // the IP command that did not complete or reported an error (0: none)
  std::uint32_t intr = 0;         // INTR as read after the failed command
  memtest::Result wiring = memtest::pass();
  memtest::Result capacity = memtest::pass();
  std::uint32_t test_ms = 0;      // the tests' duration (the cycle counter)
};

const char* step_name(Step step) noexcept;

// The outcome of the boot's bring-up (valid once the reset sequence has passed the SDRAM step).
const Status& status() noexcept;

// Runs the capacity test again, over every word (`full`) or sparsely, with the data cache switched off around it so that the cells are
// what is read. Main thread only; nothing may live in the SDRAM while it runs (phase 2: nothing does). Returns the new status.
const Status& retest(bool full) noexcept;

}  // namespace o3os::platform::rt10xx::sdram
