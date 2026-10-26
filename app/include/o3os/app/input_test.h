// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the hidden INPUT TEST screen knows (OS-007d): the raw input, as the HAL produced it, before the gesture layer and the bindings.
// Every button, pad and the encoder, live, in the spirit of the hidden diagnostic screens that the 1.7.2 release notes mention (PAR-132: documented in the release notes, not verified): what is down now, how many presses each
// control has registered since the screen was cleared (a bounce shows as a count of 2 for one press of the finger), the encoder's
// turns, how old the last event was when the app took it (the scan to app part of the press-to-screen latency), and the input
// path's own health counters (hal::InputStats).
//
// Pure data and one function, host-tested; the screen (screens/input_test_screen.h) only draws it.

#include <o3os/app/buttons.h>
#include <o3os/core/fixed_string.h>
#include <o3os/core/input_event.h>
#include <o3os/hal/input.h>

#include <cstdint>

namespace o3os::app {

inline constexpr unsigned kInputTestPads = 48;
inline constexpr std::uint8_t kInputTestCountMax = 99;  // a press count is shown in two digits and stops there

// OS-BUG-PADS: the pad bus line, built from the bus record (hal::PadBusStats): the first and latest result and the attempts of each chip, the result of the bus clear and the pin levels, e.g.
// "BUS 34 NAK/ACK X3  74 ACK/ACK X5  CLR OK  SCL SDA INT 110>111". The levels part is always in it (a held bus is what it is for); the screen draws the line on a row of its own, so the width is the
// only limit: 69 characters at most (tests/input_test_test.cpp measures the worst record against every theme's row).
using InputTestBusLine = core::FixedString<72>;
void input_test_bus_line(const hal::PadBusStats& bus, InputTestBusLine& out) noexcept;

struct InputTestState {
  std::uint64_t buttons_down = 0;  // bit n = button code n is down
  std::uint64_t pads_down = 0;     // bit n = pad n is down
  // Word aligned by declaration (ADR 0011): reset() may clear them a word at a time.
  alignas(4) std::uint8_t button_presses[kButtonCount] = {};
  alignas(4) std::uint8_t pad_presses[kInputTestPads] = {};
  std::int32_t encoder_total = 0;  // detents since the last clear, + clockwise
  std::int16_t encoder_last = 0;   // the last encoder event's value
  std::uint32_t encoder_events = 0;
  std::uint32_t events = 0;        // every event seen
  std::uint32_t last_age_us = 0;   // the last event's age when the app took it: its timestamp to the app's clock
  std::uint32_t max_age_us = 0;
  hal::InputStats stats;

  // Folds one raw event in; `now` is the app's clock at the moment it took the event. True if anything on screen changes (a kind the
  // screen does not show, or an id outside the matrices, changes nothing and is counted only in `events`).
  bool observe(const core::InputEvent& event, core::Tick now) noexcept;
  // The health counters from hal::Input::stats(); true if they differ in what the screen draws of them. A pad bus record that is not drawn (the bus line is hidden on a healthy bus), the count
  // past what is shown and the pin levels' unused bits change nothing: the pad scanner is polled every 10 ms and an idle healthy screen stays static.
  bool set_stats(const hal::InputStats& s) noexcept;
  // OS-BUG-PADS: the pad bus line is on screen when a pad chip was tried and either did not ack its first transaction (it may have answered since: first and last are both shown) or the pad scanner is
  // not online. A bus that answered from the start shows nothing: the screen is as it was.
  bool pad_bus_trouble() const noexcept;
  // The press counts, the encoder total and the latency back to zero. What is physically down stays down (a button held across a
  // clear would otherwise read as up until it is released): the masks are the picture of the hardware, the counts are the test's.
  void reset() noexcept;
};
static_assert(alignof(InputTestState) >= 4, "InputTestState holds word-aligned count arrays");

}  // namespace o3os::app
