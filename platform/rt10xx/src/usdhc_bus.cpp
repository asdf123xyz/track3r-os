// SPDX-License-Identifier: GPL-3.0-or-later
//
// uSDHC1 as an SdBus (OS-PORT-RT10XX phase 4). The MK66's SDHC driver (platform/mk66 storage/src/sdhc_bus.cpp) with this chip's
// differences (usdhc_facts.h): the clock root is PLL2 PFD2 through the CCM, read and never reprogrammed; the data bits of a command go to
// MIX_CTRL before the command word; SYS_CTRL has no SDCLKEN; the pads are the IOMUXC's. Everything else (the polled FIFO, the bounded
// waits of sd_policy.h, the recovery after a data error) is the same design.
#include "internal.h"
#include "regs.h"

#include <o3os/platform/mk66/storage/sd_policy.h>
#include <o3os/platform/rt10xx/clock_config.h>
#include <o3os/platform/rt10xx/platform.h>
#include <o3os/platform/rt10xx/storage/usdhc_bus.h>
#include <o3os/platform/rt10xx/storage/usdhc_facts.h>

#include <cstring>

namespace o3os::platform::rt10xx::storage {
namespace {

namespace f = usdhc;
namespace sd = o3os::platform::mk66::storage::sd;
namespace pol = o3os::platform::mk66::storage::sd::policy;
using sd::Cmd;
using sd::Resp;
using sd::Response;
using sd::Result;
using regs::at;

constexpr std::uint32_t kTicksPerUs = clock::kCoreHz / 1'000'000u;

inline volatile std::uint32_t& reg(std::uint32_t offset) noexcept { return at<std::uint32_t>(f::kBase + offset); }
inline pol::Deadline deadline_us(std::uint32_t us) noexcept { return pol::Deadline{regs::dwt_cyccnt(), pol::to_ticks(us, kTicksPerUs)}; }
inline bool expired(const pol::Deadline& d) noexcept { return d.expired(regs::dwt_cyccnt()); }

bool reset_circuits(std::uint32_t mask) noexcept {
  reg(f::kOffSysCtrl) = reg(f::kOffSysCtrl) | mask;
  const pol::Deadline d = deadline_us(pol::kControllerResetUs);
  while ((reg(f::kOffSysCtrl) & mask) != 0u) {
    if (expired(d)) {
      return false;
    }
  }
  return true;
}

Result classify_data_error(std::uint32_t status) noexcept { return (status & f::kIntDtoe) != 0u ? Result::DataTimeout : Result::DataError; }

void mux_pads() noexcept {
  const f::SdPad cmd_data[] = {f::kCmd, f::kData0, f::kData1, f::kData2, f::kData3};
  for (const f::SdPad& p : cmd_data) {
    at<std::uint32_t>(p.pad_ctl) = f::kPadCmdData;
    at<std::uint32_t>(p.mux_ctl) = f::kPadAlt;
  }
  at<std::uint32_t>(f::kClk.pad_ctl) = f::kPadClk;
  at<std::uint32_t>(f::kClk.mux_ctl) = f::kPadAlt;
}

}  // namespace

void UsdhcBus::delay_ms(std::uint32_t ms) noexcept { boot::delay_ms(ms); }
std::uint32_t UsdhcBus::micros() noexcept { return system_clock().now(); }
void UsdhcBus::set_bus_width(unsigned bits) noexcept { reg(f::kOffProtCtrl) = f::prot_bus_width(reg(f::kOffProtCtrl), bits); }

std::uint32_t UsdhcBus::set_clock(std::uint32_t max_hz) noexcept {
  const f::ClockPlan plan = f::plan_clock(root_hz_, max_hz);
  if (!plan.ok) {
    return 0;
  }
  reg(f::kOffSysCtrl) = (reg(f::kOffSysCtrl) & ~f::kSysClockMask) | f::sys_clock_fields(plan);
  const pol::Deadline d = deadline_us(pol::kClockStableUs);
  while ((reg(f::kOffPresState) & f::kPrsSdstb) == 0u) {
    if (expired(d)) {
      return 0;
    }
  }
  return plan.hz;
}

Result UsdhcBus::power_up() noexcept {
  // The root: PFD2 selected, divided by 2, the gate opened; then what it runs at, from the analog words as they are.
  at<std::uint32_t>(f::kCcgr6) &= ~f::kCcgr6Usdhc1;  // gated while the root changes
  at<std::uint32_t>(f::kCscmr1) &= ~f::kCscmr1Usdhc1Sel;
  at<std::uint32_t>(f::kCscdr1) = (at<std::uint32_t>(f::kCscdr1) & ~f::kCscdr1PodfMask) | (f::kPodf << f::kCscdr1PodfShift);
  at<std::uint32_t>(f::kCcgr6) |= f::kCcgr6Usdhc1;
  root_hz_ = f::root_hz(at<std::uint32_t>(f::kPllSys), at<std::uint32_t>(f::kPfd528), f::kPodf);
  if (root_hz_ == 0u) {
    return Result::NotReady;  // PLL2 or its PFD2 is not running (`sd` shows the root as 0)
  }
  if (!reset_circuits(f::kSysRsta)) {
    return Result::NotReady;
  }
  if (set_clock(f::kIdentifyMaxHz) == 0u) {
    return Result::NotReady;
  }
  mux_pads();
  reg(f::kOffIntStatusEn) = f::kIntEnabled;  // visible in INT_STATUS, ...
  reg(f::kOffIntSignalEn) = 0;               // ... never an interrupt: polled
  reg(f::kOffIntStatus) = f::kIntAll;
  reg(f::kOffProtCtrl) = f::prot_bus_width(reg(f::kOffProtCtrl), 1);
  reg(f::kOffWtmkLvl) = f::wtmk(reg(f::kOffWtmkLvl));
  reg(f::kOffMixCtrl) = reg(f::kOffMixCtrl) & ~f::kMixMask;
  boot::delay_ms(pol::kPowerSettleMs);
  reg(f::kOffSysCtrl) = reg(f::kOffSysCtrl) | f::kSysInita;  // 80 clocks to the card
  const pol::Deadline d = deadline_us(pol::kInitClocksUs);
  while ((reg(f::kOffSysCtrl) & f::kSysInita) != 0u) {
    if (expired(d)) {
      return Result::NotReady;
    }
  }
  return Result::Ok;
}

Result UsdhcBus::wait_idle(bool data_lines) noexcept {
  const std::uint32_t mask = f::kPrsCihb | (data_lines ? f::kPrsCdihb : 0u);
  pol::Deadline d = deadline_us(pol::kInhibitUs);
  while ((reg(f::kOffPresState) & mask) != 0u) {
    if (expired(d)) {
      (void)reset_circuits(f::kSysRstc | f::kSysRstd);
      d = deadline_us(pol::kInhibitUs);
      while ((reg(f::kOffPresState) & mask) != 0u) {
        if (expired(d)) {
          return Result::NotReady;
        }
      }
      return Result::Ok;
    }
  }
  return Result::Ok;
}

Result UsdhcBus::wait_command_done() noexcept {
  const pol::Deadline d = deadline_us(pol::kCommandUs);
  for (;;) {
    const std::uint32_t s = reg(f::kOffIntStatus);
    if ((s & f::kIntCommandErrors) != 0u) {
      reg(f::kOffIntStatus) = s;
      (void)reset_circuits(f::kSysRstc);
      return (s & f::kIntCtoe) != 0u ? Result::NoResponse : Result::CommandError;
    }
    if ((s & f::kIntCc) != 0u) {
      reg(f::kOffIntStatus) = f::kIntCc;
      return Result::Ok;
    }
    if (expired(d)) {
      (void)reset_circuits(f::kSysRstc);
      return Result::NoResponse;
    }
  }
}

// The data bits first (MIX_CTRL), then the argument, then the command word, whose write starts the command.
Result UsdhcBus::start_command(Cmd cmd, std::uint32_t arg) noexcept {
  reg(f::kOffMixCtrl) = (reg(f::kOffMixCtrl) & ~f::kMixMask) | f::mix_ctrl(cmd);
  reg(f::kOffCmdArg) = arg;
  reg(f::kOffCmdXfrTyp) = f::xfr_typ(cmd);
  return wait_command_done();
}

Result UsdhcBus::send(Cmd cmd, std::uint32_t arg, Response& out, std::uint32_t busy_us) noexcept {
  const Resp type = sd::response_of(cmd);
  const bool busy_command = type == Resp::R1b;
  Result r = wait_idle(busy_command);
  if (r != Result::Ok) {
    return r;
  }
  reg(f::kOffIntStatus) = f::kIntAll;
  r = start_command(cmd, arg);
  if (r != Result::Ok) {
    return r;
  }
  for (unsigned i = 0; i < 4; ++i) {
    out.word[i] = type == Resp::None ? 0u : reg(f::kOffCmdRsp0 + 4u * i);
  }
  if (busy_command) {
    const pol::Deadline d = deadline_us(busy_us);
    while ((reg(f::kOffPresState) & f::kPrsDat0) == 0u) {
      if (expired(d)) {
        return Result::NotReady;
      }
    }
  }
  return Result::Ok;
}

Result UsdhcBus::command(Cmd cmd, std::uint32_t arg, Response& out) noexcept {
  if (!sd::known(cmd) || sd::reads_data(cmd) || sd::writes_data(cmd)) {
    return Result::Refused;
  }
  return send(cmd, arg, out, pol::kBusyUs);
}

void UsdhcBus::recover_after_data_error(Cmd cmd) noexcept {
  (void)reset_circuits(f::kSysRstc | f::kSysRstd);
  reg(f::kOffIntStatus) = f::kIntAll;
  if (cmd != Cmd::SwitchFunc) {
    Response ignored;
    (void)send(Cmd::StopTransmission, 0, ignored, pol::kStopBusyUs);
  }
}

Result UsdhcBus::read_data(Cmd cmd, std::uint32_t arg, void* dst, std::uint32_t block_size, std::uint32_t blocks, Response& out) noexcept {
  if (!sd::known(cmd) || !sd::reads_data(cmd)) {
    return Result::Refused;
  }
  if (dst == nullptr || blocks == 0u || blocks > 0xFFFFu || block_size == 0u || block_size > sd::kBlockSize || (block_size % 64u) != 0u ||
      (cmd != Cmd::ReadMultipleBlock && blocks != 1u)) {
    return Result::BadArgument;
  }
  Result r = wait_idle(true);
  if (r != Result::Ok) {
    return r;
  }
  reg(f::kOffIntStatus) = f::kIntAll;
  reg(f::kOffBlkAtt) = f::blk_att(block_size, blocks);
  r = start_command(cmd, arg);
  if (r != Result::Ok) {
    recover_after_data_error(cmd);
    return r;
  }
  for (unsigned i = 0; i < 4; ++i) {
    out.word[i] = reg(f::kOffCmdRsp0 + 4u * i);
  }
  const pol::Deadline d = deadline_us(pol::data_timeout_us(blocks));
  std::uint8_t* p = static_cast<std::uint8_t*>(dst);
  const std::uint32_t groups = block_size / 4u / f::kReadWatermarkWords * blocks;
  Result failure = Result::Ok;
  for (std::uint32_t g = 0; g < groups && failure == Result::Ok; ++g) {
    for (;;) {
      const std::uint32_t s = reg(f::kOffIntStatus);
      if ((s & f::kIntDataErrors) != 0u) {
        failure = classify_data_error(s);
        break;
      }
      if ((reg(f::kOffPresState) & f::kPrsBren) != 0u) {
        break;
      }
      if (expired(d)) {
        failure = Result::DataTimeout;
        break;
      }
    }
    if (failure != Result::Ok) {
      break;
    }
    for (std::uint32_t w = 0; w < f::kReadWatermarkWords; ++w) {
      const std::uint32_t word = reg(f::kOffDataPort);
      std::memcpy(p, &word, 4);
      p += 4;
    }
  }
  while (failure == Result::Ok) {
    const std::uint32_t s = reg(f::kOffIntStatus);
    if ((s & f::kIntDataErrors) != 0u) {
      failure = classify_data_error(s);
    } else if ((s & f::kIntTc) != 0u) {
      break;
    } else if (expired(d)) {
      failure = Result::DataTimeout;
    }
  }
  if (failure != Result::Ok) {
    recover_after_data_error(cmd);
    return failure;
  }
  reg(f::kOffIntStatus) = f::kIntAll;
  return Result::Ok;
}

Result UsdhcBus::write_data(Cmd cmd, std::uint32_t arg, const void* src, std::uint32_t block_size, std::uint32_t blocks, Response& out) noexcept {
  if (!sd::known(cmd) || !sd::writes_data(cmd)) {
    return Result::Refused;
  }
  if (src == nullptr || blocks != 1u || block_size != sd::kBlockSize) {
    return Result::BadArgument;
  }
  Result r = wait_idle(true);
  if (r != Result::Ok) {
    return r;
  }
  reg(f::kOffIntStatus) = f::kIntAll;
  reg(f::kOffBlkAtt) = f::blk_att(block_size, 1);
  r = start_command(cmd, arg);
  if (r != Result::Ok) {
    recover_after_data_error(cmd);
    return r;
  }
  for (unsigned i = 0; i < 4; ++i) {
    out.word[i] = reg(f::kOffCmdRsp0 + 4u * i);
  }
  const pol::Deadline fill = deadline_us(pol::kWriteDataUs);
  const std::uint8_t* p = static_cast<const std::uint8_t*>(src);
  Result failure = Result::Ok;
  for (std::uint32_t g = 0; g < block_size / 4u / f::kWriteWatermarkWords && failure == Result::Ok; ++g) {
    for (;;) {
      const std::uint32_t s = reg(f::kOffIntStatus);
      if ((s & f::kIntDataErrors) != 0u) {
        failure = classify_data_error(s);
        break;
      }
      if ((reg(f::kOffPresState) & f::kPrsBwen) != 0u) {
        break;
      }
      if (expired(fill)) {
        failure = Result::DataTimeout;
        break;
      }
    }
    if (failure != Result::Ok) {
      break;
    }
    for (std::uint32_t w = 0; w < f::kWriteWatermarkWords; ++w) {
      std::uint32_t word;
      std::memcpy(&word, p, 4);
      reg(f::kOffDataPort) = word;
      p += 4;
    }
  }
  const pol::Deadline complete = deadline_us(pol::kWriteDataUs + pol::kWriteBusyUs);
  while (failure == Result::Ok) {
    const std::uint32_t s = reg(f::kOffIntStatus);
    if ((s & f::kIntDataErrors) != 0u) {
      failure = classify_data_error(s);
    } else if ((s & f::kIntTc) != 0u) {
      break;
    } else if (expired(complete)) {
      failure = Result::DataTimeout;
    }
  }
  if (failure == Result::Ok) {
    const pol::Deadline busy = deadline_us(pol::kWriteBusyUs);
    while ((reg(f::kOffPresState) & f::kPrsDat0) == 0u) {
      if (expired(busy)) {
        failure = Result::DataTimeout;
        break;
      }
    }
  }
  if (failure != Result::Ok) {
    recover_after_data_error(cmd);
    return failure;
  }
  reg(f::kOffIntStatus) = f::kIntAll;
  return Result::Ok;
}

}  // namespace o3os::platform::rt10xx::storage
