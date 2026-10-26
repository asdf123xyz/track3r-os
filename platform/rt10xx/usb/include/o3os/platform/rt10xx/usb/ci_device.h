// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The USB device driver of the rt10xx (OS-PORT-RT10XX phase 2) for the chip's OTG1 controller, an EHCI-derived dual-role controller in
// device mode (the i.MX RT1050 Reference Manual's "USB" chapter: endpoint queue heads and transfer descriptors in memory, primed and
// completed through the ENDPT* registers). It does for this controller what usb/device.h does for the MK66's: endpoint 0 is the usb
// layer's ControlPipe, the CDC port moves the usb layer's chunk rings (the `diag` console behind them), the MIDI endpoints move the MIDI
// event rings. The register policy `Hw` (the comment below) is the image's over the real registers (usb/src/usb1.cpp) and a fake in the
// host tests (tests/rt10xx_usb_ci_test.cpp), so every path here runs on the host.
//
// Design, kept simple on purpose for a probe:
//   * one transfer descriptor per endpoint direction, primed only when that direction is idle (no descriptor list appends, no tripwire
//     on the list), so a direction is "armed" or "idle" and nothing else;
//   * full speed forced (the image's Hw sets PFSC): bulk packets are 64 bytes, the size the CDC chunks are;
//   * the start-of-frame interrupt polls the rings once a millisecond for the directions that were idle (the MK66 driver's method);
//   * the memory (queue heads, descriptors, packet buffers) is one struct the image places in the OCRAM, where the controller's bus
//     master reaches it and the core does not cache it; the driver only ever hands the controller `hw.bus_address(pointer)`, so a host
//     test can map its own memory;
//   * contexts: isr() is the USB interrupt; the main loop only reads the counters and the control pipe's atomics and writes the rings'
//     producer sides, as with the MK66 driver.
//
// struct Hw {
//   std::uint32_t usbsts();                 void usbsts_clear(std::uint32_t bits);
//   std::uint32_t usbcmd();                 void usbcmd(std::uint32_t value);
//   std::uint32_t portsc();                 void portsc(std::uint32_t value);
//   std::uint32_t endptsetupstat();         void endptsetupstat_clear(std::uint32_t bits);
//   std::uint32_t endptcomplete();          void endptcomplete_clear(std::uint32_t bits);
//   std::uint32_t endptprime();             void endptprime_set(std::uint32_t bits);
//   std::uint32_t endptstat();
//   std::uint32_t endptflush();             void endptflush_set(std::uint32_t bits);
//   std::uint32_t endptctrl(unsigned n);    void endptctrl(unsigned n, std::uint32_t value);
//   void deviceaddr(std::uint32_t value);   // the whole register (address << 25)
//   void controller_reinit();               // a bus reset that completed before the interrupt ran: the controller is reset and set up again (usb1.cpp)
//   std::uint32_t bus_address(const void* p);  // what the controller is given for a pointer
//   void dmb();                             // a store barrier between filling a descriptor and handing it over
//   std::uint32_t cycles();                 // a free-running cycle counter, for the interrupt's own cost
// };

#include <o3os/platform/rt10xx/usb/soc_facts.h>
#include <o3os/usb/cdc.h>
#include <o3os/usb/control.h>
#include <o3os/usb/usb_types.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace o3os::platform::rt10xx::usb {

// ---- The data structures (RM "Device Data Structures") ----------------------------------------------------------------------------
struct alignas(32) Dtd {
  volatile std::uint32_t next;       // the next descriptor, or kTerminate
  volatile std::uint32_t token;      // 30:16 total bytes, 15 IOC, 11:10 MultO, 7:0 status
  volatile std::uint32_t buffer[5];  // 4 KiB pages; the first carries the offset in its low 12 bits
  std::uint32_t reserved;
};
static_assert(sizeof(Dtd) == soc::kTransferDescriptorBytes && alignof(Dtd) == 32, "a transfer descriptor is 32 bytes, 32-aligned");

struct alignas(64) Dqh {
  volatile std::uint32_t capabilities;  // 31:30 mult, 29 ZLT (1 = no zero-length termination), 26:16 max packet, 15 IOS
  volatile std::uint32_t current;       // the current descriptor (the controller's)
  volatile std::uint32_t next;          // the next descriptor to run, or kTerminate
  volatile std::uint32_t token;         // a copy of the running descriptor's token
  volatile std::uint32_t buffer[5];
  std::uint32_t reserved;
  volatile std::uint32_t setup[2];      // the 8 bytes of the last SETUP (endpoint 0 receive)
  std::uint32_t pad[4];
};
static_assert(sizeof(Dqh) == soc::kQueueHeadBytes && alignof(Dqh) == 64, "a queue head is 64 bytes, 64-aligned");

inline constexpr std::uint32_t kTerminate = 1u;
inline constexpr std::uint32_t kTokenActive = 1u << 7;
inline constexpr std::uint32_t kTokenHalted = 1u << 6;
inline constexpr std::uint32_t kTokenBufferError = 1u << 5;
inline constexpr std::uint32_t kTokenTransactionError = 1u << 3;
inline constexpr std::uint32_t kTokenIoc = 1u << 15;
inline constexpr std::uint32_t kTokenBytesShift = 16;
inline constexpr std::uint32_t kTokenBytesMask = 0x7FFFu;
inline constexpr std::uint32_t kCapZeroLengthOff = 1u << 29;
inline constexpr std::uint32_t kCapMaxPacketShift = 16;
inline constexpr std::uint32_t kCapInterruptOnSetup = 1u << 15;

// The endpoints this driver uses (usb_types.h gives their addresses): 0 control, 1 the CDC notification (interrupt IN, never armed),
// 2 the CDC data pair, 3 the MIDI pair. Queue head index: number x 2 + (transmit ? 1 : 0).
inline constexpr unsigned kEndpoints = 4;
inline constexpr unsigned kDirections = 2 * kEndpoints;
inline constexpr std::uint32_t kPacketBytes = 64;
constexpr unsigned slot(unsigned number, bool transmit) noexcept { return number * 2u + (transmit ? 1u : 0u); }
static_assert((o3os::usb::layout::kEpCdcOut & 0x0Fu) == 2u && (o3os::usb::layout::kEpCdcIn & 0x0Fu) == 2u && (o3os::usb::layout::kEpMidiOut & 0x0Fu) == 3u &&
                  (o3os::usb::layout::kEpMidiIn & 0x0Fu) == 3u && (o3os::usb::layout::kEpCdcNotify & 0x0Fu) == 1u,
              "the endpoint numbers of usb_types.h are the ones this driver lays out");
inline constexpr unsigned kCdcNumber = 2;
inline constexpr unsigned kMidiNumber = 3;
inline constexpr unsigned kNotifyNumber = 1;

// Everything the controller touches, in one object the image places in the OCRAM (O3OS_OCRAM_NOINIT): the queue head list first, at its
// 2 KiB alignment.
struct alignas(2048) Memory {
  Dqh heads[kDirections];
  Dtd descriptors[kDirections];
  std::uint8_t buffers[kDirections][kPacketBytes];
};
static_assert(sizeof(Memory) <= 2048u + 512u, "about 1.5 KiB of OCRAM");
static_assert(offsetof(Memory, heads) == 0, "the queue head list is at the alignment");

struct CiCounters {
  std::uint32_t isr_count = 0;
  std::uint32_t isr_cycles_last = 0;
  std::uint32_t isr_cycles_max = 0;
  std::uint32_t bus_resets = 0;
  std::uint32_t late_resets = 0;        // the reset had completed before the interrupt ran: the controller was set up again
  std::uint32_t port_changes = 0;
  std::uint32_t suspends = 0;
  std::uint32_t resumes = 0;
  std::uint32_t sofs = 0;
  std::uint32_t sof_cycles = 0;         // the cycle counter's advance between the last two starts of frame: the core clock against the host's millisecond (`diag cpu`)
  std::uint32_t setups = 0;
  std::uint32_t setup_rereads = 0;      // the tripwire caught a SETUP arriving while one was being read
  std::uint32_t ep0_stalls = 0;
  std::uint32_t completions[kDirections] = {};
  std::uint32_t descriptor_errors = 0;  // a completed descriptor whose status says halted or error
  std::uint32_t stray_completions = 0;  // a completion bit for a direction nothing had armed
  std::uint32_t cdc_tx_packets = 0;
  std::uint32_t cdc_tx_bytes = 0;
  std::uint32_t cdc_rx_packets = 0;
  std::uint32_t cdc_rx_bytes = 0;
  std::uint32_t cdc_rx_holds = 0;       // the receive side left idle because the ring held kCdcRxHoldChunks
  std::uint32_t midi_in_packets = 0;
  std::uint32_t midi_out_packets = 0;
  std::uint32_t midi_out_events = 0;
  std::uint32_t errors = 0;             // USBSTS UEI
  std::uint32_t system_errors = 0;      // USBSTS SEI
  std::uint32_t speed = 0;              // PORTSC PSPD at the last port change: 0 full, 1 low, 2 high
};

template <class Hw>
class CiDevice {
 public:
  using ControlPipe = o3os::usb::ControlPipe;
  using ControlStep = o3os::usb::ControlStep;
  using Effect = o3os::usb::Effect;

  CiDevice(Hw& hw, Memory& memory, ControlPipe& control, o3os::usb::CdcTxRing& cdc_tx, o3os::usb::CdcRxRing& cdc_rx,
           o3os::usb::MidiRing& midi_to_host, o3os::usb::MidiRing& midi_from_host) noexcept
      : hw_(hw), m_(memory), ctl_(control), cdc_tx_(cdc_tx), cdc_rx_(cdc_rx), midi_in_(midi_to_host), midi_out_(midi_from_host) {}

  // Puts the data structures and the controller's endpoint state where a bus reset leaves them: every head initialised, endpoint 0
  // armed to receive, the others disabled, address 0. Called once before the controller is run (the D+ pull-up), and by every bus reset.
  void prepare() noexcept {
    reset_endpoints();
    ctl_.bus_reset();
    hw_.deviceaddr(0);
    arm_ep0_out();
  }

  // The USB interrupt.
  void isr() noexcept {
    const std::uint32_t entered = hw_.cycles();
    ++c_.isr_count;
    const std::uint32_t status = hw_.usbsts() & soc::kIntrWorkingSet;
    hw_.usbsts_clear(status);
    if ((status & soc::kStsReset) != 0u) {
      on_bus_reset();
    }
    if ((status & soc::kStsPortChange) != 0u) {
      on_port_change();
    }
    if ((status & soc::kStsUsbInt) != 0u) {
      const std::uint32_t setups = hw_.endptsetupstat();
      if ((setups & 1u) != 0u) {
        on_setup();
      }
      const std::uint32_t complete = hw_.endptcomplete();
      if (complete != 0u) {
        hw_.endptcomplete_clear(complete);
        on_complete(complete);
      }
    }
    if ((status & soc::kStsSof) != 0u) {
      ++c_.sofs;
      c_.sof_cycles = entered - last_sof_cycles_;
      last_sof_cycles_ = entered;
      service_rings();
    }
    if ((status & soc::kStsSuspend) != 0u) {
      ++c_.suspends;
      ctl_.on_suspend();
    }
    if ((status & soc::kStsUsbError) != 0u) {
      ++c_.errors;
    }
    if ((status & soc::kStsSystemError) != 0u) {
      ++c_.system_errors;
    }
    const std::uint32_t spent = hw_.cycles() - entered;
    c_.isr_cycles_last = spent;
    if (spent > c_.isr_cycles_max) {
      c_.isr_cycles_max = spent;
    }
  }

  // The main loop's nudge (after it published to a ring, instead of waiting for the next start of frame): the same work the SOF does.
  // Only with the interrupt masked by the caller, or from the interrupt itself.
  void service_rings() noexcept {
    if (!ctl_.configured()) {
      return;
    }
    if (!armed_[slot(kCdcNumber, true)]) {
      arm_cdc_in();
    }
    if (!armed_[slot(kCdcNumber, false)]) {
      arm_cdc_out();
    }
    if (!armed_[slot(kMidiNumber, true)]) {
      arm_midi_in();
    }
    if (!armed_[slot(kMidiNumber, false)]) {
      arm_bulk_out(kMidiNumber);
    }
  }

  const CiCounters& counters() const noexcept { return c_; }
  bool armed(unsigned number, bool transmit) const noexcept { return armed_[slot(number, transmit)]; }

 private:
  // ---- Endpoint state ----------------------------------------------------------------------------------------------------------
  void init_head(unsigned s, std::uint32_t max_packet, bool ios) noexcept {
    Dqh& q = m_.heads[s];
    q.capabilities = kCapZeroLengthOff | (max_packet << kCapMaxPacketShift) | (ios ? kCapInterruptOnSetup : 0u);
    q.current = 0;
    q.next = kTerminate;
    q.token = 0;
    for (volatile std::uint32_t& b : q.buffer) {
      b = 0;
    }
    q.setup[0] = 0;
    q.setup[1] = 0;
  }

  void reset_endpoints() noexcept {
    // Nothing may be in flight: wait for the primes to clear, then flush every direction (the RM's bus-reset recipe).
    for (unsigned i = 0; i < 1000u && hw_.endptprime() != 0u; ++i) {
    }
    hw_.endptflush_set(0xFFFFFFFFu);
    for (unsigned i = 0; i < 1000u && hw_.endptflush() != 0u; ++i) {
    }
    for (unsigned s = 0; s < kDirections; ++s) {
      init_head(s, kPacketBytes, s == slot(0, false));
      armed_[s] = false;
      armed_len_[s] = 0;
    }
    // Endpoint 0 is a control endpoint in both directions; the others are off until SET_CONFIGURATION.
    hw_.endptctrl(0, soc::kEpRxEnable | soc::kEpTxEnable);
    for (unsigned n = 1; n < kEndpoints; ++n) {
      hw_.endptctrl(n, 0);
    }
  }

  // Arms one direction with `len` bytes of its buffer (an IN sends them, an OUT receives up to them): one descriptor, primed when idle.
  void arm(unsigned number, bool transmit, std::uint32_t len) noexcept {
    const unsigned s = slot(number, transmit);
    Dtd& d = m_.descriptors[s];
    Dqh& q = m_.heads[s];
    d.next = kTerminate;
    d.token = (len << kTokenBytesShift) | kTokenIoc | kTokenActive;
    const std::uint32_t address = hw_.bus_address(m_.buffers[s]);
    d.buffer[0] = address;
    for (unsigned i = 1; i < 5u; ++i) {
      d.buffer[i] = (address & ~0xFFFu) + 0x1000u * i;  // the following pages, never reached by a 64-byte transfer
    }
    q.next = hw_.bus_address(&d);
    q.token = 0;  // the head's active and halt bits clear
    hw_.dmb();
    armed_[s] = true;
    armed_len_[s] = len;
    hw_.endptprime_set(soc::endpoint_bit(number, transmit));
  }

  // The bytes a completed descriptor moved, and whether it ended well.
  std::uint32_t completed_bytes(unsigned s, bool& ok) noexcept {
    const std::uint32_t token = m_.descriptors[s].token;
    ok = (token & (kTokenHalted | kTokenBufferError | kTokenTransactionError)) == 0u;
    const std::uint32_t remaining = (token >> kTokenBytesShift) & kTokenBytesMask;
    return remaining <= armed_len_[s] ? armed_len_[s] - remaining : 0u;
  }

  void arm_ep0_out() noexcept { arm(0, false, kPacketBytes); }

  // ---- Endpoint 0 -----------------------------------------------------------------------------------------------------------------
  void on_setup() noexcept {
    ++c_.setups;
    // A SETUP ends whatever transfer was in progress on endpoint 0: flush both directions, then read the packet under the tripwire.
    hw_.endptsetupstat_clear(1u);
    const std::uint32_t both = soc::endpoint_bit(0, false) | soc::endpoint_bit(0, true);
    hw_.endptflush_set(both);
    for (unsigned i = 0; i < 1000u && (hw_.endptflush() & both) != 0u; ++i) {
    }
    armed_[slot(0, false)] = false;
    armed_[slot(0, true)] = false;
    std::uint8_t raw[8];
    for (unsigned tries = 0; tries < 8u; ++tries) {
      hw_.usbcmd(hw_.usbcmd() | soc::kCmdSetupTripwire);
      const std::uint32_t w0 = m_.heads[slot(0, false)].setup[0];
      const std::uint32_t w1 = m_.heads[slot(0, false)].setup[1];
      std::memcpy(raw, &w0, 4);
      std::memcpy(raw + 4, &w1, 4);
      if ((hw_.usbcmd() & soc::kCmdSetupTripwire) != 0u) {
        break;
      }
      ++c_.setup_rereads;
    }
    hw_.usbcmd(hw_.usbcmd() & ~soc::kCmdSetupTripwire);
    // A stall of a previous request ends with the new SETUP (the controller clears the stall bits itself; stated here for the fake).
    hw_.endptctrl(0, soc::kEpRxEnable | soc::kEpTxEnable);
    arm_ep0_out();  // the status OUT or the data OUT of this request
    apply(ctl_.on_setup(raw));
  }

  void apply(const ControlStep& step) noexcept {
    switch (step.effect) {
      case Effect::None:
        break;
      case Effect::SetAddress:
        hw_.deviceaddr(static_cast<std::uint32_t>(step.arg) << soc::kDeviceAddrShift);
        break;
      case Effect::Configure:
        configure(step.arg != 0u);
        break;
      case Effect::SetInterface:
        break;  // no alternate settings in STANDARD mode
      case Effect::Halt:
        set_halt(step.arg, true);
        break;
      case Effect::ClearHalt:
        set_halt(step.arg, false);
        break;
      case Effect::MscReset:
        break;  // DISK mode is not in this phase
    }
    switch (step.kind) {
      case ControlStep::Kind::None:
        break;
      case ControlStep::Kind::In:
        if (step.len != 0u) {
          std::memcpy(m_.buffers[slot(0, true)], step.data, step.len);
        }
        arm(0, true, step.len);
        break;
      case ControlStep::Kind::Stall:
        ++c_.ep0_stalls;
        hw_.endptctrl(0, soc::kEpRxEnable | soc::kEpTxEnable | soc::kEpRxStall | soc::kEpTxStall);
        break;
    }
  }

  void configure(bool on) noexcept {
    if (on) {
      hw_.endptctrl(kNotifyNumber, soc::kEpTxEnable | (soc::kEpTypeInterrupt << soc::kEpTxTypeShift) | soc::kEpTxToggleReset |
                                       (soc::kEpTypeInterrupt << soc::kEpRxTypeShift));
      constexpr std::uint32_t kBulkPair = soc::kEpRxEnable | (soc::kEpTypeBulk << soc::kEpRxTypeShift) | soc::kEpRxToggleReset | soc::kEpTxEnable |
                                          (soc::kEpTypeBulk << soc::kEpTxTypeShift) | soc::kEpTxToggleReset;
      hw_.endptctrl(kCdcNumber, kBulkPair);
      hw_.endptctrl(kMidiNumber, kBulkPair);
      arm_cdc_out();
      arm_bulk_out(kMidiNumber);
    } else {
      // Take back what is armed, then switch the endpoints off.
      const std::uint32_t mask = ~(soc::endpoint_bit(0, false) | soc::endpoint_bit(0, true));
      hw_.endptflush_set(0xFFFFFFFFu & mask);
      for (unsigned i = 0; i < 1000u && (hw_.endptflush() & mask) != 0u; ++i) {
      }
      for (unsigned n = 1; n < kEndpoints; ++n) {
        hw_.endptctrl(n, 0);
        armed_[slot(n, false)] = false;
        armed_[slot(n, true)] = false;
      }
    }
  }

  void set_halt(std::uint8_t address, bool on) noexcept {
    const unsigned n = address & 0x0Fu;
    const bool transmit = (address & 0x80u) != 0u;
    if (n == 0u || n >= kEndpoints) {
      return;
    }
    std::uint32_t v = hw_.endptctrl(n);
    const std::uint32_t stall = transmit ? soc::kEpTxStall : soc::kEpRxStall;
    const std::uint32_t toggle = transmit ? soc::kEpTxToggleReset : soc::kEpRxToggleReset;
    if (on) {
      v |= stall;
    } else {
      v = (v & ~stall) | toggle;  // DATA0 again (USB 2.0 9.4.5)
    }
    hw_.endptctrl(n, v);
    if (!on) {
      const unsigned s = slot(n, transmit);
      armed_[s] = false;  // the controller dropped the primed descriptor with the stall; arm again from the rings
      service_rings();
    }
  }

  // ---- Completions ------------------------------------------------------------------------------------------------------------------
  void on_complete(std::uint32_t bits) noexcept {
    for (unsigned n = 0; n < kEndpoints; ++n) {
      for (unsigned t = 0; t < 2u; ++t) {
        const bool transmit = t != 0u;
        if ((bits & soc::endpoint_bit(n, transmit)) == 0u) {
          continue;
        }
        const unsigned s = slot(n, transmit);
        ++c_.completions[s];
        if (!armed_[s]) {
          ++c_.stray_completions;
          continue;
        }
        armed_[s] = false;
        bool ok = false;
        const std::uint32_t bytes = completed_bytes(s, ok);
        if (!ok) {
          ++c_.descriptor_errors;
        }
        if (n == 0u) {
          if (transmit) {
            apply(ctl_.on_in_done());
          } else {
            const ControlStep step = ctl_.on_out_done(m_.buffers[s], bytes);
            arm_ep0_out();
            apply(step);
          }
        } else if (n == kCdcNumber) {
          if (transmit) {
            arm_cdc_in();
          } else {
            take_cdc_packet(bytes);
            arm_cdc_out();
          }
        } else if (n == kMidiNumber) {
          if (transmit) {
            arm_midi_in();
          } else {
            take_midi_packet(bytes);
            arm_bulk_out(kMidiNumber);
          }
        }
      }
    }
  }

  // ---- CDC ----------------------------------------------------------------------------------------------------------------------------
  void arm_cdc_in() noexcept {
    o3os::usb::ByteChunk chunk;
    if (cdc_tx_.read(&chunk, 1) != 1u) {
      return;  // idle: the next start of frame looks again
    }
    const unsigned s = slot(kCdcNumber, true);
    std::memcpy(m_.buffers[s], chunk.data, chunk.len);
    ++c_.cdc_tx_packets;
    c_.cdc_tx_bytes += chunk.len;
    arm(kCdcNumber, true, chunk.len);
  }

  void arm_cdc_out() noexcept {
    if (cdc_rx_.size_from_producer() >= o3os::usb::kCdcRxHoldChunks) {
      ++c_.cdc_rx_holds;  // the host is NAKed until the main loop has read
      return;
    }
    arm_bulk_out(kCdcNumber);
  }

  void arm_bulk_out(unsigned number) noexcept { arm(number, false, kPacketBytes); }

  void take_cdc_packet(std::uint32_t bytes) noexcept {
    if (bytes == 0u || bytes > kPacketBytes) {
      return;
    }
    o3os::usb::ByteChunk chunk;
    chunk.len = bytes;
    std::memcpy(chunk.data, m_.buffers[slot(kCdcNumber, false)], bytes);
    cdc_rx_.write(&chunk, 1);
    ++c_.cdc_rx_packets;
    c_.cdc_rx_bytes += bytes;
  }

  // ---- MIDI -------------------------------------------------------------------------------------------------------------------------
  void arm_midi_in() noexcept {
    std::uint32_t events[o3os::usb::kMidiEventsPerPacket];
    const std::uint32_t n = midi_in_.read(events, o3os::usb::kMidiEventsPerPacket);
    if (n == 0u) {
      return;
    }
    std::memcpy(m_.buffers[slot(kMidiNumber, true)], events, n * 4u);
    ++c_.midi_in_packets;
    arm(kMidiNumber, true, n * 4u);
  }

  void take_midi_packet(std::uint32_t bytes) noexcept {
    ++c_.midi_out_packets;
    const std::uint8_t* p = m_.buffers[slot(kMidiNumber, false)];
    for (std::uint32_t off = 0; off + 4u <= bytes; off += 4u) {
      std::uint32_t event;
      std::memcpy(&event, p + off, 4);
      if (o3os::usb::midi_event_valid(event)) {
        midi_out_.write(&event, 1);
        ++c_.midi_out_events;
      }
    }
  }

  // ---- Bus events -------------------------------------------------------------------------------------------------------------------
  void on_bus_reset() noexcept {
    ++c_.bus_resets;
    hw_.endptsetupstat_clear(hw_.endptsetupstat());
    hw_.endptcomplete_clear(hw_.endptcomplete());
    if ((hw_.portsc() & soc::kPortReset) == 0u) {
      // The reset ended before this interrupt ran: the controller must be reset and set up again (RM), then prepared as for any reset.
      ++c_.late_resets;
      hw_.controller_reinit();
    }
    prepare();
  }

  void on_port_change() noexcept {
    ++c_.port_changes;
    const std::uint32_t port = hw_.portsc();
    c_.speed = (port >> soc::kPortSpeedShift) & 0x3u;
    if ((port & soc::kPortSuspend) == 0u && ctl_.suspended()) {
      ++c_.resumes;
      ctl_.on_resume();
    }
  }

  Hw& hw_;
  Memory& m_;
  ControlPipe& ctl_;
  o3os::usb::CdcTxRing& cdc_tx_;
  o3os::usb::CdcRxRing& cdc_rx_;
  o3os::usb::MidiRing& midi_in_;
  o3os::usb::MidiRing& midi_out_;
  CiCounters c_;
  bool armed_[kDirections] = {};
  std::uint32_t armed_len_[kDirections] = {};
  std::uint32_t last_sof_cycles_ = 0;
};

}  // namespace o3os::platform::rt10xx::usb
