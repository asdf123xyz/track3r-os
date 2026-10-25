// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-ZOOM and OS-QJUMP (docs/specs/PAT-NAV.md): the three zoom levels of the Pattern grid (layout, window, columns, the small tiles), Note as a held modifier through the
// whole chain (gestures, bindings, actions), Note + Instr / Left / Right, and the quarter jumps on Shift + Note, Instr, Vol and Fx.
#include <doctest/doctest.h>

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/pattern_layout.h>
#include <o3os/app/pattern_view.h>
#include <o3os/app/view_state.h>
#include <o3os/core/input_event.h>
#include <o3os/eve/display_list_builder.h>
#include <o3os/model/editor.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/sim/eve_raster.h>
#include <o3os/sim/raster_display.h>
#include <o3os/ui/renderer.h>
#include <o3os/ui/theme.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "support/project_fixture.h"

using namespace o3os;
using app::Button;
using app::Column;
using app::PatternLayout;
using app::PatternView;

namespace {

const ui::Theme& built_in(unsigned i) { return ui::theme(ui::theme_id_at(i)); }

alignas(8) std::uint8_t g_ring[model::kUndoRegionBytes];

// An App with a document: pattern 1 of 64 steps on `tracks` tracks.
struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;

  explicit Rig(unsigned tracks = 12) {
    model::Project& p = test::arena(0).project;
    p.reset();
    (void)p.set_active_tracks(tracks);
    (void)p.create_pattern(1, 64);
    (void)p.append_order(1);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    app->run_once();
  }
  const app::PatternView& view() const { return app->view(); }
  void press(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true)); }
  void release(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false)); }
  void pass(unsigned ms = 0) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  void tap(Button b) {
    press(b);
    pass(1);
    release(b);
    pass(1);
  }
  // Note (or Shift) held, `b` tapped, then both let go: the roll of the hardware check.
  void with(Button held, Button b) {
    press(held);
    pass(1);
    tap(b);
    release(held);
    pass(1);
  }
  bool popup_open() const { return app->state().overlay == app::OverlayId::StepPopup; }
};

}  // namespace

// ---- the layout ----------------------------------------------------------------------------------------------------------

TEST_CASE("zoom layout: three levels fit in every theme, level 0 is the layout of OS-003b, and the fields are where they are worked out") {
  for (unsigned t = 0; t < ui::kThemeCount; ++t) {
    const ui::Theme& th = built_in(t);
    const PatternLayout base = app::make_pattern_layout(th, PatternView::kVisibleRows, PatternView::kVisibleTracks);
    const PatternLayout l0 = app::make_pattern_layout(th, PatternView::kVisibleRows, 5, 0);
    const PatternLayout l1 = app::make_pattern_layout(th, PatternView::kVisibleRows, 8, 1);
    const PatternLayout l2 = app::make_pattern_layout(th, PatternView::kVisibleRows, 12, 2);
    CHECK_MESSAGE(base.fits, th.name);
    CHECK_MESSAGE(l0.fits, th.name);
    CHECK_MESSAGE(l1.fits, th.name);
    CHECK_MESSAGE(l2.fits, th.name);
    // Level 0 is today's: the same numbers as the default.
    CHECK((l0.track_w == base.track_w && l0.cell_dy == base.cell_dy && l0.cursor_dy == base.cursor_dy && l0.cursor_h == base.cursor_h && l0.fields == 5));
    for (int i = 0; i < app::kLayoutFields; ++i) {
      CHECK((l0.field_x[i] == base.field_x[i] && l0.field_w[i] == base.field_w[i]));
    }
    // (800 - 30) / n per track, the fields of the level, in the cells of its face, centred, one gap apart.
    CHECK((l0.track_w == 154 && l1.track_w == 96 && l2.track_w == 64));
    CHECK((l1.fields == 3 && l2.fields == 1));
    CHECK((l1.face == ui::Face::Label && l2.face == ui::Face::Micro));
    CHECK((l1.field_w[0] == 24 && l1.field_w[1] == 16 && l1.field_w[2] == 16 && l2.field_w[0] == 18));
    CHECK((l1.field_x[0] == 16 && l1.field_x[1] == 44 && l1.field_x[2] == 64 && l2.field_x[0] == 23));
    const int gap = th.metrics.gap;
    CHECK(l1.field_x[1] == l1.field_x[0] + l1.field_w[0] + gap);
    CHECK(l1.field_x[2] + l1.field_w[2] + th.metrics.border <= l1.track_w);
    // The block is as high as the Data face's at every level, and the glyphs of the level lie inside the row.
    CHECK(l1.cursor_h == l0.cursor_h);
    CHECK(l2.cursor_h == l0.cursor_h);
    CHECK((l1.cursor_dy >= 0 && l1.cursor_dy + l1.cursor_h <= l1.row_pitch));
    CHECK((l2.cursor_dy >= 0 && l2.cursor_dy + l2.cursor_h <= l2.row_pitch));
    CHECK((l1.cell_dy + th.geometry(ui::Face::Label).cell_h <= l1.row_pitch && l2.cell_dy + th.geometry(ui::Face::Micro).cell_h <= l2.row_pitch));
    CHECK(l1.index_dy == l0.index_dy);  // the row numbers stay Data tiles
    CHECK(l2.track0_x + l2.tracks * l2.track_w <= ui::kScreenW);
  }
}

// ---- the view: window, columns, moves -------------------------------------------------------------------------------------

TEST_CASE("zoom view: the level cycles 5, 8, 12, 5, and the window is clamped to the tracks and holds the cursor") {
  PatternView v;
  const PatternView::Dims d{64, 12};
  CHECK((v.zoom() == 0 && v.visible_tracks() == 5 && v.visible_fields() == 5));
  (void)v.move_track(11, d);  // the last track: the window scrolls to 7..11
  CHECK(v.left_track() == 7);
  CHECK(v.cycle_zoom(d));
  CHECK((v.zoom() == 1 && v.visible_tracks() == 8 && v.left_track() == 4));  // clamp(7, 0, 12 - 8), the cursor's track 11 is inside 4..11
  CHECK(v.cycle_zoom(d));
  CHECK((v.zoom() == 2 && v.visible_tracks() == 12 && v.left_track() == 0));
  CHECK(v.cycle_zoom(d));
  CHECK((v.zoom() == 0 && v.left_track() == 7));  // back to 5: the cursor's track is in view again
  // A cursor in the middle keeps its window where it can.
  PatternView w;
  (void)w.move_track(6, d);
  CHECK(w.left_track() == 2);
  (void)w.set_zoom(1, d);
  CHECK(w.left_track() == 2);  // 2..9 holds track 6
  CHECK_FALSE(w.set_zoom(1, d));  // no change
  CHECK((w.set_zoom(9, d) && w.zoom() == 2));  // clamped to the last level
  CHECK(w.set_zoom(-3, d));
  CHECK(w.zoom() == 0);
}

TEST_CASE("zoom view: a project with fewer tracks than the level shows what exists, and the window starts at the first") {
  PatternView v;
  const PatternView::Dims d{64, 3};
  (void)v.move_track(2, d);
  CHECK(v.left_track() == 0);
  (void)v.set_zoom(1, d);
  CHECK(v.left_track() == 0);
  (void)v.set_zoom(2, d);
  CHECK(v.left_track() == 0);
  CHECK_FALSE(v.move_track(5, d));  // the last track is the third
  CHECK(v.track() == 2);
}

TEST_CASE("zoom view: a column the level hides snaps to the nearest visible one on its left") {
  const PatternView::Dims d{64, 12};
  PatternView v;
  v.select_column(Column::Fx2);
  (void)v.set_zoom(1, d);
  CHECK(v.column() == Column::Vol);
  v.select_column(Column::Instr);
  (void)v.set_zoom(2, d);
  CHECK(v.column() == Column::Note);
  CHECK((v.column_visible(Column::Note) && !v.column_visible(Column::Instr)));
  (void)v.set_zoom(0, d);
  CHECK(v.column() == Column::Note);  // it does not come back by itself
  CHECK(v.column_visible(Column::Fx2));
  // A hold popup chose a hidden column; the cursor comes back into sight.
  PatternView w;
  (void)w.set_zoom(2, d);
  w.select_column(Column::Fx1);
  CHECK(w.snap_column());
  CHECK(w.column() == Column::Note);
  CHECK_FALSE(w.snap_column());
}

TEST_CASE("zoom view: Left and Right skip the hidden fields and cross to the neighbouring track at the visible edge") {
  const PatternView::Dims d{64, 12};
  PatternView v;
  (void)v.set_zoom(1, d);  // Note, Instr, Vol
  CHECK((v.move_right(d) && v.column() == Column::Instr));
  CHECK((v.move_right(d) && v.column() == Column::Vol));
  CHECK((v.move_right(d) && v.track() == 1 && v.column() == Column::Note));
  CHECK((v.move_left(d) && v.track() == 0 && v.column() == Column::Vol));  // the last visible field of the track before
  CHECK((v.move_left(d) && v.column() == Column::Instr));
  CHECK(v.end(d));
  CHECK((v.track() == 11 && v.column() == Column::Vol));
  CHECK_FALSE(v.move_right(d));  // the last track's last visible field
  CHECK(v.home(d));
  CHECK((v.track() == 0 && v.column() == Column::Note));
  CHECK_FALSE(v.move_left(d));
  (void)v.set_zoom(2, d);  // Note only: every press crosses
  CHECK((v.move_right(d) && v.track() == 1));
  CHECK((v.move_right(d) && v.track() == 2));
  CHECK((v.move_left(d) && v.track() == 1 && v.column() == Column::Note));
  CHECK(v.end(d));
  CHECK((v.track() == 11 && v.column() == Column::Note));
  // At the default level nothing changed: five fields a track.
  PatternView z;
  for (int i = 0; i < 5; ++i) {
    (void)z.move_right(d);
  }
  CHECK((z.track() == 1 && z.column() == Column::Note));
}

// ---- the keys: through the gesture layer, the bindings and the actions ----------------------------------------------------------

TEST_CASE("zoom keys: Note + Instr cycles the level, with no Note column, no popup and no Instr column") {
  Rig rig;
  rig.tap(Button::Vol);
  CHECK(rig.view().column() == Column::Vol);
  rig.with(Button::Note, Button::Instr);
  CHECK(rig.view().zoom() == 1);
  CHECK(rig.view().column() == Column::Vol);  // neither Note's Tap (the Note column) nor Instr's (the Instr column) ran
  CHECK_FALSE(rig.popup_open());
  rig.with(Button::Note, Button::Instr);
  CHECK(rig.view().zoom() == 2);
  CHECK(rig.view().column() == Column::Note);  // Vol is hidden at 12: it snapped
  rig.with(Button::Note, Button::Instr);
  CHECK(rig.view().zoom() == 0);
  rig.pass(2000);  // the Note's Hold never comes after the roll
  CHECK_FALSE(rig.popup_open());
}

TEST_CASE("zoom keys: Note alone is as before: a tap is the Note column, a hold the popup") {
  Rig rig;
  rig.tap(Button::Instr);
  CHECK(rig.view().column() == Column::Instr);
  rig.tap(Button::Note);
  CHECK(rig.view().column() == Column::Note);
  CHECK(rig.view().zoom() == 0);
  rig.press(Button::Note);
  rig.pass(450);
  CHECK(rig.popup_open());
  rig.release(Button::Note);
  rig.pass(1);
  CHECK_FALSE(rig.popup_open());
}

TEST_CASE("zoom keys: Note + Vol and Note + Fx are plain Vol and Fx, and cancel the Note popup") {
  Rig rig;
  rig.with(Button::Note, Button::Vol);
  CHECK(rig.view().column() == Column::Vol);
  rig.press(Button::Note);
  rig.pass(1);
  rig.tap(Button::Fx);
  CHECK(rig.view().column() == Column::Fx1);
  rig.pass(1000);  // still held: no Hold
  CHECK_FALSE(rig.popup_open());
  rig.release(Button::Note);
  rig.pass(1);
  CHECK(rig.view().column() == Column::Fx1);  // and no Note column on the release
  CHECK(rig.view().zoom() == 0);
}

TEST_CASE("zoom keys: Note + Left / Right moves a whole track and keeps the column, at the first and the last track too") {
  Rig rig;
  rig.tap(Button::Vol);
  rig.with(Button::Note, Button::Left);
  CHECK(rig.view().track() == 0);  // the first: stays
  rig.with(Button::Note, Button::Right);
  CHECK((rig.view().track() == 1 && rig.view().column() == Column::Vol));
  rig.with(Button::Note, Button::Right);
  CHECK(rig.view().track() == 2);
  rig.with(Button::Note, Button::Left);
  CHECK((rig.view().track() == 1 && rig.view().column() == Column::Vol));
  (void)rig.app->invoke(core::action_id("cursor.end"));
  CHECK(rig.view().track() == 11);
  rig.with(Button::Note, Button::Right);
  CHECK(rig.view().track() == 11);  // the last: stays
  // Without Note, Right is a column move as ever.
  rig.tap(Button::Left);
  CHECK(rig.view().track() == 11);
}

TEST_CASE("zoom keys: a held Note + Right repeats the track move, and stops when Note is let go") {
  Rig rig;
  rig.press(Button::Note);
  rig.pass(1);
  rig.press(Button::Right);
  rig.pass(1);
  CHECK(rig.view().track() == 1);
  rig.pass(450);  // the first repeat comes at 400 ms
  CHECK(rig.view().track() == 2);
  rig.pass(310);  // and the second 300 ms later
  CHECK(rig.view().track() == 3);
  rig.release(Button::Note);
  rig.pass(1);
  const int stopped = rig.view().track();
  rig.pass(1000);  // the arrow is still down: it does not go on as a plain Right either
  CHECK(rig.view().track() == stopped);
  rig.release(Button::Right);
  rig.pass(1);
  CHECK(rig.view().track() == stopped);
}

TEST_CASE("zoom keys: in the Note popup the same keys reach the grid underneath, and the popup stays open") {
  Rig rig;
  rig.press(Button::Note);
  rig.pass(450);
  CHECK(rig.popup_open());
  rig.tap(Button::Right);  // under a held Note, whose Hold has fired
  CHECK(rig.view().track() == 1);
  CHECK(rig.popup_open());
  rig.tap(Button::Instr);
  CHECK(rig.view().zoom() == 1);
  CHECK(rig.popup_open());
  rig.tap(Button::Left);
  CHECK(rig.view().track() == 0);
  rig.release(Button::Note);
  rig.pass(1);
  CHECK_FALSE(rig.popup_open());
}

TEST_CASE("zoom keys: a field the level hides refuses its column key with a hint, and the hold popups still open and close back into sight") {
  Rig rig;
  (void)rig.app->invoke(core::action_id("pattern.zoom"));
  (void)rig.app->invoke(core::action_id("pattern.zoom"));
  CHECK(rig.view().zoom() == 2);
  rig.tap(Button::Instr);
  CHECK(rig.view().column() == Column::Note);
  CHECK(std::string(rig.app->message()).find("NOTE + INSTR") != std::string::npos);
  rig.pass(10000);
  rig.tap(Button::Fx);
  CHECK(rig.view().column() == Column::Note);
  CHECK(std::string(rig.app->message()).find("NOTE + INSTR") != std::string::npos);
  rig.pass(10000);
  // The hold of Instr opens the popup (and moves the column to Instr); its release puts the cursor back on a visible column.
  rig.press(Button::Instr);
  rig.pass(450);
  CHECK(rig.popup_open());
  rig.release(Button::Instr);
  rig.pass(1);
  CHECK_FALSE(rig.popup_open());
  CHECK(rig.view().column() == Column::Note);
}

TEST_CASE("zoom keys: the level is the view's, RAM only: it survives a project load and starts at 5") {
  Rig rig;
  CHECK(rig.view().zoom() == 0);  // a new view
  rig.with(Button::Note, Button::Instr);
  CHECK(rig.view().zoom() == 1);
  rig.app->attach_document(rig.editor.get());  // a document attached again (a project load)
  rig.app->run_once();
  CHECK(rig.view().zoom() == 1);
}

TEST_CASE("zoom keys: a project of three tracks at levels 8 and 12 moves within its tracks") {
  Rig rig(3);
  rig.with(Button::Note, Button::Instr);
  rig.with(Button::Note, Button::Right);
  rig.with(Button::Note, Button::Right);
  rig.with(Button::Note, Button::Right);
  CHECK(rig.view().track() == 2);
  CHECK(rig.view().left_track() == 0);
  rig.with(Button::Note, Button::Instr);
  CHECK((rig.view().zoom() == 2 && rig.view().left_track() == 0));
  rig.with(Button::Note, Button::Right);
  CHECK(rig.view().track() == 2);
}

// ---- QJUMP ---------------------------------------------------------------------------------------------------------------------

TEST_CASE("quarter jumps: Shift + Note, Instr, Vol and Fx go to 0, 16, 32 and 48 of a 64-step pattern, from anywhere, and keep the column") {
  Rig rig;
  rig.tap(Button::Vol);
  const Button keys[4] = {Button::Note, Button::Instr, Button::Vol, Button::Fx};
  const int rows[4] = {0, 16, 32, 48};
  for (int pass = 0; pass < 2; ++pass) {  // the second pass from the last row backwards
    for (int i = 0; i < 4; ++i) {
      const int k = pass == 0 ? i : 3 - i;
      rig.with(Button::Shift, keys[k]);
      CHECK(rig.view().row() == rows[k]);
      CHECK(rig.view().column() == Column::Vol);
      CHECK_FALSE(rig.popup_open());
    }
  }
  rig.pass(2000);
  CHECK_FALSE(rig.popup_open());
  // Shift + Insert is still HOME, and the plain keys are as they were.
  (void)rig.app->invoke(core::action_id("cursor.end"));
  rig.with(Button::Shift, Button::Insert);
  CHECK(rig.view().row() == 0);
}

TEST_CASE("quarter jumps: Shift + Fx is no longer FILL (the action stays registered with no key), and F4 still opens the dialog") {
  Rig rig;
  (void)rig.app->invoke(core::action_id("view.mode"), static_cast<core::ActionArg>(app::PatternMode::Edit));
  const unsigned depth = rig.journal.depth();
  rig.with(Button::Shift, Button::Fx);
  CHECK(rig.journal.depth() == depth);  // nothing was filled
  CHECK(rig.view().row() == 48);
  CHECK(std::string(rig.app->message()).find("FILL") == std::string::npos);
  CHECK(rig.app->invoke(core::action_id("pattern.fill")) != app::DispatchResult::Unknown);
  rig.tap(Button::F4);
  CHECK(rig.app->state().overlay == app::OverlayId::Fill);
}

// ---- drawing -------------------------------------------------------------------------------------------------------------------

namespace {

struct Frame {
  sim::EveRaster raster;
  sim::RasterDisplay display{raster};
  std::size_t words = 0;
  bool ok = false;
  Frame(const PatternView& view, const app::PatternData& data, unsigned theme_index) {
    CHECK(sim::load_assets(display) == hal::UploadResult::Ok);
    eve::DisplayListBuffer storage;
    eve::DisplayListBuilder builder(display, storage.words, eve::kMaxDisplayListWords);
    ui::Renderer r(builder, built_in(theme_index));
    CHECK(r.begin(ui::Color::Pane));
    view.draw(r, data, PatternView::Status{1, app::App::kSoftKeyLabels});
    words = r.words();
    ok = r.end();
    CHECK(display.render_failures() == 0u);
  }
};

}  // namespace

TEST_CASE("zoom draw: every level draws in every theme within the display list, and the levels differ") {
  auto data = std::make_unique<app::PatternData>();
  app::fill_demo(*data);
  for (unsigned t = 0; t < ui::kThemeCount; ++t) {
    std::vector<std::size_t> words;
    for (int level = 0; level < 3; ++level) {
      PatternView v;
      (void)v.set_zoom(level, PatternView::Dims{app::kSteps, app::kTracks});
      const Frame f(v, *data, t);
      CHECK_MESSAGE(f.ok, built_in(t).name);
      CHECK(f.words <= 1792);
      words.push_back(f.words);
    }
    CHECK(words[0] != words[1]);
    CHECK(words[1] != words[2]);
  }
}

TEST_CASE("zoom draw: the smaller tiles are the glyphs of their face (a hex pair and a note, in the sim's RAM_G)") {
  // A note tile of the Label face is the three glyphs of the 8x16 face side by side: compare the tile with the text.
  for (const ui::Face face : {ui::Face::Label, ui::Face::Micro}) {
    sim::EveRaster tile_raster;
    sim::RasterDisplay tile_display{tile_raster};
    CHECK(sim::load_assets(tile_display) == hal::UploadResult::Ok);
    sim::EveRaster text_raster;
    sim::RasterDisplay text_display{text_raster};
    CHECK(sim::load_assets(text_display) == hal::UploadResult::Ok);
    const unsigned notes[] = {0, 1, 38, 120, 121, 123, 127};  // ---, C-0, a middle note, B-9, OFF, CUT, a cell of the tenth octave
    for (const unsigned cell : notes) {
      eve::DisplayListBuffer s1;
      eve::DisplayListBuilder b1(tile_display, s1.words, eve::kMaxDisplayListWords);
      ui::Renderer r1(b1, built_in(0));
      CHECK(r1.begin(ui::Color::Pane));
      r1.note_tile(10, 10, cell, ui::Color::Text, face);
      CHECK(r1.end());
      eve::DisplayListBuffer s2;
      eve::DisplayListBuilder b2(text_display, s2.words, eve::kMaxDisplayListWords);
      ui::Renderer r2(b2, built_in(0));
      CHECK(r2.begin(ui::Color::Pane));
      char name[4];
      ui::note_tile_text(cell, name);
      r2.text(10, 10, name, ui::Color::Text, face);
      CHECK(r2.end());
      CHECK_MESSAGE(std::memcmp(tile_raster.rgba(), text_raster.rgba(), 800u * 480u * 4u) == 0, "cell " << cell);
    }
    if (face == ui::Face::Label) {
      for (const unsigned value : {0u, 7u, 0x5Au, 0x7Fu, 0x80u, 0xFFu}) {  // 0x80 and up are drawn as glyphs
        eve::DisplayListBuffer s1;
        eve::DisplayListBuilder b1(tile_display, s1.words, eve::kMaxDisplayListWords);
        ui::Renderer r1(b1, built_in(0));
        CHECK(r1.begin(ui::Color::Pane));
        r1.hex_tile(10, 10, value, ui::Color::Text, face);
        CHECK(r1.end());
        eve::DisplayListBuffer s2;
        eve::DisplayListBuilder b2(text_display, s2.words, eve::kMaxDisplayListWords);
        ui::Renderer r2(b2, built_in(0));
        CHECK(r2.begin(ui::Color::Pane));
        static const char kDigits[] = "0123456789ABCDEF";
        const char pair[3] = {kDigits[value >> 4], kDigits[value & 0xF], '\0'};
        r2.text(10, 10, pair, ui::Color::Text, face);
        CHECK(r2.end());
        CHECK_MESSAGE(std::memcmp(tile_raster.rgba(), text_raster.rgba(), 800u * 480u * 4u) == 0, "hex " << value);
      }
    }
  }
}
