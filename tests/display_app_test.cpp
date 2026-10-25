// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-007c, the boot to the pattern view, end to end on the host: the OS app runs over the FT812 display driver (the chip model
// and a DMA engine fake instead of the hardware), uploads its bitmaps through the driver's upload path, draws the pattern view
// in all three themes with the cursor moving, and the picture the simulator's rasteriser makes from what the chip holds (its
// RAM_G and the display list it swapped in) is the same picture, bit for bit, as the simulator's own run of the same app. So
// what reaches the hardware is what the simulator checks. Host only (the simulator's rasteriser is megabytes of framebuffer).
#include <doctest/doctest.h>

#include "../platform/mk66/tests/mk66_display_fakes.h"

#include <o3os/app/app.h>
#include <o3os/core/action.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/platform/host/sha256.h>
#include <o3os/platform/mk66/display/chip.h>
#include <o3os/platform/mk66/display/display.h>
#include <o3os/sim/eve_raster.h>
#include <o3os/sim/raster_display.h>
#include <o3os/ui/assets.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace display = o3os::platform::mk66::display;
using o3os::platform::mk66::display::fake::FakeBulk;
using o3os::platform::mk66::display::fake::FakeChip;
using o3os::platform::host::FakeClock;
using o3os::platform::host::FakeInput;
using o3os::sim::EveRaster;
using o3os::sim::RasterDisplay;

namespace {

std::string picture_hash(const EveRaster& raster) {
  return o3os::platform::host::sha256_hex(raster.rgba(), EveRaster::kFramebufferBytes);
}

// The words of the list the chip is scanning: from the start of the swapped-in RAM_DL to the first DISPLAY (an all-zero word,
// the only one in a list), inclusive.
std::vector<std::uint32_t> displayed_words(const FakeChip& chip) {
  std::vector<std::uint32_t> words;
  const std::vector<std::uint8_t>& dl = chip.displayed();
  for (std::size_t i = 0; i + 4 <= dl.size(); i += 4) {
    std::uint32_t w;
    std::memcpy(&w, &dl[i], 4);
    words.push_back(w);
    if (w == 0) {
      break;
    }
  }
  return words;
}

}  // namespace

TEST_CASE("display app: the pattern view over the FT812 driver is the simulator's picture, in every theme and with the cursor moving") {
  // The reference: the app on the simulator's display.
  EveRaster ref_raster;
  RasterDisplay ref_display(ref_raster);
  CHECK(o3os::sim::load_assets(ref_display) == o3os::hal::UploadResult::Ok);
  FakeInput ref_input;
  FakeClock ref_clock;
  o3os::app::App ref_app(ref_input, ref_clock, ref_display);

  // The device path: the chip model behind the driver, brought up by the real init.
  FakeChip chip;
  FakeBulk bulk(chip);
  display::Chip driver(chip);
  display::Ft812Display disp(driver, bulk);
  CHECK(driver.init(display::InitConfig{}).status == display::InitStatus::Ok);
  disp.start(true);
  CHECK(disp.self_test());
  // The asset upload as main does it: through upload_assets(), in the driver's chunks, over the DMA engine fake.
  CHECK(o3os::ui::upload_assets(disp) == o3os::hal::UploadResult::Ok);
  FakeInput input;
  FakeClock clock;
  o3os::app::App app(input, clock, disp);

  const auto settle_until_swapped = [&] {
    for (int ms = 0; ms < 200 && !disp.idle(); ++ms) {
      chip.advance_ms(1);
      disp.service();
    }
    CHECK(disp.idle());
  };
  settle_until_swapped();

  // The bitmaps in RAM_G are the simulator's, byte for byte.
  CHECK(std::memcmp(chip.ram_g_all().data(), ref_raster.ram_g(), o3os::ui::kRamGEnd) == 0);

  const o3os::core::ActionId down = o3os::core::action_id("nav.down");
  const o3os::core::ActionId theme = o3os::core::action_id("ui.theme.next");
  const o3os::core::ActionId home = o3os::core::action_id("cursor.home");
  unsigned compared = 0;
  for (unsigned step = 0; step < 14; ++step) {
    if (step != 0) {  // the demo cycle's moves: a cursor step each time, a theme change every fifth, back to the top once
      const o3os::core::ActionId a = step == 11 ? home : down;
      ref_app.invoke(a);
      app.invoke(a);
      if (step % 5 == 0) {
        ref_app.invoke(theme);
        app.invoke(theme);
      }
    }
    ref_app.run_once();
    app.run_once();  // draws if something changed; refuses (retries next pass) while the last swap is pending
    for (int ms = 0; ms < 200 && app.dirty(); ++ms) {
      chip.advance_ms(1);
      disp.service();
      app.run_once();
    }
    CHECK_FALSE(app.dirty());
    settle_until_swapped();

    // What the simulator's rasteriser makes of what the chip holds ...
    EveRaster raster;
    CHECK(raster.write_ram_g(0, chip.ram_g_all().data(), o3os::ui::kRamGEnd));
    const std::vector<std::uint32_t> words = displayed_words(chip);
    CHECK(words.size() > 10u);
    CHECK(raster.render(words.data(), words.size()));
    // ... is what it makes of the app's own run.
    CHECK(picture_hash(raster) == picture_hash(ref_raster));
    ++compared;
  }
  CHECK(compared == 14u);
  CHECK(app.theme() == ref_app.theme());
  CHECK(disp.stats().frames >= 10u);  // a step that changes nothing (the cursor at the pattern's end) draws no frame
  CHECK(disp.stats().bulk_failures == 0u);
  CHECK(disp.stats().polled_sends == 0u);  // every frame and every asset chunk went over the engine
  CHECK(app.failed_frames() == 0u);
  CHECK(chip.bus_violations() == 0u);  // no polled access ever met a DMA transfer on the bus
  // the backlight came up, after the first frame of the app was on screen
  for (int ms = 0; ms < 400; ++ms) {
    chip.advance_ms(1);
    disp.service();
  }
  CHECK(disp.backlight_on());
  CHECK(chip.reg(o3os::platform::mk66::display::ft812::reg::kPwmDuty) == display::kBacklightDefault);
}
