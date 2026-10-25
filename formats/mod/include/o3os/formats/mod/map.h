// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/formats/med/map.h>
#include <o3os/formats/mod/flow.h>
#include <o3os/formats/mod/module.h>
#include <o3os/model/instrument.h>

#include <cstdint>

namespace o3os::formats::mod {

// MOD and S3M into the project model, the pure part (OS-MODIMP, docs/formats/mod.md sections 3 and 5): a cell becomes the cells of the OctaMED mapping (med/map.h LineMapper), so that
// the commands run on the engine that plays OctaMED songs (the Amiga voice and the MED tick processor, audio/med_tick.h) or become the OG / core FX, exactly as for a MED import. Nothing
// here touches a file, the model or the heap.
//
//  - Notes. MOD: the nearest entry of ProTracker's period table, our note byte = index + 49 (C-2 = period 428 = 61, the sample at its native Paula rate), which is also libopenmpt's own
//    note number (measured). S3M: octave x 12 + semitone + 13 (C-4 = byte 0x40 = 61, the sample at its C2SPD), also libopenmpt's. The importer's synthetic SongInfo carries the
//    offset (play_transpose), the cell the note index.
//  - Volume. MOD Cxx and the S3M volume column (0..64) become a 0C command in the decimal coding the MED mapper reads (0x64 is 64), so the model's volume byte is 2 x the volume.
//  - Time. ProTracker Fxx and S3M A / T are the song's tempo model: BPM mode, four rows to a beat (a tick lasts 2.5 / BPM seconds, as in both formats), with the speed as ticks a row. They
//    are written through LineMapper::set_speed / set_tempo (the MED 09 and 0F, with the BPM and speed range of these formats).
//  - Flow commands (Bxx, Dxy, E6x, EEx and the S3M B, C, SBx, SEx) are executed by the flow (flow.h) and write nothing.
//  - Everything else is translated to a MED command by the tables in map.cpp, and what has no MED counterpart is counted as dropped. OS-COREFX: MOD 8xx and E8x are a pan like the S3M Xxx (0..0x80, 0x40 the
//    centre) and S8x; they are Channel pan (FX 57, 0..100, held: Cell command 0x46, written on every engine), and, for the instruments on the Modern engine,
//    00, 0A, 1A / 1B and 11 / 12 become the tracker FX 46..50 (med::ImportOptions::core_fx).

inline constexpr unsigned kFxIds = 96;
inline constexpr unsigned kNoFx = 0xFF;

// Source effect ids, for the tallies of the import report: MOD 0..15 (effects 0..F), 16..31 (E0x..EFx); S3M 33..58 (A..Z), 64..79 (S0x..SFx), 80 (any other).
unsigned fx_id(Kind kind, const Cell& cell) noexcept;
const char* fx_name(unsigned id) noexcept;  // "MOD 4xy vibrato", "S3M SBx pattern loop" ... for reports and tools; unreferenced in the image

// How an effect is mapped, by design (the mapper's own counters can only downgrade it: an engine command that the OG / core FX do not have is dropped there).
enum class FxClass : std::uint8_t {
  Mapped,   // to the MED command of the same meaning
  Approx,   // to the nearest thing (counted as approximated)
  Drop,     // no counterpart (counted as dropped)
  Flow,     // executed by the flow
};
FxClass fx_class(unsigned id) noexcept;

struct FxTally {
  std::uint32_t seen = 0;
  std::uint32_t approx = 0;
  std::uint32_t dropped = 0;
};

// What one cell says in the terms of LineMapper::cell.
struct Translated {
  med::Cell cell;                    // page 0: note, instrument, the effect as a MED command (command 0 when it has none)
  med::Cell extra[2];                // page 1: the volume column (0C), the note cut (0F FF)
  std::uint8_t extras = 0;
  OpKind timing = OpKind::None;      // Speed or Tempo: LineMapper::set_speed / set_tempo
  std::uint8_t timing_value = 0;
  unsigned id = kNoFx;               // the source effect (kNoFx: none, or a no-op)
  bool dropped = false;              // the translation itself has no MED command for it
  std::uint8_t med_cmd = 0;          // the MED command whose counters the mapper's drops and approximations of this effect are read from (0: none)
  bool flow = false;                 // the flow executes it
  bool approx = false;               // mapped to the nearest thing (by design: fx_class, or by this cell's parameter)
  bool volume_invalid = false;       // S3M: a volume column value above 64 (not 255)
  bool bad_instrument = false;       // an instrument number the importer has no slot for: dropped from the step
  bool bad_note = false;             // S3M: a note byte that is none
  bool off_table = false;            // MOD: a period that is not in the table
};

// OS-COREFX CF2: Scream Tracker's parameter memory. An S3M effect D E F I J K L P Q R S whose parameter is 00 repeats the channel's last non-zero parameter of its CLASS: D K L share the volume slide's,
// E F the portamento's (E04 then F00 slides up by 4), the others have their own, N (channel volume slide) and W (global volume slide) of CF3 too (measured against libopenmpt, tests/corefx_openmpt_oracle.py). memory_class gives the class 0..kMemClasses-1 of an S3M effect
// byte (A = 1), or -1 for one without memory.
inline constexpr unsigned kMemClasses = 10;
int memory_class(std::uint8_t effect) noexcept;

// `slots`: instrument numbers 1..slots exist (the importer's instrument slots). `core_fx` (OS-COREFX): the commands will be written as the tracker FX of the Modern engine (med::ImportOptions::core_fx),
// which count a fine slide in quarter units: an S3M extra fine slide is then exact (its quarter units are in Cell::ext) instead of approximated or dropped.
void translate(Kind kind, const Cell& src, std::uint32_t slots, Translated& out, bool core_fx = false) noexcept;

// ---- instruments -------------------------------------------------------------------------------------------------------
enum SampleFlag : std::uint16_t {
  kSampleRateClamped = 1u << 0,    // an S3M C2SPD outside 1,000..192,000 Hz (held to it: the engine's range, OS-RATEPITCH)
  kSampleRateDefault = 1u << 1,    // an S3M C2SPD of 0: ST3 plays it at 8,363 Hz
};

// The instrument of slot `slot` (number slot + 1) as the model holds it, and the report row (med::InstReport, so that the PCM and the reports are the same kind of object as for MED).
// `pcm_left` is what remains of the sample arena. True when `out` is an instrument to store. `modern`: the instrument is written MODERN, not AUTO (an S3M, whose notes span the
// octaves a Paula channel cannot play, and any module imported for the Modern engine, whose core FX the Amiga voices do not run; OS-BUG-MODENGINE); `extra_flags` gets SampleFlag bits.
bool map_sample(Kind kind, std::uint32_t slot, const SampleInfo& info, med::Clock clock, std::uint64_t pcm_left, bool modern, model::Instrument& out, med::InstReport& rep,
                std::uint16_t& extra_flags) noexcept;
// The Hz the PCM of a sample is written at: the Paula rate of period 428 for a MOD, the C2SPD (8,363 for 0, clamped) for an S3M.
std::uint32_t sample_rate_of(Kind kind, const SampleInfo& info, med::Clock clock) noexcept;

// The pan of channel `t` of a MOD in percent: Paula's L R R L, repeated.
std::uint8_t mod_pan_percent(std::uint32_t t) noexcept;

}  // namespace o3os::formats::mod
