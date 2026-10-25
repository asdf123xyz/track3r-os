// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/formats/mod/flow.h>

#include <algorithm>
#include <cstring>

namespace o3os::formats::mod {

using med::flow_caps::kMaxOrders;
using med::flow_caps::kMaxPatterns;
using med::FlowReport;
using med::kPatReplay;
using med::LinearSong;
using med::PatternDef;

FlowOp flow_op(Kind kind, const Cell& c) noexcept {
  FlowOp op;
  const std::uint8_t p = c.param;
  const std::uint8_t sub = static_cast<std::uint8_t>(p >> 4);
  const std::uint8_t x = static_cast<std::uint8_t>(p & 15u);
  const std::uint8_t row = static_cast<std::uint8_t>((p >> 4) * 10u + (p & 15u));
  if (kind == Kind::Mod) {
    switch (c.effect) {
      case 0x0B:
        op.kind = OpKind::Jump;
        op.value = p;
        break;
      case 0x0D:
        op.kind = OpKind::Break;
        op.value = row;
        break;
      case 0x0E:
        if (sub == 6) {
          op.kind = x == 0 ? OpKind::LoopStart : OpKind::LoopBack;
          op.value = x;
        } else if (sub == 0x0E && x != 0) {
          op.kind = OpKind::Delay;
          op.value = x;
        }
        break;
      case 0x0F:
        if (p != 0) {
          op.kind = p < 0x20 ? OpKind::Speed : OpKind::Tempo;
          op.value = p;
        }
        break;
      default:
        break;
    }
    return op;
  }
  switch (c.effect) {  // S3M: A = 1 ... Z = 26
    case 1:
      if (p != 0) {
        op.kind = OpKind::Speed;
        op.value = p;
      }
      break;
    case 2:
      op.kind = OpKind::Jump;
      op.value = p;
      break;
    case 3:
      op.kind = OpKind::Break;
      op.value = row;
      break;
    case 19:
      if (sub == 0x0B) {
        op.kind = x == 0 ? OpKind::LoopStart : OpKind::LoopBack;
        op.value = x;
      } else if (sub == 0x0E && x != 0) {
        op.kind = OpKind::Delay;
        op.value = x;
      }
      break;
    case 20:
      if (p >= 0x20) {
        op.kind = OpKind::Tempo;
        op.value = p;
      }
      break;
    default:
      break;
  }
  return op;
}

namespace {

bool same_key(const PatternDef& a, const PatternDef& b) noexcept {
  return a.block == b.block && a.first == b.first && a.last == b.last && a.flags == b.flags && a.tempo_dep == b.tempo_dep && a.entry_tempo == b.entry_tempo &&
         a.entry_spd == b.entry_spd;
}

}  // namespace

// ---- the job -----------------------------------------------------------------------------------------------------------

void FlowJob::begin(Module& module, const med::FlowOptions& options, LinearSong& out, bool structural_mode) noexcept {
  m_ = &module;
  out_ = &out;
  opt_ = options;
  opt_.max_patterns = std::min(opt_.max_patterns, kMaxPatterns);
  opt_.max_orders = std::min(opt_.max_orders, kMaxOrders);
  kind_ = module.info().kind;
  structural_ = structural_mode;
  channels_ = module.info().channels;
  orders_ = module.info().order_count;
  model_ = med::TempoModel{med::TempoMode::Bpm, false, 4, opt_.clock};  // a tick lasts 2.5 / BPM seconds: BPM mode with four rows to a beat
  out.reset();
  FlowReport& rep = out.report;
  rep.structural = structural_mode;
  status_ = Status();
  tempo_ = module.info().tempo;
  spd_ = module.info().speed;
  out.initial_tempo = static_cast<std::uint16_t>(tempo_);
  out.initial_spd = static_cast<std::uint8_t>(spd_);
  pos_ = 0;
  row_ = 0;
  sp_ = 0;
  piece_open_ = false;
  piece_dep_ = false;
  reset_loops();
  for (std::int16_t& s : seen_) {
    s = -1;
  }
  if (orders_ == 0 || channels_ == 0) {
    rep.no_playable = true;
    progress_ = Progress::Done;
    return;
  }
  if (!structural_mode) {
    enter(0, 0);
    LinearSong::RunStart& r = out.memo[out.memo_count++];  // the start of the song is the first position entered
    r.pos = 0;
    r.line = 0;
    r.order = 0;
    r.tempo = static_cast<std::uint16_t>(tempo_);
    r.spd = static_cast<std::uint8_t>(spd_);
  }
  progress_ = Progress::Running;
}

Progress FlowJob::step(std::size_t budget) noexcept {
  std::size_t work = 0;
  while (progress_ == Progress::Running && (work == 0 || work < budget)) {
    work += tick();
  }
  return progress_;
}

std::size_t FlowJob::tick() noexcept { return structural_ ? tick_structural() : tick_row(); }

void FlowJob::fail(Error e, const char* detail) noexcept {
  status_ = Status{e, 0, detail};
  progress_ = Progress::Failed;
  out_->report.stop_pos = pos_;
  out_->report.stop_line = row_;
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

bool FlowJob::add_piece(std::uint16_t pattern, std::uint32_t first, std::uint32_t last, std::uint8_t flags, bool dep, std::uint32_t tempo, std::uint32_t spd) noexcept {
  LinearSong& out = *out_;
  PatternDef d;
  d.block = pattern;
  d.first = static_cast<std::uint16_t>(first);
  d.last = static_cast<std::uint16_t>(last);
  d.flags = flags;
  if (dep) {
    d.tempo_dep = 1;
    d.entry_tempo = static_cast<std::uint16_t>(tempo);
    d.entry_spd = static_cast<std::uint8_t>(spd);
  }
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

void FlowJob::count_clamp(std::uint32_t row) noexcept {
  if (med::tempo_to_bpm(model_, tempo_, spd_).clamp != med::Clamp::None) {
    FlowReport& rep = out_->report;
    if (rep.tempo_clamps++ == 0) {
      rep.first_clamp_block = m_->order(pos_);
      rep.first_clamp_line = row;
    }
  }
}

void FlowJob::reset_loops() noexcept {
  std::memset(loop_row_, 0, sizeof loop_row_);
  std::memset(loop_count_, 0, sizeof loop_count_);
}

// The song is at (pos, row) by a jump, a break or the end of a pattern: a position entered. True is a new entry; the loop bookkeeping of a revisit is done by the caller.
void FlowJob::enter(std::uint32_t pos, std::uint32_t row) noexcept {
  pos_ = pos;
  row_ = row;
  piece_open_ = false;
  reset_loops();
  ++out_->report.runs;
}

// ---- one row ------------------------------------------------------------------------------------------------------------

std::size_t FlowJob::tick_row() noexcept {
  LinearSong& out = *out_;
  FlowReport& rep = out.report;
  const std::uint32_t cost = 1u + channels_;
  if (++rep.visits > opt_.max_visits) {
    fail(Error::LimitExceeded, "flow: more row visits than the cap");
    return cost;
  }
  rep.cells += cost;
  if (rep.cells > opt_.max_cells) {
    fail(Error::LimitExceeded, "flow: more cells decoded than the work cap");
    return cost;
  }
  const std::uint32_t pattern = m_->order(pos_);
  if (!piece_open_) {
    piece_open_ = true;
    piece_first_ = row_;
    piece_tempo_ = tempo_;
    piece_spd_ = spd_;
    piece_dep_ = false;
  }
  const Status s = m_->read_line(pattern, row_, cells_);
  if (!s.ok()) {
    status_ = s;
    progress_ = Progress::Failed;
    rep.stop_pos = pos_;
    rep.stop_line = row_;
    return cost;
  }

  // the commands of the row, channel by channel
  const bool mod = kind_ == Kind::Mod;
  std::int32_t jump = -1;        // the order position of a Jump
  std::int32_t brk = -1;         // the row of a Break (valid, or 0 where the rules make it that)
  bool any_jump = false;         // a Jump or a Break
  std::int32_t loop_dest = -1;   // a loop that jumps back
  std::uint32_t delay = 0;
  bool delay_set = false;
  bool row_dep = false;          // this row sets the speed or the tempo
  for (std::uint32_t ch = 0; ch < channels_; ++ch) {
    if (kind_ == Kind::S3m && !m_->info().chan_used[ch]) {
      continue;  // a channel ST3 does not use (setting 255): nothing on it counts
    }
    const FlowOp op = flow_op(kind_, cells_[ch]);
    switch (op.kind) {
      case OpKind::None:
        break;
      case OpKind::Jump:
        jump = op.value;
        any_jump = true;
        if (mod) {
          brk = 0;  // ProTracker: a jump clears the break row of a channel to its left
        }
        ++rep.jumps;
        break;
      case OpKind::Break:
        if (op.value > 63) {
          ++rep.break_line_clamped;
          if (mod) {
            brk = 0;
            any_jump = true;
          }  // S3M: the command is ignored
        } else {
          brk = op.value;
          any_jump = true;
        }
        ++rep.breaks;
        break;
      case OpKind::LoopStart:
        loop_row_[ch] = static_cast<std::uint8_t>(row_);
        break;
      case OpKind::LoopBack:
        if (loop_count_[ch] == 0) {
          loop_count_[ch] = op.value;
          loop_dest = loop_row_[ch];
        } else if (--loop_count_[ch] != 0) {
          loop_dest = loop_row_[ch];
        }
        break;
      case OpKind::Delay:
        if (mod || !delay_set) {  // MOD: the last delay of the row wins, S3M: the first
          delay = op.value;
          delay_set = true;
        }
        break;
      case OpKind::Speed:
        spd_ = op.value;
        row_dep = true;
        ++rep.speed_sets;
        count_clamp(row_);
        break;
      case OpKind::Tempo:
        tempo_ = op.value;
        row_dep = true;
        ++rep.tempo_sets;
        count_clamp(row_);
        break;
    }
  }

  piece_dep_ = piece_dep_ || row_dep;

  // where next
  std::uint32_t next_pos = pos_;
  std::uint32_t next_row = row_ + 1;
  bool entry = false;     // the next row is a position entered (and the piece ends here)
  bool cut = delay != 0;  // the piece ends after this row
  bool is_loop = false;
  if (loop_dest >= 0) {
    next_row = static_cast<std::uint32_t>(loop_dest);
    cut = true;
    ++rep.loops;
  } else if (any_jump) {
    entry = true;
    if (jump >= 0) {
      if (static_cast<std::uint32_t>(jump) >= orders_) {
        ++rep.jump_out_of_range;
        next_pos = 0;
      } else {
        next_pos = static_cast<std::uint32_t>(jump);
      }
    } else {
      next_pos = pos_ + 1;
    }
    if (next_pos >= orders_) {
      next_pos = 0;
    }
    next_row = brk >= 0 ? static_cast<std::uint32_t>(brk) : 0u;
  } else if (row_ + 1 >= caps::kRows) {
    entry = true;
    next_pos = pos_ + 1 >= orders_ ? 0u : pos_ + 1u;
    next_row = 0;
  }
  if (entry) {
    cut = true;
  }

  // the piece and its repeats
  const std::uint32_t this_pos = pos_;
  const std::uint32_t this_row = row_;
  if (cut) {
    if (!add_piece(static_cast<std::uint16_t>(pattern), piece_first_, this_row, 0, piece_dep_, piece_tempo_, piece_spd_)) {
      return cost;
    }
    piece_open_ = false;
    for (std::uint32_t i = 0; i < delay; ++i) {
      ++rep.repeats;
      // a repeat is the row again with its commands: a Speed or Tempo among them is applied again (the state it is keyed by is the one after the row)
      if (!add_piece(static_cast<std::uint16_t>(pattern), this_row, this_row, kPatReplay, row_dep, tempo_, spd_)) {
        return cost;
      }
    }
  }
  if (entry) {
    // a position entered: a revisit of one entered before is the loop of the song
    for (std::uint32_t i = 0; i < out.memo_count; ++i) {
      const LinearSong::RunStart& r = out.memo[i];
      if (r.pos == next_pos && r.line == next_row) {
        rep.loop_found = true;
        rep.loop_order = r.order;
        rep.loop_pos = r.pos;
        rep.loop_line = r.line;
        rep.loop_state_differs = r.tempo != tempo_ || r.spd != spd_;
        is_loop = true;
        break;
      }
    }
    if (is_loop) {
      finish();
      return cost;
    }
    if (out.memo_count >= kMaxOrders + 1u) {
      fail(Error::LimitExceeded, "flow: more entry points than the cap");
      return cost;
    }
    LinearSong::RunStart& r = out.memo[out.memo_count++];
    r.pos = static_cast<std::uint16_t>(next_pos);
    r.line = static_cast<std::uint16_t>(next_row);
    r.order = out.order_count;
    r.tempo = static_cast<std::uint16_t>(tempo_);
    r.spd = static_cast<std::uint8_t>(spd_);
    enter(next_pos, next_row);
    return cost;
  }
  if (cut) {  // a loop's jump back or a delay: the next row starts a piece of its own
    pos_ = this_pos;
    row_ = next_row;
    piece_open_ = false;
    return cost;
  }
  row_ = next_row;
  return cost;
}

// ---- the structural fallback -------------------------------------------------------------------------------------------------

// The patterns are the source patterns (each one once, rows 0..63), the order is the order list; one order position a tick.
std::size_t FlowJob::tick_structural() noexcept {
  LinearSong& out = *out_;
  FlowReport& rep = out.report;
  if (sp_ >= orders_) {
    finish();
    return 1;
  }
  const std::uint32_t pattern = m_->order(sp_);
  std::int16_t& at = seen_[pattern];
  if (at < 0) {
    // count what the mapping will not keep, once per pattern
    std::uint32_t cells = 0;
    for (std::uint32_t row = 0; row < caps::kRows; ++row) {
      const Status s = m_->read_line(pattern, row, cells_);
      if (!s.ok()) {
        status_ = s;
        progress_ = Progress::Failed;
        return 1;
      }
      cells += channels_;
      for (std::uint32_t ch = 0; ch < channels_; ++ch) {
        if (kind_ == Kind::S3m && !m_->info().chan_used[ch]) {
          continue;
        }
        switch (flow_op(kind_, cells_[ch]).kind) {
          case OpKind::Break: ++rep.break_cells; break;
          case OpKind::Jump: ++rep.dropped_jumps; break;
          case OpKind::LoopStart:
          case OpKind::LoopBack: ++rep.dropped_loops; break;
          case OpKind::Delay: ++rep.dropped_repeats; break;
          default: break;
        }
      }
    }
    rep.cells += cells;
    if (rep.cells > opt_.max_cells) {
      fail(Error::LimitExceeded, "flow: more cells decoded than the work cap");
      return cells;
    }
    if (out.pattern_count >= opt_.max_patterns) {
      limit("flow: more patterns than the model holds");
      return cells;
    }
    PatternDef d;
    d.block = static_cast<std::uint16_t>(pattern);
    d.first = 0;
    d.last = static_cast<std::uint16_t>(caps::kRows - 1);
    at = static_cast<std::int16_t>(out.pattern_count);
    out.patterns[out.pattern_count++] = d;
  }
  if (out.order_count >= opt_.max_orders) {
    limit("flow: more order entries than the model holds");
    return 1;
  }
  out.order[out.order_count++] = static_cast<std::uint8_t>(at);
  ++sp_;
  ++rep.runs;
  return 1;
}

}  // namespace o3os::formats::mod
