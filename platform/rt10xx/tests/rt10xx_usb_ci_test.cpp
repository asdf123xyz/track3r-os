// SPDX-License-Identifier: GPL-3.0-or-later
//
// The OTG1 device driver (platform/rt10xx/usb/ci_device.h) against a fake controller that models what the chip's does with the queue heads,
// the descriptors and the ENDPT* registers: a host enumerates the device through the usb layer's control pipe, types on the CDC port, reads
// the replies, and the driver's book-keeping (armed directions, holds, resets, stalls) is what the comments promise.
#include <doctest/doctest.h>

#include <o3os/platform/rt10xx/usb/ci_device.h>
#include <o3os/usb/descriptors.h>

#include <cstdint>
#include <cstring>

namespace ci = o3os::platform::rt10xx::usb;
namespace soc = ci::soc;
using o3os::usb::Config;
using o3os::usb::ControlPipe;

namespace {

// The fake controller: the registers as the driver reads and writes them, and the transfers a host would make. Pointers are given to the
// driver as offsets into the Memory (bus_address), so a 64-bit host fits them in the 32-bit descriptor words.
struct FakeHw {
  explicit FakeHw(ci::Memory& m) : mem(m) {}

  std::uint32_t usbsts() { return sts; }
  void usbsts_clear(std::uint32_t bits) { sts &= ~bits; }
  std::uint32_t usbcmd() { return cmd; }
  void usbcmd(std::uint32_t v) { cmd = v; }
  std::uint32_t portsc() { return port; }
  void portsc(std::uint32_t v) { port = v; }
  std::uint32_t endptsetupstat() { return setupstat; }
  void endptsetupstat_clear(std::uint32_t bits) { setupstat &= ~bits; }
  std::uint32_t endptcomplete() { return complete; }
  void endptcomplete_clear(std::uint32_t bits) { complete &= ~bits; }
  std::uint32_t endptprime() { return 0; }  // a prime is taken at once
  void endptprime_set(std::uint32_t bits) {
    for (unsigned s = 0; s < ci::kDirections; ++s) {
      if ((bits & bit_of(s)) != 0u) {
        CHECK(mem.heads[s].next != ci::kTerminate);
        primed[s] = true;
        ++primes;
      }
    }
  }
  std::uint32_t endptstat() {
    std::uint32_t v = 0;
    for (unsigned s = 0; s < ci::kDirections; ++s) {
      if (primed[s]) {
        v |= bit_of(s);
      }
    }
    return v;
  }
  std::uint32_t endptflush() { return 0; }
  void endptflush_set(std::uint32_t bits) {
    for (unsigned s = 0; s < ci::kDirections; ++s) {
      if ((bits & bit_of(s)) != 0u) {
        primed[s] = false;
      }
    }
    ++flushes;
  }
  std::uint32_t endptctrl(unsigned n) { return ctrl[n]; }
  void endptctrl(unsigned n, std::uint32_t v) { ctrl[n] = v; }
  void deviceaddr(std::uint32_t v) { address = v >> soc::kDeviceAddrShift; }
  void controller_reinit() { ++reinits; }
  std::uint32_t bus_address(const void* p) {
    return static_cast<std::uint32_t>(static_cast<const std::uint8_t*>(p) - reinterpret_cast<const std::uint8_t*>(&mem));
  }
  void dmb() {}
  std::uint32_t cycles() { return ++clock; }

  // ---- The host's side ----
  static std::uint32_t bit_of(unsigned s) { return soc::endpoint_bit(s / 2u, (s & 1u) != 0u); }
  std::uint8_t* resolve(std::uint32_t bus) { return reinterpret_cast<std::uint8_t*>(&mem) + bus; }
  ci::Dtd& dtd_of(unsigned s) { return *reinterpret_cast<ci::Dtd*>(resolve(mem.heads[s].next & ~0x1Fu)); }

  // A SETUP packet on endpoint 0 (the controller writes the queue head's setup words and flags it).
  void host_setup(std::uint8_t type, std::uint8_t request, std::uint16_t value, std::uint16_t index, std::uint16_t length) {
    const std::uint8_t raw[8] = {type, request, static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(index),
                                 static_cast<std::uint8_t>(index >> 8), static_cast<std::uint8_t>(length), static_cast<std::uint8_t>(length >> 8)};
    std::uint32_t w0, w1;
    std::memcpy(&w0, raw, 4);
    std::memcpy(&w1, raw + 4, 4);
    mem.heads[0].setup[0] = w0;
    mem.heads[0].setup[1] = w1;
    setupstat |= 1u;
    sts |= soc::kStsUsbInt;
  }
  // An OUT packet of `n` bytes to endpoint `number`: taken when a descriptor is primed (true), NAKed otherwise (false).
  bool host_out(unsigned number, const std::uint8_t* data, std::uint32_t n) {
    const unsigned s = ci::slot(number, false);
    if (!primed[s] || (ctrl[number] & soc::kEpRxStall) != 0u) {
      return false;
    }
    ci::Dtd& d = dtd_of(s);
    const std::uint32_t total = (d.token >> ci::kTokenBytesShift) & ci::kTokenBytesMask;
    CHECK(n <= total);
    if (n != 0u) {
      std::memcpy(resolve(d.buffer[0]), data, n);
    }
    d.token = ((total - n) << ci::kTokenBytesShift);  // active clear, no error
    primed[s] = false;
    complete |= bit_of(s);
    sts |= soc::kStsUsbInt;
    return true;
  }
  // An IN token on endpoint `number`: the primed descriptor's bytes (0..64), or -1 for a NAK, -2 for a STALL.
  int host_in(unsigned number, std::uint8_t* out) {
    const unsigned s = ci::slot(number, true);
    if ((ctrl[number] & soc::kEpTxStall) != 0u) {
      return -2;
    }
    if (!primed[s]) {
      return -1;
    }
    ci::Dtd& d = dtd_of(s);
    const std::uint32_t total = (d.token >> ci::kTokenBytesShift) & ci::kTokenBytesMask;
    std::memcpy(out, resolve(d.buffer[0]), total);
    d.token = 0;
    primed[s] = false;
    complete |= bit_of(s);
    sts |= soc::kStsUsbInt;
    return static_cast<int>(total);
  }
  void host_reset(bool still_in_progress = true) {
    port = still_in_progress ? soc::kPortReset : 0u;
    sts |= soc::kStsReset;
  }
  void host_sof() { sts |= soc::kStsSof; }

  ci::Memory& mem;
  std::uint32_t sts = 0, cmd = 0, port = 0, setupstat = 0, complete = 0, address = 0xFF, clock = 0;
  std::uint32_t ctrl[ci::kEndpoints] = {};
  bool primed[ci::kDirections] = {};
  unsigned primes = 0, flushes = 0, reinits = 0;
};

struct Rig {
  Rig()
      : dev_len(o3os::usb::build_device_descriptor(cfg, dev_desc, sizeof dev_desc)),
        cfg_len(o3os::usb::build_config_descriptor(cfg, cfg_desc, sizeof cfg_desc)),
        pipe(cfg, dev_desc, dev_len, cfg_desc, cfg_len),
        hw(mem),
        dev(hw, mem, pipe, tx, rx, mi, mo) {
    cfg.mode = o3os::usb::Mode::Standard;
    cfg.speed = o3os::usb::Speed::Full;
    cfg.usb_return = false;
  }
  // Enumerates: reset, GET_DESCRIPTOR(device) in full, SET_ADDRESS 7, SET_CONFIGURATION 1.
  void enumerate() {
    hw.host_reset();
    dev.isr();
    get_descriptor(0x0100, 18);
    hw.host_setup(0x00, 5, 7, 0, 0);  // SET_ADDRESS
    dev.isr();
    std::uint8_t buf[64];
    CHECK(hw.host_in(0, buf) == 0);  // the status stage
    dev.isr();
    CHECK(hw.address == 7u);
    hw.host_setup(0x00, 9, 1, 0, 0);  // SET_CONFIGURATION 1
    dev.isr();
    CHECK(hw.host_in(0, buf) == 0);
    dev.isr();
    CHECK(pipe.configured());
  }
  // GET_DESCRIPTOR of `value` (type << 8 | index), reading `want` bytes in 64-byte chunks; returns the bytes read.
  std::uint32_t get_descriptor(std::uint16_t value, std::uint16_t want, std::uint8_t* into = nullptr) {
    hw.host_setup(0x80, 6, value, 0, want);
    dev.isr();
    std::uint32_t got = 0;
    for (unsigned chunks = 0; chunks < 16u; ++chunks) {
      std::uint8_t buf[64];
      const int n = hw.host_in(0, buf);
      CHECK(n >= 0);
      if (into != nullptr) {
        std::memcpy(into + got, buf, static_cast<std::size_t>(n));
      }
      got += static_cast<std::uint32_t>(n);
      dev.isr();
      if (n < 64 || got >= want) {
        break;
      }
    }
    CHECK(hw.host_out(0, nullptr, 0));  // the status stage
    dev.isr();
    return got;
  }

  Config cfg;
  std::uint8_t dev_desc[o3os::usb::kDeviceDescriptorSize] = {};
  std::uint8_t cfg_desc[o3os::usb::kMaxConfigDescriptorSize] = {};
  std::size_t dev_len;
  std::size_t cfg_len;
  ControlPipe pipe;
  o3os::usb::CdcTxRing tx;
  o3os::usb::CdcRxRing rx;
  o3os::usb::MidiRing mi;
  o3os::usb::MidiRing mo;
  ci::Memory mem{};
  FakeHw hw;
  ci::CiDevice<FakeHw> dev;
};

}  // namespace

TEST_CASE("rt10xx usb ci: the data structures") {
  CHECK(sizeof(ci::Dqh) == 64u);
  CHECK(sizeof(ci::Dtd) == 32u);
  CHECK(alignof(ci::Memory) == 2048u);
  CHECK(ci::slot(2, true) == 5u);
  CHECK(soc::endpoint_bit(2, true) == (1u << 18));
  CHECK(soc::endpoint_bit(3, false) == (1u << 3));
}

TEST_CASE("rt10xx usb ci: prepare arms endpoint 0 to receive and nothing else") {
  Rig r;
  r.dev.prepare();
  CHECK(r.dev.armed(0, false));
  CHECK_FALSE(r.dev.armed(0, true));
  CHECK_FALSE(r.dev.armed(2, false));
  CHECK(r.hw.address == 0u);
  CHECK(r.hw.ctrl[0] == (soc::kEpRxEnable | soc::kEpTxEnable));
  CHECK(r.hw.ctrl[2] == 0u);
  CHECK((r.mem.heads[0].capabilities & ci::kCapInterruptOnSetup) != 0u);
  CHECK(((r.mem.heads[1].capabilities >> ci::kCapMaxPacketShift) & 0x7FFu) == 64u);
}

TEST_CASE("rt10xx usb ci: enumeration through the control pipe") {
  Rig r;
  r.dev.prepare();
  r.enumerate();
  CHECK(r.dev.counters().bus_resets == 1u);
  CHECK(r.dev.counters().setups == 3u);
  CHECK(r.dev.counters().ep0_stalls == 0u);
  CHECK(r.dev.counters().stray_completions == 0u);
  // The configuration opened the CDC and MIDI endpoints and armed their receive sides; the notification endpoint stays unarmed.
  CHECK((r.hw.ctrl[2] & soc::kEpRxEnable) != 0u);
  CHECK((r.hw.ctrl[2] & soc::kEpTxEnable) != 0u);
  CHECK(((r.hw.ctrl[2] >> soc::kEpRxTypeShift) & 3u) == soc::kEpTypeBulk);
  CHECK(((r.hw.ctrl[1] >> soc::kEpTxTypeShift) & 3u) == soc::kEpTypeInterrupt);
  CHECK(r.dev.armed(2, false));
  CHECK(r.dev.armed(3, false));
  CHECK_FALSE(r.dev.armed(1, true));
  CHECK_FALSE(r.dev.armed(2, true));
  // The device descriptor the host read is the one the layer built.
  std::uint8_t got[64];
  CHECK(r.get_descriptor(0x0100, 18, got) == 18u);
  CHECK(std::memcmp(got, r.dev_desc, 18) == 0);
  // The configuration descriptor in full, in 64-byte chunks with the status stage after the last.
  std::uint8_t all[o3os::usb::kMaxConfigDescriptorSize];
  CHECK(r.get_descriptor(0x0200, static_cast<std::uint16_t>(r.cfg_len), all) == r.cfg_len);
  CHECK(std::memcmp(all, r.cfg_desc, r.cfg_len) == 0);
}

TEST_CASE("rt10xx usb ci: an unsupported request stalls endpoint 0 and the next SETUP clears it") {
  Rig r;
  r.dev.prepare();
  r.hw.host_reset();
  r.dev.isr();
  r.hw.host_setup(0x80, 6, 0x0600, 0, 10);  // GET_DESCRIPTOR device qualifier: a full-speed-only device stalls it
  r.dev.isr();
  CHECK(r.dev.counters().ep0_stalls == 1u);
  CHECK((r.hw.ctrl[0] & (soc::kEpRxStall | soc::kEpTxStall)) == (soc::kEpRxStall | soc::kEpTxStall));
  std::uint8_t buf[64];
  CHECK(r.hw.host_in(0, buf) == -2);
  r.get_descriptor(0x0100, 18);  // the next request goes through
  CHECK((r.hw.ctrl[0] & soc::kEpRxStall) == 0u);
}

TEST_CASE("rt10xx usb ci: the CDC port moves the rings") {
  Rig r;
  r.dev.prepare();
  r.enumerate();
  // The host types a line: the receive side's packet lands in the receive ring and the direction is armed again.
  const char line[] = "help\r";
  CHECK(r.hw.host_out(2, reinterpret_cast<const std::uint8_t*>(line), 5));
  r.dev.isr();
  o3os::usb::ByteChunk chunk;
  CHECK(r.rx.read(&chunk, 1) == 1u);
  CHECK(chunk.len == 5u);
  CHECK(std::memcmp(chunk.data, line, 5) == 0);
  CHECK(r.dev.armed(2, false));
  CHECK(r.dev.counters().cdc_rx_packets == 1u);
  CHECK(r.dev.counters().cdc_rx_bytes == 5u);
  // The reply goes through the writer and the transmit ring; the next start of frame arms the transmit side; the host reads it.
  o3os::usb::CdcTxWriter writer(r.tx);
  writer.write("OK\n", 3);
  writer.flush();
  std::uint8_t buf[64];
  CHECK(r.hw.host_in(2, buf) == -1);  // nothing armed yet
  r.hw.host_sof();
  r.dev.isr();
  CHECK(r.dev.armed(2, true));
  CHECK(r.hw.host_in(2, buf) == 3);
  CHECK(std::memcmp(buf, "OK\n", 3) == 0);
  r.dev.isr();
  CHECK_FALSE(r.dev.armed(2, true));  // the ring is empty again
  CHECK(r.dev.counters().cdc_tx_packets == 1u);
  CHECK(r.dev.counters().cdc_tx_bytes == 3u);
  // A long reply: 200 bytes become four packets of 63, 63, 63 and 11 bytes, one armed after the other as the host reads.
  char longtext[200];
  for (unsigned i = 0; i < 200u; ++i) {
    longtext[i] = static_cast<char>('a' + i % 26u);
  }
  writer.write(longtext, 200);
  writer.flush();
  r.hw.host_sof();
  r.dev.isr();
  std::uint32_t total = 0;
  for (unsigned i = 0; i < 4u; ++i) {
    const int n = r.hw.host_in(2, buf);
    CHECK(n > 0);
    CHECK(std::memcmp(buf, longtext + total, static_cast<std::size_t>(n)) == 0);
    total += static_cast<std::uint32_t>(n);
    r.dev.isr();
  }
  CHECK(total == 200u);
  CHECK(r.hw.host_in(2, buf) == -1);
}

TEST_CASE("rt10xx usb ci: the receive hold NAKs the host while the main loop has not read") {
  Rig r;
  r.dev.prepare();
  r.enumerate();
  const std::uint8_t byte = 'x';
  unsigned taken = 0;
  for (unsigned i = 0; i < 20u; ++i) {
    if (r.hw.host_out(2, &byte, 1)) {
      ++taken;
      r.dev.isr();
    }
  }
  CHECK(taken == o3os::usb::kCdcRxHoldChunks);  // the hold starts at this many unread chunks
  CHECK_FALSE(r.dev.armed(2, false));
  CHECK(r.dev.counters().cdc_rx_holds >= 1u);
  CHECK(r.rx.dropped() == 0u);
  // The main loop reads; the next start of frame arms the receive side again.
  o3os::usb::ByteChunk chunk;
  while (r.rx.read(&chunk, 1) == 1u) {
  }
  r.hw.host_sof();
  r.dev.isr();
  CHECK(r.dev.armed(2, false));
  CHECK(r.hw.host_out(2, &byte, 1));
}

TEST_CASE("rt10xx usb ci: a bus reset takes everything back; a late one re-initialises the controller") {
  Rig r;
  r.dev.prepare();
  r.enumerate();
  o3os::usb::CdcTxWriter writer(r.tx);
  writer.write("late", 4);
  writer.flush();
  r.hw.host_sof();
  r.dev.isr();
  CHECK(r.dev.armed(2, true));
  r.hw.host_reset();
  r.dev.isr();
  CHECK(r.dev.counters().bus_resets == 2u);
  CHECK(r.dev.counters().late_resets == 0u);
  CHECK_FALSE(r.pipe.configured());
  CHECK(r.hw.address == 0u);
  CHECK(r.hw.ctrl[2] == 0u);
  CHECK_FALSE(r.dev.armed(2, true));
  CHECK(r.dev.armed(0, false));
  r.hw.host_reset(false);  // the port's reset bit already clear when the interrupt runs
  r.dev.isr();
  CHECK(r.dev.counters().late_resets == 1u);
  CHECK(r.hw.reinits == 1u);
  r.enumerate();  // and the device enumerates again
  CHECK(r.pipe.configured());
}

TEST_CASE("rt10xx usb ci: MIDI events to and from the host") {
  Rig r;
  r.dev.prepare();
  r.enumerate();
  // From the host: a note on (cable 0, CIN 9) and two padding words; only the event reaches the ring.
  const std::uint8_t packet[12] = {0x09, 0x90, 0x3C, 0x7F, 0, 0, 0, 0, 0, 0, 0, 0};
  CHECK(r.hw.host_out(3, packet, 12));
  r.dev.isr();
  std::uint32_t event = 0;
  CHECK(r.mo.read(&event, 1) == 1u);
  CHECK(event == 0x7F3C9009u);
  CHECK(r.dev.counters().midi_out_events == 1u);
  CHECK(r.dev.armed(3, false));
  // To the host: an event written to the ring goes out at the next start of frame.
  const std::uint32_t out = 0x40803E08u;  // note off
  r.mi.write(&out, 1);
  r.hw.host_sof();
  r.dev.isr();
  std::uint8_t buf[64];
  CHECK(r.hw.host_in(3, buf) == 4);
  CHECK(std::memcmp(buf, &out, 4) == 0);
}

TEST_CASE("rt10xx usb ci: halt and clear halt on a bulk endpoint") {
  Rig r;
  r.dev.prepare();
  r.enumerate();
  r.hw.host_setup(0x02, 3, 0, 0x82, 0);  // SET_FEATURE ENDPOINT_HALT on the CDC IN endpoint
  r.dev.isr();
  std::uint8_t buf[64];
  CHECK(r.hw.host_in(0, buf) == 0);
  r.dev.isr();
  CHECK((r.hw.ctrl[2] & soc::kEpTxStall) != 0u);
  CHECK(r.pipe.halted(0x82));
  r.hw.host_setup(0x02, 1, 0, 0x82, 0);  // CLEAR_FEATURE
  r.dev.isr();
  CHECK(r.hw.host_in(0, buf) == 0);
  r.dev.isr();
  CHECK((r.hw.ctrl[2] & soc::kEpTxStall) == 0u);
  CHECK((r.hw.ctrl[2] & soc::kEpTxToggleReset) != 0u);
  CHECK_FALSE(r.pipe.halted(0x82));
}

TEST_CASE("rt10xx usb ci: suspend and resume reach the control pipe") {
  Rig r;
  r.dev.prepare();
  r.enumerate();
  r.hw.sts |= soc::kStsSuspend;
  r.dev.isr();
  CHECK(r.pipe.suspended());
  CHECK(r.dev.counters().suspends == 1u);
  r.hw.port = 0;  // SUSP clear
  r.hw.sts |= soc::kStsPortChange;
  r.dev.isr();
  CHECK_FALSE(r.pipe.suspended());
  CHECK(r.dev.counters().resumes == 1u);
}
