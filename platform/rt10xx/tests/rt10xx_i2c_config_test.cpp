// SPDX-License-Identifier: GPL-3.0-or-later
//
// The codec's control bus (i2c_config.h): the LPI2C1 registers, the timing word decoded and the SCL rate it makes, the command words.
#include <doctest/doctest.h>

#include <o3os/platform/rt10xx/i2c_config.h>

namespace i2c = o3os::platform::rt10xx::i2c_cfg;

namespace {
std::uint32_t field(std::uint32_t v, unsigned shift, unsigned bits) { return (v >> shift) & ((1u << bits) - 1u); }
}  // namespace

TEST_CASE("rt10xx i2c: the registers") {
  CHECK(i2c::kLpi2c1 == 0x403F0000u);
  CHECK(i2c::kMcr == 0x403F0010u);
  CHECK(i2c::kMsr == 0x403F0014u);
  CHECK(i2c::kMccr0 == 0x403F0048u);
  CHECK(i2c::kMtdr == 0x403F0060u);
  CHECK(i2c::kMrdr == 0x403F0070u);
  CHECK(i2c::kCscdr2 == 0x400FC038u);
}

TEST_CASE("rt10xx i2c: the timing word and the SCL rate") {
  CHECK(field(i2c::kMccr0Word, 0, 6) == 31u);   // CLKLO 32 cycles
  CHECK(field(i2c::kMccr0Word, 8, 6) == 23u);   // CLKHI 24
  CHECK(field(i2c::kMccr0Word, 16, 6) == 14u);  // SETHOLD 15
  CHECK(field(i2c::kMccr0Word, 24, 6) == 6u);   // DATAVD 7
  CHECK(i2c::scl_hz_before_rise() == 24'000'000u / 58u);
  CHECK(i2c::scl_hz_before_rise() <= 420'000u);
  CHECK(i2c::scl_hz_before_rise() >= 350'000u);
  CHECK(i2c::kMcfgr1Word == 0u);                  // prescaler 1, two-pin open drain
  CHECK(field(i2c::kMcfgr2Word, 16, 4) == 2u);    // SCL glitch filter
  CHECK(field(i2c::kMcfgr2Word, 24, 4) == 2u);    // SDA glitch filter
  CHECK(field(i2c::kMcfgr2Word, 0, 12) == 0u);    // no bus-idle timeout
}

TEST_CASE("rt10xx i2c: the command words and the flags") {
  CHECK(i2c::kCmdStart == (4u << 8));
  CHECK(i2c::kCmdStop == (2u << 8));
  CHECK(i2c::kCmdReceive == (1u << 8));
  CHECK(i2c::kCmdTransmit == 0u);
  CHECK((i2c::kMsrErrors & i2c::kMsrNackDetect) != 0u);
  CHECK((i2c::kMsrErrors & i2c::kMsrPinLowTimeout) != 0u);
  CHECK((i2c::kMsrClearAll & i2c::kMsrTxReady) == 0u);  // the FIFO flags are not write-to-clear
  CHECK((i2c::kMsrClearAll & i2c::kMsrRxReady) == 0u);
  CHECK(i2c::kMrdrEmpty == (1u << 14));
}
