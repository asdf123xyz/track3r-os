// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The MK66 half of the display driver: DSPI2 as the FT812's Link, the eDMA as its BulkEngine, and the one bring-up call the
// image's main makes. Implemented only in the MK66 image build (display/src/device.cpp); the declarations compile everywhere
// so the header check passes on every platform.
//
// Order of events in main: bring_up() (SPI2 and eDMA on, the chip initialised with retries, the DMA path self-tested), then
// the app loop calls display().service() every pass. Nothing here is for an ISR except what device.cpp's own handler does.

#include <o3os/platform/mk66/display/chip.h>
#include <o3os/platform/mk66/display/display.h>

#include <cstdint>

#ifndef O3OS_DISPLAY_ROTATE
// The panel orientation of this build, REG_ROTATE 0..7 (CMake option O3OS_DISPLAY_ROTATE). 1: upright
// on the maintainer's unit with this driver (OS-HW1, `diag rot`); 0, ADR 0011 amendment 1's prediction, shows it upside down.
#define O3OS_DISPLAY_ROTATE 1
#endif

namespace o3os::platform::mk66::display::device {

inline constexpr unsigned kMaxAttempts = 3;  // whole bring-up sequences (each with its own PD# pulse) before giving up

static_assert(O3OS_DISPLAY_ROTATE >= 0 && O3OS_DISPLAY_ROTATE <= 7, "REG_ROTATE is three bits");
inline constexpr Orientation kBuildOrientation = static_cast<Orientation>(O3OS_DISPLAY_ROTATE);

struct BringUpResult {
  InitReport init;         // the report of the last attempt
  unsigned attempts = 0;   // sequences run (1..kMaxAttempts)
  bool up = false;         // the chip is up, the scan-out runs and the display accepts frames
  bool dma = false;        // the DMA path passed its self-test (false: everything goes by the polled path)
};

// Brings up SPI2, the eDMA channels (their descriptors, DMAMUX and interrupt line only) and the chip. Boot time, main loop
// context. Takes about 0.2 s with a healthy display, up to about 1.6 s when the chip is absent.
BringUpResult bring_up() noexcept;

// The result of the last bring_up() call, for the diagnostics to come (`diag`, ADR 0011) and for a debugger: whether the chip was
// already running at entry, how many sequences it took, whether the DMA path was proven.
const BringUpResult& last_bring_up() noexcept;

// The image's one display and chip. Frames and uploads are refused until bring_up() succeeded.
Ft812Display& display() noexcept;
Chip& chip() noexcept;

}  // namespace o3os::platform::mk66::display::device
