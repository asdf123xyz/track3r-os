// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The rt10xx's uSDHC1 as an SdBus (OS-PORT-RT10XX phase 4): the register policy the chip-neutral card logic (platform/mk66 storage/sd_card.h)
// runs on. Polled, no DMA and no interrupt, for the MK66 driver's reasons (platform/mk66 storage/sdhc_bus.h): the main loop waits for its
// transfers anyway, and the controller's FIFO is read 16 words at a time as the watermark says they are there. Every wait is bounded by the
// MK66's sd_policy.h. Declared for every build; defined in the image only (src/usdhc_bus.cpp). Main loop only.

#include <o3os/platform/mk66/storage/sd_bus.h>

#include <cstdint>

namespace o3os::platform::rt10xx::storage {

class UsdhcBus final : public o3os::platform::mk66::storage::sd::SdBus {
 public:
  using Result = o3os::platform::mk66::storage::sd::Result;
  using Cmd = o3os::platform::mk66::storage::sd::Cmd;
  using Response = o3os::platform::mk66::storage::sd::Response;

  bool card_present() const noexcept override { return true; }  // no card-detect contact is known: a card that does not answer is "no card"
  Result power_up() noexcept override;
  std::uint32_t set_clock(std::uint32_t max_hz) noexcept override;
  void set_bus_width(unsigned bits) noexcept override;
  Result command(Cmd cmd, std::uint32_t arg, Response& out) noexcept override;
  Result read_data(Cmd cmd, std::uint32_t arg, void* dst, std::uint32_t block_size, std::uint32_t blocks, Response& out) noexcept override;
  Result write_data(Cmd cmd, std::uint32_t arg, const void* src, std::uint32_t block_size, std::uint32_t blocks, Response& out) noexcept override;
  bool write_protect_switch() const noexcept override { return false; }  // no write-protect contact is known
  void delay_ms(std::uint32_t ms) noexcept override;
  std::uint32_t micros() noexcept override;

  std::uint32_t root_hz() const noexcept { return root_hz_; }  // the clock root power_up() found (0: PLL2 PFD2 not running)

 private:
  Result send(Cmd cmd, std::uint32_t arg, Response& out, std::uint32_t busy_us) noexcept;
  Result start_command(Cmd cmd, std::uint32_t arg) noexcept;
  Result wait_idle(bool data_lines) noexcept;
  Result wait_command_done() noexcept;
  void recover_after_data_error(Cmd cmd) noexcept;
  std::uint32_t root_hz_ = 0;
};

}  // namespace o3os::platform::rt10xx::storage
