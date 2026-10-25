// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-FU5: a project loaded with fewer than 12 active tracks (an OG 8-track project, a MED song of 4) is raised to 12 by App::document_replaced, in the step before the unsaved baseline: the
// project is not marked modified and nothing is an undo unit. The readers keep returning the file's value.
#include <doctest/doctest.h>

#include "support/mem_card.h"
#include "support/need.h"
#include "support/og_fixture.h"
#include "support/mmd_fixture.h"

#include <o3os/app/app.h>
#include <o3os/app/save_service.h>
#include <o3os/formats/med/import.h>
#include <o3os/formats/med/mmd.h>
#include <o3os/formats/og.h>
#include <o3os/model/editor.h>
#include <o3os/platform/host/fakes.h>

#include <cstdint>
#include <memory>

using namespace o3os;

namespace {

alignas(8) std::uint8_t g_ring[64 * 1024];

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  test::MemCard card;
  app::SaveService saves;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  model::ModelArena& arena;

  explicit Rig(model::ModelArena& a) : arena(a) {
    arena.project.reset();
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(arena.project, journal);
    app->attach_document(editor.get());
    saves.attach(card, card);
    app->attach_saves(&saves);
    app->run_once();
  }
  // What the platform does after it has loaded into the document.
  void loaded() {
    app->document_replaced();
    clock.advance(16 * core::kTicksPerMs);
    app->run_once();
  }
};

}  // namespace

TEST_CASE("load normalise: an OG project of 8 tracks comes out at 12 and is not modified; the reader still says 8") {
  Rig rig(test::og::arena(0));
  test::og::ProjectSpec spec;
  spec.version = 14;  // an 8-track layout of the OG file format (the versions before 16, docs/formats/og.md)
  formats::og::ProjectReader reader;
  NEED(test::og::run_reader(reader, test::og::project_bytes(spec), rig.arena) == formats::Progress::Done);
  CHECK(rig.arena.project.active_tracks() == 8);  // what the file says
  rig.loaded();
  CHECK(rig.arena.project.active_tracks() == 12);
  CHECK_FALSE(rig.saves.dirty(rig.arena.project));
  CHECK_FALSE(rig.editor->can_undo());  // not an undo unit
  rig.loaded();                         // and it holds: nothing left to change
  CHECK(rig.arena.project.active_tracks() == 12);
  CHECK_FALSE(rig.saves.dirty(rig.arena.project));
}

TEST_CASE("load normalise: a MED song of 4 tracks comes out at 12 and is not modified") {
  static model::ModelArena arena;
  Rig rig(arena);
  test::mmd::ModuleSpec m;
  m.version = 1;
  m.deftempo = 33;
  m.tempo2 = 6;
  test::mmd::BlockSpec b;
  b.tracks = 4;
  b.lines = 4;
  m.blocks = {b};
  m.playseq = {0};
  const test::mmd::Bytes bytes = test::mmd::build(m);
  formats::med::SpanSource src(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
  static formats::med::Importer importer;
  importer.begin(src, arena, formats::med::ImportOptions());
  formats::Progress end;
  do {
    end = importer.step(1u << 20);
  } while (end == formats::Progress::Running);
  NEED(end == formats::Progress::Done);
  CHECK(arena.project.active_tracks() == 4);
  rig.loaded();
  CHECK(arena.project.active_tracks() == 12);
  CHECK_FALSE(rig.saves.dirty(arena.project));
  CHECK_FALSE(rig.editor->can_undo());
}

TEST_CASE("load normalise: 16 active tracks stay 16, and a project already at 12 is left alone") {
  static model::ModelArena arena;
  Rig rig(arena);
  CHECK(arena.project.set_active_tracks(16) == model::Status::Ok);
  rig.loaded();
  CHECK(arena.project.active_tracks() == 16);
  CHECK(arena.project.set_active_tracks(12) == model::Status::Ok);
  rig.loaded();
  CHECK(arena.project.active_tracks() == 12);
  CHECK_FALSE(rig.saves.dirty(arena.project));
}
