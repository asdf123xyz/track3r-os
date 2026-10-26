// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/input_test.h>

#include <o3os/app/text_format.h>

namespace o3os::app {

namespace {

void count_press(std::uint8_t& n) noexcept {
  if (n < kInputTestCountMax) {
    ++n;
  }
}

constexpr std::uint32_t kBusCountShown = 999;  // a chip's attempts are shown up to here, then "999+"
constexpr std::uint8_t kLineBits = hal::PadBusStats::kLineScl | hal::PadBusStats::kLineSda | hal::PadBusStats::kLineInt;

// A chip that did not ack its first transaction.
bool refused_first(const hal::BusProbe& p) noexcept { return p.first != hal::BusProbe::kNone && p.first != 0u; }

// Whether the pad bus line is on screen (InputTestState::pad_bus_trouble).
bool bus_trouble(const hal::InputStats& s) noexcept {
  const hal::PadBusStats& bus = s.pad_bus;
  const bool tried = bus.scanner.count != 0u || bus.leds.count != 0u;
  const bool pads_online = (s.online & hal::InputStats::kPadsOnline) != 0u;
  return tried && (refused_first(bus.scanner) || refused_first(bus.leds) || !pads_online);
}

// What the bus line draws of a record, and nothing else: the attempts to 999+, the clear as OK / HELD / "--", the levels as three bits or "---".
std::uint32_t shown_count(std::uint32_t n) noexcept { return n > kBusCountShown ? kBusCountShown + 1u : n; }
std::uint8_t shown_clear(std::uint8_t c) noexcept { return c == hal::PadBusStats::kClearReleased || c == hal::PadBusStats::kClearHeld ? c : hal::BusProbe::kNone; }
std::uint8_t shown_levels(std::uint8_t bits) noexcept { return bits == hal::BusProbe::kNone ? bits : static_cast<std::uint8_t>(bits & kLineBits); }

bool same_probe(const hal::BusProbe& a, const hal::BusProbe& b) noexcept {
  return a.first == b.first && a.last == b.last && shown_count(a.count) == shown_count(b.count);
}

bool same_bus(const hal::PadBusStats& x, const hal::PadBusStats& y) noexcept {
  return same_probe(x.scanner, y.scanner) && same_probe(x.leds, y.leds) && shown_clear(x.clear) == shown_clear(y.clear) && shown_levels(x.lines_first) == shown_levels(y.lines_first) &&
         shown_levels(x.lines_now) == shown_levels(y.lines_now);
}

// Equal in everything the screen draws of the stats: the strip's counters and the two online flags, and the bus line, which exists only on trouble.
bool same_stats(const hal::InputStats& a, const hal::InputStats& b) noexcept {
  constexpr std::uint8_t kOnlineBits = hal::InputStats::kPadsOnline | hal::InputStats::kButtonsOnline;
  const bool trouble = bus_trouble(a);
  return a.dropped == b.dropped && a.errors == b.errors && a.filtered == b.filtered && a.resyncs == b.resyncs && (a.online & kOnlineBits) == (b.online & kOnlineBits) &&
         trouble == bus_trouble(b) && (!trouble || same_bus(a.pad_bus, b.pad_bus));
}

// A chip's first and latest result and its attempts: "NAK/ACK X12", the count stops at "999+".
void append_probe(InputTestBusLine& s, const hal::BusProbe& p) noexcept {
  s.append(hal::bus_result_name(p.first));
  s.append("/");
  s.append(hal::bus_result_name(p.last));
  s.append(" X");
  append_uint(s, p.count > kBusCountShown ? kBusCountShown : p.count);
  if (p.count > kBusCountShown) {
    s.append("+");
  }
}

// The levels of SCL, SDA and INT as three digits (1 = high), "---" when they were not read.
void append_levels(InputTestBusLine& s, std::uint8_t bits) noexcept {
  if (bits == hal::BusProbe::kNone) {
    s.append("---");
    return;
  }
  s.append((bits & hal::PadBusStats::kLineScl) != 0u ? "1" : "0");
  s.append((bits & hal::PadBusStats::kLineSda) != 0u ? "1" : "0");
  s.append((bits & hal::PadBusStats::kLineInt) != 0u ? "1" : "0");
}

}  // namespace

void input_test_bus_line(const hal::PadBusStats& bus, InputTestBusLine& out) noexcept {
  out.clear();
  out.append("BUS 34 ");
  append_probe(out, bus.scanner);
  out.append("  74 ");
  append_probe(out, bus.leds);
  out.append("  CLR ");
  out.append(bus.clear == hal::PadBusStats::kClearReleased ? "OK" : (bus.clear == hal::PadBusStats::kClearHeld ? "HELD" : "--"));
  out.append("  SCL SDA INT ");
  append_levels(out, bus.lines_first);
  out.append(">");
  append_levels(out, bus.lines_now);
}

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

bool InputTestState::pad_bus_trouble() const noexcept { return bus_trouble(stats); }

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
