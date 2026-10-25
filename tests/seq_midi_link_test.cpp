// SPDX-License-Identifier: GPL-3.0-or-later
//
// MIDI sync end to end through seq::MidiLink (OS-005c2): the sequencer, the router, the DIN emitter and USB out on one side, simulated masters stamping bytes into the
// midiRt rings on the other, driven block by block the way the engine does. Master: clock, Start, SPP + Continue and Stop on the enabled ports, the OUT OFFSET, the hang
// bookkeeping. Slave: the follow loop, the transport, the IN OFFSET, the port that is followed, a master that goes quiet, a mode change while following, the Play key under an
// external clock, the relay. The ISR side of the ports is the emitter's own poll() stepped through each block (the interrupt logic is midi_io_test.cpp's). Host and Cortex-M4F
// (`qemu_seq_model`).
#include <doctest/doctest.h>

#include <o3os/core/qtime.h>
#include <o3os/midi/config.h>
#include <o3os/model/params.h>
#include <o3os/model/settings.h>
#include <o3os/seq/midi_link.h>

#include <cmath>
#include <cstdint>
#include <initializer_list>

#include "support/need.h"
#include "support/seq_fixture.h"
#include "support/static_slot.h"

using namespace o3os;
using namespace o3os::test;
using audio::EventKind;

namespace {

constexpr std::uint64_t kCyc = core::kRefCyclesPerSample.num / core::kRefCyclesPerSample.den;  // 4080 in the default build
constexpr double kCycD = static_cast<double>(core::kRefCyclesPerSample.num) / static_cast<double>(core::kRefCyclesPerSample.den);
constexpr std::uint64_t kClockPeriod = 3750000;  // cycles: 24 ppqn at 120 BPM
constexpr std::uint64_t kByte = midi::din::kByteCycles;
constexpr std::uint64_t kFrame = midi::usb_out::kFrameCycles;

// What went out: DIN bytes with the cycle each started, USB event words with the frame that carried them.
struct Wires {
  static constexpr std::uint32_t kCap = 6000;
  std::uint8_t din[kCap];
  std::uint64_t din_at[kCap];
  std::uint32_t din_n = 0;
  std::uint32_t usb[kCap];
  std::uint64_t usb_at[kCap];
  std::uint32_t usb_n = 0;

  std::uint32_t count_din(std::uint8_t b) const {
    std::uint32_t c = 0;
    for (std::uint32_t i = 0; i < din_n; ++i) {
      c += din[i] == b ? 1u : 0u;
    }
    return c;
  }
  // The i-th DIN byte equal to b: its index, or din_n.
  std::uint32_t find_din(std::uint8_t b, std::uint32_t idx = 0) const {
    std::uint32_t c = 0;
    for (std::uint32_t i = 0; i < din_n; ++i) {
      if (din[i] == b) {
        if (c == idx) {
          return i;
        }
        ++c;
      }
    }
    return din_n;
  }
  std::uint32_t count_usb_byte0(std::uint8_t b) const {
    std::uint32_t c = 0;
    for (std::uint32_t i = 0; i < usb_n; ++i) {
      c += static_cast<std::uint8_t>((usb[i] >> 8) & 0xFFu) == b ? 1u : 0u;
    }
    return c;
  }
};

Wires g_wires;
Collector& g_col = seq_collector();
StaticSlot<seq::MidiLink> g_link_slot;
std::uint32_t g_kicks = 0;

// A master clock for the Slave tests: 120 BPM, one clock every 3,750,000 cycles from `t0`, stamped one byte time after its start as the UART interrupt would.
struct Master {
  double t0 = 3.0e6;
  double period = static_cast<double>(kClockPeriod);
  bool quiet = false;  // no clocks (a pulled cable)
  std::uint32_t next = 0;
  double time_of(std::uint32_t c) const { return t0 + static_cast<double>(c) * period; }
};

struct Rig {
  seq::Sequencer* s = nullptr;
  seq::MidiLink* link = nullptr;
  std::uint64_t sample = 0;
  std::uint64_t din_now = 0;
  std::uint64_t next_frame = 0;
  Master m;
  // bytes the master sends besides its clock: (true time in cycles, byte, port)
  struct Extra {
    double t;
    std::uint8_t byte;
    std::uint8_t port;
  };
  Extra extra[16];
  std::uint32_t extra_n = 0;

  void init(const midi::MidiConfig& cfg) {
    g_col.n = 0;
    g_col.total_events = 0;
    g_wires.din_n = 0;
    g_wires.usb_n = 0;
    g_kicks = 0;
    s = &seq_slot().make(seq_project());
    seq::MidiLinkHooks hooks;
    hooks.kick = [](void*) noexcept { ++g_kicks; };
    link = &g_link_slot.make(*s, hooks);  // no heard_at: the counter reads the timeline's own cycles
    link->set_config(cfg);
    sample = 0;
    din_now = 0;
    next_frame = 0;
    m = Master{};
    extra_n = 0;
  }

  void send(double true_cycles, std::uint8_t byte, std::uint8_t port = 0) {
    extra[extra_n++] = Extra{true_cycles, byte, port};
  }

  // The master's bytes whose stamps (a byte time after they started) have passed by `now`.
  void feed_master(double now_cycles) {
    for (;;) {
      std::uint32_t best = extra_n;
      for (std::uint32_t i = 0; i < extra_n; ++i) {
        if (best == extra_n || extra[i].t < extra[best].t) {
          best = i;
        }
      }
      const double clock_t = m.time_of(m.next) + 57600.0;
      const bool clock_ready = !m.quiet && clock_t < now_cycles;
      const bool extra_ready = best != extra_n && extra[best].t + 57600.0 < now_cycles;
      if (extra_ready && (!clock_ready || extra[best].t <= m.time_of(m.next))) {
        midi::RtInRing& ring = extra[best].port == 0 ? link->din_in() : link->usb_in();
        CHECK(ring.push(midi::RtItem{static_cast<std::uint32_t>(static_cast<std::uint64_t>(extra[best].t + 57600.0)), extra[best].byte, extra[best].port, 0}));
        extra[best] = extra[extra_n - 1];
        --extra_n;
        continue;
      }
      if (!clock_ready) {
        break;
      }
      CHECK(link->din_in().push(midi::RtItem{static_cast<std::uint32_t>(static_cast<std::uint64_t>(clock_t)), midi::kClock, 0, 0}));
      ++m.next;
    }
  }

  // One block of 64 samples: the master's bytes, the link's step, then the ports' interrupts as they would have run over the block.
  void block() {
    audio::SeqEvent out[audio::kMaxEventsPerBlock];
    feed_master(static_cast<double>(sample) * kCycD);
    const std::uint32_t c = link->step(sample, 64, out, audio::kMaxEventsPerBlock);
    for (std::uint32_t i = 0; i < c; ++i) {
      g_col.add(sample, out[i]);
    }
    // The ports' interrupts run from the time this block was rendered to the time its first sample is heard, one block later (the TX ring's render-ahead): events of the
    // block that are due before that (an OUT advance) leave at once, the others wait for later blocks' passes.
    const std::uint64_t end = core::cycles_from_samples(sample);
    midi::DinEmitter& em = link->din();
    for (std::uint32_t guard = 0; guard < 64; ++guard) {
      const std::uint64_t w = em.next_wake(din_now);
      if (w == midi::kNever || w >= end) {
        break;
      }
      din_now = w;
      const midi::DinEmitter::Tx tx = em.poll(w);
      if (tx.valid && g_wires.din_n < Wires::kCap) {
        g_wires.din[g_wires.din_n] = tx.byte;
        g_wires.din_at[g_wires.din_n] = tx.start;
        ++g_wires.din_n;
      } else if (!tx.valid) {
        ++din_now;
      }
    }
    din_now = din_now < end ? end : din_now;
    while (next_frame < end) {
      std::uint32_t ev[midi::usb_out::kEventsPerFrame];
      const std::uint32_t n = link->usb_out().frame(next_frame, ev, midi::usb_out::kEventsPerFrame);
      for (std::uint32_t i = 0; i < n && g_wires.usb_n < Wires::kCap; ++i) {
        g_wires.usb[g_wires.usb_n] = ev[i];
        g_wires.usb_at[g_wires.usb_n] = next_frame;
        ++g_wires.usb_n;
      }
      next_frame += kFrame;
    }
    sample += 64;
  }

  template <typename F>
  void run_to(std::uint64_t to_sample, F at_block) {
    while (sample < to_sample) {
      at_block(sample);
      block();
    }
  }
  void run_to(std::uint64_t to_sample) {
    run_to(to_sample, [](std::uint64_t) {});
  }
};

Rig g_rig;

// Pattern 1: 16 steps, a note on every step of track 0 played by the MIDI instrument (slot 10, channel 3 = wire channel 2), the step number as the note; one song position.
void midi_project(std::uint32_t tempo_x100 = 12000) {
  fresh_project(2, 2);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(tempo_x100) == model::Status::Ok);
  CHECK(p.create_pattern(1, 16) == model::Status::Ok);
  for (std::uint32_t k = 0; k < 16; ++k) {
    CHECK(p.set_step(1, 0, k, make_step(static_cast<std::uint8_t>(40 + k), 11)) == model::Status::Ok);
  }
  CHECK(p.append_order(1) == model::Status::Ok);
}

midi::MidiConfig master_cfg(std::uint8_t out_ports) {
  midi::MidiConfig c;
  c.mode = midi::SyncMode::Master;
  c.out_ports = out_ports;
  c.note_ports = midi::kPortDin | midi::kPortUsb;
  return c;
}
midi::MidiConfig slave_cfg(midi::Port in_port, std::uint8_t out_ports = 0) {
  midi::MidiConfig c;
  c.mode = midi::SyncMode::Slave;
  c.in_port = in_port;
  c.out_ports = out_ports;
  return c;
}

double sample_of_cycles(double cycles) { return cycles / kCycD; }

// The first Step record at or after sample `from`, and the n-th after it: returns the sample, or -1.
double step_time(const Collector& c, std::uint32_t idx) {
  const Rec* r = c.nth(EventKind::Step, idx);
  return r != nullptr ? static_cast<double>(r->t) : -1.0;
}

}  // namespace

// ---- Master ---------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("midi link master: Start, then a clock every 20.83 ms on the wire, the notes after the Start, on DIN and USB") {
  midi_project();
  g_rig.init(master_cfg(midi::kPortDin | midi::kPortUsb));
  g_rig.s->play_pattern(1);
  g_rig.run_to(100000);  // 2.27 s: a little more than one pattern (2 s)

  // DIN: the Start byte is the first byte, ahead of every note
  NEED(g_wires.din_n > 20u);
  CHECK(g_wires.din[0] == midi::kStart);
  const std::uint32_t first_note = g_wires.find_din(0x92);
  NEED(first_note < g_wires.din_n);
  CHECK(g_wires.din_at[0] < g_wires.din_at[first_note]);
  CHECK(g_wires.din[first_note + 1] == 40);  // the pattern's first note
  // clocks: 24 a beat, a beat is 0.5 s at 120 BPM: 4 beats and a bit in 2.27 s
  const std::uint32_t clocks = g_wires.count_din(midi::kClock);
  CHECK(clocks >= 107u);  // the first at the first step (a block after the play), then every 919.1 samples: 109 in 100,000 samples, the last block's not yet heard
  CHECK(clocks <= 110u);
  // each clock is one period after the one before it; a note's bytes may hold one up by at most a byte time, and the next one is back on the grid
  std::uint64_t prev = 0;
  std::uint32_t seen = 0;
  std::uint32_t exact = 0;
  std::uint64_t first_clock = 0;
  for (std::uint32_t i = 0; i < g_wires.din_n; ++i) {
    if (g_wires.din[i] != midi::kClock) {
      continue;
    }
    if (seen == 0u) {
      first_clock = g_wires.din_at[i];
    }
    const std::uint64_t ideal = first_clock + static_cast<std::uint64_t>(seen) * kClockPeriod;
    const std::int64_t err = static_cast<std::int64_t>(g_wires.din_at[i]) - static_cast<std::int64_t>(ideal);
    CHECK(err >= -2);                                // never early (to a cycle's rounding)
    CHECK(err <= static_cast<std::int64_t>(kByte));  // at worst one byte in flight
    exact += err <= 2 ? 1u : 0u;
    (void)prev;
    prev = g_wires.din_at[i];
    ++seen;
  }
  CHECK(exact * 10u >= seen * 9u);  // nearly all of them are on the grid to the cycle: the wire is mostly idle
  // the clock follows the Start by less than a clock period; the first note is on the first microstep: the Start leads it by the start lead (one block)
  const std::uint32_t first_clock_idx = g_wires.find_din(midi::kClock);
  NEED(first_clock_idx < g_wires.din_n);
  CHECK(g_wires.din_at[first_clock_idx] - g_wires.din_at[0] <= 2 * 64 * kCyc);

  // USB: the same stream in event words: Start, clocks, the note-on of channel 2, none earlier than due, none later than a frame
  CHECK(g_wires.count_usb_byte0(midi::kStart) == 1u);
  CHECK(g_wires.count_usb_byte0(midi::kClock) + 3u >= clocks);  // a frame carries a clock at or after its time: the last few are in frames that have not come yet
  CHECK(g_wires.count_usb_byte0(midi::kClock) <= clocks);
  CHECK(g_wires.count_usb_byte0(0x92) >= 16u);
  NEED(g_wires.usb_n > 20u);
  CHECK(g_wires.usb[0] == midi::usb_event(0, midi::msg1(midi::kStart)));
  CHECK(g_kicks > 20u);  // the render pended the DIN emitter for every block that handed it something
}

TEST_CASE("midi link master: only the ports enabled get the clock, Off sends none; notes go to the notes' ports either way") {
  // USB only
  midi_project();
  g_rig.init(master_cfg(midi::kPortUsb));
  g_rig.s->play_pattern(1);
  g_rig.run_to(40000);
  CHECK(g_wires.count_din(midi::kClock) == 0u);
  CHECK(g_wires.count_din(midi::kStart) == 0u);
  CHECK(g_wires.count_din(0x92) > 0u);
  CHECK(g_wires.count_usb_byte0(midi::kClock) > 20u);
  CHECK(g_wires.count_usb_byte0(midi::kStart) == 1u);
  // DIN only
  midi_project();
  g_rig.init(master_cfg(midi::kPortDin));
  g_rig.s->play_pattern(1);
  g_rig.run_to(40000);
  CHECK(g_wires.count_usb_byte0(midi::kClock) == 0u);
  CHECK(g_wires.count_din(midi::kClock) > 20u);
  // Off: no clock, no Start, the notes still go out
  midi_project();
  midi::MidiConfig off;
  off.mode = midi::SyncMode::Off;
  off.out_ports = midi::kPortDin | midi::kPortUsb;  // set, but Off wins
  g_rig.init(off);
  g_rig.s->play_pattern(1);
  g_rig.run_to(40000);
  CHECK(g_wires.count_din(midi::kClock) == 0u);
  CHECK(g_wires.count_din(midi::kStart) == 0u);
  CHECK(g_wires.count_usb_byte0(midi::kClock) == 0u);
  CHECK(g_wires.count_din(0x92) > 0u);
  // the notes' ports can be moved: USB only
  midi_project();
  midi::MidiConfig n = master_cfg(midi::kPortDin);
  n.note_ports = midi::kPortUsb;
  g_rig.init(n);
  g_rig.s->play_pattern(1);
  g_rig.run_to(40000);
  CHECK(g_wires.count_din(0x92) == 0u);
  CHECK(g_wires.count_usb_byte0(0x92) > 0u);
}

TEST_CASE("midi link master: a play from the middle sends SPP and Continue, not Start; Stop goes out with the offs of the notes that were open") {
  midi_project();
  g_rig.init(master_cfg(midi::kPortDin));
  g_rig.s->play_pattern(1, 8);
  g_rig.run_to(20000);
  NEED(g_wires.din_n > 6u);
  CHECK(g_wires.count_din(midi::kStart) == 0u);
  const std::uint32_t spp = g_wires.find_din(midi::kSongPosition);
  NEED(spp + 3u < g_wires.din_n);
  CHECK(g_wires.din[spp + 1] == 8);  // 8 MIDI beats = 8 steps, low 7 bits first
  CHECK(g_wires.din[spp + 2] == 0);
  CHECK(g_wires.din[spp + 3] == midi::kContinue);
  CHECK(spp < g_wires.find_din(0x92));  // the position goes before the first note
  // stop in the middle of a note: the sequencer's own off, then Stop; no note is left hanging
  g_rig.s->stop();
  g_rig.run_to(26000);
  const std::uint32_t ons = g_wires.count_din(0x92);
  const std::uint32_t offs = g_wires.count_din(0x82);
  CHECK(ons >= 1u);
  CHECK(offs == ons);
  CHECK(g_wires.count_din(midi::kStop) == 1u);
  CHECK(g_rig.link->out().book().owed() == 0u);
  const std::uint32_t before = g_wires.din_n;
  g_rig.run_to(60000);
  CHECK(g_wires.din_n == before);  // the wire is quiet after a stop: no clock, no note
}

TEST_CASE("midi link master: the OUT OFFSET delays everything on that port by its exact length, the other port does not move") {
  std::uint64_t base_din = 0;
  std::uint64_t base_usb = 0;
  for (const int dms_i : {0, 25, -10}) {
    const std::int16_t dms = static_cast<std::int16_t>(dms_i);
    midi_project();
    midi::MidiConfig c = master_cfg(midi::kPortDin | midi::kPortUsb);
    c.out_offset_dms[0] = dms;  // DIN only
    g_rig.init(c);
    g_rig.s->play_pattern(1);
    g_rig.run_to(30000);
    const std::uint32_t clk = g_wires.find_din(midi::kClock, 3);
    NEED(clk < g_wires.din_n);
    NEED(g_wires.usb_n > 6u);
    // the USB frame that carries the fourth clock
    std::uint32_t k = 0;
    std::uint64_t usb_at = 0;
    std::uint32_t seen = 0;
    for (k = 0; k < g_wires.usb_n; ++k) {
      if (static_cast<std::uint8_t>((g_wires.usb[k] >> 8) & 0xFFu) == midi::kClock && seen++ == 3u) {
        usb_at = g_wires.usb_at[k];
        break;
      }
    }
    if (dms == 0) {
      base_din = g_wires.din_at[clk];
      base_usb = usb_at;
    } else {
      CHECK(static_cast<std::int64_t>(g_wires.din_at[clk]) - static_cast<std::int64_t>(base_din) == static_cast<std::int64_t>(dms) * 18000);
      CHECK(usb_at == base_usb);
    }
  }
}

// ---- Slave ------------------------------------------------------------------------------------------------------------------------------------

namespace {
// A slave rig at 90 BPM project tempo (the master is at 120) playing the master's Start at clock `c_start`.
void slave_start(double start_before_clock_cycles = 540000.0, std::uint32_t c_start = 40) {
  g_rig.s->select(seq::PlayMode::Pattern, 1);
  g_rig.send(g_rig.m.time_of(c_start) - start_before_clock_cycles, midi::kStart);
}
}  // namespace

TEST_CASE("midi link slave: the loop locks, the Start moves the transport, the steps sit on the master's clock, the tempo is the master's") {
  midi_project(9000);
  g_rig.init(slave_cfg(midi::Port::Din));
  const std::uint32_t c_start = 40;
  slave_start(540000.0, c_start);
  g_rig.run_to(400000);
  NEED(g_col.count(EventKind::Step) >= 20u);
  CHECK(g_rig.s->following());
  CHECK(g_rig.s->playing());
  // step k at the master's clock c_start + 6 k: the first is late by what a block and a byte add, the others are exact to a sample's rounding
  for (std::uint32_t k = 0; k < 20; ++k) {
    const double want = sample_of_cycles(g_rig.m.time_of(c_start + 6u * k));
    const double err = step_time(g_col, k) - want;
    if (k == 0) {
      CHECK(err >= -1.5);
      CHECK(err <= 64.0 + 16.0);
    } else {
      CHECK_MESSAGE(std::fabs(err) <= 1.5, k);
    }
  }
  const midi::SyncStatus st = g_rig.link->status();
  CHECK(st.following);
  CHECK(st.clock_present);
  CHECK(st.locked);
  CHECK(st.bpm_x100 == 12000u);
  CHECK(st.clocks > 100u);
  CHECK(st.transport_events == 1u);
  CHECK(g_rig.s->tempo_x100() == 12000u);
  CHECK(g_rig.link->stats().stale_releases == 0u);
}

TEST_CASE("midi link slave: Stop stops, SPP then Continue resumes at that step, the clock out is the relay of the followed clock") {
  midi_project(9000);
  g_rig.init(slave_cfg(midi::Port::Din, midi::kPortDin));
  const std::uint32_t c_start = 40;
  slave_start(540000.0, c_start);
  const double stop_at = g_rig.m.time_of(c_start + 6 * 5 + 3);
  const double spp_at = g_rig.m.time_of(c_start + 6 * 5 + 9);
  const double cont_at = g_rig.m.time_of(c_start + 6 * 5 + 11) - 1.0e6;
  g_rig.send(stop_at, midi::kStop);
  g_rig.send(spp_at, midi::kSongPosition);
  g_rig.send(spp_at + 60000.0, 9);
  g_rig.send(spp_at + 120000.0, 0);
  g_rig.send(cont_at, midi::kContinue);
  g_rig.run_to(160000);  // the Start is at 0.85 s, the Stop at 1.5 s, the Continue at 1.75 s: 3.6 s
  // after the Stop the transport stops (the relayed Stop on DIN too), then the Continue plays on from step 9
  CHECK(g_wires.count_din(midi::kStop) >= 1u);
  CHECK(g_wires.count_din(midi::kContinue) == 1u);
  CHECK(g_wires.find_din(midi::kSongPosition) < g_wires.din_n);
  NEED(g_col.count(EventKind::Stopped) >= 1u);
  const std::uint32_t steps = g_col.count(EventKind::Step);
  NEED(steps > 6u);
  // the first step after the Continue is step 9 (value), on the master's clock grid
  bool found = false;
  for (std::uint32_t i = 0; i < g_col.n; ++i) {
    if (g_col.rec[i].ev.kind == EventKind::Stopped) {
      for (std::uint32_t j = i; j < g_col.n; ++j) {
        if (g_col.rec[j].ev.kind == EventKind::Step) {
          CHECK(g_col.rec[j].ev.value == 9);
          found = true;
          break;
        }
      }
      break;
    }
  }
  CHECK(found);
  // the relayed clock is on the followed grid: spacing one master period, to a few cycles, while it plays
  std::uint32_t seen = 0;
  std::uint64_t prev = 0;
  std::uint32_t good = 0;
  for (std::uint32_t i = 0; i < g_wires.din_n; ++i) {
    if (g_wires.din[i] != midi::kClock) {
      continue;
    }
    if (seen != 0u && g_wires.din_at[i] - prev <= kClockPeriod + 4000u && g_wires.din_at[i] - prev + 4000u >= kClockPeriod) {
      ++good;
    }
    prev = g_wires.din_at[i];
    ++seen;
  }
  CHECK(seen > 20u);
  CHECK(good * 10u >= seen * 8u);
}

TEST_CASE("midi link slave: the IN OFFSET shifts the steps by its exact length; the followed port is the only one that counts") {
  for (const int dms_i : {20, 50}) {
    const std::int16_t dms = static_cast<std::int16_t>(dms_i);
    midi_project(9000);
    midi::MidiConfig c = slave_cfg(midi::Port::Din);
    c.in_offset_dms[0] = dms;
    g_rig.init(c);
    const std::uint32_t c_start = 40;
    slave_start(540000.0, c_start);
    g_rig.run_to(300000);
    NEED(g_col.count(EventKind::Step) >= 8u);
    const double shift = static_cast<double>(dms) * 100.0 * 180.0 / kCycD;  // samples
    for (std::uint32_t k = 3; k < 8; ++k) {
      const double want = sample_of_cycles(g_rig.m.time_of(c_start + 6u * k)) + shift;
      CHECK_MESSAGE(std::fabs(step_time(g_col, k) - want) <= 2.0, dms);
    }
  }
  // the master is on DIN but the tracker follows USB: its clock and Start are ignored, nothing follows, nothing plays
  midi_project(9000);
  g_rig.init(slave_cfg(midi::Port::Usb));
  slave_start();
  g_rig.run_to(200000);
  CHECK_FALSE(g_rig.s->playing());
  CHECK_FALSE(g_rig.s->following());
  CHECK(g_col.count(EventKind::Step) == 0u);
  CHECK(g_rig.link->in().stats().ignored_port > 50u);
  CHECK(g_rig.link->in().stats().clocks == 0u);
}

TEST_CASE("midi link slave: a master that goes quiet stops the transport and is let go; it is followed again when it returns") {
  midi_project(9000);
  g_rig.init(slave_cfg(midi::Port::Din));
  const std::uint32_t c_start = 40;
  slave_start(540000.0, c_start);
  // 100 clocks, then the cable is pulled: no Stop, no clock
  const std::uint64_t pulled = static_cast<std::uint64_t>(g_rig.m.time_of(100) / kCycD);
  g_rig.run_to(pulled);
  CHECK(g_rig.s->playing());
  g_rig.m.quiet = true;
  // 48 clocks of silence (about a second) plus the blocks it takes to stop and let go
  g_rig.run_to(pulled + 90000);
  CHECK_FALSE(g_rig.s->playing());
  CHECK_FALSE(g_rig.s->following());
  CHECK(g_col.count(EventKind::Stopped) == 1u);
  CHECK(g_rig.link->stats().stale_releases == 1u);
  const midi::SyncStatus st = g_rig.link->status();
  CHECK_FALSE(st.following);
  CHECK_FALSE(st.clock_present);
  CHECK(st.bpm_x100 == 0u);
  // it comes back: clocks, then a Start; the transport follows it again on its clock
  g_rig.m.quiet = false;
  g_rig.m.next = 0;  // a new run of clocks: its first comes 4e6 cycles from now
  g_rig.m.t0 = static_cast<double>(g_rig.sample) * kCycD + 4.0e6;
  const std::uint32_t steps_before = g_col.count(EventKind::Step);
  g_rig.s->select(seq::PlayMode::Pattern, 1);
  g_rig.send(g_rig.m.time_of(30) - 540000.0, midi::kStart);
  g_rig.run_to(g_rig.sample + 200000);
  CHECK(g_rig.s->playing());
  CHECK(g_rig.s->following());
  CHECK(g_col.count(EventKind::Step) > steps_before + 8u);
}

TEST_CASE("midi link slave: a change of mode while following stops the transport first, then the sequencer is back on its own clock") {
  midi_project(9000);
  g_rig.init(slave_cfg(midi::Port::Din));
  slave_start(540000.0, 40);
  g_rig.run_to(120000);
  NEED(g_rig.s->playing());
  NEED(g_rig.s->following());
  g_rig.link->set_config(master_cfg(midi::kPortDin));
  g_rig.run_to(120000 + 640);
  CHECK_FALSE(g_rig.s->playing());
  CHECK_FALSE(g_rig.s->following());
  CHECK(g_col.count(EventKind::Stopped) == 1u);
  // as a master it plays on its own tempo (the project's, 90 BPM) and sends the clock for it
  g_rig.m.quiet = true;
  const std::uint32_t clocks_before = g_wires.count_din(midi::kClock);
  g_rig.s->play_pattern(1);
  g_rig.run_to(g_rig.sample + 30000);
  CHECK(g_rig.s->playing());
  CHECK_FALSE(g_rig.s->following());
  CHECK(g_wires.count_din(midi::kClock) > clocks_before + 20u);
  CHECK(g_rig.s->tempo_x100() == 9000u);
}

TEST_CASE("midi link slave: the Play key under an external clock starts on the master's next clock, never in the past") {
  midi_project(9000);
  g_rig.init(slave_cfg(midi::Port::Din));
  g_rig.s->select(seq::PlayMode::Pattern, 1);
  // the master has been running clocks alone for a while (no Start): the tracker is attached and idle
  g_rig.run_to(120000);
  NEED(g_rig.s->following());
  CHECK_FALSE(g_rig.s->playing());
  const std::uint32_t late_before = g_rig.s->stats().late_events;
  const std::uint32_t overrun_before = g_rig.s->stats().microstep_overrun;
  // the Play key, between two clocks
  g_rig.s->play_pattern(1);
  g_rig.run_to(g_rig.sample + 60000);
  NEED(g_col.count(EventKind::Step) >= 6u);
  CHECK(g_rig.s->playing());
  // the first step is on a master clock (to the rounding of a sample), the next ones six clocks apart
  const double t0 = step_time(g_col, 0);
  bool on_grid = false;
  for (std::uint32_t c = 0; c < 400; ++c) {
    if (std::fabs(sample_of_cycles(g_rig.m.time_of(c)) - t0) <= 1.5) {
      on_grid = true;
      CHECK(std::fabs(step_time(g_col, 1) - sample_of_cycles(g_rig.m.time_of(c + 6u))) <= 1.5);
    }
  }
  CHECK(on_grid);
  CHECK(g_rig.s->stats().late_events == late_before);        // nothing was placed in the past
  CHECK(g_rig.s->stats().microstep_overrun == overrun_before);
}

TEST_CASE("midi link: the device settings give the mode, the followed port, the correction, the output ports and the channel; the runtime-only rest is kept") {
  namespace ds = model::device_setting;
  model::Settings dev;
  dev.init(core::ParamKind::Device);
  midi::MidiConfig base;
  base.in_offset_dms[1] = 30;
  base.out_offset_dms[0] = -20;
  base.cc_number[2] = 11;
  base.follow = midi::FollowTightness::Tight;
  base.note_ports = midi::kPortUsb;  // overwritten: MIDI OUT owns it
  // The defaults of the file: CLOCK MODE off, MIDI OUT DIN + USB, OUT CHANNEL 1: no clock, both ports, channel 0 on the wire
  midi::MidiConfig c = seq::sync_config_from_settings(dev, base);
  CHECK(c.mode == midi::SyncMode::Off);
  CHECK(c.out_ports == (midi::kPortDin | midi::kPortUsb));
  CHECK(c.note_ports == (midi::kPortDin | midi::kPortUsb));
  CHECK(c.notes_channel == 0u);
  CHECK(c.in_offset_dms[1] == 30);  // the runtime's own, kept
  CHECK(c.out_offset_dms[0] == -20);
  CHECK(c.cc_number[2] == 11);
  CHECK(c.follow == midi::FollowTightness::Tight);
  // CLOCK MODE
  CHECK(dev.set(ds::ClockMode, 1.0f) == model::Status::Ok);
  CHECK(seq::sync_config_from_settings(dev, base).mode == midi::SyncMode::Master);
  CHECK(dev.set(ds::ClockMode, 2.0f) == model::Status::Ok);  // Slave follows the first port of MIDI IN: DIN + USB and DIN are DIN, USB alone is USB
  c = seq::sync_config_from_settings(dev, base);
  CHECK(c.mode == midi::SyncMode::Slave);
  CHECK(c.in_port == midi::Port::Din);
  CHECK(dev.set(ds::MidiInPort, 1.0f) == model::Status::Ok);
  CHECK(seq::sync_config_from_settings(dev, base).in_port == midi::Port::Usb);
  CHECK(dev.set(ds::MidiInPort, 0.0f) == model::Status::Ok);
  CHECK(seq::sync_config_from_settings(dev, base).in_port == midi::Port::Din);
  CHECK(dev.set(ds::ClockMode, 0.0f) == model::Status::Ok);
  // MIDI OUT and OUT CHANNEL
  CHECK(dev.set(ds::MidiOutPort, 1.0f) == model::Status::Ok);
  c = seq::sync_config_from_settings(dev, base);
  CHECK(c.out_ports == midi::kPortUsb);
  CHECK(c.note_ports == midi::kPortUsb);
  CHECK(dev.set(ds::MidiOutPort, 0.0f) == model::Status::Ok);
  CHECK(seq::sync_config_from_settings(dev, base).out_ports == midi::kPortDin);
  CHECK(dev.set(ds::MidiOutChannel, 16.0f) == model::Status::Ok);
  CHECK(seq::sync_config_from_settings(dev, base).notes_channel == 15u);
  // CLOCK IN (our rule): USB and MIDI In jack are a slave on that port, whatever CLOCK MODE says
  CHECK(dev.set(ds::ClockIn, 1.0f) == model::Status::Ok);
  c = seq::sync_config_from_settings(dev, base);
  CHECK(c.mode == midi::SyncMode::Slave);
  CHECK(c.in_port == midi::Port::Usb);
  CHECK(dev.set(ds::ClockIn, 2.0f) == model::Status::Ok);
  c = seq::sync_config_from_settings(dev, base);
  CHECK(c.mode == midi::SyncMode::Slave);
  CHECK(c.in_port == midi::Port::Din);
  CHECK(dev.set(ds::ClockMode, 1.0f) == model::Status::Ok);
  CHECK(seq::sync_config_from_settings(dev, base).mode == midi::SyncMode::Slave);
  // PAR-119
  CHECK(dev.set(ds::ClockSync, -3.0f) == model::Status::Ok);
  c = seq::sync_config_from_settings(dev, base);
  CHECK(c.sync_correction == -3);
  CHECK(dev.set(ds::ClockSync, 6.0f) == model::Status::Ok);
  CHECK(seq::sync_config_from_settings(dev, base).sync_correction == 6);
}

TEST_CASE("midi link: control changes of both ports reach MIDI Learn's ring, in order, with their port and channel; timing and notes do not, and without a ring nothing breaks") {
  midi_project();
  static midi::CcInput cc;
  midi::MidiConfig cfg;  // Off
  g_rig.init(cfg);
  g_rig.link->attach_cc(&cc);
  // DIN: a control change, a note on in between, a clock inside the next message and running status; USB: another channel
  g_rig.send(1.0e6, 0xB0);
  g_rig.send(1.0e6 + 1.0e5, 74);
  g_rig.send(1.0e6 + 2.0e5, 100);
  g_rig.send(1.0e6 + 3.0e5, 0x90);
  g_rig.send(1.0e6 + 4.0e5, 60);
  g_rig.send(1.0e6 + 5.0e5, 127);
  g_rig.send(1.0e6 + 6.0e5, 0xBF);
  g_rig.send(1.0e6 + 7.0e5, 7);
  g_rig.send(1.0e6 + 8.0e5, 11);
  g_rig.send(1.0e6 + 9.0e5, 8);
  g_rig.send(1.0e6 + 1.0e6, 22);
  g_rig.send(4.0e6, 0xB2, 1);
  g_rig.send(4.0e6 + 1.0e5, 120, 1);  // all sound off: not a control
  g_rig.send(4.0e6 + 2.0e5, 0, 1);
  g_rig.send(4.0e6 + 3.0e5, 21, 1);
  g_rig.send(4.0e6 + 4.0e5, 64, 1);
  g_rig.run_to(6000);
  midi::CcMsg m{};
  NEED(cc.pop(m));
  CHECK((m.port == 0 && m.channel == 1 && m.cc == 74 && m.value == 100));
  NEED(cc.pop(m));
  CHECK((m.port == 0 && m.channel == 16 && m.cc == 7 && m.value == 11));
  NEED(cc.pop(m));
  CHECK((m.port == 0 && m.channel == 16 && m.cc == 8 && m.value == 22));
  NEED(cc.pop(m));
  CHECK((m.port == 1 && m.channel == 3 && m.cc == 21 && m.value == 64));
  CHECK_FALSE(cc.pop(m));
  CHECK(cc.dropped() == 0u);
  CHECK(g_rig.link->stats().passed_dropped == 0u);  // handed on, not dropped
  // detached: the bytes are counted and dropped as before
  g_rig.link->attach_cc(nullptr);
  g_rig.send(static_cast<double>(g_rig.sample) * kCycD + 1.0e5, 0xB0);
  g_rig.send(static_cast<double>(g_rig.sample) * kCycD + 2.0e5, 1);
  g_rig.send(static_cast<double>(g_rig.sample) * kCycD + 3.0e5, 2);
  g_rig.run_to(g_rig.sample + 3000);
  CHECK_FALSE(cc.pop(m));
  CHECK(g_rig.link->stats().passed_dropped == 3u);
}

TEST_CASE("midi link: a configuration is clamped, taken at the next block, and the latest of several wins") {
  midi_project();
  g_rig.init(master_cfg(midi::kPortDin));
  g_rig.run_to(640);
  CHECK(g_rig.link->stats().configs == 1u);
  midi::MidiConfig c = master_cfg(midi::kPortUsb);
  c.out_offset_dms[1] = 30000;  // far out of range
  g_rig.link->set_config(midi::MidiConfig{});
  g_rig.link->set_config(c);
  CHECK(g_rig.link->config().out_offset_dms[1] == midi::kMaxOffsetDms);  // the main loop's view is the clamped value
  g_rig.run_to(1280);
  CHECK(g_rig.link->stats().configs == 2u);  // two writes, one application: the last
  CHECK(g_rig.link->out().config().clock_ports == midi::kPortUsb);
  CHECK(g_rig.link->out().config().offset_cycles[1] == midi::offset_cycles(midi::kMaxOffsetDms));
  CHECK(g_rig.s->config().emit_clock);
  CHECK(g_rig.s->config().start_lead == seq::MidiLink::kStartLead);
  // Off takes the clock out switch down again
  g_rig.link->set_config(midi::MidiConfig{});
  g_rig.run_to(1920);
  CHECK_FALSE(g_rig.s->config().emit_clock);
  CHECK(g_rig.s->config().start_lead == 0u);
  CHECK(g_rig.link->out().config().clock_ports == 0u);
}

// ---- After an export (OS-WAVEXP) -------------------------------------------------------------------------------------------------------------------------

TEST_CASE("midi link slave: after an export (the link not stepped, the interrupts stamping on) nothing stale is applied - no Start, the rings emptied - and the loop locks on the master again") {
  for (const bool resume : {true, false}) {
    CAPTURE(resume);
    midi_project(9000);
    g_rig.init(slave_cfg(midi::Port::Din));
    g_rig.s->select(seq::PlayMode::Pattern, 1);
    g_rig.run_to(200000);  // the master's clocks, no Start: locked and followed, the tracker stopped
    NEED(g_rig.link->status().clock_present);
    CHECK_FALSE(g_rig.s->playing());
    // The export: the engine renders offline for tens of seconds and does not step the link. What the UART's interrupt stamped meanwhile is in the ring: clocks, a Start, an SPP's first byte.
    g_rig.m.quiet = resume;  // (with resume() the master is quiet here and comes back below; without it the control lets it run on)
    // (Stamped as the last of them would be, the ones just before the export ends: the worst case, each maps to a recent time. The older ones map to nothing sensible at all.)
    midi::RtInRing& ring = g_rig.link->din_in();
    const std::uint64_t now = g_rig.sample * kCyc;
    for (std::uint32_t k = 0; k < 100; ++k) {
      CHECK(ring.push(midi::RtItem{static_cast<std::uint32_t>(now - (101u - k) * kClockPeriod / 4u), midi::kClock, 0, 0}));
    }
    CHECK(ring.push(midi::RtItem{static_cast<std::uint32_t>(now - 100000u), midi::kStart, 0, 0}));
    const seq::MidiLink::Stats before = g_rig.link->stats();
    if (resume) {
      g_rig.link->resume();  // Engine::offline_leave, through EventSource::resume
    }
    g_rig.run_to(g_rig.sample + 64 * 50);
    const seq::MidiLink::Stats after = g_rig.link->stats();
    if (resume) {
      CHECK_FALSE(g_rig.s->playing());
      CHECK(after.events_applied == before.events_applied);  // the stale Start was not applied
      CHECK(after.rings_discarded - before.rings_discarded >= 101u);
      CHECK_FALSE(g_rig.s->following());  // the loop let go, to be acquired afresh
      // The master is still there: its clocks come back, the loop locks again, and still nothing starts by itself.
      g_rig.m.quiet = false;
      g_rig.m.next = 0;
      g_rig.m.t0 = static_cast<double>(g_rig.sample) * kCycD + 4.0e6;
      g_rig.run_to(g_rig.sample + 200000);
      const midi::SyncStatus st = g_rig.link->status();
      CHECK(st.clock_present);
      CHECK(st.locked);
      CHECK(st.bpm_x100 == 12000u);
      CHECK_FALSE(g_rig.s->playing());
    } else {
      g_rig.m.quiet = false;  // the control: without resume() the stale Start is armed and applied on the master's next clock
      g_rig.run_to(g_rig.sample + 64 * 400);
      CHECK(g_rig.link->stats().events_applied > before.events_applied);
    }
  }
}

// ---- TRANSPORT IN / OUT (E-CFG-03) -----------------------------------------------------------------------------------------------------------

TEST_CASE("midi link master: TRANSPORT OUT Off sends the clock but no Start, SPP, Continue or Stop") {
  for (const std::uint32_t from_step : {0u, 8u}) {  // a Start, and a play from the middle (SPP + Continue)
    CAPTURE(from_step);
    midi_project();
    midi::MidiConfig c = master_cfg(midi::kPortDin | midi::kPortUsb);
    c.transport_out = false;
    g_rig.init(c);
    g_rig.s->play_pattern(1, from_step);
    g_rig.run_to(20000);
    CHECK_FALSE(g_rig.s->config().emit_transport);  // the render thread applies the config at a block start
    CHECK(g_rig.s->config().start_lead == 0u);      // nothing to put on the wire first
    CHECK(g_wires.count_din(midi::kClock) > 10u);  // the clock still goes out
    CHECK(g_wires.count_usb_byte0(0xF8) > 10u);
    CHECK(g_wires.count_din(0x92) >= 1u);  // and the notes
    g_rig.s->stop();
    g_rig.run_to(30000);
    for (const std::uint8_t b : {midi::kStart, midi::kContinue, midi::kStop, midi::kSongPosition}) {
      CHECK(g_wires.count_din(b) == 0u);
    }
    for (const std::uint8_t b : {midi::kStart, midi::kContinue, midi::kStop, midi::kSongPosition}) {
      CHECK(g_wires.count_usb_byte0(b) == 0u);
    }
    CHECK(g_col.count(EventKind::Stopped) == 1u);
    CHECK(g_rig.link->out().book().owed() == 0u);
  }
  // The control: with the switch On (the default) the same run sends them.
  midi_project();
  g_rig.init(master_cfg(midi::kPortDin));
  CHECK(g_rig.s->config().emit_transport);
  g_rig.s->play_pattern(1);
  g_rig.run_to(20000);
  g_rig.s->stop();
  g_rig.run_to(30000);
  CHECK(g_wires.count_din(midi::kStart) == 1u);
  CHECK(g_wires.count_din(midi::kStop) == 1u);
}

TEST_CASE("midi link master: the switch is latched at Play, so turning TRANSPORT OUT Off mid-run keeps the Stop of the Start that went out, and On mid-run adds none") {
  midi_project();
  g_rig.init(master_cfg(midi::kPortDin));
  g_rig.s->play_pattern(1);
  g_rig.run_to(10000);
  CHECK(g_wires.count_din(midi::kStart) == 1u);
  midi::MidiConfig off = master_cfg(midi::kPortDin);
  off.transport_out = false;
  g_rig.link->set_config(off);
  g_rig.run_to(14000);
  CHECK_FALSE(g_rig.s->config().emit_transport);  // taken at a block start
  CHECK(g_rig.s->playing());
  g_rig.s->stop();
  g_rig.run_to(20000);
  CHECK(g_wires.count_din(midi::kStop) == 1u);  // the Stop of the run's Start
  // The other way round: a run that began with the switch Off has no Start, and no Stop when it is turned On meanwhile.
  g_rig.link->set_config(off);
  g_rig.run_to(24000);
  g_rig.s->play_pattern(1);
  g_rig.run_to(30000);
  g_rig.link->set_config(master_cfg(midi::kPortDin));
  g_rig.run_to(34000);
  g_rig.s->stop();
  g_rig.run_to(40000);
  CHECK(g_wires.count_din(midi::kStart) == 1u);
  CHECK(g_wires.count_din(midi::kStop) == 1u);
}

TEST_CASE("midi link slave: TRANSPORT IN Off ignores Start and Stop and SPP from the master, the clock is still followed and the Play key starts on the next clock") {
  midi_project(9000);
  midi::MidiConfig c = slave_cfg(midi::Port::Din);
  c.transport_in = false;
  g_rig.init(c);
  g_rig.s->select(seq::PlayMode::Pattern, 1);
  // The master: clocks from the start, a Start at clock 50, an SPP and a Continue, a Stop.
  g_rig.send(g_rig.m.time_of(50) - 540000.0, midi::kStart);
  g_rig.send(g_rig.m.time_of(60) - 540000.0, midi::kSongPosition);
  g_rig.send(g_rig.m.time_of(60) - 480000.0, 4);
  g_rig.send(g_rig.m.time_of(60) - 420000.0, 0);
  g_rig.send(g_rig.m.time_of(62) - 540000.0, midi::kContinue);
  g_rig.send(g_rig.m.time_of(70) - 540000.0, midi::kStop);
  g_rig.run_to(120000);
  CHECK_FALSE(g_rig.s->playing());  // none of it moved the transport
  CHECK(g_col.count(EventKind::Step) == 0u);
  const seq::MidiLink::Stats st = g_rig.link->stats();
  CHECK(st.events_applied == 0u);
  CHECK(st.transport_ignored >= 4u);
  CHECK(g_rig.link->status().transport_events == 0u);
  // the clock is followed: the loop is locked and the sequencer attached
  const midi::SyncStatus ss = g_rig.link->status();
  CHECK(ss.clock_present);
  CHECK(ss.locked);
  CHECK(ss.bpm_x100 == 12000u);
  NEED(g_rig.s->following());
  // the Play key starts on the master's next clock, on its grid
  g_rig.s->play_pattern(1);
  g_rig.run_to(g_rig.sample + 60000);
  NEED(g_col.count(EventKind::Step) >= 6u);
  CHECK(g_rig.s->playing());
  const double t0 = step_time(g_col, 0);
  bool on_grid = false;
  for (std::uint32_t k = 0; k < 400; ++k) {
    on_grid = on_grid || std::fabs(sample_of_cycles(g_rig.m.time_of(k)) - t0) <= 1.5;
  }
  CHECK(on_grid);
  // a Stop of the master does not stop it; the master's clock keeps it going
  g_rig.send(static_cast<double>(g_rig.sample) * kCycD + 1.0e6, midi::kStop);
  g_rig.run_to(g_rig.sample + 20000);
  CHECK(g_rig.s->playing());
  CHECK(g_rig.link->stats().transport_ignored >= 5u);
  CHECK(g_col.count(EventKind::Stopped) == 0u);
}

TEST_CASE("midi link slave: TRANSPORT OUT Off relays no transport, the clock is relayed and TRANSPORT IN still moves the transport") {
  midi_project(9000);
  midi::MidiConfig c = slave_cfg(midi::Port::Din, midi::kPortDin);
  c.transport_out = false;
  g_rig.init(c);
  const std::uint32_t c_start = 40;
  slave_start(540000.0, c_start);
  g_rig.send(g_rig.m.time_of(c_start + 6 * 5 + 3), midi::kStop);
  g_rig.send(g_rig.m.time_of(c_start + 6 * 5 + 9), midi::kSongPosition);
  g_rig.send(g_rig.m.time_of(c_start + 6 * 5 + 9) + 60000.0, 9);
  g_rig.send(g_rig.m.time_of(c_start + 6 * 5 + 9) + 120000.0, 0);
  g_rig.send(g_rig.m.time_of(c_start + 6 * 5 + 11) - 1.0e6, midi::kContinue);
  g_rig.run_to(160000);
  // the transport followed the master (the Stop, then the Continue at step 9) ...
  NEED(g_col.count(EventKind::Stopped) >= 1u);
  CHECK(g_rig.link->stats().events_applied >= 4u);
  CHECK(g_rig.link->stats().transport_ignored == 0u);
  CHECK(g_rig.s->playing());
  // ... and none of it went out; the relayed clock did
  CHECK(g_wires.count_din(midi::kClock) > 20u);
  for (const std::uint8_t b : {midi::kStart, midi::kContinue, midi::kStop, midi::kSongPosition}) {
    CHECK(g_wires.count_din(b) == 0u);
  }
}

TEST_CASE("midi link: sync_config_from_settings carries both transport switches") {
  namespace ds = model::device_setting;
  model::Settings dev;
  dev.init(core::ParamKind::Device);
  midi::MidiConfig base;
  base.transport_in = false;  // overwritten: the settings own them
  base.transport_out = false;
  midi::MidiConfig c = seq::sync_config_from_settings(dev, base);
  CHECK(c.transport_in);  // the default is On
  CHECK(c.transport_out);
  CHECK(dev.set(ds::TransportIn, 0.0f) == model::Status::Ok);
  c = seq::sync_config_from_settings(dev, base);
  CHECK_FALSE(c.transport_in);
  CHECK(c.transport_out);
  CHECK(dev.set(ds::TransportOut, 0.0f) == model::Status::Ok);
  CHECK(dev.set(ds::TransportIn, 1.0f) == model::Status::Ok);
  c = seq::sync_config_from_settings(dev, base);
  CHECK(c.transport_in);
  CHECK_FALSE(c.transport_out);
  // they are independent of the mode and of the other MIDI rows
  CHECK(dev.set(ds::ClockMode, 1.0f) == model::Status::Ok);
  CHECK(dev.set(ds::MidiOutPort, 0.0f) == model::Status::Ok);
  c = seq::sync_config_from_settings(dev, base);
  CHECK(c.mode == midi::SyncMode::Master);
  CHECK(c.out_ports == midi::kPortDin);
  CHECK_FALSE(c.transport_out);
}
