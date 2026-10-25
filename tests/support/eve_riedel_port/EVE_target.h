// SPDX-License-Identifier: GPL-3.0-or-later
//
// The "target" of the pinned Riedel library (third_party/eve-riedel, MIT, tag 5.0.9) for the host tests: the library calls
// these pin, delay and SPI hooks, and here they go to the FT812 model of the display tests (tests/oracle_port in
// eve_riedel_test.cpp binds the model). The library's own target headers (one per MCU) are not vendored; this file is the
// interface its EVE_target_template.h describes, written for the test: no DMA, one byte at a time.
#ifndef O3OS_TEST_EVE_TARGET_H
#define O3OS_TEST_EVE_TARGET_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
void o3_port_pdn(int asserted);          // PD# driven low = 1
void o3_port_cs(int asserted);           // chip select driven low = 1
void o3_port_delay_ms(unsigned ms);
uint8_t o3_port_xfer(uint8_t out);       // one SPI byte, full duplex
#ifdef __cplusplus
}
#endif

static inline void DELAY_MS(uint16_t val) { o3_port_delay_ms(val); }
static inline void EVE_pdn_set(void) { o3_port_pdn(1); }
static inline void EVE_pdn_clear(void) { o3_port_pdn(0); }
static inline void EVE_cs_set(void) { o3_port_cs(1); }
static inline void EVE_cs_clear(void) { o3_port_cs(0); }
static inline void spi_transmit(uint8_t data) { (void)o3_port_xfer(data); }
static inline void spi_transmit_32(uint32_t data) {
  spi_transmit((uint8_t)(data & 0xFFu));
  spi_transmit((uint8_t)((data >> 8) & 0xFFu));
  spi_transmit((uint8_t)((data >> 16) & 0xFFu));
  spi_transmit((uint8_t)((data >> 24) & 0xFFu));
}
/* spi_transmit_burst() is only used for command-FIFO words: always four bytes */
static inline void spi_transmit_burst(uint32_t data) { spi_transmit_32(data); }
static inline uint8_t spi_receive(uint8_t data) { return o3_port_xfer(data); }
static inline uint8_t fetch_flash_byte(const uint8_t* p_data) { return *p_data; }

#endif /* O3OS_TEST_EVE_TARGET_H */
