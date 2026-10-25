// SPDX-License-Identifier: GPL-3.0-or-later
//
// SdramBox (sdram_box.h, OS-MEM1): an object moved from SRAM_U .bss to the never-cleared SDRAM slack window must still start as a
// zero-initialised static did. The test builds the box over dirty memory, which is what the SDRAM holds at boot.
#include <doctest/doctest.h>

#include <o3os/platform/mk66/sdram_box.h>

#include <cstdint>
#include <cstring>
#include <new>

namespace {

struct Leaves {  // a member the constructor leaves alone: .bss made it zero, the box must too
  std::uint32_t untouched[8];
  std::uint16_t set_by_ctor = 7;
  alignas(8) std::uint8_t aligned_tail[3];
};

struct Dirty {
  alignas(o3os::platform::mk66::SdramBox<Leaves>) unsigned char bytes[sizeof(o3os::platform::mk66::SdramBox<Leaves>)];
};

}  // namespace

TEST_CASE("mk66 sdram_box: a box over dirty memory starts zeroed, then constructed") {
  using Box = o3os::platform::mk66::SdramBox<Leaves>;
  Dirty d;
  std::memset(d.bytes, 0xA5, sizeof d.bytes);
  Box* box = ::new (static_cast<void*>(d.bytes)) Box();
  CHECK((*box)->set_by_ctor == 7);
  for (std::uint32_t w : (*box)->untouched) {
    CHECK(w == 0u);
  }
  CHECK((*box)->aligned_tail[0] == 0);
  CHECK((*box)->aligned_tail[2] == 0);
  CHECK(reinterpret_cast<std::uintptr_t>(&**box) % alignof(Leaves) == 0u);
  CHECK(sizeof(Box) == sizeof(Leaves));
}
