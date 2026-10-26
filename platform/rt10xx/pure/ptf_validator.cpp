// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Tracker+ firmware file's streaming validator (ptf_validator.h). The state machine is the MK66 validator's (platform/mk66 update/src/
// hex_validator.cpp) with this chip's record types, base, range and vector rules.
#include <o3os/platform/rt10xx/update/ptf_validator.h>

#include <o3os/platform/rt10xx/boot_image.h>
#include <o3os/platform/rt10xx/mem_map.h>

namespace o3os::platform::rt10xx::update {
namespace {

static_assert(kHeadBytes == mem::kVectorTableBytes, "the head is the vector table of mem_map.h");

int hex_value(std::uint8_t c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return -1;
}

bool reserved_vector(unsigned index) noexcept { return index == 7u || index == 8u || index == 9u || index == 10u || index == 13u; }

}  // namespace

std::uint32_t PtfValidator::base_address() noexcept { return mem::kImageBase; }

void PtfValidator::reset() noexcept {
  state_ = State::LineStart;
  cr_ = false;
  have_hi_ = false;
  nibble_ = 0;
  pos_ = 0;
  count_ = 0;
  type_ = 0;
  sum_ = 0;
  offset_ = 0;
  datum_ = 0;
  upper_ = 0;
  next_ = mem::kImageBase;
  eof_ = false;
  have_start_ = false;
  start_ = 0;
  fault_ = Fault::Ok;
  fault_at_ = 0;
  consumed_ = 0;
  crc_.reset();
  for (std::uint8_t& b : head_) {
    b = 0;
  }
}

void PtfValidator::fail(Fault f) noexcept {
  if (fault_ == Fault::Ok) {
    fault_ = f;
    fault_at_ = consumed_;
  }
}

bool PtfValidator::feed(const std::uint8_t* data, std::size_t n) noexcept {
  for (std::size_t i = 0; i < n && fault_ == Fault::Ok; ++i) {
    crc_.add(data[i]);
    step(data[i]);
    if (fault_ == Fault::Ok) {
      ++consumed_;
    }
  }
  return fault_ == Fault::Ok;
}

void PtfValidator::step(std::uint8_t c) noexcept {
  if (state_ == State::LineStart) {
    if (c == ':') {
      if (eof_) {
        fail(Fault::HexAfterEof);
      } else if (cr_) {
        fail(Fault::HexChar);
      } else {
        state_ = State::InRecord;
        pos_ = 0;
        sum_ = 0;
        have_hi_ = false;
      }
    } else if (c == '\r') {
      cr_ = true;
    } else if (c == '\n') {
      if (!cr_) {
        fail(Fault::HexChar);  // a .ptf's lines end CR LF (the official files' layout, check_image.py --expect-ptf)
      }
      cr_ = false;
    } else {
      fail(eof_ ? Fault::HexAfterEof : Fault::HexChar);
    }
    return;
  }
  const int digit = hex_value(c);
  if (digit >= 0) {
    if (!have_hi_) {
      nibble_ = static_cast<std::uint8_t>(digit);
      have_hi_ = true;
    } else {
      have_hi_ = false;
      byte_in(static_cast<std::uint8_t>((nibble_ << 4) | static_cast<unsigned>(digit)));
    }
    return;
  }
  if (c == '\r' || c == '\n') {
    if (have_hi_) {
      fail(Fault::HexDigits);
    } else if (pos_ < 5u || pos_ != static_cast<unsigned>(count_) + 5u) {
      fail(Fault::HexRecord);
    } else {
      if (c == '\n') {
        fail(Fault::HexChar);  // LF without CR
        return;
      }
      end_of_record();
      state_ = State::LineStart;
      cr_ = true;
    }
    return;
  }
  fail(Fault::HexChar);
}

void PtfValidator::byte_in(std::uint8_t b) noexcept {
  sum_ = static_cast<std::uint8_t>(sum_ + b);
  if (pos_ >= 4u && pos_ > 4u + count_) {
    fail(Fault::HexRecord);
    return;
  }
  switch (pos_) {
    case 0:
      count_ = b;
      break;
    case 1:
      offset_ = static_cast<std::uint16_t>(b << 8);
      break;
    case 2:
      offset_ = static_cast<std::uint16_t>(offset_ | b);
      break;
    case 3:
      type_ = b;
      datum_ = 0;
      if (b == 0x00u) {
        break;
      }
      if (b == 0x01u) {
        if (count_ != 0u) {
          fail(Fault::HexRecord);
        }
      } else if (b == 0x04u) {
        if (count_ != 2u) {
          fail(Fault::HexRecord);
        }
      } else if (b == 0x05u) {
        if (count_ != 4u) {
          fail(Fault::HexRecord);
        }
      } else if (b == 0x03u) {
        fail(Fault::HexStart);
      } else {
        fail(Fault::HexType);
      }
      break;
    default:
      if (pos_ < 4u + count_) {
        const unsigned index = pos_ - 4u;
        if (type_ == 0x00u) {
          data_byte(index, b);
        } else {
          datum_ = (datum_ << 8) | b;
        }
      } else if (sum_ != 0u) {
        fail(Fault::HexChecksum);
      }
      break;
  }
  ++pos_;
}

void PtfValidator::data_byte(std::uint32_t index, std::uint8_t b) noexcept {
  const std::uint64_t address = static_cast<std::uint64_t>(upper_) + offset_ + index;
  if (address >= mem::kImageMaxEnd || address < mem::kImageBase) {
    fail(Fault::HexRange);
    return;
  }
  if (address != next_) {
    fail(Fault::HexOrder);
    return;
  }
  const std::uint32_t at = next_ - mem::kImageBase;
  if (at < kHeadBytes) {
    head_[at] = b;
  }
  ++next_;
}

void PtfValidator::end_of_record() noexcept {
  if (type_ == 0x01u) {
    eof_ = true;
  } else if (type_ == 0x04u) {
    upper_ = datum_ << 16;
  } else if (type_ == 0x05u) {
    if (have_start_) {
      fail(Fault::HexRecord);  // a second start record (check_image.py refuses it)
      return;
    }
    have_start_ = true;
    start_ = datum_;
  }
}

std::uint32_t PtfValidator::head_word(unsigned index) const noexcept {
  const unsigned at = index * 4u;
  return static_cast<std::uint32_t>(head_[at]) | (static_cast<std::uint32_t>(head_[at + 1]) << 8) | (static_cast<std::uint32_t>(head_[at + 2]) << 16) |
         (static_cast<std::uint32_t>(head_[at + 3]) << 24);
}

bool PtfValidator::finish() noexcept {
  if (fault_ != Fault::Ok) {
    return false;
  }
  if (state_ == State::InRecord) {
    if (have_hi_) {
      fail(Fault::HexDigits);
    } else if (pos_ < 5u || pos_ != static_cast<unsigned>(count_) + 5u) {
      fail(Fault::HexRecord);
    } else {
      end_of_record();
      state_ = State::LineStart;
    }
    if (fault_ != Fault::Ok) {
      return false;
    }
  }
  if (!eof_) {
    fail(Fault::HexNoEof);
    return false;
  }
  if (next_ - mem::kImageBase < kHeadBytes) {
    fail(Fault::HexShort);
    return false;
  }
  const std::uint32_t sp = head_word(0);
  if (!(sp > mem::kDtcmBase && sp <= mem::kStackTop && (sp % 8u) == 0u)) {
    fail(Fault::BadSp);
    return false;
  }
  const std::uint32_t code_start = mem::kImageBase + kHeadBytes;
  const std::uint32_t reset = head_word(1);
  const std::uint32_t reset_target = reset & ~1u;
  if ((reset & 1u) == 0u || reset_target < code_start || reset_target >= next_) {
    fail(Fault::BadReset);
    return false;
  }
  for (unsigned index = 2; index < kVectorEntries; ++index) {
    const std::uint32_t entry = head_word(index);
    if (reserved_vector(index)) {
      if (entry != 0u) {
        fail(Fault::BadVector);
        return false;
      }
      continue;
    }
    if (entry == 0u) {
      continue;
    }
    const std::uint32_t target = entry & ~1u;
    const bool in_image = target >= code_start && target < next_;
    const bool in_itcm = target >= mem::kItcmBase && target - mem::kItcmBase < mem::kRamfuncSize;
    if ((entry & 1u) == 0u || !(in_image || in_itcm)) {
      fail(Fault::BadVector);
      return false;
    }
  }
  if (!have_start_ || start_ != reset) {  // a .ptf carries the start record (check_image.py --expect-ptf)
    fail(Fault::HexStart);
    return false;
  }
  if (image::padded_end(next_) != next_) {
    fail(Fault::BadConfig);  // the end rule (ptf_fault_name says so)
    return false;
  }
  return true;
}

const char* ptf_fault_name(Fault f) noexcept {
  switch (f) {
    case Fault::HexChar: return "a bad character, or a line that does not end CR LF";
    case Fault::HexType: return "record type other than 00, 01, 04, 05";
    case Fault::HexStart: return "a type 03 record, or no start record, or one that is not the reset vector";
    case Fault::HexOrder: return "data not at the next address (a hole, a byte twice, records out of order, or not starting at 0x60030000)";
    case Fault::HexRange: return "an address outside 0x60030000..0x607FFFFF";
    case Fault::HexShort: return "the image ends before the end of its vector table";
    case Fault::BadSp: return "the initial SP is not an 8-aligned DTCM address";
    case Fault::BadReset: return "the reset vector is not a Thumb address in the image past the vector table";
    case Fault::BadVector: return "a vector is not 0 or a Thumb address in the image or the ITCM code, or a reserved slot is not 0";
    case Fault::BadConfig: return "the image ends in the first page of a 4 KiB sector (the end rule: make_ptf.py pads it)";
    default: return o3os::platform::mk66::update::fault_name(f);
  }
}

}  // namespace o3os::platform::rt10xx::update
