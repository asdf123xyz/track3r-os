// SPDX-License-Identifier: GPL-3.0-or-later
//
// The pinned Riedel library's own "EVE_RVT70" panel preset (a 7" 800x480 module with the same timing as the Tracker's), read for
// the oracle test. Built with -DEVE_RVT70 and without the custom module header (third_party/eve-riedel/CMakeLists.txt); it
// shares no macro with eve_riedel_oracle.c because it is a separate translation unit.
#include "EVE.h"

void o3_riedel_rvt70_panel(uint32_t out[15]) {
  out[0] = EVE_HSIZE;
  out[1] = EVE_HCYCLE;
  out[2] = EVE_HOFFSET;
  out[3] = EVE_HSYNC0;
  out[4] = EVE_HSYNC1;
  out[5] = EVE_VSIZE;
  out[6] = EVE_VCYCLE;
  out[7] = EVE_VOFFSET;
  out[8] = EVE_VSYNC0;
  out[9] = EVE_VSYNC1;
  out[10] = EVE_PCLK;
  out[11] = EVE_PCLKPOL;
  out[12] = EVE_SWIZZLE;
  out[13] = EVE_CSPREAD;
  out[14] = EVE_GEN;
}
