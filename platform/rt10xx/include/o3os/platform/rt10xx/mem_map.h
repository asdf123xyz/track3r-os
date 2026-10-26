// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The memory map of the Tracker+ (NXP i.MX RT10xx, Cortex-M7): the single source of every region address and size of the
// rt10xx port (OS-PORT-RT10XX, docs/specs/OS-PORT-RT10XX.md). `rt10xx.ld` is generated from the constants below by
// platform/mk66/tools/gen_ld.py (the generator is chip-neutral: it fills @kName@ placeholders from `inline constexpr std::uint32_t`
// statements), and platform/rt10xx/tools/check_ld.py verifies the result against this header, so the linker script cannot drift.
//
// What keeps the generator trivial: every linker constant is an `inline constexpr std::uint32_t kName = <expression>;` whose
// expression is made of integer literals, earlier constants and + - * / % << >> & | ^ ( ) only. The rest of the file (the region
// tables, the functions, the static_asserts) is for the compiler and the host tests.
//
// Evidence (docs/specs/OS-PORT-RT10XX.md section 2 has the table with its labels):
//   * the chip's fixed addresses (ITCM 0x0000_0000, DTCM 0x2000_0000, OCRAM 0x2020_0000, FlexSPI 0x6000_0000, SEMC 0x8000_0000)
//     are the i.MX RT1050 Reference Manual's system memory map (chapter "Memory Map");
//   * the FlexRAM split (128 KiB ITCM, 352 KiB DTCM, 32 KiB OCRAM), the stack top 0x2005_8000, the flash of 16 MiB, the image
//     base 0x6003_0000 below which the bootloader lives, the 64 MiB of SDRAM and the fact that no RAM answers at 0x2028_0000 are
//     observations of the unit and of its official firmware, recorded by the Modular-Trackular project (FINDINGS sections 2, 8, 11,
//     15; MIT). They describe the board, not the official firmware's code, and are used here as facts of the hardware;
//   * the largest image installed through the bootloader so far ended 8 MiB into the flash (FINDINGS section 8). Larger ones are
//     untested, so the image region ends there (kImageMaxEnd), not at the end of the flash.
// Everything else below (the split of the TCMs, the SDRAM table) is this port's own choice, scaled from ADR 0013 D7.

#include <cstdint>

namespace o3os::platform::rt10xx::mem {

inline constexpr std::uint32_t kKiB = 1024u;
inline constexpr std::uint32_t kMiB = 1024u * 1024u;

// ---- Flash: the FlexSPI NOR, 16 MiB, memory-mapped at 0x6000_0000 ------------------------------------------------------------------
inline constexpr std::uint32_t kFlashBase = 0x60000000u;
inline constexpr std::uint32_t kFlashSize = 16u * kMiB;
// The first 192 KiB hold the FlexSPI configuration block, the boot data and the bootloader (the one that reads `Firmware/` from
// the card): not ours, never in an image, never written. A `.ptf` covers nothing below kImageBase (FINDINGS section 1).
inline constexpr std::uint32_t kBootloaderSize = 0x30000u;
inline constexpr std::uint32_t kImageBase = kFlashBase + kBootloaderSize;
// The image region ends where the largest installed image ended (8 MiB into the flash): what the bootloader is known to write.
inline constexpr std::uint32_t kImageMaxEnd = kFlashBase + 8u * kMiB;
inline constexpr std::uint32_t kImageMaxSize = kImageMaxEnd - kImageBase;
// The NOR's erase sector and program page, and the end rule of the bootloader (FINDINGS section 26: an image whose last byte lies in
// the first page of a new sector is programmed without an erase, so the tools pad such an image up to kPagePadBytes into the second
// page; boot_image.h has the rule as a function, make_ptf.py applies it).
inline constexpr std::uint32_t kFlashSectorSize = 4096u;
inline constexpr std::uint32_t kFlashPageSize = 256u;
inline constexpr std::uint32_t kPagePadBytes = 16u;
// Cortex-M7 exception table (16 system vectors) plus the RT10xx's 160 device interrupts, one word each, at kImageBase (VTOR).
inline constexpr std::uint32_t kVectorTableBytes = (16u + 160u) * 4u;

// ---- FlexRAM: ITCM, DTCM, OCRAM (flexram.h has the bank configuration word) ----------------------------------------------------
inline constexpr std::uint32_t kItcmBase = 0x00000000u;
inline constexpr std::uint32_t kItcmSize = 128u * kKiB;
inline constexpr std::uint32_t kDtcmBase = 0x20000000u;
inline constexpr std::uint32_t kDtcmSize = 352u * kKiB;
inline constexpr std::uint32_t kOcramBase = 0x20200000u;
inline constexpr std::uint32_t kOcramSize = 32u * kKiB;
// The rest of the OCRAM window (up to 0x2028_0000 on an RT106x) has no RAM behind it on this unit: an access faults (FINDINGS 11).
inline constexpr std::uint32_t kOcramWindowEnd = 0x20280000u;

// ---- ITCM: hot code and the module slab (the SRAM_L of ADR 0013 D7, four times the size) --------------------------------------
// `.ramfunc_l` keeps its MK66 name (audio/CMakeLists.txt: the audio layer's O3OS_AUDIO_HOT names that section), linked into the
// ITCM and copied from flash by the reset sequence; `.tapmem_l` (the audio taps' data) sits behind it. The top of the ITCM is the
// module slab of ADR 0002 / OS-005i, outside the linker region so nothing else can land there.
inline constexpr std::uint32_t kModuleSlabSize = 32u * kKiB;
inline constexpr std::uint32_t kRamfuncSize = kItcmSize - kModuleSlabSize;
inline constexpr std::uint32_t kModuleSlabBase = kItcmBase + kRamfuncSize;
inline constexpr std::uint32_t kItcmHotBudget = 72u * kKiB;   // the MK66's 23,468 B of hot code with room to grow; asserted by the linker
inline constexpr std::uint32_t kItcmDataBudget = 24u * kKiB;  // `.tapmem_l`: TapMem is 22,272 B on the MK66

// ---- DTCM: data, bss, noinit, stacks (the SRAM_U of ADR 0013 D7, 352 KiB instead of 192) ------------------------------------------
// Low to high: `.noinit` (the crash record first), the fault stack, the audio DMA rings, `.data`, `.bss`, free, the main stack growing
// down from the top. One stack serves thread mode and every ISR (MSP only, no RTOS).
inline constexpr std::uint32_t kStackSize = 32u * kKiB;
inline constexpr std::uint32_t kStackTop = kDtcmBase + kDtcmSize;
inline constexpr std::uint32_t kFaultStackSize = 512u;
inline constexpr std::uint32_t kCrashBlockSize = 128u;
// The audio DMA rings' alignment (the eDMA's address modulo wraps a ring on a multiple of its own size, hal/audio.h).
inline constexpr std::uint32_t kDmaRingAlign = 1024u;
inline constexpr std::uint32_t kDtcmMinFree = 64u * kKiB;

// ---- SDRAM: 64 MiB through the SEMC, the ADR 0013 D7 table scaled (sizes in KiB) -----------------------------------------------
// Every region starts where the previous one ends. The fixed rows keep their MK66 sizes (they are sized by the engine, not by the
// part); the sample arena takes everything the fixed rows and a 1 MiB slack leave: 59,136 KiB, against the MK66's 10,816.
inline constexpr std::uint32_t kSdramBase = 0x80000000u;
inline constexpr std::uint32_t kSdramSize = 64u * kMiB;
inline constexpr std::uint32_t kSampleArenaBase = kSdramBase;
inline constexpr std::uint32_t kSampleArenaSize = 59136u * kKiB;
inline constexpr std::uint32_t kDelayBase = kSampleArenaBase + kSampleArenaSize;
inline constexpr std::uint32_t kDelaySize = 768u * kKiB;
inline constexpr std::uint32_t kReverbBase = kDelayBase + kDelaySize;
inline constexpr std::uint32_t kReverbSize = 256u * kKiB;
// OS-SENDFX mirrors (platform/mk66 mem_map.h has the same two): what the send effects take of their regions.
inline constexpr std::uint32_t kDelayRingBytes = 705872u;
inline constexpr std::uint32_t kReverbMemBytes = 53264u;
inline constexpr std::uint32_t kModuleRegionBase = kReverbBase + kReverbSize;
inline constexpr std::uint32_t kModuleRegionSize = 768u * kKiB;
inline constexpr std::uint32_t kProjectBase = kModuleRegionBase + kModuleRegionSize;
inline constexpr std::uint32_t kProjectSize = 1920u * kKiB;
inline constexpr std::uint32_t kModelArenaWorstCaseBytes = 1933504u;  // mirrors o3os::model::kModelArenaBytes (tests/rt10xx_mem_map_test.cpp checks it)
inline constexpr std::uint32_t kUndoBase = kProjectBase + kProjectSize;
inline constexpr std::uint32_t kUndoSize = 384u * kKiB;
inline constexpr std::uint32_t kFileJobBase = kUndoBase + kUndoSize;
inline constexpr std::uint32_t kFileJobSize = 512u * kKiB;
inline constexpr std::uint32_t kLutBase = kFileJobBase + kFileJobSize;
inline constexpr std::uint32_t kLutSize = 192u * kKiB;
inline constexpr std::uint32_t kUiCacheBase = kLutBase + kLutSize;
inline constexpr std::uint32_t kUiCacheSize = 512u * kKiB;
inline constexpr std::uint32_t kDiagLogBase = kUiCacheBase + kUiCacheSize;
inline constexpr std::uint32_t kDiagLogSize = 64u * kKiB;
// The slack is the linker's `.noinit_sdram` window (O3OS_SDRAM_NOINIT in sections.h), 1 MiB on a 64 MiB part.
inline constexpr std::uint32_t kSdramSlackBase = kDiagLogBase + kDiagLogSize;
inline constexpr std::uint32_t kSdramSlackSize = 1024u * kKiB;

// ---- For the compiler and the tests ------------------------------------------------------------------------------------------
struct Region {
  const char* name;
  std::uint32_t base;
  std::uint32_t size;
};

constexpr std::uint32_t end_of(const Region& r) noexcept { return r.base + r.size; }

inline constexpr Region kSdramRegions[] = {
    {"sample_arena", kSampleArenaBase, kSampleArenaSize},
    {"delay", kDelayBase, kDelaySize},
    {"reverb", kReverbBase, kReverbSize},
    {"module_region", kModuleRegionBase, kModuleRegionSize},
    {"project", kProjectBase, kProjectSize},
    {"undo", kUndoBase, kUndoSize},
    {"file_job", kFileJobBase, kFileJobSize},
    {"lut", kLutBase, kLutSize},
    {"ui_cache", kUiCacheBase, kUiCacheSize},
    {"diag_log", kDiagLogBase, kDiagLogSize},
    {"slack", kSdramSlackBase, kSdramSlackSize},
};
inline constexpr unsigned kSdramRegionCount = sizeof(kSdramRegions) / sizeof(kSdramRegions[0]);

constexpr bool sdram_regions_contiguous() noexcept {
  std::uint32_t expect = kSdramBase;
  for (unsigned i = 0; i < kSdramRegionCount; ++i) {
    if (kSdramRegions[i].base != expect || kSdramRegions[i].size == 0u) {
      return false;
    }
    expect = end_of(kSdramRegions[i]);
  }
  return true;
}
constexpr bool sdram_regions_aligned(std::uint32_t alignment) noexcept {
  for (unsigned i = 0; i < kSdramRegionCount; ++i) {
    if (kSdramRegions[i].base % alignment != 0u || kSdramRegions[i].size % alignment != 0u) {
      return false;
    }
  }
  return true;
}
constexpr std::uint64_t sdram_regions_total() noexcept {
  std::uint64_t total = 0;
  for (unsigned i = 0; i < kSdramRegionCount; ++i) {
    total += kSdramRegions[i].size;
  }
  return total;
}

// The RAMs that exist, for a fault handler that must not trust an exception frame pointer blindly: a word-sized access at
// [address, address + length) lies wholly inside the DTCM, the OCRAM (the 32 KiB that answer) or the ITCM.
constexpr bool in_ram(std::uint32_t address, std::uint32_t length) noexcept {
  const std::uint64_t first = address;
  const std::uint64_t last = first + length;  // one past the end
  const bool dtcm = first >= kDtcmBase && last <= static_cast<std::uint64_t>(kDtcmBase) + kDtcmSize;
  const bool ocram = first >= kOcramBase && last <= static_cast<std::uint64_t>(kOcramBase) + kOcramSize;
  const bool itcm = first >= kItcmBase && last <= static_cast<std::uint64_t>(kItcmBase) + kItcmSize && length > 0u;
  return dtcm || ocram || itcm;
}
// A flash address inside the image region (where code and constants are).
constexpr bool in_image_flash(std::uint32_t address) noexcept { return address >= kImageBase && address < kImageMaxEnd; }

// ---- The DTCM plan, row by row (ADR 0013 D7's SRAM_U rows, on the larger part) --------------------------------------------------
// Estimates carried over from the MK66's measured image (its mem_map.h has the history of each row), so that the port's `diag mem`
// prints the same rows beside the real use. The display's DMA staging and the USB buffers are not here: on this chip they go to the
// OCRAM (sections.h O3OS_OCRAM_NOINIT), where a bus master reaches them whatever the TCM backdoor does (an open hardware check).
struct Budget {
  const char* name;
  std::uint32_t bytes;
};
inline constexpr Budget kDtcmBudget[] = {
    {"audio_io_rings", 2048u},
    {"block_buffers", 14u * kKiB},
    {"voice_state", 28u * kKiB + 64u},
    {"module_hot_state", 16u * kKiB},
    {"sequencer", 8u * kKiB + 64u + 640u},
    {"rings", 24u * kKiB},               // the MIDI link alone is 21.7 KB on the MK66
    {"sd_staging", 8u * kKiB},
    {"ui_app_bss", 48u * kKiB},          // the MK66 moved the App and the loaders to SDRAM for room; here they may come back to the DTCM
    {"stack", kStackSize},
    {"crash_block_fault_stack", kCrashBlockSize + kFaultStackSize},
};
inline constexpr unsigned kDtcmBudgetCount = sizeof(kDtcmBudget) / sizeof(kDtcmBudget[0]);

template <unsigned N>
constexpr std::uint32_t budget_sum(const Budget (&rows)[N]) noexcept {
  std::uint32_t total = 0;
  for (unsigned i = 0; i < N; ++i) {
    total += rows[i].bytes;
  }
  return total;
}

static_assert(kImageBase == 0x60030000u, "the image base the bootloader programs and the official firmware's VTOR (FINDINGS 1, 2)");
static_assert(kImageBase % kFlashSectorSize == 0u, "the image starts on a sector");
static_assert(kImageMaxEnd <= kFlashBase + kFlashSize, "the image region is inside the 16 MiB flash");
static_assert(kVectorTableBytes == 704u, "16 system vectors + 160 device interrupts (irq_list.h)");
static_assert(kItcmSize + kDtcmSize + kOcramSize == 512u * kKiB, "FlexRAM: 512 KiB in sixteen 32 KiB banks (flexram.h decodes the split)");
static_assert(kStackTop == 0x20058000u, "the official firmware's stack top (FINDINGS 2): the end of the 352 KiB DTCM");
static_assert(kOcramBase + kOcramSize <= kOcramWindowEnd, "the OCRAM that exists is below the window's end");
static_assert(kRamfuncSize + kModuleSlabSize == kItcmSize, "the ITCM is hot code plus the module slab");
static_assert(kModuleSlabBase + kModuleSlabSize == kItcmBase + kItcmSize, "the module slab is the top of the ITCM");
static_assert(kItcmHotBudget + kItcmDataBudget <= kRamfuncSize, "the hot code and the tap data both fit below the module slab");
static_assert(kStackSize + kFaultStackSize + kCrashBlockSize < kDtcmSize / 2u, "stacks and the crash block are a minor part of the DTCM");
static_assert(kCrashBlockSize % 8u == 0u && kFaultStackSize % 8u == 0u && kStackSize % 8u == 0u, "AAPCS: 8-byte aligned stacks");
static_assert(kDmaRingAlign != 0u && (kDmaRingAlign & (kDmaRingAlign - 1u)) == 0u, "a power of two: the eDMA modulo");

static_assert(kSdramRegionCount == 11u, "ADR 0013 D7's ten regions and the slack");
static_assert(sdram_regions_contiguous(), "SDRAM regions follow one another from the base of SDRAM, none empty");
static_assert(sdram_regions_aligned(32u), "every SDRAM region base and size is 32-byte aligned (the cache line of the Cortex-M7 and the SEMC's bursts)");
static_assert(sdram_regions_total() == kSdramSize, "the regions and the slack sum to exactly 64 MiB");
static_assert(end_of(kSdramRegions[kSdramRegionCount - 1]) == kSdramBase + kSdramSize, "the slack ends at the end of SDRAM");
static_assert(kDelayRingBytes <= kDelaySize && kReverbMemBytes <= kReverbSize, "the send effects (OS-SENDFX) fit their regions");
static_assert(kProjectSize >= kModelArenaWorstCaseBytes, "the project region must hold the model's worst-case arena (model limits.h)");
static_assert(kSampleArenaSize > 5u * 10816u * kKiB, "the sample arena is more than five times the MK66's");

static_assert(budget_sum(kDtcmBudget) + kDtcmMinFree <= kDtcmSize, "the DTCM rows leave at least 64 KiB free");

}  // namespace o3os::platform::rt10xx::mem
