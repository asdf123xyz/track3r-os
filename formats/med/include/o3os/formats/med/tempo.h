// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/formats/med/mmd.h>

#include <cstdint>

namespace o3os::formats::med {

// MED timing on our 192 PPQN grid (OS-008a slice M1, docs/formats/med.md 3.5). A native step is one MED line, 15 / BPM seconds
// (ADR 0013 D5: 4 steps per beat, 48 microsteps per step); a MED line lasts SPD ticks of T_tick seconds, so
// BPM_ours = 15 / (SPD x T_tick). Everything is integer arithmetic in 64 bits, rounded to nearest, and every result is
// BPM x 100 in the model's range (kMinTempoX100..kMaxTempoX100 = 4000..80000), so a tempo of 0, an SPD of 0 and a division
// by zero cannot happen and an out-of-range line is clamped and flagged, never silent.
//
//   Speed mode (the default), tempo > 10:  T_tick = 474,326 / (CIA clock x tempo)   PAL 709,379 Hz, NTSC 715,909 Hz
//        BPM x 100 = 1500 x clock x tempo / (474,326 x SPD)
//   Speed mode, tempo 1..10 (SoundTracker-compatible): T_tick = tempo / 293.7        BPM x 100 = 440,550 / (tempo x SPD)
//   8CHANNEL (wins over BPM mode): T_tick = 2.5 / BPM8, BPM8 = 179, 164, 152, 141, 131, 123, 116, 110, 104, 99 for tempo 1..10
//        (above 10 = 10)                                                              BPM x 100 = 600 x BPM8 / SPD
//   BPM mode: T_tick = 10 / (BPM x lines per beat)                                    BPM x 100 = 150 x BPM x lpb / SPD
//   Mixing (SoundStudio, flags2 0x80) in speed mode, tempo < 8: a fixed 157.86 (OpenMPT, "observed in SoundStudio 1.03,
//        bug?"): BPM x 100 = 94,716 / SPD; tempos 8 to 10 then use the first formula.
// The 8CHANNEL values are the table two independent readers share (libxmp mmd_common.c, OpenMPT Load_med.cpp); the formula
// of the Haskell med-module (min(10, tempo) / 390.7) disagrees below tempo 10 and is not used (med.md 3.5, Q5).
// libxmp and OpenMPT time speed mode as 0.66 / tempo seconds (BPM = tempo / 0.264); the CIA constants above are 1.3 % slower
// (0.6686 / tempo): the tests record that divergence.

enum class TempoMode : std::uint8_t { Speed, Bpm, EightChannel };
enum class Clock : std::uint8_t { Pal, Ntsc };

inline constexpr std::uint32_t kCiaPalHz = 709379;
inline constexpr std::uint32_t kCiaNtscHz = 715909;
inline constexpr std::uint32_t kMinBpmX100 = 4000;   // model::kMinTempoX100, our 40..800 BPM
inline constexpr std::uint32_t kMaxBpmX100 = 80000;

struct TempoModel {
  TempoMode mode = TempoMode::Speed;
  bool mixing = false;
  std::uint8_t lines_per_beat = 4;  // BPM mode: (flags2 & 0x1F) + 1; 4 otherwise
  Clock clock = Clock::Pal;
};

// The timing model of a song: 8CHANNEL wins over BPM mode (OctaMED 4.03 to SoundStudio 1.03, OpenMPT r13787).
TempoModel tempo_model(const SongInfo& song, Clock clock = Clock::Pal) noexcept;

enum class Clamp : std::uint8_t {
  None,
  Low,       // the exact value was below 40 BPM: x100 is 4000
  High,      // above 800 BPM: x100 is 80000
  BadInput,  // tempo 0 or SPD 0 (SPD is then taken as 1): x100 is clamped into range
};

struct Bpm {
  std::uint32_t x100 = kMinBpmX100;
  Clamp clamp = Clamp::None;
};

// `tempo` is the MED value (MMDsong.deftempo or the data of a 0F command, 1..240), `spd` the ticks per line (1..32; the
// header's tempo2 or the data of a 09 command).
Bpm tempo_to_bpm(const TempoModel& model, std::uint32_t tempo, std::uint32_t spd) noexcept;

// BPM mode with a tempo below 7: OctaMED 5 and SoundStudio 1.03 do not play the nominal value there (OpenMPT uses 111.5 BPM;
// not reproduced, counted so the import report can name it, docs/formats/med.md 3.5).
inline bool tempo_is_suspect(const TempoModel& model, std::uint32_t tempo) noexcept {
  return model.mode == TempoMode::Bpm && tempo < 7;
}

}  // namespace o3os::formats::med
