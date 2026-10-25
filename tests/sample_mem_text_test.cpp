// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-AUDIT-A3: the sample-memory figure the Sample Load card and the Sample Edit screen show (KB, rounded down, the same rounding as Record's TIME LEFT).
#include <doctest/doctest.h>

#include <o3os/app/text_format.h>
#include <o3os/core/fixed_string.h>

#include <cstdint>
#include <string>

using namespace o3os;

namespace {

std::string free_text(std::uint32_t free_samples) {
  core::FixedString<40> s;
  CHECK(app::append_free_kb(s, free_samples));
  return s.c_str();
}

std::string used_free_text(std::uint32_t total, std::uint32_t free_samples) {
  core::FixedString<40> s;
  CHECK(app::append_used_free_kb(s, total, free_samples));
  return s.c_str();
}

}  // namespace

TEST_CASE("sample memory text: KB are rounded down, 512 samples to a KB") {
  CHECK(app::sample_kb(0u) == 0u);
  CHECK(app::sample_kb(511u) == 0u);
  CHECK(app::sample_kb(512u) == 1u);
  CHECK(app::sample_kb(1023u) == 1u);
  CHECK(app::sample_kb(1024u) == 2u);
  CHECK(app::sample_kb(0xFFFFFFFFu) == 8388607u);  // no overflow at the top of the range
  CHECK(free_text(2047u) == "3 KB FREE");
  CHECK(free_text(2048u) == "4 KB FREE");
}

TEST_CASE("sample memory text: an empty bank is all free, a full bank has none free") {
  const std::uint32_t total = 4u * 1024u * 1024u;  // 8192 KB
  CHECK(used_free_text(total, total) == "0 KB USED  8192 KB FREE");
  CHECK(used_free_text(total, 0u) == "8192 KB USED  0 KB FREE");
  CHECK(used_free_text(0u, 0u) == "0 KB USED  0 KB FREE");  // no sample memory at all
}

TEST_CASE("sample memory text: used and free are each rounded down, a free part above the total counts as all free") {
  CHECK(used_free_text(10000u, 4999u) == "9 KB USED  9 KB FREE");  // 5001 and 4999 samples
  CHECK(used_free_text(1000u, 5000u) == "0 KB USED  9 KB FREE");
}

TEST_CASE("sample memory text: a text that does not fit is reported, not overrun") {
  core::FixedString<8> s;
  CHECK_FALSE(app::append_used_free_kb(s, 4u * 1024u * 1024u, 0u));
}
