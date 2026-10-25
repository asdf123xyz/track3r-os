// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Tracker's panel in the pinned Riedel library's own macros, loaded through its EVE_CUSTOM_MODULE_H hook: the same
// numbers as platform/mk66/display/panel.h (kPanel800x480), which the oracle test checks against these. The library's own
// RVT70 preset has this timing too, with CSPREAD 1 (this OS writes 0): eve_riedel_rvt70.c reads that preset for the test.
#ifndef O3OS_TEST_PANEL_H
#define O3OS_TEST_PANEL_H

#define EVE_HSIZE (800L)
#define EVE_VSIZE (480L)
#define EVE_VSYNC0 (0L)
#define EVE_VSYNC1 (10L)
#define EVE_VOFFSET (23L)
#define EVE_VCYCLE (525L)
#define EVE_HSYNC0 (0L)
#define EVE_HSYNC1 (10L)
#define EVE_HOFFSET (46L)
#define EVE_HCYCLE (1056L)
#define EVE_PCLK (2L)
#define EVE_PCLKPOL (1L)
#define EVE_SWIZZLE (0L)
#define EVE_CSPREAD (0L)
#define EVE_GEN 2
#define EVE_ROTATE 0   /* our default orientation: REG_ROTATE 0 */
#define EVE_SOFT_RESET /* the reset command after PD#, as our init sends it */

#endif /* O3OS_TEST_PANEL_H */
