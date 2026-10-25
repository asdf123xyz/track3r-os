// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/input_test.h>

namespace o3os::app {

namespace {

void count_press(std::uint8_t& n) noexcept {
  if (n < kInputTestCountMax) {
    ++n;
  }
}

bool same_stats(const hal::InputStats& a, const hal::InputStats& b) noexcept {
  return a.dropped == b.dropped && a.errors == b.errors && a.filtered == b.filtered && a.resyncs == b.resyncs && a.online == b.online;
}

}  // namespace

bool InputTestState::observe(const core::InputEvent& event, core::Tick now) noexcept {
  ++events;
  last_age_us = core::tick_elapsed(now, event.timestamp);
  if (last_age_us > max_age_us) {
    max_age_us = last_age_us;
  }
  const bool down = event.value != 0;
  switch (event.kind) {
    case core::InputKind::Button:
      if (event.id >= kButtonCount) {
        return false;
      }
      if (down) {
        if ((buttons_down & (std::uint64_t{1} << event.id)) == 0u) {
          count_press(button_presses[event.id]);
        }
        buttons_down |= std::uint64_t{1} << event.id;
      } else {
        buttons_down &= ~(std::uint64_t{1} << event.id);
      }
      return true;
    case core::InputKind::Pad:
      if (event.id >= kInputTestPads) {
        return false;
      }
      if (down) {
        if ((pads_down & (std::uint64_t{1} << event.id)) == 0u) {
          count_press(pad_presses[event.id]);
        }
        pads_down |= std::uint64_t{1} << event.id;
      } else {
        pads_down &= ~(std::uint64_t{1} << event.id);
      }
      return true;
    case core::InputKind::Encoder:
      encoder_total += event.value;
      encoder_last = event.value;
      ++encoder_events;
      return true;
    default:
      return false;  // the PC keyboard, HID and MIDI kinds are not on the device's panel
  }
}

bool InputTestState::set_stats(const hal::InputStats& s) noexcept {
  if (same_stats(stats, s)) {
    return false;
  }
  stats = s;
  return true;
}

void InputTestState::reset() noexcept {
  for (std::uint8_t& n : button_presses) {
    n = 0;
  }
  for (std::uint8_t& n : pad_presses) {
    n = 0;
  }
  encoder_total = 0;
  encoder_last = 0;
  encoder_events = 0;
  events = 0;
  last_age_us = 0;
  max_age_us = 0;
}

}  // namespace o3os::app
