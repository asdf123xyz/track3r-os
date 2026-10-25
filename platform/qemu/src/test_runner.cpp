// SPDX-License-Identifier: GPL-3.0-or-later
//
// main() of every target test suite: doctest's own runner (the test files are the host's *_test.cpp, unchanged),
// the SysTick clock started, and one summary line after the run with the facts a suite owner needs:
//
//   o3os-qemu: suite=core exit=0 rom=… data=… bss=… heap_peak=…/… stack_peak=…/… guest_ms=…
//
// The exit status is doctest's (non-zero when a test case failed), or 3 when the stack paint shows an overflow.
// It reaches QEMU through exit() -> _exit() -> SYS_EXIT_EXTENDED (src/syscalls.cpp).
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <o3os/platform/qemu/clock.h>
#include <o3os/platform/qemu/runtime.h>

#include <cstdint>
#include <cstdio>

extern "C" const char o3os_suite_name[];  // generated per suite (cmake/O3osQemu.cmake)

int main() {
  namespace qemu = o3os::platform::qemu;
  qemu::SysTickClock::start();
  // Line buffered, so what a test printed before a fault or a timeout is not lost in a full buffer.
  std::setvbuf(stdout, nullptr, _IOLBF, 256);

  doctest::Context context;
  context.setOption("no-colors", true);
  int status = context.run();

  const qemu::MemoryUse use = qemu::memory_use();
  const std::uint64_t guest_ms = qemu::system_clock().uptime_ms();
  if (status == 0 && !use.stack_intact) {
    std::printf("o3os-qemu: stack overflow: the lowest words of the stack paint were overwritten\n");
    status = 3;
  }
  std::printf("o3os-qemu: suite=%s exit=%d rom=%lu data=%lu bss=%lu heap_peak=%lu/%lu stack_peak=%lu/%lu guest_ms=%lu\n",
              o3os_suite_name, status, static_cast<unsigned long>(use.rom_bytes), static_cast<unsigned long>(use.data_bytes),
              static_cast<unsigned long>(use.bss_bytes), static_cast<unsigned long>(use.heap_peak),
              static_cast<unsigned long>(use.heap_limit), static_cast<unsigned long>(use.stack_peak),
              static_cast<unsigned long>(use.stack_size), static_cast<unsigned long>(guest_ms));
  return status;
}
