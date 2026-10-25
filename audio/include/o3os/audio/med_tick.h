// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The MED tick processor (OS-008c, docs/formats/med.md section 5 slice M3): what one track of an OctaMED song does in one tick, as a pure function of the track's state.
// It owns no voice and touches no audio: audio/amiga_voices.h feeds it the sequencer's events (the note, the two command columns, the Tick event) and applies what it
// answers (a Paula period, a volume 0..64, a restart of the sample, a cut) to a Paula channel. It lives in the audio layer because it consumes audio events and the
// audio layer may not include the sequencer or the formats.
//
// COMMANDS. A command of a column is a MED command 00..3F and its data byte: the engine FX window of the step (ADR 0004 amendment 1: FX type 128 + command, value = the
// data byte; seq/fx.h). It HOLDS until the column's FX changes or ends (that is how the control layers carry an FX: seq/control.h): it runs on every row, as a MED
// command repeated on every row does. The importer writes `E00 00` ("no effect") on the row after a command's last row, and a note step ends both columns' FX by
// itself, so a MED song's one-row commands stay one row (docs/formats/med.md "as built", M3).
//   00 xy arpeggio (base, +x, +y semitones a tick) | 01 / 02 xx slide up / down a tick | 03 xx tone portamento (03 00 continues) | 04 xy vibrato (speed x, depth y, twice
//   as deep as 14) | 14 xy vibrato at ProTracker depth | 05 xy tone portamento + volume slide | 06 xy vibrato + volume slide | 07 xy tremolo | 08 xy hold (y ticks) and
//   decay (x a tick), beside a note | 0A / 0D xy volume slide (x up, y down) | 11 / 12 xx pitch slide once | 15 xx finetune -8..7 | 18 xx cut after xx ticks |
//   19 xx sample offset xx x 256 | 1A / 1B xx volume up / down once | 1F xy delay x ticks and retrigger every y, beside a note | 0F F1 / F3 retrigger (every 3, once /
//   every 2 ticks) | 0F F2 delay 3 ticks | 0F F8 / F9 LED filter off / on | 0F FD set the pitch without retriggering. Anything else does nothing (kept, counted nowhere:
//   docs/formats/med.md O4).
// What follows libxmp (MIT; the golden frame streams of tests/data/med_tick_golden.txt were made with it, see THIRD_PARTY.md): slides, the tone portamento and
// the volume slide run on EVERY tick, tick 0 included, as OctaMED does unless the song has the SoundTracker-slide flag (Cfg::slide_tick0 false: ticks 1.. only); the
// vibrato and tremolo tables are ProTracker's (64 steps of the sine, the phase moves from tick 1, one step late); a note under 03 / 05 sets the portamento's target and does not
// restart; the period stays in Paula's range (113..65535: libxmp keeps none for MED, a slide past 113 is not playable on an Amiga); the hold counter of a retriggered note grows by the tick of the retrigger (OctaMED
// 3.00 and later). What differs: the period is an integer of the ProTracker table (a real Amiga's), libxmp's is a float (the difference is under one unit of period);
// "hold symbols" (an instrument number on the next row sustaining the hold) are not modelled: they need the next row.
//
// Real time: integer only, no heap, bounded. Called once per track per tick (50 to 300 a second), from the render context: cold code in flash (O3OS_AUDIO_COLD).

#include <o3os/audio/hot.h>

#include <cstdint>

namespace o3os::audio::med {

struct Cfg {
  bool slide_tick0 = true;  // false: the song has the SoundTracker slide flag (FLAG_STSLIDE): slides, portamento and volume slides run on ticks 1.. only
};

// A note the track starts on this row. `note` is the model's note 0..127 (the table's first note is 48), `volume` 0..64 (the instrument's, or the step's volume column).
struct Note {
  int note = 60;
  int finetune = 0;           // -8..7
  std::uint8_t volume = 64;
  std::uint8_t hold = 0;      // the instrument's hold ticks and decay (0: none); an 08 command beside the note replaces them
  std::uint8_t decay = 0;
};

struct Out {
  std::uint16_t period = 0;   // the Paula period to play at: 113..65535 (0: the track has no pitch yet)
  std::uint8_t volume = 0;    // 0..64
  bool start = false;         // (re)start the sample from `offset` (a note, a retrigger)
  bool cut = false;           // silence the voice (a cut)
  std::int8_t led = -1;       // the LED filter: -1 unchanged, 0 off, 1 on
  std::uint32_t offset = 0;   // frames, with `start`
};

class Track {
 public:
  Track() noexcept = default;
  void reset() noexcept { *this = Track(); }

  // The track's command column `col` (0 or 1) now holds MED command `cmd` with `data`; or holds none.
  void set_cmd(unsigned col, std::uint8_t cmd, std::uint8_t data) noexcept {
    if (col < 2u) {
      cmd_[col] = cmd;
      data_[col] = data;
      has_[col] = true;
    }
  }
  void end_cmd(unsigned col) noexcept {
    if (col < 2u) {
      has_[col] = false;
    }
  }
  // A note starts on this track at the next tick(0).
  void note_on(const Note& n) noexcept {
    note_ = n;
    note_pending_ = true;
  }

  // One tick: `t` 0..ticks-1 within the row, `ticks` ticks a row (1..32). Tick 0 takes the pending note and the row's commands. Fills `out`.
  O3OS_AUDIO_COLD void tick(unsigned t, unsigned ticks, const Cfg& cfg, Out& out) noexcept;

  std::uint16_t period() const noexcept { return period_; }
  std::uint8_t volume() const noexcept { return static_cast<std::uint8_t>(vol_); }

 private:
  O3OS_AUDIO_COLD void begin_row(unsigned t, unsigned ticks, Out& out) noexcept;
  O3OS_AUDIO_COLD void apply_note(bool porta, bool set_pitch, Out& out) noexcept;
  O3OS_AUDIO_COLD void arm_retrig(unsigned every, bool once) noexcept;

  // the persistent columns
  std::uint8_t cmd_[2] = {0, 0};
  std::uint8_t data_[2] = {0, 0};
  bool has_[2] = {false, false};
  // the note
  Note note_{};
  bool note_pending_ = false;
  unsigned defer_ = 0;           // the row (its note and commands) is taken at this tick: a delayed note; 0: none
  std::int16_t cur_note_ = -1;   // the note the period came from (arpeggio steps from it)
  std::int8_t finetune_ = 0;
  // what plays
  std::uint16_t period_ = 0;
  std::uint16_t target_ = 0;
  std::int16_t vol_ = 0;
  // flags of the row, set when it is taken
  std::int16_t slide_ = 0;        // period change a tick (01 negative, 02 positive), 0: none
  std::uint16_t porta_ = 0;       // tone portamento step, 0: none
  std::int8_t porta_dir_ = 0;     // +1: the period rises to the target, -1 falls, 0: arrived
  std::int8_t vslide_ = 0;        // volume change a tick
  bool vib_ = false;
  bool trem_ = false;
  bool arp_ = false;
  std::uint8_t arp_x_ = 0, arp_y_ = 0, arp_i_ = 0;
  std::int16_t fine_vol_ = 0;     // once, at tick 0
  std::int16_t fine_per_ = 0;
  std::int16_t cut_at_ = -1;      // 18 xx: the tick the note is cut at
  bool rt_on_ = false;
  bool rt_once_ = false;          // retrigger a single time (0F F1)
  std::int16_t rt_val_ = 0;
  std::int16_t rt_count_ = 0;
  // memories and the oscillators
  std::uint8_t porta_mem_ = 0;
  std::uint8_t vib_speed_ = 0, vib_depth_ = 0, vib_pos_ = 0;
  std::uint8_t trem_speed_ = 0, trem_depth_ = 0, trem_pos_ = 0;
  std::uint8_t vol_mem_ = 0;
  std::uint8_t off_mem_ = 0;
  // hold and decay: hold ticks left (-1: none), decay a tick (0: all at once)
  std::int16_t hold_ = -1;
  std::uint8_t decay_ = 0;
  bool hold_active_ = false;
};

}  // namespace o3os::audio::med
