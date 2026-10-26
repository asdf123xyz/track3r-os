// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A streaming validator of a Tracker+ firmware file (OS-PORT-RT10XX phase 4): the rules of platform/rt10xx/tools/check_image.py `check`,
// run on the device over the bytes of Firmware/<name>.ptf before the bootloader is ever asked to install it. The design is the MK66's
// (platform/mk66 update/hex_validator.h): a state machine over single bytes, so the chunking cannot matter; O(1) state besides the vector
// table; the file's CRC-32 and the image's length as results; a sticky first fault with its offset.
//
// The rules (each a verdict of check_image.py, or a stricter form of one):
//   * record syntax: ':' and hex digit pairs, the count, the checksum (sum 0 mod 256), CR LF line ends (--expect-ptf); types 00, 01, 04 and 05 only
//     (02 and 03 are HexType / HexStart); an EOF record ends the data; only line ends may follow it;
//   * STRICTER than check_image.py, as on the MK66: data bytes arrive at ascending consecutive addresses starting at the image base
//     (0x6003_0000), which is what make_ptf.py and objcopy write (HexOrder otherwise); this also makes "no hole" and "no byte twice" one check;
//   * every address below 0x6080_0000 (HexRange); the image holds at least the vector table (HexShort);
//   * the initial SP an 8-aligned address in the DTCM, 0x2000_0000 < SP <= 0x2005_8000 (BadSp);
//   * the reset vector a Thumb address inside the image past the vector table (BadReset);
//   * the other 174 vectors: the reserved slots 7-10 and 13 are 0; every other one 0, or a Thumb address inside the image past the table, or
//     inside the ITCM's code region (BadVector);
//   * exactly one start record (05), carrying the reset vector (HexStart; --expect-ptf requires it; a second one is HexRecord);
//   * the end rule (boot_image.h padded_end): the image does not end in the first 256-byte page of a 4 KiB sector (EndRule: reported as
//     update::Fault::BadConfig, whose MK66 meaning, the flash configuration field, does not exist on this chip; ptf_fault_name() says which).
// platform/rt10xx/tests/rt10xx_ptf_validator_test.cpp checks every rule and every chunking.

#include <o3os/platform/mk66/update/crc32.h>
#include <o3os/platform/mk66/update/fault.h>

#include <cstddef>
#include <cstdint>

namespace o3os::platform::rt10xx::update {

using Fault = o3os::platform::mk66::update::Fault;

inline constexpr unsigned kVectorEntries = 16 + 160;
inline constexpr std::uint32_t kHeadBytes = kVectorEntries * 4u;  // 704

class PtfValidator {
 public:
  PtfValidator() noexcept { reset(); }
  void reset() noexcept;
  bool feed(const std::uint8_t* data, std::size_t n) noexcept;
  bool finish() noexcept;
  bool ok() const noexcept { return fault_ == Fault::Ok; }
  Fault fault() const noexcept { return fault_; }
  std::uint32_t fault_offset() const noexcept { return fault_at_; }
  // The image's length in bytes from its base (after finish() returned true), and the end address.
  std::uint32_t image_length() const noexcept { return next_ - base_address(); }
  std::uint32_t image_end() const noexcept { return next_; }
  std::uint32_t crc() const noexcept { return crc_.value(); }
  std::uint32_t reset_vector() const noexcept { return head_word(1); }
  static std::uint32_t base_address() noexcept;

 private:
  enum class State : std::uint8_t { LineStart, InRecord };
  void step(std::uint8_t c) noexcept;
  void byte_in(std::uint8_t b) noexcept;
  void data_byte(std::uint32_t index, std::uint8_t b) noexcept;
  void end_of_record() noexcept;
  void fail(Fault f) noexcept;
  std::uint32_t head_word(unsigned index) const noexcept;

  State state_ = State::LineStart;
  bool cr_ = false;
  bool have_hi_ = false;
  std::uint8_t nibble_ = 0;
  std::uint16_t pos_ = 0;
  std::uint8_t count_ = 0;
  std::uint8_t type_ = 0;
  std::uint8_t sum_ = 0;
  std::uint16_t offset_ = 0;
  std::uint32_t datum_ = 0;  // the data of an 04 (2 bytes) or 05 (4 bytes) record
  std::uint32_t upper_ = 0;
  std::uint32_t next_ = 0;
  bool eof_ = false;
  bool have_start_ = false;
  std::uint32_t start_ = 0;
  Fault fault_ = Fault::Ok;
  std::uint32_t fault_at_ = 0;
  std::uint32_t consumed_ = 0;
  o3os::platform::mk66::update::Crc32 crc_;
  std::uint8_t head_[kHeadBytes] = {};
};

// The rt10xx's names of the faults whose MK66 text speaks of the K66 (the range, the SP, the start record, the end rule); the others are
// update::fault_name's.
const char* ptf_fault_name(Fault f) noexcept;

}  // namespace o3os::platform::rt10xx::update
