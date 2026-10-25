// SPDX-License-Identifier: MIT
//
// SMF EXPORT (OS-H4, new Community OS behaviour): the Standard MIDI File of a pattern, with no OS state and no libc, so that the module and its host test share it.
//
//   FILE     type 1, PPQ 96, one pattern row = 24 ticks (a 16th note). MTrk 0 holds the tempo and the 4/4 time signature; then one MTrk per track that has a note, in track order, named "Track N" (N from 1).
//   NOTES    a step with a note is a note-on at row * 24 + the Micro-move offset; the note ends (a note-off at the tick of the next event of that track) at the next note, at an Off, Cut or Fade step, or
//            at the end of the range. The volume column is the velocity as the sequencer takes it (min(vol, 127), 0 becomes 1); a step with none uses the instrument's own default (Res::default_velocity).
//   CHANNEL  Res::channel (a MIDI instrument's own CHANNEL, else the track's number mod 16, plus 1).
//   MICRO    FX 2, as the sequencer plays it: o3os::seq::micromove_start(value) (seq/include/o3os/seq/fx.h:264) is in 48ths of a step, 1 .. 47, so the offset in ticks is micromove_start(value) / 2, 0 .. 23.
//            The app cannot include the core, so the formula is copied below; tests/module_edit_test.cpp checks the copy against the core's over every value. Both FX columns are looked at; the sequencer
//            reads the second column first and the first last, so the first column's Micro-move wins.
//   NOT IN V1  no other FX is exported (Chord FX 14 is not expanded): each one found in the range is counted by number (Counts::skipped) for the page.
//
// The writer has no seek, so each MTrk is built whole in a buffer before its first byte is written: its length is known when its header is written.
#pragma once

#include <stdint.h>

namespace smfexport {

constexpr uint32_t kPpq = 96;
constexpr uint32_t kTicksPerRow = 24;
constexpr uint32_t kFxMicroMove = 2;
constexpr uint32_t kFxChord = 14;
constexpr uint32_t kMaxPatRows = 128;
constexpr uint32_t kMaxTracks = 16;
constexpr uint32_t kHeaderBytes = 14;      // "MThd" and its six bytes
constexpr uint32_t kTempoTrackBytes = 8 + 7 + 8 + 4;  // MTrk header, tempo meta, time signature meta, end of track
// The largest MTrk of 128 rows: a name meta (1 + 3 + 8), per row a note-off and a note-on of 2 + 3 bytes each, the end of track (4), and the 8-byte header. Rounded up.
constexpr uint32_t kTrackBytesMax = 8 + 12 + kMaxPatRows * 10 + 4 + 8;
// The most work one call of the app does (OS-FINALQA FQA-10b1): a call into an app has 4 ms (O3M_VIEW_MAX_US), and the app's code and its state are in SDRAM, uncached. A "unit" is one of: scan one
// track, open the file, take kRowsPerSlice rows of a track, write one chunk of at most kChunk bytes, close. A frame runs units while the host's millisecond clock has not moved, and at most kUnitsPerFrame.
constexpr uint32_t kChunk = 1024;         // the most one export_write call carries
constexpr uint32_t kRowsPerSlice = 32;
constexpr uint32_t kUnitsPerFrame = 8;
constexpr uint32_t kFxSlots = 44;          // skipped-FX counters: index 1..42 the Tracker's FX by number, 43 an engine's own (128 and above)

// Micro-move 0..100 to microsteps 1 .. 47 (the core's micromove_start, copied: fx.h:264).
constexpr uint8_t micromove_start(uint8_t value) {
  const uint32_t v = value > 100u ? 100u : value;
  const uint32_t start = (v + 1u) * 48u / 101u + 1u;
  return static_cast<uint8_t>(start > 47u ? 47u : start);
}
constexpr uint32_t micromove_ticks(uint8_t value) { return micromove_start(value) / 2u; }

// The tempo meta's microseconds per quarter note for a tempo in BPM x 100 (4000 .. 80000): 6e9 / bpm_x100 in 32-bit arithmetic (6e9 does not fit): 60,000,000 / x * 100 plus the rest, scaled.
constexpr uint32_t tempo_us(uint32_t bpm_x100) {
  const uint32_t x = bpm_x100 != 0u ? bpm_x100 : 12000u;
  return (60000000u / x) * 100u + (60000000u % x) * 100u / x;
}

struct Counts {
  uint32_t notes;                // note-on events written
  uint32_t skipped[kFxSlots];    // FX found and not exported, by number
};

// A byte buffer that cannot overflow: a byte past its end sets `over` and is dropped.
struct Buf {
  uint8_t* p;
  uint32_t cap;
  uint32_t n;
  bool over;
  void put(uint32_t b) {
    if (n < cap) {
      p[n++] = static_cast<uint8_t>(b);
    } else {
      over = true;
    }
  }
  void put32(uint32_t v) {
    put(v >> 24);
    put(v >> 16);
    put(v >> 8);
    put(v);
  }
  void vlq(uint32_t v) {
    uint8_t tmp[5];
    uint32_t k = 0;
    tmp[k++] = static_cast<uint8_t>(v & 0x7Fu);
    for (v >>= 7; v != 0u && k < 5u; v >>= 7) {
      tmp[k++] = static_cast<uint8_t>((v & 0x7Fu) | 0x80u);
    }
    while (k != 0u) {
      put(tmp[--k]);
    }
  }
  void text(const char* s) {
    while (*s != '\0') {
      put(static_cast<uint8_t>(*s++));
    }
  }
};

// "MThd": format 1, `tracks` MTrk chunks, 96 ticks per quarter note. Returns the bytes (kHeaderBytes).
inline uint32_t build_header(uint8_t* out, uint32_t cap, uint32_t tracks) {
  Buf b{out, cap, 0, false};
  b.text("MThd");
  b.put32(6);
  b.put(0);
  b.put(1);
  b.put(tracks >> 8);
  b.put(tracks);
  b.put(kPpq >> 8);
  b.put(kPpq);
  return b.over ? 0u : b.n;
}

// MTrk 0: the tempo and 4/4. Returns the bytes (kTempoTrackBytes), 0 if `cap` is too small.
inline uint32_t build_tempo_track(uint8_t* out, uint32_t cap, uint32_t bpm_x100) {
  Buf b{out, cap, 0, false};
  b.text("MTrk");
  b.put32(7 + 8 + 4);
  const uint32_t us = tempo_us(bpm_x100);
  b.put(0);  // delta
  b.put(0xFF);
  b.put(0x51);
  b.put(3);
  b.put(us >> 16);
  b.put(us >> 8);
  b.put(us);
  b.put(0);
  b.put(0xFF);
  b.put(0x58);
  b.put(4);
  b.put(4);     // 4/4
  b.put(2);     // a quarter note is the beat (2^2)
  b.put(24);    // MIDI clocks per metronome click
  b.put(8);     // 32nd notes in a quarter note
  b.put(0);
  b.put(0xFF);
  b.put(0x2F);
  b.put(0);
  return b.over ? 0u : b.n;
}

// Whether `steps[0..rows)` holds a note (a step with a note-on to write).
template <class Step>
bool has_note(const Step* steps, uint32_t rows) {
  for (uint32_t j = 0; j < rows; ++j) {
    if (steps[j].note >= 1u && steps[j].note <= 128u) {
      return true;
    }
  }
  return false;
}

inline void count_skipped(Counts* c, uint32_t type) {
  if (c != nullptr && type != 0u) {
    ++c->skipped[type <= 42u ? type : 43u];
  }
}

// MTrk of one track, built in slices so that no single call of the app has to do a whole track (a track is up to 128 rows, and the app's code and state run from SDRAM, uncached: a whole track
// in one call went over the 4 ms of a view on the unit). `begin` writes the chunk header and the name, `run` takes the next rows of `steps` (any slice sizes give the same bytes), `finish`
// ends the track and patches its length. `track` is 0-based; `Res` supplies
//   uint32_t first_instrument()                 the instrument a step without one starts from (0..64, 0 = none);
//   uint32_t channel(track, instrument)         0..15;
//   uint32_t default_velocity(instrument)       1..127, for a step with no volume.
// `counts` (may be null) gets the note-ons and the FX that were not exported.
struct TrackWriter {
  Buf b;
  uint32_t rows;          // the track's rows (<= 128)
  uint32_t done;          // rows taken so far
  uint32_t track;
  uint32_t last;          // the tick of the last event written
  int32_t playing;        // the MIDI note that sounds, or -1
  uint32_t playing_ch;
  uint32_t instrument;
  uint8_t* out;

  void note_off(uint32_t tick) {
    b.vlq(tick - last);
    last = tick;
    b.put(0x80u | playing_ch);
    b.put(static_cast<uint32_t>(playing));
    b.put(0x40);
    playing = -1;
  }

  template <class Res>
  void begin(uint8_t* buf, uint32_t cap, uint32_t row_count, uint32_t track_index, Res& res) {
    out = buf;
    b = Buf{buf, cap, 0, false};
    rows = row_count;
    done = 0;
    track = track_index;
    last = 0;
    playing = -1;
    playing_ch = 0;
    b.text("MTrk");
    b.put32(0);  // the length, patched by finish
    b.put(0);
    b.put(0xFF);
    b.put(0x03);
    char name[12];
    uint32_t k = 0;
    for (const char* s = "Track "; *s != '\0'; ++s) {
      name[k++] = *s;
    }
    const uint32_t number = track + 1u;
    if (number >= 10u) {
      name[k++] = static_cast<char>('0' + number / 10u);
    }
    name[k++] = static_cast<char>('0' + number % 10u);
    b.put(k);
    for (uint32_t i = 0; i < k; ++i) {
      b.put(static_cast<uint8_t>(name[i]));
    }
    instrument = res.first_instrument();
  }

  // The next rows, at most `max_rows` of them (and never past the track's end); returns how many it took.
  template <class Step, class Res>
  uint32_t run(const Step* steps, uint32_t max_rows, Res& res, Counts* counts) {
    const uint32_t end = max_rows < rows - done ? done + max_rows : rows;
    const uint32_t first = done;
    for (uint32_t j = done; j < end; ++j) {
      const Step& s = steps[j];
      uint32_t offset = 0;
      if (s.fx1_type == kFxMicroMove) {
        offset = micromove_ticks(s.fx1_value);
      } else if (s.fx2_type == kFxMicroMove) {
        offset = micromove_ticks(s.fx2_value);
      }
      if (s.fx1_type != kFxMicroMove) {
        count_skipped(counts, s.fx1_type);
      }
      if (s.fx2_type != kFxMicroMove) {
        count_skipped(counts, s.fx2_type);
      }
      const uint32_t tick = j * kTicksPerRow + offset;
      if (s.note >= 1u && s.note <= 128u) {
        if (s.instr >= 1u && s.instr <= 64u) {
          instrument = s.instr;
        }
        if (playing >= 0) {
          note_off(tick);
        }
        uint32_t vel = s.vol != 255u ? (s.vol > 127u ? 127u : s.vol) : res.default_velocity(instrument);
        vel = vel < 1u ? 1u : (vel > 127u ? 127u : vel);
        const uint32_t ch = res.channel(track, instrument) & 15u;
        b.vlq(tick - last);
        last = tick;
        b.put(0x90u | ch);
        b.put(s.note - 1u);
        b.put(vel);
        playing = static_cast<int32_t>(s.note - 1u);
        playing_ch = ch;
        if (counts != nullptr) {
          ++counts->notes;
        }
      } else if ((s.note == 253u || s.note == 254u || s.note == 255u) && playing >= 0) {
        note_off(tick);  // fade, cut and off all end the note here (v1 does not tell them apart)
      }
    }
    done = end;
    return end - first;
  }

  bool complete() const { return done >= rows; }

  // After the last row: ends the note that still sounds, the end of track, the length. Returns the bytes (the chunk's header included), 0 when the buffer was too small.
  uint32_t finish() {
    if (playing >= 0) {
      note_off(rows * kTicksPerRow);
    }
    b.vlq(rows * kTicksPerRow - last);  // every track ends at the end of the range, so that the tracks are as long as the pattern
    b.put(0xFF);
    b.put(0x2F);
    b.put(0);
    if (b.over) {
      return 0;
    }
    const uint32_t body = b.n - 8u;
    out[4] = static_cast<uint8_t>(body >> 24);
    out[5] = static_cast<uint8_t>(body >> 16);
    out[6] = static_cast<uint8_t>(body >> 8);
    out[7] = static_cast<uint8_t>(body);
    return b.n;
  }
};

// The whole track in one call (the host tests; a track of a few rows). Returns the bytes, 0 when `cap` is too small.
template <class Step, class Res>
uint32_t build_track(uint8_t* out, uint32_t cap, const Step* steps, uint32_t rows, uint32_t track, Res& res, Counts* counts) {
  TrackWriter w;
  w.begin(out, cap, rows, track, res);
  w.run(steps, rows, res, counts);
  return w.finish();
}

}  // namespace smfexport
