// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Which device interrupt line has which role in the priority table of ADR 0013 section D1 (hal/irq.h), on the rt10xx (OS-PORT-RT10XX).
// The table of priorities is the HAL's; this file only says which lines belong to which row, and boot::init_interrupts() writes it into
// the NVIC at boot. A line not listed here is Unassigned (below everything with a role), so a peripheral enabled before its row is
// written here can never preempt the audio path. The Cortex-M7 of this part implements four priority bits, like the MK66's Cortex-M4,
// so the HAL's values (multiples of 16) apply unchanged.
//
// The lines are the ones the Tracker+ board is known or expected to use (docs/specs/OS-PORT-RT10XX.md section 2): the DIN MIDI jack is
// LPUART5 (observed); the audio-out DMA raises line 0 (observed); USB is the OTG1 controller (the USB pins are the chip's dedicated
// ones); the card is USDHC1 and the display's SPI an LPSPI (expected, the instances are open research questions, so all four LPSPI lines
// and both USDHC lines carry the completion role). Nothing in this phase enables any of them.

#include <o3os/hal/irq.h>
#include <o3os/platform/rt10xx/irq_list.h>

#include <cstdint>

namespace o3os::platform::rt10xx {

struct LineRole {
  Irq line;
  hal::irq::Role role;
};

inline constexpr LineRole kLineRoles[] = {
    // The audio DMA channel's line (the eDMA channel 0 the official firmware's audio out uses; this port's channel is chosen in phase 3,
    // resources.h then names it and checks it against this table) and the SAI lines.
    {Irq::DMA0_DMA16, hal::irq::Role::AudioDma},
    {Irq::SAI1, hal::irq::Role::AudioDma},
    {Irq::SAI2, hal::irq::Role::AudioDma},
    // MIDI DIN: LPUART5.
    {Irq::LPUART5, hal::irq::Role::MidiUart},
    {Irq::USB_OTG1, hal::irq::Role::Usb},
    // Completion flags only: the card, the display's SPI and its DMA channel (named in phase 2), the I2C buses.
    {Irq::USDHC1, hal::irq::Role::Completion},
    {Irq::USDHC2, hal::irq::Role::Completion},
    {Irq::LPSPI1, hal::irq::Role::Completion},
    {Irq::LPSPI2, hal::irq::Role::Completion},
    {Irq::LPSPI3, hal::irq::Role::Completion},
    {Irq::LPSPI4, hal::irq::Role::Completion},
    {Irq::LPI2C1, hal::irq::Role::Completion},
    {Irq::LPI2C2, hal::irq::Role::Completion},
    {Irq::LPI2C3, hal::irq::Role::Completion},
    {Irq::LPI2C4, hal::irq::Role::Completion},
    // The MIDI-out emitter's timer (ADR 0013 D5): one PIT channel; the RT10xx's four PIT channels share one line.
    {Irq::PIT, hal::irq::Role::MidiOutTimer},
    // The software-pended render line (D1): a reserved line, pended through the NVIC. PendSV is the fallback.
    {Irq::Reserved86, hal::irq::Role::Render},
    // Pad, button and encoder pin interrupts: the GPIO ports' combined lines.
    {Irq::GPIO1_Combined_0_15, hal::irq::Role::PinChange},
    {Irq::GPIO1_Combined_16_31, hal::irq::Role::PinChange},
    {Irq::GPIO2_Combined_0_15, hal::irq::Role::PinChange},
    {Irq::GPIO2_Combined_16_31, hal::irq::Role::PinChange},
    {Irq::GPIO3_Combined_0_15, hal::irq::Role::PinChange},
    {Irq::GPIO3_Combined_16_31, hal::irq::Role::PinChange},
    {Irq::GPIO4_Combined_0_15, hal::irq::Role::PinChange},
    {Irq::GPIO4_Combined_16_31, hal::irq::Role::PinChange},
    {Irq::GPIO5_Combined_0_15, hal::irq::Role::PinChange},
    {Irq::GPIO5_Combined_16_31, hal::irq::Role::PinChange},
};
inline constexpr unsigned kLineRoleCount = sizeof(kLineRoles) / sizeof(kLineRoles[0]);

constexpr hal::irq::Role role_of(Irq line) noexcept {
  for (const LineRole& entry : kLineRoles) {
    if (entry.line == line) {
      return entry.role;
    }
  }
  return hal::irq::Role::Unassigned;
}

// The render line, pended by the audio DMA ISR through NVIC_ISPR (phase 3).
inline constexpr Irq kRenderLine = Irq::Reserved86;

// Cortex-M7 system handlers (exception numbers 4..15), per the same table.
inline constexpr hal::irq::Role kMemManageRole = hal::irq::Role::Fault;
inline constexpr hal::irq::Role kBusFaultRole = hal::irq::Role::Fault;
inline constexpr hal::irq::Role kUsageFaultRole = hal::irq::Role::Fault;
inline constexpr hal::irq::Role kSvCallRole = hal::irq::Role::Unassigned;
inline constexpr hal::irq::Role kPendSvRole = hal::irq::Role::Render;
inline constexpr hal::irq::Role kSysTickRole = hal::irq::Role::SysTick;

constexpr bool lines_unique() noexcept {
  for (unsigned i = 0; i < kLineRoleCount; ++i) {
    for (unsigned j = i + 1; j < kLineRoleCount; ++j) {
      if (kLineRoles[i].line == kLineRoles[j].line) {
        return false;
      }
    }
  }
  return true;
}
static_assert(lines_unique(), "each line appears once in the table");
static_assert(role_of(kRenderLine) == hal::irq::Role::Render, "the render line has the render role");
static_assert(role_of(Irq::LPUART1) == hal::irq::Role::Unassigned, "an unlisted line is Unassigned");

}  // namespace o3os::platform::rt10xx
