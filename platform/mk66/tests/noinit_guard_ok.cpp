// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fixture of ctest `mk66_noinit_guard`: objects the noinit macros accept. Must compile.
#include <o3os/platform/mk66/sections.h>

namespace {
struct Block {
  int a;
  int b[3];
};
}  // namespace

O3OS_SDRAM_NOINIT Block g_in_sdram;
O3OS_SDRAM_NOINIT int g_word_in_sdram;
O3OS_NOINIT Block g_in_sram;
O3OS_NOINIT int g_word_in_sram;
