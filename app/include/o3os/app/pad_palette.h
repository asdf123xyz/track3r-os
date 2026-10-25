// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The default brightness of a pad's LED in each role, drawn from the theme's tokens (OS-007d). The board's pad LEDs are monochrome
// (hal/leds.h: one PWM channel a pad), so "the colour of a pad" is a brightness: this maps the theme's own accent and selection to
// the two levels this OS uses for a pad: a "front" layer (a pad in use) and a "back" layer (a pad that is merely
// available).
//
//   Front  the theme's Accent token (the one hot colour: the cursor), its luminance spread over [kFrontMin, kFrontMax];
//   Back   the theme's Sel token (the selection), its luminance spread over [kBackMin, kBackMax].
//
// No colour literal: the tokens are the theme's. The bounds are this OS's: nothing is brighter than 255, the brightest level the
// board's driver accepts, and the back layer never exceeds kPadBackMax (132), so a theme with a bright selection cannot light a whole grid at the front level. Back is always
// below Front. An instrument-by-colour palette needs RGB pads; the board has none.

#include <o3os/ui/theme.h>

#include <cstdint>

namespace o3os::app {

enum class PadRole : std::uint8_t { Off, Back, Front };

inline constexpr std::uint8_t kPadFrontMin = 160;
inline constexpr std::uint8_t kPadFrontMax = 255;
inline constexpr std::uint8_t kPadBackMin = 40;
inline constexpr std::uint8_t kPadBackMax = 132;
static_assert(kPadBackMax < kPadFrontMin, "a pad in the background is always dimmer than one in use");

// Luma of a token on 0..255: the Rec. 709 weights on the stored values (54 + 183 + 19 = 256).
constexpr unsigned token_luma(const ui::Rgb& c) noexcept {
  return (54u * c.r + 183u * c.g + 19u * c.b) / 256u;
}

constexpr std::uint8_t spread(unsigned luma, std::uint8_t lo, std::uint8_t hi) noexcept {
  return static_cast<std::uint8_t>(lo + luma * static_cast<unsigned>(hi - lo) / 255u);
}

constexpr std::uint8_t pad_level(const ui::Theme& theme, PadRole role) noexcept {
  switch (role) {
    case PadRole::Front:
      return spread(token_luma(theme.rgb(ui::Color::Accent)), kPadFrontMin, kPadFrontMax);
    case PadRole::Back:
      return spread(token_luma(theme.rgb(ui::Color::Sel)), kPadBackMin, kPadBackMax);
    case PadRole::Off:
      break;
  }
  return 0;
}

}  // namespace o3os::app
