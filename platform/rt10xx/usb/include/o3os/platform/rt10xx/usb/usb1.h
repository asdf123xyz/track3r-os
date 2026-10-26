// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The OTG1 USB device of the rt10xx image (OS-PORT-RT10XX phase 2, usb/src/usb1.cpp): the register policy over the real controller, the
// clock and PHY bring-up, the driver (ci_device.h) and the usb layer's control pipe and rings, the interrupt. What the console and main
// use of it. Implemented only in the image build; the declarations compile everywhere.

#include <o3os/platform/rt10xx/usb/ci_device.h>
#include <o3os/usb/cdc.h>
#include <o3os/usb/control.h>

#include <cstdint>

namespace o3os::platform::rt10xx::usb1 {

// Brings the port up as a STANDARD device (CDC diag port + MIDI, full speed): PLL3's USB clock, the PHY, the controller, the driver's
// prepare(), the interrupt, then the pull-up. False when a wait did not complete (the PLL lock, the controller reset); nothing is attached
// then and `start_failure()` says which step. Main thread, once.
bool start() noexcept;
const char* start_failure() noexcept;  // "" when start() succeeded or was not called
bool attached() noexcept;              // start() succeeded: the pull-up is on

const o3os::usb::ControlPipe& control() noexcept;
const o3os::platform::rt10xx::usb::CiCounters& counters() noexcept;
o3os::usb::CdcTxWriter& cdc_writer() noexcept;
o3os::usb::CdcRxRing& cdc_rx() noexcept;
o3os::usb::MidiRing& midi_to_host() noexcept;
o3os::usb::MidiRing& midi_from_host() noexcept;
// The serial number the descriptors carry: the chip's 64-bit unique id as 16 hex digits.
const char* serial() noexcept;
// The main loop's nudge after it published to a ring: the driver arms what is idle now instead of at the next start of frame.
void kick() noexcept;

}  // namespace o3os::platform::rt10xx::usb1
