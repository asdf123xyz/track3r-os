// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// How an image reaches the Tracker+ (OS-PORT-RT10XX): the facts of the unit's bootloader and of the `.ptf` file it reads, as functions
// the tools (platform/rt10xx/tools/make_ptf.py, check_image.py) mirror and the host tests check. Nothing here flashes anything.
//
// The facts (Modular-Trackular FINDINGS sections 1, 3, 8, 26: observations of the unit; used here as facts of the hardware, see
// docs/specs/OS-PORT-RT10XX.md section 2 for the labels):
//   * a `.ptf` is Intel HEX with CRLF line endings: type 04 (extended linear address) records, type 00 data records, one type 05 (start
//     linear address, the reset handler) and one type 01; one contiguous range from 0x6003_0000; nothing below it;
//   * the update path: the Config menu lists `Firmware/` files whose name starts with "TrackerPlus" and ends with ".ptf" (case-sensitive),
//     writes the chosen name to `Firmware/_fwinfo` and "start" to `Firmware/_turnon`, and resets; the bootloader reads both, programs the
//     flash from the file and clears `_turnon`. The pinhole next to the USB port starts the same update from the bootloader (the recovery
//     path: with only the official `.ptf` in `Firmware/`, it puts the official firmware back);
//   * images up to 8 MiB of flash install (kImageMaxEnd); an image is accepted whose records differ from the official ones;
//   * the bootloader programs 256-byte pages and erases a sector only on the way through a full page: an image whose last byte lies in
//     the FIRST page of a new 4 KiB sector is written without the erase and comes out ANDed with what the sector held. The rule in use:
//     pad such an image with 0xFF up to kPagePadBytes into the second page. The exact rule inside the bootloader is unknown; the
//     observed boundary is "ends in page 0: broken; ends in page 1 or later: fine".

#include <o3os/platform/rt10xx/mem_map.h>

#include <cstddef>
#include <cstdint>

namespace o3os::platform::rt10xx::image {

// The firmware folder and the two marker files of the update path (relative to the card's root; the OS writes the markers in phase 4,
// hal::firmware::FirmwareSink::install()).
inline constexpr const char* kFirmwareDir = "Firmware";
inline constexpr const char* kInfoFile = "Firmware/_fwinfo";
inline constexpr const char* kTurnOnFile = "Firmware/_turnon";
inline constexpr const char* kTurnOnText = "start";  // the five bytes the official firmware writes
inline constexpr const char* kNamePrefix = "TrackerPlus";
inline constexpr const char* kNameSuffix = ".ptf";
// The version text the official Config screen shows is at most 9 characters ("1.2.0-xyz": FINDINGS 8); the port's own screens have no
// such limit, but a file name the official bootloader lists must follow the name rule above.

namespace detail {
constexpr std::size_t length(const char* s) noexcept {
  std::size_t n = 0;
  while (s[n] != '\0') {
    ++n;
  }
  return n;
}
constexpr bool starts_with(const char* s, const char* prefix) noexcept {
  for (std::size_t i = 0; prefix[i] != '\0'; ++i) {
    if (s[i] != prefix[i]) {
      return false;
    }
  }
  return true;
}
constexpr bool ends_with(const char* s, const char* suffix) noexcept {
  const std::size_t n = length(s);
  const std::size_t m = length(suffix);
  if (m > n) {
    return false;
  }
  for (std::size_t i = 0; i < m; ++i) {
    if (s[n - m + i] != suffix[i]) {
      return false;
    }
  }
  return true;
}
}  // namespace detail

// A file name (no directory) the official firmware's update list accepts: "TrackerPlus<anything>.ptf", case-sensitive, and not a macOS
// "._" resource file (it skips those). The port's own list (phase 4) follows the same rule, so one card serves both firmwares.
constexpr bool listed_name(const char* name) noexcept {
  return name != nullptr && detail::starts_with(name, kNamePrefix) && detail::ends_with(name, kNameSuffix) &&
         detail::length(name) > detail::length(kNamePrefix) + detail::length(kNameSuffix);
}

// The end rule. `end` is one past the last byte of the image; the result is the end to pad to with 0xFF (equal to `end` when no padding
// is needed). An image that ends exactly on a sector boundary ends in the last page of the previous sector: no padding.
constexpr std::uint32_t padded_end(std::uint32_t end) noexcept {
  const std::uint32_t in_sector = end % mem::kFlashSectorSize;
  if (in_sector == 0u || in_sector > mem::kFlashPageSize) {
    return end;
  }
  return end - in_sector + mem::kFlashPageSize + mem::kPagePadBytes;
}

// The image lies in the region the bootloader is known to program.
constexpr bool image_range_ok(std::uint32_t base, std::uint32_t end) noexcept {
  return base == mem::kImageBase && end > base && end <= mem::kImageMaxEnd;
}

static_assert(listed_name("TrackerPlus_1.2.0.ptf"), "the official name");
static_assert(listed_name("TrackerPlus_o3os.ptf"), "a name of this port");
static_assert(!listed_name("trackerplus_1.2.0.ptf"), "case-sensitive");
static_assert(!listed_name("TrackerPlus.ptf"), "prefix and suffix alone is not a name");
static_assert(!listed_name("TrackerPlus_1.2.0.PTF"), "the suffix is lower case");
static_assert(!listed_name("track3r-os-v1.0.0.ptf"), "the OG's names are not listed by the Tracker+ bootloader");
static_assert(padded_end(0x601BB0D5u) == 0x601BB110u, "FINDINGS 26: the image that boot-looped (0xD5 into a new sector) is padded 16 bytes into page 1");
static_assert(padded_end(0x601BB10Du) == 0x601BB10Du, "an image already in page 1 is left alone");
static_assert(padded_end(0x601BB000u) == 0x601BB000u, "an image ending on a sector boundary is left alone");
static_assert(padded_end(0x601BB100u) == 0x601BB110u, "an image whose last byte is the last byte of page 0 is padded too");
static_assert(padded_end(0x601BB101u) == 0x601BB101u, "an image whose last byte is the first byte of page 1 is left alone");
static_assert(image_range_ok(mem::kImageBase, mem::kImageBase + 0x1000u) && !image_range_ok(0x60000000u, 0x60001000u) &&
                  !image_range_ok(mem::kImageBase, mem::kImageMaxEnd + 1u),
              "the range check");

}  // namespace o3os::platform::rt10xx::image
