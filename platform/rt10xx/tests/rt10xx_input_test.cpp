// SPDX-License-Identifier: GPL-3.0-or-later
//
// The inputs' pure parts (phase 4): the quadrature decoder (quadrature.h) and the I2C instance table with its candidate pads (i2c_config.h).
#include <doctest/doctest.h>

#include <o3os/platform/rt10xx/i2c_config.h>
#include <o3os/platform/rt10xx/quadrature.h>

using o3os::platform::rt10xx::input::Quadrature;
namespace i2c = o3os::platform::rt10xx::i2c_cfg;

namespace {
// The Gray sequence 00 01 11 10, as (a, b).
constexpr bool kA[4] = {false, false, true, true};
constexpr bool kB[4] = {false, true, true, false};
}  // namespace

TEST_CASE("rt10xx quadrature: one detent each way") {
  Quadrature q;
  q.reset(false, false);
  for (unsigned i = 1; i <= 4; ++i) {
    q.sample(kA[i % 4], kB[i % 4]);
  }
  CHECK(q.count() == 4u);
  for (unsigned i = 4; i > 0; --i) {
    q.sample(kA[(i - 1) % 4], kB[(i - 1) % 4]);
  }
  CHECK(q.count() == 0u);
  CHECK(q.skipped() == 0u);
}

TEST_CASE("rt10xx quadrature: wraps, repeats and skips") {
  Quadrature q;
  q.reset(false, false);
  q.sample(true, false);  // 00 -> 10: down
  CHECK(q.count() == 0xFFFFu);
  q.sample(true, false);  // no change
  CHECK(q.count() == 0xFFFFu);
  q.sample(false, true);  // 10 -> 01: both changed, a missed step
  CHECK(q.count() == 0xFFFFu);
  CHECK(q.skipped() == 1u);
  Quadrature fresh;
  fresh.sample(true, true);  // the first sample only primes it
  CHECK(fresh.count() == 0u);
  fresh.sample(true, false);  // 11 -> 10: up
  CHECK(fresh.count() == 1u);
}

TEST_CASE("rt10xx i2c: the instance table") {
  CHECK(i2c::kInstanceCount == 3u);
  CHECK(i2c::has_instance(1));
  CHECK(i2c::has_instance(3));
  CHECK(i2c::has_instance(4));
  CHECK_FALSE(i2c::has_instance(2));
  CHECK(i2c::instance(3)->base == 0x403F8000u);
  CHECK(i2c::instance(4)->base == 0x403FC000u);
  CHECK(i2c::instance(4)->ccgr == 0x400FC080u);  // LPI2C4's gate is in CCGR6
  CHECK(i2c::instance(3)->scl.mux_ctl == 0x401F8118u);  // GPIO_AD_B1_07
  CHECK(i2c::instance(4)->scl.mux_ctl == 0x401F80ECu);  // GPIO_AD_B0_12
  CHECK(i2c::instance(4)->scl.alt == 0u);
  CHECK(i2c::instance(1)->scl_gpio_bit == 16u);  // GPIO_AD_B1_00 = GPIO1_IO16
  CHECK(i2c::instance(3)->sda_gpio_bit == 22u);  // GPIO_AD_B1_06 = GPIO1_IO22
  CHECK(i2c::kScanFirst == 0x08u);
  CHECK(i2c::kScanLast == 0x77u);
}
