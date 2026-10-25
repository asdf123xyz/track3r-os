// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-011a: the sample bank's edit history (app/sample_bank.h): versions of one slot's audio kept as bounded undo steps, swapped by pointer; the pool of buffers is
// bounded and reused; nothing the engine may be reading, or a step needs, is ever overwritten.
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3os/app/sample_bank.h>
#include <o3os/audio/voices.h>
#include <o3os/model/instrument.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

using namespace o3os;
using app::PointsRec;
using app::SampleBank;

namespace {

struct Rig {
  std::unique_ptr<audio::VoicePool> voices = std::make_unique<audio::VoicePool>();
  std::unique_ptr<model::ModelArena> arena = std::make_unique<model::ModelArena>();
  std::vector<std::int16_t> memory;
  std::unique_ptr<app::SampleBankMem> mem = std::make_unique<app::SampleBankMem>();
  SampleBank bank;

  explicit Rig(std::uint32_t samples = 40000) : memory(samples + 8) {
    arena->project.reset();
    bank.init(*voices, *arena, *mem, memory.data(), samples);
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Sample);
    CHECK(arena->project.set_instrument(0, in) == model::Status::Ok);
    CHECK(arena->project.set_instrument(1, in) == model::Status::Ok);
    load(0, 1000, 1);
    bank.rebind_now(0);
  }
  void load(unsigned slot, std::uint32_t frames, std::int16_t fill) {
    std::uint32_t cap = 0;
    std::int16_t* p = bank.reserve(cap);
    NEED(p != nullptr);
    for (std::uint32_t i = 0; i < frames; ++i) {
      p[i] = fill;
    }
    CHECK(bank.commit_load(slot, p, frames));
  }
  // One edit: a new version `fill` of `frames` frames.
  bool edit(unsigned slot, std::uint32_t frames, std::int16_t fill, std::int16_t points = 0) {
    std::int16_t* b = bank.edit_begin(slot, frames);
    if (b == nullptr) {
      return false;
    }
    for (std::uint32_t i = 0; i < frames; ++i) {
      b[i] = fill;
    }
    PointsRec before;
    PointsRec after;
    before.points.start = static_cast<std::uint16_t>(points);
    after.points.start = static_cast<std::uint16_t>(points + 1);
    return bank.edit_commit(slot, b, frames, before, after);
  }
  std::int16_t first(unsigned slot) { return bank.pcm(slot).data[0]; }
};

}  // namespace

TEST_CASE("sample history: an edit becomes the slot's audio and undo and redo swap the versions back and forth with the points they carry") {
  Rig rig;
  const std::int16_t* original = rig.bank.pcm(0).data;
  CHECK(rig.edit(0, 600, 2, 10));  // v1: 600 frames of 2
  CHECK(rig.first(0) == 2);
  CHECK(rig.bank.pcm(0).frames == 600u);
  CHECK(rig.edit(0, 600, 3, 20));  // v2
  CHECK(rig.bank.undo_depth(0) == 2u);
  PointsRec p;
  CHECK(rig.bank.edit_undo(0, p));
  CHECK(rig.first(0) == 2);
  CHECK(p.points.start == 20u);  // the points before the second edit
  CHECK(rig.bank.edit_undo(0, p));
  CHECK(rig.bank.pcm(0).data == original);  // the original, by pointer: nothing was copied
  CHECK(rig.bank.pcm(0).frames == 1000u);
  CHECK(p.points.start == 10u);
  CHECK_FALSE(rig.bank.edit_undo(0, p));
  CHECK(rig.bank.redo_depth(0) == 2u);
  CHECK(rig.bank.edit_redo(0, p));
  CHECK(rig.first(0) == 2);
  CHECK(p.points.start == 11u);  // the points after the first edit
  CHECK(rig.bank.edit_redo(0, p));
  CHECK(rig.first(0) == 3);
  CHECK_FALSE(rig.bank.edit_redo(0, p));
  // A new edit after an undo drops the redo steps.
  CHECK(rig.bank.edit_undo(0, p));
  CHECK(rig.edit(0, 500, 4));
  CHECK(rig.bank.redo_depth(0) == 0u);
}

TEST_CASE("sample history: the memory is bounded: a long session reuses its few buffers and the arena stops growing; history keeps three steps") {
  Rig rig;
  std::uint32_t free_after_pool = 0;
  for (int i = 0; i < 40; ++i) {
    CHECK(rig.edit(0, 1000, static_cast<std::int16_t>(10 + i)));
    if (i == static_cast<int>(SampleBank::kSpares) + 2) {
      free_after_pool = rig.bank.free_samples();
    }
  }
  CHECK(free_after_pool != 0u);
  CHECK(rig.bank.free_samples() == free_after_pool);  // no more tail taken after the pool was full
  CHECK(rig.bank.undo_depth(0) == SampleBank::kHistory);
  CHECK(rig.first(0) == 49);
  PointsRec p;
  for (unsigned i = 0; i < SampleBank::kHistory; ++i) {
    CHECK(rig.bank.edit_undo(0, p));
  }
  CHECK(rig.first(0) == 46);  // three steps back from 49
  CHECK_FALSE(rig.bank.edit_undo(0, p));
}

TEST_CASE("sample history: a buffer the engine may be playing, or a step holds, is never handed out again; the original is never written") {
  Rig rig;
  std::vector<std::int16_t> original(rig.bank.pcm(0).data, rig.bank.pcm(0).data + 1000);
  const std::int16_t* bound = rig.bank.pcm(0).data;  // what the voices have (rebind_now in the rig)
  // Many edits with the engine never told (no poll): the voices stay bound to the original.
  for (int i = 0; i < 30; ++i) {
    NEED(rig.edit(0, 1000, static_cast<std::int16_t>(100 + i)));
    CHECK(std::memcmp(bound, original.data(), 2000) == 0);
    CHECK(rig.voices->sample(0).data == bound);
  }
  // Now it is told: the voices move to the latest, and the old binding may be reused later.
  CHECK_FALSE(rig.bank.poll(0, false));
  CHECK_FALSE(rig.bank.poll(1000, true));
  CHECK(rig.bank.poll(1000 + SampleBank::kSettleMs, true));
  CHECK(rig.voices->sample(0).data == rig.bank.pcm(0).data);
  // Every buffer a step holds still has its audio after more edits.
  PointsRec p;
  CHECK(rig.bank.edit_undo(0, p));
  CHECK(rig.first(0) == 128);
  CHECK(rig.bank.edit_undo(0, p));
  CHECK(rig.first(0) == 127);
}

TEST_CASE("sample history: another slot starts its own history; no room, no sample or a stranger's buffer are refused; clear forgets it all") {
  Rig rig(3000);  // 1000 loaded, 2000 left: two copies of 1000 at most
  CHECK(rig.edit(0, 1000, 5));
  CHECK(rig.edit(0, 1000, 6));
  CHECK(rig.edit(0, 1000, 7));  // the pool is full (2 buffers): the third edit reuses the oldest free one (the first, the engine has the original)
  CHECK(rig.bank.free_samples() == 0u);
  CHECK(rig.first(0) == 7);
  // Slot 1 has no audio: nothing to edit; with some, its session replaces slot 0's history.
  CHECK(rig.bank.edit_begin(1, 100) == nullptr);
  CHECK(rig.bank.edit_begin(64, 100) == nullptr);
  CHECK(rig.bank.edit_begin(0, 0) == nullptr);
  rig.bank.adopt(1, rig.memory.data(), 500);
  std::int16_t* b = rig.bank.edit_begin(1, 500);
  CHECK(rig.bank.undo_depth(0) == 0u);  // the history moved to slot 1
  NEED(b != nullptr);
  CHECK(rig.bank.edit_commit(1, b, 500, PointsRec{}, PointsRec{}));
  // A buffer that is not the session's, or too long for it, is refused.
  std::int16_t stranger[8] = {};
  CHECK_FALSE(rig.bank.edit_commit(1, stranger, 8, PointsRec{}, PointsRec{}));
  CHECK_FALSE(rig.bank.edit_commit(0, b, 10, PointsRec{}, PointsRec{}));  // not slot 0's session
  rig.bank.clear();
  CHECK(rig.bank.undo_depth(1) == 0u);
  CHECK(rig.bank.redo_depth(1) == 0u);
  PointsRec p;
  CHECK_FALSE(rig.bank.edit_undo(1, p));
}

TEST_CASE("sample history: with no room for a copy the edit is refused and nothing changes") {
  Rig rig(1000);  // the arena holds the sample and nothing else
  CHECK(rig.bank.edit_begin(0, 1000) == nullptr);
  CHECK(rig.first(0) == 1);
}

TEST_CASE("sample history: a load over the slot of the history ends it, and a load into another slot keeps it (OS-BUG-SLDOVER)") {
  Rig rig;
  PointsRec p;
  CHECK(rig.edit(0, 600, 2));
  CHECK(rig.edit(0, 500, 3));
  CHECK(rig.bank.edit_undo(0, p));  // one step undone, one to redo
  rig.load(1, 300, 7);              // another slot: the history of slot 0 stays
  CHECK(rig.bank.undo_depth(0) == 1u);
  CHECK(rig.bank.redo_depth(0) == 1u);
  rig.load(0, 400, 9);  // over the slot: its steps are of audio that is gone
  CHECK(rig.bank.undo_depth(0) == 0u);
  CHECK(rig.bank.redo_depth(0) == 0u);
  CHECK_FALSE(rig.bank.edit_undo(0, p));
  CHECK_FALSE(rig.bank.edit_redo(0, p));
  CHECK(rig.first(0) == 9);  // the loaded audio is what is there
  CHECK(rig.bank.pcm(0).frames == 400u);
}
