// SPDX-License-Identifier: GPL-3.0-or-later
//
// LPI2C1 as a polled master (OS-PORT-RT10XX phase 3): the codec's control bus. The command FIFO takes START + address, the data bytes,
// a receive command and STOP; the status register says when each is done and whether the slave answered. Every wait is bounded; a failed
// transfer ends with the FIFOs reset and a STOP on the bus. The words are i2c_config.h's, the pads audio_pins.h's (assumed).
#include <o3os/platform/rt10xx/i2c.h>

#include "internal.h"
#include "regs.h"

#include <o3os/platform/rt10xx/audio_config.h>
#include <o3os/platform/rt10xx/audio_pins.h>
#include <o3os/platform/rt10xx/i2c_config.h>

#include <cstdint>

namespace o3os::platform::rt10xx::i2c {
namespace {

namespace cfg = i2c_cfg;
using regs::at;

std::uint32_t g_last_status = 0;
std::uint32_t g_transfers = 0;
bool g_inited = false;

void mux_pad(const audio_pins::Pad& pad) noexcept {
  at<std::uint32_t>(pad.mux_ctl) = pad.alt | audio_pins::kMuxSion;
  at<std::uint32_t>(pad.pad_ctl) = audio_pins::kI2cPadWord;
  if (pad.select_input != 0u) {
    at<std::uint32_t>(pad.select_input) = pad.select_value;
  }
}

void reset_master() noexcept {
  at<std::uint32_t>(cfg::kMcr) = cfg::kMcrReset;
  at<std::uint32_t>(cfg::kMcr) = 0;
  at<std::uint32_t>(cfg::kMcfgr1) = cfg::kMcfgr1Word;
  at<std::uint32_t>(cfg::kMcfgr2) = cfg::kMcfgr2Word;
  at<std::uint32_t>(cfg::kMcfgr3) = 0;
  at<std::uint32_t>(cfg::kMccr0) = cfg::kMccr0Word;
  at<std::uint32_t>(cfg::kMfcr) = cfg::kMfcrWord;
  at<std::uint32_t>(cfg::kMier) = 0;
  at<std::uint32_t>(cfg::kMsr) = cfg::kMsrClearAll;
  at<std::uint32_t>(cfg::kMcr) = cfg::kMcrEnable;
}

// Waits until the transmit FIFO can take a command, or an error flag is up. False on a timeout or an error (the flags are left for the caller).
bool wait_tx_room() noexcept {
  for (std::uint32_t i = 0; i < cfg::kByteTimeoutPolls; ++i) {
    const std::uint32_t msr = at<std::uint32_t>(cfg::kMsr);
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
  at<std::uint32_t>(cfg::kMtdr) = command;
  return true;
}

Result classify(std::uint32_t msr) noexcept {
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
  const std::uint32_t msr = at<std::uint32_t>(cfg::kMsr);
  g_last_status = msr;
  const Result r = classify(msr);
  at<std::uint32_t>(cfg::kMcr) = cfg::kMcrEnable | cfg::kMcrResetTxFifo | cfg::kMcrResetRxFifo;
  at<std::uint32_t>(cfg::kMsr) = cfg::kMsrClearAll;
  at<std::uint32_t>(cfg::kMtdr) = cfg::kCmdStop;
  for (std::uint32_t i = 0; i < cfg::kStopTimeoutPolls && (at<std::uint32_t>(cfg::kMsr) & cfg::kMsrMasterBusy) != 0u; ++i) {
  }
  at<std::uint32_t>(cfg::kMsr) = cfg::kMsrClearAll;
  return r;
}

class Lpi2cBus final : public Bus {
 public:
  Result transfer(std::uint8_t address7, const std::uint8_t* tx, std::uint8_t tx_len, std::uint8_t* rx, std::uint8_t rx_len) noexcept override {
    ++g_transfers;
    at<std::uint32_t>(cfg::kMsr) = cfg::kMsrClearAll;
    if (!push(cfg::kCmdStart | static_cast<std::uint32_t>(address7 << 1))) {
      return fail();
    }
    for (std::uint8_t i = 0; i < tx_len; ++i) {
      if (!push(cfg::kCmdTransmit | tx[i])) {
        return fail();
      }
    }
    if (rx_len != 0u) {
      if (!push(cfg::kCmdStart | static_cast<std::uint32_t>((address7 << 1) | 1u)) || !push(cfg::kCmdReceive | static_cast<std::uint32_t>(rx_len - 1u))) {
        return fail();
      }
      for (std::uint8_t i = 0; i < rx_len; ++i) {
        bool got = false;
        for (std::uint32_t polls = 0; polls < cfg::kByteTimeoutPolls; ++polls) {
          if ((at<std::uint32_t>(cfg::kMsr) & cfg::kMsrErrors) != 0u) {
            return fail();
          }
          const std::uint32_t data = at<std::uint32_t>(cfg::kMrdr);
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
      const std::uint32_t msr = at<std::uint32_t>(cfg::kMsr);
      if ((msr & cfg::kMsrErrors) != 0u) {
        return fail();
      }
      if ((msr & cfg::kMsrStopDetect) != 0u) {
        g_last_status = msr;
        at<std::uint32_t>(cfg::kMsr) = cfg::kMsrClearAll;
        return Result::Ok;
      }
    }
    return fail();
  }
  Result read(std::uint8_t address7, std::uint8_t* rx, std::uint8_t rx_len) noexcept override { return transfer(address7, nullptr, 0, rx, rx_len); }
  Result recover() noexcept override {
    reset_master();
    return (at<std::uint32_t>(cfg::kMsr) & cfg::kMsrBusBusy) == 0u ? Result::Ok : Result::BusStuck;
  }
};

Lpi2cBus g_bus;

}  // namespace

Result init() noexcept {
  if (g_inited) {
    return Result::Ok;
  }
  // The clock: the oscillator, undivided, then the gate.
  at<std::uint32_t>(cfg::kCscdr2) = (at<std::uint32_t>(cfg::kCscdr2) & ~cfg::kCscdr2Lpi2cPodfMask) | cfg::kCscdr2Lpi2cClkSel;
  at<std::uint32_t>(audio_cfg::gates::kCcgr2) |= audio_cfg::gates::kCcgr2Lpi2c1;
  // The pads (assumed: audio_pins.h), then the master.
  mux_pad(audio_pins::kScl);
  mux_pad(audio_pins::kSda);
  reset_master();
  g_inited = true;
  return Result::Ok;
}

Bus& bus() noexcept { return g_bus; }

Result write_reg(std::uint8_t address7, std::uint16_t reg, std::uint16_t value) noexcept {
  const std::uint8_t tx[4] = {static_cast<std::uint8_t>(reg >> 8), static_cast<std::uint8_t>(reg), static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value)};
  return g_bus.transfer(address7, tx, 4, nullptr, 0);
}

Result read_reg(std::uint8_t address7, std::uint16_t reg, std::uint16_t& value) noexcept {
  const std::uint8_t tx[2] = {static_cast<std::uint8_t>(reg >> 8), static_cast<std::uint8_t>(reg)};
  std::uint8_t rx[2] = {0, 0};
  const Result r = g_bus.transfer(address7, tx, 2, rx, 2);
  value = static_cast<std::uint16_t>((rx[0] << 8) | rx[1]);
  return r;
}

std::uint32_t last_status() noexcept { return g_last_status; }
std::uint32_t transfers() noexcept { return g_transfers; }

}  // namespace o3os::platform::rt10xx::i2c
