// SPDX-License-Identifier: GPL-3.0-or-later
//
// The reset-surviving state of the rt10xx platform layer: the crash record, the stage/module words a fault handler stamps into it, and
// the boot report built from them. One `.noinit` block at the bottom of the DTCM (0x2000_0000), which the C runtime never touches.
// Whether the FlexRAM holds through every reset source of this chip is an open hardware check; a record that does not validate is "no
// record". The record's pure functions are the MK66's (crash_record.cpp in platform_mk66_pure); the frame plausibility check is this
// chip's (mem_map.h in_ram), so the snapshot is assembled here.
#include "internal.h"
#include "regs.h"

#include <o3os/platform/mk66/crash_record.h>
#include <o3os/platform/rt10xx/mem_map.h>
#include <o3os/platform/rt10xx/platform.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace o3os::platform::rt10xx {
namespace {

constexpr std::size_t kRecordWords = sizeof(crash::CrashRecord) / sizeof(std::uint32_t);

struct alignas(8) NoinitBlock {
  volatile std::uint32_t record[kRecordWords];
  volatile std::uint32_t context;
  volatile std::uint32_t context_inverse;
  std::uint8_t reserved[mem::kCrashBlockSize - sizeof(crash::CrashRecord) - 2 * sizeof(std::uint32_t)];
};
static_assert(sizeof(NoinitBlock) == mem::kCrashBlockSize, "the block is the size mem_map.h reserves (and the linker asserts)");
static_assert(alignof(NoinitBlock) == 8, "declared alignment");
static_assert(sizeof(crash::CrashRecord) % sizeof(std::uint32_t) == 0, "the record is whole words");

[[gnu::section(".bss.noinit.crash"), gnu::used]] NoinitBlock g_block;

crash::CrashRecord g_boot_record;
BootInfo g_boot_info = {0, false};

// The exception frame (r0-r3, r12, lr, pc, xPSR) is read only if its address is word aligned and inside RAM that exists.
constexpr std::uint32_t kFrameWords = 8;
bool frame_plausible(std::uint32_t address) noexcept { return (address & 3u) == 0u && mem::in_ram(address, kFrameWords * 4u); }

}  // namespace

namespace boot {

crash::CrashRecord load_stored_record() noexcept {
  std::uint32_t words[kRecordWords];
  for (std::size_t i = 0; i < kRecordWords; ++i) {
    words[i] = g_block.record[i];
  }
  crash::CrashRecord r;
  std::memcpy(&r, words, sizeof r);
  return r;
}

void store_record(const crash::CrashRecord& r) noexcept {
  std::uint32_t words[kRecordWords];
  std::memcpy(words, &r, sizeof words);
  for (std::size_t i = 0; i < kRecordWords; ++i) {
    g_block.record[i] = words[i];
  }
}

std::uint32_t load_context() noexcept {
  const std::uint32_t context = g_block.context;
  return context == ~g_block.context_inverse ? context : crash::kUnknownContext;
}

void reset_context() noexcept {
  const std::uint32_t context = crash::make_context(0, crash::Stage::Reset);
  g_block.context = context;
  g_block.context_inverse = ~context;
}

void capture_boot_record() noexcept {
  const crash::CrashRecord stored = load_stored_record();
  if (crash::valid(stored)) {
    g_boot_record = stored;
    g_boot_info.crash_record_found = true;
  }
}

void capture_reset_cause() noexcept {
  const std::uint32_t sources = regs::src_srsr();
  g_boot_info.reset_sources = sources;
  regs::src_srsr() = sources;  // write-1-to-clear: the next boot sees only its own cause
}

[[noreturn]] void reset_chip() noexcept {
  asm volatile("dsb" ::: "memory");
  regs::scb_aircr() = regs::kAircrVectkey | regs::kAircrSysresetreq;
  asm volatile("dsb" ::: "memory");
  for (;;) {
    asm volatile("nop");
  }
}

[[noreturn]] void halt() noexcept {
  asm volatile("cpsid i" ::: "memory");
  for (;;) {
    asm volatile("wfi");
  }
}

}  // namespace boot

const BootInfo& boot_info() noexcept { return g_boot_info; }

const crash::CrashRecord* boot_crash_record() noexcept { return g_boot_info.crash_record_found ? &g_boot_record : nullptr; }

void clear_crash_record() noexcept {
  for (std::size_t i = 0; i < kRecordWords; ++i) {
    g_block.record[i] = 0;
  }
  g_boot_info.crash_record_found = false;
}

void set_stage(crash::Stage stage) noexcept {
  const std::uint32_t current = boot::load_context();
  const std::uint16_t module = current == crash::kUnknownContext ? std::uint16_t{0} : crash::module_of(current);
  const std::uint32_t next = crash::make_context(module, stage);
  g_block.context = next;
  g_block.context_inverse = ~next;
}

void set_module(std::uint16_t module) noexcept {
  const std::uint32_t current = boot::load_context();
  const crash::Stage stage = current == crash::kUnknownContext ? crash::Stage::Unknown : crash::stage_of(current);
  const std::uint32_t next = crash::make_context(module, stage);
  g_block.context = next;
  g_block.context_inverse = ~next;
}

namespace {

// Collects what a handler can read of the core's state, writes the record and leaves: a reset while the OS runs (stage Running), a halt
// while it boots (a boot that cannot complete must not loop; the next image to boot reports the record).
[[noreturn]] void record_and_leave(std::uint32_t exception, std::uint32_t exc_return, const std::uint32_t* frame,
                                   std::uint32_t pc_if_no_frame) noexcept {
  asm volatile("cpsid i" ::: "memory");
  const std::uint32_t context = boot::load_context();
  crash::FaultSnapshot s = {};
  s.exception = exception;
  s.exc_return = exc_return;
  s.cfsr = regs::scb_cfsr();
  s.hfsr = regs::scb_hfsr();
  s.mmfar = regs::scb_mmfar();
  s.bfar = regs::scb_bfar();
  s.context = context;
  s.uptime_ms = boot::uptime_ms_low();
  const std::uint32_t frame_address = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(frame));
  if (frame != nullptr && frame_plausible(frame_address)) {
    s.pc = frame[6];
    s.lr = frame[5];
    s.xpsr = frame[7];
    s.sp = frame_address;
  } else {
    s.pc = pc_if_no_frame;
    s.lr = pc_if_no_frame;
    s.xpsr = 0;
    s.sp = crash::kInvalidFrame;
  }
  boot::store_record(crash::encode(s, boot::load_stored_record()));
  const bool running = context != crash::kUnknownContext && crash::stage_of(context) == crash::Stage::Running;
  if (running) {
    boot::reset_chip();
  }
  boot::halt();
}

}  // namespace

extern "C" [[noreturn]] void o3os_fault_entry(const std::uint32_t* frame, std::uint32_t exc_return) {
  std::uint32_t ipsr;
  asm volatile("mrs %0, ipsr" : "=r"(ipsr));
  record_and_leave(ipsr & 0x1FFu, exc_return, frame, crash::kInvalidFrame);
}

[[noreturn]] void panic() noexcept {
  const auto caller = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));
  record_and_leave(crash::kPanicException, 0, nullptr, caller);
}

}  // namespace o3os::platform::rt10xx
