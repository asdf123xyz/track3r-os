// SPDX-License-Identifier: GPL-3.0-or-later
//
// The rt10xx memory map (mem_map.h, OS-PORT-RT10XX): the chip's regions pinned by value (docs/specs/OS-PORT-RT10XX.md section 2), the
// SDRAM table's properties and the DTCM plan. The static_asserts in the header stop a build that breaks the sums; these cases pin the
// numbers so that a deliberate change is a deliberate change to a test too. The model-arena mirror is checked against the model's own
// constant in tests/rt10xx_mem_map_model_test.cpp (a platform test may not include the model: the layer rule).
#include <doctest/doctest.h>

#include <o3os/platform/rt10xx/mem_map.h>

#include <cstdint>
#include <string_view>

namespace mem = o3os::platform::rt10xx::mem;

TEST_CASE("rt10xx mem_map: the chip's regions and the board's facts") {
  CHECK(mem::kFlashBase == 0x60000000u);
  CHECK(mem::kFlashSize == 16u * 1024u * 1024u);
  CHECK(mem::kBootloaderSize == 0x30000u);
  CHECK(mem::kImageBase == 0x60030000u);
  CHECK(mem::kImageMaxEnd == 0x60800000u);
  CHECK(mem::kImageMaxSize == 0x7D0000u);
  CHECK(mem::kVectorTableBytes == 704u);
  CHECK(mem::kItcmBase == 0u);
  CHECK(mem::kItcmSize == 131072u);
  CHECK(mem::kDtcmBase == 0x20000000u);
  CHECK(mem::kDtcmSize == 360448u);
  CHECK(mem::kStackTop == 0x20058000u);
  CHECK(mem::kOcramBase == 0x20200000u);
  CHECK(mem::kOcramSize == 32768u);
  CHECK(mem::kOcramWindowEnd == 0x20280000u);
  CHECK(mem::kSdramBase == 0x80000000u);
  CHECK(mem::kSdramSize == 64u * 1024u * 1024u);
  CHECK(mem::kFlashSectorSize == 4096u);
  CHECK(mem::kFlashPageSize == 256u);
}

TEST_CASE("rt10xx mem_map: the ITCM is hot code, tap data and the module slab") {
  CHECK(mem::kModuleSlabSize == 32768u);
  CHECK(mem::kRamfuncSize == 98304u);
  CHECK(mem::kModuleSlabBase == 98304u);
  CHECK(mem::kItcmHotBudget + mem::kItcmDataBudget <= mem::kRamfuncSize);
}

TEST_CASE("rt10xx mem_map: SDRAM table, region by region") {
  struct Row {
    const char* name;
    std::uint32_t kib;
  };
  const Row table[] = {{"sample_arena", 59136}, {"delay", 768},  {"reverb", 256},   {"module_region", 768},
                       {"project", 1920},       {"undo", 384},   {"file_job", 512}, {"lut", 192},
                       {"ui_cache", 512},       {"diag_log", 64}, {"slack", 1024}};
  CHECK(mem::kSdramRegionCount == sizeof(table) / sizeof(table[0]));
  if (mem::kSdramRegionCount != sizeof(table) / sizeof(table[0])) {
    return;
  }
  std::uint32_t expect_base = mem::kSdramBase;
  std::uint64_t total_kib = 0;
  for (unsigned i = 0; i < mem::kSdramRegionCount; ++i) {
    const mem::Region& r = mem::kSdramRegions[i];
    INFO("region " << table[i].name);
    CHECK(std::string_view(r.name) == table[i].name);
    CHECK(r.size == table[i].kib * 1024u);
    CHECK(r.base == expect_base);
    CHECK(r.base % 32u == 0u);
    CHECK(r.size % 32u == 0u);
    expect_base = mem::end_of(r);
    total_kib += table[i].kib;
  }
  CHECK(total_kib == 65536u);
  CHECK(expect_base == mem::kSdramBase + mem::kSdramSize);
  CHECK(mem::sdram_regions_contiguous());
  CHECK(mem::sdram_regions_aligned(32u));
  CHECK(mem::sdram_regions_total() == mem::kSdramSize);
}

TEST_CASE("rt10xx mem_map: in_ram and in_image_flash") {
  CHECK(mem::in_ram(0x20000000u, 32));
  CHECK(mem::in_ram(0x20057FE0u, 32));
  CHECK_FALSE(mem::in_ram(0x20057FE4u, 32));  // crosses the stack top
  CHECK_FALSE(mem::in_ram(0x20058000u, 4));
  CHECK(mem::in_ram(0x20200000u, 32));
  CHECK_FALSE(mem::in_ram(0x20208000u, 4));  // the OCRAM beyond its 32 KiB
  CHECK_FALSE(mem::in_ram(0x20280000u, 4));  // no RAM behind the window (FINDINGS 11)
  CHECK(mem::in_ram(0x00000000u, 32));
  CHECK_FALSE(mem::in_ram(0x00020000u, 4));
  CHECK_FALSE(mem::in_ram(0x70000000u, 4));
  CHECK_FALSE(mem::in_ram(0x60030000u, 4));
  CHECK(mem::in_image_flash(0x60030000u));
  CHECK(mem::in_image_flash(0x607FFFFFu));
  CHECK_FALSE(mem::in_image_flash(0x6002FFFFu));
  CHECK_FALSE(mem::in_image_flash(0x60800000u));
}

TEST_CASE("rt10xx mem_map: the DTCM plan leaves its floor") {
  const std::uint32_t sum = mem::budget_sum(mem::kDtcmBudget);
  CHECK(sum + mem::kDtcmMinFree <= mem::kDtcmSize);
  CHECK(mem::kDtcmMinFree == 65536u);
  CHECK(mem::kStackSize == 32768u);
  CHECK(mem::kDmaRingAlign == 1024u);
  CHECK(mem::kCrashBlockSize == 128u);
  CHECK(mem::kFaultStackSize == 512u);
}
