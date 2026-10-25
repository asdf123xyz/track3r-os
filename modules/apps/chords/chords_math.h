// SPDX-License-Identifier: MIT
//
// Chords (OS-H3B, new Community OS behaviour): the music and the steps it writes, with no OS state and no libc, so that the module and its host test share it.
//   - A progression is a list of scale DEGREES (1..7); the chord on a degree is the scale's diatonic one: triads major / minor / diminished, and with SEVENTHS maj7 / min7 / dom7 / m7b5. The scales are the
//     major scale and the natural minor. So I-V-vi-IV in C major is C, G, Am, F; the same numbers in MINOR are Cm, Gm, Ab, Fm.
//   - The root of a chord is KEY + the degree's semitones, folded into the octave OCTAVE (so a progression never climbs): the step byte 12 x OCTAVE + pitch class + 1 (the Pattern names 49 C-4).
//   - CHORD FX: the root, the instrument and FX 14 Chord at each chord row. Chord 14 takes an index of one of the 30 masks of seq/fx.h kChordMask (docs/specs/OS-PFX.md): kChordIndex below is this app's copy of the
//     index of each chord quality, and the host test checks every one against the OS table. A MIDI instrument sends all the tones of the mask; any other instrument plays the root alone (the sequencer
//     keeps one voice a track, seq/src/sequencer.cpp pfx_note), which is why SPREAD exists.
//   - SPREAD: tone t of the chord on track t of the target, no FX.
//   - The chord rows are 0, RATE, 2 x RATE ... of the target; the progression plays once (chords that do not fit are dropped); the other rows of the target get an empty note.
// Integer only.
#pragma once

#include <stdint.h>

namespace chords {

constexpr uint32_t kFxChord = 14;
constexpr uint32_t kProgCount = 8;
constexpr uint32_t kMaxChords = 12;
constexpr uint32_t kMinOctave = 2;
constexpr uint32_t kMaxOctave = 6;
constexpr uint32_t kDefaultOctave = 4;

enum Quality : uint32_t { kMaj, kMin, kDim, kMaj7, kMin7, kDom7, kM7b5, kQualityCount };

// Semitones above the root of each tone, root first; the third column of kTones is 3 for a triad and 4 for a seventh chord.
constexpr uint8_t kTones[kQualityCount][4] = {{0, 4, 7, 0}, {0, 3, 7, 0}, {0, 3, 6, 0}, {0, 4, 7, 11}, {0, 3, 7, 10}, {0, 4, 7, 10}, {0, 3, 6, 10}};
constexpr uint32_t tone_count(uint32_t quality) { return quality >= kMaj7 ? 4u : 3u; }

// The Chord FX value of each quality: the index in this OS's table of 30 chord masks (seq/fx.h kChordMask; the major triad is index 5, label 47, mask 0x0091, and so on). Every quality has an entry in the table, so SEVENTHS never falls back to the triad.
constexpr uint8_t kChordIndex[kQualityCount] = {5, 4, 3, 24, 20, 23, 17};

constexpr uint8_t kMajorScale[7] = {0, 2, 4, 5, 7, 9, 11};
constexpr uint8_t kMinorScale[7] = {0, 2, 3, 5, 7, 8, 10};
constexpr uint8_t kMajorTriad[7] = {kMaj, kMin, kMin, kMaj, kMaj, kMin, kDim};
constexpr uint8_t kMinorTriad[7] = {kMin, kDim, kMaj, kMin, kMin, kMaj, kMaj};
constexpr uint8_t kMajorSeventh[7] = {kMaj7, kMin7, kMin7, kMaj7, kDom7, kMin7, kM7b5};
constexpr uint8_t kMinorSeventh[7] = {kMin7, kM7b5, kMaj7, kMin7, kMin7, kMaj7, kDom7};

struct Prog {
  uint8_t n;
  uint8_t degree[kMaxChords];
};
constexpr Prog kProgs[kProgCount] = {
    {4, {1, 5, 6, 4}},                              // I-V-vi-IV
    {4, {1, 6, 4, 5}},                              // I-vi-IV-V
    {3, {2, 5, 1}},                                 // ii-V-I
    {4, {1, 4, 5, 4}},                              // I-IV-V-IV
    {4, {6, 4, 1, 5}},                              // vi-IV-I-V
    {4, {1, 6, 3, 7}},                              // i-VI-III-VII
    {4, {1, 4, 5, 1}},                              // i-iv-v-i
    {12, {1, 1, 1, 1, 4, 4, 1, 1, 5, 4, 1, 5}},     // the 12-bar blues
};

struct Setup {
  uint32_t key;       // 0..11, C..B
  uint32_t minor;     // 0 MAJOR, 1 MINOR
  uint32_t prog;      // 0..7
  uint32_t rate;      // rows per chord, 1..
  uint32_t octave;    // 2..6
  uint32_t sevenths;  // 0 / 1
  uint32_t instr;     // 1..64
};

struct Chord {
  uint32_t root;     // the step byte of the root
  uint32_t quality;  // Quality
};

constexpr uint32_t chord_count(const Setup& u) { return kProgs[u.prog % kProgCount].n; }

constexpr Chord chord_of(const Setup& u, uint32_t k) {
  const Prog& p = kProgs[u.prog % kProgCount];
  const uint32_t d = (p.degree[k % p.n] - 1u) % 7u;
  const uint32_t pc = (u.key + (u.minor != 0u ? kMinorScale[d] : kMajorScale[d])) % 12u;
  const uint32_t q = u.sevenths != 0u ? (u.minor != 0u ? kMinorSeventh[d] : kMajorSeventh[d]) : (u.minor != 0u ? kMinorTriad[d] : kMajorTriad[d]);
  return Chord{12u * u.octave + pc + 1u, q};
}

// The default RATE: the target's rows divided by the number of chords, at least 1.
constexpr uint32_t default_rate(const Setup& u, uint32_t rows) {
  const uint32_t r = rows / chord_count(u);
  return r != 0u ? r : 1u;
}

// How many chords fit in `rows` rows at the setup's RATE.
constexpr uint32_t chords_fitting(const Setup& u, uint32_t rows) {
  const uint32_t per = u.rate != 0u ? u.rate : 1u;
  const uint32_t fit = (rows + per - 1u) / per;
  return fit < chord_count(u) ? fit : chord_count(u);
}

// The chord that starts on row j of the target, or -1.
constexpr int32_t chord_at_row(const Setup& u, uint32_t rows, uint32_t j) {
  const uint32_t per = u.rate != 0u ? u.rate : 1u;
  return j % per == 0u && j / per < chords_fitting(u, rows) ? static_cast<int32_t>(j / per) : -1;
}

// CHORD FX: the steps of the target's first track. `cur` as it is, `out` the steps to write with the mask NOTE | INSTR | FX1 | FX2. A chord row gets the root, the instrument and the Chord FX in the
// slot that already holds one, else FX2, else FX1; a row whose two slots hold other FX gets the root without the Chord FX and is counted in *no_slot. Every other row of the target gets an empty note.
// Returns the chords written.
template <class Step>
uint32_t chordfx_steps(const Setup& u, const Step* cur, Step* out, uint32_t n, uint32_t* no_slot) {
  uint32_t written = 0;
  for (uint32_t j = 0; j < n; ++j) {
    out[j] = cur[j];
    const int32_t k = chord_at_row(u, n, j);
    if (k < 0) {
      out[j].note = 0;
      continue;
    }
    const Chord c = chord_of(u, static_cast<uint32_t>(k));
    out[j].note = static_cast<uint8_t>(c.root);
    out[j].instr = static_cast<uint8_t>(u.instr);
    const uint8_t value = kChordIndex[c.quality];
    if (cur[j].fx2_type == kFxChord) {
      out[j].fx2_value = value;
    } else if (cur[j].fx1_type == kFxChord) {
      out[j].fx1_value = value;
    } else if (cur[j].fx2_type == 0u) {
      out[j].fx2_type = kFxChord;
      out[j].fx2_value = value;
    } else if (cur[j].fx1_type == 0u) {
      out[j].fx1_type = kFxChord;
      out[j].fx1_value = value;
    } else if (no_slot != nullptr) {
      ++*no_slot;
    }
    ++written;
  }
  return written;
}

// SPREAD: tone `tone` (0 root, 1 third, 2 fifth, 3 seventh) of each chord on one track; `out` is written with the mask NOTE | INSTR. A chord with no such tone (a triad's seventh) leaves the row empty.
template <class Step>
uint32_t spread_steps(const Setup& u, uint32_t tone, const Step* cur, Step* out, uint32_t n) {
  uint32_t written = 0;
  for (uint32_t j = 0; j < n; ++j) {
    out[j] = cur[j];
    const int32_t k = chord_at_row(u, n, j);
    if (k < 0) {
      out[j].note = 0;
      continue;
    }
    const Chord c = chord_of(u, static_cast<uint32_t>(k));
    if (tone >= tone_count(c.quality)) {
      out[j].note = 0;
      continue;
    }
    out[j].note = static_cast<uint8_t>(c.root + kTones[c.quality][tone]);
    out[j].instr = static_cast<uint8_t>(u.instr);
    ++written;
  }
  return written;
}

}  // namespace chords
