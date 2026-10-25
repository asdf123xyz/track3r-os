// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/formats/med/flow.h>
#include <o3os/formats/med/mmd.h>
#include <o3os/formats/med/tempo.h>
#include <o3os/model/instrument.h>
#include <o3os/model/limits.h>
#include <o3os/model/step.h>

#include <cstddef>
#include <cstdint>

namespace o3os::formats::med {

// OctaMED into the project model: the pure part of slice M2 (OS-008b, docs/formats/med.md sections 3 and 4). Everything here is
// a function of a few numbers: no file, no model, no allocation. The importer (import.h) drives it; the tests drive it directly.
//
// What it decides, each rule pinned by a test and, where the second opinion exists, by libopenmpt (tests/med_openmpt_oracle.py):
//  - Pitch. A MED note n plays a one-octave sample (type 0) at the Paula rate of period 428 at n = 13, and every other
//    instrument kind has its own unity note (libopenmpt 0.7.3 measured: types 1, 5, 6 at n = 1; types 2, 3, 4 at n = 13; type 7
//    and, by libxmp's tables, synth and hybrid at n = 37; MMD3 files sound two octaves lower, so n + 24 where the others have
//    n + 48). The model's sampler plays a sample at its own rate at note 61 (unity: the note table has 1.0 at 60), so the pattern note is `n + 48 + playtransp` (`- 24` in MMD3), which is also OpenMPT's own note number, the
//    instrument's rate is the Paula rate of period 428 (8,287 Hz PAL, 8,363 Hz NTSC) and the instrument's `tune` carries `strans`
//    and what its kind needs to land on that unity (+12, -24 or 0), clamped to the tune range +-24 and flagged.
//  - Volume. `0C` is decimal-coded unless the song says VOLHEX; in that case libopenmpt reads it as 7 bit ((v & 0x7F + 1) / 2)
//    where the document says "hex 0..64", so the importer follows the program that was measured (open item O12).
//  - Commands: section 4, one table, with the engine window (128 + command) as an option for the Amiga engine (M3).

// ---- the step FX types the importer writes -----------------------------------------------------------------------------
namespace fx {
// The OG types (ADR 0004).
inline constexpr std::uint8_t kMicroMove = 2;      // 0..100 of a step
inline constexpr std::uint8_t kRoll = 3;           // 9 x direction + period, period 1..8 = 1/1 1/2 1/3 1/4 1/6 1/8 1/12 1/16 of a step
inline constexpr std::uint8_t kMidiCcA = 8;        // A..E = 8..12
inline constexpr std::uint8_t kBreakPattern = 13;
inline constexpr std::uint8_t kTempo = 15;         // 5..200: BPM / 2 (PAR-082, inference)
inline constexpr std::uint8_t kGlide = 19;         // 0..100: a time, 0..50 = 0..1000 ms, 50..100 = 1000..15000 ms
inline constexpr std::uint8_t kGate = 20;          // 0..100 (id 20 of the FX table, model/fx_table.h)
inline constexpr std::uint8_t kPosition = 22;      // 0..100 percent of the sample
inline constexpr std::uint8_t kVolumeLfo = 23;     // 0..23 rate preset
inline constexpr std::uint8_t kReverse = 26;
inline constexpr std::uint8_t kPanning = 31;       // 0..100, 50 centre
inline constexpr std::uint8_t kFinetuneLfo = 33;   // 0..28 rate preset
inline constexpr std::uint8_t kMicroTune = 34;     // 0..198, 99 = in tune, -100..+100 cents
inline constexpr std::uint8_t kSlideUp = 41;       // 0..255 (id 41 of the FX table, model/fx_table.h)
inline constexpr std::uint8_t kSlideDown = 42;
// The core FX of ADR 0004 amendment 1.
inline constexpr std::uint8_t kSpeed = 43;         // ticks per step, 1..32
inline constexpr std::uint8_t kTempoFine = 44;     // adds (v - 128) x 0.25 BPM to the Tempo of the same step
inline constexpr std::uint8_t kOrderJump = 45;     // position 0..254 (the structural fallback's 0B)
// The tracker FX of ADR 0004 amendment 2 (OS-COREFX), written only when ImportOptions::core_fx asks for them.
inline constexpr std::uint8_t kTrkArp = 46;           // xy: the note, +x, +y semitones on the ticks of the row
inline constexpr std::uint8_t kTrkVolSlide = 47;      // xy: x up else y down, a tick, from tick 1
inline constexpr std::uint8_t kTrkFineVol = 48;       // xy: once, on tick 0
inline constexpr std::uint8_t kTrkFinePortaUp = 49;   // quarter Paula period units (Scream Tracker's), once, on tick 0
inline constexpr std::uint8_t kTrkFinePortaDown = 50;
// CF2 (ADR 0004 amendment 2): the tracker effects with no MED command. The MOD / S3M importer hands them to the mapper as Cell commands 0x40 + (FX id - 51), the data byte being the effect's value, and
// the mapper writes them only with ImportOptions::core_fx (the commands above 0x3F mean nothing to MED).
inline constexpr std::uint8_t kTrkPanSlide = 51;
inline constexpr std::uint8_t kTrkVibWave = 52;
inline constexpr std::uint8_t kTrkTremWave = 53;
inline constexpr std::uint8_t kTrkGliss = 54;
inline constexpr std::uint8_t kTrkRetrig = 55;
inline constexpr std::uint8_t kTrkTremor = 56;
inline constexpr std::uint8_t kTrkChanPan = 57;  // Cell command 0x46, data 0..100: the track's pan, held until the next one (MOD 8xx E8x, S3M Xxx S8x)
// CF3: Cell commands 0x47..0x4B, the data byte the effect's value (the S3M M N V W Y; the mapper writes them only with core_fx).
inline constexpr std::uint8_t kTrkChanVol = 58;        // 0..64, held
inline constexpr std::uint8_t kTrkChanVolSlide = 59;
inline constexpr std::uint8_t kTrkGlobalVol = 60;      // 0..64, held
inline constexpr std::uint8_t kTrkGlobalVolSlide = 61;
inline constexpr std::uint8_t kTrkPanbrello = 62;
inline constexpr std::uint8_t kCoreCmdFirst = 0x40;
inline constexpr std::uint8_t kCoreCmdLast = 0x4B;
// The Amiga/MED engine window: 128 + the MED command (0..63) holds every command verbatim.
inline constexpr std::uint8_t kEngineBase = 128;
inline constexpr std::uint8_t kEngineLast = 191;
}  // namespace fx

// ---- policy and options ------------------------------------------------------------------------------------------------
namespace pitch {
inline constexpr int kNoteOffset = 48;       // our note byte = MED note + 48 + playtransp ...
inline constexpr int kMmd3NoteShift = -24;   // ... - 24 for MMD3 (SoundStudio's songs sound two octaves lower)
inline constexpr std::uint32_t kUnityPeriod = 428;           // ProTracker C-2
inline constexpr std::uint32_t kPaulaPalHz = 3546895;
inline constexpr std::uint32_t kPaulaNtscHz = 3579545;
inline constexpr int kTuneLimit = 24;        // our tune range, semitones (the model's TUNE, -24..24)
}  // namespace pitch

// The Paula rate of period 428 (what a MED sample's native rate means): 8,287 Hz PAL, 8,363 Hz NTSC.
std::uint32_t paula_rate(Clock clock) noexcept;

// What to do when the song needs more patterns or order entries than the model holds (docs/formats/med.md Q4). The default
// refuses: LimitExceeded and a report that says what was found, so the UI can offer the other two (never a silent trim).
enum class Overflow : std::uint8_t {
  Refuse,      // LimitExceeded
  Structural,  // patterns = blocks, order = the play list, flow commands dropped and counted
  Truncate,    // the first order entries that fit
};

struct ImportOptions {
  Clock clock = Clock::Pal;                // the CIA clock of the tempo (3.5) and the Paula clock of the pitch
  Overflow overflow = Overflow::Refuse;
  // Write the commands that the Amiga engine has an entry for as FX 128 + command, the data byte verbatim (the engine window of
  // ADR 0004 amendment 1), instead of the OG / core approximation. For songs whose instruments will play on that engine (M3).
  bool engine_fx = false;
  // OS-COREFX (ADR 0004 amendment 2): write the tracker commands that have a core FX of the Modern engine (arpeggio 00, volume slide 0A, fine volume 1A / 1B on a row that the baked volume cannot
  // carry, fine slides 11 / 12) as FX 46..50, which the OctaMED importer leaves off (its commands are MED's, and nothing here is measured against OctaMED for them). The MOD / S3M importer
  // turns it on for the instruments that play on the Modern engine (not with the engine window).
  bool core_fx = false;
  // The work caps of the scan and of the mapping pass, in cells decoded (the flow has its own, FlowOptions::max_cells). The default
  // is the most a song of 255 pieces of 128 lines on 64 tracks can need; a test lowers one to see the refusal.
  std::uint32_t max_scan_cells = flow_caps::kMaxCells;
  std::uint32_t max_map_cells = flow_caps::kMaxCells;
};

// ---- the import report (never silent) -----------------------------------------------------------------------------------
inline constexpr std::uint8_t kNoSlot = 0xFF;

// What one MED track carried, weighted by how often its block is played.
struct TrackReport {
  std::uint32_t cells = 0;            // non-empty cells x plays
  std::uint32_t notes = 0;            // of them with a note
  std::uint8_t slot = kNoSlot;        // our track slot; kNoSlot for an empty track and for one that did not fit
};

enum InstFlag : std::uint16_t {
  kInstSynth = 1u << 0,          // a synth instrument: its first waveform as a looped single cycle, the tables dropped
  kInstHybrid = 1u << 1,         // a hybrid: its sample, the tables dropped
  kInstOctave = 1u << 2,         // several octaves: the longest one only
  kInstExtSample = 1u << 3,      // type 7 (two extra low octaves are not synthesised)
  kInstLoopQuantised = 1u << 4,  // the loop positions of the model (0..65535) cannot hold the exact frames
  kInstTuneClamped = 1u << 5,    // strans plus the kind's offset left our +-24
  kInstStereoMix = 1u << 6,      // stereo mixed down to mono
  kInstSixteenBit = 1u << 7,     // 16-bit data (kept)
  kInstDisabled = 1u << 8,       // InstrExt "disabled": volume 0
  kInstHoldDecay = 1u << 9,      // hold / decay (08, InstrExt, default decay) not mapped
  kInstDefaultPitch = 1u << 10,  // InstrExt default pitch not applied
  kInstMidiDropped = 1u << 11,   // a MIDI instrument: program and a sample it also had are not kept
  kInstLoopRule = 1u << 12,      // the loop flag and the loop length disagree: libxmp and OpenMPT read that differently (O11)
  kInstChannelClamped = 1u << 13,  // a MIDI channel above 16
};

enum class InstResult : std::uint8_t {
  None,         // no instrument in this slot of the file
  Sample,       // a sample instrument was made
  Midi,         // a MIDI instrument was made
  EmptyData,    // a sample of no frames: no instrument
  Unsupported,  // a type the reader does not take (packed, delta, HighLife, VSTi...): refused
  Oversize,     // the PCM alone would not fit the sample arena: refused
  ArenaFull,    // the arena was full by the time this instrument came: refused
  NoWave,       // a synth without a waveform: refused
};

struct InstReport {
  InstResult result = InstResult::None;
  std::uint8_t reader_notes = 0;      // InstNote bits of the reader (loop clamped, trimmed, long loop)
  std::uint16_t flags = 0;            // InstFlag
  std::int8_t tune = 0;
  std::int8_t fine_tune = 0;
  std::uint32_t frames = 0;           // mono frames of the PCM the instrument needs (Importer::read_pcm)
  // where the PCM comes from, for Importer::read_pcm
  std::uint32_t src = 0;              // first byte of the (left) data
  std::uint32_t src_right = 0;        // first byte of the right channel, 0 when mono
  std::uint8_t src_bits = 8;
};

struct ImportReport {
  // Back to the defaults, field by field (a whole-struct copy would put 4 KB on the stack).
  void reset() noexcept;

  std::uint8_t version = 0;                // MMD0..3
  std::uint32_t ignored = 0;               // the reader's Ignored bits present in the file (echo, ARexx, notation, ...)
  bool master_volume_ignored = false;      // mastervol is not the default 64
  bool structural = false;                 // the structural fallback was taken (Overflow::Structural, and it was needed)
  bool overflow = false;                   // refused for lack of room (Overflow::Refuse): Structural and Truncate would be offered
  bool initial_tempo_clamped = false;
  FlowReport flow;                         // what the simulation counted: loop point, tempo clamps, repeats, ...
  std::uint16_t patterns = 0;              // made, of 128
  std::uint16_t order = 0;                 // entries, of 255
  // tracks
  std::uint16_t tracks_used = 0;           // MED tracks that carry anything in what is played
  std::uint16_t tracks_kept = 0;           // given a slot
  std::uint16_t tracks_dropped = 0;        // used and without a slot (TrackReport::slot == kNoSlot)
  TrackReport track[caps::kMaxTracks];
  // cells
  std::uint32_t cells = 0;                 // cells decoded by the mapping pass
  std::uint32_t notes_out_of_range = 0;    // notes the model cannot hold: dropped, the instrument number kept
  std::uint32_t note_cuts = 0;             // note bytes of exactly 0x80, read as a note cut (OpenMPT)
  std::uint32_t note_hi_ignored = 0;       // bit 7 of a note byte with a note: masked away
  std::uint32_t fx_overflow = 0;           // commands that found no free FX slot of their step (a third command)
  std::uint32_t volume_clamped = 0;        // 0C above full scale
  std::uint32_t volume_overwritten = 0;    // a second volume in one step: the later one won
  std::uint32_t volume_baked = 0;          // notes without an instrument number that got the volume the track kept from a 0C or 1A / 1B
  std::uint32_t timing_carried = 0;        // Tempo, Speed or Break FX of a track without a slot, written on a track that has one
  std::uint32_t timing_lost = 0;           // ... and those that found no free FX slot on any (counted, not silent)
  std::uint32_t tempo_fx = 0;              // Tempo steps written
  std::uint32_t tempo_fx_clamped = 0;      // ... whose BPM is outside the FX range (5..200 x 2)
  std::uint32_t tempo_fx_fine = 0;         // ... with a Tempo fine correction
  std::uint32_t speed_fx = 0;              // Speed steps written
  std::uint32_t state_unknown = 0;         // commands that needed the track's instrument or volume and had none: dropped
  std::uint32_t cmd_dropped[256] = {};     // by MED command byte: dropped
  std::uint32_t cmd_approx[256] = {};      // by MED command byte: written, but not the same audible result
  // instruments
  std::uint8_t instruments = 0;            // made (sample and MIDI)
  std::uint8_t instruments_refused = 0;    // Unsupported, Oversize, ArenaFull, NoWave
  std::uint64_t pcm_bytes = 0;             // 16-bit mono PCM of the instruments made
  InstReport instrument[caps::kMaxSamples];
};

// ---- note, volume, tempo: the arithmetic -------------------------------------------------------------------------------
struct NoteResult {
  std::uint8_t note = 0;        // the model's note byte: 0 none, 1..128, model::kNoteCut
  bool out_of_range = false;    // the cell had a note the model cannot hold (note stays 0)
  bool cut = false;             // the exact byte 0x80
  bool hi_ignored = false;
};
NoteResult map_note(const SongInfo& song, const Cell& cell) noexcept;

struct VolumeResult {
  std::uint8_t value = 0;       // the model's volume byte, 0..127
  bool clamped = false;
};
// A `0C` data byte: decimal-coded (the default), or 7 bit when the song says VOLHEX (see the top of this file).
VolumeResult map_volume(const SongInfo& song, std::uint8_t data) noexcept;
inline std::uint8_t volume_to_med(std::uint8_t v127) noexcept { return static_cast<std::uint8_t>((v127 + 1u) >> 1); }

// BPM x 100 as the Tempo FX (5..200, BPM / 2) and, when it is not exactly representable, the Tempo fine FX that corrects it to a
// quarter of a BPM. `clamped`: the BPM is outside 10..400 and the value is the nearest.
struct TempoFx {
  std::uint8_t value = 0;
  std::uint8_t fine = 0;        // 0 = none, else the value of FX 44 (128 + k, k = -4..4, never 0)
  bool clamped = false;
  bool exact = false;           // value (with fine, when there is one) is the BPM to 0.01
};
TempoFx tempo_fx(std::uint32_t bpm_x100) noexcept;

// The Roll FX value for `y` ticks between retriggers in a line of `spd` ticks: the value (period 1..8), 0 when there is nothing to
// roll (fewer than 1.5 retriggers per line); `exact` when y divides spd and spd / y is one of 1, 2, 3, 4, 6, 8, 12, 16.
std::uint8_t roll_for(std::uint32_t spd, std::uint32_t y, bool& exact) noexcept;
// Micro-move for a delay of `x` ticks in a line of `spd` ticks: 0..100; exact when 100 x / spd is a whole number.
std::uint8_t micro_move_for(std::uint32_t spd, std::uint32_t x, bool& exact) noexcept;
// The note-cut tick of 18 xx as Gate: 0..100 of the line.
std::uint8_t gate_for(std::uint32_t spd, std::uint32_t xx, bool& exact) noexcept;
// 04 / 14 / 07: the speed nibble as an LFO rate preset 0..`max`.
std::uint8_t lfo_rate_for(std::uint32_t speed_nibble, std::uint32_t max) noexcept;
// 03 xx (speed >= 1), the portamento speed, as a Glide time: the time a slide of one octave takes at that speed (period 428 to
// 214) when a tick lasts `tick_us` microseconds, put on the glide scale (value / 50 x 1000 ms up to 1000 ms, then up to
// 15000 ms at 100).
std::uint8_t glide_for(std::uint32_t speed, std::uint32_t tick_us) noexcept;

// ---- the commands -------------------------------------------------------------------------------------------------------
// The instruments as the command mapping needs to see them, by MED instrument number (1..63).
struct InstFact {
  std::uint8_t svol = 0xFF;      // default volume 0..64, 0xFF = this number made no instrument
  bool midi = false;
  std::uint32_t frames = 0;      // the PCM, in frames
};

// Maps the cells of one line to steps. The flow has already decided what plays when (linearised or structural): this decides
// what a cell says. State it keeps per piece: the tempo and speed (so that 09 and 0F make the right Tempo), and per track the
// instrument and volume last seen (for 19, 1A, 1B and the MIDI controllers). All of it resets at the start of a piece, which is
// the unit the flow keys its patterns by.
class LineMapper {
 public:
  // `facts` is indexed by MED instrument number, 0..63 (entry 0 unused) and must stay valid. `structural`: no flow was run.
  void begin(const SongInfo& song, const ImportOptions& options, const InstFact* facts, std::uint32_t initial_tempo,
             std::uint32_t initial_spd, bool structural, ImportReport& report) noexcept;
  // A piece: `tempo` / `spd` are the MED values in force at its first line, `replay` clears notes and instruments (a 1E repeat).
  void begin_piece(std::uint32_t tempo, std::uint32_t spd, bool replay) noexcept;
  void begin_line() noexcept;
  // One cell: page 0 is the default cell, 1.. the extra command pages (command and data only). Pages go in order, tracks within.
  void cell(std::uint32_t track, const Cell& cell, std::uint32_t page) noexcept;
  // A cell of a track that has no slot: only what moves time is mapped (09, 0F tempo and, in the structural fallback, the breaks and
  // jumps), into that track's own staging step, so that the importer can carry it onto a track that has one. Everything else about
  // such a track is in the report as the track's cells and notes; its commands are not counted twice.
  void timing_cell(std::uint32_t track, const Cell& cell) noexcept;
  // After the last page of a line: a note without an instrument number keeps the volume the track had, which a 0C or 1A / 1B changed
  // (libopenmpt measured: 0C 20 then a bare note plays at 20, not full), so that volume is written into the step. `tracks` has bit t
  // set for each MED track that has a slot (the others are not touched).
  void end_line(std::uint64_t tracks) noexcept;
  // OS-MODIMP: a tempo or speed change of a module whose own commands are not MED's (ProTracker Fxx, Scream Tracker A and T), in force from this line: `bpm` (any 1..255, the song's
  // model is BPM mode with four lines to a beat) or the ticks a line `spd` (1..255; the Speed FX holds 1..32). Written as the MED 0F and 09 commands are (Tempo, Tempo fine, Speed);
  // `cmd` is the MED command a drop or an approximation is counted under.
  void set_tempo(std::uint32_t track, std::uint32_t bpm, std::uint8_t cmd) noexcept;
  void set_speed(std::uint32_t track, std::uint32_t spd, std::uint8_t cmd) noexcept;
  const model::Step& step(std::uint32_t track) const noexcept { return stage_[track]; }
  std::uint32_t tempo() const noexcept { return tempo_; }
  std::uint32_t speed() const noexcept { return spd_; }

 private:
  void command(std::uint32_t t, const Cell& c) noexcept;
  void command_0f(std::uint32_t t, const Cell& c) noexcept;
  bool engine(std::uint32_t t, const Cell& c) noexcept;
  bool core_command(std::uint32_t t, const Cell& c) noexcept;  // OS-COREFX: true when handled
  bool put(model::Step& s, std::uint8_t type, std::uint8_t value) noexcept;
  bool free_slots(const model::Step& s, unsigned n) const noexcept;
  void drop(std::uint8_t cmd) noexcept { ++rep_->cmd_dropped[cmd]; }
  void approx(std::uint8_t cmd) noexcept { ++rep_->cmd_approx[cmd]; }
  void set_volume(model::Step& s, std::uint8_t v127) noexcept;
  void emit_tempo(model::Step& s, std::uint8_t cmd, std::uint32_t tempo, std::uint32_t spd, unsigned reserve) noexcept;
  std::uint32_t tick_us() const noexcept;
  const InstFact* fact(std::uint32_t inst) const noexcept;
  std::uint32_t current_inst(std::uint32_t t) const noexcept { return inst_[t]; }

  const SongInfo* song_ = nullptr;
  ImportOptions opt_;
  TempoModel model_;
  const InstFact* facts_ = nullptr;
  ImportReport* rep_ = nullptr;
  bool structural_ = false;
  bool replay_ = false;
  std::uint32_t tempo0_ = 0, spd0_ = 0;   // the song's start (the state of a piece that sets no tempo or speed)
  std::uint32_t tempo_ = 0, spd_ = 0;
  model::Step stage_[caps::kMaxTracks];
  std::uint8_t inst_[caps::kMaxTracks] = {};
  std::int8_t vol_[caps::kMaxTracks] = {};  // -1 unknown, else MED volume 0..64
  std::uint8_t eng_[caps::kMaxTracks] = {};  // bit c: FX column c of the last line held an engine FX (engine_fx: the next line ends it with E00 00)
};

// ---- tracks ---------------------------------------------------------------------------------------------------------------
// Gives the MED tracks that carry something a slot: all of them in MED order when there are at most 16, else the 16 with the most
// played cells (ties: the lower track), still in MED order. Fills `slot`, `tracks_used`, `tracks_kept`, `tracks_dropped`; empty
// tracks keep kNoSlot. Returns the number of slots used.
std::uint32_t plan_tracks(ImportReport& report) noexcept;

// The pan of MED track `t` (0..100): the file's, else Paula's L R R L (hard), else centre for mixing songs. `has_pans`: the file
// has a pan table.
std::uint8_t track_pan_percent(const SongInfo& song, bool has_pans, std::int8_t file_pan, std::uint32_t t) noexcept;
std::uint8_t track_volume_percent(std::uint8_t med_volume) noexcept;

// ---- instruments ------------------------------------------------------------------------------------------------------
// The instrument of MED slot `slot` (number slot + 1) as the model holds it, and the report row. `pcm_left` is what remains of the
// sample arena. True when `out` is an instrument to store (Sample or Midi).
bool map_instrument(const Reader& reader, std::uint32_t slot, const InstrumentInfo& info, Clock clock, std::uint64_t pcm_left,
                    model::Instrument& out, InstReport& rep) noexcept;

}  // namespace o3os::formats::med
