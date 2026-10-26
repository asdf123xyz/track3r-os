// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The rt10xx's I2C buses (OS-PORT-RT10XX phases 3 and 4): LPI2C1, 3 and 4 as polled masters behind the MK66's I2cBus interface
// (platform/mk66 i2c_bus.h), so the MK66's chip drivers (the SGTL5000 plan, the TCA8418 scanners, the IS31FL3731 pad LEDs) run on them.
// Each instance's pads are an assumption (i2c_config.h kInstances) and are muxed by its init() only. Implemented in the image only
// (src/i2c.cpp). Main thread only.

#include <o3os/platform/mk66/i2c_bus.h>

#include <cstdint>

namespace o3os::platform::rt10xx::i2c {

using Result = o3os::platform::mk66::I2cResult;
using Bus = o3os::platform::mk66::I2cBus;

// Instance n (1, 3 or 4): the gate, the pads, the master. Unknown n: Result::Nak. Idempotent.
Result init(unsigned n) noexcept;
// The bus of instance n, or nullptr for an instance this chip's table does not have. A bus not yet init()ed answers every transfer Nak.
Bus* bus_of(unsigned n) noexcept;
bool inited(unsigned n) noexcept;

// The codec's bus, LPI2C1 (phase 3's API).
inline Result init() noexcept { return init(1); }
Bus& bus() noexcept;
Result write_reg(std::uint8_t address7, std::uint16_t reg, std::uint16_t value) noexcept;
Result read_reg(std::uint8_t address7, std::uint16_t reg, std::uint16_t& value) noexcept;
std::uint32_t last_status() noexcept;
std::uint32_t transfers() noexcept;

}  // namespace o3os::platform::rt10xx::i2c
