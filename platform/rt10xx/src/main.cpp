// SPDX-License-Identifier: GPL-3.0-or-later
//
// The probe image's main (OS-PORT-RT10XX phases 1 to 3): the reset sequence (startup.cpp) has assigned the FlexRAM, set the vector table,
// stopped the watchdogs, brought up the MPU, the clock tree, the SDRAM (its outcome recorded), the caches, the eDMA and SysTick; main starts
// the log, brings the USB port up with the `diag` console (usb/src/usb1.cpp, console.cpp) and idles in WFI, servicing the console and a
// heartbeat. The audio path (src/audio.cpp) and the DIN port (src/midi_din.cpp) are in the image but run only on the console's `audio
// start` and `midi start` (their pads are assumed: audio_pins.h, midi_pins.h); there is no display, card or input yet: the later phases of
// docs/specs/OS-PORT-RT10XX.md. What this image proves on the unit: the boot path completes, the unit stays up, a computer sees the CDC
// port and the console answers; `sdram`, `cpu`, `crash`, `audio` and `midi` then say what the hardware did.
//
// O3OS_RT10XX_FAULT_DRILL: 10 s after power-up, one deliberate UsageFault (an undefined instruction), to prove the crash record round
// trip once an image can read it back. OFF in the default image.
#include "internal.h"
#include "regs.h"

#include <o3os/platform/rt10xx/audio.h>
#include <o3os/platform/rt10xx/display.h>
#include <o3os/platform/rt10xx/mem_map.h>
#include <o3os/platform/rt10xx/platform.h>
#include <o3os/platform/rt10xx/sdram.h>
#include <o3os/platform/rt10xx/sections.h>
#include <o3os/platform/rt10xx/usb/console.h>
#include <o3os/platform/rt10xx/usb/usb1.h>

#include <cstdint>

#ifndef O3OS_RT10XX_FAULT_DRILL
#define O3OS_RT10XX_FAULT_DRILL 0
#endif
#ifndef O3OS_RT10XX_AUDIO_AUTOSTART
#define O3OS_RT10XX_AUDIO_AUTOSTART 0
#endif

namespace rt10xx = o3os::platform::rt10xx;

namespace {

// Seconds since boot, counted by the main loop; the loop is the only writer. For a debugger, and for `diag` once there is one.
volatile std::uint32_t g_heartbeat = 0;
// The boot report of this run, kept where a debugger (or the next phase's console) can find it.
volatile std::uint32_t g_reset_sources = 0;
volatile std::uint32_t g_boot_crash_count = 0;

// The module id the drill's fault carries, outside any id a loader hands out (the MK66's drill uses 0xD1 too).
constexpr std::uint16_t kDrillModule = 0xD1;
constexpr std::uint32_t kDrillAfterMs = 10'000;

// A function that runs from the ITCM: if the heartbeat keeps counting, `.ramfunc_l` was copied from flash and runs there (the first
// hardware session reads the counter through the next phase's console, or a debugger).
O3OS_RAMFUNC_L std::uint32_t count_from_itcm(std::uint32_t value) noexcept { return value + 1u; }

}  // namespace

int main() {
  namespace console = rt10xx::console;
  console::init_log();
  console::log("boot: main reached");
  g_reset_sources = rt10xx::boot_info().reset_sources;
  console::log_hex("boot: reset sources", g_reset_sources);
  if (const rt10xx::crash::CrashRecord* record = rt10xx::boot_crash_record()) {
    g_boot_crash_count = record->count;
    console::log_hex("boot: previous run crashed, exception", record->exception);
    // Kept, not cleared: `crash` shows it, `crash clear` erases it. The record's count keeps growing across crashes (ADR 0002's boot counter).
  }
  {
    const rt10xx::sdram::Status& s = rt10xx::sdram::status();
    console::log(s.ok ? "boot: sdram OK" : "boot: sdram NOT OK");
    console::log_dec("boot: sdram step reached", static_cast<std::uint32_t>(s.reached));
    console::log_dec("boot: sdram tests ms", s.test_ms);
  }
  if (rt10xx::usb1::start()) {
    console::log("boot: usb attached");
  } else {
    console::log(rt10xx::usb1::start_failure());
    console::log("boot: usb start FAILED");
  }
#if O3OS_RT10XX_AUDIO_AUTOSTART
  // Phase 3, the A/B image: the audio path at boot (muted), as the MK66 image has it. The default image waits for `audio start`: the pads
  // of audio_pins.h are assumed, and the console must be up to read what happened.
  if (rt10xx::audio_device().start(rt10xx::audio_engine().callback()) == o3os::hal::audio::Device::Result::Ok) {
    console::log("boot: audio started, outputs muted");
  } else {
    console::log_dec("boot: audio start FAILED, result", rt10xx::audio_last_result());
  }
#endif

  std::uint64_t next_second = rt10xx::system_clock().uptime_ms() + 1000u;
  for (;;) {
    asm volatile("wfi");
    console::service();
    rt10xx::audio_main_pass();
    rt10xx::display::service();  // phase 4: the swap and the backlight ramp, once `display start` has run  // the main loop's beat for the pipeline's starvation guard (off here; the beat costs one store)
    const std::uint64_t now = rt10xx::system_clock().uptime_ms();
    if (now >= next_second) {
      next_second += 1000u;
      g_heartbeat = count_from_itcm(g_heartbeat);
#if O3OS_RT10XX_FAULT_DRILL
      if (now >= kDrillAfterMs) {
        rt10xx::set_module(kDrillModule);
        asm volatile("udf #0");  // UsageFault: undefined instruction
      }
#endif
    }
  }
}
