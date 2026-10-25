// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-INSTUI S2b: the live MIDI note path end to end on the host, for audio_live_midi_test.cpp and inst_live_midi_test.cpp. A real Engine (voices attached) with the real LiveBox queue, over a
// real seq::MidiLink and Sequencer on a project of the caller's; the ports' interrupts are stepped through each block the way seq_midi_link_test.cpp does, and everything that reaches the
// wire is captured twice, from the DIN emitter (bytes, with running status undone) and from the USB ring (event words). A test pushes live events on `box().queue`, calls block(), and
// reads notes(). Host only (std::vector); the big objects live in function-local statics (one rig at a time, like the other suites' StaticSlots).

#include <o3os/audio/engine.h>
#include <o3os/audio/live_notes.h>
#include <o3os/audio/voices.h>
#include <o3os/core/qtime.h>
#include <o3os/hal/audio.h>
#include <o3os/model/project.h>
#include <o3os/seq/midi_link.h>
#include <o3os/seq/sequencer.h>

#include "static_slot.h"

#include <cstdint>
#include <vector>

namespace o3os::test {

// One channel note message that reached a port.
struct WireNote {
  bool on;
  std::uint8_t ch;
  std::uint8_t note;
  std::uint8_t vel;
  bool operator==(const WireNote& o) const { return on == o.on && ch == o.ch && note == o.note && vel == o.vel; }
};

// OS-INSTUI S8b: any channel message that reached a port (note, program change, controller, aftertouch, pitch bend), with the cycle it started at (DIN: the first byte of the message on the wire;
// USB: the start of the 1 ms frame that carried it).
struct WireMsg {
  std::uint8_t port;  // 0 DIN, 1 USB
  std::uint8_t len;   // 2 or 3
  std::uint8_t b[3];
  std::uint64_t at;
  bool is(std::uint8_t status_hi, std::uint8_t ch) const { return (b[0] & 0xF0u) == status_hi && (b[0] & 0x0Fu) == ch; }
};

class LiveMidiRig {
 public:
  static std::uint32_t fake_cycles() noexcept {
    static std::uint32_t t = 0;
    return t += 7u;
  }

  // Builds everything over `project` (the sequencer plays it, the link reads its instruments). `ports`: midi::kPortDin | midi::kPortUsb.
  void init(model::Project& project, std::uint8_t ports = midi::kPortDin | midi::kPortUsb) {
    static StaticSlot<seq::Sequencer> seq_slot;
    static StaticSlot<seq::MidiLink> link_slot;
    static StaticSlot<audio::Engine> engine_slot;
    static StaticSlot<audio::VoicePool> pool_slot;
    static audio::TapMem taps;
    static audio::SeqEvent events[audio::kMaxEventsPerBlock];
    static audio::LiveBox live_box;
    live_box.notes = audio::LiveNotes{};
    live_box.pending = audio::LiveEvent{};
    live_box.pending_off = false;
    audio::LiveEvent stale;
    while (live_box.queue.pop(stale)) {
    }
    seq_ = &seq_slot.make(project);
    link_ = &link_slot.make(*seq_, seq::MidiLinkHooks{});
    midi::MidiConfig cfg;
    cfg.note_ports = ports;
    link_->set_config(cfg);
    engine_ = &engine_slot.make(&fake_cycles, taps);
    pool_ = &pool_slot.make();
    engine_->attach_voices(pool_);
    engine_->attach_live(&live_box);
    engine_->attach_events(link_->event_source(events, audio::kMaxEventsPerBlock));
    box_ = &live_box;
    sample_ = 0;
    din_now_ = 0;
    next_frame_ = 0;
    din_notes_.clear();
    usb_notes_.clear();
    din_status_ = 0;
    din_have_ = 0;
    din_have_all_ = 0;
    din_msg_at_ = 0;
    msgs_.clear();
  }

  audio::LiveBox& box() { return *box_; }
  audio::Engine& engine() { return *engine_; }
  audio::VoicePool& pool() { return *pool_; }
  seq::Sequencer& sequencer() { return *seq_; }
  seq::MidiLink& link() { return *link_; }
  std::uint64_t sample() const { return sample_; }

  // One block of the render, then the ports' interrupts over it: the engine pops the live queue and steps the link; what the router handed the ports leaves them.
  void block() {
    alignas(8) std::int16_t in[hal::audio::kBlockSamples] = {};
    alignas(8) std::int16_t out[hal::audio::kBlockSamples] = {};
    hal::audio::BlockReport report{};
    const hal::audio::BlockIo io{in, out, 0, &report};
    engine_->render_block(io);
    sample_ += hal::audio::kBlock;
    drain(core::cycles_from_samples(sample_));
  }
  void blocks(unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
      block();
    }
  }
  // Enough idle blocks for everything staged to be handed over and to leave the ports (the horizon is two blocks; the DIN wire takes about a millisecond a note, 23 blocks).
  void settle(unsigned n = 8) { blocks(n); }

  // What each port saw, in order. The two must always agree.
  const std::vector<WireNote>& notes() const { return usb_notes_; }
  const std::vector<WireNote>& din_notes() const { return din_notes_; }
  void clear_wire() {
    usb_notes_.clear();
    din_notes_.clear();
    msgs_.clear();
  }
  // Every channel message that reached either port, in the order each port saw them (the two ports' messages are interleaved by when they were captured), and the ones of one port.
  const std::vector<WireMsg>& msgs() const { return msgs_; }
  std::vector<WireMsg> msgs_of(std::uint8_t port) const {
    std::vector<WireMsg> r;
    for (const WireMsg& m : msgs_) {
      if (m.port == port) {
        r.push_back(m);
      }
    }
    return r;
  }

  // Every note-on has its note-off on the same channel and note, in that order, and nothing is left on: the count of each (channel, note) never goes below zero and ends at zero.
  static bool balanced(const std::vector<WireNote>& w) {
    int count[16][128] = {};
    for (const WireNote& n : w) {
      int& c = count[n.ch & 15u][n.note & 127u];
      c += n.on ? 1 : -1;
      if (c < 0) {
        return false;
      }
    }
    for (auto& row : count) {
      for (int c : row) {
        if (c != 0) {
          return false;
        }
      }
    }
    return true;
  }
  // How many notes are on at the end of the capture.
  static int held(const std::vector<WireNote>& w) {
    int n = 0;
    for (const WireNote& x : w) {
      n += x.on ? 1 : -1;
    }
    return n;
  }

 private:
  void drain(std::uint64_t end) {
    midi::DinEmitter& em = link_->din();
    for (std::uint32_t guard = 0; guard < 256; ++guard) {
      const std::uint64_t w = em.next_wake(din_now_);
      if (w == midi::kNever || w >= end) {
        break;
      }
      din_now_ = w;
      const midi::DinEmitter::Tx tx = em.poll(w);
      if (tx.valid) {
        din_byte(tx.byte, tx.start);
      } else {
        ++din_now_;
      }
    }
    din_now_ = din_now_ < end ? end : din_now_;
    while (next_frame_ < end) {
      std::uint32_t ev[midi::usb_out::kEventsPerFrame];
      const std::uint32_t n = link_->usb_out().frame(next_frame_, ev, midi::usb_out::kEventsPerFrame);
      for (std::uint32_t i = 0; i < n; ++i) {
        const std::uint8_t status = static_cast<std::uint8_t>((ev[i] >> 8) & 0xFFu);
        if (status >= 0x80u && status < 0xF0u) {
          msgs_.push_back(WireMsg{1, static_cast<std::uint8_t>(midi::data_bytes_of(status) + 1u), {status, static_cast<std::uint8_t>((ev[i] >> 16) & 0xFFu), static_cast<std::uint8_t>((ev[i] >> 24) & 0xFFu)}, next_frame_});
        }
        if ((status & 0xE0u) == 0x80u && status < 0xA0u) {
          usb_notes_.push_back(WireNote{(status & 0xF0u) == 0x90u, static_cast<std::uint8_t>(status & 0x0Fu), static_cast<std::uint8_t>((ev[i] >> 16) & 0xFFu),
                                        static_cast<std::uint8_t>((ev[i] >> 24) & 0xFFu)});
        }
      }
      next_frame_ += midi::usb_out::kFrameCycles;
    }
  }
  // The DIN byte stream, running status undone: note messages are kept, everything else (clock, program change, ...) is skipped.
  void din_byte(std::uint8_t b, std::uint64_t at) {
    if (b >= 0xF8u) {
      return;
    }
    if (b >= 0x80u) {
      din_status_ = b;
      din_have_ = 0;
      din_have_all_ = 0;
      din_msg_at_ = at;
      return;
    }
    if (din_status_ >= 0x80u && din_status_ < 0xF0u) {  // S8b: every channel message, running status undone (a message with running status starts at its first data byte)
      if (din_have_all_ == 0u && din_msg_at_ == 0xFFFFFFFFFFFFFFFFull) {
        din_msg_at_ = at;
      }
      din_all_[din_have_all_++] = b;
      if (din_have_all_ == midi::data_bytes_of(din_status_)) {
        msgs_.push_back(WireMsg{0, static_cast<std::uint8_t>(din_have_all_ + 1u), {din_status_, din_all_[0], din_have_all_ > 1u ? din_all_[1] : std::uint8_t{0}}, din_msg_at_});
        din_have_all_ = 0;
        din_msg_at_ = 0xFFFFFFFFFFFFFFFFull;  // the next message under running status starts at its first data byte
      }
    }
    if ((din_status_ & 0xE0u) != 0x80u || din_status_ >= 0xA0u) {
      return;  // not a note message: its data bytes are not ours
    }
    din_data_[din_have_++] = b;
    if (din_have_ == 2u) {
      din_notes_.push_back(WireNote{(din_status_ & 0xF0u) == 0x90u, static_cast<std::uint8_t>(din_status_ & 0x0Fu), din_data_[0], din_data_[1]});
      din_have_ = 0;  // running status: the next two data bytes are another message of the same status
    }
  }

  seq::Sequencer* seq_ = nullptr;
  seq::MidiLink* link_ = nullptr;
  audio::Engine* engine_ = nullptr;
  audio::VoicePool* pool_ = nullptr;
  audio::LiveBox* box_ = nullptr;
  std::uint64_t sample_ = 0;
  std::uint64_t din_now_ = 0;
  std::uint64_t next_frame_ = 0;
  std::vector<WireNote> din_notes_;
  std::vector<WireNote> usb_notes_;
  std::uint8_t din_status_ = 0;
  std::uint8_t din_have_ = 0;
  std::uint8_t din_data_[2] = {};
  std::uint8_t din_all_[2] = {};
  std::uint8_t din_have_all_ = 0;
  std::uint64_t din_msg_at_ = 0;
  std::vector<WireMsg> msgs_;
};

// Both ports carried the same notes in the same order (a note-off's velocity is the router's own: not compared).
inline bool same_notes_on_both_ports(const LiveMidiRig& rig) {
  const std::vector<WireNote>& a = rig.din_notes();
  const std::vector<WireNote>& b = rig.notes();
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].on != b[i].on || a[i].ch != b[i].ch || a[i].note != b[i].note || (a[i].on && a[i].vel != b[i].vel)) {
      return false;
    }
  }
  return true;
}

}  // namespace o3os::test
