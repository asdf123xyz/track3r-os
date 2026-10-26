// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The rt10xx's DIN MIDI driver (OS-PORT-RT10XX phase 3): LPUART5 receive, LPUART5 transmit driven by the PIT-compare emitter. The same
// offer as platform/mk66 midi.h, which says what runs where (the UART line at priority 48, the timer line at 96, the render at 128); the
// logic of both interrupts is midi/din_io.h's. Implemented in the image only (src/midi_din.cpp); the declarations compile everywhere.

#include <o3os/midi/din_io.h>

namespace o3os::platform::rt10xx::midi_io {

// Clock gates, the UART (31,250 baud, 8N1, FIFOs off), both pads, the PIT module, the NVIC lines. `rx`: the DIN port's midiRt ring (its
// one producer from now on), `din`: the emitter, `ref`: the time base the render publishes. Main thread, after boot::init_interrupts.
bool start(o3os::midi::DinEmitter& din, o3os::midi::RtInRing& rx, const o3os::midi::TimeRef& ref) noexcept;
bool started() noexcept;

// Pends the emitter's interrupt (render context: the NVIC's software trigger). Idempotent: the interrupt decides from the queues.
void kick() noexcept;

// Counters, for `diag` and the hardware session: single words, one writer each (the interrupt), read as they are.
const o3os::midi::DinRxStats& rx_stats() noexcept;
const o3os::midi::DinTxStats& tx_stats() noexcept;

}  // namespace o3os::platform::rt10xx::midi_io
