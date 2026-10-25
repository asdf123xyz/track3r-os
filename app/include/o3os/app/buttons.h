// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace o3os::app {

// Front-panel buttons by name (docs/design/product-ux.md section 3). The numeric codes are the hardware button
// numbers of the Tracker hardware (buttons 0..32); they are what
// core::InputEvent carries as `id` for Button events. PROVISIONAL until the button HAL is specified.
// Not buttons: the pads (InputKind::Pad), the encoder (InputKind::Encoder) and Power.
enum class Button : std::uint8_t {
  F1 = 0, F2, F3, F4, F5, F6, F7, F8,                       // soft keys
  Params = 8, Performance, File, Config, Master,            // screen buttons
  SampleLoad = 13, SamplePlay, SampleEdit, SampleRec, Song,
  Note = 18, Instr, Vol, Fx,                                // step column buttons
  Pattern = 22,
  Play = 23, Copy, Insert, Up, Delete,
  Rec = 28, Shift, Left, Down, Right,
};

inline constexpr unsigned kButtonCount = 33;

// Names used by simulator scenarios (lower case, matched case-insensitively). `inline`: only linked into
// programs that use it (the simulator), so it costs the device nothing.
struct ButtonName {
  const char* name;
  Button button;
};
inline constexpr ButtonName kButtonNames[] = {
    {"f1", Button::F1},
    {"f2", Button::F2},
    {"f3", Button::F3},
    {"f4", Button::F4},
    {"f5", Button::F5},
    {"f6", Button::F6},
    {"f7", Button::F7},
    {"f8", Button::F8},
    {"params", Button::Params},
    {"performance", Button::Performance},
    {"file", Button::File},
    {"config", Button::Config},
    {"master", Button::Master},
    {"sampleload", Button::SampleLoad},
    {"sampleplay", Button::SamplePlay},
    {"sampleedit", Button::SampleEdit},
    {"samplerec", Button::SampleRec},
    {"song", Button::Song},
    {"note", Button::Note},
    {"instr", Button::Instr},
    {"vol", Button::Vol},
    {"fx", Button::Fx},
    {"pattern", Button::Pattern},
    {"play", Button::Play},
    {"copy", Button::Copy},
    {"insert", Button::Insert},
    {"up", Button::Up},
    {"delete", Button::Delete},
    {"rec", Button::Rec},
    {"shift", Button::Shift},
    {"left", Button::Left},
    {"down", Button::Down},
    {"right", Button::Right},
};
static_assert(sizeof(kButtonNames) / sizeof(kButtonNames[0]) == kButtonCount, "one name per button");

}  // namespace o3os::app
