// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-USBDISK U1c, the writable step: what a key does on the boot disk's strip (spec 6.1 (2) and the review minor 7: the unit's own eject ignores the computer's PREVENT ALLOW, so while the computer
// may be writing it asks twice). Pure state, no clock of its own, main loop only; app_main.cpp feeds it once a pass and acts on the answer (the restart is a chip reset: the only way out of the mode).
//
//   A key or pad pressed (every key up since the last press: the chord still held at power-up is not one) is
//     * a restart at once when nothing can be lost: the computer has not configured the disk, the card is out, the host ejected it, or the disk is exported write-protected;
//     * otherwise a WARNING ("the computer may still be writing"), open for kConfirmMs; a second press inside that window is the restart.
//   After the restart is asked it waits for the command in flight (a WRITE(10) whose blocks are still arriving) to end, but for kFinishMs at most: the card is atomic per block and the main
//   loop that resets is the one that writes, so a block is never cut; what the wait saves is the rest of the command the computer was in the middle of.
//
// The strip shows prompt(): None (the usual text), Confirm (the warning), Finishing (the restart is waiting for the command).

#include <cstdint>

namespace o3os::platform::mk66 {

class DiskRestart {
 public:
  static constexpr std::uint32_t kConfirmMs = 5000;  // how long the warning waits for the second press
  static constexpr std::uint32_t kFinishMs = 3000;   // the longest the restart waits for a command in flight (1 MiB at the unit's 432 KiB/s write rate is 2.4 s)

  struct Input {
    bool key_down = false;  // any button or pad is down
    bool at_risk = false;   // a computer has configured the disk, a card is in, not ejected, exported writable: a copy may be running
    bool busy = false;      // a SCSI command is in flight (the engine is not idle)
  };
  enum class Prompt : std::uint8_t { None, Confirm, Finishing };

  // Once a pass. True: reset the chip now.
  bool update(std::uint32_t now_ms, const Input& in) noexcept {
    if (!in.key_down) {
      armed_ = true;
    } else if (armed_) {
      armed_ = false;  // this press is spent; the next one needs every key up first
      if (!in.at_risk || (warned_ && now_ms - warned_at_ < kConfirmMs)) {
        restarting_ = true;
        asked_at_ = now_ms;
      } else {
        warned_ = true;
        warned_at_ = now_ms;
      }
    }
    if (!restarting_) {
      return false;
    }
    return !in.busy || now_ms - asked_at_ >= kFinishMs;
  }

  Prompt prompt(std::uint32_t now_ms) const noexcept {
    if (restarting_) {
      return Prompt::Finishing;
    }
    return warned_ && now_ms - warned_at_ < kConfirmMs ? Prompt::Confirm : Prompt::None;
  }

 private:
  bool armed_ = false;       // every key has been up since the last press (false at power-up: the chord is still held)
  bool warned_ = false;
  bool restarting_ = false;
  std::uint32_t warned_at_ = 0;
  std::uint32_t asked_at_ = 0;
};

}  // namespace o3os::platform::mk66
