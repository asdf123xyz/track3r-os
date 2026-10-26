// SPDX-License-Identifier: GPL-3.0-or-later
//
// The MPU table (mpu_config.h): the register words the boot code writes, decoded back field by field.
#include <doctest/doctest.h>

#include <o3os/platform/rt10xx/mpu_config.h>

namespace mpu = o3os::platform::rt10xx::mpu;

namespace {
std::uint32_t field(std::uint32_t v, unsigned shift, unsigned bits) { return (v >> shift) & ((1u << bits) - 1u); }
}  // namespace

TEST_CASE("rt10xx mpu: the table is valid and covers what the port uses") {
  CHECK(mpu::table_valid());
  CHECK(mpu::kRegionCount == 7u);
  CHECK(mpu::kRegionCount <= mpu::kImplementedRegions);
  CHECK(mpu::kCtrlEnable == 1u);  // PRIVDEFENA off: the background denies
}

TEST_CASE("rt10xx mpu: RBAR and RASR words") {
  for (unsigned i = 0; i < mpu::kRegionCount; ++i) {
    const mpu::Region& r = mpu::kRegions[i];
    INFO("region " << r.use);
    const std::uint32_t rbar = mpu::rbar_of(r);
    const std::uint32_t rasr = mpu::rasr_of(r);
    CHECK(field(rbar, 0, 4) == r.number);
    CHECK((rbar & 0x10u) != 0u);                 // VALID
    CHECK((rbar & ~0x1Fu) == r.base);
    CHECK((rasr & 1u) == 1u);                    // ENABLE
    CHECK(field(rasr, 1, 5) == mpu::size_field(r));
    CHECK(field(rasr, 24, 3) == static_cast<std::uint32_t>(r.access));
    CHECK(field(rasr, 28, 1) == (r.execute_never ? 1u : 0u));
    CHECK(field(rasr, 18, 1) == (r.shareable ? 1u : 0u));
    const std::uint32_t tex = field(rasr, 19, 3), c = field(rasr, 17, 1), b = field(rasr, 16, 1);
    switch (r.memory) {
      case mpu::Memory::Device: CHECK((tex == 0u && c == 0u && b == 1u)); break;
      case mpu::Memory::NormalNonCacheable: CHECK((tex == 1u && c == 0u && b == 0u)); break;
      case mpu::Memory::NormalWriteBack: CHECK((tex == 1u && c == 1u && b == 1u)); break;
    }
  }
}

TEST_CASE("rt10xx mpu: the regions the port relies on") {
  CHECK(mpu::kRegions[0].size == 0u);  // 4 GiB background
  CHECK(mpu::size_field(mpu::kRegions[0]) == 31u);
  CHECK(mpu::kRegions[1].base == 0u);
  CHECK(mpu::kRegions[1].size == 131072u);
  CHECK_FALSE(mpu::kRegions[1].execute_never);  // the ITCM runs hot code
  CHECK(mpu::kRegions[2].base == 0x20000000u);
  CHECK(mpu::kRegions[2].execute_never);
  CHECK(mpu::kRegions[3].base == 0x20200000u);
  CHECK(mpu::kRegions[3].memory == mpu::Memory::NormalNonCacheable);  // bus-master buffers
  CHECK(mpu::kRegions[4].base == 0x40000000u);
  CHECK(mpu::kRegions[4].memory == mpu::Memory::Device);
  CHECK(mpu::kRegions[5].base == 0x60000000u);
  CHECK(mpu::kRegions[5].access == mpu::Access::ReadOnly);  // the image never programs the flash
  CHECK(mpu::kRegions[6].base == 0x80000000u);
  CHECK(mpu::kRegions[6].size == 64u * 1024u * 1024u);
  CHECK(mpu::kRegions[6].memory == mpu::Memory::NormalWriteBack);
  CHECK_FALSE(mpu::kRegions[6].execute_never);  // module code may run from the SDRAM (ADR 0002)
  CHECK(mpu::size_field(mpu::kRegions[5]) == 23u);
  CHECK(mpu::size_field(mpu::kRegions[6]) == 25u);
  CHECK(mpu::size_field(mpu::kRegions[3]) == 14u);  // 32 KiB
}
