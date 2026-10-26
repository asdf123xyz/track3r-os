// SPDX-License-Identifier: GPL-3.0-or-later
//
// The display link's words (display_config.h): the LPSPI root, the two rates against the FT81x's limits, the transmit command word, the pads.
#include <doctest/doctest.h>

#include <o3os/platform/rt10xx/display_config.h>

namespace d = o3os::platform::rt10xx::display_cfg;

TEST_CASE("rt10xx display: the root and the rates") {
  CHECK(d::kRootHz == 132'000'000u);
  CHECK(((d::kCbcmrLpspiValue >> 4) & 3u) == 2u);   // PLL2
  CHECK(((d::kCbcmrLpspiValue >> 26) & 7u) == 3u);  // divide by 4
  CHECK((d::kCbcmrLpspiValue & ~d::kCbcmrLpspiMask) == 0u);
  CHECK(d::sck_hz(d::kInitRate) == 1'100'000u);
  CHECK(d::sck_hz(d::kDataRate) == 18'857'142u);
  CHECK(d::sck_hz(d::kInitRate) <= d::kMaxInitHz);
  CHECK(d::sck_hz(d::kDataRate) <= d::kMaxDataHz);
  CHECK(d::tcr(d::kDataRate) == 7u);                 // 8-bit frames, mode 0, MSB first, no prescale
  CHECK((d::tcr(d::kDataRate) >> 30) == 0u);          // CPOL 0, CPHA 0: the FT81x's mode
  CHECK(d::ccr(d::kInitRate) == 118u);
}

TEST_CASE("rt10xx display: the registers and the pads") {
  CHECK(d::kTcr == 0x403A0060u);
  CHECK(d::kTdr == 0x403A0064u);
  CHECK(d::kRdr == 0x403A0074u);
  CHECK(d::kSck.alt == 3u);
  CHECK(d::kCs.alt == 5u);  // a GPIO
  CHECK(d::kSck.select_input == 0x401F8520u);
  CHECK(d::kCcgr1Lpspi4 == (3u << 6));
  CHECK(d::kGpio2DrClear == 0x401BC088u);
}
