// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The MK66 half of the input drivers (OS-007d, src/device.cpp, image only): the board's pins, the FTM2 quadrature decoder, the two I2C
// masters (the platform's, src/i2c.cpp), and the objects the image wires to the app: the scan (`hal::Input`) and the pad LEDs
// (`hal::Leds`). Main thread only.

#include <o3os/platform/mk66/input/pad_leds.h>
#include <o3os/platform/mk66/input/scanner.h>

#include <cstdint>

namespace o3os::platform::mk66::input::device {

struct BringUp {
  bool buttons_online;  // the control-button key scanner answered its configuration
};

// Configures the pins (the INT lines as inputs with pull-ups, the encoder pins), starts the FTM2 quadrature decoder,
// brings up the control bus (I2C0; a held bus is released first) and configures the control-button key scanner. Bounded: a chip that does not
// answer costs three attempts and is retried later by the scan itself. Call once, after the clock tree, the SDRAM and the early pins.
//
// OS-BUG-PADS: the pad bus (I2C2) is NOT brought up here. Its gate (pad_bus.h) opens one second after the board's power rail came up
// (boot::rail_up_ms) and after audio_started(); the first service() pass after that initialises the module and configures the pad key scanner,
// and the pad LED driver follows on its first flush. So the pads are not online when this returns, and the boot chords (control buttons) do not wait for them.
BringUp bring_up() noexcept;
// OS-BUG-PADS: the audio start has finished, whether it worked or not (main.cpp, right after it): the second condition of the pad bus's gate. Main loop only.
void audio_started() noexcept;

// The scan and the pad LEDs; valid from bring_up(). The LED driver sets itself up on its first flushes.
Scanner& scanner() noexcept;
// OS-DJ C1 (spec Q8): the FTM2 counter, one 16-bit read of a register the decoder writes and nothing else touches; the render calls it once a block through MasterScratch::set_counter. Any context, no state.
std::uint16_t encoder_count() noexcept;
PadLeds& leds() noexcept;

}  // namespace o3os::platform::mk66::input::device
