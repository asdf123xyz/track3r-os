// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/formats/med/flow.h>
#include <o3os/formats/med/tempo.h>
#include <o3os/formats/mod/module.h>
#include <o3os/formats/progress.h>

#include <cstddef>
#include <cstdint>

namespace o3os::formats::mod {

// The flow of a MOD or S3M song (OS-MODIMP, docs/formats/mod.md section 4): what plays in which order. The order list, position jumps, pattern breaks, pattern loops and pattern delays
// decide it; the model has a list of patterns and an order, so a bounded simulation walks the song once and lists what it plays, in the structures of the OctaMED flow (med/flow.h:
// LinearSong, PatternDef, FlowReport, so that the importer, its report and its tools are the same kind of object for every module format). A pattern here is a piece of a source
// pattern (at most 64 rows: nothing is ever cut at a beat); `PatternDef::block` is the source pattern number.
//
// The rules are libopenmpt 0.7.3's, measured (tests/modimp_openmpt_oracle.py compares the rows visited and the time):
//  - Rows are applied channel by channel, left to right.
//  - Break (MOD Dxy, S3M Cxy): the next row is at the next order position, row x * 10 + y. Over 63: MOD plays row 0 of the next position, S3M ignores the command.
//  - Jump (Bxx): continue at order position xx, row 0. MOD: the jump clears a break row set by a channel to its left, so B after D plays row 0; S3M keeps it (break row and jump
//    are independent). A position past the end of the order list restarts the song at position 0 (counted).
//  - Pattern loop (MOD E6x, S3M SBx): x = 0 marks the loop start (row 0 until one is set), any other x plays the loop body x more times; per channel; the counters reset when an
//    order position is entered. A loop that jumps back wins over a break or jump on the same row (the row is read again at the end of the loop).
//  - Pattern delay (MOD EEx, S3M SEx): the row is played again x times with its notes cleared and its commands kept (each repeat is a one-row pattern flagged kPatReplay, as the
//    OctaMED 1E). MOD: the last delay of a row wins; S3M: the first.
//  - Speed: MOD F01..F1F, S3M Axx (1..255); tempo: MOD F20..FF, S3M T20..TFF (BPM, in the ticks of 2.5 / BPM seconds). Other values are ignored (MOD F00 does not stop the song).
//  - The end of the order list restarts at position 0. A song that returns to a (position, row) at which it entered before has a loop: the simulation stops there and says where.
// Bounds: as med/flow.h (every row played is in exactly one order entry of at most 64 rows, so a song the model can take never visits more than 255 x 64 rows; the work is also capped in
// cells decoded). Pattern and order overflow is LimitExceeded unless FlowOptions::truncate asks to stop there instead.

enum class OpKind : std::uint8_t { None, Jump, Break, LoopStart, LoopBack, Delay, Speed, Tempo };

struct FlowOp {
  OpKind kind = OpKind::None;
  std::uint8_t value = 0;   // Jump: the position; Break: the row (0..99 as decoded, the rules above clamp it); LoopBack: the count; Delay: the rows; Speed: ticks a row; Tempo: BPM
};

// What a cell's effect does to the flow (None for every other effect).
FlowOp flow_op(Kind kind, const Cell& cell) noexcept;

// The flow as a resumable job (the begin / step / progress shape of the formats' jobs). `module` and `out` must outlive the job and stay as they are until it is Done or Failed.
// step(budget) does about `budget` units of work (a unit is a cell decoded; a call always does at least one row and overruns by at most one row of 32 channels) and returns Running,
// Done or Failed.
class FlowJob {
 public:
  // The linearisation (the default) or the structural fallback: the patterns are the source patterns, the order is the order list, and no flow is simulated (what the mapping cannot
  // keep is counted in the report).
  void begin(Module& module, const med::FlowOptions& options, med::LinearSong& out, bool structural_mode = false) noexcept;
  Progress step(std::size_t budget) noexcept;
  Progress progress() const noexcept { return progress_; }
  const Status& status() const noexcept { return status_; }

 private:
  std::size_t tick() noexcept;
  std::size_t tick_row() noexcept;
  std::size_t tick_structural() noexcept;
  void fail(Error e, const char* detail) noexcept;
  void finish() noexcept;
  bool limit(const char* detail) noexcept;
  bool add_piece(std::uint16_t pattern, std::uint32_t first, std::uint32_t last, std::uint8_t flags, bool dep, std::uint32_t tempo, std::uint32_t spd) noexcept;
  void count_clamp(std::uint32_t row) noexcept;
  void enter(std::uint32_t pos, std::uint32_t row) noexcept;
  void reset_loops() noexcept;

  Module* m_ = nullptr;
  med::LinearSong* out_ = nullptr;
  med::FlowOptions opt_;
  med::TempoModel model_;
  Kind kind_ = Kind::None;
  bool structural_ = false;
  Progress progress_ = Progress::Failed;
  Status status_{Error::Malformed, 0, "the job has not begun"};
  std::uint32_t channels_ = 0;
  std::uint32_t orders_ = 0;
  // the simulation
  std::uint32_t pos_ = 0;          // order position
  std::uint32_t row_ = 0;
  std::uint32_t tempo_ = 0;        // BPM
  std::uint32_t spd_ = 0;          // ticks a row
  bool piece_open_ = false;
  std::uint32_t piece_first_ = 0;
  std::uint32_t piece_tempo_ = 0;
  std::uint32_t piece_spd_ = 0;
  bool piece_dep_ = false;
  std::uint8_t loop_row_[caps::kMaxChannels] = {};
  std::uint8_t loop_count_[caps::kMaxChannels] = {};
  Cell cells_[caps::kMaxChannels];
  // the structural fallback: the order position being listed
  std::uint32_t sp_ = 0;
  std::int16_t seen_[caps::kMaxS3mPatterns + 1] = {};   // pattern number -> its index in the pattern list, -1 none
};

}  // namespace o3os::formats::mod
