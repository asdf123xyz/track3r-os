// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-COREFX CF1 (docs/specs/OS-COREFX.md, ADR 0004 amendment 2): the tracker FX 46..50 in the sequencer. Each one is a Row FX: the sequencer consumes it at the start of its row and sends the
// voices a TrkFx event (never a ModSet: the control table would hold it past its row), on the row's first sample and before the row's NoteOn and Tick 0; the first row of a column that holds
// none after a row that held one sends the type-0 TrkFx that ends it; a fresh run starts without any. Host and Cortex-M4F (`qemu_seq_model`: one project in .bss).
#include <doctest/doctest.h>

#include <o3os/seq/fx.h>
#include <o3os/seq/sequencer.h>

#include <cstdint>
#include <initializer_list>

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

void pattern16(std::uint32_t tracks = 4) {
  fresh_project(tracks, 6);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(12000) == model::Status::Ok);
  CHECK(p.create_pattern(1, 16) == model::Status::Ok);
}

seq::Sequencer& make(bool ticks = false) {
  seq::Sequencer& s = g_slot.make(seq_project());
  g_col.n = 0;
  g_col.total_events = 0;
  g_col.hash = 0xCBF29CE484222325ull;
  seq::Sequencer::Config cfg = s.config();
  cfg.emit_ticks = ticks;
  s.configure(cfg);
  return s;
}

// The sample of microstep j of step n at 120 BPM (core/qtime.h: round(ticks * kCpuHz * den * 125 / (4 * num * bpm_x100)), half up).
std::uint64_t at_us(std::uint32_t n, std::uint32_t j) {
  const std::uint64_t ticks = 48u * n + j;
  const std::uint64_t num = ticks * core::kCpuHz * core::kRefCyclesPerSample.den * 125u;
  const std::uint64_t d = std::uint64_t{4u} * core::kRefCyclesPerSample.num * 12000u;  // 64-bit: 4 * 1800000 * 12000 does not fit 32
  return (2u * num + d) / (2u * d);
}

// The TrkFx events of a track at sample `t`, as (type, value, column), in the order they came out; at most 4.
struct Trk {
  std::uint8_t type, value, col;
};
std::uint32_t trk_at(std::uint8_t track, std::uint64_t t, Trk out[4]) {
  std::uint32_t c = 0;
  for (std::uint32_t i = 0; i < g_col.n && c < 4u; ++i) {
    const Rec& r = g_col.rec[i];
    if (r.ev.kind == EventKind::TrkFx && r.ev.track == track && r.t == t) {
      out[c++] = Trk{r.ev.a, r.ev.b, r.ev.flags};
    }
  }
  return c;
}

// Where in the record list the first event of `kind` on `track` at sample `t` stands (g_col.n if none).
std::uint32_t index_of(EventKind kind, std::uint8_t track, std::uint64_t t) {
  for (std::uint32_t i = 0; i < g_col.n; ++i) {
    if (g_col.rec[i].ev.kind == kind && g_col.rec[i].ev.track == track && g_col.rec[i].t == t) {
      return i;
    }
  }
  return g_col.n;
}

}  // namespace

TEST_CASE("seq corefx: the sixteen tracker FX are Row FX of no group, kind Row, any byte, ids 46..62 contiguous after the core FX") {
  CHECK(fx::kTrkArp == 46);
  CHECK(fx::kTrkVolSlide == 47);
  CHECK(fx::kTrkFineVol == 48);
  CHECK(fx::kTrkFinePortaUp == 49);
  CHECK(fx::kTrkFinePortaDown == 50);
  CHECK(fx::kTrkPanSlide == 51);
  CHECK(fx::kTrkVibWave == 52);
  CHECK(fx::kTrkTremWave == 53);
  CHECK(fx::kTrkGliss == 54);
  CHECK(fx::kTrkRetrig == 55);
  CHECK(fx::kTrkTremor == 56);
  CHECK(fx::kTrkChanPan == 57);
  CHECK(fx::kTrkChanVol == 58);
  CHECK(fx::kTrkChanVolSlide == 59);
  CHECK(fx::kTrkGlobalVol == 60);
  CHECK(fx::kTrkGlobalVolSlide == 61);
  CHECK(fx::kTrkPanbrello == 62);
  CHECK(fx::kCoreLast == 62);
  CHECK(fx::kOrderJump + 1 == fx::kTrkArp);
  for (std::uint8_t t = fx::kTrkArp; t <= fx::kCoreLast; ++t) {
    CHECK(seq::fx_kind(t) == seq::FxKind::Row);
    CHECK(seq::fx_group(t, 0) == seq::FxGroup::None);
    std::uint8_t type = t, value = 255;
    seq::fx_sanitize(type, value);
    CHECK(type == t);
    CHECK(value == (t == fx::kTrkChanPan ? 100 : (t == fx::kTrkChanVol || t == fx::kTrkGlobalVol ? 64 : 255)));  // the whole byte is the effect's (the nibbles x y, or a count); Channel pan is 0..100, Channel and Global volume 0..64
  }
}

TEST_CASE("seq corefx: a tracker FX on a note step is one TrkFx event, on the row's first sample, before its NoteOn, and no ModSet") {
  for (std::uint8_t type = fx::kTrkArp; type <= fx::kCoreLast; ++type) {
    CAPTURE(int(type));
    pattern16();
    CHECK(seq_project().set_step(1, 0, 0, make_step(60, 1, type, 0x37)) == model::Status::Ok);
    seq::Sequencer& s = make();
    s.play_pattern(1);
    g_col.run(s, 0, at_us(1, 0), 64);
    Trk t[4];
    NEED(trk_at(0, at_us(0, 0), t) == 1u);
    CHECK(t[0].type == type);
    CHECK(t[0].value == 0x37);
    CHECK(t[0].col == 0);
    CHECK(g_col.count(EventKind::ModSet) == 0u);  // not a held voice FX
    CHECK(g_col.count(EventKind::ModEnd) == 0u);
    const std::uint32_t trk = index_of(EventKind::TrkFx, 0, at_us(0, 0));
    const std::uint32_t on = index_of(EventKind::NoteOn, 0, at_us(0, 0));
    NEED(on < g_col.n);
    CHECK(trk < on);  // the effect is in place when the note starts
  }
}

TEST_CASE("seq corefx: the second FX column is flag 1; both columns may hold one; a step without a note sends it too") {
  pattern16();
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 0, make_step(60, 1, fx::kTrkVolSlide, 0x04, fx::kTrkArp, 0x47)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 1, fx_step(0, 0, fx::kTrkFineVol, 0x20)) == model::Status::Ok);  // no note: the second column only
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(2, 0), 64);
  Trk t[4];
  NEED(trk_at(0, at_us(0, 0), t) == 2u);
  // the reference order: the second column first
  CHECK((t[0].type == fx::kTrkArp && t[0].col == 1 && t[0].value == 0x47));
  CHECK((t[1].type == fx::kTrkVolSlide && t[1].col == 0 && t[1].value == 0x04));
  // row 1: the first column held one on row 0 and holds none: its end; the second column has a new one
  NEED(trk_at(0, at_us(1, 0), t) == 2u);
  CHECK((t[0].type == fx::kTrkFineVol && t[0].col == 1 && t[0].value == 0x20));
  CHECK((t[1].type == 0 && t[1].col == 0));
}

TEST_CASE("seq corefx: an effect ends with its row: the first row of the column without one sends the type-0 TrkFx, once; a new effect replaces without one") {
  pattern16();
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 0, make_step(60, 1, fx::kTrkArp, 0x37)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 1, fx_step(fx::kTrkArp, 0x58)) == model::Status::Ok);  // again, another value: replaced, nothing ends
  CHECK(p.set_step(1, 0, 3, make_step(62, 1)) == model::Status::Ok);            // row 2 has none: the end; row 3 has none either: no second end
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(6, 0), 64);
  Trk t[4];
  NEED(trk_at(0, at_us(0, 0), t) == 1u);
  CHECK((t[0].type == fx::kTrkArp && t[0].value == 0x37));
  NEED(trk_at(0, at_us(1, 0), t) == 1u);
  CHECK((t[0].type == fx::kTrkArp && t[0].value == 0x58));
  NEED(trk_at(0, at_us(2, 0), t) == 1u);
  CHECK((t[0].type == 0 && t[0].col == 0));
  CHECK(trk_at(0, at_us(3, 0), t) == 0u);
  CHECK(trk_at(0, at_us(4, 0), t) == 0u);
  CHECK(g_col.count(EventKind::TrkFx, 0) == 3u);
  CHECK(g_col.count(EventKind::TrkFx, 1) == 0u);  // other tracks never hear of it
}

TEST_CASE("seq corefx: with Tick events the row's TrkFx, NoteOn and Tick 0 come in that order on one sample; a fresh run starts with no effect to end") {
  pattern16();
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 0, make_step(60, 1, fx::kTrkVolSlide, 0x02)) == model::Status::Ok);
  seq::Sequencer& s = make(true);
  s.play_pattern(1);
  g_col.run(s, 0, at_us(2, 0), 64);
  const std::uint32_t trk = index_of(EventKind::TrkFx, 0, at_us(0, 0));
  const std::uint32_t on = index_of(EventKind::NoteOn, 0, at_us(0, 0));
  const std::uint32_t tick0 = index_of(EventKind::Tick, audio::kNoTrack, at_us(0, 0));
  NEED(trk < g_col.n);
  NEED(on < g_col.n);
  NEED(tick0 < g_col.n);
  CHECK(trk < on);
  CHECK(on < tick0);
  CHECK(g_col.rec[tick0].ev.a == 0);  // tick 0 of 6
  CHECK(g_col.rec[tick0].ev.b == 6);
  // stop, and play again: the transport start resets the track state, so the first row (now without the effect) ends nothing
  CHECK(seq_project().set_step(1, 0, 0, make_step(60, 1)) == model::Status::Ok);
  s.stop();
  g_col.n = 0;
  s.play_pattern(1);
  g_col.run(s, at_us(2, 0), at_us(4, 0), 64);
  CHECK(g_col.count(EventKind::TrkFx) == 0u);
}

TEST_CASE("seq corefx: the effects are the same whatever the block size (the determinism of the event stream)") {
  pattern16();
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 0, make_step(60, 1, fx::kTrkArp, 0x37, fx::kTrkFinePortaUp, 8)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 2, fx_step(fx::kTrkVolSlide, 0x03)) == model::Status::Ok);
  std::uint64_t first = 0;
  for (const std::uint32_t block : {1u, 7u, 64u, 441u, 5000u}) {
    seq::Sequencer& s = make(true);
    s.play_pattern(1);
    g_col.run(s, 0, at_us(6, 0), block);
    if (first == 0) {
      first = g_col.hash;
    }
    CHECK_MESSAGE(g_col.hash == first, "block " << block);
  }
}

TEST_CASE("seq corefx: when the event queue is full at the row that ends an effect, the end is owed and sent on a later row (review MINOR 4)") {
  constexpr std::uint32_t kTracks = 16;
  fresh_project(kTracks, 6);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(12000) == model::Status::Ok);
  CHECK(p.create_pattern(1, 32) == model::Status::Ok);
  for (std::uint32_t t = 0; t < kTracks; ++t) {
    for (std::uint32_t row = 0; row < 20; ++row) {
      CHECK(p.set_step(1, t, row, fx_step(fx::kTrkArp, 0x37)) == model::Status::Ok);
    }
  }
  seq::Sequencer& s = make();
  s.play_pattern(1);
  // 20 rows of 16 effects, nothing taking the events away (a block with room for none): the 256-entry queue fills up and row 20, which ends every effect, finds it full
  audio::SeqEvent none[1];
  (void)s.step(0, static_cast<std::uint32_t>(at_us(21, 0)), none, 0);
  CHECK(s.stats().queue_overflow > 0u);
  // now the events are taken away; the rows go on and every track hears its end once (the first blocks still find the queue full, so the owed ends are tried again)
  g_col.run(s, at_us(21, 0), at_us(30, 0), 64);
  std::uint32_t ends[kTracks] = {};
  for (std::uint32_t i = 0; i < g_col.n; ++i) {
    const Rec& r = g_col.rec[i];
    if (r.ev.kind == EventKind::TrkFx && r.ev.a == 0 && r.ev.track < kTracks) {
      ++ends[r.ev.track];
    }
  }
  for (std::uint32_t t = 0; t < kTracks; ++t) {
    CHECK_MESSAGE(ends[t] == 1u, "track " << t);  // not lost, not repeated
  }
}

TEST_CASE("seq corefx: stop and resume with an effect on: the effect is ended by the first row without one, never left on") {
  pattern16();
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 0, make_step(60, 1, fx::kTrkArp, 0x37)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 4, make_step(62, 1)) == model::Status::Ok);
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(0, 20), 64);  // inside row 0: the effect is on
  CHECK(g_col.count(EventKind::TrkFx, 0) == 1u);
  s.stop();
  g_col.run(s, at_us(0, 20), at_us(1, 0), 64);
  s.resume();
  g_col.run(s, at_us(1, 0), at_us(12, 0), 64);
  // the last TrkFx the track heard ends the effect (a resume that replays row 0 starts it again and then ends it)
  int last = -1;
  for (std::uint32_t i = 0; i < g_col.n; ++i) {
    if (g_col.rec[i].ev.kind == EventKind::TrkFx && g_col.rec[i].ev.track == 0) {
      last = g_col.rec[i].ev.a;
    }
  }
  CHECK(last == 0);
}

// ---- CF2: the six new FX take the same paths as 46..50 (the processor reads the row, the end is owed, a stop leaves none on) -------------------------------------------------------------

TEST_CASE("seq corefx CF2: 51..56 end with their row, once, and a replacing effect sends no end") {
  for (std::uint8_t type = fx::kTrkPanSlide; type <= fx::kTrkTremor; ++type) {
    CAPTURE(int(type));
    pattern16();
    model::Project& p = seq_project();
    CHECK(p.set_step(1, 0, 0, make_step(60, 1, type, 0x37)) == model::Status::Ok);
    CHECK(p.set_step(1, 0, 1, fx_step(type, 0x58)) == model::Status::Ok);
    CHECK(p.set_step(1, 0, 3, make_step(62, 1)) == model::Status::Ok);
    seq::Sequencer& s = make();
    s.play_pattern(1);
    g_col.run(s, 0, at_us(6, 0), 64);
    Trk t[4];
    NEED(trk_at(0, at_us(0, 0), t) == 1u);
    CHECK((t[0].type == type && t[0].value == 0x37 && t[0].col == 0));
    NEED(trk_at(0, at_us(1, 0), t) == 1u);
    CHECK((t[0].type == type && t[0].value == 0x58));
    NEED(trk_at(0, at_us(2, 0), t) == 1u);
    CHECK((t[0].type == 0 && t[0].col == 0));
    CHECK(trk_at(0, at_us(3, 0), t) == 0u);
    CHECK(g_col.count(EventKind::TrkFx, 0) == 3u);
    CHECK(g_col.count(EventKind::ModSet) == 0u);
  }
}

TEST_CASE("seq corefx CF2: stop and resume with 51..56 on: the last TrkFx the track heard is the end; a full queue at the end owes it (each of the six)") {
  for (std::uint8_t type = fx::kTrkPanSlide; type <= fx::kTrkTremor; ++type) {
    CAPTURE(int(type));
    pattern16();
    model::Project& p = seq_project();
    CHECK(p.set_step(1, 0, 0, make_step(60, 1, type, 0x21)) == model::Status::Ok);
    CHECK(p.set_step(1, 0, 4, make_step(62, 1)) == model::Status::Ok);
    seq::Sequencer& s = make();
    s.play_pattern(1);
    g_col.run(s, 0, at_us(0, 20), 64);
    CHECK(g_col.count(EventKind::TrkFx, 0) == 1u);
    s.stop();
    g_col.run(s, at_us(0, 20), at_us(1, 0), 64);
    s.resume();
    g_col.run(s, at_us(1, 0), at_us(12, 0), 64);
    int last = -1;
    for (std::uint32_t i = 0; i < g_col.n; ++i) {
      if (g_col.rec[i].ev.kind == EventKind::TrkFx && g_col.rec[i].ev.track == 0) {
        last = g_col.rec[i].ev.a;
      }
    }
    CHECK(last == 0);
  }
  constexpr std::uint32_t kTracks = 16;
  for (std::uint8_t type : {fx::kTrkPanSlide, fx::kTrkRetrig, fx::kTrkTremor}) {
    CAPTURE(int(type));
    fresh_project(kTracks, 6);
    model::Project& p = seq_project();
    CHECK(p.set_tempo_x100(12000) == model::Status::Ok);
    CHECK(p.create_pattern(1, 32) == model::Status::Ok);
    for (std::uint32_t t = 0; t < kTracks; ++t) {
      for (std::uint32_t row = 0; row < 20; ++row) {
        CHECK(p.set_step(1, t, row, fx_step(type, 0x21)) == model::Status::Ok);
      }
    }
    seq::Sequencer& s = make();
    s.play_pattern(1);
    audio::SeqEvent none[1];
    (void)s.step(0, static_cast<std::uint32_t>(at_us(21, 0)), none, 0);
    CHECK(s.stats().queue_overflow > 0u);
    g_col.run(s, at_us(21, 0), at_us(30, 0), 64);
    std::uint32_t ends[kTracks] = {};
    for (std::uint32_t i = 0; i < g_col.n; ++i) {
      const Rec& r = g_col.rec[i];
      if (r.ev.kind == EventKind::TrkFx && r.ev.a == 0 && r.ev.track < kTracks) {
        ++ends[r.ev.track];
      }
    }
    for (std::uint32_t t = 0; t < kTracks; ++t) {
      CHECK_MESSAGE(ends[t] == 1u, "track " << t);
    }
  }
}

// ---- the Panning FX (31) ends where the other FX end; Channel pan (57) is a TrkFx, held by the processor ---------------------------------------------------------------------------------

TEST_CASE("seq corefx: the Panning FX (31) is a ModSet that ends with a ModEnd at the next note step without it and on FX Off; Channel pan (57) is one TrkFx and never a ModSet or ModEnd") {
  pattern16();
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 0, make_step(60, 1, fx::kPanning, 75)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 2, make_step(62, 1)) == model::Status::Ok);                          // a plain note: the FX ends before it
  CHECK(p.set_step(1, 0, 3, make_step(64, 1, fx::kPanning, 10)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 5, fx_step(fx::kOff, 0)) == model::Status::Ok);                      // FX Off: it ends
  CHECK(p.set_step(1, 0, 6, make_step(65, 1, fx::kTrkChanPan, 30)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 8, make_step(66, 1)) == model::Status::Ok);                          // Channel pan is not the sequencer's to end
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(10, 0), 64);
  const std::uint8_t group = static_cast<std::uint8_t>(audio::kModGroupPanning);
  std::uint32_t sets = 0, ends = 0;
  for (std::uint32_t i = 0; i < g_col.n; ++i) {
    const audio::SeqEvent& e = g_col.rec[i].ev;
    if (e.track != 0 || e.flags != group) {
      continue;
    }
    sets += e.kind == EventKind::ModSet ? 1u : 0u;
    ends += e.kind == EventKind::ModEnd ? 1u : 0u;
  }
  CHECK(sets == 2u);
  CHECK(ends == 2u);
  auto panning_at = [](EventKind kind, std::uint32_t step) {
    for (std::uint32_t i = 0; i < g_col.n; ++i) {
      const audio::SeqEvent& e = g_col.rec[i].ev;
      if (e.kind == kind && e.track == 0 && e.flags == audio::kModGroupPanning && g_col.rec[i].t == at_us(step, 0)) {
        return i;
      }
    }
    return g_col.n;
  };
  const std::uint32_t set0 = panning_at(EventKind::ModSet, 0);
  NEED(set0 < g_col.n);
  CHECK(g_col.rec[set0].ev.b == 75);
  CHECK(g_col.rec[set0].ev.a == fx::kPanning);
  CHECK(set0 < index_of(EventKind::NoteOn, 0, at_us(0, 0)));
  const std::uint32_t end2 = panning_at(EventKind::ModEnd, 2);
  NEED(end2 < g_col.n);
  CHECK(end2 < index_of(EventKind::NoteOn, 0, at_us(2, 0)));  // the note after it plays at its instrument's pan
  NEED(panning_at(EventKind::ModSet, 3) < g_col.n);
  CHECK(panning_at(EventKind::ModEnd, 5) < g_col.n);          // FX Off
  Trk t[4];
  NEED(trk_at(0, at_us(6, 0), t) == 1u);
  CHECK((t[0].type == fx::kTrkChanPan && t[0].value == 30));
  CHECK(panning_at(EventKind::ModSet, 6) == g_col.n);
  CHECK(panning_at(EventKind::ModEnd, 8) == g_col.n);
}
