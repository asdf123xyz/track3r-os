// SPDX-License-Identifier: GPL-3.0-or-later
// OURS, not pt2-clone's: the three constants the vendored Paula files take from the original's pt2_header.h (which includes the SDL and GUI world).
#pragma once
#include <stdint.h>
#include <stdbool.h>
#define PI 3.14159265358979323846264338327950288
#define AMIGA_PAL_XTAL_HZ 28375160.0
#define AMIGA_PAL_CCK_HZ (AMIGA_PAL_XTAL_HZ / 8.0)
#define ASSERT(x)
