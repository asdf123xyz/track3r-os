// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <o3os/model/editor.h>
#include <o3os/model/project.h>
#include <o3os/model/undo.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#if !defined(__arm__)
#include <cstdio>
#endif

#include "support/need.h"
#include "support/rng.h"

// Like NEED, for a function that returns a value.
#define NEED_OR(cond, retval)                       \
  do {                                              \
    const bool o3_ok = static_cast<bool>(cond);     \
    CHECK_MESSAGE(o3_ok, #cond);                    \
    if (!o3_ok) {                                   \
      return retval;                                \
    }                                               \
  } while (0)

// The undo journal and the Editor (OS-002c). No heap on the target: the project, the ring and the snapshots are static, so this
// file also runs on QEMU (suite `qemu_model_undo`), with fewer and smaller episodes.

using namespace o3os;
using model::Editor;
using model::Instrument;
using model::Pattern;
using model::Project;
using model::Status;
using model::Step;
using model::UndoJournal;
using model::UndoResult;

namespace {

#if defined(__arm__)
constexpr bool kTarget = true;
#else
constexpr bool kTarget = false;
#endif

// What the oracle looks at: pattern ids 1..kIds and instrument slots 0..kInst-1. The random edits stay inside them, so a
// Snap is the whole project as far as any edit can reach.
constexpr unsigned kIds = kTarget ? 2 : 6;
constexpr unsigned kInst = kTarget ? 3 : 8;
constexpr unsigned kMaxDepth = kTarget ? 8 : 220;  // an episode makes at most this many undo units

struct PatSnap {
  std::uint8_t present;
  std::uint8_t lengths[model::kMaxTracks];
  char name[model::kPatternNameMax + 1];
  Step steps[model::kMaxTracks][model::kMaxPatternLength];
};

struct Snap {
  char name[model::kProjectNameMax + 1];
  std::uint32_t tempo;
  std::uint8_t swing, default_length, active_tracks;
  model::MasterSettings master;
  model::TrackSlot tracks[model::kMaxTracks];
  std::uint32_t song_length;
  std::uint8_t song[model::kMaxSongPositions];
  std::uint32_t pattern_count;
  std::uint32_t instrument_count;
  PatSnap pats[kIds];
  Instrument inst[kInst];
  std::uint8_t inst_present[kInst];
};

void capture(const Project& p, Snap& s) {
  std::memset(&s, 0, sizeof s);
  const std::size_t n = model::field_length(p.name(), model::kProjectNameMax);
  std::memcpy(s.name, p.name(), n);
  s.tempo = p.tempo_x100();
  s.swing = p.swing();
  s.default_length = p.default_pattern_length();
  s.active_tracks = p.active_tracks();
  s.master = p.master();
  for (std::uint32_t t = 0; t < model::kMaxTracks; ++t) {
    s.tracks[t] = *p.track(t);
  }
  s.song_length = p.song_length();
  for (std::uint32_t i = 0; i < s.song_length; ++i) {
    s.song[i] = p.order(i);
  }
  s.pattern_count = p.pattern_count();
  s.instrument_count = p.instrument_count();
  for (unsigned id = 1; id <= kIds; ++id) {
    const Pattern* pat = p.pattern(id);
    PatSnap& ps = s.pats[id - 1];
    ps.present = pat != nullptr ? 1 : 0;
    if (pat != nullptr) {
      std::memcpy(ps.lengths, pat->track_length, sizeof ps.lengths);
      std::memcpy(ps.name, pat->name, sizeof ps.name);
      std::memcpy(ps.steps, pat->steps, sizeof ps.steps);
    }
  }
  for (unsigned i = 0; i < kInst; ++i) {
    const Instrument* in = p.instrument(i);
    s.inst_present[i] = in != nullptr ? 1 : 0;
    if (in != nullptr) {
      s.inst[i] = *in;
    }
  }
}

bool same(const Snap& a, const Snap& b) { return std::memcmp(&a, &b, sizeof a) == 0; }

// Six snapshots the tests share (a Snap is 33 KB on the target, 95 KB on the host: statics per test would not fit QEMU).
Snap& scratch(unsigned i) {
  static Snap s[6];
  return s[i];
}

// ---- the stacks of snapshots the oracle keeps ------------------------------------------------------------------------------

#if defined(__arm__)
Snap g_pool[2 * kMaxDepth + 4];
Snap* pool_base() { return g_pool; }
#else
Snap* pool_base() {
  static Snap* pool = new Snap[2 * kMaxDepth + 4];  // test code: the host is allowed a heap (the library is not)
  return pool;
}
#endif

// A stack of snapshots with room for kMaxDepth + 2 of them (`past` and `future` share the pool).
class SnapStack {
 public:
  explicit SnapStack(Snap* items, unsigned cap) : items_(items), cap_(cap) {}
  unsigned size() const { return n_; }
  bool empty() const { return n_ == 0; }
  void clear() { n_ = 0; }
  void push(const Snap& s) {
    if (n_ == cap_) {  // cannot happen: an episode makes at most kMaxDepth units
      std::memmove(items_, items_ + 1, (cap_ - 1) * sizeof(Snap));
      --n_;
    }
    items_[n_++] = s;
  }
  Snap& back() { return items_[n_ - 1]; }
  void pop() { --n_; }
  void drop_oldest() {
    std::memmove(items_, items_ + 1, (n_ - 1) * sizeof(Snap));
    --n_;
  }

 private:
  Snap* items_;
  unsigned cap_;
  unsigned n_ = 0;
};

// ---- the project, the ring and the random edits ------------------------------------------------------------------------------

model::ModelArena g_arena;
alignas(8) std::uint8_t g_ring[model::kUndoRegionBytes];

Project& project() { return g_arena.project; }

void fresh_project() {
  project().reset();
  (void)project().add_module("synth", 5, 1, 0);  // so Module instruments can be valid
}

Instrument make_instrument(test::Rng& rng) {
  Instrument in;
  switch (rng.below(3)) {
    case 0:
      model::init_instrument(in, model::InstrumentKind::Sample);
      break;
    case 1:
      model::init_instrument(in, model::InstrumentKind::Midi);
      in.midi_channel = static_cast<std::uint8_t>(1 + rng.below(16));
      break;
    default:
      model::init_instrument(in, model::InstrumentKind::Module);
      in.module_index = 0;
      in.param_len = static_cast<std::uint16_t>(rng.below(40));
      for (unsigned i = 0; i < in.param_len; ++i) in.params[i] = rng.byte();
      break;
  }
  const unsigned n = rng.below(model::kInstrumentNameMax + 1);
  for (unsigned i = 0; i < n; ++i) in.name[i] = static_cast<char>('A' + rng.below(26));
  if (rng.below(3) == 0) {
    in.sampler.volume = static_cast<std::uint8_t>(rng.below(101));
    in.sampler.tune = static_cast<std::int8_t>(rng.byte());
    in.sampler.cutoff = static_cast<float>(rng.below(1000)) / 7.0f;
    in.sampler.start = static_cast<std::uint16_t>(rng.next());
  }
  if (rng.below(12) == 0) in.sampler.volume = 101;  // invalid: rejected
  return in;
}

void random_name(test::Rng& rng, char* out, std::size_t cap, std::size_t& len) {
  len = rng.below(static_cast<std::uint32_t>(cap + 3));  // sometimes too long: rejected
  for (std::size_t i = 0; i < len; ++i) out[i] = static_cast<char>('a' + rng.below(26));
  if (len != 0 && rng.below(14) == 0) out[rng.below(static_cast<std::uint32_t>(len))] = '\0';  // a NUL inside: rejected
}

Step random_step(test::Rng& rng) {
  Step s;
  s.note = static_cast<std::uint8_t>(rng.below(3) == 0 ? 0 : rng.below(130));
  s.instr = static_cast<std::uint8_t>(rng.below(4));
  s.vol = static_cast<std::uint8_t>(rng.below(2) == 0 ? 255 : rng.below(128));
  s.fx[0] = {static_cast<std::uint8_t>(rng.below(5)), rng.byte()};
  s.fx[1] = {0, 0};
  return s;
}

std::uint32_t pick_id(test::Rng& rng) {
  const std::uint32_t r = rng.below(40);
  if (r == 0) return 0;      // invalid
  if (r == 1) return 256;    // invalid
  return 1 + rng.below(kIds);
}
std::uint32_t pick_track(test::Rng& rng) { return rng.below(24) == 0 ? 16 + rng.below(5) : rng.below(model::kMaxTracks); }

// One random edit, valid or not, through the Editor.
unsigned g_last_op = 0;
void random_edit(Editor& ed, test::Rng& rng) {
  const unsigned which = rng.below(24);
  g_last_op = which;
  switch (which) {
    case 0: {
      char buf[model::kProjectNameMax + 4];
      std::size_t n;
      random_name(rng, buf, model::kProjectNameMax, n);
      (void)ed.set_name(buf, n);
      break;
    }
    case 1: (void)ed.set_tempo_x100(model::kMinTempoX100 - 100 + rng.below(model::kMaxTempoX100 - model::kMinTempoX100 + 200)); break;
    case 2: (void)ed.set_swing(24 + rng.below(54)); break;
    case 3: (void)ed.set_default_pattern_length(rng.below(132)); break;
    case 4: (void)ed.set_active_tracks(rng.below(19)); break;
    case 5: {
      model::MasterSettings m{static_cast<std::uint8_t>(rng.below(102)), static_cast<std::uint8_t>(rng.below(101)),
                              static_cast<std::uint8_t>(rng.below(101)), static_cast<std::uint8_t>(rng.below(101)),
                              static_cast<std::uint8_t>(rng.below(17))};
      (void)ed.set_master(m);
      break;
    }
    case 6: {
      model::TrackSlot t{};
      t.type = static_cast<model::TrackType>(rng.below(4));
      t.flags = static_cast<std::uint8_t>(rng.below(4));
      t.volume = static_cast<std::uint8_t>(rng.below(102));
      t.pan = static_cast<std::uint8_t>(rng.below(101));
      t.bus = static_cast<std::uint8_t>(rng.below(18));
      std::size_t n = rng.below(model::kTrackNameMax);
      for (std::size_t i = 0; i < n; ++i) t.name[i] = static_cast<char>('a' + rng.below(26));
      (void)ed.set_track(pick_track(rng), t);
      break;
    }
    case 7: {
      char buf[model::kTrackNameMax + 4];
      std::size_t n;
      random_name(rng, buf, model::kTrackNameMax, n);
      (void)ed.set_track_name(pick_track(rng), buf, n);
      break;
    }
    case 8: (void)ed.append_order(rng.below(kIds + 4)); break;
    case 9: (void)ed.insert_order(rng.below(ed.project().song_length() + 3), rng.below(kIds + 4)); break;
    case 10: (void)ed.set_order(rng.below(ed.project().song_length() + 2), rng.below(kIds + 4)); break;
    case 11: (void)ed.remove_order(rng.below(ed.project().song_length() + 2)); break;
    case 12:
      if (rng.below(6) == 0) (void)ed.clear_order();
      else (void)ed.append_order(1 + rng.below(kIds));
      break;
    case 13: (void)ed.create_pattern(pick_id(rng), rng.below(132)); break;
    case 14: (void)ed.delete_pattern(pick_id(rng)); break;
    case 15: {
      char buf[model::kPatternNameMax + 4];
      std::size_t n;
      random_name(rng, buf, model::kPatternNameMax, n);
      (void)ed.set_pattern_name(pick_id(rng), buf, n);
      break;
    }
    case 16: (void)ed.set_track_length(pick_id(rng), pick_track(rng), rng.below(132)); break;
    case 17: (void)ed.set_pattern_length(pick_id(rng), rng.below(132)); break;
    case 18: (void)ed.clear_pattern(pick_id(rng)); break;
    case 19:
    case 20: (void)ed.set_step(pick_id(rng), pick_track(rng), rng.below(40) == 0 ? 128 + rng.below(3) : rng.below(model::kMaxPatternLength), random_step(rng)); break;
    case 21: {
      Step steps[model::kMaxPatternLength];
      const std::uint32_t count = rng.below(8) == 0 ? rng.below(model::kMaxPatternLength + 2) : 1 + rng.below(12);
      for (std::uint32_t i = 0; i < count && i < model::kMaxPatternLength; ++i) steps[i] = random_step(rng);
      const std::uint32_t first = rng.below(10) == 0 ? rng.below(model::kMaxPatternLength + 2) : rng.below(32);
      (void)ed.set_steps(pick_id(rng), pick_track(rng), first, count == 0 ? nullptr : steps, count);
      break;
    }
    case 22: {
      Instrument in = make_instrument(rng);
      const std::uint32_t slot = rng.below(kInst + 1);
      (void)ed.set_instrument(slot == kInst ? 64 : slot, in);  // slot 64 is out of range: rejected
      break;
    }
    default: {
      const std::uint32_t slot = rng.below(kInst + 1);
      (void)ed.clear_instrument(slot == kInst ? 64 : slot);
      break;
    }
  }
}

// ---- helper: one op undone and redone exactly ------------------------------------------------------------------------------

template <typename F>
void roundtrip(const char* what, F edit) {
  fresh_project();
  UndoJournal journal;
  NEED(journal.begin(g_ring, 64 * 1024));
  Editor ed(project(), journal);
  // some content to edit
  NEED(ed.create_pattern(1, 16) == Status::Ok);
  NEED(ed.create_pattern(2, 32) == Status::Ok);
  NEED(ed.set_step(1, 0, 3, Step{60, 1, 100, {{1, 2}, {3, 4}}}) == Status::Ok);
  NEED(ed.append_order(1) == Status::Ok);
  NEED(ed.append_order(2) == Status::Ok);
  Instrument in;
  model::init_instrument(in, model::InstrumentKind::Sample);
  std::memcpy(in.name, "kick", 5);
  NEED(ed.set_instrument(1, in) == Status::Ok);
  NEED(ed.set_name("before") == Status::Ok);
  ed.history_reset();
  Snap& before = scratch(0);
  Snap& after = scratch(1);
  Snap& again = scratch(2);
  capture(project(), before);
  edit(ed);
  capture(project(), after);
  INFO(what);
  CHECK_MESSAGE(!same(before, after), what << ": the edit changed nothing");
  CHECK(ed.can_undo());
  CHECK(journal.depth() == 1u);
  CHECK(ed.undo() == UndoResult::Ok);
  capture(project(), again);
  CHECK_MESSAGE(same(before, again), what << ": undo is not exact");
  CHECK(project().check_invariants());
  CHECK(ed.can_redo());
  CHECK(ed.redo() == UndoResult::Ok);
  capture(project(), again);
  CHECK_MESSAGE(same(after, again), what << ": redo is not exact");
  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(ed.undo() == UndoResult::Nothing);
  CHECK(ed.redo() == UndoResult::Ok);
  CHECK(ed.redo() == UndoResult::Nothing);
  CHECK(project().check_invariants());
}

}  // namespace

// ---- the journal ------------------------------------------------------------------------------------------------------------

TEST_CASE("undo: the journal takes only a region it can use") {
  UndoJournal j;
  CHECK_FALSE(j.ready());
  CHECK_FALSE(j.begin(nullptr, 4096));
  CHECK_FALSE(j.begin(g_ring, UndoJournal::kMinRegionBytes - 4));
  CHECK_FALSE(j.begin(g_ring, 4097));      // not a multiple of 4
  CHECK_FALSE(j.begin(g_ring + 2, 4096));  // not aligned
  CHECK_FALSE(j.ready());
  CHECK(j.begin(g_ring, 4096));
  CHECK(j.ready());
  CHECK(j.capacity_bytes() == 4096u);
  CHECK(j.depth() == 0u);
  CHECK(j.redo_depth() == 0u);
  CHECK(j.used_bytes() == 0u);
  // An unusable journal records nothing and the edits still happen.
  UndoJournal none;
  fresh_project();
  Editor ed(project(), none);
  CHECK(ed.create_pattern(1, 8) == Status::Ok);
  CHECK(ed.set_step(1, 0, 0, Step{60, 1, 100, {{0, 0}, {0, 0}}}) == Status::Ok);
  CHECK_FALSE(ed.can_undo());
  CHECK(ed.undo() == UndoResult::Nothing);
  CHECK(project().pattern(1)->steps[0][0].note == 60);
}

TEST_CASE("undo: the capacity arithmetic: units of 48, 32, 284, 1440 and 14412 bytes in 384 KiB") {
  CHECK(model::kUndoRegionBytes == 384u * 1024u);
  CHECK(UndoJournal::unit_bytes_for(17) == 48u);
  CHECK(UndoJournal::unit_bytes_for(3) == 32u);
  CHECK(UndoJournal::unit_bytes_for(256) == 284u);
  CHECK(UndoJournal::unit_bytes_for(1410) == 1440u);
  CHECK(UndoJournal::unit_bytes_for(14384) == 14412u);

  fresh_project();
  UndoJournal journal;
  NEED(journal.begin(g_ring, model::kUndoRegionBytes));
  Editor ed(project(), journal);
  NEED(ed.create_pattern(1, 128) == Status::Ok);
  ed.history_reset();

  // 8,192 step edits fill the ring exactly (393,216 / 48): none dropped; the 8,193rd drops the oldest.
  for (std::uint32_t i = 0; i < 8192; ++i) {
    NEED(ed.set_step(1, i % 16, (i / 16) % 128, Step{static_cast<std::uint8_t>(1 + i % 100), 1, 100, {{0, 0}, {0, 0}}}) == Status::Ok);
  }
  CHECK(journal.depth() == 8192u);
  CHECK(journal.dropped_units() == 0u);
  CHECK(journal.used_bytes() == 8192u * 48u);
  CHECK(ed.set_step(1, 0, 0, Step{99, 2, 50, {{0, 0}, {0, 0}}}) == Status::Ok);
  CHECK(journal.depth() == 8192u);
  CHECK(journal.dropped_units() == 1u);

  // 12,288 song edits (32 bytes each): 255 appends and then set_order edits that always change the value.
  ed.history_reset();
  const std::uint32_t dropped_before = journal.dropped_units();
  for (std::uint32_t i = 0; i < 255; ++i) {
    NEED(ed.append_order(1 + i % 5) == Status::Ok);
  }
  for (std::uint32_t i = 255; i < 12288; ++i) {
    const std::uint32_t pos = i % 255;
    NEED(ed.set_order(pos, project().order(pos) % 200u + 1u) == Status::Ok);
  }
  CHECK(journal.depth() == 12288u);
  CHECK(journal.dropped_units() == dropped_before);  // exactly full, nothing dropped
  CHECK(journal.used_bytes() == 12288u * 32u);

  // At least 10 song-edit levels, whatever the song edit (PAR-068: 10): the largest single edit of all
  // (deleting a full pattern, 14,412 bytes) leaves 27.
  ed.history_reset();
  for (int i = 0; i < 60; ++i) {
    NEED(ed.create_pattern(2, 128) == Status::Ok);
    NEED(ed.set_step(2, 0, 0, Step{1, 1, 1, {{0, 0}, {0, 0}}}) == Status::Ok);
    NEED(ed.delete_pattern(2) == Status::Ok);
  }
  CHECK(journal.depth() >= 27u);
  // Clearing a full song and dropping a pattern are single units too.
  ed.history_reset();
  for (std::uint32_t i = 0; i < 255; ++i) (void)ed.append_order(1);
  ed.history_reset();
  CHECK(ed.clear_order() == Status::Ok);
  CHECK(journal.used_bytes() == 284u);
  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(project().song_length() == 255u);
}

TEST_CASE("undo: every mutator is undone and redone exactly") {
  roundtrip("set_name", [](Editor& e) { CHECK(e.set_name("after") == Status::Ok); });
  roundtrip("set_name empty", [](Editor& e) { CHECK(e.set_name("") == Status::Ok); });
  roundtrip("set_tempo", [](Editor& e) { CHECK(e.set_tempo_x100(17500) == Status::Ok); });
  roundtrip("set_swing", [](Editor& e) { CHECK(e.set_swing(60) == Status::Ok); });
  roundtrip("set_default_pattern_length", [](Editor& e) { CHECK(e.set_default_pattern_length(48) == Status::Ok); });
  roundtrip("set_active_tracks", [](Editor& e) { CHECK(e.set_active_tracks(8) == Status::Ok); });
  roundtrip("set_master", [](Editor& e) { CHECK(e.set_master(model::MasterSettings{50, 60, 70, 80, 9}) == Status::Ok); });
  roundtrip("set_track", [](Editor& e) {
    model::TrackSlot t{model::TrackType::Midi, 1, 40, 20, 3, {}};
    std::memcpy(t.name, "bass", 5);
    CHECK(e.set_track(5, t) == Status::Ok);
  });
  roundtrip("set_track_name", [](Editor& e) { CHECK(e.set_track_name(2, "lead", 4) == Status::Ok); });
  roundtrip("append_order", [](Editor& e) { CHECK(e.append_order(2) == Status::Ok); });
  roundtrip("insert_order", [](Editor& e) { CHECK(e.insert_order(1, 7) == Status::Ok); });
  roundtrip("set_order", [](Editor& e) { CHECK(e.set_order(0, 9) == Status::Ok); });
  roundtrip("remove_order", [](Editor& e) { CHECK(e.remove_order(0) == Status::Ok); });
  roundtrip("clear_order", [](Editor& e) { CHECK(e.clear_order() == Status::Ok); });
  roundtrip("create_pattern", [](Editor& e) { CHECK(e.create_pattern(2 + 1, 24) == Status::Ok); });
  roundtrip("delete_pattern", [](Editor& e) { CHECK(e.delete_pattern(1) == Status::Ok); });
  roundtrip("set_pattern_name", [](Editor& e) { CHECK(e.set_pattern_name(2, "intro", 5) == Status::Ok); });
  roundtrip("set_track_length", [](Editor& e) { CHECK(e.set_track_length(1, 4, 64) == Status::Ok); });
  roundtrip("set_pattern_length", [](Editor& e) { CHECK(e.set_pattern_length(1, 100) == Status::Ok); });
  roundtrip("clear_pattern", [](Editor& e) { CHECK(e.clear_pattern(1) == Status::Ok); });
  roundtrip("set_step", [](Editor& e) { CHECK(e.set_step(2, 15, 31, Step{70, 2, 90, {{1, 1}, {2, 2}}}) == Status::Ok); });
  roundtrip("set_steps", [](Editor& e) {
    Step s[5] = {{61, 1, 1, {{0, 0}, {0, 0}}}, {62, 1, 1, {{0, 0}, {0, 0}}}, {63, 1, 1, {{0, 0}, {0, 0}}},
                 {64, 1, 1, {{0, 0}, {0, 0}}}, {65, 1, 1, {{0, 0}, {0, 0}}}};
    CHECK(e.set_steps(1, 2, 10, s, 5) == Status::Ok);
  });
  roundtrip("set_instrument new", [](Editor& e) {
    Instrument in;
    model::init_instrument(in, model::InstrumentKind::Midi);
    in.midi_channel = 4;
    CHECK(e.set_instrument(7, in) == Status::Ok);
  });
  roundtrip("set_instrument over", [](Editor& e) {
    Instrument in;
    model::init_instrument(in, model::InstrumentKind::Sample);
    in.sampler.volume = 33;
    std::memcpy(in.name, "snare", 6);
    CHECK(e.set_instrument(1, in) == Status::Ok);
  });
  roundtrip("clear_instrument", [](Editor& e) { CHECK(e.clear_instrument(1) == Status::Ok); });
}

TEST_CASE("undo: a rejected edit, or one that changes nothing, leaves no unit and keeps the redo history") {
  fresh_project();
  UndoJournal journal;
  NEED(journal.begin(g_ring, 8192));
  Editor ed(project(), journal);
  NEED(ed.create_pattern(1, 16) == Status::Ok);
  NEED(ed.set_tempo_x100(15000) == Status::Ok);
  NEED(ed.set_tempo_x100(16000) == Status::Ok);
  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(journal.depth() == 2u);
  CHECK(journal.redo_depth() == 1u);
  const std::size_t used = journal.used_bytes();

  CHECK(ed.set_tempo_x100(1) == Status::BadValue);                       // out of range
  CHECK(ed.set_step(9, 0, 0, Step{}) == Status::NotFound);               // no such pattern
  CHECK(ed.set_step(1, 16, 0, Step{}) == Status::BadIndex);
  CHECK(ed.delete_pattern(9) == Status::NotFound);
  CHECK(ed.clear_pattern(9) == Status::NotFound);
  CHECK(ed.set_name("a\0b", 3) == Status::BadValue);
  CHECK(ed.set_tempo_x100(15000) == Status::Ok);                          // the value it already has: applied, not recorded
  CHECK(ed.set_step(1, 0, 0, project().pattern(1)->steps[0][0]) == Status::Ok);
  CHECK(ed.set_steps(1, 0, 0, nullptr, 0) == Status::Ok);                 // an accepted no-op
  CHECK(ed.clear_order() == Status::Ok);                                  // an empty song
  CHECK(ed.set_swing(project().swing()) == Status::Ok);
  CHECK(journal.depth() == 2u);
  CHECK(journal.redo_depth() == 1u);  // none of that ended the redo history
  CHECK(journal.used_bytes() == used);
  CHECK(ed.redo() == UndoResult::Ok);
  CHECK(project().tempo_x100() == 16000u);

  // A real edit ends it.
  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(ed.set_swing(60) == Status::Ok);
  CHECK(journal.redo_depth() == 0u);
  CHECK(ed.redo() == UndoResult::Nothing);
}

TEST_CASE("undo: a group is one unit, groups nest, an empty group leaves nothing") {
  fresh_project();
  UndoJournal journal;
  NEED(journal.begin(g_ring, 16384));
  Editor ed(project(), journal);
  Snap& before = scratch(0);
  Snap& after = scratch(1);
  Snap& other = scratch(2);
  capture(project(), before);

  ed.begin_group(core::action_id("pattern.fill"), 7);
  CHECK(ed.group_depth() == 1u);
  CHECK(journal.group_open());
  CHECK(ed.create_pattern(1, 16) == Status::Ok);
  ed.begin_group(core::action_id("inner"), 1);  // the inner one does not make another unit, and does not name it
  CHECK(ed.group_depth() == 2u);
  CHECK(ed.set_step(1, 0, 0, Step{60, 1, 1, {{0, 0}, {0, 0}}}) == Status::Ok);
  CHECK(ed.set_step(1, 1, 1, Step{61, 1, 1, {{0, 0}, {0, 0}}}) == Status::Ok);
  ed.end_group();
  CHECK(journal.depth() == 0u);  // still open: the outer group has not ended
  CHECK(ed.append_order(1) == Status::Ok);
  CHECK(ed.undo() == UndoResult::Busy);  // not in the middle of a group
  CHECK(ed.redo() == UndoResult::Busy);
  ed.end_group();
  CHECK(ed.group_depth() == 0u);
  CHECK(journal.depth() == 1u);
  core::ActionId id = core::ActionId::None;
  core::ActionArg arg = 0;
  CHECK(journal.newest_undo(id, arg));
  CHECK(id == core::action_id("pattern.fill"));  // the outermost group's name and argument
  CHECK(arg == 7);
  CHECK_FALSE(journal.next_redo(id, arg));
  capture(project(), after);
  CHECK(ed.undo() == UndoResult::Ok);  // the whole group
  capture(project(), other);
  CHECK(same(before, other));
  CHECK(project().pattern_count() == 0u);
  CHECK(project().song_length() == 0u);
  CHECK_FALSE(journal.newest_undo(id, arg));
  CHECK(journal.next_redo(id, arg));
  CHECK(arg == 7);
  CHECK(ed.redo() == UndoResult::Ok);
  capture(project(), other);
  CHECK(same(after, other));

  // A mutator outside any group is a unit of its own, named "none".
  CHECK(ed.set_tempo_x100(15000) == Status::Ok);
  CHECK(journal.depth() == 2u);
  CHECK(journal.newest_undo(id, arg));
  CHECK(id == core::ActionId::None);

  // A group with nothing in it leaves nothing, and does not end the redo history.
  CHECK(ed.undo() == UndoResult::Ok);
  ed.begin_group(core::action_id("nothing"), 0);
  CHECK(ed.set_tempo_x100(1) == Status::BadValue);
  ed.end_group();
  CHECK(journal.depth() == 1u);
  CHECK(journal.redo_depth() == 1u);

  // end_group without begin_group does nothing.
  ed.end_group();
  CHECK(ed.group_depth() == 0u);
}

TEST_CASE("undo: many groups: each undo takes exactly one, in order") {
  fresh_project();
  UndoJournal journal;
  NEED(journal.begin(g_ring, 64 * 1024));
  Editor ed(project(), journal);
  for (std::uint32_t i = 1; i <= 5; ++i) {
    ed.begin_group(core::action_id("g"), static_cast<core::ActionArg>(i));
    NEED(ed.create_pattern(i, 8) == Status::Ok);
    NEED(ed.append_order(i) == Status::Ok);
    ed.end_group();
  }
  CHECK(journal.depth() == 5u);
  for (std::uint32_t i = 5; i >= 1; --i) {
    CHECK(project().pattern_count() == i);
    CHECK(project().song_length() == i);
    CHECK(ed.undo() == UndoResult::Ok);
  }
  CHECK(project().pattern_count() == 0u);
  CHECK(ed.undo() == UndoResult::Nothing);
  for (std::uint32_t i = 1; i <= 5; ++i) {
    CHECK(ed.redo() == UndoResult::Ok);
    CHECK(project().song_length() == i);
  }
  CHECK(ed.redo() == UndoResult::Nothing);
}

TEST_CASE("undo: when the ring fills the oldest units go, whole, and what is left still undoes exactly") {
  fresh_project();
  UndoJournal journal;
  constexpr std::size_t kRing = kTarget ? 480 : 1024;  // 48-byte units: 10 or 21 fit
  constexpr std::uint32_t kFit = static_cast<std::uint32_t>(kRing / 48);
  constexpr std::uint32_t kEdits = kTarget ? 18 : 30;
  NEED(journal.begin(g_ring, kRing));
  Editor ed(project(), journal);
  NEED(ed.create_pattern(1, 128) == Status::Ok);
  ed.history_reset();
  Snap* snaps = pool_base();  // (the property test's pool: the tests run one after the other)
  Snap& now = scratch(0);
  for (std::uint32_t i = 0; i < kEdits; ++i) {
    capture(project(), snaps[i]);  // the state before edit i
    NEED(ed.set_step(1, i % 16, i, Step{static_cast<std::uint8_t>(1 + i), 1, 1, {{0, 0}, {0, 0}}}) == Status::Ok);
  }
  CHECK(journal.depth() == kFit);
  CHECK(journal.dropped_units() == kEdits - kFit);
  CHECK(journal.lost_units() == 0u);
  for (std::uint32_t k = 0; k < kFit; ++k) {
    CHECK(ed.undo() == UndoResult::Ok);
    capture(project(), now);
    CHECK_MESSAGE(same(now, snaps[kEdits - 1 - k]), "level " << k);
  }
  CHECK(ed.undo() == UndoResult::Nothing);  // the older ones are gone
  for (std::uint32_t k = 0; k < kFit; ++k) {
    CHECK(ed.redo() == UndoResult::Ok);
  }
  capture(project(), now);
  CHECK(now.pats[0].steps[(kEdits - 1) % 16][kEdits - 1].note == kEdits);

  // Units of mixed sizes with the ring wrapping round its end, many times over: undo at every point matches the oracle.
  fresh_project();
  NEED(journal.begin(g_ring, 2000));  // not a multiple of the unit sizes: records straddle the end
  Editor ed2(project(), journal);
  NEED(ed2.create_pattern(1, 16) == Status::Ok);
  ed2.history_reset();
  test::Rng rng(5);
  for (int round = 0; round < 400; ++round) {
    Snap& prev = scratch(1);
    Snap& cur = scratch(2);
    capture(project(), prev);
    const std::uint32_t t = rng.below(16);
    const std::uint32_t kind = rng.below(4);
    if (kind == 0) {
      NEED(ed2.set_pattern_name(1, "abcdefghijklmnopqrstuvwxyz", 1 + rng.below(26)) == Status::Ok);
    } else if (kind == 1) {
      Step s[9];
      for (Step& x : s) x = random_step(rng);
      NEED(ed2.set_steps(1, t, rng.below(100), s, 1 + rng.below(9)) == Status::Ok);
    } else if (kind == 2) {
      NEED(ed2.set_step(1, t, rng.below(128), random_step(rng)) == Status::Ok);
    } else {
      NEED(ed2.append_order(1 + rng.below(5)) == Status::Ok || project().song_length() == 255u);
    }
    capture(project(), cur);
    if (!same(prev, cur) && round % 3 == 0) {
      CHECK(ed2.undo() == UndoResult::Ok);
      Snap& back = scratch(3);
      capture(project(), back);
      CHECK_MESSAGE(same(back, prev), "round " << round);
      CHECK(ed2.redo() == UndoResult::Ok);
      capture(project(), back);
      CHECK_MESSAGE(same(back, cur), "round " << round);
    }
  }
  CHECK(journal.dropped_units() > 100u);
  CHECK(journal.used_bytes() <= 2000u);
}

TEST_CASE("undo: a unit that cannot fit even an empty ring clears the history, and the edit stays done") {
  fresh_project();
  UndoJournal journal;
  NEED(journal.begin(g_ring, 4096));  // a pattern delete needs 14,412 bytes
  Editor ed(project(), journal);
  NEED(ed.create_pattern(1, 16) == Status::Ok);
  NEED(ed.set_tempo_x100(15000) == Status::Ok);
  CHECK(journal.depth() == 2u);
  CHECK(ed.delete_pattern(1) == Status::Ok);
  CHECK(project().pattern(1) == nullptr);   // done
  CHECK(journal.depth() == 0u);             // and no older unit survives to be applied to the changed project
  CHECK(journal.lost_units() == 1u);
  CHECK(ed.undo() == UndoResult::Nothing);
  CHECK(ed.clear_pattern(1) == Status::NotFound);
  // The journal goes on working afterwards.
  CHECK(ed.create_pattern(2, 8) == Status::Ok);
  CHECK(journal.depth() == 1u);
  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(project().pattern(2) == nullptr);

  // A group that grows past the ring is lost as a whole; its edits all happened; the rest of the group is not recorded.
  UndoJournal small;
  NEED(small.begin(g_ring, 512));
  fresh_project();
  Editor e2(project(), small);
  e2.begin_group(core::action_id("big"), 0);
  for (std::uint32_t i = 0; i < 50; ++i) {
    CHECK(e2.append_order(1 + i % 4) == Status::Ok);  // 12 bytes each + 20: 50 of them cannot fit 512
  }
  e2.end_group();
  CHECK(project().song_length() == 50u);
  CHECK(small.depth() == 0u);
  CHECK(small.lost_units() == 1u);
  CHECK(e2.undo() == UndoResult::Nothing);
  CHECK(e2.append_order(1) == Status::Ok);
  CHECK(small.depth() == 1u);
}

#if !defined(__arm__)
// The record count of a unit is 16 bits. 65,535 records (12 bytes each) fit a megabyte of ring and are one unit; a 65,536th makes
// the group too big to hold, like any other. (Host only: the target has no room for a megabyte of ring in this suite.)
alignas(8) std::uint8_t g_big_ring[1u << 20];

TEST_CASE("undo: a group of 65,535 records is one unit, one more is lost") {
  fresh_project();
  UndoJournal journal;
  NEED(journal.begin(g_big_ring, sizeof g_big_ring));
  Editor ed(project(), journal);
  NEED(ed.append_order(1) == Status::Ok);
  ed.history_reset();
  Snap& before = scratch(0);
  Snap& now = scratch(1);
  capture(project(), before);
  ed.begin_group(core::action_id("many"), 0);
  for (std::uint32_t i = 0; i < 65535; ++i) {
    NEED(ed.set_order(0, project().order(0) % 200u + 1u) == Status::Ok);  // always a different value
  }
  ed.end_group();
  CHECK(journal.depth() == 1u);
  CHECK(journal.lost_units() == 0u);
  CHECK(ed.undo() == UndoResult::Ok);
  capture(project(), now);
  CHECK(same(before, now));
  CHECK(ed.redo() == UndoResult::Ok);

  ed.begin_group(core::action_id("too many"), 0);
  for (std::uint32_t i = 0; i < 65536; ++i) {
    NEED(ed.set_order(0, project().order(0) % 200u + 1u) == Status::Ok);
  }
  ed.end_group();
  CHECK(journal.lost_units() == 1u);
  CHECK(journal.depth() == 0u);
  CHECK(ed.undo() == UndoResult::Nothing);
}
#endif

TEST_CASE("undo: a project changed behind the editor's back makes the history stale, not wrong") {
  fresh_project();
  UndoJournal journal;
  NEED(journal.begin(g_ring, 8192));
  Editor ed(project(), journal);
  NEED(ed.create_pattern(1, 8) == Status::Ok);
  NEED(ed.set_tempo_x100(15000) == Status::Ok);
  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(ed.can_undo());
  CHECK(ed.can_redo());
  CHECK(ed.in_sync());
  CHECK(project().set_swing(60) == Status::Ok);  // not through the editor
  CHECK_FALSE(ed.in_sync());
  CHECK_FALSE(ed.can_undo());  // a history that no longer matches the project cannot be applied, and does not say it can
  CHECK_FALSE(ed.can_redo());
  CHECK(journal.depth() == 1u);  // (it is still there until the editor next looks)
  Snap& before = scratch(0);
  Snap& after = scratch(1);
  capture(project(), before);
  CHECK(ed.undo() == UndoResult::Stale);
  capture(project(), after);
  CHECK(same(before, after));  // nothing was applied to what it no longer matches
  CHECK(journal.depth() == 0u);
  CHECK(journal.redo_depth() == 0u);
  CHECK(ed.stale_resets() == 1u);
  CHECK(ed.undo() == UndoResult::Nothing);

  // An edit through the editor notices it too, and starts a fresh history.
  CHECK(ed.set_tempo_x100(16000) == Status::Ok);
  CHECK(journal.depth() == 1u);
  project().reset();  // a load
  CHECK(ed.set_name("x") == Status::Ok);
  CHECK(ed.stale_resets() == 2u);
  CHECK(journal.depth() == 1u);  // only the edit made after the reset
  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(project().name()[0] == '\0');

  // history_reset() after a deliberate load: no stale report, the next edit is undoable.
  project().reset();
  CHECK_FALSE(ed.in_sync());
  ed.history_reset();
  CHECK(ed.in_sync());
  CHECK(ed.set_name("y") == Status::Ok);
  CHECK(ed.stale_resets() == 2u);
  CHECK(ed.undo() == UndoResult::Ok);
}

TEST_CASE("undo: a damaged ring is refused, never applied") {
  fresh_project();
  UndoJournal journal;
  NEED(journal.begin(g_ring, 8192));
  Editor ed(project(), journal);
  NEED(ed.create_pattern(1, 8) == Status::Ok);
  NEED(ed.set_tempo_x100(15000) == Status::Ok);
  NEED(ed.set_swing(60) == Status::Ok);
  Snap& after = scratch(1);
  g_ring[14] ^= 0xFF;  // the check field of the first unit's header (at offset 0 of a fresh ring), damaged...
  g_ring[15] ^= 0xFF;
  CHECK(ed.undo() == UndoResult::Ok);        // ...the newest unit is a different one and is fine
  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(ed.undo() == UndoResult::Failed);    // the damaged one: refused
  CHECK(journal.damaged() == 1u);
  CHECK(journal.depth() == 0u);
  CHECK(ed.failures() == 1u);
  capture(project(), after);
  CHECK(after.swing == 50);                  // the two good units were undone, the damaged one not applied
  CHECK(after.tempo == model::kDefaultTempoX100);
  CHECK(after.pattern_count == 1u);

  // The trailer lengths of a unit disagree with its header: also refused.
  fresh_project();
  NEED(journal.begin(g_ring, 8192));
  Editor ed2(project(), journal);
  NEED(ed2.set_tempo_x100(15000) == Status::Ok);
  g_ring[UndoJournal::unit_bytes_for(8) - 4] ^= 0x04;  // the unit's trailer length
  CHECK(ed2.undo() == UndoResult::Failed);
  CHECK(project().tempo_x100() == 15000u);
}

// ---- the property test ------------------------------------------------------------------------------------------------------

namespace {

struct Counters {
  std::uint32_t edits = 0, groups = 0, undos = 0, redos = 0, stale = 0, nothing = 0, drops = 0, lost = 0, units = 0, rejected = 0, exact = 0;
};

// The ring in which the episodes that must undo all the way to the start run (nothing dropped for room, nothing tampered with).
constexpr std::size_t kExactRing = kTarget ? 24000 : model::kUndoRegionBytes;

// One episode: a random project, a ring of a random size, `ops` random operations, each checked against full snapshots. The
// oracle keeps a snapshot of the project before every unit the journal should hold (`past`) and after every unit undone
// (`future`). An operation that changed the project must have made a unit (unless the unit was too big for the ring and the
// history went); one that changed nothing must have made none; undo and redo must restore the snapshot, exactly.
Counters episode(test::Rng& rng, unsigned ops, std::size_t ring, bool allow_tamper) {
  Counters c;
  fresh_project();
  // Some starting content, made without the journal.
  for (unsigned id = 1; id <= kIds; ++id) {
    if (rng.below(3) != 0) {
      (void)project().create_pattern(id, 1 + rng.below(128));
      for (int k = 0; k < 20; ++k) (void)project().set_step(id, rng.below(16), rng.below(128), random_step(rng));
    }
  }
  for (int k = 0; k < 6; ++k) (void)project().append_order(1 + rng.below(kIds + 2));
  for (unsigned i = 0; i < kInst; ++i) {
    if (rng.below(2) == 0) {
      Instrument in = make_instrument(rng);
      (void)project().set_instrument(i, in);
    }
  }
  UndoJournal journal;
  NEED_OR(journal.begin(g_ring, ring), c);
  Editor ed(project(), journal);

  Snap* pool = pool_base();
  SnapStack past(pool, kMaxDepth + 2), future(pool + kMaxDepth + 2, kMaxDepth + 2);
  Snap& cur = scratch(0);
  Snap& before = scratch(1);
  Snap& after = scratch(2);
  Snap& probe = scratch(3);
  Snap& first = scratch(4);
  capture(project(), cur);
  first = cur;
  std::uint32_t lost0 = 0;
  std::uint32_t dropped0 = 0;
  bool exact_history = true;  // nothing was dropped, lost or cleared: undoing everything must reach `first`

  // Brings the oracle in line with what an edit or a group did, and checks the journal agrees. The journal says whether a unit
  // was made (a group that nets out to nothing still is one); the project must not have changed without one.
  const auto settle = [&](const Snap& was) {
    capture(project(), after);
    CHECK(project().check_invariants());
    const bool changed = !same(was, after);
    const std::uint32_t drops_now = journal.dropped_units() - dropped0;
    if (journal.lost_units() != lost0) {
      past.clear();  // too big to hold: the whole history went, the edit stayed
      future.clear();
      CHECK(journal.depth() == 0u);
      CHECK(journal.redo_depth() == 0u);
      lost0 = journal.lost_units();
      exact_history = false;
      ++c.lost;
    } else if (journal.depth() + drops_now == past.size() + 1u) {
      future.clear();  // a new unit ends the redo history
      past.push(was);
      ++c.units;
    } else {
      // No unit: nothing can have changed, and the redo history is as it was.
      CHECK_MESSAGE(!changed, "op " << g_last_op << " changed the project and left no unit");
      CHECK(journal.depth() + drops_now == past.size());
      CHECK(journal.redo_depth() == future.size());
      ++c.rejected;
    }
    // Units the journal dropped for room are gone from the oldest end of the oracle too.
    while (past.size() > journal.depth()) {
      past.drop_oldest();
      ++c.drops;
      exact_history = false;
    }
    dropped0 = journal.dropped_units();
    CHECK(past.size() == journal.depth());
    CHECK(future.size() == journal.redo_depth());
    cur = after;
  };

  for (unsigned step = 0; step < ops; ++step) {
    if (past.size() + 2 >= kMaxDepth) {
      break;  // the oracle's room is spent
    }
    const std::uint32_t pick = rng.below(100);
    if (pick < 52) {
      before = cur;
      random_edit(ed, rng);
      ++c.edits;
      settle(before);
    } else if (pick < 66) {
      before = cur;
      ed.begin_group(core::action_id("group"), static_cast<core::ActionArg>(step));
      const unsigned n = 1 + rng.below(4);
      for (unsigned k = 0; k < n; ++k) {
        random_edit(ed, rng);
        if (rng.below(8) == 0) {
          ed.begin_group(core::action_id("nested"), 0);
          random_edit(ed, rng);
          ed.end_group();
        }
      }
      ed.end_group();
      ++c.groups;
      settle(before);
    } else if (pick < 80) {
      const bool can = journal.depth() != 0;
      const UndoResult r = ed.undo();
      if (!can) {
        CHECK(r == UndoResult::Nothing);
        ++c.nothing;
      } else {
        CHECK(r == UndoResult::Ok);
        NEED_OR(!past.empty(), c);
        capture(project(), probe);
        CHECK_MESSAGE(same(probe, past.back()), "undo " << c.undos << " is not exact");
        future.push(cur);
        cur = past.back();
        past.pop();
        ++c.undos;
        CHECK(project().check_invariants());
      }
      CHECK(past.size() == journal.depth());
      CHECK(future.size() == journal.redo_depth());
    } else if (pick < 92) {
      const bool can = journal.redo_depth() != 0;
      const UndoResult r = ed.redo();
      if (!can) {
        CHECK(r == UndoResult::Nothing);
        ++c.nothing;
      } else {
        CHECK(r == UndoResult::Ok);
        NEED_OR(!future.empty(), c);
        capture(project(), probe);
        CHECK_MESSAGE(same(probe, future.back()), "redo " << c.redos << " is not exact");
        past.push(cur);
        cur = future.back();
        future.pop();
        ++c.redos;
        CHECK(project().check_invariants());
      }
      CHECK(past.size() == journal.depth());
      CHECK(future.size() == journal.redo_depth());
    } else if (pick < 95 && allow_tamper) {
      // Something changes the project behind the editor's back: undo and redo then refuse, change nothing and clear.
      (void)project().set_tempo_x100(model::kMinTempoX100 + rng.below(1000));
      capture(project(), before);
      const UndoResult r = rng.below(2) == 0 ? ed.undo() : ed.redo();
      CHECK(r == UndoResult::Stale);
      capture(project(), probe);
      CHECK(same(before, probe));
      past.clear();
      future.clear();
      cur = before;
      exact_history = false;
      CHECK(journal.depth() == 0u);
      CHECK(journal.redo_depth() == 0u);
      ++c.stale;
    } else if (pick < 98) {
      // A group whose every edit is rejected leaves no unit and keeps the redo history.
      const unsigned redo0 = journal.redo_depth();
      before = cur;
      ed.begin_group(core::action_id("rejected"), 0);
      (void)ed.set_tempo_x100(1);
      (void)ed.set_step(0, 0, 0, Step{});
      (void)ed.delete_pattern(200);
      ed.end_group();
      capture(project(), probe);
      CHECK(same(before, probe));
      CHECK(journal.redo_depth() == redo0);
      CHECK(journal.depth() == past.size());
    }
  }
  // Undo everything the journal holds: each step is exact; when nothing was ever dropped it ends at the start, exactly.
  while (journal.depth() != 0) {
    CHECK(ed.undo() == UndoResult::Ok);
    capture(project(), probe);
    NEED_OR(!past.empty(), c);
    CHECK(same(probe, past.back()));
    future.push(cur);
    cur = past.back();
    past.pop();
    ++c.undos;
  }
  if (exact_history) {
    capture(project(), probe);
    CHECK_MESSAGE(same(probe, first), "undoing every edit did not reach the start");
    c.exact += c.units != 0 ? 1u : 0u;
  }
  // And redo everything: back to where the last edit left it.
  while (journal.redo_depth() != 0) {
    CHECK(ed.redo() == UndoResult::Ok);
    capture(project(), probe);
    NEED_OR(!future.empty(), c);
    CHECK(same(probe, future.back()));
    past.push(cur);
    cur = future.back();
    future.pop();
    ++c.redos;
  }
  CHECK(ed.failures() == 0u);
  CHECK(journal.damaged() == 0u);
  return c;
}

}  // namespace

TEST_CASE("undo: property test: random edits, groups, undo and redo against full snapshots") {
#if defined(__arm__)
  constexpr unsigned kEpisodes = 80;
  constexpr unsigned kOps = 40;
  static const std::size_t rings[] = {96, 512, 2048, 6000, kExactRing};
#else
  unsigned scale = 1;
  if (const char* env = std::getenv("O3OS_FUZZ_SCALE")) {
    const int v = std::atoi(env);
    scale = v > 0 ? static_cast<unsigned>(v) : 1u;
  }
  const unsigned kEpisodes = 1500 * scale;
  constexpr unsigned kOps = 150;
  static const std::size_t rings[] = {256, 1024, 3000, 9000, 40000, kExactRing};
#endif
  test::Rng rng(0x0D01CE5u);
  Counters total;
  for (unsigned e = 0; e < kEpisodes; ++e) {
    const std::size_t ring = rings[e % (sizeof rings / sizeof rings[0])];
    // The biggest ring is the exact one: nothing is dropped and nothing goes stale, so undoing everything must reach the start.
    const Counters c = episode(rng, kOps, ring, ring != kExactRing);
    total.edits += c.edits;
    total.groups += c.groups;
    total.undos += c.undos;
    total.redos += c.redos;
    total.stale += c.stale;
    total.nothing += c.nothing;
    total.drops += c.drops;
    total.lost += c.lost;
    total.units += c.units;
    total.rejected += c.rejected;
    total.exact += c.exact;
  }
  // The test was not vacuous: units were made, undone and redone, dropped for room, lost for size, found stale.
  CHECK(total.units > kEpisodes * 2u);
  CHECK(total.undos > kEpisodes * 2u);
  CHECK(total.redos > kEpisodes);
  CHECK(total.drops > 0u);
  CHECK(total.lost > 0u);
  CHECK(total.stale > 0u);
  CHECK(total.rejected > 0u);
  CHECK(total.exact > kEpisodes / 20u);  // episodes in which nothing was dropped or cleared: undo everything reached the start
#if !defined(__arm__)
  if (std::getenv("O3OS_UNDO_REPORT") != nullptr) {
    std::printf(
        "undo property: %u episodes, %u edits + %u groups = %u operations (%u units recorded, %u rejected or no-ops), "
        "%u undos, %u redos, %u stale, %u dropped for room, %u lost for size, %u episodes undone all the way to the start\n",
        kEpisodes, total.edits, total.groups, total.edits + total.groups, total.units, total.rejected, total.undos,
        total.redos, total.stale, total.drops, total.lost, total.exact);
  }
#endif
}
