// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fixture of ctest `mk66_noinit_guard`: an initialised object in a noinit section. Must NOT compile: a NOLOAD output section
// would drop the value silently, so the compiler refuses it ("only zero initializers are allowed in section").
#include <o3os/platform/mk66/sections.h>

O3OS_SDRAM_NOINIT int g_initialised = 5;
