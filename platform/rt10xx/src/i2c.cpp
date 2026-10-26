// SPDX-License-Identifier: GPL-3.0-or-later
//
// LPI2C1, 3 and 4 as polled masters (OS-PORT-RT10XX phases 3 and 4). The command FIFO takes START + address, the data bytes, a receive
// command and STOP; the status register says when each is done and whether the slave answered. Every wait is bounded; a failed transfer
// ends with the FIFOs reset and a STOP on the bus. The words are i2c_config.h's, each instance's pads its kInstances row (assumed).
#include <o3os/platform/rt10xx/i2c.h>

#include "internal.h"
#include "regs.h"

#include <o3os/platform/rt10xx/audio_pins.h>
#include <o3os/platform/rt10xx/i2c_config.h>

#include <cstdint>

namespace o3os::platform::rt10xx::i2c {
namespace {

namespace cfg = i2c_cfg;
using regs::at;

std::uint32_t g_last_status = 0;
std::uint32_t g_transfers = 0;
bool g_root_set = false;

class Lpi2cBus final : public Bus {
 public:
  explicit Lpi2cBus(const cfg::Instance& inst) noexcept : inst_(inst) {}

  bool begin() noexcept {
    if (inited_) {
      return true;
    }
    at<std::uint32_t>(inst_.ccgr) |= inst_.ccgr_bits;
    mux(inst_.scl);
    mux(inst_.sda);
    reset_master();
    inited_ = true;
    return true;
  }
  bool inited() const noexcept { return inited_; }
  bool ready() noexcept override { return inited_; }

  Result transfer(std::uint8_t address7, const std::uint8_t* tx, std::uint8_t tx_len, std::uint8_t* rx, std::uint8_t rx_len) noexcept override {
    return inited_ ? run(address7, tx, tx_len, rx, rx_len, true) : Result::Nak;
  }
  // The contract's plain read: START+R, the bytes, STOP; no write phase.
  Result read(std::uint8_t address7, std::uint8_t* rx, std::uint8_t rx_len) noexcept override {
    return inited_ ? run(address7, nullptr, 0, rx, rx_len, false) : Result::Nak;
  }
  // The master reset, then, if the bus is still busy, up to nine SCL pulses with the pads as GPIOs and a STOP, the pads back to the master.
  Result recover() noexcept override {
    if (!inited_) {
      return Result::Nak;
    }
    reset_master();
    if ((reg(cfg::kOffMsr) & cfg::kMsrBusBusy) == 0u) {
      return Result::Ok;
    }
    at<std::uint32_t>(audio_pins::kGpio1Ccgr) |= audio_pins::kGpio1Gate;
    pad_as_gpio(inst_.sda, inst_.sda_gpio_bit, true, false);  // SDA read while SCL is pulsed
    pad_as_gpio(inst_.scl, inst_.scl_gpio_bit, true, true);
    bool released = false;
    for (unsigned pulse = 0; pulse < 9u && !released; ++pulse) {
      gpio_write(inst_.scl_gpio_bit, false);
      boot::delay_us(5);
      gpio_write(inst_.scl_gpio_bit, true);
      boot::delay_us(5);
      released = gpio_read(inst_.sda_gpio_bit);
    }
    if (released) {  // a STOP: SDA low while SCL is high, then SDA released
      pad_as_gpio(inst_.sda, inst_.sda_gpio_bit, true, true);
      gpio_write(inst_.sda_gpio_bit, false);
      boot::delay_us(5);
      gpio_write(inst_.sda_gpio_bit, true);
      boot::delay_us(5);
    }
    pad_as_gpio(inst_.sda, inst_.sda_gpio_bit, false, false);
    pad_as_gpio(inst_.scl, inst_.scl_gpio_bit, false, false);
    reset_master();
    return (released && (reg(cfg::kOffMsr) & cfg::kMsrBusBusy) == 0u) ? Result::Ok : Result::BusStuck;
  }

 private:
  volatile std::uint32_t& reg(std::uintptr_t offset) const noexcept { return at<std::uint32_t>(inst_.base + offset); }

  static void mux(const audio_pins::Pad& pad) noexcept {
    at<std::uint32_t>(pad.pad_ctl) = audio_pins::kI2cPadWord;
    if (pad.select_input != 0u) {
      at<std::uint32_t>(pad.select_input) = pad.select_value;
    }
    at<std::uint32_t>(pad.mux_ctl) = pad.alt | audio_pins::kMuxSion;
  }

  void reset_master() noexcept {
    reg(cfg::kOffMcr) = cfg::kMcrReset;
    reg(cfg::kOffMcr) = 0;
    reg(cfg::kOffMcfgr1) = cfg::kMcfgr1Word;
    reg(cfg::kOffMcfgr2) = cfg::kMcfgr2Word;
    reg(cfg::kOffMcfgr3) = 0;
    reg(cfg::kOffMccr0) = cfg::kMccr0Word;
    reg(cfg::kOffMfcr) = cfg::kMfcrWord;
    reg(cfg::kOffMier) = 0;
    reg(cfg::kOffMsr) = cfg::kMsrClearAll;
    reg(cfg::kOffMcr) = cfg::kMcrEnable;
  }

  bool wait_tx_room() noexcept {
    for (std::uint32_t i = 0; i < cfg::kByteTimeoutPolls; ++i) {
      const std::uint32_t msr = reg(cfg::kOffMsr);
      if ((msr & cfg::kMsrErrors) != 0u) {
        return false;
      }
      if ((msr & cfg::kMsrTxReady) != 0u) {
        return true;
      }
    }
    return false;
  }

  bool push(std::uint32_t command) noexcept {
    if (!wait_tx_room()) {
      return false;
    }
    reg(cfg::kOffMtdr) = command;
    return true;
  }

  static Result classify(std::uint32_t msr) noexcept {
    if ((msr & cfg::kMsrNackDetect) != 0u) {
      return Result::Nak;
    }
    if ((msr & cfg::kMsrArbitrationLost) != 0u) {
      return Result::ArbitrationLost;
    }
    if ((msr & cfg::kMsrPinLowTimeout) != 0u) {
      return Result::BusStuck;
    }
    return Result::Timeout;
  }

  // The transfer ended badly: note the status, reset the FIFOs, try a STOP, re-enable.
  Result fail() noexcept {
    const std::uint32_t msr = reg(cfg::kOffMsr);
    g_last_status = msr;
    const Result r = classify(msr);
    reg(cfg::kOffMcr) = cfg::kMcrEnable | cfg::kMcrResetTxFifo | cfg::kMcrResetRxFifo;
    reg(cfg::kOffMsr) = cfg::kMsrClearAll;
    reg(cfg::kOffMtdr) = cfg::kCmdStop;
    for (std::uint32_t i = 0; i < cfg::kStopTimeoutPolls && (reg(cfg::kOffMsr) & cfg::kMsrMasterBusy) != 0u; ++i) {
    }
    reg(cfg::kOffMsr) = cfg::kMsrClearAll;
    return r;
  }

  Result run(std::uint8_t address7, const std::uint8_t* tx, std::uint8_t tx_len, std::uint8_t* rx, std::uint8_t rx_len, bool write_phase) noexcept {
    ++g_transfers;
    reg(cfg::kOffMsr) = cfg::kMsrClearAll;
    if (write_phase) {
      if (!push(cfg::kCmdStart | static_cast<std::uint32_t>(address7 << 1))) {
        return fail();
      }
      for (std::uint8_t i = 0; i < tx_len; ++i) {
        if (!push(cfg::kCmdTransmit | tx[i])) {
          return fail();
        }
      }
    }
    if (rx_len != 0u) {
      if (!push(cfg::kCmdStart | static_cast<std::uint32_t>((address7 << 1) | 1u)) || !push(cfg::kCmdReceive | static_cast<std::uint32_t>(rx_len - 1u))) {
        return fail();
      }
      for (std::uint8_t i = 0; i < rx_len; ++i) {
        bool got = false;
        for (std::uint32_t polls = 0; polls < cfg::kByteTimeoutPolls; ++polls) {
          if ((reg(cfg::kOffMsr) & cfg::kMsrErrors) != 0u) {
            return fail();
          }
          const std::uint32_t data = reg(cfg::kOffMrdr);
          if ((data & cfg::kMrdrEmpty) == 0u) {
            rx[i] = static_cast<std::uint8_t>(data);
            got = true;
            break;
          }
        }
        if (!got) {
          return fail();
        }
      }
    }
    if (!push(cfg::kCmdStop)) {
      return fail();
    }
    for (std::uint32_t i = 0; i < cfg::kStopTimeoutPolls; ++i) {
      const std::uint32_t msr = reg(cfg::kOffMsr);
      if ((msr & cfg::kMsrErrors) != 0u) {
        return fail();
      }
      if ((msr & cfg::kMsrStopDetect) != 0u) {
        g_last_status = msr;
        reg(cfg::kOffMsr) = cfg::kMsrClearAll;
        return Result::Ok;
      }
    }
    return fail();
  }

  // The pads as open-drain GPIO1 lines (ALT5; the pad word keeps ODE and the pull-up): an output drives low or releases, the input reads.
  static void gpio_write(unsigned bit, bool high) noexcept {
    volatile std::uint32_t& dr = at<std::uint32_t>(audio_pins::kGpio1 + audio_pins::kGpioDr);
    dr = high ? (dr | (1u << bit)) : (dr & ~(1u << bit));
  }
  static bool gpio_read(unsigned bit) noexcept { return (at<std::uint32_t>(audio_pins::kGpio1 + audio_pins::kGpioPsr) & (1u << bit)) != 0u; }
  static void pad_as_gpio(const audio_pins::Pad& pad, unsigned bit, bool gpio, bool output) noexcept {
    volatile std::uint32_t& dir = at<std::uint32_t>(audio_pins::kGpio1 + audio_pins::kGpioGdir);
    if (gpio) {
      gpio_write(bit, true);
      dir = output ? (dir | (1u << bit)) : (dir & ~(1u << bit));
      at<std::uint32_t>(pad.mux_ctl) = audio_pins::kGpioAlt | audio_pins::kMuxSion;
    } else {
      at<std::uint32_t>(pad.mux_ctl) = pad.alt | audio_pins::kMuxSion;
      dir &= ~(1u << bit);
    }
  }

  const cfg::Instance& inst_;
  bool inited_ = false;
};

Lpi2cBus g_buses[cfg::kInstanceCount] = {Lpi2cBus(cfg::kInstances[0]), Lpi2cBus(cfg::kInstances[1]), Lpi2cBus(cfg::kInstances[2])};
static_assert(cfg::kInstanceCount == 3u, "one bus object per instance row");

Lpi2cBus* find(unsigned n) noexcept {
  for (unsigned i = 0; i < cfg::kInstanceCount; ++i) {
    if (cfg::kInstances[i].number == n) {
      return &g_buses[i];
    }
  }
  return nullptr;
}

}  // namespace

Result init(unsigned n) noexcept {
  Lpi2cBus* b = find(n);
  if (b == nullptr) {
    return Result::Nak;
  }
  if (!g_root_set) {  // the LPI2C root, shared by every instance: the oscillator, undivided
    at<std::uint32_t>(cfg::kCscdr2) = (at<std::uint32_t>(cfg::kCscdr2) & ~cfg::kCscdr2Lpi2cPodfMask) | cfg::kCscdr2Lpi2cClkSel;
    g_root_set = true;
  }
  b->begin();
  return Result::Ok;
}

Bus* bus_of(unsigned n) noexcept { return find(n); }
bool inited(unsigned n) noexcept {
  const Lpi2cBus* b = find(n);
  return b != nullptr && b->inited();
}
Bus& bus() noexcept { return g_buses[0]; }

Result write_reg(std::uint8_t address7, std::uint16_t reg, std::uint16_t value) noexcept {
  const std::uint8_t tx[4] = {static_cast<std::uint8_t>(reg >> 8), static_cast<std::uint8_t>(reg), static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value)};
  return g_buses[0].transfer(address7, tx, 4, nullptr, 0);
}

Result read_reg(std::uint8_t address7, std::uint16_t reg, std::uint16_t& value) noexcept {
  const std::uint8_t tx[2] = {static_cast<std::uint8_t>(reg >> 8), static_cast<std::uint8_t>(reg)};
  std::uint8_t rx[2] = {0, 0};
  const Result r = g_buses[0].transfer(address7, tx, 2, rx, 2);
  value = static_cast<std::uint16_t>((rx[0] << 8) | rx[1]);
  return r;
}

std::uint32_t last_status() noexcept { return g_last_status; }
std::uint32_t transfers() noexcept { return g_transfers; }

}  // namespace o3os::platform::rt10xx::i2c
