// SPDX-License-Identifier: GPL-3.0-or-later
//
// The rt10xx's inputs (OS-PORT-RT10XX phase 4, input.h): the MK66's Scanner and PadLeds (chip-neutral, over I2cBus) on two LPI2C instances
// chosen at run time, and the encoder through the quadrature decoder of quadrature.h. A RoutedBus forwards to the instance start() chose, so
// the scanner and the LED driver are static objects built once.
#include <o3os/platform/rt10xx/input.h>

#include "internal.h"
#include "regs.h"

#include <o3os/platform/mk66/input/pins.h>
#include <o3os/platform/rt10xx/i2c.h>
#include <o3os/platform/rt10xx/i2c_config.h>
#include <o3os/platform/rt10xx/platform.h>
#include <o3os/platform/rt10xx/quadrature.h>

namespace o3os::platform::rt10xx::input {
namespace {

namespace minput = o3os::platform::mk66::input;
using regs::at;

// An I2cBus that forwards to the instance it is routed to; unrouted, it answers Nak (the scanner and the LEDs back off).
class RoutedBus final : public i2c::Bus {
 public:
  void route(i2c::Bus* target) noexcept { target_ = target; }
  i2c::Result transfer(std::uint8_t a, const std::uint8_t* tx, std::uint8_t tx_len, std::uint8_t* rx, std::uint8_t rx_len) noexcept override {
    return target_ != nullptr ? target_->transfer(a, tx, tx_len, rx, rx_len) : i2c::Result::Nak;
  }
  i2c::Result read(std::uint8_t a, std::uint8_t* rx, std::uint8_t rx_len) noexcept override {
    return target_ != nullptr ? target_->read(a, rx, rx_len) : i2c::Result::Nak;
  }
  i2c::Result recover() noexcept override { return target_ != nullptr ? target_->recover() : i2c::Result::Nak; }
  bool ready() noexcept override { return target_ != nullptr && target_->ready(); }

 private:
  i2c::Bus* target_ = nullptr;
};

Quadrature g_quad;
std::uintptr_t g_enc_psr = 0;  // the encoder port's pad status register, 0 while not set
std::uint32_t g_enc_a = 0;
std::uint32_t g_enc_b = 0;

class Hardware final : public minput::ScanHardware {
 public:
  bool pads_int_asserted() noexcept override { return false; }     // no INT line is known: the safety poll drains the chip
  bool buttons_int_asserted() noexcept override { return false; }
  std::uint16_t encoder_count() noexcept override { return g_quad.count(); }
};

RoutedBus g_pad_route;
RoutedBus g_control_route;
Hardware g_hw;
minput::Scanner g_scanner(g_pad_route, g_control_route, g_hw, system_clock());
minput::PadLeds g_leds(g_pad_route, system_clock());
bool g_started = false;
unsigned g_pad_n = 0;
unsigned g_control_n = 0;

bool answers(unsigned n, std::uint8_t address) noexcept {
  i2c::Bus* b = i2c::bus_of(n);
  return b != nullptr && i2c::init(n) == i2c::Result::Ok && b->transfer(address, nullptr, 0, nullptr, 0) == i2c::Result::Ok;
}

void sample_encoder() noexcept {
  if (g_enc_psr != 0u) {
    const std::uint32_t levels = at<std::uint32_t>(g_enc_psr);
    g_quad.sample((levels & g_enc_a) != 0u, (levels & g_enc_b) != 0u);
  }
}

}  // namespace

bool start(unsigned pad_n, unsigned control_n) noexcept {
  if (g_started) {
    return true;
  }
  if (pad_n == 0u) {
    for (const i2c_cfg::Instance& inst : i2c_cfg::kInstances) {
      if (answers(inst.number, minput::pins::kLedDriverAddress)) {
        pad_n = inst.number;
        break;
      }
    }
  }
  if (pad_n == 0u || i2c::bus_of(pad_n) == nullptr || i2c::bus_of(control_n) == nullptr) {
    return false;
  }
  i2c::init(pad_n);
  i2c::init(control_n);
  g_pad_route.route(i2c::bus_of(pad_n));
  g_control_route.route(i2c::bus_of(control_n));
  g_pad_n = pad_n;
  g_control_n = control_n;
  g_scanner.begin();
  g_started = true;
  return true;
}

bool started() noexcept { return g_started; }
unsigned pad_bus() noexcept { return g_pad_n; }
unsigned control_bus() noexcept { return g_control_n; }

bool set_encoder(unsigned port, unsigned bit_a, unsigned bit_b) noexcept {
  const std::uintptr_t base = regs::gpio_base(port);
  if (base == 0u || bit_a > 31u || bit_b > 31u || bit_a == bit_b) {
    return false;
  }
  regs::gpio_gates_on();
  g_enc_a = 1u << bit_a;
  g_enc_b = 1u << bit_b;
  g_enc_psr = base + 0x08u;  // PSR: the pads' levels (inputs need nothing else: the GPIO direction resets to input)
  const std::uint32_t levels = at<std::uint32_t>(g_enc_psr);
  g_quad.reset((levels & g_enc_a) != 0u, (levels & g_enc_b) != 0u);
  return true;
}

bool encoder_set() noexcept { return g_enc_psr != 0u; }
std::uint32_t encoder_skipped() noexcept { return g_quad.skipped(); }
std::uint16_t encoder_count() noexcept { return g_quad.count(); }

void service() noexcept {
  sample_encoder();
  if (g_started) {
    g_scanner.service();
    (void)g_leds.flush();
  }
}

minput::Scanner& scanner() noexcept { return g_scanner; }
minput::PadLeds& leds() noexcept { return g_leds; }

}  // namespace o3os::platform::rt10xx::input
