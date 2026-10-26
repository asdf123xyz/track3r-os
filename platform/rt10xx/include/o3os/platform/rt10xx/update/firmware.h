// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The rt10xx's firmware update (OS-PORT-RT10XX phase 4): the hand-over the official Tracker+ firmware makes, as an observed fact
// (Modular-Trackular FINDINGS section 3: delete and write Firmware/_fwinfo with the image's bare name, write "start" into Firmware/_turnon,
// SYSRESETREQ; the bootloader installs the image, and the pinhole installs the .ptf in Firmware/ when an image does not boot), preceded by
// this OS's own checks: the image's name is one the bootloader lists (boot_image.h listed_name) and the store accepts, and the whole file
// passes the streaming validator (ptf_validator.h) twice, at `fw arm` and again at `fw update`, with the same CRC-32. Writes go through the
// MK66's FatFirmwareStore and its path guard only (`_fwinfo`, `_turnon`). Images come onto the card from a computer: there is no upload
// over USB yet (`fw put` is a leftover). Implemented in the image only (src/firmware.cpp). Main loop only.

#include <o3os/platform/mk66/update/firmware_store.h>
#include <o3os/platform/rt10xx/update/ptf_validator.h>

#include <cstdint>

namespace o3os::platform::rt10xx::update {

struct CheckResult {
  Fault fault = Fault::NotFound;
  std::uint32_t file_bytes = 0;
  std::uint32_t image_bytes = 0;
  std::uint32_t crc = 0;
  std::uint32_t fault_offset = 0;
};

o3os::platform::mk66::update::FirmwareStore& store() noexcept;
// Reads Firmware/<name> whole through a fresh validator. `name` must be a listed name the store accepts (else BadName).
CheckResult check(const char* name) noexcept;
// A check that passed, remembered with a one-use token valid for kArmLifetimeMs; 0 when the check failed.
std::uint32_t arm(const char* name, CheckResult& result) noexcept;
const char* armed_name() noexcept;
// With the token of the last arm(), within its lifetime: the check again (the same CRC), the outputs stopped, _fwinfo replaced, _turnon
// written, then the chip reset (this call does not return). Any failure returns its Fault and leaves no _fwinfo or _turnon of this call.
Fault update(std::uint32_t token) noexcept;
// Deletes Firmware/_fwinfo and Firmware/_turnon (a hand-over that should not happen any more).
Fault clear() noexcept;
inline constexpr std::uint32_t kArmLifetimeMs = 30000;

}  // namespace o3os::platform::rt10xx::update
