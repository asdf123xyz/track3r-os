// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// ARM semihosting for the QEMU `mps2-an386` target test layer: the console and the exit code. QEMU must run with
// `-semihosting-config enable=on,target=native` (cmake/O3osQemu.cmake does that); without it the BKPT traps and
// the fault handler cannot report anything. Never linked by core..app; this is the platform layer.

#include <cstddef>

namespace o3os::platform::qemu {

// Writes bytes to the console, which QEMU forwards to the host (SYS_WRITE on ":tt"). Not reentrant: a fault
// handler that interrupts a console write may garble that line, nothing else.
void console_write(const char* data, std::size_t size) noexcept;

// Writes a NUL-terminated string to the console.
void console_puts(const char* text) noexcept;

// Ends the QEMU process with `code` as its exit status (SYS_EXIT_EXTENDED, ADP_Stopped_ApplicationExit).
// Does not return.
[[noreturn]] void exit_qemu(int code) noexcept;

}  // namespace o3os::platform::qemu
