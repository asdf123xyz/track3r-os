// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Memory facts of a running target image (QEMU `mps2-an386` layer): what the linker script laid out and how much of
// the heap and the stack the program has used so far. The runner prints them after every suite.

#include <cstddef>
#include <cstdint>

namespace o3os::platform::qemu {

// Value the stack is painted with at reset (src/startup.cpp); memory_use() looks for the first word a run changed.
inline constexpr std::uint32_t kStackPaint = 0xA5A5A5A5u;

struct MemoryUse {
  std::size_t rom_bytes;      // code + rodata + .data load image (what flash would hold)
  std::size_t data_bytes;     // .data
  std::size_t bss_bytes;      // .bss
  std::size_t heap_peak;      // highest heap break reached through _sbrk
  std::size_t heap_limit;     // bytes of the heap region (the board's PSRAM)
  std::size_t stack_peak;     // deepest thread stack use seen (painted words overwritten)
  std::size_t stack_size;     // the thread stack (the program runs on PSP; exceptions have their own MSP stack)
  bool stack_intact;          // false if the lowest words of the stack paint were overwritten (overflow)
};

// Reads the linker symbols, the _sbrk high-water mark and the stack paint. Cheap, but scans the stack paint.
MemoryUse memory_use() noexcept;

}  // namespace o3os::platform::qemu
