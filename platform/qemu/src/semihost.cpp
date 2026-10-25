// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/platform/qemu/semihost.h>

#include <cstdint>
#include <cstring>

namespace o3os::platform::qemu {
namespace {

// Semihosting operation numbers (Arm "Semihosting for AArch32 and AArch64").
constexpr std::uint32_t kSysOpen = 0x01;
constexpr std::uint32_t kSysWrite = 0x05;
constexpr std::uint32_t kSysExitExtended = 0x20;
constexpr std::uint32_t kApplicationExit = 0x20026;  // ADP_Stopped_ApplicationExit
constexpr std::uint32_t kModeWrite = 4;              // SYS_OPEN mode "w"; with ":tt" it selects the console

// One semihosting call: r0 = operation, r1 = parameter block, result in r0. BKPT 0xAB is the Thumb trap.
std::uint32_t call(std::uint32_t op, const void* block) noexcept {
  std::uint32_t result;
  asm volatile(
      "mov r0, %1\n\t"
      "mov r1, %2\n\t"
      "bkpt 0xAB\n\t"
      "mov %0, r0"
      : "=r"(result)
      : "r"(op), "r"(block)
      : "r0", "r1", "memory");
  return result;
}

}  // namespace

void console_write(const char* data, std::size_t size) noexcept {
  // The handle is opened on first use. Console output only comes from the one main context (and, once, from a
  // fault handler that is about to end the run), so no lock is needed.
  static std::uint32_t handle = 0xFFFFFFFFu;
  if (handle == 0xFFFFFFFFu) {
    const std::uint32_t open_block[3] = {reinterpret_cast<std::uintptr_t>(":tt"), kModeWrite, 3};
    handle = call(kSysOpen, open_block);
    if (handle == 0xFFFFFFFFu) {
      return;  // no console (semihosting disabled): nothing sensible to do
    }
  }
  const std::uint32_t write_block[3] = {handle, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(data)),
                                        static_cast<std::uint32_t>(size)};
  call(kSysWrite, write_block);
}

void console_puts(const char* text) noexcept { console_write(text, std::strlen(text)); }

void exit_qemu(int code) noexcept {
  const std::uint32_t block[2] = {kApplicationExit, static_cast<std::uint32_t>(code)};
  call(kSysExitExtended, block);
  for (;;) {
    asm volatile("wfi");  // only reached when semihosting is off
  }
}

}  // namespace o3os::platform::qemu
