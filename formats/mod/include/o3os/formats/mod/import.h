// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/formats/format.h>
#include <o3os/formats/med/flow.h>
#include <o3os/formats/med/map.h>
#include <o3os/formats/mod/flow.h>
#include <o3os/formats/mod/map.h>
#include <o3os/formats/mod/module.h>
#include <o3os/model/project.h>

#include <cstddef>
#include <cstdint>

namespace o3os::formats::mod {

// The MOD and S3M importer (OS-MODIMP, new Community OS behaviour; docs/formats/mod.md): a module into a project, as a resumable job and as a registered FormatReader ("mod" and "s3m", one
// object). It is the OctaMED importer's twin (med/import.h) and shares its mapping: the cells become MED cells (map.h), LineMapper writes the steps, the report is a med::ImportReport
// with the effect tallies of these formats beside it. Contexts: main loop only, in steps of bounded work.
//
// The phases, each a few ticks of at most one row (32 channels of cells) or one instrument:
//   open         the header, the order list, the parapointer tables (Module::open)
//   flow         the bounded simulation of the song (flow.h): what is played in which order, as pieces of patterns; refused when it needs more patterns or order entries than the model
//                holds, unless the options say what to do instead
//   scan         one pass over the played rows counting what each channel carries, to decide which channels get a track (more than 16 keep the busiest, as MED)
//   header       name, tempo, tracks (channel pans and mutes)
//   instruments  one per tick: the model's instrument, never the PCM
//   patterns     one row per tick, cells to steps (LineMapper)
//   order        the order list
// Nothing reaches the project before the scan is done, so a refusal for lack of room leaves it as it was; a failure after that resets it (valid and empty).
//
// The PCM is not written by the importer: the platform makes `samples/instrNN.wav` for every instrument of the report that says Sample, at pcm_rate(slot), from read_pcm() (16-bit mono: 8-bit
// data times 256, unsigned data re-centred, a stereo S3M sample its left channel). The Importer and the RandomSource must stay alive for that.
//
// Where the commands run (ImportOptions::engine_fx): a MOD plays through the Amiga voice and the MED tick processor like an OctaMED song (the instruments carry the Paula rate and the effects
// go to the engine window); an S3M keeps each sample's own rate, its instruments are written MODERN and its effects are the OG / core FX, so the ones that FX set has no counterpart for
// are dropped and counted. `On` sends an S3M's effects to the engine window too (for a module whose instruments the user puts on the Amiga engine); `Off` does the opposite for a MOD (its instruments are written MODERN too, whatever their Paula rate: with engine AUTO they would play on the Amiga engine, which has none of the core FX).
//
// Memory: about 26 KB (the module and its decoded pattern, the linearisation, the report, the line staging) and a model::Instrument of scratch.

enum class EngineFx : std::uint8_t { Auto, On, Off };

struct ImportOptions {
  med::Clock clock = med::Clock::Pal;                  // the Paula clock of a MOD's pitch and the tempo of its notes (the Paula rate of period 428: 8,287 Hz PAL, 8,363 Hz NTSC)
  med::Overflow overflow = med::Overflow::Refuse;      // what to do when the song needs more patterns or order entries than the model holds (as for MED)
  EngineFx engine_fx = EngineFx::Auto;
  // The work caps of the scan and of the mapping pass, in cells decoded (the flow has its own); the default is the most a song of 255 pieces of 64 rows on 32 channels can need.
  std::uint32_t max_scan_cells = med::flow_caps::kMaxCells;
  std::uint32_t max_map_cells = med::flow_caps::kMaxCells;
};

// FormatReport::dropped_groups bits (drop_group_name says what each is).
enum DropGroup : unsigned {
  kDropAdLib = 0,       // AdLib instruments and the channels they play on
  kDropPacked,          // ADPCM-packed samples
  kDropStereo,          // stereo S3M samples: the left channel only
  kDropInstruments,     // instruments refused (arena, size) or beyond the 63 slots
  kDropTracks,          // channels with data had no track
  kDropEffects,         // effects without a counterpart
  kDropApprox,          // effects mapped to the nearest thing
  kDropGlobalVolume,    // the S3M global volume (not 64, and not written as a V) or master volume (not 0): "global or master volume"
  kDropSlides,          // slides that run one tick more per row than the module's (the engine's SoundTracker-slide switch is not the project's)
  kDropLoopPoint,       // the song loops back and the model cannot store where to
  kDropFlow,            // structural or truncated: flow commands dropped, part of the song cut
  kDropPeriods,         // MOD periods that are not in the table, snapped to the nearest note
  kDropRestart,         // the MOD restart position
  kDropGroupCount
};
const char* drop_group_name(unsigned index) noexcept;

struct ImportReport {
  void reset() noexcept;

  med::ImportReport med;           // tracks, cells, notes, volumes, tempo, instruments, FX counts, as for MED (cmd_dropped / cmd_approx are by MED command)
  Kind kind = Kind::None;
  Variant variant = Variant::None;
  std::uint8_t channels = 0;
  bool engine_fx = false;          // the commands went to the engine window
  bool structural = false;
  bool overflow = false;           // refused for lack of room (Overflow::Refuse): Structural and Truncate would be offered
  bool initial_tempo_clamped = false;
  bool speed_clamped = false;      // an S3M A above 32: the Speed FX holds 1..32 (the tempo it makes is exact)
  FxTally fx[kFxIds];              // per source effect: seen, approximated, dropped (fx_name)
  std::uint32_t periods_off_table = 0;
  std::uint32_t bad_notes = 0;
  std::uint32_t instrument_refs_dropped = 0;  // cells naming an instrument number with no slot
  std::uint32_t volume_invalid = 0;
  std::uint32_t adlib_cells = 0;              // cells on AdLib channels (nothing plays them)
  std::uint32_t unused_cells = 0;             // cells on S3M channels the header marks unused
  std::uint32_t tick0_slide_cells = 0;        // slide commands written for the engine: it runs them on tick 0 too, ST3 and ProTracker do not
  std::uint32_t instruments_beyond = 0;       // S3M instruments above number 63
  std::uint32_t samples_trimmed = 0;          // samples whose data runs past the end of the file
  std::uint32_t loops_clamped = 0;
  std::uint32_t rates_clamped = 0;            // S3M C2SPD outside 1,000..192,000 Hz
  std::uint32_t rates_defaulted = 0;          // S3M C2SPD of 0 (8,363 Hz)
  std::uint32_t stereo_samples = 0;
  std::uint32_t adlib_instruments = 0;
  std::uint32_t packed_samples = 0;
  std::uint32_t patterns_in_file = 0;
  // the header, for the report: S3M global volume (64 = nothing to say), S3M master volume byte, MOD restart
  std::uint8_t global_volume = 64;
  std::uint8_t master_volume = 0;
  std::uint8_t restart = 0;
  std::uint8_t skipped_orders = 0;
  std::uint8_t bad_orders = 0;
  bool fast_slides = false;
  bool global_volume_written = false;  // OS-COREFX: the header's global volume went into the song as a V (FX 60) at row 0 of the first pattern played, so the drop group does not say it
};

class Importer {
 public:
  // Starts importing `source` into `arena`'s project (reset first). `source` must outlive the job and the PCM reads.
  void begin(RandomSource& source, model::ModelArena& arena, const ImportOptions& options) noexcept;
  // Does about `budget` cells of work and returns Running, Done or Failed.
  Progress step(std::size_t budget) noexcept;
  Progress progress() const noexcept { return progress_; }
  // The first problem when Failed. offset is a file offset or 0.
  const Status& status() const noexcept { return status_; }
  const ImportReport& report() const noexcept { return report_; }
  const Module& module() const noexcept { return module_; }
  const med::LinearSong& linear() const noexcept { return linear_; }
  const ImportOptions& options() const noexcept { return opt_; }
  unsigned permille() const noexcept;

  // The PCM of instrument slot `slot` (number slot + 1) after Done, when report().med.instrument[slot].result is Sample.
  std::uint32_t pcm_frames(std::uint32_t slot) const noexcept { return slot < caps::kSlots ? report_.med.instrument[slot].frames : 0u; }
  std::uint32_t pcm_rate(std::uint32_t slot) const noexcept;
  // `n` frames from frame `first` into dst (mono, 16-bit). Reads the file in pieces of at most 512 bytes.
  Status read_pcm(std::uint32_t slot, std::uint32_t first, std::uint32_t n, std::int16_t* dst) const noexcept;

 private:
  enum class Phase : std::uint8_t { Open, Flow, Scan, Header, Instruments, Patterns, Order, Done };

  std::size_t tick() noexcept;
  std::size_t tick_open() noexcept;
  std::size_t tick_flow() noexcept;
  std::size_t tick_scan() noexcept;
  std::size_t tick_header() noexcept;
  std::size_t tick_instruments() noexcept;
  std::size_t tick_patterns() noexcept;
  std::size_t tick_order() noexcept;
  bool open_pattern() noexcept;
  bool carry(std::uint32_t pattern_id, std::uint32_t line, const model::StepFx& fx) noexcept;
  void apply_cell(std::uint32_t channel, const Cell& cell) noexcept;
  void tally(const Translated& t, std::uint32_t before_drop, std::uint32_t before_approx, std::uint8_t cmd) noexcept;
  void fail(Error e, std::uint32_t offset, const char* detail) noexcept;
  void fail_status(const Status& s) noexcept { fail(s.error, s.offset, s.detail); }
  bool channel_counts(std::uint32_t ch) const noexcept;
  std::uint32_t row_cost() const noexcept { return 1u + module_.info().channels; }

  RandomSource* src_ = nullptr;
  model::Project* project_ = nullptr;
  ImportOptions opt_;
  Module module_;
  med::LinearSong linear_;
  FlowJob flow_;
  med::LineMapper mapper_;
  med::ImportOptions mopt_;      // what the mapper is given
  med::SongInfo song_;           // the song as the mapper sees it: BPM mode, four rows to a beat, the note offset of the format
  ImportReport report_;
  med::InstFact facts_[caps::kSlots + 1];
  std::uint8_t plays_[med::flow_caps::kMaxPatterns] = {};
  model::Instrument scratch_;
  Phase phase_ = Phase::Open;
  Progress progress_ = Progress::Failed;
  Status status_{Error::Malformed, 0, "the job has not begun"};
  std::uint64_t pcm_left_ = 0;
  bool structural_ = false;
  std::uint32_t pd_ = 0;
  std::uint32_t line_ = 0;
  bool pd_open_ = false;
  std::uint32_t slot_ = 0;
  std::uint32_t work_cells_ = 0;
  std::uint32_t slots_ = 0;      // instrument numbers the importer has slots for: min(sample count, 63)
  std::uint64_t slot_mask_ = 0;  // the channels of the pattern being mapped that have a track
  Cell cells_[caps::kMaxChannels];
  // OS-COREFX CF2, S3M parameter memory: the last non-zero parameter of each channel's volume slide (D K L share it), portamento (E F), J, Q, R, S, P and I (map.h memory_class). A cell with parameter 00
  // takes it, so that the pattern the mapper writes holds explicit values (libopenmpt's ST3 playback does the same).
  std::uint8_t mem_[caps::kMaxChannels][kMemClasses] = {};
};

// The registered strategy ("mod" and "s3m", FormatKind::Project; one object serves both descriptors). It needs ReadParams::random (the file, random access); the `source` of begin() is not
// read. Options are set on the object before begin().
class ImportReader final : public FormatReader {
 public:
  void begin(core::ByteSource& source, model::ModelArena& arena, const ReadParams& params) noexcept override;
  Progress step(std::size_t byte_budget) noexcept override;
  Progress progress() const noexcept override { return progress_; }
  const FormatReport& report() const noexcept override { return report_; }
  unsigned permille() const noexcept override { return importer_.permille(); }

  ImportOptions& options() noexcept { return options_; }
  const Importer& importer() const noexcept { return importer_; }

 private:
  void sync() noexcept;

  Importer importer_;
  ImportOptions options_;
  FormatReport report_;
  Progress progress_ = Progress::Failed;
};

}  // namespace o3os::formats::mod
