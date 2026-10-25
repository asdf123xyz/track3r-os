// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A bump allocator over a fixed region (OS-012a): where a module's image and its instances' state go. No heap and no free of one
// block: the OS reserves two regions for modules (the SRAM_L slab for hot code, the SDRAM module region for everything else), loads
// into them one after the other and gives the whole lot back when the project changes (reset). A failed load gives its own part back
// (mark / rollback). Main loop only.
//
// A region is a pair as in image.h: `mem` where the bytes are in this process, `addr` the address the target sees them at (on the MK66
// they are one number).

#include <cstdint>

namespace o3os::module {

class Arena {
 public:
  static constexpr std::uint32_t kMaxAlign = 64;

  // False (and the arena stays empty) when the base is not aligned to kMaxAlign in both views, or the size is zero.
  bool init(std::uint8_t* mem, std::uint32_t addr, std::uint32_t size) noexcept {
    mem_ = nullptr;
    addr_ = size_ = used_ = 0;
    if (mem == nullptr || size == 0u || (addr % kMaxAlign) != 0u || (reinterpret_cast<std::uintptr_t>(mem) % kMaxAlign) != 0u ||
        static_cast<std::uint64_t>(addr) + size > 0x100000000ull) {
      return false;
    }
    mem_ = mem;
    addr_ = addr;
    size_ = size;
    return true;
  }

  // `bytes` (not zero) at an offset that is a multiple of `align` (a power of two up to kMaxAlign). False when it does not fit.
  bool take(std::uint32_t bytes, std::uint32_t align, std::uint8_t*& mem, std::uint32_t& addr) noexcept {
    if (mem_ == nullptr || bytes == 0u || align == 0u || align > kMaxAlign || (align & (align - 1u)) != 0u) {
      return false;
    }
    const std::uint64_t start = (static_cast<std::uint64_t>(used_) + align - 1u) & ~static_cast<std::uint64_t>(align - 1u);
    if (start + bytes > size_) {
      return false;
    }
    mem = mem_ + start;
    addr = addr_ + static_cast<std::uint32_t>(start);
    used_ = static_cast<std::uint32_t>(start + bytes);
    return true;
  }

  std::uint32_t mark() const noexcept { return used_; }
  void rollback(std::uint32_t mark) noexcept { used_ = mark < used_ ? mark : used_; }
  void reset() noexcept { used_ = 0; }

  std::uint32_t used() const noexcept { return used_; }
  std::uint32_t size() const noexcept { return size_; }
  std::uint32_t available() const noexcept { return size_ - used_; }
  bool ready() const noexcept { return mem_ != nullptr; }

 private:
  std::uint8_t* mem_ = nullptr;
  std::uint32_t addr_ = 0;
  std::uint32_t size_ = 0;
  std::uint32_t used_ = 0;
};

}  // namespace o3os::module
