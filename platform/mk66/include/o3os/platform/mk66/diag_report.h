// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// `diag mem` for the MK66 (OS-005a): the memory map and the SRAM budgets of mem_map.h turned into the pure report the audio
// library's formatter prints (audio/diag.h). Nothing here reads hardware: the figures only the linked image knows (the bytes
// of hot code and of tap data in SRAM_L, the bytes of SRAM_U the image uses) are arguments, taken from the linker symbols by the console binding
// once there is a console (the CDC `diag`, OS-006b/007). The tests print the real table, so a change to the map shows in the
// formatter's golden text.

#include <o3os/audio/diag.h>
#include <o3os/platform/mk66/mem_map.h>

#include <cstdint>

namespace o3os::platform::mk66 {

// The rows live here; `report` points into this object, so it must not be copied.
struct MemTables {
  audio::diag::MemRow sdram[mem::kSdramRegionCount];
  audio::diag::BudgetRow sram_l[mem::kSramLBudgetCount];
  audio::diag::BudgetRow sram_u[mem::kSramUBudgetCount];
  audio::diag::MemReport report;
};

inline void fill_mem_tables(MemTables& t, std::uint32_t hot_code_bytes, std::uint32_t sram_u_image_bytes, std::uint32_t tap_data_bytes = 0u) noexcept {
  for (unsigned i = 0; i < mem::kSdramRegionCount; ++i) {
    t.sdram[i] = audio::diag::MemRow{mem::kSdramRegions[i].name, mem::kSdramRegions[i].base, mem::kSdramRegions[i].size};
  }
  for (unsigned i = 0; i < mem::kSramLBudgetCount; ++i) {
    t.sram_l[i] = audio::diag::BudgetRow{mem::kSramLBudget[i].name, mem::kSramLBudget[i].bytes};
  }
  for (unsigned i = 0; i < mem::kSramUBudgetCount; ++i) {
    t.sram_u[i] = audio::diag::BudgetRow{mem::kSramUBudget[i].name, mem::kSramUBudget[i].bytes};
  }
  t.report.sdram = t.sdram;
  t.report.sdram_count = mem::kSdramRegionCount;
  t.report.sdram_total = mem::kSdramSize;
  t.report.sram_l = t.sram_l;
  t.report.sram_l_count = mem::kSramLBudgetCount;
  t.report.sram_l_size = mem::kSramLSize;
  t.report.sram_l_hot_used = hot_code_bytes;
  t.report.sram_l_hot_budget = mem::kSramLHotBudget;
  t.report.sram_l_data_used = tap_data_bytes;
  t.report.sram_l_data_budget = mem::kSramLDataBudget;
  t.report.sram_u = t.sram_u;
  t.report.sram_u_count = mem::kSramUBudgetCount;
  t.report.sram_u_size = mem::kSramUSize;
  t.report.sram_u_used = sram_u_image_bytes;
  t.report.sram_u_stack = mem::kStackSize;
  t.report.sram_u_min_free = mem::kSramUMinFree;
}

}  // namespace o3os::platform::mk66
