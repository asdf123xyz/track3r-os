// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/formats/format.h>
#include <o3os/formats/med/flow.h>
#include <o3os/formats/med/map.h>
#include <o3os/formats/med/mmd.h>
#include <o3os/model/project.h>

#include <cstddef>
#include <cstdint>

namespace o3os::formats::med {

// The OctaMED importer (OS-008b, slice M2, docs/formats/med.md section 3 and 7): a MED module into a project, as a resumable job
// and as a registered FormatReader ("med"). Contexts: main loop only, in steps of bounded work.
//
// The phases, each a few ticks of at most one line (64 tracks x 17 pages of cells) or one instrument:
//   open        the reader's open, then validate (pointer graph, caps, instruments) in steps of 64 units (Reader::validate_step)
//   flow        the bounded simulation of the song (flow.h): what is played in which order, as pieces of blocks. Refused
//               when it needs more patterns or order entries than the model holds, unless the options say what to do instead
//   scan        one pass over the played lines counting what each track carries, to decide which tracks get a slot
//   header      name, tempo, track slots
//   instruments one per tick: the model's instrument, never the PCM
//   patterns    one line per tick, cells to steps (map.h)
//   order       the order list
// Nothing reaches the project before the scan is done, so a refusal for lack of room or a damaged block leaves it as it was; a
// failure after that resets it (valid and empty), as every whole-project reader does.
//
// The PCM is not written by the importer: the platform makes `samples/instrNN.wav` (the name the OG import gives the slot) for every instrument of the report that says
// Sample, at pcm_rate(), from read_pcm() (16-bit mono; 8-bit data times 256, 16-bit data as it is, stereo mixed down, the longest
// octave of a multi-octave sample, the first waveform of a synth instrument). The Importer, and the RandomSource it was given,
// must stay alive for that.
//
// Memory: about 13 KB (the reader, the linearisation, the report, the line staging) and a model::Instrument of scratch.
class Importer {
 public:
  // Starts importing `source` into `arena`'s project (reset first). `source` must outlive the job and the PCM reads.
  void begin(RandomSource& source, model::ModelArena& arena, const ImportOptions& options) noexcept;
  // Does about `budget` cells of work (a line is at most 1,088) and returns Running, Done or Failed.
  Progress step(std::size_t budget) noexcept;
  Progress progress() const noexcept { return progress_; }
  // The first problem when Failed. offset is a file offset (reader errors) or 0.
  const Status& status() const noexcept { return status_; }
  const ImportReport& report() const noexcept { return report_; }
  const Reader& reader() const noexcept { return reader_; }
  const LinearSong& linear() const noexcept { return linear_; }
  const ImportOptions& options() const noexcept { return opt_; }
  unsigned permille() const noexcept;

  // The PCM of instrument slot `slot` (MED number slot + 1) after Done, when report().instrument[slot].result is Sample.
  std::uint32_t pcm_frames(std::uint32_t slot) const noexcept {
    return slot < caps::kMaxSamples ? report_.instrument[slot].frames : 0u;
  }
  std::uint32_t pcm_rate() const noexcept { return paula_rate(opt_.clock); }
  // `n` frames from frame `first` into dst (mono, 16-bit). Reads the file in pieces of at most 512 bytes.
  Status read_pcm(std::uint32_t slot, std::uint32_t first, std::uint32_t n, std::int16_t* dst) const noexcept;

 private:
  enum class Phase : std::uint8_t { Open, Validate, Flow, Scan, Header, Instruments, Patterns, Order, Done };

  std::size_t tick() noexcept;
  std::size_t tick_open() noexcept;
  std::size_t tick_validate() noexcept;
  std::size_t tick_flow() noexcept;
  std::size_t tick_scan() noexcept;
  std::size_t tick_header() noexcept;
  std::size_t tick_instruments() noexcept;
  std::size_t tick_patterns() noexcept;
  std::size_t tick_order() noexcept;
  bool open_pattern() noexcept;
  bool carry(std::uint32_t pattern_id, std::uint32_t line, const model::StepFx& fx) noexcept;
  void fail(Error e, std::uint32_t offset, const char* detail) noexcept;
  void fail_status(const Status& s) noexcept { fail(s.error, s.offset, s.detail); }
  void count_plays() noexcept;
  std::uint32_t piece_cost() const noexcept { return blk_.tracks * (1u + blk_.pages); }

  RandomSource* src_ = nullptr;
  model::Project* project_ = nullptr;
  ImportOptions opt_;
  Reader reader_;
  LinearSong linear_;
  ValidateState vstate_;
  Summary sum_;
  FlowJob flow_;
  LineMapper mapper_;
  ImportReport report_;
  InstFact facts_[caps::kMaxSamples + 1];
  std::uint8_t plays_[flow_caps::kMaxPatterns] = {};
  model::Instrument scratch_;
  Phase phase_ = Phase::Open;
  Progress progress_ = Progress::Failed;
  Status status_{Error::Malformed, 0, "the job has not begun"};
  std::uint64_t pcm_left_ = 0;
  // cursors
  std::uint32_t pd_ = 0;        // pattern definition (scan, patterns)
  std::uint32_t line_ = 0;      // line of the block
  bool pd_open_ = false;
  std::uint32_t slot_ = 0;      // instrument
  std::uint32_t work_cells_ = 0;  // cells decoded by the scan or mapping pass, against flow_caps::kMaxCells
  BlockInfo blk_;
  std::uint64_t slot_mask_ = 0;   // the MED tracks of the block being mapped that have a slot
  Cell cells_[caps::kMaxTracks];
};

// FormatReport::dropped_groups bits of a MED import (drop_group_name says what each is). The first eleven are the reader's Ignored
// bits, same numbers.
enum DropGroup : unsigned {
  kDropAnnotation = 0,
  kDropNotation,
  kDropMidiDumps,
  kDropMmdInfo,
  kDropArexx,
  kDropMidiCmd3x,
  kDropEcho,
  kDropVolAdjust,
  kDropStereoSep,
  kDropExtraSongs,
  kDropChannelSplit,
  kDropMasterVolume,   // mastervol is not 64
  kDropTracks,         // tracks with data had no slot
  kDropCommands,       // commands without a target (ImportReport::cmd_dropped)
  kDropSynthTables,    // synth and hybrid instruments: the tables
  kDropHoldDecay,      // hold and decay
  kDropDefaultPitch,   // InstrExt default pitch
  kDropMidiProgram,    // the MIDI program of a MIDI instrument, a sample it also had
  kDropLoopPoint,      // the song loops back and the model cannot store where to
  kDropInstruments,    // instruments refused (type, size, arena)
  kDropFlow,           // structural or truncated: flow commands dropped, part of the song cut
  kDropGroupCount
};

const char* drop_group_name(unsigned index) noexcept;

// The registered strategy ("med", FormatKind::Project). It needs ReadParams::random (the file, random access); the `source` of
// begin() is not read. Options are set on the object before begin(). DEFAULT (OS-008c, maintainer-reversible): `engine_fx` is ON here, so that an imported song's commands are the
// engine window's (FX 128 + the MED command, with the E00 00 terminator) that the Amiga voices run (audio/amiga_voices.h) and the instruments, which carry the Paula rate, play on
// the Amiga engine (app::SampleBank::engine_for); set `options().engine_fx = false` before begin() for the OG / core mapping. The Importer class itself defaults to false.
class ImportReader final : public FormatReader {
 public:
  static ImportOptions amiga_default() noexcept {
    ImportOptions o;
    o.engine_fx = true;
    return o;
  }

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
  ImportOptions options_ = amiga_default();
  FormatReport report_;
  Progress progress_ = Progress::Failed;
};

}  // namespace o3os::formats::med
