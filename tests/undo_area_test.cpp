// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-UNDOGRP at the model's level: one undo history of three rings (PATTERN, SONG, SOUND), the click groups that fold an encoder's sweep into one unit,
// the structure fence between the areas, and the bounded memory of each ring. The app's side (which screen belongs to which area, the 1500 ms rule, the
// Mixer, the strip texts) is undo_group_app_test.cpp; the journal and the Editor on their own are undo_test.cpp.
#include <doctest/doctest.h>

#include <o3os/core/action.h>
#include <o3os/core/param.h>
#include <o3os/model/editor.h>
#include <o3os/model/params.h>
#include <o3os/model/project.h>
#include <o3os/model/undo.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "support/need.h"
#include "support/project_fixture.h"
#include "support/rng.h"

using namespace o3os;
using model::Editor;
using model::Status;
using model::Step;
using model::UndoArea;
using model::UndoHistory;
using model::UndoResult;
namespace ip = model::instrument_param;

namespace {

alignas(8) std::uint8_t g_region[model::kUndoRegionBytes];

model::Project& project() { return test::arena(2).project; }

core::ParamId cutoff(unsigned slot) { return core::param_id(core::ParamKind::Instrument, slot, ip::Cutoff); }
core::ParamId volume(unsigned slot) { return core::param_id(core::ParamKind::Instrument, slot, ip::Volume); }

Step note(std::uint8_t n) {
  Step s = model::kEmptyStep;
  s.note = n;
  s.instr = 1;
  return s;
}

// A project with three patterns, a two-slot song and three sample instruments; the history is empty.
void fresh(std::unique_ptr<Editor>& ed, UndoHistory& h) {
  project().reset();
  NEED(h.begin(g_region, sizeof g_region));
  ed = std::make_unique<Editor>(project(), h);
  for (unsigned id = 1; id <= 3; ++id) {
    NEED(ed->create_pattern(id, 16) == Status::Ok);
  }
  NEED(ed->append_order(1) == Status::Ok);
  NEED(ed->append_order(2) == Status::Ok);
  for (unsigned i = 0; i < 3; ++i) {
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Sample);
    NEED(ed->set_instrument(i, in) == Status::Ok);
  }
  ed->history_reset();
}

// One parameter click the way the app makes it: a group in the SOUND area with the merge hint on the row.
Status click(Editor& ed, core::ParamId id, float value) {
  ed.begin_group(core::action_id("param.edit"), 1, UndoArea::Sound);
  ed.merge_hint(id);
  const Status st = ed.set_param(id, value);
  ed.end_group();
  return st;
}

Status step_edit(Editor& ed, unsigned pattern, unsigned track, unsigned at, const Step& s) {
  ed.begin_group(core::action_id("step.note"), 1, UndoArea::Pattern);
  const Status st = ed.set_step(pattern, track, at, s);
  ed.end_group();
  return st;
}

Status song_edit(Editor& ed, std::uint32_t tempo_x100) {
  ed.begin_group(core::action_id("song.tempo"), 1, UndoArea::Song);
  const Status st = ed.set_tempo_x100(tempo_x100);
  ed.end_group();
  return st;
}

}  // namespace

TEST_CASE("undo areas: the history is three rings carved out of the 384 KiB region, 160 / 96 / 128 KiB") {
  UndoHistory h;
  CHECK(h.begin(g_region, sizeof g_region));
  CHECK(h.ring[0].capacity_bytes() == 160u * 1024u);
  CHECK(h.ring[1].capacity_bytes() == 96u * 1024u);
  CHECK(h.ring[2].capacity_bytes() == 128u * 1024u);
  CHECK(h.ring[0].capacity_bytes() + h.ring[1].capacity_bytes() + h.ring[2].capacity_bytes() == model::kUndoRegionBytes);
  CHECK(model::kUndoPatternBytes + model::kUndoSongBytes + model::kUndoSoundBytes == model::kUndoRegionBytes);
  // a region that cannot make three usable rings is refused
  CHECK_FALSE(h.begin(g_region, 100));
  CHECK_FALSE(h.begin(g_region + 1, 4096));
  CHECK_FALSE(h.begin(nullptr, 4096));
  CHECK_FALSE(h.begin(g_region, 4098));
}

TEST_CASE("undo groups: 48 clicks on one row are one unit; one undo returns to the value before the sweep, redo to the last, one parameter is applied") {
  UndoHistory h;
  std::unique_ptr<Editor> ed;
  fresh(ed, h);
  const float start = project().get_param(cutoff(0));
  for (int i = 1; i <= 48; ++i) {
    NEED(click(*ed, cutoff(0), 0.2f + 0.01f * static_cast<float>(i)) == Status::Ok);
  }
  const float last = project().get_param(cutoff(0));
  CHECK(last != start);
  CHECK(h.ring[2].depth() == 1u);
  CHECK(h.ring[0].depth() == 0u);
  CHECK(h.ring[1].depth() == 0u);
  CHECK(h.ring[2].used_bytes() == model::UndoJournal::unit_bytes_for(12));  // 40 bytes: one record, patched in place
  CHECK(ed->undo(UndoArea::Sound) == UndoResult::Ok);
  CHECK(project().get_param(cutoff(0)) == start);
  CHECK(ed->applied_count() == 1u);
  CHECK(ed->applied(0).id == cutoff(0));
  CHECK(ed->applied(0).value == start);
  CHECK(ed->redo(UndoArea::Sound) == UndoResult::Ok);
  CHECK(project().get_param(cutoff(0)) == last);
  CHECK(ed->applied_count() == 1u);
  CHECK(ed->undo(UndoArea::Sound) == UndoResult::Ok);
  CHECK(ed->undo(UndoArea::Sound) == UndoResult::Nothing);
}

TEST_CASE("undo groups: rows that alternate make a unit each, a sweep that comes back to its start leaves no unit, a no-op click changes nothing") {
  UndoHistory h;
  std::unique_ptr<Editor> ed;
  fresh(ed, h);
  // A, B, A: three units
  NEED(click(*ed, cutoff(0), 0.3f) == Status::Ok);
  NEED(click(*ed, volume(0), 40.0f) == Status::Ok);
  NEED(click(*ed, cutoff(0), 0.4f) == Status::Ok);
  CHECK(h.ring[2].depth() == 3u);
  // the same row twice more: still three
  NEED(click(*ed, cutoff(0), 0.5f) == Status::Ok);
  CHECK(h.ring[2].depth() == 3u);
  // a sweep that returns to where it began (the unit's first old value) leaves nothing
  NEED(click(*ed, volume(1), 33.0f) == Status::Ok);
  CHECK(h.ring[2].depth() == 4u);
  const float before = project().get_param(cutoff(1));
  NEED(click(*ed, cutoff(1), 0.6f) == Status::Ok);
  NEED(click(*ed, cutoff(1), 0.7f) == Status::Ok);
  CHECK(h.ring[2].depth() == 5u);
  NEED(click(*ed, cutoff(1), before) == Status::Ok);
  CHECK(h.ring[2].depth() == 4u);
  CHECK(project().get_param(cutoff(1)) == before);
  // and it started a group again: the next click is a new unit
  NEED(click(*ed, cutoff(1), 0.9f) == Status::Ok);
  NEED(click(*ed, cutoff(1), 0.95f) == Status::Ok);
  CHECK(h.ring[2].depth() == 5u);
  // a click that changes nothing makes no unit and does not end the group
  NEED(click(*ed, cutoff(1), 0.95f) == Status::Ok);
  NEED(click(*ed, cutoff(1), 0.97f) == Status::Ok);
  CHECK(h.ring[2].depth() == 5u);
  // on a sweep that began on a fresh project there is no unit at all
  CHECK(ed->undo(UndoArea::Sound) == UndoResult::Ok);
  CHECK(project().get_param(cutoff(1)) == before);
  UndoHistory h2;
  std::unique_ptr<Editor> e2;
  fresh(e2, h2);
  const float start = project().get_param(cutoff(1));
  NEED(click(*e2, cutoff(1), 0.2f) == Status::Ok);
  NEED(click(*e2, cutoff(1), start) == Status::Ok);
  CHECK(h2.ring[2].depth() == 0u);
  CHECK(h2.ring[2].used_bytes() == 0u);
  NEED(click(*e2, cutoff(1), 0.4f) == Status::Ok);  // the group does not reopen a unit that is gone
  CHECK(h2.ring[2].depth() == 1u);
}

TEST_CASE("undo groups: another undoable edit, an undo, a redo and merge_break() each end a group; a navigation (nothing written) does not") {
  UndoHistory h;
  std::unique_ptr<Editor> ed;
  fresh(ed, h);
  NEED(click(*ed, cutoff(0), 0.3f) == Status::Ok);
  NEED(click(*ed, cutoff(0), 0.4f) == Status::Ok);
  CHECK(h.ring[2].depth() == 1u);
  // an edit in another area ends it
  NEED(step_edit(*ed, 1, 0, 0, note(60)) == Status::Ok);
  NEED(click(*ed, cutoff(0), 0.5f) == Status::Ok);
  CHECK(h.ring[2].depth() == 2u);
  // an edit of the same area, another row
  NEED(click(*ed, volume(1), 33.0f) == Status::Ok);
  NEED(click(*ed, cutoff(0), 0.6f) == Status::Ok);
  CHECK(h.ring[2].depth() == 4u);
  // an undo and then a redo
  CHECK(ed->undo(UndoArea::Sound) == UndoResult::Ok);
  CHECK(ed->redo(UndoArea::Sound) == UndoResult::Ok);
  NEED(click(*ed, cutoff(0), 0.7f) == Status::Ok);
  CHECK(h.ring[2].depth() == 5u);
  // the app's pause and screen change
  ed->merge_break();
  NEED(click(*ed, cutoff(0), 0.8f) == Status::Ok);
  CHECK(h.ring[2].depth() == 6u);
  // a group with nothing written in it (a cursor move that was dispatched as undoable) leaves the click group open
  ed->begin_group(core::action_id("nav.left"), 0, UndoArea::Sound);
  ed->end_group();
  NEED(click(*ed, cutoff(0), 0.85f) == Status::Ok);
  CHECK(h.ring[2].depth() == 6u);
  // param.reset-like edits (no merge hint) are units of their own and end the group
  ed->begin_group(core::action_id("param.reset"), 0, UndoArea::Sound);
  NEED(ed->set_param(cutoff(0), 1.0f) == Status::Ok);
  ed->end_group();
  NEED(click(*ed, cutoff(0), 0.5f) == Status::Ok);
  CHECK(h.ring[2].depth() == 8u);
}

TEST_CASE("undo groups: a first edit of a module blob (set_instrument then set_param) merges its later clicks into the set_param only") {
  UndoHistory h;
  std::unique_ptr<Editor> ed;
  fresh(ed, h);
  NEED(ed->add_module("synth", 5, 1, 0) == Status::Ok);
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Module);
  in.module_index = 0;
  NEED(ed->set_instrument(5, in) == Status::Ok);
  ed->history_reset();
  const core::ParamId id = core::param_id(core::ParamKind::Instrument, 5, ip::kModuleFirst);
  // click 1: the blob is written (an instrument record) and the value; click 2 and 3 only the value
  ed->begin_group(core::action_id("param.edit"), 1, UndoArea::Sound);
  ed->merge_hint(id);
  model::Instrument full = in;
  full.param_len = 4;
  const float zero = 0.0f;
  std::memcpy(full.params, &zero, 4);
  NEED(ed->set_instrument(5, full) == Status::Ok);
  NEED(ed->set_param(id, 0.25f) == Status::Ok);
  ed->end_group();
  const std::size_t one = h.ring[2].used_bytes();
  CHECK(h.ring[2].depth() == 1u);
  NEED(click(*ed, id, 0.5f) == Status::Ok);
  NEED(click(*ed, id, 0.75f) == Status::Ok);
  CHECK(h.ring[2].depth() == 1u);
  CHECK(h.ring[2].used_bytes() == one);
  CHECK(project().get_param(id) == 0.75f);
  CHECK(ed->undo(UndoArea::Sound) == UndoResult::Ok);
  CHECK(project().instrument(5)->param_len == 0u);  // the blob is as it was before the sweep
  CHECK(ed->redo(UndoArea::Sound) == UndoResult::Ok);
  CHECK(project().get_param(id) == 0.75f);
}

TEST_CASE("undo areas: each area undoes its own edits, redo and depth are independent") {
  UndoHistory h;
  std::unique_ptr<Editor> ed;
  fresh(ed, h);
  const std::uint32_t tempo0 = project().tempo_x100();
  const float cut0 = project().get_param(cutoff(0));
  NEED(song_edit(*ed, 15000) == Status::Ok);
  NEED(step_edit(*ed, 1, 0, 0, note(60)) == Status::Ok);
  NEED(step_edit(*ed, 1, 0, 1, note(62)) == Status::Ok);
  NEED(click(*ed, cutoff(0), 0.4f) == Status::Ok);
  CHECK(h.ring[0].depth() == 2u);
  CHECK(h.ring[1].depth() == 1u);
  CHECK(h.ring[2].depth() == 1u);
  CHECK(ed->can_undo(UndoArea::Pattern));
  // Pattern: two undos and then nothing; the song and the sound stay
  CHECK(ed->undo(UndoArea::Pattern) == UndoResult::Ok);
  CHECK(ed->undo(UndoArea::Pattern) == UndoResult::Ok);
  CHECK(ed->undo(UndoArea::Pattern) == UndoResult::Nothing);
  CHECK(project().pattern(1)->steps[0][0].note == model::kEmptyStep.note);
  CHECK(project().tempo_x100() == 15000u);
  CHECK(project().get_param(cutoff(0)) == 0.4f);
  CHECK(h.ring[1].depth() == 1u);
  CHECK(h.ring[2].depth() == 1u);
  // Song undoes the tempo only, and the pattern's redo is still there
  CHECK(ed->undo(UndoArea::Song) == UndoResult::Ok);
  CHECK(project().tempo_x100() == tempo0);
  CHECK(project().get_param(cutoff(0)) == 0.4f);
  CHECK(ed->redo(UndoArea::Song) == UndoResult::Ok);
  CHECK(h.ring[0].redo_depth() == 2u);
  // an edit in SONG does not discard the PATTERN redo units (D9)
  NEED(song_edit(*ed, 16000) == Status::Ok);
  CHECK(h.ring[0].redo_depth() == 2u);
  CHECK(ed->redo(UndoArea::Pattern) == UndoResult::Ok);
  CHECK(project().pattern(1)->steps[0][0].note == 60);
  // Sound undoes the cutoff only
  CHECK(ed->undo(UndoArea::Sound) == UndoResult::Ok);
  CHECK(project().get_param(cutoff(0)) == cut0);
  CHECK(project().tempo_x100() == 16000u);
  CHECK(project().pattern(1)->steps[0][0].note == 60);
}

TEST_CASE("undo areas: an Auto unit goes where its first record belongs") {
  UndoHistory h;
  std::unique_ptr<Editor> ed;
  fresh(ed, h);
  // outside any group, one mutator is a unit of its own
  NEED(ed->set_step(1, 0, 0, note(50)) == Status::Ok);
  NEED(ed->set_tempo_x100(12000) == Status::Ok);
  NEED(ed->set_param(cutoff(0), 0.2f) == Status::Ok);
  NEED(ed->set_name("title") == Status::Ok);
  NEED(ed->append_order(3) == Status::Ok);
  CHECK(h.ring[0].depth() == 1u);
  CHECK(h.ring[1].depth() == 3u);
  CHECK(h.ring[2].depth() == 1u);
  // a group of an app: the first record decides, and the unit is never split
  ed->begin_group(core::action_id("module.app_group"), 0);
  NEED(ed->set_step(1, 0, 1, note(51)) == Status::Ok);
  NEED(ed->set_param(cutoff(0), 0.9f) == Status::Ok);
  NEED(ed->set_tempo_x100(12500) == Status::Ok);
  ed->end_group();
  CHECK(h.ring[0].depth() == 2u);
  CHECK(h.ring[1].depth() == 3u);
  CHECK(h.ring[2].depth() == 1u);
  CHECK(ed->undo(UndoArea::Pattern) == UndoResult::Ok);
  CHECK(project().tempo_x100() == 12000u);
  CHECK(project().get_param(cutoff(0)) == 0.2f);
  CHECK(project().pattern(1)->steps[0][1].note == model::kEmptyStep.note);
  // a group begun with a parameter edit is SOUND
  ed->begin_group(core::action_id("midi.cc"), 0);
  NEED(ed->set_param(cutoff(0), 0.3f) == Status::Ok);
  ed->end_group();
  CHECK(h.ring[2].depth() == 2u);
}

TEST_CASE("undo areas: an area with no history (the Mixer) records nothing, and a final parameter records nothing") {
  UndoHistory h;
  std::unique_ptr<Editor> ed;
  fresh(ed, h);
  const core::ParamId vol = core::param_id(core::ParamKind::Track, 2, model::track_param::Volume);
  const float v0 = project().get_param(vol);
  ed->begin_group(core::action_id("mixer.edit"), 1, UndoArea::None);
  NEED(ed->set_param(vol, v0 - 7.0f) == Status::Ok);
  NEED(ed->set_track_name(2, "bass", 4) == Status::Ok);
  ed->end_group();
  CHECK(project().get_param(vol) == v0 - 7.0f);
  CHECK(std::strcmp(project().track(2)->name, "bass") == 0);
  for (const model::UndoJournal& r : h.ring) {
    CHECK(r.depth() == 0u);
    CHECK(r.used_bytes() == 0u);
  }
  CHECK(ed->in_sync());
  // a controller on the Mixer's parameter from another screen: applied, not recorded; a neighbour in the same unit is
  ed->begin_group(core::action_id("midi.cc"), 0);
  ed->set_final(true);
  NEED(ed->set_param(vol, v0) == Status::Ok);
  ed->set_final(false);
  NEED(ed->set_param(cutoff(0), 0.2f) == Status::Ok);
  ed->end_group();
  CHECK(h.ring[2].depth() == 1u);
  CHECK(h.ring[0].depth() == 0u);
  CHECK(h.ring[1].depth() == 0u);
  CHECK(ed->undo(UndoArea::Sound) == UndoResult::Ok);
  CHECK(project().get_param(vol) == v0);  // the volume stays where the controller put it
}

TEST_CASE("undo fence: undoing a pattern created from SONG after PATTERN edits clears the PATTERN history only, and says so") {
  UndoHistory h;
  std::unique_ptr<Editor> ed;
  fresh(ed, h);
  // the Song screen makes pattern 9 (a paste) and a slot for it
  ed->begin_group(core::action_id("song.paste"), 0, UndoArea::Song);
  NEED(ed->create_pattern(9, 16) == Status::Ok);
  NEED(ed->append_order(9) == Status::Ok);
  ed->end_group();
  CHECK(h.ring[1].depth() == 1u);
  NEED(step_edit(*ed, 9, 0, 0, note(60)) == Status::Ok);
  NEED(step_edit(*ed, 1, 0, 0, note(61)) == Status::Ok);
  NEED(click(*ed, cutoff(0), 0.3f) == Status::Ok);
  CHECK(h.ring[0].depth() == 2u);
  CHECK(ed->undo(UndoArea::Song) == UndoResult::Ok);
  CHECK(project().pattern(9) == nullptr);
  CHECK(ed->fence_cleared() == UndoArea::Pattern);
  CHECK(h.ring[0].depth() == 0u);
  CHECK(h.ring[0].redo_depth() == 0u);
  CHECK(h.ring[2].depth() == 1u);  // the sound is untouched
  CHECK(project().pattern(1)->steps[0][0].note == 61);  // and so is the pattern's content
  // a redo from SONG makes pattern 9 again; PATTERN has nothing to clear now, and says nothing
  CHECK(ed->redo(UndoArea::Song) == UndoResult::Ok);
  CHECK(ed->fence_cleared() == UndoArea::Auto);
  NEED(step_edit(*ed, 9, 0, 2, note(64)) == Status::Ok);
  // the history of PATTERN is cleared by the redo too when it holds units
  CHECK(ed->undo(UndoArea::Song) == UndoResult::Ok);
  CHECK(ed->fence_cleared() == UndoArea::Pattern);
  CHECK(h.ring[0].depth() == 0u);
}

TEST_CASE("undo fence: a non-structural unit from another area clears nothing; the owner's own ring is not fenced") {
  UndoHistory h;
  std::unique_ptr<Editor> ed;
  fresh(ed, h);
  NEED(step_edit(*ed, 1, 0, 0, note(60)) == Status::Ok);
  // a step edit made from the Song screen (a cross-screen edit that lands where it was made)
  ed->begin_group(core::action_id("song.paste"), 0, UndoArea::Song);
  NEED(ed->set_step(2, 0, 0, note(70)) == Status::Ok);
  ed->end_group();
  CHECK(ed->undo(UndoArea::Song) == UndoResult::Ok);
  CHECK(ed->fence_cleared() == UndoArea::Auto);
  CHECK(h.ring[0].depth() == 1u);
  // a pattern created from PATTERN is undone from PATTERN: nothing to clear
  ed->begin_group(core::action_id("chooser.pad"), 0, UndoArea::Pattern);
  NEED(ed->create_pattern(8, 16) == Status::Ok);
  ed->end_group();
  CHECK(ed->undo(UndoArea::Pattern) == UndoResult::Ok);
  CHECK(ed->fence_cleared() == UndoArea::Auto);
  CHECK(h.ring[0].depth() == 1u);
  CHECK(h.ring[0].redo_depth() == 1u);
}

TEST_CASE("undo fence: writing a pattern from SONG drops the PATTERN redo units; an instrument slot filled or cleared from another area does the same for SOUND") {
  UndoHistory h;
  std::unique_ptr<Editor> ed;
  fresh(ed, h);
  NEED(step_edit(*ed, 1, 0, 0, note(60)) == Status::Ok);
  CHECK(ed->undo(UndoArea::Pattern) == UndoResult::Ok);
  CHECK(h.ring[0].redo_depth() == 1u);
  NEED(click(*ed, cutoff(0), 0.3f) == Status::Ok);
  CHECK(ed->undo(UndoArea::Sound) == UndoResult::Ok);
  CHECK(h.ring[2].redo_depth() == 1u);
  // a non-structural song edit leaves both
  NEED(song_edit(*ed, 12100) == Status::Ok);
  CHECK(h.ring[0].redo_depth() == 1u);
  CHECK(h.ring[2].redo_depth() == 1u);
  // a pattern made from SONG
  ed->begin_group(core::action_id("song.add_slot"), 0, UndoArea::Song);
  NEED(ed->create_pattern(7, 16) == Status::Ok);
  ed->end_group();
  CHECK(h.ring[0].redo_depth() == 0u);
  CHECK(h.ring[2].redo_depth() == 1u);
  // an instrument slot filled from SONG
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Midi);
  in.midi_channel = 3;
  ed->begin_group(core::action_id("song.paste"), 0, UndoArea::Song);
  NEED(ed->set_instrument(20, in) == Status::Ok);
  ed->end_group();
  CHECK(h.ring[2].redo_depth() == 0u);
  CHECK(ed->undo(UndoArea::Song) == UndoResult::Ok);  // clearing the slot: SOUND had no units left to clear, so no notice
  CHECK(ed->fence_cleared() == UndoArea::Auto);
}

TEST_CASE("undo rings: each area fills and evicts alone, and an oversize unit clears only its own ring") {
  UndoHistory h;
  std::unique_ptr<Editor> ed;
  project().reset();
  NEED(h.begin(g_region, 24 * 1024));  // 10 KiB / 6 KiB / 8 KiB
  ed = std::make_unique<Editor>(project(), h);
  NEED(ed->create_pattern(1, 16) == Status::Ok);
  NEED(ed->append_order(1) == Status::Ok);
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Sample);
  NEED(ed->set_instrument(0, in) == Status::Ok);
  ed->history_reset();
  // SONG fills and drops its oldest; the others do not notice
  NEED(step_edit(*ed, 1, 0, 0, note(60)) == Status::Ok);
  NEED(click(*ed, cutoff(0), 0.5f) == Status::Ok);
  for (std::uint32_t i = 0; i < 400; ++i) {
    NEED(song_edit(*ed, 12000 + i) == Status::Ok);
  }
  CHECK(h.ring[1].dropped_units() > 0u);
  CHECK(h.ring[1].depth() * model::UndoJournal::unit_bytes_for(8) <= h.ring[1].capacity_bytes());
  CHECK(h.ring[0].dropped_units() == 0u);
  CHECK(h.ring[2].dropped_units() == 0u);
  CHECK(h.ring[0].depth() == 1u);
  CHECK(h.ring[2].depth() == 1u);
  // a whole-pattern delete (14,412 bytes) does not fit the 10 KiB PATTERN ring: that ring is cleared and counts it; the others stay
  const unsigned song_depth = h.ring[1].depth();
  ed->begin_group(core::action_id("pattern.delete"), 0, UndoArea::Pattern);
  NEED(ed->delete_pattern(1) == Status::Ok);
  ed->end_group();
  CHECK(h.ring[0].lost_units() == 1u);
  CHECK(h.ring[0].depth() == 0u);
  CHECK(h.ring[1].lost_units() == 0u);
  CHECK(h.ring[1].depth() == song_depth);
  CHECK(h.ring[2].depth() == 1u);
  CHECK(ed->undo(UndoArea::Pattern) == UndoResult::Nothing);
  CHECK(ed->undo(UndoArea::Sound) == UndoResult::Ok);
}

TEST_CASE("undo areas: a change behind the editor's back (NEW, a load, an import) clears all three rings, once") {
  UndoHistory h;
  std::unique_ptr<Editor> ed;
  fresh(ed, h);
  NEED(step_edit(*ed, 1, 0, 0, note(60)) == Status::Ok);
  NEED(song_edit(*ed, 15000) == Status::Ok);
  NEED(click(*ed, cutoff(0), 0.3f) == Status::Ok);
  CHECK(ed->has_history());
  (void)project().set_name("behind your back");
  CHECK_FALSE(ed->in_sync());
  CHECK_FALSE(ed->can_undo(UndoArea::Pattern));
  CHECK_FALSE(ed->can_undo(UndoArea::Song));
  CHECK_FALSE(ed->can_undo(UndoArea::Sound));
  CHECK(ed->undo(UndoArea::Song) == UndoResult::Stale);
  CHECK(ed->stale_resets() == 1u);
  CHECK_FALSE(ed->has_history());
  for (const model::UndoJournal& r : h.ring) {
    CHECK(r.depth() == 0u);
    CHECK(r.redo_depth() == 0u);
  }
  CHECK(ed->undo(UndoArea::Pattern) == UndoResult::Nothing);
  CHECK(ed->stale_resets() == 1u);
  // a reset with nothing wrong in it clears all of them too
  NEED(step_edit(*ed, 1, 0, 0, note(60)) == Status::Ok);
  NEED(click(*ed, cutoff(0), 0.3f) == Status::Ok);
  ed->history_reset();
  CHECK_FALSE(ed->has_history());
}

TEST_CASE("undo areas: the single-ring shape keeps one history for every area") {
  project().reset();
  model::UndoJournal j;
  NEED(j.begin(g_region, 64 * 1024));
  Editor ed(project(), j);
  NEED(ed.create_pattern(1, 16) == Status::Ok);
  ed.begin_group(core::action_id("x"), 0, UndoArea::Song);
  NEED(ed.set_tempo_x100(12000) == Status::Ok);
  ed.end_group();
  NEED(ed.set_step(1, 0, 0, note(60)) == Status::Ok);
  CHECK(j.depth() == 3u);
  CHECK(&ed.journal(UndoArea::Pattern) == &j);
  CHECK(&ed.journal(UndoArea::Song) == &j);
  CHECK(&ed.journal(UndoArea::Sound) == &j);
  CHECK(ed.undo(UndoArea::Sound) == UndoResult::Ok);  // whatever the area: the one ring's newest
  CHECK(project().pattern(1)->steps[0][0].note == model::kEmptyStep.note);
  CHECK(ed.undo(UndoArea::Pattern) == UndoResult::Ok);
  CHECK(project().tempo_x100() == model::kDefaultTempoX100);
  CHECK(ed.fence_cleared() == UndoArea::Auto);
}

// ---- the property: random edits, clicks, undo and redo in three areas, against a snapshot per unit of each area ----------------------

namespace {

std::uint64_t fnv(std::uint64_t h, const void* data, std::size_t n) {
  const std::uint8_t* p = static_cast<const std::uint8_t*>(data);
  for (std::size_t i = 0; i < n; ++i) {
    h = (h ^ p[i]) * 1099511628211ull;
  }
  return h;
}

// What each area owns, as a hash: the steps of the three patterns, the song, the parameters of three instruments.
std::uint64_t slice(unsigned area) {
  std::uint64_t h = 1469598103934665603ull;
  const model::Project& p = project();
  if (area == 0) {
    for (unsigned id = 1; id <= 3; ++id) {
      const model::Pattern* pat = p.pattern(id);
      h = fnv(h, pat->steps, sizeof pat->steps);
    }
  } else if (area == 1) {
    const std::uint32_t t = p.tempo_x100();
    h = fnv(h, &t, sizeof t);
    const std::uint32_t n = p.song_length();
    h = fnv(h, &n, sizeof n);
    for (std::uint32_t i = 0; i < n; ++i) {
      const std::uint8_t o = p.order(i);
      h = fnv(h, &o, 1);
    }
  } else {
    for (unsigned i = 0; i < 3; ++i) {
      const float c = p.get_param(cutoff(i));
      const float v = p.get_param(volume(i));
      h = fnv(h, &c, sizeof c);
      h = fnv(h, &v, sizeof v);
    }
  }
  return h;
}

struct Model {
  std::vector<std::uint64_t> undo[3];
  std::vector<std::uint64_t> redo[3];
  std::uint64_t key = 0;  // the click group the next click on that row joins (0: none); a commit anywhere ends it, so one is enough
};

}  // namespace

TEST_CASE("undo areas: property, random edits, clicks, undo and redo in three areas leave every area exactly as its own snapshots say") {
  for (std::uint32_t seed = 1; seed <= 40; ++seed) {
    UndoHistory h;
    std::unique_ptr<Editor> ed;
    fresh(ed, h);
    test::Rng rng(seed * 7919u);
    Model m;
    std::uint64_t cur[3] = {slice(0), slice(1), slice(2)};
    auto break_all = [&] { m.key = 0; };
    const unsigned steps = 1200;
    for (unsigned n = 0; n < steps; ++n) {
      const unsigned kind = rng.below(10);
      if (kind <= 2) {  // a PATTERN edit
        const unsigned a = 0;
        (void)step_edit(*ed, 1 + rng.below(3), rng.below(3), rng.below(8), note(static_cast<std::uint8_t>(40 + rng.below(4))));
        const std::uint64_t now = slice(a);
        if (now != cur[a]) {
          m.undo[a].push_back(cur[a]);
          m.redo[a].clear();
          cur[a] = now;
          break_all();
        }
      } else if (kind == 3) {  // a SONG edit
        const unsigned a = 1;
        (void)song_edit(*ed, 12000u + rng.below(4) * 100u);
        const std::uint64_t now = slice(a);
        if (now != cur[a]) {
          m.undo[a].push_back(cur[a]);
          m.redo[a].clear();
          cur[a] = now;
          break_all();
        }
      } else if (kind <= 6) {  // a click on one of three rows
        const unsigned a = 2;
        const unsigned row = rng.below(3);
        const core::ParamId id = row == 0 ? cutoff(0) : (row == 1 ? volume(0) : cutoff(1));
        const float value = row == 1 ? static_cast<float>(rng.below(4) * 10) : static_cast<float>(rng.below(4)) / 4.0f;
        (void)click(*ed, id, value);
        const std::uint64_t now = slice(a);
        if (now != cur[a]) {
          const std::uint64_t k = static_cast<std::uint64_t>(id);
          if (m.key == k && m.redo[a].empty() && !m.undo[a].empty()) {
            if (now == m.undo[a].back()) {
              m.undo[a].pop_back();  // back where the sweep began: the unit is gone
              m.key = 0;
            }
          } else {
            m.undo[a].push_back(cur[a]);
            m.redo[a].clear();
            m.key = k;
          }
          cur[a] = now;
        }
      } else if (kind <= 8) {  // undo
        const unsigned a = rng.below(3);
        const UndoArea area = a == 0 ? UndoArea::Pattern : (a == 1 ? UndoArea::Song : UndoArea::Sound);
        const UndoResult r = ed->undo(area);
        break_all();
        if (m.undo[a].empty()) {
          CHECK(r == UndoResult::Nothing);
        } else {
          CHECK(r == UndoResult::Ok);
          m.redo[a].push_back(cur[a]);
          cur[a] = m.undo[a].back();
          m.undo[a].pop_back();
        }
      } else {  // redo
        const unsigned a = rng.below(3);
        const UndoArea area = a == 0 ? UndoArea::Pattern : (a == 1 ? UndoArea::Song : UndoArea::Sound);
        const UndoResult r = ed->redo(area);
        break_all();
        if (m.redo[a].empty()) {
          CHECK(r == UndoResult::Nothing);
        } else {
          CHECK(r == UndoResult::Ok);
          m.undo[a].push_back(cur[a]);
          cur[a] = m.redo[a].back();
          m.redo[a].pop_back();
        }
      }
      for (unsigned a = 0; a < 3; ++a) {
        if (slice(a) != cur[a] || h.ring[a].depth() != m.undo[a].size() || h.ring[a].redo_depth() != m.redo[a].size()) {
          FAIL("seed " << seed << " step " << n << " area " << a << ": the area does not match its snapshots (depth " << h.ring[a].depth() << " of "
                       << m.undo[a].size() << ")");
        }
      }
    }
    CHECK(project().check_invariants());
  }
}

TEST_CASE("undo groups: a click made inside a macro or a take (a nested group) asks for no merge, and no later click joins that unit") {
  UndoHistory h;
  std::unique_ptr<Editor> ed;
  fresh(ed, h);
  ed->begin_group(core::action_id("macro.play"), 0, UndoArea::Sound);
  ed->begin_group(core::action_id("param.edit"), 1, UndoArea::Sound);
  ed->merge_hint(cutoff(0));
  NEED(ed->set_param(cutoff(0), 0.3f) == Status::Ok);
  ed->end_group();
  ed->end_group();
  CHECK(h.ring[2].depth() == 1u);
  CHECK(h.ring[2].merge_key() == 0u);
  NEED(click(*ed, cutoff(0), 0.4f) == Status::Ok);
  CHECK(h.ring[2].depth() == 2u);
}

// ---- the structure fence is the same whichever way a pattern is made or removed, and a Failed unit ends every area ------------------------------------

TEST_CASE("undo fence: A, a pattern Pattern made and Song named; undone from Pattern, Song's name of it goes too and never fails") {
  UndoHistory h;
  std::unique_ptr<Editor> ed;
  fresh(ed, h);
  ed->begin_group(core::action_id("chooser.pad"), 0, UndoArea::Pattern);
  NEED(ed->create_pattern(7, 16) == Status::Ok);
  ed->end_group();
  ed->begin_group(core::action_id("text.accept"), 0, UndoArea::Song);
  NEED(ed->set_pattern_name(7, "intro", 5) == Status::Ok);
  ed->end_group();
  CHECK(h.ring[1].depth() == 1u);
  CHECK(ed->undo(UndoArea::Pattern) == UndoResult::Ok);
  CHECK(project().pattern(7) == nullptr);
  CHECK(ed->fence_cleared(UndoArea::Song));
  CHECK_FALSE(ed->fence_cleared(UndoArea::Pattern));
  CHECK(h.ring[1].depth() == 0u);
  CHECK(ed->undo(UndoArea::Song) == UndoResult::Nothing);
  CHECK(ed->failures() == 0u);
}

TEST_CASE("undo fence: B, a pattern Song made, undone, and made again by Pattern; Song's redo of it is gone, so it never fails with Exists") {
  UndoHistory h;
  std::unique_ptr<Editor> ed;
  fresh(ed, h);
  ed->begin_group(core::action_id("song.add_slot"), 1, UndoArea::Song);
  NEED(ed->create_pattern(7, 16) == Status::Ok);
  NEED(ed->append_order(7) == Status::Ok);
  ed->end_group();
  CHECK(ed->undo(UndoArea::Song) == UndoResult::Ok);
  CHECK(h.ring[1].redo_depth() == 1u);
  ed->begin_group(core::action_id("chooser.pad"), 0, UndoArea::Pattern);
  NEED(ed->create_pattern(7, 16) == Status::Ok);
  ed->end_group();
  CHECK(h.ring[1].redo_depth() == 0u);
  CHECK(ed->redo(UndoArea::Song) == UndoResult::Nothing);
  CHECK(ed->failures() == 0u);
  CHECK(project().pattern(7) != nullptr);
}

TEST_CASE("undo fence: a pattern deleted by one ring ends the history of the others that name it; a redo does the same") {
  UndoHistory h;
  std::unique_ptr<Editor> ed;
  fresh(ed, h);
  NEED(step_edit(*ed, 2, 0, 0, note(60)) == Status::Ok);
  NEED(song_edit(*ed, 15000) == Status::Ok);
  ed->begin_group(core::action_id("pattern.delete"), 0, UndoArea::Song);
  NEED(ed->delete_pattern(2) == Status::Ok);
  ed->end_group();
  CHECK(h.ring[0].depth() == 0u);  // PATTERN named pattern 2
  CHECK(h.ring[1].depth() == 2u);
  // undone from SONG, then a Pattern edit of it, then redone from SONG: the Pattern edit's history ends with it
  CHECK(ed->undo(UndoArea::Song) == UndoResult::Ok);
  CHECK(project().pattern(2) != nullptr);
  NEED(step_edit(*ed, 2, 0, 1, note(61)) == Status::Ok);
  CHECK(ed->redo(UndoArea::Song) == UndoResult::Ok);
  CHECK(project().pattern(2) == nullptr);
  CHECK(ed->fence_cleared(UndoArea::Pattern));
  CHECK(ed->undo(UndoArea::Pattern) == UndoResult::Nothing);
}

TEST_CASE("undo rings: a unit that fails ends the history of every area, not only its own") {
  UndoHistory h;
  std::unique_ptr<Editor> ed;
  fresh(ed, h);
  NEED(step_edit(*ed, 1, 0, 0, note(60)) == Status::Ok);
  NEED(click(*ed, cutoff(0), 0.3f) == Status::Ok);
  NEED(song_edit(*ed, 15000) == Status::Ok);
  CHECK(h.ring[0].depth() == 1u);
  CHECK(h.ring[2].depth() == 1u);
  // damage the SONG ring's only unit (its trailer), as a stray write into the region would
  g_region[model::kUndoPatternBytes + model::UndoJournal::unit_bytes_for(8) - 4] ^= 0x04;
  CHECK(ed->undo(UndoArea::Song) == UndoResult::Failed);
  CHECK(ed->failures() == 1u);
  for (const model::UndoJournal& r : h.ring) {
    CHECK(r.depth() == 0u);
    CHECK(r.redo_depth() == 0u);
  }
  CHECK(ed->undo(UndoArea::Pattern) == UndoResult::Nothing);
  CHECK(ed->undo(UndoArea::Sound) == UndoResult::Nothing);
  CHECK(ed->in_sync());
}

TEST_CASE("undo areas: set_final applies an edit and writes no unit, and ends with the caller's say") {
  UndoHistory h;
  std::unique_ptr<Editor> ed;
  fresh(ed, h);
  ed->begin_group(core::action_id("module.app_tempo"), 0);
  ed->set_final(true);
  NEED(ed->set_tempo_x100(17000) == Status::Ok);
  ed->set_final(false);
  ed->end_group();
  CHECK(project().tempo_x100() == 17000u);
  CHECK_FALSE(ed->has_history());
  NEED(ed->set_tempo_x100(18000) == Status::Ok);
  CHECK(h.ring[1].depth() == 1u);
}

TEST_CASE("undo areas: a sample load into a slot ends the SOUND ring only when a record names that slot (adopt_instrument_load, OS-BUG-SLDOVER)") {
  UndoHistory h;
  std::unique_ptr<Editor> ed;
  model::Instrument loaded;
  model::init_instrument(loaded, model::InstrumentKind::Sample);
  const auto load_into = [&](unsigned slot) -> bool {  // the Project changed behind the Editor's back, as a sample load does
    CHECK(project().set_instrument(slot, loaded) == Status::Ok);
    return ed->adopt_instrument_load(slot);
  };
  // a parameter of slot 0 and of slot 1 in the SOUND ring, a step in the PATTERN ring
  const auto edits = [&] {
    fresh(ed, h);
    NEED(click(*ed, cutoff(0), 0.2f) == Status::Ok);
    NEED(click(*ed, volume(1), 0.3f) == Status::Ok);
    NEED(step_edit(*ed, 1, 0, 0, note(50)) == Status::Ok);
    NEED(ed->can_undo(UndoArea::Sound));
  };
  edits();
  CHECK(load_into(1));  // slot 1 is named: the ring ends whole, and says so
  CHECK_FALSE(ed->can_undo(UndoArea::Sound));
  CHECK_FALSE(ed->can_redo(UndoArea::Sound));
  CHECK(ed->can_undo(UndoArea::Pattern));  // the other rings stay
  edits();
  CHECK(load_into(0));
  CHECK_FALSE(ed->can_undo(UndoArea::Sound));
  edits();
  CHECK_FALSE(load_into(2));  // nobody names slot 2: the history stays, and is still in sync
  CHECK(ed->in_sync());
  CHECK(ed->can_undo(UndoArea::Sound));
  CHECK(ed->can_undo(UndoArea::Pattern));
  CHECK(h.ring[2].depth() == 2u);
}
