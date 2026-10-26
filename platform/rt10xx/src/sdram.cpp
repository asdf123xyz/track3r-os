// SPDX-License-Identifier: GPL-3.0-or-later
//
// The SDRAM bring-up of the rt10xx image (OS-PORT-RT10XX phase 2): the SEMC's clock, the pads, the controller's registers, the JEDEC
// initialisation through the IP command port (precharge all, auto refresh, mode set), the refresh timer, then the tests of
// platform/mk66's memtest.h over the array. Every wait is bounded; a step that fails is recorded in the status and the sequence stops
// there, the boot goes on (sdram.h says why). The words are semc_config.h's, the register facts regs.h's.
//
// The tests run with the data cache off (the reset sequence runs this step before it enables the caches; `retest` switches the cache
// off around the test), so that a read is a read of the cell, not of a cache line.
#include "internal.h"
#include "regs.h"

#include <o3os/platform/mk66/memtest.h>
#include <o3os/platform/rt10xx/mem_map.h>
#include <o3os/platform/rt10xx/sdram.h>
#include <o3os/platform/rt10xx/semc_config.h>

#include <cstdint>

#ifndef O3OS_RT10XX_SDRAM_FULL_TEST
#define O3OS_RT10XX_SDRAM_FULL_TEST 0
#endif

namespace o3os::platform::rt10xx::sdram {
namespace {

Status g_status;

// The accessor memtest.h wants, over the array through volatile word and byte pointers. `stride` makes the sparse capacity test: index i
// is word i x stride of the array.
struct SdramMem {
  volatile std::uint32_t* words;
  std::uint32_t stride;
  std::uint32_t read(std::uint32_t index) const noexcept { return words[index * stride]; }
  void write(std::uint32_t index, std::uint32_t value) const noexcept { words[index * stride] = value; }
  void write8(std::uint32_t byte_index, std::uint8_t value) const noexcept {
    reinterpret_cast<volatile std::uint8_t*>(words)[byte_index] = value;  // the wiring tests only (stride 1)
  }
};

constexpr std::uint32_t kWords = mem::kSdramSize / 4u;

template <typename Predicate>
bool wait_until(Predicate done, std::uint32_t polls = 1'000'000u) noexcept {
  for (std::uint32_t i = 0; i < polls; ++i) {
    if (done()) {
      return true;
    }
  }
  return false;
}

// One IP command: the key and the command word, then the completion flag; false when it did not finish or the controller reported an error.
bool ip_command(std::uint32_t command) noexcept {
  using regs::at;
  at<std::uint32_t>(semc::kIntr) = semc::kIntrAll;  // write-1-to-clear
  at<std::uint32_t>(semc::kIpcmd) = semc::kIpcmdKey | command;
  if (!wait_until([] { return (at<std::uint32_t>(semc::kIntr) & semc::kIntrIpCmdDone) != 0u; })) {
    g_status.failed_command = command;
    g_status.intr = at<std::uint32_t>(semc::kIntr);
    return false;
  }
  const std::uint32_t intr = at<std::uint32_t>(semc::kIntr);
  at<std::uint32_t>(semc::kIntr) = semc::kIntrAll;
  if ((intr & semc::kIntrIpCmdErr) != 0u) {
    g_status.failed_command = command;
    g_status.intr = intr;
    return false;
  }
  return true;
}

void run_tests(bool full) noexcept {
  const std::uint32_t started = regs::dwt_cyccnt();
  g_status.reached = Step::WiringTest;
  const SdramMem whole{reinterpret_cast<volatile std::uint32_t*>(static_cast<std::uintptr_t>(mem::kSdramBase)), 1u};
  g_status.wiring = memtest::wiring_test(whole, kWords);
  if (g_status.wiring.ok) {
    g_status.reached = Step::CapacityTest;
    g_status.full_test = full;
    if (full) {
      g_status.capacity = memtest::fill_test(whole, kWords, semc::kFillSeed);
    } else {
      const SdramMem sparse{whole.words, semc::kSparseStride};
      g_status.capacity = memtest::fill_test(sparse, kWords / semc::kSparseStride, semc::kFillSeed);
      if (!g_status.capacity.ok) {
        g_status.capacity.index *= semc::kSparseStride;  // the array's word index, not the sparse one
      }
    }
  }
  const std::uint32_t cycles = regs::dwt_cyccnt() - started;
  g_status.test_ms = cycles / (clock::kCoreHz / 1000u);
  g_status.ok = g_status.wiring.ok && g_status.capacity.ok;
  if (g_status.ok) {
    g_status.reached = Step::Done;
  }
}

}  // namespace

const char* step_name(Step step) noexcept {
  switch (step) {
    case Step::NotRun: return "not run";
    case Step::Clock: return "clock";
    case Step::Pads: return "pads";
    case Step::Controller: return "controller";
    case Step::PrechargeAll: return "precharge all";
    case Step::AutoRefresh: return "auto refresh";
    case Step::ModeSet: return "mode set";
    case Step::RefreshOn: return "refresh on";
    case Step::WiringTest: return "wiring test";
    case Step::CapacityTest: return "capacity test";
    case Step::Done: return "done";
  }
  return "?";
}

const Status& status() noexcept { return g_status; }

const Status& retest(bool full) noexcept {
  if (g_status.reached < Step::WiringTest) {
    return g_status;  // the controller never came up: nothing to test
  }
  const bool cached = boot::dcache_enabled();
  if (cached) {
    boot::dcache_disable();
  }
  g_status.failed_command = 0;
  run_tests(full);
  if (cached) {
    boot::dcache_enable();
  }
  return g_status;
}

}  // namespace o3os::platform::rt10xx::sdram

namespace o3os::platform::rt10xx::boot {

void init_sdram() noexcept {
  using regs::at;
  namespace semc = o3os::platform::rt10xx::semc;
  sdram::Status& s = sdram::g_status;
  s = sdram::Status{};

  // The clock: the periph clock (the core's) divided by four, and the SEMC's clock gate.
  s.reached = sdram::Step::Clock;
  regs::ccm_cbcdr() = (regs::ccm_cbcdr() & ~(regs::kCbcdrSemcClkSel | regs::kCbcdrSemcAltClkSel | regs::kCbcdrSemcPodfMask)) | (semc::kSemcPodf << regs::kCbcdrSemcPodfShift);
  if (!sdram::wait_until([] { return (regs::ccm_cdhipr() & regs::kCdhiprSemcPodfBusy) == 0u; })) {
    return;
  }
  regs::ccm_ccgr3() = regs::ccm_ccgr3() | regs::kCcgr3Semc;
  regs::ccm_ccgr4() = regs::ccm_ccgr4() | regs::kCcgr4Iomuxc;

  // The pads: EMC_00..39 to their SEMC function, with the SEMC pad word.
  s.reached = sdram::Step::Pads;
  for (unsigned pad = semc::kFirstPad; pad < semc::kFirstPad + semc::kPadCount; ++pad) {
    at<std::uint32_t>(semc::kMuxCtlEmc00 + 4u * pad) = semc::kMuxSemc;
    at<std::uint32_t>(semc::kPadCtlEmc00 + 4u * pad) = semc::kPadWord;
  }

  // The controller: a software reset, then its words, the base register, the SDRAM timing words (refresh off), the IP command port.
  s.reached = sdram::Step::Controller;
  at<std::uint32_t>(semc::kMcr) = semc::kMcrSwrst;
  if (!sdram::wait_until([] { return (at<std::uint32_t>(semc::kMcr) & semc::kMcrSwrst) == 0u; })) {
    return;
  }
  at<std::uint32_t>(semc::kMcr) = semc::kMcrWord;  // MDIS clear: the controller runs
  at<std::uint32_t>(semc::kBmcr0) = semc::kBmcr0Word;
  at<std::uint32_t>(semc::kBmcr1) = semc::kBmcr1Word;
  at<std::uint32_t>(semc::kIocr) = semc::kIocrWord;
  for (unsigned i = 1; i < 9u; ++i) {
    at<std::uint32_t>(semc::kBr0 + 4u * i) = 0u;  // the other base registers: not valid
  }
  at<std::uint32_t>(semc::kBr0) = semc::kBr0Word;
  at<std::uint32_t>(semc::kSdramcr0) = semc::kSdramcr0Word;
  at<std::uint32_t>(semc::kSdramcr1) = semc::kSdramcr1Word;
  at<std::uint32_t>(semc::kSdramcr2) = semc::kSdramcr2Word;
  at<std::uint32_t>(semc::kSdramcr3) = semc::kSdramcr3Word;
  at<std::uint32_t>(semc::kIpcr0) = semc::kIpcr0Word;
  at<std::uint32_t>(semc::kIpcr1) = semc::kIpcr1Word;
  at<std::uint32_t>(semc::kIpcr2) = semc::kIpcr2Word;
  if (!sdram::wait_until([] { return (at<std::uint32_t>(semc::kSts0) & semc::kSts0Idle) != 0u; })) {
    return;
  }
  delay_us(semc::kPowerUpDelayUs);  // the part sees its clock from the pads step: the JEDEC stabilisation wait

  // The JEDEC sequence through the IP command port.
  s.reached = sdram::Step::PrechargeAll;
  if (!sdram::ip_command(semc::kCmdPrechargeAll)) {
    return;
  }
  s.reached = sdram::Step::AutoRefresh;
  for (unsigned i = 0; i < semc::kInitRefreshes; ++i) {
    if (!sdram::ip_command(semc::kCmdAutoRefresh)) {
      return;
    }
  }
  s.reached = sdram::Step::ModeSet;
  at<std::uint32_t>(semc::kIptxdat) = semc::kModeRegister;
  if (!sdram::ip_command(semc::kCmdModeSet)) {
    return;
  }
  s.reached = sdram::Step::RefreshOn;
  at<std::uint32_t>(semc::kSdramcr3) = semc::kSdramcr3Word | semc::kSdramcr3Ren;
  asm volatile("dsb" ::: "memory");

  // The tests (the data cache is still off: startup.cpp enables it after this step).
  sdram::run_tests(O3OS_RT10XX_SDRAM_FULL_TEST != 0);
}

}  // namespace o3os::platform::rt10xx::boot
