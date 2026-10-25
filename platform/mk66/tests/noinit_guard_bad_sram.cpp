// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fixture of ctest `mk66_noinit_guard`: the same for the SRAM variant, with an aggregate initialiser.
#include <o3os/platform/mk66/sections.h>

struct Block {
  int a;
  int b[3];
};

O3OS_NOINIT Block g_initialised = {1, {2, 3, 4}};
