// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// When the outputs may open (OS-005d, OS-007g, OS-OUTON). Every image boots muted; main.cpp opens them (the codec's unmute, then the amplifier key) once the
// audio path has "run clean for 0.6 s", the tone image's own words. This is that rule as a pure object, so that it is tested on the host and on QEMU.
//
// OS-OUTON: the open happens once, at boot, and the outputs then stay open until power-off or a reset (the amplifier key's click is the one at power-on, not one at
// the first Play); open_wanted() below says when the main loop makes that attempt, OpenRetry what a failed one costs, and the Play, Song Play or live note that finds
// them closed keeps its old request as the fallback.
//
// The rule was "400 blocks since the start and no underrun and no hardware fault EVER": one late block or one DMA error anywhere in the uptime (the USB
// enumeration, the SD card's mount and its first long read, a display re-initialisation) kept the outputs shut for good, and a Play that was still waiting for them
// waited for ever: the badge said PLAY and nothing sounded. The rule is now what it says: the last `clean_blocks` boundaries had none. The main loop offers every
// status reading it takes (observe()), whether or not a Play is waiting, so the gate knows where the last fault was; with none since the start it is the old rule
// (ready from boundary `clean_blocks`), so a clean boot opens as fast as it did.
//
// Counters are the status snapshot's: free-running and cumulative since the audio started, compared for equality only. A reading whose boundary count went down
// is a restart of the audio path (`diag audio start`): the window starts over.

#include <cstdint>

namespace o3os::platform::mk66 {

class OutputGate {
 public:
  explicit constexpr OutputGate(std::uint32_t clean_blocks) noexcept : clean_(clean_blocks) {}

  // One status reading (boundaries, underruns and hardware faults of audio::StatusSnapshot). A change of either counter moves the start of the clean run to this
  // reading's boundary; a reading can be up to one poll period (about 70 blocks) later than the fault it sees, which only makes the gate later, never earlier.
  void observe(std::uint32_t boundaries, std::uint32_t underruns, std::uint32_t hw_faults) noexcept {
    if (boundaries < last_) {
      since_ = 0;  // the counters started again
      underruns_ = underruns;
      faults_ = hw_faults;
    } else if (underruns != underruns_ || hw_faults != faults_) {
      since_ = boundaries;
      underruns_ = underruns;
      faults_ = hw_faults;
    }
    last_ = boundaries;
  }

  // True when the last `clean_blocks` boundaries up to `boundaries` (a fresh reading's count) had no fault.
  constexpr bool ready(std::uint32_t boundaries) const noexcept { return boundaries >= since_ && boundaries - since_ >= clean_; }

  constexpr std::uint32_t clean_blocks() const noexcept { return clean_; }

 private:
  std::uint32_t clean_;
  std::uint32_t since_ = 0;      // the boundary the clean run started at
  std::uint32_t last_ = 0;       // the last reading's boundaries
  std::uint32_t underruns_ = 0;  // the counters at the last change
  std::uint32_t faults_ = 0;
};

// OS-OUTON: whether the main loop makes its attempt to open the outputs on this pass (the gate's ready() and OpenRetry::ready() are asked separately).
//   audio_up    the audio path is running (from boot, or from `diag audio start` after a boot failure, which then goes through the same gate)
//   opened      the outputs are open: set by the open, CLEARED by whatever mutes them (fw update's quiesce), so the open is attempted again only when that has ended
//   asked       a Play, a Song Play or a live note found them closed and waits (the fallback: the boot attempt did not happen yet or failed)
//   loading     the boot project load is still running (its settle, project, samples and modules phases; the SD card's mount runs before the main loop)
//   fw_busy     the firmware update path holds the card or is handing over (an upload, a check, the wait for the bootloader): never raise the amplifier then
//   powering_off  OS-POWER P1: the shutdown has stopped the engine and is stopping the audio path (appimg::power_off_latched(), set at its stage A and never cleared): NOTHING reopens the outputs,
//               whatever the other arguments are, a Play or a live note that waits included. It is its own flag: `opened` is cleared by every mute, so it cannot be the guard.
// At boot nobody has asked: the attempt waits for the load to end (the unmute blocks about 0.2 s, and must not sit on the loader's slices or the card's reads). A request
// that is already there does not wait for the load: it is the old first-Play path, and the Play it holds back is waiting for sound.
constexpr bool open_wanted(bool audio_up, bool opened, bool asked, bool loading, bool fw_busy, bool powering_off) noexcept {
  return !powering_off && audio_up && !opened && !fw_busy && (asked || !loading);
}

// OS-OUTON: what a failed unmute does to the main loop. The open now runs at every boot, so a codec write that is not acknowledged (the I2C bus is shared with the button
// scanner) must not stop the unit: the loop goes on (the display, the console and the LED code stay up), the next attempt waits kBackoffMs (the unmute blocks for up to 0.2 s,
// not every pass), and the boot's own attempts, those nobody asked for, stop after kAutoAttempts failures. A Play, a Song Play or a live note that finds the outputs closed
// (`asked`) is the lazy retry after that, still held back by the backoff; a success starts the count again.
class OpenRetry {
 public:
  static constexpr std::uint32_t kBackoffMs = 2000;
  static constexpr std::uint8_t kAutoAttempts = 3;

  constexpr bool ready(bool asked, std::uint32_t now_ms) const noexcept {
    if (failures_ == 0u) {
      return true;
    }
    if (!asked && failures_ >= kAutoAttempts) {
      return false;
    }
    return static_cast<std::int32_t>(now_ms - retry_at_) >= 0;
  }
  constexpr void failed(std::uint32_t now_ms) noexcept {
    if (failures_ < 255u) {
      ++failures_;
    }
    retry_at_ = now_ms + kBackoffMs;
  }
  constexpr void succeeded() noexcept { failures_ = 0; }
  constexpr std::uint8_t failures() const noexcept { return failures_; }
  constexpr bool gave_up() const noexcept { return failures_ >= kAutoAttempts; }

 private:
  std::uint8_t failures_ = 0;
  std::uint32_t retry_at_ = 0;
};

}  // namespace o3os::platform::mk66
