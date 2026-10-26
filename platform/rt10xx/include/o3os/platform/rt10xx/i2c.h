// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The codec's control bus of the rt10xx image (OS-PORT-RT10XX phase 3, src/i2c.cpp): LPI2C1 as a polled master behind the MK66's bus
// interface (platform/mk66 i2c_bus.h, chip-neutral), and the register helpers the codec plan uses. Main thread only; every wait is bounded.

#include <o3os/platform/mk66/i2c_bus.h>

#include <cstdint>

namespace o3os::platform::rt10xx::i2c {

using Result = o3os::platform::mk66::I2cResult;
using Bus = o3os::platform::mk66::I2cBus;

// Clock, pads (audio_pins.h: assumed), the master's words. Idempotent.
Result init() noexcept;
Bus& bus() noexcept;
// A 16-bit register of a chip with 16-bit register addresses (the SGTL5000): big-endian on the wire.
Result write_reg(std::uint8_t address7, std::uint16_t reg, std::uint16_t value) noexcept;
Result read_reg(std::uint8_t address7, std::uint16_t reg, std::uint16_t& value) noexcept;
// Diagnostics: the last status word a transfer ended with, and how many transfers were made.
std::uint32_t last_status() noexcept;
std::uint32_t transfers() noexcept;

}  // namespace o3os::platform::rt10xx::i2c
