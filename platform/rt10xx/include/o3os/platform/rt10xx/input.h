// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The rt10xx's inputs (OS-PORT-RT10XX phase 4): the MK66's scanner over two TCA8418 key scanners (0x34, one per bus) and the IS31FL3731 pad
// LED driver (0x74, observed on the Tracker+), on two of this chip's I2C buses, and the encoder through a GPIO quadrature decoder.
//
// What is observed and what is assumed. Observed (Modular-Trackular FINDINGS section 20): the pad LED driver is an IS31FL3731 at 0x74,
// one LED per pad in its first four rows, on an LPI2C at 400 kHz, and there are two such buses. ASSUMED: that the keys are scanned by the
// MK66's chips (TCA8418 at 0x34, a pad matrix and a button matrix with the MK66's maps), which buses carry them, and where the encoder
// is. So nothing is fixed at build time: start(pad, control) takes the two LPI2C instances (or finds the pad bus as the one where 0x74
// answers), encoder(port, a, b) takes the encoder's two GPIO lines, and `input` on the console shows raw events to check the maps.
// No INT line is known: the scanner's safety poll drains the chips. The encoder is sampled once a main-loop pass (every millisecond at
// least): `input` shows how many steps that missed. Implemented in the image only (src/input.cpp). Main loop only.

#include <o3os/hal/input.h>
#include <o3os/platform/mk66/input/pad_leds.h>
#include <o3os/platform/mk66/input/scanner.h>

#include <cstdint>

namespace o3os::platform::rt10xx::input {

// The two buses: `pad_bus` carries the pad scanner and the LED driver, `control_bus` the button scanner. 0 for `pad_bus`: the instance
// where 0x74 answers. False: an instance this chip has not, or no 0x74 anywhere.
bool start(unsigned pad_bus, unsigned control_bus) noexcept;
bool started() noexcept;
unsigned pad_bus() noexcept;
unsigned control_bus() noexcept;
// The encoder's A and B lines: GPIO port 1..5, bits 0..31 (the pads must be in their GPIO function: `pins mux`). False: out of range.
bool set_encoder(unsigned port, unsigned bit_a, unsigned bit_b) noexcept;
bool encoder_set() noexcept;
std::uint32_t encoder_skipped() noexcept;
std::uint16_t encoder_count() noexcept;
// Main loop, every pass: the encoder sample, the scan, a piece of the LED flush.
void service() noexcept;
o3os::platform::mk66::input::Scanner& scanner() noexcept;
o3os::platform::mk66::input::PadLeds& leds() noexcept;

}  // namespace o3os::platform::rt10xx::input
