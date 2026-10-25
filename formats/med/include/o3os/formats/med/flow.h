// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/formats/med/mmd.h>
#include <o3os/formats/med/tempo.h>
#include <o3os/formats/progress.h>
#include <o3os/model/limits.h>

#include <cstddef>
#include <cstdint>

namespace o3os::formats::med {

// Flow simulation and linearisation (OS-008a slice M1, docs/formats/med.md 3.2). MED songs are programs: play sequences,
// jumps, breaks, loops and repeated lines decide what is played. Our model is a list of patterns and an order, so a bounded
// simulation walks the song once and lists what it plays; each stretch becomes a PatternDef (a piece of a block) and each
// play of it an order entry. Functions of a Reader: no model, no allocation, a result struct the caller owns. FlowJob runs
// the simulation in steps of bounded work, so that a job of the main loop can drive it; linearise() and structural() run it
// to the end.
//
// Rules (command numbers are the MED command byte; MMD0 has only 0..15):
//  - Commands are applied line by line, page by page (page 0 first), track by track; the last of two competing commands wins.
//  - 0F00 and 1D xx end the line: continue at the next play-list position, line 0 or line xx (hex). A line past the end of
//    that block restarts it at line 0 (counted).
//  - 0B xx: continue at position xx of the play sequence of the section being played (all of it for MMD0/1), line 0; a
//    position past the end of the section restarts the song at position 0 (counted). With 1D on the same line, at its line.
//  - 16 xx: per track, 00 marks the start line, any other value repeats back to it that many times, up to 255 as OctaMED
//    allows (libxmp's and OpenMPT's 15 is their 4-bit E6x conversion); the body is played 1 + n times. Break and jump win
//    over a loop on the same line. Loop counters reset when a position is entered. 255 repeats of a body need 256 order
//    entries, so such a loop ends in LimitExceeded (structural() or truncate takes over, as for any song that is too long).
//  - 1E xx: the line is played again xx more times (up to 255) with its notes cleared and its commands kept: each repeat is
//    a one-line pattern flagged kPatReplay.
//  - 09 xx sets the ticks per line (1..0x20; other values are ignored, counted); 0F 01..F0 sets the tempo.
//  - 0F FE ends the song after its line; so does a play-sequence Stop command. A play-sequence Jump continues at its target.
//    (libxmp and OpenMPT ignore 0FFE; the spec says it ends the song.)
//  - The end of the play list restarts at position 0. Entries that name no block, an empty block or a reserved number are
//    skipped (counted). A song that returns to a (position, line) at which it entered a block before has a loop: the
//    simulation stops there and says where (the model cannot store a loop point yet).
//  - A block of L lines is cut at multiples of C lines, C the largest multiple of the lines per beat that is <= 128 (4 lines
//    per beat when BPM mode is off), so beats are not cut; a stretch becomes one PatternDef per cut piece.
//  - Tempo and speed are state that a pattern depends on (09 changes the BPM equivalent of the current tempo, 0F the
//    equivalent at the current speed), so a piece with such a command is keyed by the state before its first line
//    (tempo_dep, entry_tempo, entry_spd); other pieces are shared by every play.
//
// Bounds. Every line played is in exactly one order entry (a piece of at most 128 lines), so a song the model can take never
// visits more than 255 x 128 = 32,640 lines: that is the default visit cap, and a song that needs more ends in LimitExceeded
// like one that needs a 256th order entry. Reading is the cost that matters: a line costs one read per page and decodes
// tracks x (1 + pages) cells, up to 64 x 17 = 1,088, so the work is also capped in cells (default 2^21 = 255 x 128 x 64, the
// most a song of 64 tracks without extra pages can need) and a block built to be slow (64 tracks, 16 pages) is refused after
// 1,927 lines, not 32,640. The search for what plays next is capped by the same number of play-list steps. Pattern and order
// overflow is LimitExceeded unless FlowOptions::truncate asks to stop there instead.

namespace flow_caps {
inline constexpr std::uint32_t kMaxPatterns = model::kMaxPatterns;       // 128
inline constexpr std::uint32_t kMaxOrders = model::kMaxSongPositions;    // 255
inline constexpr std::uint32_t kPatternLines = model::kMaxPatternLength;  // 128
inline constexpr std::uint32_t kMaxVisits = kMaxOrders * kPatternLines;   // 32,640
inline constexpr std::uint32_t kMaxCells = 1u << 21;                      // 2,097,152
inline constexpr std::uint32_t kMaxLoopRepeats = 255;                     // the data byte of 16 xx: nothing is cut today
inline constexpr std::uint32_t kMaxLineRepeats = 255;                     // the data byte of 1E xx
}  // namespace flow_caps

inline constexpr std::uint8_t kPatReplay = 1;  // PatternDef::flags: the notes of these lines are cleared (a 1E repeat)

// One native pattern the song needs: lines first..last (inclusive) of a block.
struct PatternDef {
  std::uint16_t block = 0;
  std::uint16_t first = 0;
  std::uint16_t last = 0;
  std::uint8_t flags = 0;          // kPatReplay
  std::uint8_t tempo_dep = 0;      // 1: a line of this piece sets tempo or speed; the two fields below are the state before `first`
  std::uint16_t entry_tempo = 0;   // MED tempo value (0F data or deftempo)
  std::uint8_t entry_spd = 0;      // ticks per line
};

struct FlowOptions {
  std::uint32_t max_patterns = flow_caps::kMaxPatterns;
  std::uint32_t max_orders = flow_caps::kMaxOrders;
  std::uint32_t max_visits = flow_caps::kMaxVisits;  // lines played (and, apart from the cells, play-list steps)
  std::uint32_t max_cells = flow_caps::kMaxCells;    // cells decoded: tracks x (1 + pages) per line scanned
  bool truncate = false;           // at the pattern or order limit stop, keep what was listed, and say so, instead of failing
  Clock clock = Clock::Pal;
};

// What the simulation did, for the import report (docs/formats/med.md 3.6). Counts of commands are per line visit.
struct FlowReport {
  bool ended = false;              // the song stops: 0FFE or a play-sequence Stop
  bool loop_found = false;         // the song returns to loop_order; the model has no loop point to store it in
  bool loop_state_differs = false; // tempo or speed on returning differ from the first visit of the loop's first line
  bool truncated = false;          // FlowOptions::truncate cut the song at a limit
  bool structural = false;         // this is the structural fallback, not the linearisation
  bool no_playable = false;        // no entry of the play list plays anything
  bool header_clamped = false;     // the header's tempo2 was 0 or above 32
  std::uint16_t loop_order = 0;    // order index the loop returns to
  std::uint16_t loop_pos = 0;      // ... the play-list position and line it enters
  std::uint16_t loop_line = 0;
  std::uint32_t visits = 0;        // lines simulated, repeats and loop passes included
  std::uint32_t cells = 0;         // cells decoded by the scans (the work measure, capped by FlowOptions::max_cells)
  std::uint32_t settle_steps = 0;  // play-list entries looked at to find what plays next (capped by max_visits)
  std::uint32_t runs = 0;          // play-list positions entered
  std::uint32_t breaks = 0;        // 0F00 / 1D lines that left the block
  std::uint32_t jumps = 0;         // 0B lines
  std::uint32_t loops = 0;         // loop-back jumps (16)
  std::uint32_t repeats = 0;       // repeated lines inserted (1E)
  std::uint32_t play_jumps = 0;    // play-sequence Jump entries followed
  std::uint32_t speed_sets = 0;
  std::uint32_t speed_ignored = 0; // 09 with data 0 or above 0x20
  std::uint32_t tempo_sets = 0;
  std::uint32_t tempo_clamps = 0;  // tempo or speed changes whose BPM equivalent is outside 40..800 (clamped)
  std::uint32_t tempo_suspect = 0; // BPM-mode tempos below 7 (tempo_is_suspect)
  std::uint32_t first_clamp_block = 0;  // where the first clamp happened (valid when tempo_clamps != 0)
  std::uint32_t first_clamp_line = 0;
  std::uint32_t loop_capped = 0;   // 16 repeat counts above kMaxLoopRepeats (the data byte cannot exceed it: stays 0)
  std::uint32_t repeat_capped = 0; // 1E counts above kMaxLineRepeats (likewise)
  std::uint32_t jump_out_of_range = 0;
  std::uint32_t break_line_clamped = 0;
  std::uint32_t skipped_entries = 0;
  // Structural fallback: cells (counted once per block) the mapping cannot keep.
  std::uint32_t break_cells = 0;   // 0F00 / 1D: become Break Pattern, the target line is dropped
  std::uint32_t dropped_jumps = 0; // 0B
  std::uint32_t dropped_loops = 0; // 16
  std::uint32_t dropped_repeats = 0;  // 1E
  std::uint32_t end_cells = 0;     // 0FFE
  // Where a failed run stopped.
  std::uint32_t stop_pos = 0;
  std::uint32_t stop_line = 0;
};

struct LinearSong {
  PatternDef patterns[flow_caps::kMaxPatterns];
  std::uint16_t pattern_count = 0;
  std::uint8_t order[flow_caps::kMaxOrders] = {};
  std::uint16_t order_count = 0;
  FlowReport report;
  // The song's own start: the state the first line plays in, and the MED values the PHDR tempo comes from.
  std::uint16_t initial_tempo = 0;
  std::uint8_t initial_spd = 0;
  // scratch of the simulation: the first order entry of every position the song entered (loop detection)
  struct RunStart {
    std::uint16_t pos, line, order, tempo;
    std::uint8_t spd;
  };
  RunStart memo[flow_caps::kMaxOrders + 1];
  std::uint16_t memo_count = 0;

  void reset() noexcept;
};

// Lines per piece: the largest multiple of the lines per beat that is <= 128.
std::uint32_t lines_per_piece(const SongInfo& song) noexcept;

// The simulation as a resumable job (docs/formats/med.md 3.2; the begin / step / progress shape of the FormatReaders). The
// Reader and the LinearSong must outlive the job and stay as they are until it is Done or Failed. step(budget) does about
// `budget` units of work and returns Running, Done or Failed: a unit is one cell decoded, one play-list entry looked at, one
// repeated line emitted; a call always does at least one step and overruns the budget by at most one line (64 x 17 cells
// plus 255 repeats). A failed job leaves the LinearSong partly filled and its report saying how far it got.
class FlowJob {
 public:
  // linearisation (the default) or the structural fallback
  void begin(const Reader& reader, const FlowOptions& options, LinearSong& out, bool structural_mode = false) noexcept;
  Progress step(std::size_t budget) noexcept;
  Progress progress() const noexcept { return progress_; }
  const Status& status() const noexcept { return status_; }

 private:
  enum class State : std::uint8_t { Settle, Begin, Line, SPlay, SCount, SEmit };
  enum class Settled : std::uint8_t { Playable, End, Error };
  struct Flow {
    std::int32_t jump = -1;       // 0B: position within the section
    std::int32_t brk = -1;        // 0F00 / 1D: line in the next block
    std::int32_t loop_dest = -1;  // 16: the line to go back to
    std::uint32_t delay = 0;      // 1E: repeats
    bool end = false;             // 0FFE
    bool dep = false;             // the line sets tempo or speed
  };

  std::size_t tick() noexcept;
  std::size_t tick_settle() noexcept;
  std::size_t tick_begin() noexcept;
  std::size_t tick_line() noexcept;
  std::size_t tick_splay() noexcept;
  std::size_t tick_scount() noexcept;
  std::size_t tick_semit() noexcept;
  void init_state() noexcept;
  void fail(Error e, const char* detail) noexcept;
  void fail_status(const Status& s) noexcept;
  void finish() noexcept;
  bool limit(const char* detail) noexcept;
  bool emit(const PatternDef& d) noexcept;
  bool piece_out(std::uint32_t first, std::uint32_t last, std::uint32_t t, std::uint32_t s, bool dep, std::uint8_t flags) noexcept;
  bool visit(std::uint32_t cost) noexcept;
  void count_clamp(std::uint32_t t, std::uint32_t s, std::uint32_t line) noexcept;
  void apply(const Cell& c, std::uint32_t track, std::uint32_t line, Flow& f) noexcept;
  bool scan_line(std::uint32_t line, Flow& f) noexcept;
  std::uint32_t next_pos(std::uint32_t p) const noexcept;

  const Reader* r_ = nullptr;
  const SongInfo* song_ = nullptr;
  LinearSong* out_ = nullptr;
  FlowOptions opt_;
  TempoModel model_;
  std::uint32_t piece_ = 1;       // lines per piece
  std::uint32_t tempo_ = 0;
  std::uint32_t spd_ = 0;
  State state_ = State::Settle;
  Progress progress_ = Progress::Failed;
  Status status_{Error::Malformed, 0, "the job has not begun"};
  // the play-list position being settled or played
  std::uint32_t pos_ = 0;
  std::uint32_t line_ = 0;
  std::uint32_t guard_ = 0;       // entries looked at since the position last changed
  PlayEntry entry_;
  BlockInfo blk_;
  // the piece being collected in the block being played
  std::uint32_t piece_first_ = 0;
  std::uint32_t piece_tempo_ = 0;
  std::uint32_t piece_spd_ = 0;
  bool piece_dep_ = false;
  std::uint16_t lstart_[caps::kMaxTracks] = {};
  std::uint8_t lcount_[caps::kMaxTracks] = {};
  // the structural fallback: the play-list entry, the line being counted, the next piece to emit, the blocks counted
  std::uint32_t sp_ = 0;
  std::uint32_t cd_line_ = 0;
  std::uint32_t emit_x_ = 0;
  std::uint8_t seen_[512] = {};
};

// Both run a FlowJob to the end. Any Status other than ok() leaves `out` partly filled and `out.report` saying how far it got.
// The default: a bounded simulation of the song's flow, listed as patterns and an order.
Status linearise(const Reader& reader, const FlowOptions& options, LinearSong& out) noexcept;

// The fallback when the linearisation does not fit: patterns are the blocks (cut into pieces), the order is the play list;
// no flow is simulated and what the mapping cannot keep is counted in the report (break_cells, dropped_*).
Status structural(const Reader& reader, const FlowOptions& options, LinearSong& out) noexcept;

}  // namespace o3os::formats::med
