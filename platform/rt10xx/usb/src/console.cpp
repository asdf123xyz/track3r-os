// SPDX-License-Identifier: GPL-3.0-or-later
//
// The `diag` console of the rt10xx image (OS-PORT-RT10XX phase 2): the shell of usb/diag_shell.h on the CDC port of usb1.cpp, and the
// commands of this platform's probe. Everything here runs in the main loop; the shell never blocks (its output is a ring that drops its
// oldest bytes when the host does not read).
//
// What each command reads (nothing here writes hardware but `reset`, `drill`, `pins gpio .. high|low`, `sdram test` and `peek`'s reads):
//   help      the table
//   version   the build identity (CMake writes it per build), the USB identity and serial
//   stage     the crash context's stage and module, uptime, why the chip reset (SRC_SRSR as read at boot)
//   crash     the record the previous run left (kept at boot); `crash clear` erases it
//   mem       the memory map and what the image uses of each region
//   sdram     the SDRAM bring-up's outcome (the step reached, the tests, their time); `sdram test [full]` runs the tests again, every word with `full`
//   cpu       the core clock: the cycle counter's advance per USB frame (the host's millisecond), SysTick's reload, the clock tree's numbers
//   usb       the driver's counters and the control pipe's state
//   log       log [count]: the last lines boot code and drivers wrote
//   pins      pins: the five GPIO ports' input levels and directions; `pins mux <pad>`: a pad's mux and pad words (pad 0..123 in the IOMUXC order);
//             `pins gpio <port> <bit> [in|high|low]`: that GPIO's pad as a GPIO (ALT5), read, or driven. A probing tool for a board whose
//             LEDs, switches and buses are not known yet (the spec's open questions). GPIO5 has no IOMUXC pad: refused.
//   peek      peek <hexaddr> [1..16]: 32-bit reads of the modules and memories this image has clocked on (an address outside them is refused)
//   drill     one deliberate UsageFault now: the crash record round trip (the unit resets; `crash` shows it after)
//   reset     SYSRESETREQ
#include <o3os/platform/rt10xx/usb/console.h>

#include "../../src/internal.h"
#include "../../src/regs.h"

#include <o3os/audio/diag.h>
#include <o3os/core/qtime.h>
#include <o3os/midi/din.h>
#include <o3os/midi/in.h>
#include <o3os/midi/message.h>
#include <o3os/midi/time_ref.h>
#include <o3os/platform/rt10xx/audio.h>
#include <o3os/platform/rt10xx/midi.h>
#include <o3os/platform/rt10xx/midi_config.h>
#include <o3os/platform/mk66/usb/diag_text.h>
#include <o3os/platform/mk66/usb/log_command.h>
#include <o3os/platform/rt10xx/clock_config.h>
#include <o3os/platform/rt10xx/mem_map.h>
#include <o3os/platform/rt10xx/platform.h>
#include <o3os/platform/rt10xx/sdram.h>
#include <o3os/platform/rt10xx/semc_config.h>
#include <o3os/platform/rt10xx/usb/soc_facts.h>
#include <o3os/platform/rt10xx/usb/usb1.h>
#include <o3os/usb/diag_log.h>
#include <o3os/usb/diag_pump.h>
#include <o3os/usb/diag_shell.h>
#include <o3os/usb/identity.h>

#include "o3os_build_info.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" {
extern std::uint32_t __ramfunc_l_start[];
extern std::uint32_t __ramfunc_l_end[];
extern std::uint32_t __tapmem_l_start[];
extern std::uint32_t __tapmem_l_end[];
extern std::uint32_t __noinit_start[];
extern std::uint32_t __bss_end[];
extern std::uint32_t __ocram_static_start[];
extern std::uint32_t __ocram_static_end[];
extern std::uint32_t __image_end[];
}

namespace o3os::platform::rt10xx::console {
namespace {

namespace shell = o3os::usb::diag;
namespace text = o3os::platform::mk66::console;  // the MK66's formatters of the crash record, the stage line, the version line, and its `log` command

// ---- The log: 64 records in the DTCM (4 KiB; the SDRAM is not trusted with it until phase 3) ----------------------------------------
constexpr std::uint32_t kLogRecords = 64;
shell::LogRecord g_log_records[kLogRecords];
shell::LogRing g_log;
static_assert(sizeof(shell::LogRecord) == 64 && (kLogRecords & (kLogRecords - 1u)) == 0u, "a power-of-two number of 64-byte records");

constexpr char kBanner[] = "Track3r diag - o3os " O3OS_BUILD_LABEL " (rt10xx probe, phase 3) - 'help' lists the commands";

// ---- Output ----------------------------------------------------------------------------------------------------------------------------
void sink_write(void*, const char* data, std::size_t n) { usb1::cdc_writer().write(data, n); }
void sink_flush(void*) { usb1::cdc_writer().flush(); }
std::size_t sink_room(void*) { return usb1::cdc_writer().room_bytes(); }

constexpr std::size_t kTextBuffer = 1024;
extern bool g_midi_harness;  // the `midi` harness (below) is up: its time base must be republished every pass

void send(shell::Shell& sh, std::size_t n, const char* buffer) { sh.out().text(buffer, n); }

bool same(const char* a, const char* b) {
  while (*a != '\0' && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}

bool parse_u32(const char* s, std::uint32_t& out, unsigned base = 10) {
  std::uint32_t v = 0;
  if (*s == '\0') {
    return false;
  }
  if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    s += 2;
  }
  for (; *s != '\0'; ++s) {
    unsigned d;
    if (*s >= '0' && *s <= '9') {
      d = static_cast<unsigned>(*s - '0');
    } else if (base == 16 && *s >= 'a' && *s <= 'f') {
      d = static_cast<unsigned>(*s - 'a') + 10u;
    } else if (base == 16 && *s >= 'A' && *s <= 'F') {
      d = static_cast<unsigned>(*s - 'A') + 10u;
    } else {
      return false;
    }
    if (d >= base) {
      return false;
    }
    v = v * base + d;
  }
  out = v;
  return true;
}

std::uint32_t uptime_ms32() { return static_cast<std::uint32_t>(system_clock().uptime_ms()); }

// ---- Commands ------------------------------------------------------------------------------------------------------------------------
void cmd_help(shell::Shell& sh, const shell::Args&) {
  for (std::size_t i = 0; i < sh.command_count(); ++i) {
    const shell::Command& c = sh.commands()[i];
    sh.out().text(c.name);
    std::size_t len = 0;
    while (c.name[len] != '\0') {
      ++len;
    }
    for (; len < 8u; ++len) {
      sh.out().chr(' ');
    }
    sh.out().text(c.help).nl();
  }
}

void cmd_version(shell::Shell& sh, const shell::Args&) {
  char buf[kTextBuffer];
  send(sh, text::format_version(O3OS_BUILD_LABEL " rt10xx", o3os::usb::kVendorId, o3os::usb::kProductId, o3os::usb::kDeviceBcdStandard, usb1::serial(), buf, sizeof buf), buf);
}

void cmd_stage(shell::Shell& sh, const shell::Args&) {
  char buf[kTextBuffer];
  const BootInfo& info = boot_info();
  send(sh, text::format_stage(boot::load_context(), uptime_ms32(), static_cast<std::uint16_t>(info.reset_sources), info.crash_record_found, 0u, buf, sizeof buf), buf);
  sh.out().text("reset sources (SRC_SRSR at boot) ").hex(info.reset_sources).text(": ");
  if ((info.reset_sources & kResetPower) != 0u) sh.out().text("power-on ");
  if ((info.reset_sources & kResetLockupOrSoftware) != 0u) sh.out().text("lockup-or-software ");
  if ((info.reset_sources & kResetPin) != 0u) sh.out().text("pin ");
  if ((info.reset_sources & kResetWatchdog) != 0u) sh.out().text("wdog1 ");
  if ((info.reset_sources & kResetRtWatchdog) != 0u) sh.out().text("rtwdog ");
  if ((info.reset_sources & kResetTemperature) != 0u) sh.out().text("temperature ");
  sh.out().nl();
}

void cmd_crash(shell::Shell& sh, const shell::Args& a) {
  if (a.argc >= 2u && same(a.argv[1], "clear")) {
    clear_crash_record();
    sh.out().text("OK crash record cleared\n");
    return;
  }
  char buf[kTextBuffer];
  send(sh, text::format_crash(boot_crash_record(), buf, sizeof buf), buf);
}

void region_line(shell::Shell& sh, const char* name, std::uint32_t base, std::uint32_t size, std::uint32_t used) {
  sh.out().text(name);
  std::size_t len = 0;
  while (name[len] != '\0') {
    ++len;
  }
  for (; len < 14u; ++len) {
    sh.out().chr(' ');
  }
  sh.out().hex(base).text("  ").dec(size / 1024u).text(" KiB  used ").dec(used).text(" B\n");
}

std::uint32_t span(const void* a, const void* b) {
  return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(b) - reinterpret_cast<std::uintptr_t>(a));
}

void cmd_mem(shell::Shell& sh, const shell::Args&) {
  sh.out().text("region        base        size       use\n");
  region_line(sh, "flash image", mem::kImageBase, mem::kImageMaxSize, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(__image_end)) - mem::kImageBase);
  region_line(sh, "itcm hot", mem::kItcmBase, mem::kRamfuncSize, span(__ramfunc_l_start, __ramfunc_l_end) + span(__tapmem_l_start, __tapmem_l_end));
  region_line(sh, "dtcm", mem::kDtcmBase, mem::kDtcmSize, span(__noinit_start, __bss_end));
  region_line(sh, "ocram", mem::kOcramBase, mem::kOcramSize, span(__ocram_static_start, __ocram_static_end));
  region_line(sh, "sdram", mem::kSdramBase, mem::kSdramSize, 0);
  sh.out().text("dtcm free between .bss and the stack: ").dec(mem::kStackTop - mem::kStackSize - static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(__bss_end))).text(" B, stack ").dec(mem::kStackSize).text(" B\n");
  sh.out().text("sdram regions: ");
  for (unsigned i = 0; i < mem::kSdramRegionCount; ++i) {
    sh.out().text(mem::kSdramRegions[i].name).chr(' ').dec(mem::kSdramRegions[i].size / 1024u).text(i + 1u < mem::kSdramRegionCount ? ", " : "\n");
  }
}

void result_line(shell::Shell& sh, const char* name, const sdram::memtest::Result& r) {
  sh.out().text(name).text(r.ok ? ": pass\n" : ": FAIL at word ");
  if (!r.ok) {
    sh.out().dec(r.index).text(" (").hex(mem::kSdramBase + 4u * r.index).text(") expected ").hex(r.expected).text(" got ").hex(r.actual).nl();
  }
}

void sdram_report(shell::Shell& sh, const sdram::Status& s) {
  sh.out().text("sdram: ").text(s.ok ? "OK" : "NOT OK").text(", reached ").text(sdram::step_name(s.reached)).nl();
  if (s.failed_command != 0u) {
    sh.out().text("  ip command ").hex(s.failed_command, 1).text(" failed, INTR ").hex(s.intr).nl();
  }
  if (s.reached >= sdram::Step::WiringTest) {
    sh.out().text("  ");
    result_line(sh, "wiring", s.wiring);
  }
  if (s.reached >= sdram::Step::CapacityTest) {
    sh.out().text("  ");
    result_line(sh, s.full_test ? "capacity (every word)" : "capacity (every 256th word)", s.capacity);
    sh.out().text("  tests took ").dec(s.test_ms).text(" ms\n");
  }
  sh.out().text("  part assumed: ").text(semc::kPart.name).text(", SEMC ").dec(semc::kClockHz / 1000000u).text(" MHz\n");
}

void cmd_sdram(shell::Shell& sh, const shell::Args& a) {
  if (a.argc >= 2u && same(a.argv[1], "test")) {
    if (g_midi_harness) {  // the tests block for seconds: the MIDI harness's time base would go stale (service() republishes it every pass)
      sh.out().text("sdram: refused while the MIDI harness is up (`midi start` was run): reset the unit first\n");
      return;
    }
    const bool full = a.argc >= 3u && same(a.argv[2], "full");
    sh.out().text(full ? "testing every word (several seconds)...\n" : "testing (sparse)...\n");
    sh.out().flush();
    sdram_report(sh, sdram::retest(full));
    return;
  }
  sdram_report(sh, sdram::status());
}

void cmd_cpu(shell::Shell& sh, const shell::Args& a) {
  if (a.argc == 2 && same(a.argv[1], "reset")) {  // the per-run figures and the fault latch (audio/status.h reset_peaks)
    audio_cpu_reset();
    sh.out().text("cpu: figures and the fault latch reset\n");
    return;
  }
  const auto& c = usb1::counters();
  sh.out().text("core clock configured ").dec(clock::kCoreHz / 1000000u).text(" MHz (IPG ").dec(clock::kIpgHz / 1000000u).text(", PERCLK ").dec(clock::kPerclkHz / 1000000u).text("), SysTick reload ").dec(clock::kSysTickReload).nl();
  sh.out().text("cycles per USB frame (the host's 1 ms): ").dec(c.sof_cycles).text(" -> about ").dec(c.sof_cycles / 1000u).text(" MHz measured against the host\n");
  sh.out().text("uptime ").dec(uptime_ms32()).text(" ms, cycle counter ").hex(regs::dwt_cyccnt()).nl();
  if (audio_running()) {  // phase 3: the audio path's figures (audio/diag.h, the MK66's `cpu` lines) once blocks run
    char buf[kTextBuffer];
    send(sh, o3os::audio::diag::format_cpu(audio_status().snapshot(), hal::audio::kCyclesPerBlock, buf, sizeof buf), buf);
  }
}

void cmd_usb(shell::Shell& sh, const shell::Args&) {
  const auto& c = usb1::counters();
  const auto& p = usb1::control();
  sh.out().text("attached ").dec(usb1::attached() ? 1u : 0u).text(usb1::attached() ? "" : usb1::start_failure()).text(", state ").dec(static_cast<std::uint32_t>(p.state())).text(" (0 default, 1 address, 2 configured), address ").dec(p.address()).text(", dtr ").dec(p.cdc().dtr() ? 1u : 0u).text(", suspended ").dec(p.suspended() ? 1u : 0u).nl();
  sh.out().text("isr ").dec(c.isr_count).text(" cycles last ").dec(c.isr_cycles_last).text(" max ").dec(c.isr_cycles_max).text(", resets ").dec(c.bus_resets).text(" late ").dec(c.late_resets).text(", port changes ").dec(c.port_changes).text(" speed ").dec(c.speed).text(", sofs ").dec(c.sofs).nl();
  sh.out().text("setups ").dec(c.setups).text(" rereads ").dec(c.setup_rereads).text(" ep0 stalls ").dec(c.ep0_stalls).text(", pipe stalls ").dec(p.counters().stalls).text(", suspends ").dec(c.suspends).text(" resumes ").dec(c.resumes).nl();
  sh.out().text("cdc tx ").dec(c.cdc_tx_packets).text(" packets ").dec(c.cdc_tx_bytes).text(" B, rx ").dec(c.cdc_rx_packets).text(" packets ").dec(c.cdc_rx_bytes).text(" B, holds ").dec(c.cdc_rx_holds).text(", writer dropped ").dec(usb1::cdc_writer().dropped_chunks()).nl();
  sh.out().text("midi in ").dec(c.midi_in_packets).text(" out ").dec(c.midi_out_packets).text(" events ").dec(c.midi_out_events).text(", errors ").dec(c.errors).text(" system ").dec(c.system_errors).text(" descriptor ").dec(c.descriptor_errors).text(" stray ").dec(c.stray_completions).nl();
  sh.out().text("completions");
  for (unsigned s = 0; s < o3os::platform::rt10xx::usb::kDirections; ++s) {
    sh.out().chr(' ').dec(c.completions[s]);
  }
  sh.out().nl();
}

void cmd_log(shell::Shell& sh, const shell::Args& a) { text::run_log_command(sh, g_log, a); }

// ---- pins: the GPIO ports and the IOMUXC pads ------------------------------------------------------------------------------------------
constexpr std::uintptr_t kGpioBase[5] = {0x401B8000u, 0x401BC000u, 0x401C0000u, 0x401C4000u, 0x400C0000u};
constexpr unsigned kPadCount = 124;  // EMC_00..41, AD_B0_00..15, AD_B1_00..15, B0_00..15, B1_00..15, SD_B0_00..05, SD_B1_00..11
constexpr std::uintptr_t kMuxCtl0 = 0x401F8014u;
constexpr std::uintptr_t kPadCtl0 = 0x401F8204u;
constexpr std::uint32_t kMuxGpio = 5u;

// The pad (in the IOMUXC order) of a GPIO: 1 is AD_B0 (bits 0..15) and AD_B1 (16..31); 2 is B0 and B1; 3 is SD_B1 (0..11), SD_B0 (12..17), EMC_32..41 (18..27); 4 is EMC_00..31.
bool pad_of_gpio(unsigned port, unsigned bit, unsigned& pad) {
  if (bit >= 32u) {
    return false;
  }
  switch (port) {
    case 1: pad = 42u + bit; return true;
    case 2: pad = 74u + bit; return true;
    case 3:
      if (bit < 12u) { pad = 112u + bit; return true; }
      if (bit < 18u) { pad = 106u + (bit - 12u); return true; }
      if (bit < 28u) { pad = 32u + (bit - 18u); return true; }
      return false;
    case 4: pad = bit; return true;
    default: return false;
  }
}

void gpio_clock_on() {
  // The five ports' clock gates (CCGR1 CG13 gpio1, CCGR0 CG15 gpio2, CCGR2 CG13 gpio3, CCGR3 CG6 gpio4, CCGR1 CG15 gpio5): on.
  regs::at<std::uint32_t>(regs::kCcm + 0x6Cu) |= (0x3u << 30);  // CCGR0: gpio2
  regs::at<std::uint32_t>(regs::kCcm + 0x70u) |= (0x3u << 26) | (0x3u << 30);  // CCGR1: gpio1, gpio5
  regs::at<std::uint32_t>(regs::kCcm + 0x74u) |= (0x3u << 12);  // CCGR3: gpio4
  regs::at<std::uint32_t>(regs::kCcm + 0x78u - 0x4u) |= (0x3u << 26);  // CCGR2: gpio3
}

void cmd_pins(shell::Shell& sh, const shell::Args& a) {
  gpio_clock_on();
  if (a.argc == 1u) {
    for (unsigned port = 1; port <= 5u; ++port) {
      const std::uintptr_t base = kGpioBase[port - 1u];
      sh.out().text("gpio").dec(port).text(" levels ").hex(regs::at<std::uint32_t>(base + 0x08u)).text(" dir ").hex(regs::at<std::uint32_t>(base + 0x04u)).text(" out ").hex(regs::at<std::uint32_t>(base + 0x00u)).nl();
    }
    return;
  }
  if (same(a.argv[1], "mux") && a.argc >= 3u) {
    std::uint32_t pad;
    if (!parse_u32(a.argv[2], pad) || pad >= kPadCount) {
      sh.out().text("ERR pad 0..123\n");
      return;
    }
    sh.out().text("pad ").dec(pad).text(" mux ").hex(regs::at<std::uint32_t>(kMuxCtl0 + 4u * pad)).text(" pad ").hex(regs::at<std::uint32_t>(kPadCtl0 + 4u * pad)).nl();
    return;
  }
  if (same(a.argv[1], "gpio") && a.argc >= 4u) {
    std::uint32_t port, bit;
    unsigned pad;
    if (!parse_u32(a.argv[2], port) || !parse_u32(a.argv[3], bit) || !pad_of_gpio(port, bit, pad)) {
      sh.out().text("ERR usage: pins gpio <1..4> <bit> [in|high|low] (GPIO5 has no IOMUXC pad)\n");
      return;
    }
    const std::uintptr_t base = kGpioBase[port - 1u];
    const char* mode = a.argc >= 5u ? a.argv[4] : "in";
    regs::at<std::uint32_t>(kMuxCtl0 + 4u * pad) = kMuxGpio;
    if (same(mode, "in")) {
      regs::at<std::uint32_t>(base + 0x04u) &= ~(1u << bit);
    } else if (same(mode, "high") || same(mode, "low")) {
      if (same(mode, "high")) {
        regs::at<std::uint32_t>(base + 0x00u) |= (1u << bit);
      } else {
        regs::at<std::uint32_t>(base + 0x00u) &= ~(1u << bit);
      }
      regs::at<std::uint32_t>(base + 0x04u) |= (1u << bit);
    } else {
      sh.out().text("ERR in|high|low\n");
      return;
    }
    sh.out().text("gpio").dec(port).chr('.').dec(bit).text(" pad ").dec(pad).text(" level ").dec((regs::at<std::uint32_t>(base + 0x08u) >> bit) & 1u).nl();
    return;
  }
  sh.out().text("ERR usage: pins | pins mux <pad> | pins gpio <port> <bit> [in|high|low]\n");
}

// ---- peek: reads inside the modules and memories this image has clocked on -------------------------------------------------------------------
struct Range {
  std::uint32_t base;
  std::uint32_t size;
};
constexpr Range kPeekRanges[] = {
    {mem::kItcmBase, mem::kItcmSize}, {mem::kDtcmBase, mem::kDtcmSize}, {mem::kOcramBase, mem::kOcramSize}, {mem::kImageBase, mem::kImageMaxSize},
    {0x401F8000u, 0x4000u},  // IOMUXC
    {0x400FC000u, 0x4000u},  // CCM
    {0x400D8000u, 0x4000u},  // CCM_ANALOG, PMU
    {0x400D9000u, 0x1000u},  // USBPHY1
    {0x402E0000u, 0x4000u},  // USB
    {0x402F0000u, 0x4000u},  // SEMC
    {0x400F8000u, 0x4000u},  // SRC
    {0x40080000u, 0x4000u},  // DCDC
    {0x401F4000u, 0x4000u},  // OCOTP
    {0x400AC000u, 0x4000u},  // IOMUXC_GPR
    {0x401B8000u, 0x10000u}, // GPIO1..4
    {0x400C0000u, 0x4000u},  // GPIO5
    {0xE000E000u, 0x1000u},  // the system control space
};

bool peekable(std::uint32_t address, std::uint32_t bytes) {
  for (const Range& r : kPeekRanges) {
    if (address >= r.base && address + bytes <= r.base + r.size) {
      return true;
    }
  }
  if (sdram::status().ok && address >= mem::kSdramBase && address + bytes <= mem::kSdramBase + mem::kSdramSize) {
    return true;
  }
  return false;
}

void cmd_peek(shell::Shell& sh, const shell::Args& a) {
  std::uint32_t address, count = 1;
  if (a.argc < 2u || !parse_u32(a.argv[1], address, 16) || (a.argc >= 3u && (!parse_u32(a.argv[2], count) || count == 0u || count > 16u))) {
    sh.out().text("ERR usage: peek <hexaddr> [1..16]\n");
    return;
  }
  if ((address & 3u) != 0u || !peekable(address, count * 4u)) {
    sh.out().text("ERR address not word aligned or outside the modules this image has clocked on\n");
    return;
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    sh.out().hex(address + 4u * i).text(": ").hex(regs::at<std::uint32_t>(address + 4u * i)).nl();
  }
}

// ---- `audio` (phase 3): the audio path on the maintainer's command. Nothing is driven before `audio start`; start() never unmutes
// (hal/audio.h H16); `audio unmute` opens the outputs only after a clean run of blocks, as the MK66's output gate does at boot.
constexpr std::uint32_t kCleanBlocksBeforeUnmute = 400;  // 0.58 s of boundaries without a fault (the MK66 main's settle count)

const char* audio_where_name(std::uint8_t where) {
  switch (where) {
    case AudioClockFault::PllLock: return "pll lock";
    case AudioClockFault::NoBoundaries: return "boundaries";
    default: return "none";
  }
}

void audio_status_lines(shell::Shell& sh) {
  const o3os::audio::StatusSnapshot snap = audio_status().snapshot();
  sh.out().text("audio ").text(audio_running() ? "running" : "stopped").text(", outputs ").text(audio_mute_failed() ? "MUTE FAILED (codec write): may be open" : audio_device().outputs_enabled() ? "open" : "muted")
      .text(", last result ").text(audio_result_name(audio_last_result())).nl();
  const AudioClockFault f = audio_clock_fault();
  if (f.where != AudioClockFault::None) {
    sh.out().text("  clock fault at ").text(audio_where_name(f.where)).text(": PLL_AUDIO ").hex(f.pll).text(" RCSR ").hex(f.rcsr).text(" TCSR ").hex(f.tcsr).nl();
  }
  sh.out().text("  boundaries ").dec(snap.boundaries).text(" renders ").dec(snap.renders).text(" underruns ").dec(snap.underruns).text(" hw faults ").dec(snap.hw_faults)
      .text(" slips ").dec(snap.slot_slips).text(" period ").dec(snap.period_min).text("..").dec(snap.period_max).text(" cycles (block ").dec(hal::audio::kCyclesPerBlock).text(")").nl();
}

void cmd_audio(shell::Shell& sh, const shell::Args& a) {
  if (a.argc == 1) {
    audio_status_lines(sh);
    return;
  }
  const char* sub = a.argv[1];
  if (same(sub, "start")) {
    if (audio_running()) {
      sh.out().text("audio: already running\n");
      return;
    }
    log("audio start: codec, clocks, DMA");
    const auto result = static_cast<std::uint8_t>(audio_device().start(audio_engine().callback()));
    sh.out().text("audio start: ").text(audio_result_name(result)).nl();
    if (audio_running()) {
      log("audio start: Ok, outputs muted");
    } else {
      log_dec("audio start: FAILED, result", result);
      const AudioClockFault f = audio_clock_fault();
      if (f.where != AudioClockFault::None) {
        log_hex("audio start: PLL_AUDIO", f.pll);
        log_hex("audio start: RCSR", f.rcsr);
      }
    }
    audio_status_lines(sh);
    return;
  }
  if (same(sub, "unmute")) {
    const o3os::audio::StatusSnapshot snap = audio_status().snapshot();
    if (!audio_running()) {
      sh.out().text("audio: not running (`audio start` first)\n");
      return;
    }
    if (snap.boundaries < kCleanBlocksBeforeUnmute || snap.fault_seen != 0u) {
      sh.out().text("audio: refused, the path has not run ").dec(kCleanBlocksBeforeUnmute).text(" clean blocks (fault seen: ").dec(snap.fault_seen).text("; `cpu reset` clears the latch)\n");
      return;
    }
    const bool ok = audio_device().unmute();  // blocks about 0.2 s: the codec's ramps
    sh.out().text(ok ? "audio: outputs open (quiet level)\n" : "audio: unmute FAILED, outputs muted\n");
    log(ok ? "audio unmute: outputs open" : "audio unmute: FAILED");
    return;
  }
  if (same(sub, "mute")) {
    audio_device().mute();
    sh.out().text(audio_mute_failed() ? "audio: mute FAILED, a codec write was not acknowledged: the outputs may still be open\n" : "audio: outputs muted\n");
    return;
  }
  if (same(sub, "tone") && a.argc == 3 && (same(a.argv[2], "on") || same(a.argv[2], "off"))) {
    if (!audio_running()) {
      sh.out().text("audio: not running\n");
      return;
    }
    const bool on = same(a.argv[2], "on");
    sh.out().text(audio_engine().set_tone(on) ? (on ? "audio: test tone on (audible once unmuted)\n" : "audio: test tone off\n") : "audio: tone request not taken (the parameter ring is full)\n");
    return;
  }
  if (same(sub, "stall")) {
    if (!audio_running()) {
      sh.out().text("audio: not running\n");
      return;
    }
    audio_engine().request_test_stall((hal::audio::kCyclesPerBlock * 7u) / 2u);  // 3.5 blocks: the ADR 0013 D1 underrun drill, muted by the pipeline
    sh.out().text("audio: one render stalled 3.5 blocks; `audio` shows the underruns and the mute\n");
    return;
  }
  if (same(sub, "stop")) {
    audio_stop();
    sh.out().text(audio_mute_failed() ? "audio: stopped (the I2S and the DMA), but a codec write FAILED: the outputs may still be open\n" : "audio: stopped, outputs muted\n");
    log(audio_mute_failed() ? "audio stop: codec write FAILED" : "audio stop");
    return;
  }
  sh.out().text("usage: audio [start | unmute | mute | tone on|off | stall | stop]\n");
}

// ---- `midi` (phase 3): the DIN port under the console. The emitter, the receive ring and the time base of the full OS belong to the
// sequencer's MidiLink (phase 5); until then this harness owns them: the console is the emitter's producer and the ring's consumer (the
// main thread, one context), and service() publishes a time base from the cycle counter so the emitter's interrupt has a timeline.
o3os::midi::DinEmitter g_midi_out;
o3os::midi::RtInRing g_midi_in;
o3os::midi::TimeRef g_midi_time;
std::uint32_t g_midi_last_cycles = 0;
std::uint64_t g_midi_cycles = 0;  // the harness's timeline: cycles since `midi start`
bool g_midi_harness = false;  // declared above
constexpr std::uint32_t kMidiClocksPerBeat = 24;
constexpr std::uint32_t kMidiBeatCycles = clock::kCoreHz / 2u;  // 120 BPM

// The timeline sample of the harness's cycle count (the inverse of core::cycles_from_samples: samples = cycles x den / num).
std::uint64_t midi_sample_of(std::uint64_t cycles) { return cycles * core::kRefCyclesPerSample.den / core::kRefCyclesPerSample.num; }

void midi_publish_time() {
  const std::uint32_t now = regs::dwt_cyccnt();
  g_midi_cycles += now - g_midi_last_cycles;
  g_midi_last_cycles = now;
  g_midi_time.publish(midi_sample_of(g_midi_cycles), now);
}

bool midi_now(std::uint64_t& out) {
  midi_publish_time();
  return g_midi_time.to_timeline(regs::dwt_cyccnt(), out);
}

void cmd_midi(shell::Shell& sh, const shell::Args& a) {
  if (a.argc >= 2 && same(a.argv[1], "start")) {
    if (!g_midi_harness) {
      g_midi_out.configure(o3os::midi::DinEmitter::Config{});
      g_midi_last_cycles = regs::dwt_cyccnt();
      g_midi_cycles = 0;
      midi_publish_time();
      g_midi_harness = true;
      midi_io::start(g_midi_out, g_midi_in, g_midi_time);
      log("midi start: LPUART5 and the PIT emitter up");
    }
    sh.out().text("midi: port up, 31,250 baud; `midi note`, `midi clock`, `midi` for the counters and received bytes\n");
    return;
  }
  if (a.argc >= 2 && (same(a.argv[1], "note") || same(a.argv[1], "clock"))) {
    std::uint64_t now = 0;
    if (!g_midi_harness || !midi_now(now)) {
      sh.out().text("midi: not started (`midi start` first)\n");
      return;
    }
    if (same(a.argv[1], "note")) {
      std::uint32_t note = 60;
      if (a.argc >= 3 && (!parse_u32(a.argv[2], note) || note > 127u)) {
        sh.out().text("usage: midi note [0..127]\n");
        return;
      }
      const std::uint8_t n = static_cast<std::uint8_t>(note & 0x7Fu);
      const std::uint64_t on_at = now + clock::kCoreHz / 1000u;  // 1 ms from now, then 100 ms of note
      const bool ok = g_midi_out.push_msg(on_at, o3os::midi::note_on(0, n, 100)) && g_midi_out.push_msg(on_at + clock::kCoreHz / 10u, o3os::midi::note_off(0, n));
      midi_io::kick();
      sh.out().text(ok ? "midi: note on, 100 ms, note off queued on channel 1\n" : "midi: the emitter's ring is full\n");
      return;
    }
    bool ok = true;
    for (std::uint32_t i = 0; i < kMidiClocksPerBeat && ok; ++i) {
      ok = g_midi_out.push_rt(now + clock::kCoreHz / 1000u + (static_cast<std::uint64_t>(kMidiBeatCycles) * i) / kMidiClocksPerBeat, 0xF8);
    }
    midi_io::kick();
    sh.out().text(ok ? "midi: 24 clock bytes queued, one beat at 120 BPM (20.833 ms apart)\n" : "midi: the real-time ring is full\n");
    return;
  }
  if (a.argc != 1) {
    sh.out().text("usage: midi [start | note [n] | clock]\n");
    return;
  }
  const o3os::midi::DinRxStats& rx = midi_io::rx_stats();
  const o3os::midi::DinTxStats& tx = midi_io::tx_stats();
  const o3os::midi::DinEmitter::Stats& em = g_midi_out.stats();
  sh.out().text("midi ").text(midi_io::started() ? "up" : "down").text(": rx isrs ").dec(rx.isrs).text(" bytes ").dec(rx.bytes).text(" errors ").dec(rx.errors).text(" overruns ").dec(rx.overruns)
      .text(" ring full ").dec(rx.ring_full).nl();
  sh.out().text("  tx irqs ").dec(tx.irqs).text(" bytes ").dec(tx.bytes).text(" rt ").dec(tx.rt_bytes).text(" no time ").dec(tx.no_time).text(" not ready ").dec(tx.not_ready)
      .text(" floor ").dec(tx.timer_floor).text(" rt late max ").dec(static_cast<std::uint32_t>(tx.rt_late_max / (clock::kCoreHz / 1000000u))).text(" us").nl();
  sh.out().text("  emitter msg bytes ").dec(em.msg_bytes).text(" rt sent ").dec(em.rt_sent).text(" inserted ").dec(em.rt_inserted).nl();
  // The bytes received since the last `midi`, with the gap before each (the loopback test: a DIN cable from OUT to IN shows the sent bytes here).
  o3os::midi::RtItem item{};
  unsigned shown = 0;
  std::uint32_t previous = 0;
  while (g_midi_in.pop(item)) {
    if (shown == 0) {
      sh.out().text("  received:");
    }
    if (shown < 32u) {
      sh.out().chr(' ').hex(item.byte, 2);
      if (shown != 0u) {
        sh.out().chr('+').dec((item.cycles - previous) / (clock::kCoreHz / 1000000u)).text("us");
      }
    }
    previous = item.cycles;
    ++shown;
  }
  if (shown != 0u) {
    if (shown > 32u) {
      sh.out().text(" ... ").dec(shown).text(" bytes");
    }
    sh.out().nl();
  }
}

void cmd_drill(shell::Shell& sh, const shell::Args&) {
  sh.out().text("raising a UsageFault now: the unit resets, `crash` shows the record after\n");
  sh.out().flush();
  usb1::kick();
  boot::delay_ms(50);  // the reply leaves
  set_module(0xD1);
  asm volatile("udf #0");
}

void cmd_reset(shell::Shell& sh, const shell::Args&) {
  sh.out().text("resetting\n");
  sh.out().flush();
  usb1::kick();
  boot::delay_ms(50);
  boot::reset_chip();
}

constexpr shell::Command kCommands[] = {
    shell::command("help", "list the commands", &cmd_help),
    shell::command("version", "build identity and USB identity", &cmd_version),
    shell::command("stage", "boot/run stage, uptime, why the chip reset", &cmd_stage),
    shell::command("crash", "crash [clear]: the crash record the previous run left", &cmd_crash),
    shell::command("mem", "memory map and what the image uses of each region", &cmd_mem),
    shell::command("sdram", "sdram [test [full]]: the SDRAM bring-up's outcome; run the tests again", &cmd_sdram),
    shell::command("cpu", "cpu [reset]: the core clock against the host's USB frames; the audio path's figures once it runs", &cmd_cpu),
    shell::command("audio", "audio [start | unmute | mute | tone on|off | stall | stop]: the audio path (phase 3)", &cmd_audio),
    shell::command("midi", "midi [start | note [n] | clock]: the DIN port's counters and received bytes; bring it up; send", &cmd_midi),
    shell::command("usb", "the USB driver's counters and the control pipe's state", &cmd_usb),
    shell::command("log", "log [count]: the last lines boot code and drivers wrote", &cmd_log),
    shell::command("pins", "pins | pins mux <pad> | pins gpio <port> <bit> [in|high|low]", &cmd_pins),
    shell::command("peek", "peek <hexaddr> [1..16]: read 32-bit words (clocked modules only)", &cmd_peek),
    shell::command("drill", "one deliberate UsageFault: the crash record round trip", &cmd_drill),
    shell::command("reset", "restart the unit (SYSRESETREQ)", &cmd_reset),
};

shell::Shell g_shell(kCommands, sizeof(kCommands) / sizeof(kCommands[0]), shell::Output(shell::Sink{&sink_write, &sink_flush, nullptr, &sink_room}));
shell::ConsolePump g_pump(g_shell, usb1::cdc_rx());
bool g_dtr_seen = false;

}  // namespace

void init_log() noexcept { g_log.init(g_log_records, kLogRecords); }

void log(const char* text_) noexcept { g_log.write(boot::uptime_ms_low(), text_); }

void log_dec(const char* text_, std::uint32_t value) noexcept {
  char buf[shell::kLogTextMax + 1];
  o3os::audio::diag::Text t(buf, sizeof buf);
  t.str(text_).chr(' ').dec(value);
  log(buf);
}

void log_hex(const char* text_, std::uint32_t value) noexcept {
  char buf[shell::kLogTextMax + 1];
  o3os::audio::diag::Text t(buf, sizeof buf);
  t.str(text_).chr(' ').hex32(value);
  log(buf);
}

void service() noexcept {
  if (!usb1::attached()) {
    return;
  }
  g_pump.service();
  const bool dtr = usb1::control().cdc().dtr();
  if (dtr && !g_dtr_seen) {
    g_shell.begin(kBanner);
  } else if (!dtr && g_dtr_seen) {
    g_shell.stop_stream();
  }
  g_dtr_seen = dtr;
  g_shell.tick();
  usb1::cdc_writer().flush();
  usb1::kick();
  if (g_midi_harness) {
    midi_publish_time();  // the emitter's time base: a publish must come within 3.58 s (TimeRef's +-2^31 cycles at 600 MHz), this runs every pass
  }
}

}  // namespace o3os::platform::rt10xx::console
