// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Tracker+ firmware file's streaming validator (update/ptf_validator.h): a good image in every chunking, and each rule broken once.
// The images are built here with the layout make_ptf.py writes (16-byte data records, an 04 record per 64 KiB, the start record, EOF, CRLF).
#include <doctest/doctest.h>

#include <o3os/platform/mk66/update/crc32.h>
#include <o3os/platform/rt10xx/update/ptf_validator.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace up = o3os::platform::rt10xx::update;
using Fault = up::Fault;

namespace {

constexpr std::uint32_t kBase = 0x60030000u;

void put32(std::vector<std::uint8_t>& m, std::size_t at, std::uint32_t v) {
  m[at] = static_cast<std::uint8_t>(v);
  m[at + 1] = static_cast<std::uint8_t>(v >> 8);
  m[at + 2] = static_cast<std::uint8_t>(v >> 16);
  m[at + 3] = static_cast<std::uint8_t>(v >> 24);
}

// An image of `size` bytes: SP, reset vector past the table, every other vector 0 except slot 2 and an ITCM handler in slot 16.
std::vector<std::uint8_t> image(std::size_t size = 0x1120) {
  std::vector<std::uint8_t> m(size, 0);
  for (std::size_t i = up::kHeadBytes; i < size; ++i) {
    m[i] = static_cast<std::uint8_t>(i * 7u);
  }
  put32(m, 0, 0x20058000u);
  put32(m, 4, kBase + up::kHeadBytes + 1u);
  put32(m, 8, kBase + up::kHeadBytes + 0x41u);
  put32(m, 16 * 4, 0x00000101u);  // a device interrupt in the ITCM's hot code
  return m;
}

std::string record(std::uint8_t type, std::uint16_t addr, const std::uint8_t* data, std::size_t n, const char* eol) {
  unsigned sum = static_cast<unsigned>(n) + (addr >> 8) + (addr & 0xFFu) + type;
  char buf[16];
  std::string s = ":";
  std::snprintf(buf, sizeof buf, "%02X%04X%02X", static_cast<unsigned>(n), addr, type);
  s += buf;
  for (std::size_t i = 0; i < n; ++i) {
    std::snprintf(buf, sizeof buf, "%02X", data[i]);
    s += buf;
    sum += data[i];
  }
  std::snprintf(buf, sizeof buf, "%02X", (0x100u - (sum & 0xFFu)) & 0xFFu);
  s += buf;
  s += eol;
  return s;
}

struct Options {
  std::uint32_t base = kBase;
  bool start = true;
  std::uint32_t start_value = 0;  // 0: the reset vector
  const char* eol = "\r\n";
};

std::string encode(const std::vector<std::uint8_t>& m, const Options& o = Options{}) {
  std::string s;
  std::uint32_t upper = 0xFFFFFFFFu;
  for (std::size_t at = 0; at < m.size(); at += 16) {
    const std::uint32_t address = o.base + static_cast<std::uint32_t>(at);
    if ((address >> 16) != upper) {
      upper = address >> 16;
      const std::uint8_t u[2] = {static_cast<std::uint8_t>(upper >> 8), static_cast<std::uint8_t>(upper)};
      s += record(0x04, 0, u, 2, o.eol);
    }
    const std::size_t n = m.size() - at < 16 ? m.size() - at : 16;
    s += record(0x00, static_cast<std::uint16_t>(address & 0xFFFFu), &m[at], n, o.eol);
  }
  if (o.start) {
    const std::uint32_t v = o.start_value != 0u ? o.start_value : (static_cast<std::uint32_t>(m[4]) | (m[5] << 8) | (m[6] << 16) | (static_cast<std::uint32_t>(m[7]) << 24));
    const std::uint8_t d[4] = {static_cast<std::uint8_t>(v >> 24), static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v)};
    s += record(0x05, 0, d, 4, o.eol);
  }
  s += record(0x01, 0, nullptr, 0, o.eol);
  return s;
}

Fault verdict(const std::string& text, std::size_t chunk = 4096) {
  up::PtfValidator v;
  const auto* p = reinterpret_cast<const std::uint8_t*>(text.data());
  for (std::size_t at = 0; at < text.size(); at += chunk) {
    v.feed(p + at, text.size() - at < chunk ? text.size() - at : chunk);
  }
  v.finish();
  return v.fault();
}

}  // namespace

TEST_CASE("rt10xx ptf: a good image, in every chunking, with its length and CRC") {
  const std::vector<std::uint8_t> m = image();
  const std::string text = encode(m);
  for (std::size_t chunk : {std::size_t{1}, std::size_t{3}, std::size_t{17}, std::size_t{512}, text.size()}) {
    up::PtfValidator v;
    const auto* p = reinterpret_cast<const std::uint8_t*>(text.data());
    for (std::size_t at = 0; at < text.size(); at += chunk) {
      CHECK(v.feed(p + at, text.size() - at < chunk ? text.size() - at : chunk));
    }
    CHECK(v.finish());
    CHECK(v.image_length() == m.size());
    CHECK(v.image_end() == kBase + m.size());
    o3os::platform::mk66::update::Crc32 crc;
    for (char c : text) {
      crc.add(static_cast<std::uint8_t>(c));
    }
    CHECK(v.crc() == crc.value());
  }
  CHECK(verdict(encode(image(0x10120))) == Fault::Ok);  // across a 64 KiB boundary (an 04 record), ending in page 1
}

TEST_CASE("rt10xx ptf: each rule broken once") {
  const std::vector<std::uint8_t> good = image();
  Options other_base;
  other_base.base = 0x60000000u;
  CHECK(verdict(encode(good, other_base)) == Fault::HexRange);  // the bootloader's flash
  Options shifted;
  shifted.base = kBase + 16u;
  CHECK(verdict(encode(good, shifted)) == Fault::HexOrder);  // not starting at the base
  Options bad_start;
  bad_start.start_value = kBase + 0x1001u;
  CHECK(verdict(encode(good, bad_start)) == Fault::HexStart);
  {
    std::vector<std::uint8_t> m = good;
    put32(m, 0, 0x20058004u);  // above the stack top
    CHECK(verdict(encode(m)) == Fault::BadSp);
    put32(m, 0, 0x20057FFCu);  // not 8-aligned
    CHECK(verdict(encode(m)) == Fault::BadSp);
  }
  {
    std::vector<std::uint8_t> m = good;
    put32(m, 4, kBase + up::kHeadBytes);  // not Thumb
    CHECK(verdict(encode(m)) == Fault::BadReset);
    put32(m, 4, kBase + 0x11u);  // inside the vector table
    CHECK(verdict(encode(m)) == Fault::BadReset);
  }
  {
    std::vector<std::uint8_t> m = good;
    put32(m, 7 * 4, 0x1u);  // a reserved slot
    CHECK(verdict(encode(m)) == Fault::BadVector);
  }
  {
    std::vector<std::uint8_t> m = good;
    put32(m, 20 * 4, 0x20000001u);  // a handler in the DTCM
    CHECK(verdict(encode(m)) == Fault::BadVector);
  }
  {
    std::vector<std::uint8_t> m = image(0x2010);  // ends 16 bytes into a new sector's first page
    CHECK(verdict(encode(m)) == Fault::BadConfig);  // the end rule
    CHECK(std::string(up::ptf_fault_name(Fault::BadConfig)).find("end rule") != std::string::npos);
  }
  CHECK(verdict(encode(image(0x200))) == Fault::HexShort);
  Options lf;
  lf.eol = "\n";
  CHECK(verdict(encode(good, lf)) == Fault::HexChar);  // a .ptf's lines end CR LF
  Options no_start;
  no_start.start = false;
  CHECK(verdict(encode(good, no_start)) == Fault::HexStart);  // the start record is required
  {
    std::string two = encode(good);
    const std::size_t eof = two.rfind(':');
    const std::size_t start = two.rfind(':', eof - 1);
    two.insert(eof, two.substr(start, eof - start));  // the start record twice
    CHECK(verdict(two) == Fault::HexRecord);
  }
  std::string text = encode(good);
  CHECK(verdict(text.substr(0, text.size() - 13)) == Fault::HexNoEof);  // the EOF record cut off
  std::string bad_sum = text;
  bad_sum[text.find('\n') + 12] = bad_sum[text.find('\n') + 12] == '0' ? '1' : '0';
  CHECK(verdict(bad_sum) == Fault::HexChecksum);
  CHECK(verdict(":020000021000EC\r\n" + text) == Fault::HexType);  // an 02 record
  CHECK(verdict(text + ":00000001FF\r\n") == Fault::HexAfterEof);
}
