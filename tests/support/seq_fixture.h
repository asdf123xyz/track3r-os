// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared pieces of the OS-005c sequencer tests (host and Cortex-M4F): ONE project in a static arena (1.9 MB: the target's RAM holds one), step and instrument
// builders, an event collector that runs a sequencer over any block pattern and keeps (absolute sample, event) records or only a running hash, and a text
// dump for the golden tests. No vector on the hot path: the collector has a fixed capacity.

#include <o3os/audio/events.h>
#include <o3os/model/project.h>
#include <o3os/seq/sequencer.h>

#include "static_slot.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace o3os::test {

// The one project of the sequencer suites (function-local static: the same object in every translation unit that includes this header).
inline model::Project& seq_project() {
  static model::ModelArena arena;
  return arena.project;
}

inline model::Step make_step(std::uint8_t note0, std::uint8_t instr = 1, std::uint8_t fx0 = 0, std::uint8_t v0 = 0, std::uint8_t fx1 = 0, std::uint8_t v1 = 0,
                             std::uint8_t vol = model::kVolumeUnset) {
  model::Step s = model::kEmptyStep;
  s.note = static_cast<std::uint8_t>(note0 + 1u);  // the model's note byte is the MIDI note plus one
  s.instr = instr;
  s.vol = vol;
  s.fx[0] = {fx0, v0};
  s.fx[1] = {fx1, v1};
  return s;
}
inline model::Step fx_step(std::uint8_t fx0, std::uint8_t v0, std::uint8_t fx1 = 0, std::uint8_t v1 = 0) {
  model::Step s = model::kEmptyStep;
  s.fx[0] = {fx0, v0};
  s.fx[1] = {fx1, v1};
  return s;
}
inline model::Step special_step(std::uint8_t note_byte, std::uint8_t instr = 1) {
  model::Step s = model::kEmptyStep;
  s.note = note_byte;
  s.instr = instr;
  return s;
}

// A fresh project: `tracks` active tracks, `sample_instr` sample instruments in slots 0..sample_instr-1 and a MIDI instrument on channel 3 in slot 10.
inline void fresh_project(std::uint32_t tracks = 4, std::uint32_t sample_instr = 4) {
  model::Project& p = seq_project();
  p.reset();
  (void)p.set_active_tracks(tracks);
  for (std::uint32_t i = 0; i < sample_instr; ++i) {
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Sample);
    (void)p.set_instrument(i, in);
  }
  model::Instrument m;
  model::init_instrument(m, model::InstrumentKind::Midi);
  m.midi_channel = 3;
  (void)p.set_instrument(10, m);
}

struct Rec;

struct Rec {
  std::uint64_t t;  // absolute sample
  audio::SeqEvent ev;
};

// Runs a sequencer over [0, total) in blocks of `block` samples (block 0: a cycle of sizes taken from `sizes`), keeping the events (up to kCap) and a hash.
class Collector {
 public:
  static constexpr std::uint32_t kCap = 6000;
  Rec rec[kCap];
  std::uint32_t n = 0;
  std::uint64_t hash = 0xCBF29CE484222325ull;
  std::uint32_t total_events = 0;
  bool keep = true;

  void add(std::uint64_t block_start, const audio::SeqEvent& e) {
    ++total_events;
    const std::uint64_t t = block_start + e.offset;
    mix(t);
    mix(static_cast<std::uint64_t>(e.kind) | (static_cast<std::uint64_t>(e.track) << 8) | (static_cast<std::uint64_t>(e.a) << 16) | (static_cast<std::uint64_t>(e.b) << 24) |
        (static_cast<std::uint64_t>(e.c) << 32) | (static_cast<std::uint64_t>(e.flags) << 40));
    mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(e.value)));
    if (keep && n < kCap) {
      rec[n].t = t;
      rec[n].ev = e;
      ++n;
    }
  }
  void mix(std::uint64_t v) {
    for (unsigned i = 0; i < 8; ++i) {
      hash ^= (v >> (8u * i)) & 0xFFu;
      hash *= 0x100000001B3ull;
    }
  }

  // Blocks of a fixed size from `from` to `to` (exclusive); the last block is shortened to end exactly at `to`.
  void run(seq::Sequencer& s, std::uint64_t from, std::uint64_t to, std::uint32_t block) {
    audio::SeqEvent out[audio::kMaxEventsPerBlock];
    for (std::uint64_t t = from; t < to;) {
      const std::uint32_t n_blk = static_cast<std::uint32_t>(to - t < block ? to - t : block);
      const std::uint32_t c = s.step(t, n_blk, out, audio::kMaxEventsPerBlock);
      for (std::uint32_t i = 0; i < c; ++i) {
        add(t, out[i]);
      }
      t += n_blk;
    }
  }
  // Block sizes from a table, cycled.
  void run_sizes(seq::Sequencer& s, std::uint64_t from, std::uint64_t to, const std::uint32_t* sizes, std::uint32_t count) {
    audio::SeqEvent out[audio::kMaxEventsPerBlock];
    std::uint32_t i = 0;
    for (std::uint64_t t = from; t < to; ++i) {
      std::uint32_t want = sizes[i % count];
      const std::uint32_t n_blk = static_cast<std::uint32_t>(to - t < want ? to - t : want);
      const std::uint32_t c = s.step(t, n_blk, out, audio::kMaxEventsPerBlock);
      for (std::uint32_t k = 0; k < c; ++k) {
        add(t, out[k]);
      }
      t += n_blk;
    }
  }

  // The records of one kind (and track, 0xFF any): how many, and the i-th one's time.
  std::uint32_t count(audio::EventKind k, std::uint8_t track = 0xFF) const {
    std::uint32_t c = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
      if (rec[i].ev.kind == k && (track == 0xFF || rec[i].ev.track == track)) {
        ++c;
      }
    }
    return c;
  }
  const Rec* nth(audio::EventKind k, std::uint32_t idx, std::uint8_t track = 0xFF) const {
    std::uint32_t c = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
      if (rec[i].ev.kind == k && (track == 0xFF || rec[i].ev.track == track)) {
        if (c == idx) {
          return &rec[i];
        }
        ++c;
      }
    }
    return nullptr;
  }
};

// One collector and one sequencer slot for every test file of a suite: the target's RAM holds one of each (a collector is 144 KB, a sequencer 8 KB).
inline Collector& seq_collector() {
  static Collector c;
  return c;
}
inline StaticSlot<seq::Sequencer>& seq_slot() {
  static StaticSlot<seq::Sequencer> slot;
  return slot;
}

inline const char* kind_name(audio::EventKind k) {
  using K = audio::EventKind;
  switch (k) {
    case K::NoteOn: return "ON";
    case K::NoteOff: return "OFF";
    case K::NoteCut: return "CUT";
    case K::NoteFade: return "FADE";
    case K::ModSet: return "MOD";
    case K::ModEnd: return "MODEND";
    case K::FxReset: return "FXRESET";
    case K::Tick: return "TICK";
    case K::StripMute: return "STRIPMUTE";
    case K::Metro: return "METRO";
    case K::TrkFx: return "TRKFX";
    case K::MidiNoteOn: return "MON";
    case K::MidiNoteOff: return "MOFF";
    case K::MidiCc: return "MCC";
    case K::MidiProgram: return "MPC";
    case K::MidiClock: return "MCLK";
    case K::MidiStart: return "MSTART";
    case K::MidiContinue: return "MCONT";
    case K::MidiStop: return "MSTOP";
    case K::MidiSpp: return "MSPP";
    case K::MidiPitchBend: return "MBEND";
    case K::MidiChanAt: return "MCAT";
    case K::MidiPolyAt: return "MPAT";
    case K::Step: return "STEP";
    case K::Pattern: return "PAT";
    case K::SongEnd: return "SONGEND";
    case K::Stopped: return "STOPPED";
    default: return "?";
  }
}

// "t kind track a b c flags value" lines for the records, skipping the kinds in `skip_mask` (bit per EventKind value < 64). Written by hand: the target's nano printf has no %llu.
inline char* put_u64(char* p, std::uint64_t v) {
  char tmp[24];
  unsigned n = 0;
  do {
    tmp[n++] = static_cast<char>('0' + v % 10u);
    v /= 10u;
  } while (v != 0u);
  while (n != 0u) {
    *p++ = tmp[--n];
  }
  return p;
}
inline char* put_i32(char* p, std::int32_t v) {
  if (v < 0) {
    *p++ = '-';
    return put_u64(p, static_cast<std::uint64_t>(-static_cast<std::int64_t>(v)));
  }
  return put_u64(p, static_cast<std::uint64_t>(v));
}
inline std::uint32_t dump(const Collector& c, char* buf, std::uint32_t cap, std::uint64_t skip_mask = 0) {
  char* p = buf;
  buf[0] = '\0';
  for (std::uint32_t i = 0; i < c.n && static_cast<std::uint32_t>(p - buf) + 100u < cap; ++i) {
    const Rec& r = c.rec[i];
    if (static_cast<std::uint32_t>(r.ev.kind) < 64u && ((skip_mask >> static_cast<std::uint32_t>(r.ev.kind)) & 1u)) {
      continue;
    }
    p = put_u64(p, r.t);
    *p++ = ' ';
    for (const char* n = kind_name(r.ev.kind); *n != '\0'; ++n) {
      *p++ = *n;
    }
    const std::uint32_t f[5] = {r.ev.track, r.ev.a, r.ev.b, r.ev.c, r.ev.flags};
    for (const std::uint32_t v : f) {
      *p++ = ' ';
      p = put_u64(p, v);
    }
    *p++ = ' ';
    p = put_i32(p, r.ev.value);
    *p++ = '\n';
    *p = '\0';
  }
  return static_cast<std::uint32_t>(p - buf);
}

constexpr std::uint64_t skip(audio::EventKind k) { return std::uint64_t{1} << static_cast<std::uint32_t>(k); }

// Helpers for the oracle tests, in the rate of the build (core/audio_clock.h).
constexpr double samples_per_second() {
  return static_cast<double>(core::kCpuHz) * core::kRefCyclesPerSample.den / core::kRefCyclesPerSample.num;
}

// Samples in one tick of the 192 PPQN grid at 120 BPM (384 ticks a second): 114.89 at 4080 cycles a sample, 114.84375 at 44,100 Hz.
constexpr double samples_per_tick_120() { return samples_per_second() / 384.0; }

}  // namespace o3os::test
