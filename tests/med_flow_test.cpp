// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "support/mmd_fixture.h"
#include "support/need.h"
#include "support/rng.h"

#include <o3os/formats/med/flow.h>
#include <o3os/formats/med/mmd.h>

#include <cstdint>
#include <initializer_list>
#include <vector>

using namespace o3os;
using namespace o3os::formats::med;
using o3os::formats::Progress;
using o3os::test::mmd::BlockSpec;
using o3os::test::mmd::Bytes;
using o3os::test::mmd::ModuleSpec;

// Flow simulation and linearisation (OS-008a slice M1, docs/formats/med.md 3.2): goldens for every rule of flow.h on tiny
// MMD0/1/2 songs, the structural fallback, the limits, and a random-song loop that checks the invariants any result must
// have. Expected values were worked out by hand from the rules, not copied from the output.

namespace {

BlockSpec blk(unsigned lines, unsigned tracks = 2) {
  BlockSpec b;
  b.tracks = tracks;
  b.lines = lines;
  return b;
}

ModuleSpec song(std::vector<BlockSpec> blocks, std::vector<std::uint8_t> playseq, unsigned version = 1) {
  ModuleSpec m;
  m.version = version;
  m.deftempo = 33;
  m.tempo2 = 6;
  m.blocks = std::move(blocks);
  m.playseq = std::move(playseq);
  return m;
}

LinearSong& storage() {
  static LinearSong s;  // 4.5 KB: not on the stack
  return s;
}

struct Run {
  Bytes bytes;
  SpanSource src;
  Reader rd;
  Status open;
  Status st;
  LinearSong& out;
  explicit Run(const ModuleSpec& m, FlowOptions opt = FlowOptions(), bool structural_mode = false)
      : bytes(test::mmd::build(m)), src(bytes.data(), static_cast<std::uint32_t>(bytes.size())), out(storage()) {
    open = rd.open(src);
    if (open.ok()) {
      st = structural_mode ? structural(rd, opt, out) : linearise(rd, opt, out);
    }
  }
  const FlowReport& rep() const { return out.report; }
};

struct Want {
  unsigned block, first, last;
  unsigned flags = 0;
  unsigned dep = 0, et = 0, es = 0;
};

void expect_patterns(const LinearSong& s, std::initializer_list<Want> want) {
  NEED(s.pattern_count == want.size());
  unsigned i = 0;
  for (const Want& w : want) {
    CAPTURE(i);
    CHECK(s.patterns[i].block == w.block);
    CHECK(s.patterns[i].first == w.first);
    CHECK(s.patterns[i].last == w.last);
    CHECK(s.patterns[i].flags == w.flags);
    CHECK(s.patterns[i].tempo_dep == w.dep);
    CHECK(s.patterns[i].entry_tempo == w.et);
    CHECK(s.patterns[i].entry_spd == w.es);
    ++i;
  }
}

void expect_order(const LinearSong& s, std::initializer_list<unsigned> want) {
  NEED(s.order_count == want.size());
  unsigned i = 0;
  for (unsigned w : want) {
    CAPTURE(i);
    CHECK(s.order[i] == w);
    ++i;
  }
}

// What every result must satisfy, whatever the song.
void check_invariants(const Run& r, bool complete) {
  const LinearSong& s = r.out;
  CHECK(s.pattern_count <= flow_caps::kMaxPatterns);
  CHECK(s.order_count <= flow_caps::kMaxOrders);
  const std::uint32_t piece = lines_per_piece(r.rd.song());
  std::uint64_t lines = 0;
  for (unsigned i = 0; i < s.order_count; ++i) {
    NEED(s.order[i] < s.pattern_count);
    const PatternDef& d = s.patterns[s.order[i]];
    lines += static_cast<std::uint64_t>(d.last) - d.first + 1;
  }
  for (unsigned i = 0; i < s.pattern_count; ++i) {
    const PatternDef& d = s.patterns[i];
    BlockInfo b;
    NEED(r.rd.block(d.block, b).ok());
    CHECK(!b.empty);
    CHECK(d.first <= d.last);
    CHECK(d.last < b.lines);
    CHECK(d.first / piece == d.last / piece);                // beats are not cut
    CHECK(d.last - d.first + 1 <= flow_caps::kPatternLines);
    CHECK(((d.flags & kPatReplay) != 0) <= (d.first == d.last));  // a replay is one line
    if (d.tempo_dep == 0) {
      CHECK(d.entry_tempo == 0);
      CHECK(d.entry_spd == 0);
    } else {
      CHECK(d.entry_spd >= 1);
      CHECK(d.entry_spd <= 32);
    }
    for (unsigned j = 0; j < i; ++j) {  // keys are unique
      const PatternDef& e = s.patterns[j];
      CHECK(!(e.block == d.block && e.first == d.first && e.last == d.last && e.flags == d.flags && e.tempo_dep == d.tempo_dep &&
              e.entry_tempo == d.entry_tempo && e.entry_spd == d.entry_spd));
    }
  }
  if (complete && !s.report.structural) {
    CHECK(lines == s.report.visits);  // every simulated line is in exactly one order entry
  }
  if (s.report.loop_found) {
    CHECK(s.report.loop_order < s.order_count);
    CHECK(!s.report.ended);
  }
}

}  // namespace

TEST_CASE("med flow: a plain song plays every block once and loops to its start") {
  Run r(song({blk(8), blk(4)}, {0, 1}));
  NEED(r.st.ok());
  expect_patterns(r.out, {{0, 0, 7}, {1, 0, 3}});
  expect_order(r.out, {0, 1});
  CHECK(r.rep().loop_found);
  CHECK(r.rep().loop_order == 0);
  CHECK(r.rep().loop_pos == 0);
  CHECK(r.rep().loop_line == 0);
  CHECK(!r.rep().loop_state_differs);
  CHECK(!r.rep().ended);
  CHECK(r.rep().visits == 12);
  CHECK(r.rep().runs == 2);
  CHECK(r.out.initial_tempo == 33);
  CHECK(r.out.initial_spd == 6);
  check_invariants(r, true);
}

TEST_CASE("med flow: 0F00 leaves the block at its line") {
  BlockSpec a = blk(8);
  a.set(2, 1, 0, 0, 0x0F, 0x00);
  Run r(song({a, blk(4)}, {0, 1}));
  NEED(r.st.ok());
  expect_patterns(r.out, {{0, 0, 2}, {1, 0, 3}});
  expect_order(r.out, {0, 1});
  CHECK(r.rep().breaks == 1);
  CHECK(r.rep().visits == 7);
  check_invariants(r, true);
}

TEST_CASE("med flow: 1D xx continues at line xx of the next block (xx is hex)") {
  BlockSpec a = blk(8);
  a.set(1, 0, 0, 0, 0x1D, 0x02);
  Run r(song({a, blk(4)}, {0, 1}));
  NEED(r.st.ok());
  expect_patterns(r.out, {{0, 0, 1}, {1, 2, 3}});
  expect_order(r.out, {0, 1});
  CHECK(r.rep().loop_found);
  CHECK(r.rep().loop_order == 0);
  check_invariants(r, true);

  // 0x10 is line 16, not line 10 (libxmp reads the byte as BCD: a divergence recorded here)
  BlockSpec big = blk(20);
  BlockSpec head = blk(2);
  head.set(0, 0, 0, 0, 0x1D, 0x10);
  Run h(song({head, big}, {0, 1}));
  NEED(h.st.ok());
  expect_patterns(h.out, {{0, 0, 0}, {1, 16, 19}});
  check_invariants(h, true);
}

TEST_CASE("med flow: a break into the middle of the play list wraps and finds the loop at a later line") {
  BlockSpec a = blk(8);
  a.set(1, 0, 0, 0, 0x1D, 0x02);
  Run r(song({a, blk(4)}, {0, 1, 0}));
  NEED(r.st.ok());
  // pos 0 line 0..1, break to pos 1 line 2..3, pos 2 line 0..1, break to pos 0 (wrap) line 2..7, pos 1 line 0..3, then pos 2
  expect_patterns(r.out, {{0, 0, 1}, {1, 2, 3}, {0, 2, 7}, {1, 0, 3}});
  expect_order(r.out, {0, 1, 0, 2, 3});
  CHECK(r.rep().loop_found);
  CHECK(r.rep().loop_order == 2);
  CHECK(r.rep().loop_pos == 2);
  CHECK(r.rep().loop_line == 0);
  CHECK(r.rep().visits == 16);
  check_invariants(r, true);
}

TEST_CASE("med flow: 0B jumps to a position of the play sequence") {
  BlockSpec a = blk(4);
  a.set(3, 0, 0, 0, 0x0B, 0x02);  // pos 0 jumps over block 1 to position 2
  Run r(song({a, blk(4), blk(2)}, {0, 1, 2}));
  NEED(r.st.ok());
  expect_patterns(r.out, {{0, 0, 3}, {2, 0, 1}});
  expect_order(r.out, {0, 1});
  CHECK(r.rep().jumps == 1);
  CHECK(r.rep().loop_found);
  CHECK(r.rep().loop_order == 0);

  BlockSpec b = blk(4);
  b.set(1, 1, 0, 0, 0x0B, 0x00);  // back to the start
  Run back(song({blk(2), b, blk(2)}, {0, 1, 2}));
  NEED(back.st.ok());
  expect_patterns(back.out, {{0, 0, 1}, {1, 0, 1}});
  expect_order(back.out, {0, 1});
  CHECK(back.rep().loop_found);
  CHECK(back.rep().loop_order == 0);
  CHECK(back.rep().jumps == 1);

  // 0B and 1D on one line: position of the first, line of the second
  BlockSpec both = blk(6);
  both.set(0, 0, 0, 0, 0x0B, 0x02);
  both.set(0, 1, 0, 0, 0x1D, 0x01);
  Run c(song({both, blk(4), blk(4)}, {0, 1, 2}));
  NEED(c.st.ok());
  expect_patterns(c.out, {{0, 0, 0}, {2, 1, 3}});
  check_invariants(c, true);
}

TEST_CASE("med flow: 16 loops a stretch of a block, 00 marks the start, n repeats it n times") {
  BlockSpec a = blk(8);
  a.set(1, 0, 0, 0, 0x16, 0x00);
  a.set(3, 0, 0, 0, 0x16, 0x02);
  Run r(song({a}, {0}));
  NEED(r.st.ok());
  // the body 1..3 plays three times: the run that ends at the first loop-back, the run that ends at the second, and the
  // run that falls through the end of the loop and goes on to the end of the block
  expect_patterns(r.out, {{0, 0, 3}, {0, 1, 3}, {0, 1, 7}});
  expect_order(r.out, {0, 1, 2});
  CHECK(r.rep().loops == 2);
  CHECK(r.rep().visits == 14);
  check_invariants(r, true);

  // up to 255 repeats, as OctaMED allows (the 15 of libxmp and OpenMPT is their 4-bit E6x); no start marker: the loop returns
  // to line 0. 16 20 is 32 repeats: 32 runs 0..2 that end in a loop-back, then 0..3
  BlockSpec b = blk(4);
  b.set(2, 0, 0, 0, 0x16, 0x20);
  Run cap(song({b}, {0}));
  NEED(cap.st.ok());
  CHECK(cap.rep().loop_capped == 0);
  CHECK(cap.rep().loops == 32);
  CHECK(cap.out.order_count == 32 + 1);
  expect_patterns(cap.out, {{0, 0, 2}, {0, 0, 3}});
  check_invariants(cap, true);

  // 16 FF is 255 repeats: 256 order entries do not fit, so it ends in LimitExceeded, or at 255 entries when asked to truncate
  BlockSpec big = blk(4);
  big.set(2, 0, 0, 0, 0x16, 0xFF);
  Run over(song({big}, {0}));
  NEED(over.open.ok());
  CHECK(over.st.error == Error::LimitExceeded);
  CHECK(over.rep().loops == 255);
  FlowOptions trunc;
  trunc.truncate = true;
  Run cut(song({big}, {0}), trunc);
  NEED(cut.st.ok());
  CHECK(cut.rep().truncated);
  CHECK(cut.out.order_count == 255);
  CHECK(cut.rep().loop_capped == 0);
  check_invariants(cut, false);
  // ... and the structural fallback takes it: the loop is dropped and counted
  Run fall(song({big}, {0}), FlowOptions(), true);
  NEED(fall.st.ok());
  CHECK(fall.rep().dropped_loops == 1);
  expect_order(fall.out, {0});

  // each track has its own loop counters
  BlockSpec c = blk(8);
  c.set(0, 0, 0, 0, 0x16, 0x00);
  c.set(2, 0, 0, 0, 0x16, 0x01);
  c.set(2, 1, 0, 0, 0x16, 0x01);  // track 1 never marked a start: its start is line 0
  Run two(song({c}, {0}));
  NEED(two.st.ok());
  // line 2: track 0 count 1 -> back to 0; track 1 count 1 -> back to 0 (last wins). Second pass: both reach 0 and fall through
  expect_patterns(two.out, {{0, 0, 2}, {0, 0, 7}});
  expect_order(two.out, {0, 1});
  check_invariants(two, true);

  // a break wins over a loop on the same line
  BlockSpec d = blk(6);
  d.set(2, 0, 0, 0, 0x16, 0x03);
  d.set(2, 1, 0, 0, 0x0F, 0x00);
  Run brk(song({d, blk(2)}, {0, 1}));
  NEED(brk.st.ok());
  expect_patterns(brk.out, {{0, 0, 2}, {1, 0, 1}});
  CHECK(brk.rep().loops == 0);
}

TEST_CASE("med flow: 1E plays a line again, notes cleared, commands kept") {
  BlockSpec a = blk(8);
  a.set(2, 0, 30, 1, 0x1E, 0x02);
  Run r(song({a}, {0}));
  NEED(r.st.ok());
  expect_patterns(r.out, {{0, 0, 2}, {0, 2, 2, kPatReplay}, {0, 3, 7}});
  expect_order(r.out, {0, 1, 1, 2});
  CHECK(r.rep().repeats == 2);
  CHECK(r.rep().visits == 10);
  check_invariants(r, true);

  BlockSpec b = blk(3);
  b.set(0, 0, 0, 0, 0x1E, 0x40);  // 64 repeats: up to 255 as OctaMED allows
  Run cap(song({b}, {0}));
  NEED(cap.st.ok());
  CHECK(cap.rep().repeat_capped == 0);
  CHECK(cap.rep().repeats == 64);
  CHECK(cap.out.order_count == 1 + 64 + 1);
  check_invariants(cap, true);

  BlockSpec big = blk(3);
  big.set(0, 0, 0, 0, 0x1E, 0xFF);  // 255 repeats and the line itself: 256 order entries do not fit
  Run over(song({big}, {0}));
  NEED(over.open.ok());
  CHECK(over.st.error == Error::LimitExceeded);
  FlowOptions trunc;
  trunc.truncate = true;
  Run cut(song({big}, {0}), trunc);
  NEED(cut.st.ok());
  CHECK(cut.rep().truncated);
  CHECK(cut.out.order_count == 255);

  // a repeat of the last line, then the block ends
  BlockSpec c = blk(3);
  c.set(2, 0, 0, 0, 0x1E, 0x01);
  Run last(song({c}, {0}));
  NEED(last.st.ok());
  expect_patterns(last.out, {{0, 0, 2}, {0, 2, 2, kPatReplay}});
  expect_order(last.out, {0, 1});
}

TEST_CASE("med flow: 0FFE ends the song after its line, a loop is not found") {
  BlockSpec a = blk(8);
  a.set(2, 0, 0, 0, 0x0F, 0xFE);
  Run r(song({a, blk(4)}, {0, 1}));
  NEED(r.st.ok());
  expect_patterns(r.out, {{0, 0, 2}});
  expect_order(r.out, {0});
  CHECK(r.rep().ended);
  CHECK(!r.rep().loop_found);
  CHECK(r.rep().visits == 3);
  check_invariants(r, true);
}

TEST_CASE("med flow: tempo and speed make a pattern depend on the state it is entered in") {
  BlockSpec a = blk(3);
  a.set(0, 0, 0, 0, 0x09, 0x03);   // speed 3
  BlockSpec t = blk(2);
  t.set(0, 0, 0, 0, 0x0F, 0x50);   // tempo 80
  Run r(song({a, t, blk(2)}, {0, 1, 0, 2, 2}));
  NEED(r.st.ok());
  expect_patterns(r.out, {{0, 0, 2, 0, 1, 33, 6}, {1, 0, 1, 0, 1, 33, 3}, {0, 0, 2, 0, 1, 80, 3}, {2, 0, 1}});
  expect_order(r.out, {0, 1, 2, 3, 3});
  CHECK(r.rep().speed_sets == 2);
  CHECK(r.rep().tempo_sets == 1);
  CHECK(r.rep().loop_found);
  CHECK(r.rep().loop_order == 0);
  CHECK(r.rep().loop_state_differs);  // the loop returns to tempo 33 / speed 6 while the song ends at 80 / 3
  check_invariants(r, true);

  // a repeated line that sets speed: the repeat is entered in the state after the line
  BlockSpec b = blk(2);
  b.set(0, 0, 0, 0, 0x09, 0x03);
  b.set(0, 1, 0, 0, 0x1E, 0x01);
  Run rep(song({b}, {0}));
  NEED(rep.st.ok());
  expect_patterns(rep.out, {{0, 0, 0, 0, 1, 33, 6}, {0, 0, 0, kPatReplay, 1, 33, 3}, {0, 1, 1}});
  expect_order(rep.out, {0, 1, 2});
  check_invariants(rep, true);
}

TEST_CASE("med flow: speed values outside 1..0x20 are ignored, tempos outside the model's range are counted") {
  BlockSpec a = blk(6);
  a.set(0, 0, 0, 0, 0x09, 0x00);
  a.set(1, 0, 0, 0, 0x09, 0x21);
  a.set(2, 0, 0, 0, 0x0F, 0xF0);   // tempo 240 at speed 6: 897 BPM, clamped to 800
  a.set(3, 0, 0, 0, 0x09, 0x01);   // speed 1 at tempo 240: 5,384 BPM, clamped
  a.set(4, 0, 0, 0, 0x0F, 0x21);   // tempo 33 at speed 1: 740 BPM, in range
  a.set(5, 0, 0, 0, 0x09, 0x20);   // speed 32 at tempo 33: 23 BPM, clamped to 40
  Run r(song({a}, {0}));
  NEED(r.st.ok());
  CHECK(r.rep().speed_ignored == 2);
  CHECK(r.rep().speed_sets == 2);
  CHECK(r.rep().tempo_sets == 2);
  CHECK(r.rep().tempo_clamps == 3);
  CHECK(r.rep().first_clamp_block == 0);
  CHECK(r.rep().first_clamp_line == 2);

  ModuleSpec m = song({blk(2)}, {0});
  m.flags2 = 0x20 | 3;
  m.deftempo = 5;
  m.blocks[0].set(0, 0, 0, 0, 0x0F, 0x03);
  Run s(m);
  NEED(s.st.ok());
  CHECK(s.rep().tempo_suspect == 2);  // the header's 5 and the command's 3, both below 7 in BPM mode
}

TEST_CASE("med flow: a block is cut at multiples of the lines per piece") {
  struct Case {
    unsigned flags2;
    unsigned piece;
  };
  for (const Case& c : {Case{0, 128}, Case{0x20 | 4, 125}, Case{0x20 | 31, 128}, Case{0x20 | 2, 126}, Case{0x20 | 0, 128}, Case{0x20 | 25, 104}}) {
    CAPTURE(c.flags2);
    ModuleSpec m = song({blk(300)}, {0});
    m.flags2 = static_cast<std::uint8_t>(c.flags2);
    Run r(m);
    NEED(r.st.ok());
    CHECK(lines_per_piece(r.rd.song()) == c.piece);
    expect_patterns(r.out, {{0, 0, c.piece - 1}, {0, c.piece, 2 * c.piece - 1}, {0, 2 * c.piece, 299}});
    expect_order(r.out, {0, 1, 2});
    check_invariants(r, true);
  }
  // MMD0 holds 256 lines at most
  Run z(song({blk(256)}, {0}, 0));
  NEED(z.st.ok());
  expect_patterns(z.out, {{0, 0, 127}, {0, 128, 255}});

  // a run that ends mid-block is cut at the block's own grid: 1D into line 100 of a 200-line block
  BlockSpec head = blk(1);
  head.set(0, 0, 0, 0, 0x1D, 100);
  Run m(song({head, blk(200)}, {0, 1}));
  NEED(m.st.ok());
  expect_patterns(m.out, {{0, 0, 0}, {1, 100, 127}, {1, 128, 199}});
  check_invariants(m, true);

  // a break in the middle of the second piece
  BlockSpec big = blk(300);
  big.set(150, 0, 0, 0, 0x0F, 0x00);
  Run b(song({big}, {0}));
  NEED(b.st.ok());
  expect_patterns(b.out, {{0, 0, 127}, {0, 128, 150}});
  check_invariants(b, true);
}

TEST_CASE("med flow: entries that play nothing are skipped and counted") {
  Run bad(song({blk(2), blk(2)}, {0, 9, 1}));
  NEED(bad.st.ok());
  expect_patterns(bad.out, {{0, 0, 1}, {1, 0, 1}});
  CHECK(bad.rep().skipped_entries == 1);

  BlockSpec e = blk(2);
  e.empty = true;
  Run empty(song({blk(2), e}, {1, 0, 1}));
  NEED(empty.st.ok());
  expect_patterns(empty.out, {{0, 0, 1}});
  CHECK(empty.rep().skipped_entries >= 1);

  Run none(song({blk(2)}, {7, 8}));
  NEED(none.st.ok());
  CHECK(none.out.order_count == 0);
  CHECK(none.rep().no_playable);

  Run nothing(song({blk(2)}, {}));
  NEED(nothing.st.ok());
  CHECK(nothing.out.order_count == 0);
  CHECK(nothing.rep().no_playable);
}

TEST_CASE("med flow: jumps and breaks past their target are counted and restart or clamp") {
  BlockSpec a = blk(4);
  a.set(1, 0, 0, 0, 0x0B, 0x09);  // position 9 of a play list of 2
  Run j(song({a, blk(4)}, {0, 1}));
  NEED(j.st.ok());
  expect_patterns(j.out, {{0, 0, 1}});
  CHECK(j.rep().jump_out_of_range == 1);
  CHECK(j.rep().loop_found);

  BlockSpec b = blk(4);
  b.set(1, 0, 0, 0, 0x1D, 0x99);  // line 153 of a 4-line block: restarts at line 0
  Run k(song({b, blk(4)}, {0, 1}));
  NEED(k.st.ok());
  expect_patterns(k.out, {{0, 0, 1}, {1, 0, 3}});
  CHECK(k.rep().break_line_clamped == 1);
  check_invariants(k, true);
}

TEST_CASE("med flow: the work is counted in cells, tracks x (1 + pages) per line, and capped") {
  Run r(song({blk(8, 3)}, {0}));
  NEED(r.st.ok());
  CHECK(r.rep().visits == 8);
  CHECK(r.rep().cells == 8 * 3);

  ModuleSpec m = song({blk(4, 2)}, {0});
  m.blocks[0].page_cell(1, 0, 0, 0x09, 0x04);  // two extra pages: three command columns per track
  Run p(m);
  NEED(p.st.ok());
  CHECK(p.rep().cells == 4 * 2 * 3);

  FlowOptions small;
  small.max_cells = 20;
  Run c(song({blk(8, 4)}, {0}), small);
  NEED(c.open.ok());
  CHECK(c.st.error == Error::LimitExceeded);
  CHECK(c.rep().visits == 6);   // 4, 8, 12, 16, 20 cells fit; the sixth line makes 24
  CHECK(c.rep().cells == 24);
  CHECK(c.rep().stop_line == 5);
  small.max_cells = 24 * 2;     // the structural fallback counts the same cells while it scans a block
  Run sc(song({blk(8, 4)}, {0}), small, true);
  NEED(sc.st.ok());
  small.max_cells = 20;
  Run sf(song({blk(8, 4)}, {0}), small, true);
  CHECK(sf.st.error == Error::LimitExceeded);

  // the defaults: 255 order entries of 128 lines is the most a song can visit, and the cell budget is that times 64 tracks
  CHECK(flow_caps::kMaxVisits == 255u * 128u);
  CHECK(flow_caps::kMaxCells == (1u << 21));
  CHECK(flow_caps::kMaxCells >= 255u * 128u * 64u);
}

namespace {

LinearSong& storage2() {
  static LinearSong s;
  return s;
}

// Everything a result holds, for comparing two runs of the same song.
void expect_same(const LinearSong& a, const LinearSong& b) {
  CHECK(a.pattern_count == b.pattern_count);
  CHECK(a.order_count == b.order_count);
  for (unsigned i = 0; i < a.pattern_count && i < b.pattern_count; ++i) {
    CHECK(a.patterns[i].block == b.patterns[i].block);
    CHECK(a.patterns[i].first == b.patterns[i].first);
    CHECK(a.patterns[i].last == b.patterns[i].last);
    CHECK(a.patterns[i].flags == b.patterns[i].flags);
    CHECK(a.patterns[i].tempo_dep == b.patterns[i].tempo_dep);
    CHECK(a.patterns[i].entry_tempo == b.patterns[i].entry_tempo);
    CHECK(a.patterns[i].entry_spd == b.patterns[i].entry_spd);
  }
  for (unsigned i = 0; i < a.order_count && i < b.order_count; ++i) CHECK(a.order[i] == b.order[i]);
  const FlowReport& x = a.report;
  const FlowReport& y = b.report;
  CHECK(x.ended == y.ended);
  CHECK(x.loop_found == y.loop_found);
  CHECK(x.loop_state_differs == y.loop_state_differs);
  CHECK(x.truncated == y.truncated);
  CHECK(x.no_playable == y.no_playable);
  CHECK(x.loop_order == y.loop_order);
  CHECK(x.loop_pos == y.loop_pos);
  CHECK(x.loop_line == y.loop_line);
  CHECK(x.visits == y.visits);
  CHECK(x.cells == y.cells);
  CHECK(x.settle_steps == y.settle_steps);
  CHECK(x.runs == y.runs);
  CHECK(x.breaks == y.breaks);
  CHECK(x.jumps == y.jumps);
  CHECK(x.loops == y.loops);
  CHECK(x.repeats == y.repeats);
  CHECK(x.speed_sets == y.speed_sets);
  CHECK(x.tempo_sets == y.tempo_sets);
  CHECK(x.skipped_entries == y.skipped_entries);
  CHECK(x.break_cells == y.break_cells);
  CHECK(x.dropped_jumps == y.dropped_jumps);
  CHECK(x.dropped_loops == y.dropped_loops);
  CHECK(x.stop_pos == y.stop_pos);
  CHECK(x.stop_line == y.stop_line);
}

}  // namespace

TEST_CASE("med flow job: any budget gives the one-shot result, and a step does about the work it was given") {
  std::vector<ModuleSpec> mods;
  BlockSpec loops = blk(8);
  loops.set(1, 0, 0, 0, 0x16, 0x00).set(3, 0, 0, 0, 0x16, 0x05).set(6, 1, 0, 0, 0x1E, 0x03);
  mods.push_back(song({loops, blk(4)}, {0, 1, 0}));
  BlockSpec dep = blk(3);
  dep.set(0, 0, 0, 0, 0x09, 0x03).set(0, 1, 0, 0, 0x1E, 0x04);
  BlockSpec t = blk(2);
  t.set(0, 0, 0, 0, 0x0F, 0x50);
  mods.push_back(song({dep, t, blk(2)}, {0, 1, 0, 2, 2}));
  BlockSpec brk = blk(300);
  brk.set(150, 0, 0, 0, 0x0F, 0x00);
  mods.push_back(song({brk, blk(200)}, {0, 1}));
  std::vector<std::uint8_t> bad(40, 9);
  bad.push_back(0);
  mods.push_back(song({blk(2)}, bad));
  mods.push_back(test::mmd::sample_module(1));
  mods.push_back(test::mmd::sample_module(2));
  mods.push_back(test::mmd::sample_module(3));
  std::vector<BlockSpec> many(130, blk(1));
  std::vector<std::uint8_t> seq;
  for (unsigned i = 0; i < 130; ++i) seq.push_back(static_cast<std::uint8_t>(i));
  mods.push_back(song(many, seq));  // fails at the pattern limit: the failure is the same too

  for (unsigned k = 0; k < mods.size(); ++k) {
    for (int mode = 0; mode < 3; ++mode) {  // linear, structural, linear that truncates
      CAPTURE(k);
      CAPTURE(mode);
      FlowOptions opt;
      opt.truncate = mode == 2;
      Run once(mods[k], opt, mode == 1);
      NEED(once.open.ok());
      for (std::size_t budget : {std::size_t(1), std::size_t(5), std::size_t(64), std::size_t(5000)}) {
        CAPTURE(budget);
        FlowJob job;
        job.begin(once.rd, opt, storage2(), mode == 1);
        unsigned steps = 0;
        std::uint32_t before = 0;
        Progress p = Progress::Running;
        while (p == Progress::Running && steps < 2000000) {
          before = storage2().report.cells;
          p = job.step(budget);
          ++steps;
          // a step overruns its budget by at most the cost of one line (64 tracks x 17 pages) and 255 repeats
          CHECK(storage2().report.cells - before <= budget + 64u * 17u);
        }
        NEED(p != Progress::Running);
        CHECK(p == job.progress());
        CHECK(job.status().error == once.st.error);
        CHECK((p == Progress::Done) == once.st.ok());
        expect_same(once.out, storage2());
        if (budget == 1) {
          CHECK(steps >= once.rep().visits - once.rep().repeats);  // one tick per step: a step per line scanned (repeats are free)
        }
        if (budget == 5000 && once.rep().visits < 50 && once.rep().cells < 1000) {
          CHECK(steps == 1);                  // a small song is one step
        }
      }
    }
  }
  // a job that never began, and a job asked again after it ended
  FlowJob idle;
  CHECK(idle.progress() == Progress::Failed);
  CHECK(idle.step(10) == Progress::Failed);
  CHECK(idle.status().error == Error::Malformed);
}

TEST_CASE("med flow: looking for what plays next is capped too") {
  // 40 entries that play nothing, then a block: with a cap of 10 steps the search gives up
  std::vector<std::uint8_t> seq(40, 9);
  seq.push_back(0);
  FlowOptions small;
  small.max_visits = 10;
  Run r(song({blk(2)}, seq), small);
  NEED(r.open.ok());
  CHECK(r.st.error == Error::LimitExceeded);
  CHECK(r.rep().settle_steps == 11);
  Run ok(song({blk(2)}, seq));
  NEED(ok.st.ok());
  expect_patterns(ok.out, {{0, 0, 1}});
  CHECK(ok.rep().skipped_entries >= 40);
}

TEST_CASE("med flow: the header's speed is clamped into 1..32") {
  ModuleSpec m = song({blk(2)}, {0});
  m.tempo2 = 0;
  Run a(m);
  NEED(a.st.ok());
  CHECK(a.out.initial_spd == 1);
  CHECK(a.rep().header_clamped);
  m.tempo2 = 40;
  Run b(m);
  NEED(b.st.ok());
  CHECK(b.out.initial_spd == 32);
  CHECK(b.rep().header_clamped);
  m.tempo2 = 6;
  Run c(m);
  NEED(c.st.ok());
  CHECK(!c.rep().header_clamped);
}

TEST_CASE("med flow: the limits, and truncation on request") {
  // more patterns than the model holds
  std::vector<BlockSpec> blocks(130, blk(1));
  std::vector<std::uint8_t> seq;
  for (unsigned i = 0; i < 130; ++i) seq.push_back(static_cast<std::uint8_t>(i));
  Run r(song(blocks, seq));
  NEED(r.open.ok());
  CHECK(r.st.error == Error::LimitExceeded);
  CHECK(r.out.pattern_count == 128);
  CHECK(r.rep().stop_pos == 128);

  FlowOptions trunc;
  trunc.truncate = true;
  Run t(song(blocks, seq), trunc);
  NEED(t.st.ok());
  CHECK(t.rep().truncated);
  CHECK(t.out.pattern_count == 128);
  CHECK(t.out.order_count == 128);
  check_invariants(t, false);

  // more order entries: 256 plays of one block
  std::vector<std::uint8_t> many(256, 0);
  Run o(song({blk(1)}, many));
  NEED(o.open.ok());
  CHECK(o.st.error == Error::LimitExceeded);
  Run o255(song({blk(1)}, std::vector<std::uint8_t>(255, 0)));
  NEED(o255.st.ok());
  CHECK(o255.out.order_count == 255);
  CHECK(o255.rep().loop_found);
  Run ot(song({blk(1)}, many), trunc);
  NEED(ot.st.ok());
  CHECK(ot.rep().truncated);
  CHECK(ot.out.order_count == 255);

  // more line visits than the cap
  FlowOptions small;
  small.max_visits = 5;
  Run v(song({blk(8)}, {0}), small);
  NEED(v.open.ok());
  CHECK(v.st.error == Error::LimitExceeded);
  CHECK(v.rep().stop_line == 5);
  Run v2(song({blk(8)}, {0}));
  NEED(v2.st.ok());
  // a break back into the same block: (0,0), then (0,1), then (0,0) again is the loop
  BlockSpec a = blk(4);
  a.set(0, 0, 0, 0, 0x1D, 0x01);
  Run w(song({a}, {0}));
  NEED(w.st.ok());
  expect_patterns(w.out, {{0, 0, 0}, {0, 1, 3}});
  CHECK(w.rep().loop_found);
  CHECK(w.rep().loop_order == 0);
}

TEST_CASE("med flow: MMD2 sections, 0B within the section, play-sequence Stop and Jump") {
  // sections: [A, B] and [C, D]; C jumps to position 1 of its own section, which is D (flat position 3), not B
  BlockSpec c = blk(2);
  c.set(0, 0, 0, 0, 0x0B, 0x01);
  ModuleSpec m = song({blk(2), blk(2), c, blk(2)}, {}, 2);
  test::mmd::PlaySeqSpec p0, p1;
  p0.entries = {0, 1};
  p1.entries = {2, 3};
  m.pseqs = {p0, p1};
  m.sections = {0, 1};
  Run r(m);
  NEED(r.open.ok());
  NEED(r.st.ok());
  expect_patterns(r.out, {{0, 0, 1}, {1, 0, 1}, {2, 0, 0}, {3, 0, 1}});
  expect_order(r.out, {0, 1, 2, 3});
  CHECK(r.rep().jumps == 1);
  CHECK(r.rep().loop_found);
  CHECK(r.rep().loop_order == 0);
  check_invariants(r, true);

  // a play-sequence Stop at index 1
  ModuleSpec s = song({blk(2), blk(2)}, {}, 2);
  test::mmd::PlaySeqSpec q;
  q.entries = {0, 1};
  q.cmds.push_back({1, 1, {}});
  s.pseqs = {q};
  s.sections = {0};
  Run st(s);
  NEED(st.st.ok());
  expect_patterns(st.out, {{0, 0, 1}});
  CHECK(st.rep().ended);
  CHECK(!st.rep().loop_found);

  // a play-sequence Jump at index 1 to index 2 skips nothing but itself
  ModuleSpec j = song({blk(2), blk(2), blk(3)}, {}, 3);
  test::mmd::PlaySeqSpec u;
  u.entries = {0, 1, 2};
  u.cmds.push_back({1, 2, {0, 2}});
  j.pseqs = {u};
  j.sections = {0};
  Run jr(j);
  NEED(jr.st.ok());
  expect_patterns(jr.out, {{0, 0, 1}, {2, 0, 2}});
  expect_order(jr.out, {0, 1});
  CHECK(jr.rep().play_jumps == 1);
  CHECK(jr.rep().loop_found);

  // a Jump to a position past the section restarts the song (counted)
  u.cmds[0].extra = {0, 9};
  j.pseqs = {u};
  Run jo(j);
  NEED(jo.st.ok());
  CHECK(jo.rep().jump_out_of_range == 1);
}

TEST_CASE("med flow: command pages are read after the default page, track by track") {
  ModuleSpec m = song({blk(6), blk(2)}, {0, 1});
  m.blocks[0].page_cell(0, 2, 1, 0x0F, 0x00);   // a break from an extra page
  Run r(m);
  NEED(r.st.ok());
  expect_patterns(r.out, {{0, 0, 2}, {1, 0, 1}});
  CHECK(r.rep().breaks == 1);
  // two competing breaks: the later page wins
  ModuleSpec m2 = song({blk(6), blk(4)}, {0, 1});
  m2.blocks[0].page_cell(0, 2, 0, 0x1D, 0x01).page_cell(1, 2, 0, 0x1D, 0x03);
  Run c(m2);
  NEED(c.st.ok());
  expect_patterns(c.out, {{0, 0, 2}, {1, 3, 3}});
}

TEST_CASE("med flow: the structural fallback keeps blocks and the play list, and counts what it drops") {
  BlockSpec a = blk(8);
  a.set(2, 0, 0, 0, 0x0F, 0x00);
  a.set(3, 0, 0, 0, 0x0B, 0x00);
  a.set(4, 0, 0, 0, 0x16, 0x00);
  a.set(5, 0, 0, 0, 0x16, 0x02);
  a.set(6, 0, 0, 0, 0x1E, 0x01);
  a.set(7, 0, 0, 0, 0x0F, 0xFE);
  a.set(1, 1, 0, 0, 0x1D, 0x03);
  Run r(song({a, blk(4)}, {0, 1, 0}), FlowOptions(), true);
  NEED(r.st.ok());
  expect_patterns(r.out, {{0, 0, 7}, {1, 0, 3}});
  expect_order(r.out, {0, 1, 0});
  CHECK(r.rep().structural);
  CHECK(r.rep().break_cells == 2);
  CHECK(r.rep().dropped_jumps == 1);
  CHECK(r.rep().dropped_loops == 2);
  CHECK(r.rep().dropped_repeats == 1);
  CHECK(r.rep().end_cells == 1);
  CHECK(r.rep().runs == 3);
  check_invariants(r, true);

  // a long block is cut into pieces, the order lists them in turn
  Run big(song({blk(300), blk(3)}, {0, 1, 0}), FlowOptions(), true);
  NEED(big.st.ok());
  expect_patterns(big.out, {{0, 0, 127}, {0, 128, 255}, {0, 256, 299}, {1, 0, 2}});
  expect_order(big.out, {0, 1, 2, 3, 0, 1, 2});

  // it fits where the linearisation does not: 130 blocks of 1 line need 130 patterns either way, but a loop that unrolls does not
  BlockSpec l = blk(4);
  l.set(1, 0, 0, 0, 0x16, 0x00);
  l.set(2, 0, 0, 0, 0x16, 0x0F);
  std::vector<BlockSpec> blocks(20, l);
  std::vector<std::uint8_t> seq;
  for (unsigned i = 0; i < 20; ++i) seq.push_back(static_cast<std::uint8_t>(i));
  Run lin(song(blocks, seq));
  NEED(lin.open.ok());
  CHECK(lin.st.error == Error::LimitExceeded);   // 20 x 16 order entries
  Run str(song(blocks, seq), FlowOptions(), true);
  NEED(str.st.ok());
  CHECK(str.out.pattern_count == 20);
  CHECK(str.out.order_count == 20);

  // a Stop and a Jump of a play sequence in the fallback
  ModuleSpec m = song({blk(2), blk(2), blk(2)}, {}, 2);
  test::mmd::PlaySeqSpec p;
  p.entries = {0, 1, 2};
  p.cmds.push_back({1, 2, {0, 0}});
  p.cmds.push_back({2, 1, {}});
  m.pseqs = {p};
  m.sections = {0};
  Run sm(m, FlowOptions(), true);
  NEED(sm.st.ok());
  expect_patterns(sm.out, {{0, 0, 1}});
  CHECK(sm.rep().dropped_jumps == 1);
  CHECK(sm.rep().ended);
}

TEST_CASE("med flow: a random song always ends in a result that satisfies the invariants") {
  test::Rng rng(0xF10Eu);
  static const unsigned cmds[] = {0x09, 0x0B, 0x0F, 0x16, 0x1D, 0x1E, 0x0C, 0x04};
  unsigned loops = 0, ended = 0, failed = 0, truncated = 0;
#if defined(__arm__)
  const unsigned runs = 150;
#else
  const unsigned runs = 1500;
#endif
  for (unsigned i = 0; i < runs; ++i) {
    const unsigned version = rng.below(3);
    std::vector<BlockSpec> blocks;
    const unsigned nb = 1 + rng.below(4);
    for (unsigned b = 0; b < nb; ++b) {
      BlockSpec bs = blk(1 + rng.below(20), 1 + rng.below(3));
      const unsigned ncmd = rng.below(6);
      for (unsigned k = 0; k < ncmd; ++k) {
        const unsigned cmd = version == 0 ? (cmds[rng.below(8)] & 0x0F) : cmds[rng.below(8)];
        unsigned data = rng.below(3) == 0 ? rng.below(256) : rng.below(8);
        if ((cmd & 0x0F) == 0x0F && rng.below(8) == 0) data = 0xFE;  // 0FFE ends the song
        bs.set(rng.below(bs.lines), rng.below(bs.tracks), 0, 0, cmd, data);
      }
      blocks.push_back(bs);
    }
    std::vector<std::uint8_t> seq;
    const unsigned ns = rng.below(7);
    for (unsigned k = 0; k < ns; ++k) seq.push_back(static_cast<std::uint8_t>(rng.below(nb + 1)));
    ModuleSpec m = song(blocks, seq, version);
    m.flags2 = static_cast<std::uint8_t>(rng.below(2) != 0 ? 0x20 | rng.below(32) : 0);
    m.deftempo = static_cast<std::uint16_t>(rng.below(300));
    m.tempo2 = static_cast<std::uint8_t>(rng.below(40));
    FlowOptions opt;
    opt.max_visits = rng.below(4) == 0 ? 40 : 3000;  // the small cap makes the failure path common
    opt.truncate = rng.below(4) == 0;
    Run r(m, opt);
    NEED(r.open.ok());
    CAPTURE(i);
    if (r.st.ok()) {
      check_invariants(r, !r.rep().truncated);
      loops += r.rep().loop_found ? 1 : 0;
      ended += r.rep().ended ? 1 : 0;
      truncated += r.rep().truncated ? 1 : 0;
    } else {
      CHECK(r.st.error == Error::LimitExceeded);
      ++failed;
    }
    Run s(m, opt, true);
    NEED(s.open.ok());
    if (s.st.ok()) {
      check_invariants(s, false);
    } else {
      CHECK(s.st.error == Error::LimitExceeded);
    }
  }
  // the random songs must have exercised more than the trivial path
  CHECK(loops > runs / 10);
  CHECK(ended > 0);
  CHECK(failed > 0);
  CHECK(truncated + failed < runs);
}
