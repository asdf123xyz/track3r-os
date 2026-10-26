// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The rt10xx's display (OS-PORT-RT10XX phase 4): the platform/mk66 FT812 driver (display/chip.h, display.h: chip-neutral) over an LPSPI
// link of this chip (display_config.h: the assumed instance and pads). Polled: the bulk (DMA) engine is phase 5's; the driver's polled path
// sends frames and uploads. Nothing is touched before start(): the console's `display start`. Implemented in the image only.

#include <o3os/platform/mk66/display/chip.h>
#include <o3os/platform/mk66/display/display.h>

#include <cstdint>

namespace o3os::platform::rt10xx::display {

// The bring-up: the root and the gates, the pads, the link at the init rate, Chip::init() (PD# is not driven: the soft reset), then the
// data rate and Ft812Display::start(polled). The report says how the chip came up.
o3os::platform::mk66::display::InitReport start() noexcept;
bool started() noexcept;
o3os::platform::mk66::display::Ft812Display& screen() noexcept;
o3os::platform::mk66::display::Chip& chip() noexcept;
// Main loop, every pass once started: the driver's service (the swap, the backlight ramp).
void service() noexcept;
// A test frame: colour bars and a frame line, drawn through the driver's frame path.
bool draw_test_frame() noexcept;
std::uint32_t link_errors() noexcept;

}  // namespace o3os::platform::rt10xx::display
