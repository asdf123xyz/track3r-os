// SPDX-License-Identifier: GPL-3.0-or-later
//
// MIDI sync end to end, the render side (seq/midi_link.h).
#include <o3os/seq/midi_link.h>

#include <o3os/core/audio_clock.h>
#include <o3os/model/params.h>

namespace o3os::seq {

using audio::EventKind;
using midi::MidiConfig;
using midi::SyncMode;

MidiLink::MidiLink(Sequencer& sequencer, const MidiLinkHooks& hooks) noexcept : seq_(sequencer), hooks_(hooks) {
  out_.attach(&din_, &usb_out_);
  apply_config(cfg_, 0);  // the defaults, into the router and the sequencer's clock-out switch
  stats_.configs = 0;
}

void MidiLink::set_config(const MidiConfig& c) noexcept { box_.write(midi::sanitized(c)); }

midi::SyncStatus MidiLink::status() const noexcept {
  midi::SyncStatus s;
  const std::uint32_t f = st_flags_.load(std::memory_order_relaxed);
  s.following = (f & 1u) != 0u;
  s.clock_present = (f & 2u) != 0u;
  s.locked = (f & 4u) != 0u;
  s.bpm_x100 = st_bpm_.load(std::memory_order_relaxed);
  s.clocks = st_clocks_.load(std::memory_order_relaxed);
  s.error_us = st_error_us_.load(std::memory_order_relaxed);
  s.transport_events = st_transport_.load(std::memory_order_relaxed);
  return s;
}

audio::EventSource MidiLink::event_source(audio::SeqEvent* buffer, std::uint32_t capacity) noexcept {
  audio::EventSource source;
  source.self = this;
  source.step = [](void* self, std::uint64_t block_start, std::uint32_t n, audio::SeqEvent* out, std::uint32_t cap) noexcept -> std::uint32_t {
    return static_cast<MidiLink*>(self)->step(block_start, n, out, cap);
  };
  source.param = [](void* self, const core::ParamChange& change) noexcept -> bool { return static_cast<MidiLink*>(self)->seq_.apply(change); };
  source.tempo = [](void* self) noexcept -> std::uint32_t { return static_cast<MidiLink*>(self)->seq_.tempo_x100(); };
  source.live = [](void* self, const audio::LiveEvent& e) noexcept -> bool { return static_cast<MidiLink*>(self)->live_event(e); };
  source.resume = [](void* self) noexcept { static_cast<MidiLink*>(self)->resume(); };
  source.stop_live = [](void* self, std::uint32_t* count) noexcept -> std::uint32_t { return static_cast<MidiLink*>(self)->stop_live_pads(count); };
  source.buffer = buffer;
  source.capacity = capacity;
  return source;
}

// ---- the block ----------------------------------------------------------------------------------------------------------------

std::uint32_t MidiLink::step(std::uint64_t block_start, std::uint32_t n, audio::SeqEvent* out, std::uint32_t cap) noexcept {
  // Without a platform hook the counter is taken to read the timeline's own cycles (the host tests drive the emitter with timeline times directly).
  const std::uint32_t heard = hooks_.heard_at != nullptr ? hooks_.heard_at(hooks_.ctx) : static_cast<std::uint32_t>(core::cycles_from_samples(block_start));
  begin_block(block_start, heard);

  const std::uint32_t count = seq_.step(block_start, n, out, cap);
  for (std::uint32_t i = 0; i < count; ++i) {
    const audio::SeqEvent& e = out[i];
    if (e.kind >= EventKind::MidiNoteOn && e.kind <= EventKind::MidiPolyAt) {
      (void)out_.send(e, block_start);
    } else if (e.kind == EventKind::Stopped) {
      // The sequencer has sent the offs of the notes it knew about (they came before this event); what is still owed after them is a note it lost track of.
      out_.stop_all(core::cycles_from_samples(block_start + e.offset));
    }
  }
  last_block_end_ = block_start + n;
  out_.flush(core::cycles_from_samples(last_block_end_));

  const std::uint32_t handed = out_.stats().handed;
  if (handed != handed_) {
    handed_ = handed;
    if (hooks_.kick != nullptr) {
      hooks_.kick(hooks_.ctx);  // the emitter's interrupt decides from its queues: a pend is always safe
      ++stats_.kicks;
    }
  }
  ++stats_.blocks;
  publish_status(block_start);
  return count;
}

// ---- live MIDI notes (the pads on a MIDI instrument; seq/midi_link.h LIVE MIDI NOTES) -----------------------------------------------

void MidiLink::live_release(std::uint32_t pad) noexcept {
  const std::uint32_t held = live_pad_[pad];
  if ((held & kLiveHeld) == 0u) {
    return;
  }
  live_pad_[pad] = 0;
  audio::SeqEvent ev{};
  ev.kind = EventKind::MidiNoteOff;
  ev.track = audio::kNoTrack;
  ev.a = static_cast<std::uint8_t>(held & 0x7Fu);
  ev.c = static_cast<std::uint8_t>((held >> 8) & 0x0Fu);
  ev.flags = static_cast<std::uint8_t>((held >> kLivePortsShift) & audio::kRoutePorts);  // the route of the note-on (OS-INSTUI S8b): an OUT, CHANNEL or OFFSET edit under the held pad changes only the next note
  ev.value = static_cast<std::int32_t>((held >> kLiveDelayShift) & 0x7Fu) * 1000;
  (void)out_.send(ev, last_block_end_);  // an off that is not owed (a stop took the note) is dropped by the router
}

std::uint32_t MidiLink::stop_live_pads(std::uint32_t* count) noexcept {
  std::uint32_t word = audio::kHeldNone;
  std::uint32_t n = 0;
  for (std::uint32_t pad = 0; pad < kLivePads; ++pad) {
    const std::uint32_t held = live_pad_[pad];
    if ((held & kLiveHeld) == 0u) {
      continue;
    }
    if (n == 0u) {
      word = static_cast<std::uint32_t>(audio::kLiveSourcePad) | (pad << 8) | (static_cast<std::uint32_t>(held & 0x7Fu) << 16) | ((((held >> 8) & 0x0Fu) + 1u) << 24);
    }
    ++n;
    live_release(pad);
  }
  if (n != 0u) {
    out_.flush(core::cycles_from_samples(last_block_end_));  // as step() ends: the offs go to the ports now
    if (hooks_.kick != nullptr) {
      hooks_.kick(hooks_.ctx);
      ++stats_.kicks;
    }
  }
  *count = n;
  return word;
}

bool MidiLink::live_event(const audio::LiveEvent& e) noexcept {
  if (e.kind == audio::LiveKind::On) {
    if (e.source != audio::kLiveSourcePad || e.key >= kLivePads) {
      return false;
    }
    live_release(e.key);  // a pad that goes down while it still holds a note (a lost release) ends that note first
    const model::Instrument* in = e.instrument >= 1u ? seq_.project().instrument(static_cast<std::uint32_t>(e.instrument) - 1u) : nullptr;
    if (in == nullptr || in->kind != model::InstrumentKind::Midi) {
      return false;
    }
    const MidiRoute r = midi_route(*in);  // as Sequencer::send_note_on: the channel 1..16 -> 0..15, the port and the OFFSET of the instrument (OS-INSTUI S8b)
    const std::uint8_t ch = r.ch;
    const std::uint8_t note = e.note > 127u ? std::uint8_t{127} : e.note;
    const std::uint8_t vel = e.velocity == audio::kLiveVelocityDefault ? midi_velocity_of(*in) : (e.velocity == 0u ? std::uint8_t{1} : (e.velocity > 127u ? std::uint8_t{127} : e.velocity));
    audio::SeqEvent ev{};
    ev.track = audio::kNoTrack;
    ev.c = ch;
    ev.flags = r.ports;
    ev.value = route_delay_us(r);
    const int program = seq_.program_due(*in);  // the receiver is put on the instrument's program before its first note
    if (program >= 0) {
      ev.kind = EventKind::MidiProgram;
      ev.a = static_cast<std::uint8_t>(program);
      (void)out_.send(ev, last_block_end_);
      seq_.program_sent(*in, program);  // limit of the pad path: MidiOut::send does not report a full staging queue, so the program is taken as told once it was handed over
    }
    ev.kind = EventKind::MidiNoteOn;
    ev.a = note;
    ev.b = vel;
    (void)out_.send(ev, last_block_end_);
    live_pad_[e.key] = kLiveHeld | (static_cast<std::uint32_t>(r.ports) << kLivePortsShift) | (static_cast<std::uint32_t>(r.delay_ms) << kLiveDelayShift) | (static_cast<std::uint32_t>(ch) << 8) | note;
    return true;
  }
  if (e.kind == audio::LiveKind::Off) {
    if (e.source == audio::kLiveSourcePad && e.key < kLivePads) {
      live_release(e.key);
    }
  } else if (e.kind == audio::LiveKind::AllOff && (e.source == audio::kLiveSourcePad || e.source == audio::kLiveAllSources)) {
    for (std::uint32_t pad = 0; pad < kLivePads; ++pad) {
      live_release(pad);
    }
  }
  return false;
}

void MidiLink::begin_block(std::uint64_t block_start, std::uint32_t heard) noexcept {
  ref_.publish(block_start, heard);   // the interrupts' time base
  map_.set_ref(block_start, heard);   // the stamps of incoming bytes
  MidiConfig c;
  if (box_.read_if_changed(c, box_seen_)) {
    apply_config(c, block_start);
  }
  drain_inputs();
  manage_follow(block_start);
}

// ---- configuration ------------------------------------------------------------------------------------------------------------

void MidiLink::apply_config(const MidiConfig& c, std::uint64_t block_start) noexcept {
  const MidiConfig old = cfg_;
  cfg_ = c;
  ++stats_.configs;
  const bool was_slave = old.mode == SyncMode::Slave;
  const bool is_slave = c.mode == SyncMode::Slave;
  const bool source_changed = was_slave != is_slave || (is_slave && old.in_port != c.in_port);

  // The router: where notes and clock go, the CC table, the OUT OFFSETs.
  midi::MidiOut::Config oc = out_.config();
  oc.note_ports = c.note_ports;
  oc.clock_ports = c.mode == SyncMode::Off ? std::uint8_t{0} : c.out_ports;
  oc.cc_channel = c.notes_channel;
  for (std::uint32_t i = 0; i < 6u; ++i) {
    oc.cc_number[i] = c.cc_number[i];
  }
  for (std::uint32_t p = 0; p < midi::kPortCount; ++p) {
    oc.offset_cycles[p] = midi::offset_cycles(c.out_offset_dms[p]);
  }
  out_.configure(oc);

  // The sequencer's clock-out switch: on while there is a port to send to (Master: the tracker's clock; Slave: the relay). With clock out on as a Master a Play begins one
  // block late so that SPP and Start go first. Both take effect at the next Play (the run's own clock state is set when it begins).
  Sequencer::Config sc = seq_.config();
  sc.emit_clock = oc.clock_ports != 0u;
  sc.emit_transport = c.transport_out;  // E-CFG-03: read by the sequencer when a run begins (render thread: this runs at a block start)
  sc.start_lead = (c.mode == SyncMode::Master && sc.emit_clock && c.transport_out) ? kStartLead : 0u;
  seq_.configure(sc);

  // MIDI in: configure() forgets an armed Start, so it is called only when something it holds changed.
  midi::MidiIn::Config ic = in_.config();
  if (ic.clock_enabled != is_slave || ic.clock_port != c.in_port || ic.start_pulses != c.sync_correction) {
    ic.clock_enabled = is_slave;
    ic.clock_port = c.in_port;
    ic.start_pulses = c.sync_correction;
    in_.configure(ic);
  }
  midi::ClockDll::Config dc = dll_.config();
  dc.track_mhz = midi::follow_mhz(c.follow);
  dll_.configure(dc);

  if (source_changed) {
    // Another timeline source: what the rings hold is the old one's (or from before anybody looked), the parsers' state belongs to it, and the loop starts over. A sequencer that
    // follows lets go (stopping first) in manage_follow().
    discard_inputs();
    in_.reset();
    reset_loop(block_start);
    if (seq_.following()) {
      want_release_ = true;
    }
  }
  if (seq_.following() && !want_release_) {
    seq_.set_follow(&dll_, in_offset_us());  // a new IN OFFSET takes effect at once
  }
}

// OS-WAVEXP: the engine was the export's for a while and this link was not stepped. The interrupts went on stamping bytes into the rings (their stamps are cycle counts of a time that is
// 10 or 20 seconds gone: they map to nonsense, and a stale Start, Continue or SPP would start the transport as the export ends), the loop's phase is as old, and the parsers hold a half message:
// the same start over as a change of the followed source (apply_config). Main loop, the render parked: nothing else touches the rings' consumer side.
void MidiLink::resume() noexcept {
  discard_inputs();
  in_.reset();
  reset_loop(last_block_end_);
  if (seq_.following()) {
    want_release_ = true;
  }
}

void MidiLink::reset_loop(std::uint64_t origin) noexcept {
  dll_.reset(origin, 8u * core::samples_per_tick_q32(seq_.nominal_tempo_x100()));
}

// ---- input --------------------------------------------------------------------------------------------------------------------

void MidiLink::discard_inputs() noexcept {
  midi::RtItem it;
  for (std::uint32_t p = 0; p < midi::kPortCount; ++p) {
    for (std::uint32_t i = 0; i < midi::kRtInRingSize && in_ring_[p].pop(it); ++i) {
      ++stats_.rings_discarded;
    }
  }
}

void MidiLink::drain_inputs() noexcept {
  midi::TransportEvent ev[kEventCap];
  midi::RtItem pass[kPassCap];
  std::uint32_t nev = 0;
  for (std::uint32_t p = 0; p < midi::kPortCount; ++p) {
    // Both rings are drained in every mode, so that neither fills: with the clock not followed the timing bytes are counted and dropped by MidiIn.
    const midi::MidiIn::Drained d = in_.drain(in_ring_[p], map_, dll_, kDrainItems, ev + nev, kEventCap - nev, pass, kPassCap);
    nev += d.events;
    if (cc_ != nullptr) {
      for (std::uint32_t i = 0; i < d.passed; ++i) {
        cc_->feed(pass[i]);  // control changes for MIDI Learn (bounded: kPassCap bytes a call)
      }
    } else {
      stats_.passed_dropped += d.passed;
    }
  }
  for (std::uint32_t i = 0; i < nev; ++i) {
    const midi::TransportEvent& e = ev[i];
    if (!cfg_.transport_in) {
      ++stats_.transport_ignored;  // E-CFG-03: TRANSPORT IN Off: the clock is followed (the DLL), the transport bytes are not acted on
      continue;
    }
    const bool begins = e.kind == midi::TransportEvent::Kind::Start || e.kind == midi::TransportEvent::Kind::Continue;
    if (begins && !seq_.following() && dll_.has_phase() && !want_release_) {
      // The master's own start restarts the run on the master's clock anyway, so the loop may be attached even though the tracker is playing on its own.
      seq_.set_follow(&dll_, in_offset_us());
      ++stats_.follow_attached;
    }
    seq_.apply(e);
    ++stats_.events_applied;
    st_transport_.store(st_transport_.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
  }
}

// ---- following ----------------------------------------------------------------------------------------------------------------

// Lets go of the master: the sequencer is stopped first (its timeline is the loop's: it cannot be handed back to the internal one while it runs), and once it has stopped
// (the next block) the loop is detached and started over. Called every block while `want_release_` or while the follow has to end.
void MidiLink::release_follow() noexcept {
  if (!seq_.following()) {
    want_release_ = false;
    stop_requested_ = false;
    return;
  }
  if (seq_.playing()) {
    if (!stop_requested_) {
      seq_.stop();
      stop_requested_ = true;
    }
    return;
  }
  seq_.clear_follow();
  stop_requested_ = false;
  want_release_ = false;
  reset_loop(last_block_end_);
}

void MidiLink::manage_follow(std::uint64_t block_start) noexcept {
  const bool slave = cfg_.mode == SyncMode::Slave;
  if (!slave || want_release_) {
    release_follow();
    return;
  }
  const core::SQ32 now_rel = (static_cast<core::SQ32>(block_start) - static_cast<core::SQ32>(dll_.origin())) * static_cast<core::SQ32>(core::kQ32One);
  const bool alive = dll_.has_phase() && !dll_.stale(now_rel, kStaleClocks);
  if (seq_.following()) {
    if (!alive) {
      ++stats_.stale_releases;
      want_release_ = true;  // the master went quiet (or the loop was reset under us): stop, then detach
      release_follow();
    }
  } else if (alive && !seq_.playing()) {
    seq_.set_follow(&dll_, in_offset_us());
    ++stats_.follow_attached;
  } else if (dll_.has_phase() && !alive) {
    reset_loop(block_start);  // a stale loop with nobody attached: forget it so that a master that comes back is acquired afresh
  }
}

void MidiLink::publish_status(std::uint64_t block_start) noexcept {
  const bool has = dll_.has_phase();
  const core::SQ32 now_rel = (static_cast<core::SQ32>(block_start) - static_cast<core::SQ32>(dll_.origin())) * static_cast<core::SQ32>(core::kQ32One);
  const bool present = cfg_.mode == SyncMode::Slave && has && !dll_.stale(now_rel, kStaleClocks);
  const std::uint32_t flags = (seq_.following() ? 1u : 0u) | (present ? 2u : 0u) | (present && dll_.locked() ? 4u : 0u);
  st_flags_.store(flags, std::memory_order_relaxed);
  st_bpm_.store(present ? dll_.bpm_x100() : 0u, std::memory_order_relaxed);
  st_clocks_.store(in_.stats().clocks, std::memory_order_relaxed);
  float e = dll_.last_error_samples();
  e = e < 0.0f ? -e : e;
  const float us = e * (static_cast<float>(core::kRefCyclesPerSample.num) / static_cast<float>(core::kRefCyclesPerSample.den)) / 180.0f;  // cycles per sample / 180 cycles per us
  st_error_us_.store(present && us < 1.0e6f ? static_cast<std::uint32_t>(us) : 0u, std::memory_order_relaxed);
}

// ---- the settings the device file carries ---------------------------------------------------------------------------------------

MidiConfig sync_config_from_settings(const model::Settings& device, const MidiConfig& base) noexcept {
  namespace ds = model::device_setting;
  const auto choice = [&device](unsigned field, unsigned top) noexcept {
    const float v = device.get(field);
    return v >= static_cast<float>(top) ? top : (v >= 1.0f ? static_cast<unsigned>(v) : 0u);
  };
  MidiConfig c = base;
  // MIDI OUT: DIN, USB, DIN + USB. Clock and transport and the notes of MIDI tracks share it.
  static constexpr std::uint8_t kPortSets[3] = {midi::kPortDin, midi::kPortUsb, static_cast<std::uint8_t>(midi::kPortDin | midi::kPortUsb)};
  const std::uint8_t out = kPortSets[choice(ds::MidiOutPort, 2)];
  c.out_ports = out;
  c.note_ports = out;
  const unsigned out_channel = choice(ds::MidiOutChannel, 16);  // 1..16; 0 only for a value below the range
  c.notes_channel = static_cast<std::uint8_t>(out_channel > 0u ? out_channel - 1u : 0u);
  const unsigned clock_in = choice(ds::ClockIn, 2);
  if (clock_in != 0u) {
    c.mode = SyncMode::Slave;
    c.in_port = clock_in == 1u ? midi::Port::Usb : midi::Port::Din;
  } else {
    const unsigned mode = choice(ds::ClockMode, 2);
    c.mode = mode == 2u ? SyncMode::Slave : (mode == 1u ? SyncMode::Master : SyncMode::Off);
    if (mode == 2u) {
      c.in_port = choice(ds::MidiInPort, 2) == 1u ? midi::Port::Usb : midi::Port::Din;  // USB only when it is the only port listened to
    }
  }
  c.transport_in = device.get(ds::TransportIn) >= 0.5f;  // E-CFG-03: Off (0) / On (1, the default)
  c.transport_out = device.get(ds::TransportOut) >= 0.5f;
  const float corr = device.get(ds::ClockSync);
  c.sync_correction = static_cast<std::int8_t>(corr < -6.0f ? -6 : (corr > 6.0f ? 6 : static_cast<int>(corr)));
  return midi::sanitized(c);
}

static_assert(model::kMidiPorts == midi::kPortCount && model::kMidiPortMask == midi::kPortMask && model::kMidiCcUnassigned == midi::kCcUnassigned &&
                  model::kMaxMidiOffsetDms == midi::kMaxOffsetDms && model::kMidiCcSlots == sizeof(midi::MidiConfig::cc_number) &&
                  model::kMidiFollowMax == static_cast<std::uint8_t>(midi::FollowTightness::Loose),
              "model/project_state.h mirrors midi/config.h");

model::MidiState project_midi_state(const MidiConfig& cfg) noexcept {
  const MidiConfig c = midi::sanitized(cfg);
  model::MidiState s;
  s.out_ports = c.out_ports;
  s.note_ports = c.note_ports;
  s.notes_channel = c.notes_channel;
  s.follow = static_cast<std::uint8_t>(c.follow);
  for (std::uint32_t i = 0; i < model::kMidiCcSlots; ++i) {
    s.cc_number[i] = c.cc_number[i];
  }
  for (std::uint32_t p = 0; p < model::kMidiPorts; ++p) {
    s.in_offset_dms[p] = c.in_offset_dms[p];
    s.out_offset_dms[p] = c.out_offset_dms[p];
  }
  return s;
}

MidiConfig apply_project_midi(const MidiConfig& base, const model::MidiState& s) noexcept {
  MidiConfig c = base;
  c.out_ports = s.out_ports;
  c.note_ports = s.note_ports;
  c.notes_channel = s.notes_channel;
  c.follow = static_cast<midi::FollowTightness>(s.follow);
  for (std::uint32_t i = 0; i < model::kMidiCcSlots; ++i) {
    c.cc_number[i] = s.cc_number[i];
  }
  for (std::uint32_t p = 0; p < model::kMidiPorts; ++p) {
    c.in_offset_dms[p] = s.in_offset_dms[p];
    c.out_offset_dms[p] = s.out_offset_dms[p];
  }
  return midi::sanitized(c);
}

}  // namespace o3os::seq
