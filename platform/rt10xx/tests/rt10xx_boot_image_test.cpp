// SPDX-License-Identifier: GPL-3.0-or-later
//
// The image-delivery facts (boot_image.h): the name rule of the unit's update list and the sector end rule, as the tools mirror them
// (platform/rt10xx/tools/make_ptf.py, check_image.py have the same cases in their self-tests).
#include <doctest/doctest.h>

#include <o3os/platform/rt10xx/boot_image.h>

#include <string_view>

namespace image = o3os::platform::rt10xx::image;
namespace mem = o3os::platform::rt10xx::mem;

TEST_CASE("rt10xx boot_image: the update path's names") {
  CHECK(std::string_view(image::kInfoFile) == "Firmware/_fwinfo");
  CHECK(std::string_view(image::kTurnOnFile) == "Firmware/_turnon");
  CHECK(std::string_view(image::kTurnOnText) == "start");
  CHECK(image::listed_name("TrackerPlus_1.2.0.ptf"));
  CHECK(image::listed_name("TrackerPlus_o3os.ptf"));
  CHECK(image::listed_name("TrackerPlusX.ptf"));
  CHECK_FALSE(image::listed_name("TrackerPlus.ptf"));
  CHECK_FALSE(image::listed_name("trackerplus_1.2.0.ptf"));
  CHECK_FALSE(image::listed_name("TrackerPlus_1.2.0.PTF"));
  CHECK_FALSE(image::listed_name("track3r-os-v1.0.0.ptf"));
  CHECK_FALSE(image::listed_name(""));
  CHECK_FALSE(image::listed_name(nullptr));
}

TEST_CASE("rt10xx boot_image: the sector end rule") {
  const std::uint32_t sector = 0x601BB000u;
  CHECK(image::padded_end(sector + 0xD5u) == sector + 0x110u);   // the image that boot-looped (FINDINGS 26)
  CHECK(image::padded_end(sector + 0x1u) == sector + 0x110u);
  CHECK(image::padded_end(sector + 0x100u) == sector + 0x110u);  // last byte in page 0
  CHECK(image::padded_end(sector + 0x101u) == sector + 0x101u);  // last byte in page 1
  CHECK(image::padded_end(sector + 0x10Du) == sector + 0x10Du);
  CHECK(image::padded_end(sector) == sector);                    // ends on the boundary: the last byte is in the previous sector
  CHECK(image::padded_end(sector + 0xFFFu) == sector + 0xFFFu);
  CHECK(mem::kPagePadBytes == 16u);
}

TEST_CASE("rt10xx boot_image: the range the bootloader is known to program") {
  CHECK(image::image_range_ok(mem::kImageBase, mem::kImageBase + 0x1000u));
  CHECK(image::image_range_ok(mem::kImageBase, mem::kImageMaxEnd));
  CHECK_FALSE(image::image_range_ok(mem::kImageBase, mem::kImageMaxEnd + 1u));
  CHECK_FALSE(image::image_range_ok(mem::kImageBase, mem::kImageBase));
  CHECK_FALSE(image::image_range_ok(mem::kFlashBase, mem::kImageBase));
}
