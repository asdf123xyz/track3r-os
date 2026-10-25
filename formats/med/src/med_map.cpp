// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/formats/med/map.h>
#include <o3os/formats/volume_level.h>

#include <algorithm>
#include <cstring>

namespace o3os::formats::med {

namespace {

constexpr std::uint8_t kFull = 64;  // MED's full volume

inline std::uint32_t div_round(std::uint64_t n, std::uint64_t d) noexcept {
  return static_cast<std::uint32_t>((n + d / 2) / d);
}

// Rounds n / 16 to nearest, ties away from zero (the pan formulas).
inline int div16_round(int n) noexcept { return (n >= 0 ? n + 8 : n - 8) / 16; }

inline std::uint8_t percent_of_64(std::uint32_t v) noexcept {
  return static_cast<std::uint8_t>(div_round(static_cast<std::uint64_t>(std::min<std::uint32_t>(v, kFull)) * 100u, kFull));
}

}  // namespace

// ---- arithmetic -----------------------------------------------------------------------------------------------------

std::uint32_t paula_rate(Clock clock) noexcept {
  const std::uint32_t hz = clock == Clock::Ntsc ? pitch::kPaulaNtscHz : pitch::kPaulaPalHz;
  return div_round(hz, pitch::kUnityPeriod);
}

NoteResult map_note(const SongInfo& song, const Cell& c) noexcept {
  NoteResult r;
  if (c.note == 0) {
    if (c.note_hi) {  // the byte is exactly 0x80: OpenMPT reads a note cut (libopenmpt 0.7.3 measured), the layout document "reserved"
      r.note = model::kNoteCut;
      r.cut = true;
    }
    return r;
  }
  r.hi_ignored = c.note_hi;
  const int v = static_cast<int>(c.note) + pitch::kNoteOffset + static_cast<int>(song.play_transpose) +
                (song.version == 3 ? pitch::kMmd3NoteShift : 0);
  if (v < static_cast<int>(model::kNoteFirst) || v > static_cast<int>(model::kNoteLast)) {
    r.out_of_range = true;
    return r;
  }
  r.note = static_cast<std::uint8_t>(v);
  return r;
}

VolumeResult map_volume(const SongInfo& song, std::uint8_t data) noexcept {
  VolumeResult r;
  if (song.vol_hex()) {
    r.value = static_cast<std::uint8_t>(data & 0x7F);  // 7 bit: 0x7F is full, as in libopenmpt ((v & 0x7F) + 1) / 2 on 0..64
    r.clamped = (data & 0x80) != 0;
    return r;
  }
  std::uint32_t v = (static_cast<std::uint32_t>(data) >> 4) * 10u + (data & 0x0Fu);  // decimal-coded: 0x64 is 64
  if (v > kFull) {
    v = kFull;
    r.clamped = true;
  }
  r.value = static_cast<std::uint8_t>(std::min<std::uint32_t>(2u * v, model::kVolumeMax));
  return r;
}

TempoFx tempo_fx(std::uint32_t bpm_x100) noexcept {
  TempoFx t;
  const std::uint32_t v = (bpm_x100 + 100u) / 200u;  // BPM / 2, to nearest
  if (v < 5u || v > 200u) {
    t.clamped = true;
    t.value = v < 5u ? 5 : 200;
    return t;
  }
  t.value = static_cast<std::uint8_t>(v);
  const int r = static_cast<int>(bpm_x100) - 200 * static_cast<int>(v);  // -100..100 hundredths of a BPM
  const int k = (r >= 0 ? r + 12 : r - 12) / 25;                          // quarters of a BPM, to nearest
  if (k != 0) {
    t.fine = static_cast<std::uint8_t>(128 + k);
  }
  t.exact = (r % 25) == 0;
  return t;
}

std::uint8_t roll_for(std::uint32_t spd, std::uint32_t y, bool& exact) noexcept {
  static constexpr std::uint8_t kDivisor[8] = {1, 2, 3, 4, 6, 8, 12, 16};  // retriggers per step of Roll periods 1..8
  exact = false;
  if (y == 0 || spd == 0) {
    exact = true;  // no retrigger asked for
    return 0;
  }
  if (y >= spd) {
    exact = true;  // the first retrigger would come after the line ends
    return 0;
  }
  unsigned best = 0;
  std::uint32_t best_err = ~0u;
  for (unsigned i = 0; i < 8; ++i) {
    const std::uint32_t t = static_cast<std::uint32_t>(kDivisor[i]) * y;
    const std::uint32_t err = t > spd ? t - spd : spd - t;
    if (err < best_err) {
      best_err = err;
      best = i;
    }
  }
  if (kDivisor[best] == 1) {
    return 0;  // closer to one note per line than to two: nothing to roll
  }
  exact = best_err == 0;
  return static_cast<std::uint8_t>(best + 1);
}

std::uint8_t micro_move_for(std::uint32_t spd, std::uint32_t x, bool& exact) noexcept {
  if (spd == 0) {
    exact = false;
    return 0;
  }
  const std::uint64_t n = 100ull * x;
  exact = (n % spd) == 0 && n / spd <= 100;
  return static_cast<std::uint8_t>(std::min<std::uint32_t>(div_round(n, spd), 100));
}

std::uint8_t gate_for(std::uint32_t spd, std::uint32_t xx, bool& exact) noexcept { return micro_move_for(spd, xx, exact); }

std::uint8_t lfo_rate_for(std::uint32_t speed_nibble, std::uint32_t max) noexcept {
  return static_cast<std::uint8_t>(std::min<std::uint32_t>(div_round(static_cast<std::uint64_t>(speed_nibble) * max, 15), max));
}

std::uint8_t glide_for(std::uint32_t speed, std::uint32_t tick_us) noexcept {
  if (speed == 0) {
    speed = 1;
  }
  const std::uint64_t ms = div_round(214ull * tick_us, 1000ull * speed);  // 214 period units from 428 to 214
  std::uint32_t v;
  if (ms <= 1000) {
    v = div_round(ms, 20);                                  // 0..50 is 0..1000 ms
  } else {
    v = 50u + div_round(ms - 1000, 280);                    // 50..100 is 1000..15000 ms
  }
  return static_cast<std::uint8_t>(std::min<std::uint32_t>(v, 100));
}

// ---- the report -----------------------------------------------------------------------------------------------------

void ImportReport::reset() noexcept {
  version = 0;
  ignored = 0;
  master_volume_ignored = false;
  structural = false;
  overflow = false;
  initial_tempo_clamped = false;
  flow = FlowReport();
  patterns = 0;
  order = 0;
  tracks_used = 0;
  tracks_kept = 0;
  tracks_dropped = 0;
  for (TrackReport& t : track) {
    t = TrackReport();
  }
  cells = 0;
  notes_out_of_range = 0;
  note_cuts = 0;
  note_hi_ignored = 0;
  fx_overflow = 0;
  volume_clamped = 0;
  volume_overwritten = 0;
  volume_baked = 0;
  timing_carried = 0;
  timing_lost = 0;
  tempo_fx = 0;
  tempo_fx_clamped = 0;
  tempo_fx_fine = 0;
  speed_fx = 0;
  state_unknown = 0;
  std::memset(cmd_dropped, 0, sizeof cmd_dropped);
  std::memset(cmd_approx, 0, sizeof cmd_approx);
  instruments = 0;
  instruments_refused = 0;
  pcm_bytes = 0;
  for (InstReport& i : instrument) {
    i = InstReport();
  }
}

// ---- tracks ---------------------------------------------------------------------------------------------------------

std::uint32_t plan_tracks(ImportReport& r) noexcept {
  bool keep[caps::kMaxTracks] = {};
  std::uint32_t used = 0;
  for (std::uint32_t t = 0; t < caps::kMaxTracks; ++t) {
    r.track[t].slot = kNoSlot;
    if (r.track[t].cells != 0) {
      ++used;
      keep[t] = true;
    }
  }
  r.tracks_used = static_cast<std::uint16_t>(used);
  std::uint32_t kept = used;
  if (used > model::kMaxTracks) {
    // more than the model has slots for: keep the busiest, the lower track number winning a tie
    kept = model::kMaxTracks;
    for (std::uint32_t t = 0; t < caps::kMaxTracks; ++t) {
      keep[t] = false;
    }
    for (std::uint32_t k = 0; k < model::kMaxTracks; ++k) {
      std::uint32_t best = caps::kMaxTracks;
      for (std::uint32_t t = 0; t < caps::kMaxTracks; ++t) {
        if (r.track[t].cells != 0 && !keep[t] && (best == caps::kMaxTracks || r.track[t].cells > r.track[best].cells)) {
          best = t;
        }
      }
      keep[best] = true;
    }
  }
  std::uint32_t slot = 0;
  for (std::uint32_t t = 0; t < caps::kMaxTracks; ++t) {
    if (keep[t]) {
      r.track[t].slot = static_cast<std::uint8_t>(slot++);
    }
  }
  r.tracks_kept = static_cast<std::uint16_t>(kept);
  r.tracks_dropped = static_cast<std::uint16_t>(used - kept);
  return slot;
}

std::uint8_t track_pan_percent(const SongInfo& song, bool has_pans, std::int8_t file_pan, std::uint32_t t) noexcept {
  if (has_pans) {
    const int p = std::min(16, std::max(-16, static_cast<int>(file_pan)));
    return static_cast<std::uint8_t>(50 + div16_round(p * 50));
  }
  if (song.mixing()) {
    return 50;
  }
  const std::uint32_t k = t & 3u;  // Paula: channels 0 and 3 left, 1 and 2 right (8 channels repeat it)
  return (k == 0 || k == 3) ? 0 : 100;
}

std::uint8_t track_volume_percent(std::uint8_t med_volume) noexcept { return percent_of_64(med_volume); }

// ---- the commands ---------------------------------------------------------------------------------------------------

void LineMapper::begin(const SongInfo& song, const ImportOptions& options, const InstFact* facts, std::uint32_t initial_tempo,
                       std::uint32_t initial_spd, bool structural, ImportReport& report) noexcept {
  song_ = &song;
  opt_ = options;
  model_ = tempo_model(song, options.clock);
  facts_ = facts;
  rep_ = &report;
  structural_ = structural;
  tempo0_ = initial_tempo;
  spd0_ = initial_spd;
  begin_piece(0, 0, false);
  begin_line();
}

void LineMapper::begin_piece(std::uint32_t tempo, std::uint32_t spd, bool replay) noexcept {
  tempo_ = tempo != 0 ? tempo : tempo0_;
  spd_ = spd != 0 ? spd : spd0_;
  replay_ = replay;
  std::memset(inst_, 0, sizeof inst_);
  std::memset(vol_, -1, sizeof vol_);
  std::memset(eng_, 0, sizeof eng_);
}

void LineMapper::begin_line() noexcept {
  for (model::Step& s : stage_) {
    s = model::kEmptyStep;
  }
}

const InstFact* LineMapper::fact(std::uint32_t inst) const noexcept {
  if (inst == 0 || inst > caps::kMaxSamples || facts_ == nullptr || facts_[inst].svol == 0xFF) {
    return nullptr;
  }
  return &facts_[inst];
}

std::uint32_t LineMapper::tick_us() const noexcept {
  const Bpm b = tempo_to_bpm(model_, tempo_, spd_);  // a tick lasts 15 / (BPM x SPD) seconds
  return div_round(1500000000ull, static_cast<std::uint64_t>(b.x100) * std::max<std::uint32_t>(spd_, 1));
}

bool LineMapper::free_slots(const model::Step& s, unsigned n) const noexcept {
  unsigned free = 0;
  for (const model::StepFx& f : s.fx) {
    free += f.type == model::kFxNone ? 1u : 0u;
  }
  return free >= n;
}

bool LineMapper::put(model::Step& s, std::uint8_t type, std::uint8_t value) noexcept {
  for (model::StepFx& f : s.fx) {
    if (f.type == model::kFxNone) {
      f.type = type;
      f.value = value;
      return true;
    }
  }
  ++rep_->fx_overflow;
  return false;
}

void LineMapper::set_volume(model::Step& s, std::uint8_t v127) noexcept {
  if (s.vol != model::kVolumeUnset) {
    ++rep_->volume_overwritten;
  }
  s.vol = v127;
}

// Tempo (and Tempo fine when a slot remains beyond the `reserve` the caller needs for its own FX) for the BPM of (tempo, spd); a
// command without a slot for its Tempo is dropped.
void LineMapper::emit_tempo(model::Step& s, std::uint8_t cmd, std::uint32_t tempo, std::uint32_t spd, unsigned reserve) noexcept {
  const Bpm b = tempo_to_bpm(model_, tempo, spd);
  const TempoFx f = tempo_fx(b.x100);
  if (!put(s, fx::kTempo, f.value)) {
    drop(cmd);
    return;
  }
  ++rep_->tempo_fx;
  if (f.clamped) {
    ++rep_->tempo_fx_clamped;
  }
  bool fine_done = true;
  if (f.fine != 0) {
    fine_done = free_slots(s, 1u + reserve) && put(s, fx::kTempoFine, f.fine);
    if (fine_done) {
      ++rep_->tempo_fx_fine;
    }
  }
  if (!f.exact || !fine_done) {
    approx(cmd);
  }
}

void LineMapper::cell(std::uint32_t t, const Cell& c, std::uint32_t page) noexcept {
  if (t >= caps::kMaxTracks) {
    return;
  }
  model::Step& s = stage_[t];
  if (page == 0 && !replay_) {
    const NoteResult n = map_note(*song_, c);
    s.note = n.note;
    rep_->notes_out_of_range += n.out_of_range ? 1u : 0u;
    rep_->note_cuts += n.cut ? 1u : 0u;
    rep_->note_hi_ignored += n.hi_ignored ? 1u : 0u;
    if (c.instrument != 0) {
      s.instr = c.instrument;
      inst_[t] = c.instrument;
      const InstFact* f = fact(c.instrument);
      vol_[t] = f != nullptr ? static_cast<std::int8_t>(std::min<std::uint8_t>(f->svol, kFull)) : static_cast<std::int8_t>(-1);
    }
  }
  if (c.command != 0 || c.data != 0) {
    command(t, c);
  }
}

void LineMapper::timing_cell(std::uint32_t t, const Cell& c) noexcept {
  if (t >= caps::kMaxTracks) {
    return;
  }
  const bool tempo = c.command == 0x09 || (c.command == 0x0F && c.data >= 1 && c.data <= 0xF0);
  const bool flow = structural_ && ((c.command == 0x0F && c.data == 0) || c.command == 0x1D || c.command == 0x0B);
  if (tempo || flow) {
    command(t, c);
  }
}

void LineMapper::set_tempo(std::uint32_t t, std::uint32_t bpm, std::uint8_t cmd) noexcept {
  if (t >= caps::kMaxTracks) {
    return;
  }
  tempo_ = bpm;
  emit_tempo(stage_[t], cmd, tempo_, spd_, 0);
}

void LineMapper::set_speed(std::uint32_t t, std::uint32_t spd, std::uint8_t cmd) noexcept {
  if (t >= caps::kMaxTracks) {
    return;
  }
  model::Step& s = stage_[t];
  spd_ = spd;
  emit_tempo(s, cmd, tempo_, spd_, 1);  // the Tempo first (timing matters more than tick resolution), then the Speed, as 09 does
  if (put(s, fx::kSpeed, static_cast<std::uint8_t>(std::min<std::uint32_t>(spd, 32)))) {
    ++rep_->speed_fx;
    if (spd > 32) {
      approx(cmd);
    }
  } else {
    drop(cmd);
  }
}

void LineMapper::end_line(std::uint64_t tracks) noexcept {
  for (std::uint32_t t = 0; t < caps::kMaxTracks; ++t) {
    model::Step& s = stage_[t];
    if (opt_.engine_fx && ((tracks >> t) & 1u) != 0u) {
      // An engine command holds until its column's FX changes (seq/control.h), a MED command lasts its row: the row after a command (with no note, which ends the columns
      // by itself) says "no effect" in the free column, E00 00 (audio/med_tick.h). A column that holds another FX cannot say it (counted nowhere: rare).
      std::uint8_t now = 0;
      for (unsigned c = 0; c < 2u; ++c) {
        const bool real = s.fx[c].type >= fx::kEngineBase && !(s.fx[c].type == fx::kEngineBase && s.fx[c].value == 0);  // E00 00 is the terminator itself
        now = static_cast<std::uint8_t>(now | (real ? 1u << c : 0u));
      }
      if (s.note == model::kNoteEmpty) {
        for (unsigned c = 0; c < 2u; ++c) {
          if (((eng_[t] >> c) & 1u) != 0u && ((now >> c) & 1u) == 0u && s.fx[c].type == model::kFxNone) {
            s.fx[c].type = fx::kEngineBase;
            s.fx[c].value = 0;
          }
        }
      }
      eng_[t] = now;
    }
    const bool note = s.note >= model::kNoteFirst && s.note <= model::kNoteLast;
    if (((tracks >> t) & 1u) == 0 || !note || s.instr != 0 || s.vol != model::kVolumeUnset || vol_[t] < 0) {
      continue;
    }
    // the volume the instrument gives anyway is not written: only one that a command moved away from it
    const InstFact* f = fact(inst_[t]);
    if (f != nullptr && f->svol != 0xFF && vol_[t] == static_cast<std::int8_t>(std::min<std::uint8_t>(f->svol, kFull))) {
      continue;
    }
    s.vol = static_cast<std::uint8_t>(std::min<int>(2 * vol_[t], model::kVolumeMax));
    ++rep_->volume_baked;
  }
}

// The commands of the Amiga engine's window (docs/formats/med.md 4): 128 + command with the data byte verbatim. True: handled.
bool LineMapper::engine(std::uint32_t t, const Cell& c) noexcept {
  if (!opt_.engine_fx) {
    return false;
  }
  bool has = false;
  bool raw = false;  // kept as it came, but nothing says what it does: counted as dropped (an engine may know)
  const std::uint8_t cmd = c.command;
  switch (cmd) {
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08:
    case 0x0A: case 0x0D: case 0x0E: case 0x11: case 0x12: case 0x14: case 0x15: case 0x18: case 0x19: case 0x1A: case 0x1B:
    case 0x1F: case 0x20: case 0x29:
      has = true;
      break;
    case 0x1E:  // a linearised song has played the repeats already
      has = structural_;
      break;
    case 0x0F:
      has = c.data == 0xF1 || c.data == 0xF2 || c.data == 0xF3 || c.data == 0xF8 || c.data == 0xF9 || c.data == 0xFD;
      if (c.data == 0xF4 || c.data == 0xF5 || c.data == 0xF6 || c.data == 0xF7 || c.data == 0xFC) {
        has = true;
        raw = true;
      }
      break;
    case 0x13: case 0x17: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28:
    case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2F: case 0x30:
      has = true;
      raw = true;
      break;
    default:
      break;
  }
  if (!has) {
    return false;
  }
  if (!put(stage_[t], static_cast<std::uint8_t>(fx::kEngineBase + cmd), c.data) || raw) {
    drop(cmd);
  }
  return true;
}

void LineMapper::command_0f(std::uint32_t t, const Cell& c) noexcept {
  model::Step& s = stage_[t];
  const std::uint8_t d = c.data;
  if (d == 0) {  // break to the next block: the flow has done it in a linearised song
    if (structural_ && !put(s, fx::kBreakPattern, 1)) {
      drop(0x0F);
    }
  } else if (d <= 0xF0) {
    tempo_ = d;
    emit_tempo(s, 0x0F, tempo_, spd_, 0);
  } else if (d == 0xF1 || d == 0xF3) {  // play the note twice / three times per line
    if (!put(s, fx::kRoll, d == 0xF1 ? 2 : 3)) {
      drop(0x0F);
    }
  } else if (d == 0xF2) {  // delay the note by half a line (3 ticks at speed 6)
    if (put(s, fx::kMicroMove, 50)) {
      approx(0x0F);
    } else {
      drop(0x0F);
    }
  } else if (d == 0xFD) {  // set the pitch without retriggering: a glide at full speed
    if (put(s, fx::kGlide, 0)) {
      approx(0x0F);
    } else {
      drop(0x0F);
    }
  } else if (d == 0xFE) {  // end of song: the flow ends it
    if (structural_) {
      drop(0x0F);
    }
  } else if (d == 0xFF) {  // note off
    if (s.note == model::kNoteEmpty) {
      s.note = model::kNoteCut;
    } else {
      drop(0x0F);
    }
  } else {  // F8 F9 (filter), FA FB (MIDI pedal), F4..F7 and FC (not described anywhere readable)
    drop(0x0F);
  }
}

// OS-COREFX (docs/specs/OS-COREFX.md, ADR 0004 amendment 2): the commands of ProTracker and Scream Tracker that the Modern engine has a tracker FX for. The units are the trackers': an arpeggio
// and a volume slide keep their xy nibbles; a fine volume slide is x or y of the tracker's 0..64 (1A up, 1B down, the nibble in the FX's x or y); a fine slide (11 / 12, Paula period units) is
// four quarter units each, and a cell that carries `ext` (the S3M extra fine slide, which is a quarter step) says its quarter units itself. A volume the mapper follows (vol_) follows these too,
// so that a bare note after them still gets the volume the track ended at (a volume slide: x or y a tick over the speed's other ticks).
bool LineMapper::core_command(std::uint32_t t, const Cell& c) noexcept {
  model::Step& s = stage_[t];
  const std::uint8_t cmd = c.command;
  const std::uint8_t d = c.data;
  std::uint8_t type = 0;
  std::uint8_t value = d;
  switch (cmd) {
    case 0x00:
      type = fx::kTrkArp;
      break;
    case 0x0A:
      type = fx::kTrkVolSlide;
      if (vol_[t] >= 0) {
        const int step = (d >> 4) != 0 ? static_cast<int>(d >> 4) : -static_cast<int>(d & 15u);
        vol_[t] = static_cast<std::int8_t>(std::min<int>(kFull, std::max<int>(0, vol_[t] + step * static_cast<int>(spd_ > 1 ? spd_ - 1 : 0))));
      }
      break;
    case 0x1A:
    case 0x1B: {
      const bool has_note = s.note >= model::kNoteFirst && s.note <= model::kNoteLast;
      if (d > 15 || (has_note && vol_[t] >= 0)) {
        return false;  // out of the nibble, or a note whose volume the step bakes exactly (the path below)
      }
      type = fx::kTrkFineVol;
      value = cmd == 0x1A ? static_cast<std::uint8_t>(d << 4) : d;
      if (vol_[t] >= 0) {
        vol_[t] = static_cast<std::int8_t>(cmd == 0x1A ? std::min<int>(vol_[t] + d, kFull) : std::max<int>(vol_[t] - d, 0));
      }
      break;
    }
    case 0x11:
    case 0x12: {
      type = cmd == 0x11 ? fx::kTrkFinePortaUp : fx::kTrkFinePortaDown;
      const std::uint32_t q = c.ext != 0 ? c.ext : 4u * d;
      value = static_cast<std::uint8_t>(std::min<std::uint32_t>(q, 255));
      if (q > 255) {
        approx(cmd);
      }
      break;
    }
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B:  // CF2, CF3: the tracker effects with no MED command (map.h); 0x46 is Channel pan, 0..100 (pan 0 is an effect)
      type = static_cast<std::uint8_t>(fx::kTrkPanSlide + (cmd - fx::kCoreCmdFirst));
      if (type == fx::kTrkChanPan) {
        value = std::min<std::uint8_t>(d, 100);
      }
      if (type == fx::kTrkChanVol || type == fx::kTrkGlobalVol) {
        value = std::min<std::uint8_t>(d, 64);  // (the importer sends 0..64: libopenmpt ignores a larger M)
      }
      if (((type == fx::kTrkPanSlide || type == fx::kTrkChanVolSlide || type == fx::kTrkGlobalVolSlide) && d == 0) || (type == fx::kTrkRetrig && (d & 15u) == 0)) {
        return true;  // P00, N00, W00 (the importer has resolved the memory), Qx0: no effect. A tremor of 0, a glissando of 0, a waveform of 0, a volume of 0 and Y00 (the panbrello goes on with the track's speed and depth) are effects.
      }
      if (!put(s, type, value)) {
        drop(cmd);
      }
      return true;
    default:
      return false;
  }
  if (value == 0) {
    return true;  // E10, EA0, D00, an arpeggio of 0: no effect
  }
  if (!put(s, type, value)) {
    drop(cmd);
  }
  return true;
}

void LineMapper::command(std::uint32_t t, const Cell& c) noexcept {
  model::Step& s = stage_[t];
  if (engine(t, c)) {
    return;
  }
  if ((opt_.core_fx || c.command == 0x46) && core_command(t, c)) {  // (0x46, Channel pan, is written on every engine: it moves the strip, which the Amiga voices do not)
    return;
  }
  const std::uint8_t cmd = c.command;
  const std::uint8_t d = c.data;
  const bool has_note = s.note >= model::kNoteFirst && s.note <= model::kNoteLast;
  switch (cmd) {
    case 0x01:
    case 0x02:
      if (put(s, cmd == 0x01 ? fx::kSlideUp : fx::kSlideDown, d)) {
        approx(cmd);  // the unit of the OG's slide is not confirmed (PAR-079)
      } else {
        drop(cmd);
      }
      break;
    case 0x03:
      if (d != 0 && put(s, fx::kGlide, glide_for(d, tick_us()))) {
        approx(cmd);
      } else {
        drop(cmd);  // 03 00 continues the last slide: there is no number to write
      }
      break;
    case 0x04:
    case 0x14:  // the LFO FX has a rate preset per step; depth lives in the instrument
      if (put(s, fx::kFinetuneLfo, lfo_rate_for(d >> 4, 28))) {
        approx(cmd);
      } else {
        drop(cmd);
      }
      break;
    case 0x07:
      if (put(s, fx::kVolumeLfo, lfo_rate_for(d >> 4, 23))) {
        approx(cmd);
      } else {
        drop(cmd);
      }
      break;
    case 0x09:
      if (d >= 1 && d <= 0x20) {  // Tempo first (timing matters more than tick resolution), then Speed
        spd_ = d;
        emit_tempo(s, cmd, tempo_, spd_, 1);
        if (put(s, fx::kSpeed, d)) {
          ++rep_->speed_fx;
        } else {
          drop(cmd);
        }
      } else {
        drop(cmd);  // the flow ignored it too
      }
      break;
    case 0x0B:  // jump: the flow has done it in a linearised song; the structural fallback writes it as Order jump
    case 0x16:
    case 0x1D:
    case 0x1E:
      if (structural_) {
        if (cmd == 0x1D) {
          if (put(s, fx::kBreakPattern, 1)) {
            if (d != 0) {
              approx(cmd);  // the target line is dropped
            }
          } else {
            drop(cmd);
          }
        } else if (cmd == 0x0B && d <= 254 && put(s, fx::kOrderJump, d)) {
          approx(cmd);  // the model's order counts the pieces the fallback cut, not the entries of the play sequence
        } else {
          drop(cmd);
        }
      }
      break;
    case 0x0C: {
      const VolumeResult v = map_volume(*song_, d);
      rep_->volume_clamped += v.clamped ? 1u : 0u;
      set_volume(s, v.value);
      vol_[t] = static_cast<std::int8_t>(volume_to_med(v.value));
      break;
    }
    case 0x0F:
      command_0f(t, c);
      break;
    case 0x15: {
      const int ft = std::min(7, std::max(-8, static_cast<int>(static_cast<std::int8_t>(d))));  // F8..FF = -8..-1
      const int v = 99 + (ft * 99 + (ft >= 0 ? 4 : -4)) / 8;                                     // 12.375 units per step
      if (put(s, fx::kMicroTune, static_cast<std::uint8_t>(v))) {
        approx(cmd);
      } else {
        drop(cmd);
      }
      break;
    }
    case 0x18: {
      bool exact = false;
      const std::uint8_t v = gate_for(spd_, d, exact);
      if (put(s, fx::kGate, v)) {
        if (!exact) {
          approx(cmd);
        }
      } else {
        drop(cmd);
      }
      break;
    }
    case 0x19: {
      const InstFact* f = fact(s.instr != 0 ? s.instr : current_inst(t));
      if (f == nullptr || f->frames == 0) {
        ++rep_->state_unknown;
        drop(cmd);
        break;
      }
      const std::uint64_t off = static_cast<std::uint64_t>(d) * 256u;
      const std::uint32_t v = std::min<std::uint32_t>(div_round(100ull * off, f->frames), 100);
      if (put(s, fx::kPosition, static_cast<std::uint8_t>(v))) {
        if ((100ull * off) % f->frames != 0) {
          approx(cmd);
        }
      } else {
        drop(cmd);
      }
      break;
    }
    case 0x1A:
    case 0x1B: {
      if (vol_[t] < 0) {
        ++rep_->state_unknown;
        drop(cmd);
        break;
      }
      const int cur = vol_[t];
      const int nv = cmd == 0x1A ? std::min<int>(cur + d, kFull) : std::max<int>(cur - d, 0);
      vol_[t] = static_cast<std::int8_t>(nv);
      set_volume(s, static_cast<std::uint8_t>(std::min<int>(2 * nv, model::kVolumeMax)));
      break;
    }
    case 0x1F: {
      if (!has_note) {
        break;  // nothing happens without a note
      }
      const std::uint32_t x = d >> 4, y = d & 15u;
      bool exact_r = true, exact_m = true;
      const std::uint8_t rv = roll_for(spd_, y, exact_r);
      const std::uint8_t mv = x != 0 ? micro_move_for(spd_, x, exact_m) : std::uint8_t{0};
      bool ok = true;
      if (rv != 0) {
        ok = put(s, fx::kRoll, rv) && ok;
      }
      if (x != 0) {
        ok = put(s, fx::kMicroMove, mv) && ok;
      }
      if (!ok) {
        drop(cmd);
      } else if (!exact_r || !exact_m) {
        approx(cmd);
      }
      break;
    }
    case 0x20:
      if (d == 0) {
        if (has_note && !put(s, fx::kReverse, 1)) {
          drop(cmd);
        }
      } else {
        drop(cmd);  // skip samples (with an extension byte): not in any model
      }
      break;
    case 0x29: {
      if (c.ext == 0) {
        drop(cmd);
        break;
      }
      const std::uint32_t v = std::min<std::uint32_t>(div_round(100ull * d, c.ext), 100);
      if (put(s, fx::kPosition, static_cast<std::uint8_t>(v))) {
        approx(cmd);
      } else {
        drop(cmd);
      }
      break;
    }
    case 0x2E: {
      const int v = std::min(16, std::max(-16, static_cast<int>(static_cast<std::int8_t>(d))));
      if (put(s, fx::kPanning, static_cast<std::uint8_t>(50 + div16_round(v * 50)))) {
        approx(cmd);
      } else {
        drop(cmd);
      }
      break;
    }
    default:
      if (cmd >= 0x31 && cmd <= 0x35) {  // MIDI controllers 1..5 as CC A..E, for a MIDI instrument only
        const std::uint32_t inst = s.instr != 0 ? s.instr : current_inst(t);
        const InstFact* f = fact(inst);
        if (f == nullptr) {
          ++rep_->state_unknown;
          drop(cmd);
        } else if (f->midi && put(s, static_cast<std::uint8_t>(fx::kMidiCcA + (cmd - 0x31)), std::min<std::uint8_t>(d, 127))) {
          approx(cmd);
        } else {
          drop(cmd);
        }
      } else {
        // 00 (arpeggio: the Arp presets are not documented), 05 06 (slides with a volume slide), 08 (hold and decay: the
        // instrument's envelope), 0A 0D (volume slides), 0E, 10 1C (MIDI: slot 37 changed its meaning), 11 12, and every
        // command no readable source describes
        drop(cmd);
      }
      break;
  }
}

// ---- instruments ------------------------------------------------------------------------------------------------------

namespace {

std::uint32_t round_div64(std::uint64_t n, std::uint64_t d) noexcept { return div_round(n, d); }

}  // namespace

bool map_instrument(const Reader& reader, std::uint32_t slot, const InstrumentInfo& info, Clock clock, std::uint64_t pcm_left,
                    model::Instrument& out, InstReport& rep) noexcept {
  rep = InstReport();
  rep.reader_notes = info.notes;
  const SongInfo& song = reader.song();
  const std::uint8_t svol = std::min<std::uint8_t>(info.volume, kFull);

  if (info.midi_channel != 0) {
    model::init_instrument(out, model::InstrumentKind::Midi);
    std::strncpy(out.name, info.name, model::kInstrumentNameMax);
    out.name[model::kInstrumentNameMax] = '\0';
    if (info.midi_channel > 16) {
      out.midi_channel = 16;
      rep.flags |= kInstChannelClamped;
    } else {
      out.midi_channel = info.midi_channel;
    }
    out.sampler.volume = percent_of_64(svol);  // the velocity
    const bool has_sample = (info.kind == InstKind::Sample && info.frames != 0) || info.kind == InstKind::Synth ||
                            info.kind == InstKind::Hybrid;
    if (info.midi_preset != 0 || has_sample) {
      rep.flags |= kInstMidiDropped;
    }
    rep.result = InstResult::Midi;
    return true;
  }

  switch (info.kind) {
    case InstKind::Empty:
      rep.result = InstResult::None;
      return false;
    case InstKind::Unsupported:
      rep.result = InstResult::Unsupported;
      return false;
    default:
      break;
  }

  // The PCM: the longest octave of a sample, the sample of a hybrid, the first waveform of a synth instrument.
  const bool synth = info.kind == InstKind::Synth;
  const bool hybrid = info.kind == InstKind::Hybrid;
  std::uint32_t frames = 0;
  if (synth) {
    rep.flags |= kInstSynth;
    for (std::uint32_t w = 0; w < info.waveforms && frames == 0; ++w) {
      std::uint32_t header = 0, data = 0, bytes = 0;
      if (reader.synth_waveform(info, w, header, data, bytes).ok() && data != 0 && bytes >= 2) {
        rep.src = data;
        frames = bytes;
      }
    }
  } else {
    if (hybrid) {
      rep.flags |= kInstHybrid;
    }
    const unsigned octaves = info.octaves < 1 ? 1u : info.octaves;
    const std::uint64_t base = info.base_frames;
    const std::uint64_t bpf = info.sixteen_bit ? 2u : 1u;
    if (info.data != 0 && info.frames != 0 && base != 0) {
      frames = static_cast<std::uint32_t>(base << (octaves - 1));
      const std::uint64_t skip = base * ((1ull << (octaves - 1)) - 1ull) * bpf;
      rep.src = info.data + static_cast<std::uint32_t>(skip);
      if (info.stereo) {
        rep.src_right = info.data + info.data_bytes + static_cast<std::uint32_t>(skip);
        rep.flags |= kInstStereoMix;
      }
      rep.src_bits = info.sixteen_bit ? 16 : 8;
    }
    if (octaves > 1) {
      rep.flags |= kInstOctave;
    }
    if (info.ext_sample) {
      rep.flags |= kInstExtSample;
    }
    if (info.sixteen_bit) {
      rep.flags |= kInstSixteenBit;
    }
  }
  if (frames == 0) {
    rep.result = synth || hybrid ? InstResult::NoWave : InstResult::EmptyData;
    return false;
  }
  const std::uint64_t pcm = static_cast<std::uint64_t>(frames) * 2u;
  if (pcm > caps::kSampleArenaBytes) {
    rep.result = InstResult::Oversize;
    return false;
  }
  if (pcm > pcm_left) {
    rep.result = InstResult::ArenaFull;
    return false;
  }
  rep.frames = frames;
  rep.result = InstResult::Sample;

  model::init_instrument(out, model::InstrumentKind::Sample);
  std::strncpy(out.name, info.name, model::kInstrumentNameMax);
  out.name[model::kInstrumentNameMax] = '\0';
  const unsigned number = slot + 1u;
  // the name the OG import gives the sample of an instrument (og.h, wav_sample.cpp): the same file the WAV importer makes for the slot
  const char path[] = {'s', 'a', 'm', 'p', 'l', 'e', 's', '/', 'i', 'n', 's', 't', 'r', static_cast<char>('0' + number / 10u),
                       static_cast<char>('0' + number % 10u), '.', 'w', 'a', 'v', '\0'};
  std::memcpy(out.sample_path, path, sizeof path);
  out.sample_rate = paula_rate(clock);
  model::SamplerParams& sp = out.sampler;
  sp.volume = info.disabled ? std::uint8_t{0} : volume_level_of_tracker(svol);  // OS-BUG-PASTE24: 64 is level 50 (0 dB), the lower volumes at their ratio in dB
  if (info.disabled) {
    rep.flags |= kInstDisabled;
  }

  // Pitch: strans, and the offset that puts this kind's unity note on the model's (see the top of map.h).
  int tune = static_cast<int>(info.transpose);
  if (synth || hybrid || info.ext_sample) {
    tune -= 24;
  } else if (info.octaves >= 5) {
    tune += 12;  // types 1, 5 and 6 put the longest octave's unity at C-1, the others at C-2
  }
  if (tune > pitch::kTuneLimit || tune < -pitch::kTuneLimit) {
    tune = std::min(pitch::kTuneLimit, std::max(-pitch::kTuneLimit, tune));
    rep.flags |= kInstTuneClamped;
  }
  sp.tune = static_cast<std::int8_t>(tune);
  rep.tune = sp.tune;
  const int ft = std::min(7, std::max(-8, static_cast<int>(info.finetune)));
  sp.fine_tune = static_cast<std::int8_t>((ft * 25 + (ft >= 0 ? 1 : -1)) / 2);  // 12.5 cents per step, to nearest
  rep.fine_tune = sp.fine_tune;
  if (info.hold != 0 || info.decay != 0) {
    rep.flags |= kInstHoldDecay;
  }
  if (info.default_pitch != 0) {
    rep.flags |= kInstDefaultPitch;
  }

  // The loop. libxmp loops an MMD0/1 sample when replen > 1 whatever InstrExt says and an MMD2+ sample by the InstrExt flag alone;
  // OpenMPT wants both, the flag once the InstrExt entry reaches it and replen > 1 (libopenmpt 0.7.3 measured, every version): the
  // importer follows OpenMPT and says where the two differ (replen is in units of 2 frames, so replen <= 1 is a loop of at most 2
  // frames; the V7 long loop has no such sentinel).
  bool loop = info.has_loop;
  std::uint32_t lstart = info.loop_start, llen = info.loop_len;
  std::uint32_t base = info.base_frames;
  if (synth) {
    loop = true;
    lstart = 0;
    llen = frames;
    base = frames;
  } else if (loop) {
    const bool flag_clear = song.version <= 1 && info.ext_bytes >= 6 && (info.ext_flags & 1u) == 0;
    const bool sentinel = (info.notes & (kNoteLongLoop | kNoteLoopClamped)) == 0 && info.loop_len <= 2;
    if (flag_clear || sentinel) {
      loop = false;
      rep.flags |= kInstLoopRule;
    }
  }
  if (loop && base != 0) {
    const std::uint64_t e = static_cast<std::uint64_t>(lstart) + llen;
    const std::uint32_t ps = round_div64(65535ull * lstart, base);
    const std::uint32_t pe = round_div64(65535ull * e, base);
    // is the loop recoverable to the frame from the 16-bit positions, in the frames of the PCM that is written?
    const std::uint64_t scale = frames / base;
    const std::uint64_t fs = lstart * scale, fe = e * scale;
    const bool exact = round_div64(static_cast<std::uint64_t>(ps) * frames, 65535) == fs &&
                       round_div64(static_cast<std::uint64_t>(pe) * frames, 65535) == fe;
    if (pe <= ps) {
      rep.flags |= kInstLoopQuantised;  // the positions cannot tell the ends apart: no loop
    } else {
      sp.loop_start = static_cast<std::uint16_t>(ps);
      sp.loop_end = static_cast<std::uint16_t>(pe);
      sp.play_mode = info.ping_pong && !synth ? 3 : 1;
      if (!exact) {
        rep.flags |= kInstLoopQuantised;
      }
    }
  }
  return true;
}

}  // namespace o3os::formats::med
