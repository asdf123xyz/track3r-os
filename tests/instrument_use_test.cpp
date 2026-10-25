// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-FILES part B, PURGE UNUSED: model/instrument_use.h names every reference to an instrument slot a project has, and these cases hold one for each. An instrument is purged only when NOTHING below names
// it: a step in any pattern (the song need not play it), on any track slot, at any step (past the track's length too), a Random Instrument window, a MIDI Learn binding of its parameters. A module instrument is
// never purged.
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/project_fixture.h"

#include <o3os/core/learn.h>
#include <o3os/core/param.h>
#include <o3os/model/instrument_use.h>
#include <o3os/model/project.h>
#include <o3os/seq/fx.h>

#include <cstring>

using namespace o3os;
using model::Instrument;
using model::InstrumentKind;
using model::Project;
using model::Step;

static_assert(model::kFxRandomInstrument == seq::fx::kRandomInstrument, "the model's copy of the Random Instrument type is the sequencer's");

namespace {

constexpr std::uint64_t bit(unsigned slot) { return std::uint64_t{1} << slot; }

Project& fresh() {
  Project& p = test::arena(2).project;
  p.reset();
  return p;
}

void put(Project& p, unsigned slot, InstrumentKind kind, const char* name = "I") {
  Instrument in;
  model::init_instrument(in, kind);
  std::strcpy(in.name, name);
  if (kind == InstrumentKind::Midi) {
    in.midi_channel = 1;
  }
  if (kind == InstrumentKind::Module) {
    if (p.module_count() == 0u) {
      NEED(p.add_module("tb303", 5, 1, 0) == model::Status::Ok);
    }
    in.module_index = 0;
  }
  NEED(p.set_instrument(slot, in) == model::Status::Ok);
}

Step step(unsigned instr, unsigned fx0 = 0, unsigned v0 = 0, unsigned fx1 = 0, unsigned v1 = 0) {
  Step s = model::kEmptyStep;
  s.note = model::kNoteFirst + 48;
  s.instr = static_cast<std::uint8_t>(instr);
  s.fx[0] = {static_cast<std::uint8_t>(fx0), static_cast<std::uint8_t>(v0)};
  s.fx[1] = {static_cast<std::uint8_t>(fx1), static_cast<std::uint8_t>(v1)};
  return s;
}

void set(Project& p, unsigned pattern, unsigned track, unsigned at, const Step& s) {
  if (p.pattern(pattern) == nullptr) {
    NEED(p.create_pattern(pattern, 16) == model::Status::Ok);
  }
  NEED(p.set_step(pattern, track, at, s) == model::Status::Ok);
}

}  // namespace

TEST_CASE("instrument use: an empty project names nothing and every sample and MIDI instrument is unused") {
  Project& p = fresh();
  CHECK(model::instruments_in_use(p) == 0u);
  put(p, 0, InstrumentKind::Sample);
  put(p, 5, InstrumentKind::Midi);
  put(p, 63, InstrumentKind::Sample);
  CHECK(model::instruments_in_use(p) == 0u);
  unsigned modules = 99;
  CHECK(model::instruments_unused(p, &modules) == (bit(0) | bit(5) | bit(63)));
  CHECK(modules == 0u);
  CHECK(model::instruments_unused(p) == (bit(0) | bit(5) | bit(63)));  // (the count is optional)
  // a free slot is never "unused": it holds nothing
  CHECK((model::instruments_unused(p) & bit(1)) == 0u);
}

TEST_CASE("instrument use: reference 1, a step names its instrument: first and last slot, any track, any step, any pattern, the song or not") {
  Project& p = fresh();
  for (unsigned slot : {0u, 1u, 2u, 3u, 62u, 63u}) {
    put(p, slot, InstrumentKind::Sample);
  }
  put(p, 10, InstrumentKind::Midi);
  set(p, 1, 0, 0, step(1));              // slot 0 (byte 1): the first step of the first track
  set(p, 200, 15, 15, step(64));         // slot 63 (byte 64): a pattern the song does not name, the last track slot, past nothing
  CHECK(model::instruments_in_use(p) == (bit(0) | bit(63)));
  set(p, 3, 7, 3, step(11));             // slot 10 (MIDI), a middle pattern
  CHECK(model::instruments_in_use(p) == (bit(0) | bit(10) | bit(63)));
  CHECK(model::instruments_unused(p) == (bit(1) | bit(2) | bit(3) | bit(62)));
  // a step with no instrument names none, and a byte above 64 names none (steps are carried verbatim)
  set(p, 4, 0, 0, step(0));
  set(p, 4, 0, 1, step(65));
  set(p, 4, 0, 2, step(255));
  CHECK(model::instruments_in_use(p) == (bit(0) | bit(10) | bit(63)));
  // a pattern in the pool the song never plays, in the song or not: the steps count the same
  NEED(p.append_order(1) == model::Status::Ok);
  CHECK(model::instruments_in_use(p) == (bit(0) | bit(10) | bit(63)));
}

TEST_CASE("instrument use: a step past its track's length, and on a track past the active tracks, still names its instrument") {
  Project& p = fresh();
  put(p, 4, InstrumentKind::Sample);
  put(p, 5, InstrumentKind::Sample);
  NEED(p.create_pattern(1, 8) == model::Status::Ok);
  NEED(p.set_active_tracks(4) == model::Status::Ok);
  NEED(p.set_step(1, 2, 127, step(5)) == model::Status::Ok);  // step 127 of a track that is 8 steps long: kept, a lengthening would bring it back
  NEED(p.set_step(1, 14, 0, step(6)) == model::Status::Ok);   // track 15 of 4 active
  CHECK(model::instruments_in_use(p) == (bit(4) | bit(5)));
  CHECK(model::instruments_unused(p) == 0u);
}

TEST_CASE("instrument use: a step that is deleted again, or a pattern deleted, frees its instrument") {
  Project& p = fresh();
  put(p, 2, InstrumentKind::Sample);
  set(p, 1, 0, 0, step(3));
  CHECK(model::instruments_in_use(p) == bit(2));
  NEED(p.set_step(1, 0, 0, model::kEmptyStep) == model::Status::Ok);
  CHECK(model::instruments_in_use(p) == 0u);
  set(p, 1, 0, 0, step(3));
  NEED(p.delete_pattern(1) == model::Status::Ok);
  CHECK(model::instruments_in_use(p) == 0u);
  CHECK(model::instruments_unused(p) == bit(2));
}

TEST_CASE("instrument use: reference 2, Random Instrument names the window around the step's instrument, in either FX column, and both columns add") {
  const std::uint8_t ri = seq::fx::kRandomInstrument;
  Project& p = fresh();
  for (unsigned slot = 0; slot < 64; ++slot) {
    put(p, slot, InstrumentKind::Sample);
  }
  set(p, 1, 0, 0, step(21, ri, 2));  // byte 21 = slot 20, column 1: 18..22
  CHECK(model::instruments_in_use(p) == (bit(18) | bit(19) | bit(20) | bit(21) | bit(22)));
  fresh();
  for (unsigned slot = 0; slot < 64; ++slot) {
    put(p, slot, InstrumentKind::Sample);
  }
  set(p, 1, 0, 0, step(21, 0, 0, ri, 1));  // column 2: 19..21
  CHECK(model::instruments_in_use(p) == (bit(19) | bit(20) | bit(21)));
  fresh();
  for (unsigned slot = 0; slot < 64; ++slot) {
    put(p, slot, InstrumentKind::Sample);
  }
  set(p, 1, 0, 0, step(21, ri, 2, ri, 3));  // both: the second column picks around the first's pick: 15..25
  std::uint64_t want = 0;
  for (unsigned s = 15; s <= 25; ++s) {
    want |= bit(s);
  }
  CHECK(model::instruments_in_use(p) == want);
  // value 0: just the instrument; a window clamps at both ends of the table
  fresh();
  for (unsigned slot = 0; slot < 64; ++slot) {
    put(p, slot, InstrumentKind::Sample);
  }
  set(p, 1, 0, 0, step(1, ri, 0));
  CHECK(model::instruments_in_use(p) == bit(0));
  set(p, 1, 1, 0, step(1, ri, 2));
  CHECK(model::instruments_in_use(p) == (bit(0) | bit(1) | bit(2)));
  set(p, 1, 2, 0, step(64, ri, 2));
  CHECK(model::instruments_in_use(p) == (bit(0) | bit(1) | bit(2) | bit(61) | bit(62) | bit(63)));
  // a different FX type with a value names nothing around it
  fresh();
  put(p, 4, InstrumentKind::Sample);
  put(p, 5, InstrumentKind::Sample);
  set(p, 1, 0, 0, step(5, seq::fx::kRandomNote, 3));
  CHECK(model::instruments_in_use(p) == bit(4));
  CHECK(model::instruments_unused(p) == bit(5));
}

TEST_CASE("instrument use: a Random Instrument on a step with no instrument plays around the instrument the track played last, so the slots that track can reach are all named") {
  const std::uint8_t ri = seq::fx::kRandomInstrument;
  Project& p = fresh();
  for (unsigned slot = 0; slot < 64; ++slot) {
    put(p, slot, InstrumentKind::Sample);
  }
  set(p, 1, 0, 0, step(11));               // track 0 plays slot 10
  set(p, 1, 0, 4, step(0, ri, 1));         // and later "the last one" at random, one either way
  // 10, then 9..11, then (the pick can be played last again) 8..12, ... the closure under a window of one: it can drift to the ends of the table
  CHECK(model::instruments_in_use(p) == ~std::uint64_t{0});
  // another track with no instrument of its own and no earlier note reaches nothing
  fresh();
  for (unsigned slot = 0; slot < 64; ++slot) {
    put(p, slot, InstrumentKind::Sample);
  }
  set(p, 1, 3, 0, step(0, ri, 5));
  CHECK(model::instruments_in_use(p) == 0u);
  // a window of zero on an idle step changes nothing
  set(p, 1, 0, 0, step(11));
  set(p, 1, 0, 4, step(0, ri, 0));
  CHECK(model::instruments_in_use(p) == bit(10));
  // the other tracks are not drawn in
  set(p, 1, 1, 0, step(40, 0, 0));
  CHECK(model::instruments_in_use(p) == (bit(10) | bit(39)));
}

TEST_CASE("instrument use: reference 3, a MIDI Learn binding of an instrument's parameter names it, a follow binding names none") {
  Project& p = fresh();
  put(p, 7, InstrumentKind::Sample);
  put(p, 8, InstrumentKind::Sample);
  core::LearnBinding b{};
  b.param = core::param_id(core::ParamKind::Instrument, 7, model::instrument_param::Volume);
  b.cc = 20;
  b.channel = 0;
  b.mode = static_cast<std::uint8_t>(core::LearnMode::Pickup);
  b.flags = 0;
  NEED(core::learn_valid(b));
  NEED(p.set_learn_slot(0, b) == model::Status::Ok);
  CHECK(model::instruments_in_use(p) == bit(7));
  CHECK(model::instruments_unused(p) == bit(8));
  // a follow binding acts on whichever instrument is selected: it names none
  core::LearnBinding f = b;
  f.param = core::param_id(core::ParamKind::Instrument, 0, model::instrument_param::Volume);
  f.cc = 21;
  f.flags = core::kLearnFollow;
  NEED(core::learn_valid(f));
  NEED(p.set_learn_slot(1, f) == model::Status::Ok);
  CHECK(model::instruments_in_use(p) == bit(7));
  // a binding of a track or a master parameter names no instrument either
  core::LearnBinding t = b;
  t.param = core::param_id(core::ParamKind::Project, 0, 0);
  t.cc = 22;
  if (core::learn_valid(t)) {
    (void)p.set_learn_slot(2, t);
  }
  CHECK(model::instruments_in_use(p) == bit(7));
  // cleared again: free
  NEED(p.set_learn_slot(0, core::LearnBinding{}) == model::Status::Ok);
  CHECK(model::instruments_in_use(p) == 0u);
}

TEST_CASE("instrument use: nothing else in a project names an instrument (song, tracks, master, the project's state)") {
  Project& p = fresh();
  put(p, 0, InstrumentKind::Sample);
  put(p, 1, InstrumentKind::Midi);
  // a song of patterns that have no steps, the tracks, the master, the settings values: none of them holds an instrument slot
  NEED(p.create_pattern(1, 16) == model::Status::Ok);
  NEED(p.append_order(1) == model::Status::Ok);
  NEED(p.set_tempo_x100(14000) == model::Status::Ok);
  NEED(p.set_track_name(3, "Bass", 4) == model::Status::Ok);
  model::PerfState perf;
  model::init_perf_state(perf);
  perf.fx[0] = 3;
  perf.row[0] = 2;
  perf.value[0][1] = 40;
  NEED(p.set_perf(perf) == model::Status::Ok);
  model::ScenesState scenes;
  model::init_scenes_state(scenes);
  scenes.scene[0].stored = 1;
  scenes.scene[0].slots = perf;
  NEED(p.set_scenes(scenes) == model::Status::Ok);  // the Perform slots and scenes act on tracks, not on instruments
  model::MixState mix;
  model::init_mix_state(mix);
  mix.route[2] = 1;
  NEED(p.set_mix(mix) == model::Status::Ok);
  model::NoteState note;
  model::init_note_state(note);
  note.root = 5;
  NEED(p.set_note(note) == model::Status::Ok);
  CHECK(model::instruments_in_use(p) == 0u);
  CHECK(model::instruments_unused(p) == (bit(0) | bit(1)));
}

TEST_CASE("instrument use: a module instrument is never purged, used or not, and is counted") {
  Project& p = fresh();
  put(p, 0, InstrumentKind::Module, "TB 303");
  put(p, 1, InstrumentKind::Module, "Pads");
  put(p, 2, InstrumentKind::Sample);
  set(p, 1, 0, 0, step(2));  // the second module is played
  unsigned modules = 0;
  CHECK(model::instruments_unused(p, &modules) == bit(2));
  CHECK(modules == 1u);  // the one nothing names; the played one is not counted
  CHECK(model::instruments_in_use(p) == bit(1));
}

TEST_CASE("instrument use: a full project of 64 sample instruments and 128 full patterns is read in one go") {
  Project& p = test::arena(2).project;
  test::fill_maximal(p);
  const std::uint64_t used = model::instruments_in_use(p);
  CHECK(used != 0u);
  const std::uint64_t unused = model::instruments_unused(p);
  CHECK((used & unused) == 0u);
}

TEST_CASE("instrument use: slots the caller holds (the S7 instrument clipboard's audio) are kept too; there is no pattern clipboard in the model, and an app's own instrument numbers are outside it") {
  Project& p = fresh();
  put(p, 3, InstrumentKind::Sample);
  put(p, 4, InstrumentKind::Sample);
  put(p, 5, InstrumentKind::Midi);
  CHECK(model::instruments_unused(p) == (bit(3) | bit(4) | bit(5)));
  CHECK(model::instruments_unused(p, nullptr, bit(4)) == (bit(3) | bit(5)));
  CHECK(model::instruments_unused(p, nullptr, bit(3) | bit(4) | bit(5)) == 0u);
  CHECK(model::instruments_in_use(p) == 0u);  // (held is not "in use": the model's own references only)
}
