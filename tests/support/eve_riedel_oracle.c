// SPDX-License-Identifier: GPL-3.0-or-later
//
// C side of the Riedel oracle (host tests only): includes the pinned library's header, which defines hundreds of unprefixed
// macros (REG_ID, RAM_G, BEGIN, ...) that must never meet the o3os headers, so the test talks to it only through these
// functions. Built with EVE_commands.c into the `eve_riedel_oracle` library (third_party/eve-riedel/CMakeLists.txt).
#include "EVE.h"

#include <string.h>

typedef struct {
  const char* name;
  uint32_t value;
} o3_const;

static const o3_const kTable[] = {
    {"RAM_G", EVE_RAM_G},
    {"RAM_DL", EVE_RAM_DL},
    {"RAM_REG", EVE_RAM_REG},
    {"RAM_CMD", EVE_RAM_CMD},
    {"RAM_G_SIZE", EVE_RAM_G_SIZE},
    {"RAM_DL_SIZE", EVE_RAM_DL_SIZE},
    {"CMDFIFO_SIZE", EVE_CMDFIFO_SIZE},
    {"REG_ID", REG_ID},
    {"REG_FRAMES", REG_FRAMES},
    {"REG_CLOCK", REG_CLOCK},
    {"REG_CPURESET", REG_CPURESET},
    {"REG_HCYCLE", REG_HCYCLE},
    {"REG_HOFFSET", REG_HOFFSET},
    {"REG_HSIZE", REG_HSIZE},
    {"REG_HSYNC0", REG_HSYNC0},
    {"REG_HSYNC1", REG_HSYNC1},
    {"REG_VCYCLE", REG_VCYCLE},
    {"REG_VOFFSET", REG_VOFFSET},
    {"REG_VSIZE", REG_VSIZE},
    {"REG_VSYNC0", REG_VSYNC0},
    {"REG_VSYNC1", REG_VSYNC1},
    {"REG_DLSWAP", REG_DLSWAP},
    {"REG_ROTATE", REG_ROTATE},
    {"REG_OUTBITS", REG_OUTBITS},
    {"REG_DITHER", REG_DITHER},
    {"REG_SWIZZLE", REG_SWIZZLE},
    {"REG_CSPREAD", REG_CSPREAD},
    {"REG_PCLK_POL", REG_PCLK_POL},
    {"REG_PCLK", REG_PCLK},
    {"REG_VOL_PB", REG_VOL_PB},
    {"REG_VOL_SOUND", REG_VOL_SOUND},
    {"REG_SOUND", REG_SOUND},
    {"REG_GPIOX_DIR", REG_GPIOX_DIR},
    {"REG_GPIOX", REG_GPIOX},
    {"REG_PWM_HZ", REG_PWM_HZ},
    {"REG_PWM_DUTY", REG_PWM_DUTY},
    {"REG_GPIO", REG_GPIO},
    {"REG_TOUCH_MODE", REG_TOUCH_MODE},
    {"REG_TOUCH_RZTHRESH", REG_TOUCH_RZTHRESH},
    {"REG_CMD_READ", REG_CMD_READ},
    {"REG_CMD_WRITE", REG_CMD_WRITE},
    {"REG_CMD_DL", REG_CMD_DL},
    {"REG_CMDB_SPACE", REG_CMDB_SPACE},
    {"REG_CMDB_WRITE", REG_CMDB_WRITE},
    {"HOST_ACTIVE", EVE_ACTIVE},
    {"HOST_CLKEXT", EVE_CLKEXT},
    {"HOST_CLKINT", EVE_CLKINT},
    {"HOST_RST_PULSE", EVE_RST_PULSE},
    {"DLSWAP_FRAME", EVE_DLSWAP_FRAME},
    {"DL_DISPLAY", DL_DISPLAY},
    {"DL_CLEAR_COLOR_RGB", DL_CLEAR_COLOR_RGB},
    {"DL_CLEAR_ALL", DL_CLEAR | CLR_COL | CLR_STN | CLR_TAG},
    {"MUTE", EVE_MUTE},
    {"CMD_SNAPSHOT2", CMD_SNAPSHOT2},
    {"RGB565", EVE_RGB565},
};

int o3_riedel_lookup(const char* name, uint32_t* out) {
  for (unsigned i = 0; i < sizeof(kTable) / sizeof(kTable[0]); ++i) {
    if (strcmp(kTable[i].name, name) == 0) {
      *out = kTable[i].value;
      return 1;
    }
  }
  return 0;
}

unsigned o3_riedel_constant_count(void) { return (unsigned)(sizeof(kTable) / sizeof(kTable[0])); }

// The panel as the library holds it for the custom module header (o3_panel.h).
void o3_riedel_panel(uint32_t out[15]) {
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

uint8_t o3_riedel_init(void) { return EVE_init(); }
uint8_t o3_riedel_busy(void) { return EVE_busy(); }
uint8_t o3_riedel_take_fault(void) { return EVE_get_and_reset_fault_state(); }

// One command word through the library's burst path: chip select, the REG_CMDB_WRITE window header, the word, release.
void o3_riedel_burst_word(uint32_t word) {
  EVE_start_cmd_burst();
  EVE_cmd_dl_burst(word);
  EVE_end_cmd_burst();
}
