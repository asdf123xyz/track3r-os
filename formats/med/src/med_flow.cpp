// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/formats/med/flow.h>

#include <algorithm>
#include <cstring>

namespace o3os::formats::med {

void LinearSong::reset() noexcept {
  pattern_count = 0;
  order_count = 0;
  memo_count = 0;
  initial_tempo = 0;
  initial_spd = 0;
  report = FlowReport();
  // The arrays are scratch past their counts; zero them so a run is reproducible and nothing stale is ever read.
  for (PatternDef& p : patterns) {
    p = PatternDef();
  }
  std::memset(order, 0, sizeof order);
  std::memset(memo, 0, sizeof memo);
}

std::uint32_t lines_per_piece(const SongInfo& song) noexcept {
  const std::uint32_t lpb = std::max<std::uint32_t>(song.lines_per_beat(), 1);
  return std::max<std::uint32_t>((flow_caps::kPatternLines / lpb) * lpb, 1);
}

namespace {

bool same_key(const PatternDef& a, const PatternDef& b) noexcept {
  return a.block == b.block && a.first == b.first && a.last == b.last && a.flags == b.flags && a.tempo_dep == b.tempo_dep &&
         a.entry_tempo == b.entry_tempo && a.entry_spd == b.entry_spd;
}

}  // namespace

// ---- the job -----------------------------------------------------------------------------------------------------------
// One tick is one step of the simulation: look at one play-list entry, enter a position, play one line, count one line of a
// block, emit one piece. step() runs ticks until the budget of work is spent.

void FlowJob::begin(const Reader& reader, const FlowOptions& options, LinearSong& out, bool structural_mode) noexcept {
  r_ = &reader;
  song_ = &reader.song();
  out_ = &out;
  opt_ = options;
  opt_.max_patterns = std::min(opt_.max_patterns, flow_caps::kMaxPatterns);
  opt_.max_orders = std::min(opt_.max_orders, flow_caps::kMaxOrders);
  model_ = tempo_model(*song_, opt_.clock);
  piece_ = lines_per_piece(*song_);
  out.reset();
  FlowReport& rep = out.report;
  rep.structural = structural_mode;
  status_ = Status();
  tempo_ = song_->def_tempo;
  spd_ = song_->tempo2;
  if (spd_ < 1 || spd_ > 32) {
    rep.header_clamped = true;
    spd_ = std::min<std::uint32_t>(std::max<std::uint32_t>(spd_, 1), 32);
  }
  out.initial_tempo = static_cast<std::uint16_t>(tempo_);
  out.initial_spd = static_cast<std::uint8_t>(spd_);
  if (!structural_mode && tempo_is_suspect(model_, tempo_)) {
    ++rep.tempo_suspect;
  }
  pos_ = 0;
  line_ = 0;
  guard_ = 0;
  sp_ = 0;
  cd_line_ = 0;
  emit_x_ = 0;
  std::memset(seen_, 0, sizeof seen_);
  if (song_->play_entries == 0) {
    rep.no_playable = true;
    progress_ = Progress::Done;
    return;
  }
  state_ = structural_mode ? State::SPlay : State::Settle;
  progress_ = Progress::Running;
}

Progress FlowJob::step(std::size_t budget) noexcept {
  std::size_t work = 0;
  while (progress_ == Progress::Running && (work == 0 || work < budget)) {
    work += tick();
  }
  return progress_;
}

std::size_t FlowJob::tick() noexcept {
  switch (state_) {
    case State::Settle: return tick_settle();
    case State::Begin: return tick_begin();
    case State::Line: return tick_line();
    case State::SPlay: return tick_splay();
    case State::SCount: return tick_scount();
    case State::SEmit: return tick_semit();
  }
  fail(Error::Malformed, "the job is in no state");
  return 1;
}

void FlowJob::fail(Error e, const char* detail) noexcept {
  status_ = Status{e, 0, detail};
  progress_ = Progress::Failed;
  out_->report.stop_pos = pos_;
  out_->report.stop_line = line_;
}

void FlowJob::fail_status(const Status& s) noexcept {
  status_ = s;
  progress_ = Progress::Failed;
  out_->report.stop_pos = pos_;
  out_->report.stop_line = line_;
}

void FlowJob::finish() noexcept {
  status_ = Status();
  progress_ = Progress::Done;
}

// A pattern or order limit: a truncation ends the song quietly, anything else is the caller's error. False: stopped.
bool FlowJob::limit(const char* detail) noexcept {
  if (opt_.truncate) {
    out_->report.truncated = true;
    finish();
  } else {
    fail(Error::LimitExceeded, detail);
  }
  return false;
}

// Adds one play of `d`: its pattern (found or created) and an order entry. False: the job has stopped.
bool FlowJob::emit(const PatternDef& d) noexcept {
  LinearSong& out = *out_;
  std::uint32_t idx = 0;
  while (idx < out.pattern_count && !same_key(out.patterns[idx], d)) {
    ++idx;
  }
  if (idx == out.pattern_count) {
    if (out.pattern_count >= opt_.max_patterns) {
      return limit("flow: more patterns than the model holds");
    }
    out.patterns[out.pattern_count++] = d;
  }
  if (out.order_count >= opt_.max_orders) {
    return limit("flow: more order entries than the model holds");
  }
  out.order[out.order_count++] = static_cast<std::uint8_t>(idx);
  return true;
}

bool FlowJob::piece_out(std::uint32_t first, std::uint32_t last, std::uint32_t t, std::uint32_t s, bool dep,
                        std::uint8_t flags) noexcept {
  PatternDef d;
  d.block = entry_.block;
  d.first = static_cast<std::uint16_t>(first);
  d.last = static_cast<std::uint16_t>(last);
  d.flags = flags;
  if (dep) {
    d.tempo_dep = 1;
    d.entry_tempo = static_cast<std::uint16_t>(t);
    d.entry_spd = static_cast<std::uint8_t>(s);
  }
  return emit(d);
}

// One line visited at a cost of `cost` cells: both caps. False: the job has failed.
bool FlowJob::visit(std::uint32_t cost) noexcept {
  FlowReport& rep = out_->report;
  if (++rep.visits > opt_.max_visits) {
    fail(Error::LimitExceeded, "flow: more line visits than the cap");
    return false;
  }
  rep.cells += cost;
  if (rep.cells > opt_.max_cells) {
    fail(Error::LimitExceeded, "flow: more cells decoded than the work cap");
    return false;
  }
  return true;
}

void FlowJob::count_clamp(std::uint32_t t, std::uint32_t s, std::uint32_t line) noexcept {
  if (tempo_to_bpm(model_, t, s).clamp != Clamp::None) {
    FlowReport& rep = out_->report;
    if (rep.tempo_clamps++ == 0) {
      rep.first_clamp_block = entry_.block;
      rep.first_clamp_line = line;
    }
  }
}

// ---- one line ----------------------------------------------------------------------------------------------------------

void FlowJob::apply(const Cell& c, std::uint32_t track, std::uint32_t line, Flow& f) noexcept {
  FlowReport& rep = out_->report;
  switch (c.command) {
    case 0x09:
      if (c.data >= 1 && c.data <= 0x20) {
        spd_ = c.data;
        ++rep.speed_sets;
        f.dep = true;
        count_clamp(tempo_, spd_, line);
      } else {
        ++rep.speed_ignored;
      }
      break;
    case 0x0B:
      f.jump = c.data;
      break;
    case 0x0F:
      if (c.data == 0) {
        f.brk = 0;
      } else if (c.data <= 0xF0) {
        tempo_ = c.data;
        ++rep.tempo_sets;
        f.dep = true;
        if (tempo_is_suspect(model_, tempo_)) {
          ++rep.tempo_suspect;
        }
        count_clamp(tempo_, spd_, line);
      } else if (c.data == 0xFE) {
        f.end = true;
      }
      break;
    case 0x16: {
      std::uint32_t n = c.data;
      if (n > flow_caps::kMaxLoopRepeats) {
        n = flow_caps::kMaxLoopRepeats;
        ++rep.loop_capped;
      }
      if (n == 0) {
        lstart_[track] = static_cast<std::uint16_t>(line);
      } else if (lcount_[track] != 0) {
        if (--lcount_[track] != 0) {
          f.loop_dest = lstart_[track];
        }
      } else {
        lcount_[track] = static_cast<std::uint8_t>(n);
        f.loop_dest = lstart_[track];
      }
      break;
    }
    case 0x1D:
      f.brk = c.data;
      break;
    case 0x1E:
      f.delay = c.data;
      if (f.delay > flow_caps::kMaxLineRepeats) {
        f.delay = flow_caps::kMaxLineRepeats;
        ++rep.repeat_capped;
      }
      break;
    default:
      break;
  }
}

bool FlowJob::scan_line(std::uint32_t line, Flow& f) noexcept {
  Cell cells[caps::kMaxTracks];
  for (std::uint32_t page = 0; page <= blk_.pages; ++page) {
    const Status s = r_->read_line(blk_, line, page, cells, false);
    if (!s.ok()) {
      fail_status(s);
      return false;
    }
    for (std::uint32_t t = 0; t < blk_.tracks; ++t) {
      apply(cells[t], t, line, f);
    }
  }
  return true;
}

// ---- the play list -----------------------------------------------------------------------------------------------------

std::uint32_t FlowJob::next_pos(std::uint32_t p) const noexcept { return p + 1 >= song_->play_entries ? 0 : p + 1; }

// One entry of the search for what plays next from pos_: skips what plays nothing, follows play-sequence jumps, stops at a
// Stop. Playable: state Begin. The line is reset to 0 whenever the position moves.
std::size_t FlowJob::tick_settle() noexcept {
  FlowReport& rep = out_->report;
  if (guard_ > song_->play_entries) {
    rep.no_playable = true;
    finish();
    return 1;
  }
  ++guard_;
  if (++rep.settle_steps > opt_.max_visits) {  // an entry costs reads: a hostile list must not make this unbounded
    fail(Error::LimitExceeded, "flow: more play-list steps than the cap");
    return 1;
  }
  Status s = r_->play_entry(pos_, entry_);
  if (!s.ok()) {
    fail_status(s);
    return 1;
  }
  if (entry_.kind == PlayKind::Block) {
    s = r_->block(entry_.block, blk_);
    if (!s.ok()) {
      fail_status(s);
      return 1;
    }
    if (!blk_.empty) {
      state_ = State::Begin;
      return 1;
    }
    ++rep.skipped_entries;
  } else if (entry_.kind == PlayKind::Stop) {
    rep.ended = true;
    finish();
    return 1;
  } else if (entry_.kind == PlayKind::Jump) {
    ++rep.play_jumps;
    if (entry_.target < entry_.section_len) {
      pos_ = entry_.section_first + entry_.target;
    } else {
      ++rep.jump_out_of_range;
      pos_ = 0;
    }
    line_ = 0;
    return 1;
  } else {
    ++rep.skipped_entries;
  }
  pos_ = next_pos(pos_);
  line_ = 0;
  return 1;
}

// A playable position: a return to a (position, line) entered before is the song's loop point (block entries reset the loop
// counters, so (position, line) is the whole state apart from tempo and speed); else note it and start playing.
std::size_t FlowJob::tick_begin() noexcept {
  LinearSong& out = *out_;
  FlowReport& rep = out.report;
  if (line_ >= blk_.lines) {
    ++rep.break_line_clamped;
    line_ = 0;
  }
  std::uint32_t m = 0;
  while (m < out.memo_count && !(out.memo[m].pos == pos_ && out.memo[m].line == line_)) {
    ++m;
  }
  if (m < out.memo_count) {
    rep.loop_found = true;
    rep.loop_order = out.memo[m].order;
    rep.loop_pos = out.memo[m].pos;
    rep.loop_line = out.memo[m].line;
    rep.loop_state_differs = out.memo[m].tempo != tempo_ || out.memo[m].spd != spd_;
    finish();
    return 1 + m;
  }
  if (out.memo_count >= opt_.max_orders + 1u) {
    limit("flow: more order entries than the model holds");
    return 1 + m;
  }
  LinearSong::RunStart& rs = out.memo[out.memo_count++];
  rs.pos = static_cast<std::uint16_t>(pos_);
  rs.line = static_cast<std::uint16_t>(line_);
  rs.order = out.order_count;
  rs.tempo = static_cast<std::uint16_t>(tempo_);
  rs.spd = static_cast<std::uint8_t>(spd_);
  ++rep.runs;
  std::memset(lstart_, 0, sizeof lstart_);
  std::memset(lcount_, 0, sizeof lcount_);
  piece_first_ = line_;
  piece_tempo_ = tempo_;
  piece_spd_ = spd_;
  piece_dep_ = false;
  state_ = State::Line;
  return 1 + m;
}

// One line of the block being played: scan it, decide where the song goes next, emit the piece that ends here.
std::size_t FlowJob::tick_line() noexcept {
  FlowReport& rep = out_->report;
  const std::uint32_t cost = blk_.tracks * (1u + blk_.pages);
  if (!visit(cost)) {
    return cost;
  }
  Flow f;
  if (!scan_line(line_, f)) {
    return cost;
  }
  piece_dep_ = piece_dep_ || f.dep;

  enum class Act : std::uint8_t { Next, NextPos, Jump, Loop, End } act;
  const bool last_line = line_ + 1 >= blk_.lines;
  if (f.end) {
    act = Act::End;
  } else if (f.jump >= 0 || f.brk >= 0) {
    act = Act::Jump;
  } else if (f.loop_dest >= 0) {
    act = Act::Loop;
  } else {
    act = last_line ? Act::NextPos : Act::Next;
  }
  const bool boundary = act == Act::Next && (line_ + 1) % piece_ == 0;
  const bool cut = act != Act::Next || f.delay > 0 || boundary;
  if (cut && !piece_out(piece_first_, line_, piece_tempo_, piece_spd_, piece_dep_, 0)) {
    return cost;
  }
  for (std::uint32_t k = 0; k < f.delay; ++k) {  // the repeats: the line again, notes cleared, commands kept
    ++rep.visits;
    if (rep.visits > opt_.max_visits) {
      fail(Error::LimitExceeded, "flow: more line visits than the cap");
      return cost + k;
    }
    ++rep.repeats;
    if (!piece_out(line_, line_, tempo_, spd_, f.dep, kPatReplay)) {
      return cost + k;
    }
  }
  const std::size_t work = cost + f.delay;
  switch (act) {
    case Act::Next:
      if (cut) {
        piece_first_ = line_ + 1;
        piece_tempo_ = tempo_;
        piece_spd_ = spd_;
        piece_dep_ = false;
      }
      ++line_;
      break;
    case Act::Loop:
      ++rep.loops;
      line_ = static_cast<std::uint32_t>(f.loop_dest);
      piece_first_ = line_;
      piece_tempo_ = tempo_;
      piece_spd_ = spd_;
      piece_dep_ = false;
      break;
    case Act::Jump: {
      const std::uint32_t nl = f.brk >= 0 ? static_cast<std::uint32_t>(f.brk) : 0;
      if (f.brk >= 0) {
        ++rep.breaks;
      }
      if (f.jump >= 0) {
        ++rep.jumps;
        if (static_cast<std::uint32_t>(f.jump) < entry_.section_len) {
          pos_ = entry_.section_first + static_cast<std::uint32_t>(f.jump);
        } else {
          ++rep.jump_out_of_range;
          pos_ = 0;
        }
      } else {
        pos_ = next_pos(pos_);
      }
      line_ = nl;
      guard_ = 0;
      state_ = State::Settle;
      break;
    }
    case Act::NextPos:
      pos_ = next_pos(pos_);
      line_ = 0;
      guard_ = 0;
      state_ = State::Settle;
      break;
    case Act::End:
      rep.ended = true;
      finish();
      break;
  }
  return work;
}

// ---- the structural fallback -------------------------------------------------------------------------------------------

// One play-list entry: a block that plays is counted (once) and then cut into pieces; what plays nothing is skipped.
std::size_t FlowJob::tick_splay() noexcept {
  FlowReport& rep = out_->report;
  if (sp_ >= song_->play_entries) {
    finish();
    return 1;
  }
  pos_ = sp_;
  line_ = 0;
  Status s = r_->play_entry(sp_, entry_);
  if (!s.ok()) {
    fail_status(s);
    return 1;
  }
  if (entry_.kind == PlayKind::Stop) {
    rep.ended = true;
    finish();
    return 1;
  }
  if (entry_.kind == PlayKind::Jump) {
    ++rep.dropped_jumps;
    ++sp_;
    return 1;
  }
  if (entry_.kind != PlayKind::Block) {
    ++rep.skipped_entries;
    ++sp_;
    return 1;
  }
  s = r_->block(entry_.block, blk_);
  if (!s.ok()) {
    fail_status(s);
    return 1;
  }
  if (blk_.empty) {
    ++rep.skipped_entries;
    ++sp_;
    return 1;
  }
  ++rep.runs;
  bool first = true;
  if (entry_.block < 4096) {
    std::uint8_t& byte = seen_[entry_.block >> 3];
    const std::uint8_t bit = static_cast<std::uint8_t>(1u << (entry_.block & 7));
    first = (byte & bit) == 0;
    byte = static_cast<std::uint8_t>(byte | bit);
  }
  cd_line_ = 0;
  emit_x_ = 0;
  state_ = first ? State::SCount : State::SEmit;
  return 1;
}

// One line of a block that is new to the fallback: count the commands the mapping cannot keep (each scanned line is a visit).
std::size_t FlowJob::tick_scount() noexcept {
  FlowReport& rep = out_->report;
  const std::uint32_t cost = blk_.tracks * (1u + blk_.pages);
  line_ = cd_line_;
  if (!visit(cost)) {
    return cost;
  }
  Cell cells[caps::kMaxTracks];
  for (std::uint32_t page = 0; page <= blk_.pages; ++page) {
    const Status s = r_->read_line(blk_, cd_line_, page, cells, false);
    if (!s.ok()) {
      fail_status(s);
      return cost;
    }
    for (std::uint32_t t = 0; t < blk_.tracks; ++t) {
      const Cell& c = cells[t];
      if (c.command == 0x0B) {
        ++rep.dropped_jumps;
      } else if (c.command == 0x16) {
        ++rep.dropped_loops;
      } else if (c.command == 0x1E) {
        ++rep.dropped_repeats;
      } else if (c.command == 0x1D || (c.command == 0x0F && c.data == 0)) {
        ++rep.break_cells;
      } else if (c.command == 0x0F && c.data == 0xFE) {
        ++rep.end_cells;
      }
    }
  }
  if (++cd_line_ >= blk_.lines) {
    state_ = State::SEmit;
  }
  return cost;
}

// One piece of the block (cut at multiples of the lines per piece) as a pattern and an order entry.
std::size_t FlowJob::tick_semit() noexcept {
  const std::uint32_t x = emit_x_;
  const std::uint32_t y = std::min<std::uint32_t>(blk_.lines - 1u, (x / piece_ + 1) * piece_ - 1);
  if (!piece_out(x, y, 0, 0, false, 0)) {
    return 1;
  }
  emit_x_ = y + 1;
  if (emit_x_ >= blk_.lines) {
    ++sp_;
    state_ = State::SPlay;
  }
  return 1;
}

Status linearise(const Reader& reader, const FlowOptions& options, LinearSong& out) noexcept {
  FlowJob job;
  job.begin(reader, options, out, false);
  while (job.step(~static_cast<std::size_t>(0)) == Progress::Running) {
  }
  return job.status();
}

Status structural(const Reader& reader, const FlowOptions& options, LinearSong& out) noexcept {
  FlowJob job;
  job.begin(reader, options, out, true);
  while (job.step(~static_cast<std::size_t>(0)) == Progress::Running) {
  }
  return job.status();
}

}  // namespace o3os::formats::med
