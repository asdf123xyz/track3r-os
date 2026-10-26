// SPDX-License-Identifier: GPL-3.0-or-later
//
// The DIN MIDI port's words (midi_config.h, midi_pins.h): an exact 31,250 baud from the oscillator, STAT's bits as the S1 byte the shared
// interrupt logic reads, the PIT tick, the pads.
#include <doctest/doctest.h>

#include <o3os/midi/din.h>
#include <o3os/midi/din_io.h>
#include <o3os/platform/rt10xx/midi_config.h>
#include <o3os/platform/rt10xx/midi_pins.h>

namespace mc = o3os::platform::rt10xx::midi_cfg;
namespace pins = o3os::platform::rt10xx::midi_pins;
namespace s1 = o3os::midi::uart_s1;

namespace {
std::uint32_t field(std::uint32_t v, unsigned shift, unsigned bits) { return (v >> shift) & ((1u << bits) - 1u); }
}  // namespace

TEST_CASE("rt10xx midi: the baud word") {
  CHECK(mc::lpuart::kBase == 0x40194000u);
  CHECK(mc::lpuart::kSbr == 48u);
  CHECK(mc::lpuart::kOsr == 16u);
  CHECK(field(mc::lpuart::kBaudWord, 0, 13) == 48u);
  CHECK(field(mc::lpuart::kBaudWord, 24, 5) == 15u);
  CHECK(field(mc::lpuart::kBaudWord, 13, 1) == 0u);  // one stop bit
  CHECK(field(mc::lpuart::kBaudWord, 17, 1) == 0u);  // BOTHEDGE not needed at OSR 16
  CHECK(mc::lpuart::actual_baud(mc::lpuart::kSourceHz, mc::lpuart::kOsr, mc::lpuart::kSbr) == o3os::midi::din::kBaud);
  CHECK(mc::lpuart::kCtrlRun == ((1u << 19) | (1u << 18) | (1u << 21)));
  CHECK(mc::lpuart::kCcgr3Lpuart5 == (3u << 2));
  CHECK(mc::lpuart::kCscdr1UartClkSel == (1u << 6));
}

TEST_CASE("rt10xx midi: STAT as the S1 byte") {
  CHECK(mc::lpuart::s1_of_stat(0) == 0u);
  CHECK(mc::lpuart::s1_of_stat(mc::lpuart::kStatTdre | mc::lpuart::kStatTc) == (s1::kTdre | s1::kTc));
  CHECK(mc::lpuart::s1_of_stat(mc::lpuart::kStatRdrf | mc::lpuart::kStatFraming) == (s1::kRdrf | s1::kFraming));
  CHECK(mc::lpuart::s1_of_stat(mc::lpuart::kStatOverrun | mc::lpuart::kStatNoise | mc::lpuart::kStatParity) == (s1::kOverrun | s1::kNoise | s1::kParity));
  CHECK(mc::lpuart::s1_of_stat(0xFF00FFFFu) == 0u);  // the bits outside 23:16 do not leak into the byte
  CHECK(mc::lpuart::kStatErrorClear == (0xFu << 16));
  CHECK((mc::lpuart::kStatErrorClear & mc::lpuart::kStatRdrf) == 0u);
  CHECK((mc::lpuart::kStatErrorClear & mc::lpuart::kStatIdle) == 0u);
}

TEST_CASE("rt10xx midi: the PIT tick and the byte") {
  CHECK(mc::pit::kBase == 0x40084000u);
  CHECK(mc::pit::ldval(0) == 0x40084100u);
  CHECK(mc::pit::tctrl(0) == 0x40084108u);
  CHECK(mc::pit::tflg(0) == 0x4008410Cu);
  CHECK(mc::pit::kCyclesPerTickShift == 3u);
  CHECK(mc::pit::kByteTicks == 24000u);
  CHECK(mc::pit::kByteCyclesAtCore == 192000u);
  CHECK(mc::pit::kCcgr1Pit == (3u << 12));
  CHECK(o3os::midi::DinTx::ticks_for(mc::pit::kByteCyclesAtCore, mc::pit::kCyclesPerTickShift) == 24000u);
  if (o3os::platform::rt10xx::clock::kTreeIsRt10xx) {
    CHECK(o3os::midi::din::kByteCycles == 192000u);
  }
  CHECK(o3os::midi::DinTx::ticks_for(8, mc::pit::kCyclesPerTickShift) == o3os::midi::DinTx::kMinTicks);
  CHECK(mc::kAudioLeadCycles == 2u * o3os::hal::audio::kCyclesPerBlock);
}

TEST_CASE("rt10xx midi: the pads") {
  CHECK(pins::kTx.alt == 1u);
  CHECK(pins::kRx.alt == 1u);
  CHECK(pins::kTx.select_input == 0x401F8544u);
  CHECK(pins::kRx.select_input == 0x401F8540u);
  CHECK(field(pins::kRxPadWord, 14, 2) == 3u);  // 22 kOhm pull-up
  CHECK(field(pins::kRxPadWord, 16, 1) == 1u);  // hysteresis
  CHECK(field(pins::kTxPadWord, 12, 1) == 0u);  // no keeper or pull on the output
}
