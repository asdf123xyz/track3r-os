// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A main-loop object that lives in the SDRAM slack window instead of SRAM_U .bss (OS-MEM1). `.noinit_sdram` is NOLOAD and never cleared,
// and GCC silently drops a constant initialiser in it, so a plain `O3OS_SDRAM_NOINIT T g;` would start as whatever the SDRAM held. This
// wrapper restores what .bss gave a static: its constructor (a dynamic initialiser, run from .init_array after the SDRAM is up: startup.cpp
// step 7 before step 8) zeroes the storage and then value-constructs the T in it, so T sees exactly the state a zero-initialised static
// would have seen. Declare the box itself with O3OS_SDRAM_NOINIT; never in anything an ISR or the render touches (SDRAM is slower than SRAM_U).
#include <cstddef>
#include <cstring>
#include <new>

namespace o3os::platform::mk66 {

template <class T>
class SdramBox {
 public:
  SdramBox() noexcept {
    std::memset(raw_, 0, sizeof raw_);
    ::new (static_cast<void*>(raw_)) T();
  }
  SdramBox(const SdramBox&) = delete;
  SdramBox& operator=(const SdramBox&) = delete;
  T* operator->() noexcept { return std::launder(reinterpret_cast<T*>(raw_)); }
  T& operator*() noexcept { return *std::launder(reinterpret_cast<T*>(raw_)); }

 private:
  alignas(T) unsigned char raw_[sizeof(T)];
};

}  // namespace o3os::platform::mk66
