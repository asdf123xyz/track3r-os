// SPDX-License-Identifier: GPL-3.0-or-later
//
// The rt10xx's DIN MIDI port (OS-PORT-RT10XX phase 3): LPUART5 and the one PIT channel of the emitter. The structure is the MK66 driver's
// (platform/mk66 src/midi_din.cpp); the logic of the two interrupts is midi/din_io.h, the values written here are midi_config.h's (checked by
// the compiler and platform/rt10xx/tests/rt10xx_midi_config_test.cpp), the pads midi_pins.h's assumption.
//
// SEQUENCE (start(); gates, peripheral, pads, interrupt lines last):
//   1. CSCDR1: the UART root from the oscillator; CCGR3[lpuart5] and CCGR1[pit] on.
//   2. PIT: the module on (MCR = 0), channel 0 stopped with its flag clear.
//   3. LPUART5: a module reset, then BAUD, CTRL = 0 (8N1), MODIR = 0, PINCFG = 0, the FIFOs flushed and off, WATER = 0.
//   4. The RX pad to the UART (pull-up, hysteresis), CTRL = TE | RE | RIE, a read of DATA and a clear of the error flags to drop anything the
//      line held, then the TX pad to the UART: with TE set the UART drives the line idle-high before the pad is handed to it.
//   5. The NVIC: pending cleared, LPUART5 and PIT enabled (their priorities were written at boot from the table: 48 and 96).
#include "internal.h"
#include "regs.h"

#include <o3os/midi/din_io.h>
#include <o3os/platform/rt10xx/irq_map.h>
#include <o3os/platform/rt10xx/midi.h>
#include <o3os/platform/rt10xx/midi_config.h>
#include <o3os/platform/rt10xx/midi_pins.h>
#include <o3os/platform/rt10xx/resources.h>
#include <o3os/platform/rt10xx/sections.h>

#include <cstdint>

namespace o3os::platform::rt10xx::midi_io {
namespace {

namespace lp = midi_cfg::lpuart;
namespace pit = midi_cfg::pit;
using regs::at;

constexpr unsigned kRxIrq = static_cast<unsigned>(Irq::LPUART5);
constexpr unsigned kPitIrq = static_cast<unsigned>(Irq::PIT);
static_assert(role_of(Irq::LPUART5) == hal::irq::Role::MidiUart && role_of(Irq::PIT) == hal::irq::Role::MidiOutTimer,
              "the DIN lines carry the priorities of their rows in the table of ADR 0013 D1 (48 and 96)");
static_assert(pit::kChannel == resources::kMidiPitChannel, "the emitter's PIT channel is the one resources.h reserves for it");

// The register policy of midi/din_io.h: LPUART5 and PIT channel 0.
struct Hw {
  static constexpr unsigned kCyclesPerTickShift = pit::kCyclesPerTickShift;
  std::uint32_t cycles() const noexcept { return regs::dwt_cyccnt(); }
  std::uint8_t status() const noexcept { return lp::s1_of_stat(at<std::uint32_t>(lp::kStat)); }
  // DATA then the error flags: reading DATA clears RDRF; OR, NF, FE and PF are write-1-to-clear on this chip (the MK66's clear on the read).
  std::uint8_t data() const noexcept {
    const std::uint8_t byte = static_cast<std::uint8_t>(at<std::uint32_t>(lp::kData) & lp::kDataMask);
    at<std::uint32_t>(lp::kStat) = lp::kStatErrorClear;
    return byte;
  }
  bool tx_ready() const noexcept { return (at<std::uint32_t>(lp::kStat) & lp::kStatTdre) != 0u; }
  void tx_write(std::uint8_t byte) const noexcept { at<std::uint32_t>(lp::kData) = byte; }
  void timer_stop() const noexcept {
    at<std::uint32_t>(pit::tctrl(pit::kChannel)) = 0;
    at<std::uint32_t>(pit::tflg(pit::kChannel)) = pit::kTflgTif;
  }
  void timer_start(std::uint32_t ticks) const noexcept {
    at<std::uint32_t>(pit::ldval(pit::kChannel)) = ticks - 1u;
    at<std::uint32_t>(pit::tctrl(pit::kChannel)) = pit::kTctrlTen | pit::kTctrlTie;
  }
};
constexpr Hw kHw{};

midi::DinEmitter* volatile g_din = nullptr;
midi::RtInRing* volatile g_rx = nullptr;
const midi::TimeRef* volatile g_ref = nullptr;
midi::DinRxStats g_rx_stats;
midi::DinTxStats g_tx_stats;
volatile bool g_started = false;

void mux_pad(const midi_pins::Pad& pad, std::uint32_t pad_word) noexcept {
  at<std::uint32_t>(pad.pad_ctl) = pad_word;
  at<std::uint32_t>(pad.select_input) = pad.select_value;
  at<std::uint32_t>(pad.mux_ctl) = pad.alt;
}

}  // namespace

bool start(midi::DinEmitter& din, midi::RtInRing& rx, const midi::TimeRef& ref) noexcept {
  if (g_started) {
    return true;
  }
  g_din = &din;
  g_rx = &rx;
  g_ref = &ref;

  // 1. The UART root and the gates.
  at<std::uint32_t>(lp::kCscdr1) = (at<std::uint32_t>(lp::kCscdr1) & ~lp::kCscdr1UartPodfMask) | lp::kCscdr1UartClkSel;
  at<std::uint32_t>(lp::kCcgr3) |= lp::kCcgr3Lpuart5;
  at<std::uint32_t>(pit::kCcgr1) |= pit::kCcgr1Pit;

  // 2. The timer.
  at<std::uint32_t>(pit::kMcr) = pit::kMcrEnable;
  kHw.timer_stop();

  // 3. The UART, reset and configured, still off.
  at<std::uint32_t>(lp::kGlobal) = lp::kGlobalReset;
  at<std::uint32_t>(lp::kGlobal) = 0;
  at<std::uint32_t>(lp::kCtrl) = 0;
  at<std::uint32_t>(lp::kBaud) = lp::kBaudWord;
  at<std::uint32_t>(lp::kModir) = 0;
  at<std::uint32_t>(lp::kPincfg) = 0;
  at<std::uint32_t>(lp::kFifo) = lp::kFifoFlush | lp::kFifoWord;
  at<std::uint32_t>(lp::kWater) = 0;

  // 4. Pads and enable.
  mux_pad(midi_pins::kRx, midi_pins::kRxPadWord);
  at<std::uint32_t>(lp::kCtrl) = lp::kCtrlRun;
  (void)kHw.data();  // drops a byte and the flags the line held
  mux_pad(midi_pins::kTx, midi_pins::kTxPadWord);

  // 5. The interrupt lines, last.
  asm volatile("dsb" ::: "memory");
  regs::nvic_icpr(kRxIrq / 32u) = regs::irq_bit(kRxIrq);
  regs::nvic_icpr(kPitIrq / 32u) = regs::irq_bit(kPitIrq);
  regs::nvic_iser(kRxIrq / 32u) = regs::irq_bit(kRxIrq);
  regs::nvic_iser(kPitIrq / 32u) = regs::irq_bit(kPitIrq);
  g_started = true;
  return true;
}

bool started() noexcept { return g_started; }

void kick() noexcept {
  if (g_started) {
    regs::nvic_stir() = kPitIrq;
  }
}

const midi::DinRxStats& rx_stats() noexcept { return g_rx_stats; }
const midi::DinTxStats& tx_stats() noexcept { return g_tx_stats; }

}  // namespace o3os::platform::rt10xx::midi_io

// LPUART5 (line 24, priority 48): every byte is stamped and queued. ITCM: nothing it calls is in flash.
extern "C" O3OS_RAMFUNC_L void LPUART5_IRQHandler() noexcept {
  namespace io = o3os::platform::rt10xx::midi_io;
  o3os::midi::RtInRing* const rx = io::g_rx;
  if (rx == nullptr) {
    return;
  }
  o3os::midi::DinRx::isr(io::kHw, *rx, io::g_rx_stats);
}

// The PIT's line (122, priority 96; channel 0 is the only one armed): the emitter. Entered by the compare and by the render's software pend.
extern "C" O3OS_RAMFUNC_L void PIT_IRQHandler() noexcept {
  namespace io = o3os::platform::rt10xx::midi_io;
  o3os::midi::DinEmitter* const din = io::g_din;
  const o3os::midi::TimeRef* const ref = io::g_ref;
  if (din == nullptr || ref == nullptr) {
    io::kHw.timer_stop();
    return;
  }
  o3os::midi::DinTx::on_irq(io::kHw, *din, *ref, io::g_tx_stats);
}
