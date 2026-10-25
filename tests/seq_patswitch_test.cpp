// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-PATSWITCH (docs/specs/OS-PATSWITCH.md), the sequencer's side, on the host and on the Cortex-M4F (`qemu_seq_model`):
//   * switch_pattern() while playing: the playing pattern becomes the chosen one at the next step boundary, once, on the same row (the next row of the old pattern, 0 at its end, modulo the new
//     pattern's running length), with the grid, the swing pair and the Step events untouched; through the command ring and by a direct call;
//   * the newest intent wins (a queued pattern, a live jump, a requested mode, a locate), the pattern that plays withdraws a switch that waits, a Song is left;
//   * nothing waits across a stop, a restart or a resume ([Stop, Switch] in one drain, [Switch, Resume]); ignored when stopped, in a count-in and for an id outside 1..255;
//   * a pattern the project lacks plays as empty at the default length and the transport keeps running, for a switch and for a queued pattern (the PATS pads);
//   * the OFF rows the switch skips: MIDI notes get their off, sampler notes their release (not a cut) unless the new row starts a note of its own, at the sample of the landing;
//   * the swing pair of an even-to-even switch, a Break pattern of the pattern left, the Perform length, the independence of the block size, the log word.
#include <doctest/doctest.h>

#include <o3os/seq/sequencer.h>

#include <cstdint>

#include "support/need.h"
#include "support/seq_fixture.h"
#include "support/static_slot.h"

using namespace o3os;
using namespace o3os::test;
using audio::EventKind;
namespace fx = o3os::seq::fx;

namespace {

StaticSlot<seq::Sequencer>& g_slot = seq_slot();
Collector& g_col = seq_collector();

constexpr std::uint32_t kBpm = 12000;

seq::Sequencer& make() {
  seq::Sequencer& s = g_slot.make(seq_project());
  g_col.n = 0;
  g_col.total_events = 0;
  g_col.hash = 0xCBF29CE484222325ull;
  return s;
}

std::uint64_t exact_sample(std::uint64_t ticks, std::uint32_t bpm_x100) {
  const std::uint64_t n = ticks * core::kCpuHz * core::kRefCyclesPerSample.den * 125u;
  const std::uint64_t d = 4u * core::kRefCyclesPerSample.num * static_cast<std::uint64_t>(bpm_x100);
  return (2u * n + d) / (2u * d);
}
// The first sample of step n of a run from sample 0 (straight), and a sample inside step n (20 microsteps in).
std::uint64_t at_step(std::uint32_t n) { return exact_sample(48u * n, kBpm); }
std::uint64_t in_step(std::uint32_t n) { return exact_sample(48u * n + 20u, kBpm); }

// A project of 6 active tracks and 4 sample instruments (slots 0..3; slot 10 is the MIDI one, named 11 in a step) at 120 BPM.
void fresh() {
  fresh_project(6, 4);
  CHECK(seq_project().set_tempo_x100(kBpm) == model::Status::Ok);
}

// Pattern `id` of `length` steps whose track 0 plays the note `base + row` on every row (a NoteOn names the row and the pattern it came from).
void rows_pattern(std::uint32_t id, std::uint32_t length, std::uint32_t base) {
  model::Project& p = seq_project();
  CHECK(p.create_pattern(id, length) == model::Status::Ok);
  for (std::uint32_t r = 0; r < length; ++r) {
    CHECK(p.set_step(id, 0, r, make_step(static_cast<std::uint8_t>(base + r), 1)) == model::Status::Ok);
  }
}

void run(seq::Sequencer& s, std::uint64_t from, std::uint64_t to) { g_col.run(s, from, to, 64); }

void send_switch(seq::Sequencer& s, std::uint32_t id, bool via_ring) {
  if (via_ring) {
    CHECK(s.commands().push(seq::Command{seq::Command::Kind::SwitchPattern, static_cast<std::uint8_t>(id), 0, 0}));
  } else {
    s.switch_pattern(id);
  }
}

void send_locate(seq::Sequencer& s, std::uint32_t row) {
  CHECK(s.commands().push(seq::Command{seq::Command::Kind::LocateRow, 0, 0, row}));
}
void send_queue(seq::Sequencer& s, std::uint32_t id) {
  CHECK(s.commands().push(seq::Command{seq::Command::Kind::QueuePattern, static_cast<std::uint8_t>(id), 0, 0}));
}

// The NoteOn of track 0 that step `n` of the run started, or null.
const Rec* note_of_step(std::uint32_t n) { return g_col.nth(EventKind::NoteOn, n, 0); }

// Events of a kind at a time on a track.
std::uint32_t count_at(EventKind k, std::uint8_t track, std::uint64_t t) {
  std::uint32_t c = 0;
  for (std::uint32_t i = 0; i < g_col.n; ++i) {
    if (g_col.rec[i].ev.kind == k && g_col.rec[i].ev.track == track && g_col.rec[i].t == t) {
      ++c;
    }
  }
  return c;
}

}  // namespace

TEST_CASE("patswitch seq: the playing pattern changes at the next step boundary, on the same row, once, with the grid untouched (ring and direct)") {
  for (std::uint32_t via_ring = 0; via_ring < 2; ++via_ring) {
    fresh();
    rows_pattern(1, 16, 0);
    rows_pattern(2, 16, 64);
    seq::Sequencer& s = make();
    s.play_pattern(1);
    run(s, 0, in_step(3));  // inside step 3
    send_switch(s, 2, via_ring != 0u);
    run(s, in_step(3), at_step(40));
    // Rows 0..3 of pattern 1, then rows 4.. of pattern 2 (the row after the running one, never 3 twice, never 4 skipped), on the untouched grid.
    for (std::uint32_t n = 0; n < 40; ++n) {
      const Rec* on = note_of_step(n);
      NEED(on != nullptr);
      const std::uint32_t want = n < 4u ? n : 64u + n % 16u;
      CHECK_MESSAGE(on->ev.a == want, "ring " << via_ring << " step " << n);
      CHECK_MESSAGE(on->t == at_step(n), "ring " << via_ring << " step " << n);
    }
    const Rec* st = g_col.nth(EventKind::Step, 4);
    NEED(st != nullptr);
    CHECK(st->ev.a == 2);
    CHECK(st->ev.value == 4);
    CHECK(st->t == at_step(4));
    // One Pattern event for the landing, at the boundary, naming the pattern and its length; the pattern then loops by itself (no second landing).
    const Rec* pat = g_col.nth(EventKind::Pattern, 0);
    NEED(pat != nullptr);
    CHECK(pat->t == at_step(4));
    CHECK(pat->ev.a == 2);
    CHECK(pat->ev.value == 16);
    CHECK(s.last_landing() == ((1u << 16) | (2u << 8) | 4u));
    CHECK(s.position().pattern_id == 2);
    CHECK(s.playing());
  }
}

TEST_CASE("patswitch seq: the row is the next row of the old pattern, 0 at its end, modulo the new pattern's length") {
  struct Row {
    std::uint32_t old_len, new_len, during, want;  // the command is sent inside step `during`; `want` is the row that plays at the landing
  };
  const Row table[] = {
      {16, 8, 11, 4},    // next row 12, 12 % 8
      {8, 16, 4, 5},     // next row 5 is inside the longer pattern
      {16, 16, 6, 7},    // equal lengths keep the row
      {16, 16, 15, 0},   // the old pattern ends at this boundary: the new one starts at its top
      {16, 32, 15, 0},
      {64, 16, 39, 8},   // next row 40, 40 % 16
      {32, 16, 20, 5},   // 21 % 16
  };
  for (const Row& r : table) {
    fresh();
    rows_pattern(1, r.old_len, 0);
    rows_pattern(2, r.new_len, 64);
    seq::Sequencer& s = make();
    s.play_pattern(1);
    run(s, 0, in_step(r.during));
    send_switch(s, 2, true);
    run(s, in_step(r.during), at_step(r.during + 4u));
    const Rec* on = note_of_step(r.during + 1u);
    NEED(on != nullptr);
    CHECK_MESSAGE(on->ev.a == 64u + r.want, r.old_len << " to " << r.new_len << " during " << r.during);
    CHECK_MESSAGE(on->t == at_step(r.during + 1u), r.old_len << " to " << r.new_len << " during " << r.during);
    // and it goes on from there, by the new pattern's length
    const Rec* next = note_of_step(r.during + 2u);
    NEED(next != nullptr);
    CHECK_MESSAGE(next->ev.a == 64u + (r.want + 1u) % r.new_len, r.old_len << " to " << r.new_len);
  }
}

TEST_CASE("patswitch seq: the Perform pattern length is the running length of the row rule") {
  for (std::uint32_t during = 3; during <= 7; during += 4) {
    fresh();
    rows_pattern(1, 16, 0);
    rows_pattern(2, 16, 64);
    seq::Sequencer& s = make();
    s.set_perf_pattern_length(8);
    s.play_pattern(1);
    run(s, 0, in_step(during));
    send_switch(s, 2, true);
    run(s, in_step(during), at_step(during + 3u));
    const Rec* on = note_of_step(during + 1u);
    NEED(on != nullptr);
    CHECK_MESSAGE(on->ev.a == 64u + (during == 3u ? 4u : 0u), during);  // 4 inside the Perform length; step 7 ends it (8 of 8): row 0
  }
}

TEST_CASE("patswitch seq: a Song is left, the chosen pattern loops, the song does not come back and nothing says SongEnd") {
  fresh();
  rows_pattern(1, 16, 0);
  rows_pattern(2, 16, 32);
  rows_pattern(3, 8, 64);
  CHECK(seq_project().append_order(1) == model::Status::Ok);
  CHECK(seq_project().append_order(2) == model::Status::Ok);
  seq::Sequencer& s = make();
  s.play_song(0);
  run(s, 0, in_step(5));
  CHECK(s.position().mode == seq::PlayMode::Song);
  send_switch(s, 3, true);
  run(s, in_step(5), at_step(60));
  CHECK(s.position().mode == seq::PlayMode::Pattern);
  CHECK(s.position().order_pos == audio::PlayPosition::kNoOrder);
  CHECK(s.position().pattern_id == 3);
  CHECK(g_col.count(EventKind::SongEnd) == 0);
  // Rows 0..5 of the song's pattern 1, then pattern 3 from row 6 and by its own 8 steps, for ever.
  CHECK(note_of_step(6)->ev.a == 64u + 6u);
  CHECK(note_of_step(7)->ev.a == 64u + 7u);
  CHECK(note_of_step(8)->ev.a == 64u);
  CHECK(note_of_step(59)->ev.a == 64u + (59u - 6u + 6u) % 8u);
  const Rec* late = g_col.nth(EventKind::Step, 50);
  NEED(late != nullptr);
  CHECK(late->ev.a == 3);
  CHECK(audio::event_order_pos(late->ev) == audio::PlayPosition::kNoOrder);
  // The pattern the song is on may be picked as well: it leaves the song and loops (the playing pad of the Song screen).
  fresh();
  rows_pattern(1, 16, 0);
  rows_pattern(2, 16, 32);
  CHECK(seq_project().append_order(1) == model::Status::Ok);
  CHECK(seq_project().append_order(2) == model::Status::Ok);
  seq::Sequencer& t = make();
  t.play_song(0);
  run(t, 0, in_step(5));
  send_switch(t, 1, false);
  run(t, in_step(5), at_step(40));
  CHECK(t.position().mode == seq::PlayMode::Pattern);
  CHECK(g_col.nth(EventKind::Step, 39)->ev.a == 1);  // the song would have been on pattern 2 by now
}

TEST_CASE("patswitch seq: the newest intent wins: a queued pattern, a live jump, a requested mode and a locate give way; a queue made after it stays") {
  // a queued pattern (PATS): dropped
  {
    fresh();
    rows_pattern(1, 16, 0);
    rows_pattern(2, 16, 32);
    rows_pattern(5, 16, 64);
    seq::Sequencer& s = make();
    s.play_pattern(1);
    run(s, 0, in_step(2));
    s.queue_pattern(5);
    send_switch(s, 2, true);
    run(s, in_step(2), at_step(40));
    CHECK(note_of_step(20)->ev.a == 32u + 4u);  // pattern 2 loops; the queue did not come back at its end
    CHECK(s.position().pattern_id == 2);
  }
  // a live jump of the Song screen, and a requested mode: dropped
  {
    fresh();
    rows_pattern(1, 16, 0);
    rows_pattern(2, 16, 32);
    rows_pattern(3, 16, 64);
    CHECK(seq_project().append_order(1) == model::Status::Ok);
    CHECK(seq_project().append_order(3) == model::Status::Ok);
    seq::Sequencer& s = make();
    s.play_song(0);
    run(s, 0, in_step(2));
    s.queue_jump(1);
    s.request_mode(seq::PlayMode::Song);
    send_switch(s, 2, true);
    run(s, in_step(2), at_step(40));
    CHECK(s.position().mode == seq::PlayMode::Pattern);
    CHECK(s.position().pattern_id == 2);
    CHECK(s.play_position().read().queued == audio::PlayPosition::kNoOrder);  // the Song slot stops blinking
    CHECK(g_col.nth(EventKind::Step, 39)->ev.a == 2);
  }
  // a pattern queued AFTER the switch is the newer intent: it lands at the end of the new pattern
  {
    fresh();
    rows_pattern(1, 16, 0);
    rows_pattern(2, 8, 32);
    rows_pattern(5, 16, 64);
    seq::Sequencer& s = make();
    s.play_pattern(1);
    run(s, 0, in_step(2));
    send_switch(s, 2, true);
    send_queue(s, 5);  // (the ring keeps the order of the asks)
    run(s, in_step(2), at_step(30));
    CHECK(g_col.nth(EventKind::Step, 3)->ev.a == 2);   // the landing
    CHECK(g_col.nth(EventKind::Step, 8)->ev.a == 5);   // pattern 2 (8 steps, from row 3) ended at step 8; the queue starts 5
    CHECK(g_col.nth(EventKind::Step, 8)->ev.value == 0);
  }
  // a locate that waits is dropped by the switch; a locate after the switch drops it
  for (std::uint32_t switch_last = 0; switch_last < 2; ++switch_last) {
    fresh();
    rows_pattern(1, 32, 0);
    rows_pattern(2, 32, 64);
    seq::Sequencer& s = make();
    s.play_pattern(1);
    run(s, 0, in_step(2));
    if (switch_last != 0u) {
      send_locate(s, 20);
      send_switch(s, 2, true);
    } else {
      send_switch(s, 2, true);
      send_locate(s, 20);
    }
    run(s, in_step(2), at_step(8));
    CHECK_MESSAGE(note_of_step(3)->ev.a == (switch_last != 0u ? 64u + 3u : 20u), switch_last);
    CHECK_MESSAGE(note_of_step(4)->ev.a == (switch_last != 0u ? 64u + 4u : 21u), switch_last);
    CHECK_MESSAGE(s.position().pattern_id == (switch_last != 0u ? 2 : 1), switch_last);
  }
}

TEST_CASE("patswitch seq: the pattern that plays withdraws a switch that waits (a detent forth and back); in the same drain too") {
  for (std::uint32_t same_drain = 0; same_drain < 2; ++same_drain) {
    fresh();
    rows_pattern(1, 16, 0);
    rows_pattern(2, 16, 64);
    seq::Sequencer& s = make();
    s.play_pattern(1);
    run(s, 0, in_step(3));
    send_switch(s, 2, true);
    if (same_drain == 0u) {
      run(s, in_step(3), in_step(3) + 100u);  // taken by the sequencer, still waiting for the boundary
    }
    send_switch(s, 1, true);
    run(s, (same_drain == 0u ? in_step(3) + 100u : in_step(3)), at_step(20));
    for (std::uint32_t n = 0; n < 20; ++n) {
      CHECK_MESSAGE(note_of_step(n)->ev.a == n % 16u, "drain " << same_drain << " step " << n);  // pattern 1 the whole time
    }
    CHECK(s.last_landing() == 0u);  // no landing (pattern 1's own end at step 16 is a Pattern event too)
    CHECK(s.position().pattern_id == 1);
  }
}

TEST_CASE("patswitch seq: ignored when stopped, during a count-in and for an id outside 1..255; 255 is a pattern like another") {
  fresh();
  rows_pattern(1, 16, 0);
  rows_pattern(2, 16, 64);
  seq::Sequencer& s = make();
  s.switch_pattern(2);  // stopped: nothing is left behind for the start
  s.play_pattern(1);
  run(s, 0, at_step(5));
  CHECK(note_of_step(0)->ev.a == 0u);
  CHECK(note_of_step(4)->ev.a == 4u);
  s.switch_pattern(0);
  s.switch_pattern(256);
  s.switch_pattern(1000);
  run(s, at_step(5), at_step(8));
  CHECK(note_of_step(7)->ev.a == 7u);
  CHECK(g_col.count(EventKind::Pattern) == 0);
  // the count-in: the line waits on its start point and the switch is dropped
  seq::Sequencer& c = make();
  c.play_pattern(1, 0, 1);
  run(c, 0, at_step(5));
  c.switch_pattern(2);
  run(c, at_step(5), at_step(16 + 6));
  CHECK(note_of_step(0)->ev.a == 0u);  // pattern 1, row 0, first after the count-in
  CHECK(note_of_step(5)->ev.a == 5u);
  CHECK(g_col.count(EventKind::Pattern) == 0);
}

TEST_CASE("patswitch seq: nothing waits across a stop, a restart or a resume: [Stop, Switch] in one drain, [Switch, Play], [Switch, Resume]") {
  // [Stop, Switch]
  {
    fresh();
    rows_pattern(1, 16, 0);
    rows_pattern(2, 16, 64);
    seq::Sequencer& s = make();
    s.play_pattern(1);
    run(s, 0, in_step(3));
    CHECK(s.commands().push(seq::Command{seq::Command::Kind::Stop, 0, 0, 0}));
    send_switch(s, 2, true);  // the stop is taken at the block start; the switch was asked in the same drain
    run(s, in_step(3), in_step(3) + 200u);
    CHECK_FALSE(s.playing());
    s.play_pattern(1);
    const std::uint64_t t0 = in_step(3) + 200u;
    run(s, t0, t0 + at_step(6));
    CHECK(s.position().pattern_id == 1);
    CHECK(s.last_landing() == 0u);
  }
  // [Switch, Play] (a start while playing restarts)
  {
    fresh();
    rows_pattern(1, 16, 0);
    rows_pattern(2, 16, 64);
    seq::Sequencer& s = make();
    s.play_pattern(1);
    run(s, 0, in_step(3));
    send_switch(s, 2, true);
    s.play_pattern(1);
    run(s, in_step(3), in_step(3) + at_step(8));
    CHECK(s.position().pattern_id == 1);
    CHECK(s.last_landing() == 0u);
  }
  // [Switch, Resume]: the resume goes on where the run is, on the pattern it is playing; the switch does not ride on the resume (it is not a located row)
  {
    fresh();
    rows_pattern(1, 16, 0);
    rows_pattern(2, 16, 64);
    seq::Sequencer& s = make();
    s.play_pattern(1);
    run(s, 0, in_step(3));
    send_switch(s, 2, true);
    CHECK(s.commands().push(seq::Command{seq::Command::Kind::Resume, 0, 0, 0}));
    run(s, in_step(3), in_step(3) + at_step(8));
    CHECK(s.last_landing() == 0u);
    CHECK(s.position().pattern_id == 1);
    CHECK(s.position().mode == seq::PlayMode::Pattern);
    CHECK(s.playing());
  }
}

TEST_CASE("patswitch seq: a pattern the project lacks plays as empty at the default length and the transport runs on, for the switch and for the queue (PATS)") {
  for (std::uint32_t via_queue = 0; via_queue < 2; ++via_queue) {
    fresh();
    rows_pattern(1, 16, 0);
    const std::uint32_t len = seq_project().default_pattern_length();
    NEED(len >= 16u);
    NEED(seq_project().pattern(9) == nullptr);
    seq::Sequencer& s = make();
    s.play_pattern(1);
    run(s, 0, in_step(2));
    if (via_queue != 0u) {
      s.queue_pattern(9);  // taken at the end of pattern 1 (step 16)
    } else {
      send_switch(s, 9, true);
    }
    const std::uint32_t landing = via_queue != 0u ? 16u : 3u;
    const std::uint32_t total = landing + 2u * len + 3u;
    run(s, in_step(2), at_step(total));
    CHECK_MESSAGE(s.playing(), via_queue);
    CHECK_MESSAGE(g_col.count(EventKind::Stopped) == 0, via_queue);
    CHECK_MESSAGE(s.position().pattern_id == 9, via_queue);
    // a Step event at every step, on the grid, whose row advances through the empty pattern and wraps at its default length
    for (std::uint32_t n = landing; n < total; ++n) {
      const Rec* st = g_col.nth(EventKind::Step, n);
      NEED(st != nullptr);
      CHECK_MESSAGE(st->t == at_step(n), via_queue << " step " << n);
      CHECK_MESSAGE(st->ev.a == 9, via_queue << " step " << n);
      const std::uint32_t row = via_queue != 0u ? (n - landing) % len : (n - landing + 3u) % len;  // the switch keeps the row (3 of the old pattern, % len); the queue starts at 0
      CHECK_MESSAGE(static_cast<std::uint32_t>(st->ev.value) == row, via_queue << " step " << n);
    }
    CHECK(g_col.count(EventKind::NoteOn) == (via_queue != 0u ? 16u : 3u));  // nothing but pattern 1's notes: the empty pattern plays nothing
    CHECK(s.stats().rows_missing > 0u);                                      // counted, not an error
  }
}

TEST_CASE("patswitch seq: the notes the skipped OFF rows would have ended end at the landing: MIDI off, sampler release (not a cut), unless the new row starts a note") {
  fresh();
  model::Project& p = seq_project();
  CHECK(p.create_pattern(1, 16) == model::Status::Ok);
  CHECK(p.create_pattern(2, 16) == model::Status::Ok);
  // Pattern 1: tracks 0, 2, 3 sampler notes, 1 and 4 MIDI notes, all on row 0, all with an OFF on row 8 (which a switch at row 4 skips); track 5 has no note.
  for (std::uint32_t t = 0; t < 5; ++t) {
    const std::uint8_t instr = (t == 1u || t == 4u) ? 11 : static_cast<std::uint8_t>(1 + t % 3u);
    CHECK(p.set_step(1, t, 0, make_step(static_cast<std::uint8_t>(60 + t), instr)) == model::Status::Ok);
    CHECK(p.set_step(1, t, 8, special_step(model::kNoteOff, instr)) == model::Status::Ok);
  }
  // Pattern 2 at the landing row 4: track 2 starts a sampler note, track 3 has an OFF, track 4 starts a MIDI note; tracks 0 and 1 are empty.
  CHECK(p.set_step(2, 2, 4, make_step(70, 3)) == model::Status::Ok);
  CHECK(p.set_step(2, 3, 4, special_step(model::kNoteOff, 1)) == model::Status::Ok);
  CHECK(p.set_step(2, 4, 4, make_step(72, 11)) == model::Status::Ok);
  seq::Sequencer& s = make();
  s.play_pattern(1);
  run(s, 0, in_step(3));
  send_switch(s, 2, true);
  run(s, in_step(3), at_step(40));
  const std::uint64_t land = at_step(4);
  // track 0: a sampler note, an empty row: released at the landing, once, and not cut
  CHECK(count_at(EventKind::NoteOff, 0, land) == 1u);
  CHECK(g_col.count(EventKind::NoteOff, 0) == 1u);  // the OFF row 8 of pattern 1 never plays: no second one, now or later
  // track 1: a MIDI note, an empty row: its off at the landing, once
  CHECK(count_at(EventKind::MidiNoteOff, 1, land) == 1u);
  CHECK(g_col.count(EventKind::MidiNoteOff, 1) == 1u);
  const Rec* off1 = g_col.nth(EventKind::MidiNoteOff, 0, 1);
  NEED(off1 != nullptr);
  CHECK(off1->ev.a == 61);  // the note it owed
  // track 2: its new row starts a note: no release (the new note replaces the voice), the NoteOn at the landing
  CHECK(count_at(EventKind::NoteOff, 2, land) == 0u);
  CHECK(count_at(EventKind::NoteOn, 2, land) == 1u);
  // track 3: an OFF row at the landing: ended once by the row itself
  CHECK(count_at(EventKind::NoteOff, 3, land) == 1u);
  // track 4: a MIDI note replaces a MIDI note: the off first, then the on, at the same sample
  CHECK(count_at(EventKind::MidiNoteOff, 4, land) == 1u);
  CHECK(count_at(EventKind::MidiNoteOn, 4, land) == 1u);
  std::uint32_t i_off = 0, i_on = 0;
  for (std::uint32_t i = 0; i < g_col.n; ++i) {
    if (g_col.rec[i].t == land && g_col.rec[i].ev.track == 4) {
      if (g_col.rec[i].ev.kind == EventKind::MidiNoteOff) {
        i_off = i;
      }
      if (g_col.rec[i].ev.kind == EventKind::MidiNoteOn) {
        i_on = i;
      }
    }
  }
  CHECK(i_off < i_on);
  // nothing was cut or faded, and the release came at the landing and not before it
  CHECK(g_col.count(EventKind::NoteCut) == 0u);
  CHECK(g_col.count(EventKind::NoteFade) == 0u);
  CHECK(g_col.nth(EventKind::NoteOff, 0, 0)->t == land);
}

// OS-BUG-PASTEHANG: a whole paste over the pattern that plays takes the OFF rows of what it played with it, so the notes that sound would hang until Stop. release_pattern() ends them at the next step.
TEST_CASE("patswitch seq: release_pattern ends the notes of a pattern rewritten under the run at the next step (MIDI off, sampler release), unless the row starts a note; other patterns and a stop owe nothing") {
  for (std::uint32_t mode = 0; mode < 4; ++mode) {  // 0: command (ring), 1: direct call, 2: no release (the old hang), 3: the release names another pattern
    fresh();
    model::Project& p = seq_project();
    CHECK(p.create_pattern(1, 16) == model::Status::Ok);
    CHECK(p.create_pattern(2, 16) == model::Status::Ok);
    // Track 0 a sampler note, tracks 1 and 2 MIDI notes, all on row 0 and none with an OFF row: they sound until something ends them.
    CHECK(p.set_step(1, 0, 0, make_step(60, 1)) == model::Status::Ok);
    CHECK(p.set_step(1, 1, 0, make_step(61, 11)) == model::Status::Ok);
    CHECK(p.set_step(1, 2, 0, make_step(62, 11)) == model::Status::Ok);
    seq::Sequencer& s = make();
    s.play_pattern(1);
    run(s, 0, in_step(1));  // inside step 1: the notes sound
    // The paste: the pattern's three tracks are empty now, but track 2 starts a new note on row 2.
    for (std::uint32_t t = 0; t < 3; ++t) {
      CHECK(p.set_step(1, t, 0, model::kEmptyStep) == model::Status::Ok);
    }
    CHECK(p.set_step(1, 2, 2, make_step(70, 11)) == model::Status::Ok);
    if (mode == 0u) {
      CHECK(s.commands().push(seq::Command{seq::Command::Kind::ReleasePattern, 1, 0, 0}));
    } else if (mode == 1u) {
      s.release_pattern(1);
    } else if (mode == 3u) {
      s.release_pattern(2);
    }
    run(s, in_step(1), at_step(12));
    const std::uint64_t at = at_step(2);  // the boundary after the step the command came in
    if (mode <= 1u) {
      CHECK_MESSAGE(count_at(EventKind::NoteOff, 0, at) == 1u, mode);       // the sampler note: released, not cut
      CHECK_MESSAGE(g_col.count(EventKind::NoteOff, 0) == 1u, mode);
      CHECK_MESSAGE(count_at(EventKind::MidiNoteOff, 1, at) == 1u, mode);   // the MIDI note of the emptied track: its off
      CHECK_MESSAGE(g_col.count(EventKind::MidiNoteOff, 1) == 1u, mode);
      const Rec* off1 = g_col.nth(EventKind::MidiNoteOff, 0, 1);
      CHECK_MESSAGE(off1 != nullptr, mode);
      CHECK_MESSAGE((off1 != nullptr ? off1->ev.a : 0u) == 61u, mode);
      CHECK_MESSAGE(count_at(EventKind::MidiNoteOff, 2, at) == 1u, mode);   // track 2 starts a note on this row: the off, then the on, at the same sample
      CHECK_MESSAGE(count_at(EventKind::MidiNoteOn, 2, at) == 1u, mode);
      CHECK_MESSAGE(g_col.count(EventKind::MidiNoteOff, 2) == 1u, mode);
      CHECK_MESSAGE(g_col.count(EventKind::NoteCut) == 0u, mode);
    } else {
      // Without the release (mode 2) or for another pattern (mode 3) the old notes hang: this is the defect.
      CHECK_MESSAGE(g_col.count(EventKind::NoteOff, 0) == 0u, mode);
      CHECK_MESSAGE(g_col.count(EventKind::MidiNoteOff, 1) == 0u, mode);
    }
    // The release is once: a later step releases nothing more, and a stop sends no second off for a note that was ended.
    const std::uint32_t offs = g_col.count(EventKind::MidiNoteOff, 1);
    run(s, at_step(12), at_step(20));
    CHECK_MESSAGE(g_col.count(EventKind::MidiNoteOff, 1) == offs, mode);
    CHECK_MESSAGE(g_col.count(EventKind::NoteOff, 0) == (mode <= 1u ? 1u : 0u), mode);
  }
}

TEST_CASE("patswitch seq: release_pattern ignores an id outside 1..255; two ids before one boundary release every track, the same id twice only its tracks") {
  struct Case {
    std::uint32_t first, second;
    bool released;  // the note of pattern 1's track 0 is ended at the next boundary
  };
  const Case table[] = {{0, 0, false}, {256, 256, false}, {2, 2, false}, {1, 1, true}, {2, 1, true}, {1, 2, true}};
  for (const Case& c : table) {
    fresh();
    model::Project& p = seq_project();
    CHECK(p.create_pattern(1, 16) == model::Status::Ok);
    CHECK(p.create_pattern(2, 16) == model::Status::Ok);
    CHECK(p.set_step(1, 0, 0, make_step(60, 1)) == model::Status::Ok);
    seq::Sequencer& s = make();
    s.play_pattern(1);
    run(s, 0, in_step(1));
    CHECK(p.set_step(1, 0, 0, model::kEmptyStep) == model::Status::Ok);
    s.release_pattern(c.first);
    s.release_pattern(c.second);
    run(s, in_step(1), at_step(6));
    CHECK_MESSAGE(g_col.count(EventKind::NoteOff, 0) == (c.released ? 1u : 0u), c.first << ", " << c.second);
  }
}

TEST_CASE("patswitch seq: a Roll or an arpeggio of the pattern left ends with the release; a track that kept no note owes nothing") {
  fresh();
  model::Project& p = seq_project();
  CHECK(p.create_pattern(1, 16) == model::Status::Ok);
  CHECK(p.create_pattern(2, 16) == model::Status::Ok);
  // Pattern 1, track 0: a note with a Roll (every 1/8 step: it repeats while the step is open, i.e. until the next note or an OFF); pattern 2 is empty on that track.
  CHECK(p.set_step(1, 0, 0, make_step(60, 1, fx::kRoll, 0x02)) == model::Status::Ok);
  seq::Sequencer& s = make();
  s.play_pattern(1);
  run(s, 0, in_step(3));
  const std::uint32_t before = g_col.count(EventKind::NoteOn, 0);
  CHECK(before > 1u);  // the roll runs
  send_switch(s, 2, true);
  run(s, in_step(3), at_step(12));
  const std::uint32_t at_land = g_col.count(EventKind::NoteOn, 0);
  run(s, at_step(12), at_step(20));
  CHECK(g_col.count(EventKind::NoteOn, 0) == at_land);  // no repeat after the landing: the step is closed (a roll that went on would play 8 more steps of repeats)
  // a track with no note sounding gets nothing
  CHECK(g_col.count(EventKind::NoteOff, 5) == 0u);
  CHECK(g_col.count(EventKind::MidiNoteOff, 5) == 0u);
}

TEST_CASE("patswitch seq: a Break pattern of the pattern left goes with it: the new pattern is not cut short a step in") {
  fresh();
  rows_pattern(1, 16, 0);
  rows_pattern(2, 16, 64);
  CHECK(seq_project().set_step(1, 1, 3, fx_step(fx::kBreakPattern, 1)) == model::Status::Ok);  // row 3 of pattern 1 ends it
  seq::Sequencer& s = make();
  s.play_pattern(1);
  run(s, 0, in_step(3));  // row 3 has begun: the break waits for its boundary
  send_switch(s, 2, true);
  run(s, in_step(3), at_step(12));
  // the break ends the old pattern at this boundary: the new one starts at its row 0, and then goes straight on (0, 1, 2, ...), not 0, 0, 1, ...
  for (std::uint32_t n = 4; n < 12; ++n) {
    CHECK_MESSAGE(note_of_step(n)->ev.a == 64u + (n - 4u), n);
  }
}

TEST_CASE("patswitch seq: an even-to-even switch keeps the swing pair; the grid is the run's, whatever the rows do") {
  for (std::uint32_t swing = 50; swing <= 66; swing += 16) {
    // control: no switch; test: 16 steps to 8 steps at a row that keeps its parity
    std::uint64_t t_control[24] = {};
    for (std::uint32_t run_no = 0; run_no < 2; ++run_no) {
      fresh();
      rows_pattern(1, 16, 0);
      rows_pattern(2, 8, 64);
      seq::Sequencer& s = make();
      CHECK(s.apply(core::ParamChange{core::param_id(core::ParamKind::Project, 0, seq::seq_param::Swing), static_cast<float>(swing), 1}));
      s.play_pattern(1);
      run(s, 0, at_step(5) + 100u);
      if (run_no != 0u) {
        send_switch(s, 2, true);
      }
      run(s, at_step(5) + 100u, at_step(40));
      for (std::uint32_t n = 0; n < 24; ++n) {
        const Rec* st = g_col.nth(EventKind::Step, n);
        NEED(st != nullptr);
        if (run_no == 0u) {
          t_control[n] = st->t;
        } else {
          CHECK_MESSAGE(st->t == t_control[n], "swing " << swing << " step " << n);  // the same sample as the run without the switch
        }
        // the parity of the row is the parity of the step (the pair): the long step of the pair is always an even row
        if (run_no != 0u && n >= 6u) {
          CHECK_MESSAGE((static_cast<std::uint32_t>(st->ev.value) & 1u) == (n & 1u), "swing " << swing << " step " << n);
        }
      }
      if (run_no != 0u) {
        CHECK(s.position().pattern_id == 2);
      }
    }
  }
}

TEST_CASE("patswitch seq: a switch does not depend on the block size when its command precedes the boundary, and costs the stream nothing else") {
  const std::uint32_t sizes[] = {64, 61, 480, 7};
  std::uint64_t hash[4] = {};
  std::uint32_t events[4] = {};
  for (std::uint32_t i = 0; i < 4; ++i) {
    fresh();
    rows_pattern(1, 16, 0);
    rows_pattern(2, 12, 64);
    seq::Sequencer& s = make();
    s.play_pattern(1);
    g_col.run(s, 0, in_step(6), sizes[i]);
    send_switch(s, 2, true);
    g_col.run(s, in_step(6), at_step(40), sizes[i]);
    hash[i] = g_col.hash;
    events[i] = g_col.total_events;
    CHECK(s.last_landing() == ((1u << 16) | (2u << 8) | 7u));  // the one landing: pattern 2, row 7
  }
  for (std::uint32_t i = 1; i < 4; ++i) {
    CHECK_MESSAGE(hash[i] == hash[0], "block " << sizes[i]);
    CHECK(events[i] == events[0]);
  }
  // no switch: the stream of a run is the same whether or not the code for a switch exists (the hash of a run without one equals itself across block sizes)
  std::uint64_t plain[2] = {};
  for (std::uint32_t i = 0; i < 2; ++i) {
    fresh();
    rows_pattern(1, 16, 0);
    seq::Sequencer& s = make();
    s.play_pattern(1);
    g_col.run(s, 0, at_step(40), sizes[i]);
    plain[i] = g_col.hash;
    CHECK(s.last_landing() == 0u);  // only the natural pattern ends
  }
  CHECK(plain[0] == plain[1]);
}

TEST_CASE("patswitch seq: every landing is counted in the log word with its pattern and row") {
  fresh();
  rows_pattern(1, 16, 0);
  rows_pattern(2, 8, 64);
  rows_pattern(3, 16, 32);
  seq::Sequencer& s = make();
  CHECK(s.last_landing() == 0u);
  s.play_pattern(1);
  run(s, 0, in_step(5));
  send_switch(s, 2, true);
  run(s, in_step(5), in_step(8));
  CHECK(s.last_landing() == ((1u << 16) | (2u << 8) | 6u));
  send_switch(s, 3, true);
  run(s, in_step(8), in_step(12));
  CHECK(s.last_landing() == ((2u << 16) | (3u << 8) | 1u));  // pattern 2 (8 steps) was on row 0 at step 8, so the next row is 1
}
