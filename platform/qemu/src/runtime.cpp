// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/platform/qemu/runtime.h>

#include <cstddef>
#include <cstdint>

extern "C" {
extern std::uint32_t __data_load[];
extern std::uint32_t __data_start[];
extern std::uint32_t __data_end[];
extern std::uint32_t __bss_start[];
extern std::uint32_t __bss_end[];
extern std::uint32_t __thread_stack_top[];
extern std::uint32_t __stack_limit[];
}

namespace o3os::platform::qemu {

std::size_t heap_peak_bytes() noexcept;   // syscalls.cpp
std::size_t heap_limit_bytes() noexcept;  // syscalls.cpp

namespace {
constexpr std::size_t kGuardWords = 8;  // the lowest words of the stack: still painted unless it overflowed
}

MemoryUse memory_use() noexcept {
  MemoryUse use{};
  use.data_bytes = static_cast<std::size_t>(__data_end - __data_start) * sizeof(std::uint32_t);
  use.bss_bytes = static_cast<std::size_t>(__bss_end - __bss_start) * sizeof(std::uint32_t);
  // .text, .rodata and the unwind tables sit below the .data load image; its end is the end of the ROM image.
  use.rom_bytes = static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(__data_load)) + use.data_bytes;
  use.heap_peak = heap_peak_bytes();
  use.heap_limit = heap_limit_bytes();
  use.stack_size = static_cast<std::size_t>(__thread_stack_top - __stack_limit) * sizeof(std::uint32_t);

  const std::uint32_t* word = __stack_limit;
  while (word < __thread_stack_top && *word == kStackPaint) {
    ++word;
  }
  use.stack_peak = static_cast<std::size_t>(__thread_stack_top - word) * sizeof(std::uint32_t);
  use.stack_intact = static_cast<std::size_t>(word - __stack_limit) >= kGuardWords;
  return use;
}

}  // namespace o3os::platform::qemu
