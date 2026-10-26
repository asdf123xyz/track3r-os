// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The `diag` console of the rt10xx image (OS-PORT-RT10XX phase 2, usb/src/console.cpp): the usb layer's shell on the CDC port, the
// commands whose data is this platform's, and the log ring boot code and drivers write to. Everything runs in the main loop.

#include <cstdint>

namespace o3os::platform::rt10xx::console {

void init_log() noexcept;                                      // once, at the start of main: the ring is empty before
void log(const char* text) noexcept;
void log_dec(const char* text, std::uint32_t value) noexcept;  // "text 1234"
void log_hex(const char* text, std::uint32_t value) noexcept;  // "text 0x000004D2"
// Once a pass of the main loop: the host's input into the shell, the shell's output into the port, the banner when a terminal opens.
void service() noexcept;

}  // namespace o3os::platform::rt10xx::console
