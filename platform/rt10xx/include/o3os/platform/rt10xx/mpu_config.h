// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The MPU table of the rt10xx image (OS-PORT-RT10XX), as data the boot code writes region by region, with the register words computed
// here so the host tests can check them. ARMv7-M PMSA (ARMv7-M Architecture Reference Manual, "Protected Memory System Architecture"):
// MPU_RBAR = base | VALID | region number; MPU_RASR = XN | AP | TEX S C B | SIZE | ENABLE, with SIZE = log2(bytes) - 1. The Cortex-M7 of
// the RT10xx implements 16 regions; a higher-numbered region wins where two overlap.
//
// This port's table (its own, not the official firmware's; the official one's regions are recorded in FINDINGS section 8 and differ in
// the attributes): the background denies everything, so a pointer into RAM that does not exist (the OCRAM window beyond its 32 KiB,
// FINDINGS 11) faults as a MemManage with the address in MMFAR instead of a BusFault. The TCMs are never cached by the core, whatever their
// attribute; the SDRAM is write-back cacheable, which is sound as long as only the core touches it (the port's rule: no bus master reads
// or writes the SDRAM; DMA buffers live in the DTCM or the OCRAM). The OCRAM is non-cacheable for that reason. The flash is cacheable and
// read-only to the core (the bootloader programs it, never the image). Peripherals are Device memory.

#include <o3os/platform/rt10xx/mem_map.h>

#include <cstdint>

namespace o3os::platform::rt10xx::mpu {

enum class Access : std::uint8_t { None = 0, ReadWrite = 3, ReadOnly = 6 };  // AP field values (privileged = user)
// TEX, C, B as the ARMv7-M table reads them: Device (TEX 0, C 0, B 1); Normal non-cacheable (TEX 1, C 0, B 0); Normal write-back,
// write-allocate (TEX 1, C 1, B 1).
enum class Memory : std::uint8_t { Device, NormalNonCacheable, NormalWriteBack };

struct Region {
  std::uint8_t number;
  std::uint32_t base;
  std::uint32_t size;  // a power of two, at least 32 bytes; the base is a multiple of it
  Access access;
  Memory memory;
  bool execute_never;
  bool shareable;
  const char* use;
};

inline constexpr Region kRegions[] = {
    {0, 0x00000000u, 0u, Access::None, Memory::NormalNonCacheable, true, false, "background: deny"},  // size 0 = the whole 4 GiB (SIZE 31)
    {1, mem::kItcmBase, mem::kItcmSize, Access::ReadWrite, Memory::NormalNonCacheable, false, false, "ITCM: hot code, the module slab"},
    {2, mem::kDtcmBase, 512u * mem::kKiB, Access::ReadWrite, Memory::NormalNonCacheable, true, false, "DTCM window (352 KiB answer)"},
    {3, mem::kOcramBase, mem::kOcramSize, Access::ReadWrite, Memory::NormalNonCacheable, true, true, "OCRAM: bus-master buffers"},
    {4, 0x40000000u, 4u * mem::kMiB, Access::ReadWrite, Memory::Device, true, true, "peripherals"},
    {5, mem::kFlashBase, mem::kFlashSize, Access::ReadOnly, Memory::NormalWriteBack, false, false, "FlexSPI flash: code and constants"},
    {6, mem::kSdramBase, mem::kSdramSize, Access::ReadWrite, Memory::NormalWriteBack, false, false, "SDRAM: samples, project, module code"},
};
inline constexpr unsigned kRegionCount = sizeof(kRegions) / sizeof(kRegions[0]);
inline constexpr unsigned kImplementedRegions = 16;

// A size of 0 in the table means the whole 4 GiB address space (SIZE field 31), which no 32-bit byte count can write.
constexpr bool is_power_of_two(std::uint32_t v) noexcept { return v != 0u && (v & (v - 1u)) == 0u; }
constexpr unsigned log2_of(std::uint32_t v) noexcept {
  unsigned n = 0;
  while (v > 1u) {
    v >>= 1u;
    ++n;
  }
  return n;
}
constexpr std::uint32_t size_field(const Region& r) noexcept { return r.size == 0u ? 31u : log2_of(r.size) - 1u; }

constexpr std::uint32_t rbar_of(const Region& r) noexcept { return r.base | (1u << 4) | r.number; }

constexpr std::uint32_t rasr_of(const Region& r) noexcept {
  std::uint32_t tex = 0, c = 0, b = 0;
  switch (r.memory) {
    case Memory::Device: tex = 0; c = 0; b = 1; break;
    case Memory::NormalNonCacheable: tex = 1; c = 0; b = 0; break;
    case Memory::NormalWriteBack: tex = 1; c = 1; b = 1; break;
  }
  return (r.execute_never ? 1u << 28 : 0u) | (static_cast<std::uint32_t>(r.access) << 24) | (tex << 19) |
         (r.shareable ? 1u << 18 : 0u) | (c << 17) | (b << 16) | (size_field(r) << 1) | 1u;
}

// MPU_CTRL: ENABLE only. PRIVDEFENA stays 0 (the background region denies; nothing not listed is reachable), HFNMIENA 0 (a fault handler
// runs with the MPU off, so the crash record can be written whatever region it lands in).
inline constexpr std::uint32_t kCtrlEnable = 1u << 0;

constexpr bool table_valid() noexcept {
  for (unsigned i = 0; i < kRegionCount; ++i) {
    const Region& r = kRegions[i];
    if (r.number != i || r.number >= kImplementedRegions) {
      return false;
    }
    if (r.size != 0u && (!is_power_of_two(r.size) || r.size < 32u || r.base % r.size != 0u)) {
      return false;
    }
    if (r.size == 0u && r.base != 0u) {
      return false;
    }
  }
  return true;
}
static_assert(table_valid(), "MPU regions: numbered in order, sizes powers of two, bases aligned to the size");
static_assert(kRegions[0].size == 0u && kRegions[0].access == Access::None && kRegions[0].execute_never, "region 0 is the deny-all background");
static_assert(size_field(kRegions[0]) == 31u, "4 GiB background");
static_assert(size_field(kRegions[5]) == 23u, "16 MiB flash: SIZE 23");
static_assert(size_field(kRegions[6]) == 25u, "64 MiB SDRAM: SIZE 25");
static_assert(rbar_of(kRegions[5]) == (0x60000000u | 0x10u | 5u), "RBAR: base, VALID, number");

}  // namespace o3os::platform::rt10xx::mpu
