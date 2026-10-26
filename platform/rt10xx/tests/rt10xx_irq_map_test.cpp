// SPDX-License-Identifier: GPL-3.0-or-later
//
// The interrupt list and map (irq_list.h, irq_map.h): 160 lines in order, the roles of the lines the board uses, the rest Unassigned.
#include <doctest/doctest.h>

#include <o3os/platform/rt10xx/irq_map.h>

namespace rt10xx = o3os::platform::rt10xx;
using o3os::hal::irq::Role;

TEST_CASE("rt10xx irq_list: 160 device interrupts, the observed lines by number") {
  CHECK(rt10xx::kIrqCount == 160u);
  CHECK(static_cast<unsigned>(rt10xx::Irq::DMA0_DMA16) == 0u);     // the official firmware's audio-out DMA line (FINDINGS 12)
  CHECK(static_cast<unsigned>(rt10xx::Irq::LPUART5) == 24u);       // the DIN MIDI jack (FINDINGS 12)
  CHECK(static_cast<unsigned>(rt10xx::Irq::USB_OTG1) == 113u);
  CHECK(static_cast<unsigned>(rt10xx::Irq::USDHC1) == 110u);
  CHECK(static_cast<unsigned>(rt10xx::Irq::SEMC) == 109u);
  CHECK(static_cast<unsigned>(rt10xx::Irq::PIT) == 122u);
  CHECK(static_cast<unsigned>(rt10xx::Irq::NMI_WAKEUP) == 159u);
}

TEST_CASE("rt10xx irq_map: roles") {
  CHECK(rt10xx::role_of(rt10xx::Irq::DMA0_DMA16) == Role::AudioDma);
  CHECK(rt10xx::role_of(rt10xx::Irq::LPUART5) == Role::MidiUart);
  CHECK(rt10xx::role_of(rt10xx::Irq::USB_OTG1) == Role::Usb);
  CHECK(rt10xx::role_of(rt10xx::Irq::USDHC1) == Role::Completion);
  CHECK(rt10xx::role_of(rt10xx::Irq::LPSPI1) == Role::Completion);
  CHECK(rt10xx::role_of(rt10xx::Irq::LPI2C1) == Role::Completion);
  CHECK(rt10xx::role_of(rt10xx::Irq::PIT) == Role::MidiOutTimer);
  CHECK(rt10xx::role_of(rt10xx::kRenderLine) == Role::Render);
  CHECK(rt10xx::role_of(rt10xx::Irq::GPIO1_Combined_0_15) == Role::PinChange);
  CHECK(rt10xx::role_of(rt10xx::Irq::LPUART1) == Role::Unassigned);
  CHECK(rt10xx::role_of(rt10xx::Irq::SEMC) == Role::Unassigned);
  CHECK(rt10xx::lines_unique());
  // Every priority the boot code writes is a multiple of 16: the four implemented bits.
  for (unsigned line = 0; line < rt10xx::kIrqCount; ++line) {
    CHECK(o3os::hal::irq::priority(rt10xx::role_of(static_cast<rt10xx::Irq>(line))) % 16u == 0u);
  }
}
