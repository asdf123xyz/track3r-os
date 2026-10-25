// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/pad_note.h>
#include <o3os/app/pattern_data.h>
#include <o3os/app/pattern_view.h>
#include <o3os/core/input_event.h>
#include <o3os/platform/host/fakes.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

using namespace o3os;
using app::Button;
using app::Column;
using app::PatternView;

namespace {

std::string field(const app::Step& s, Column c, bool* empty = nullptr) {
  char out[4];
  const bool e = app::format_field(s, c, out);
  if (empty != nullptr) {
    *empty = e;
  }
  return out;
}

app::Step with_note(std::uint8_t n) {
  app::Step s = model::kEmptyStep;
  s.note = n;
  return s;
}

}  // namespace

namespace {
// OS-PADHOLD: a Browse pad previews and remembers its note (edit.last_note) in every column; the Instr column no longer chooses the instrument, so these tests read the pad's note
// where they used to read the instrument. `pad_note_of`: the note the pad plays on the default board.
template <typename R>
unsigned pad_note_of(R& rig, unsigned pad) {
  return app::pad_note(rig.app->model().project, rig.app->instrument(), rig.app->state().edit.pads, pad);
}
template <typename R>
unsigned last_note_of(R& rig) {
  return rig.app->state().edit.last_note;
}
}  // namespace

TEST_CASE("pattern data: note names, instrument, volume and FX fields") {
  bool empty = false;
  CHECK(field(with_note(0), Column::Note, &empty) == "---");
  CHECK(empty);
  CHECK(field(with_note(1), Column::Note, &empty) == "C-0");
  CHECK_FALSE(empty);
  CHECK(field(with_note(2), Column::Note) == "C#0");
  CHECK(field(with_note(13), Column::Note) == "C-1");
  CHECK(field(with_note(25), Column::Note) == "C-2");
  CHECK(field(with_note(94), Column::Note) == "A-7");
  CHECK(field(with_note(95), Column::Note) == "A#7");
  CHECK(field(with_note(120), Column::Note) == "B-9");
  CHECK(field(with_note(121), Column::Note) == "C10");  // the tenth octave: the names that fit a tile (our spelling), the sharps lowercase (our choice)
  CHECK(field(with_note(122), Column::Note) == "c10");
  CHECK(field(with_note(128), Column::Note) == "G10");
  CHECK(field(with_note(129), Column::Note) == "???");
  CHECK(field(with_note(252), Column::Note) == "???");
  CHECK(field(with_note(model::kNoteFade), Column::Note, &empty) == "FAD");
  CHECK_FALSE(empty);
  CHECK(field(with_note(model::kNoteCut), Column::Note, &empty) == "CUT");
  CHECK_FALSE(empty);
  CHECK(field(with_note(model::kNoteOff), Column::Note, &empty) == "OFF");
  CHECK_FALSE(empty);

  app::Step s = model::kEmptyStep;
  CHECK(field(s, Column::Instr, &empty) == "--");
  CHECK(empty);
  s.instr = 1;
  CHECK(field(s, Column::Instr) == "01");
  s.instr = 48;
  CHECK(field(s, Column::Instr) == "48");
  s.instr = 49;  // a MIDI instrument: a step names up to 64 (OS-009; the OS-001b demo had 48)
  CHECK(field(s, Column::Instr) == "49");
  s.instr = 64;
  CHECK(field(s, Column::Instr) == "64");
  s.instr = 65;
  CHECK(field(s, Column::Instr) == "??");
  // An off, cut or fade step shows no instrument, whatever it holds: an instrument is drawn for a note only.
  s.instr = 5;
  CHECK(field(s, Column::Instr, &empty) == "05");
  CHECK_FALSE(empty);
  for (const std::uint8_t special : {model::kNoteOff, model::kNoteCut, model::kNoteFade}) {
    s.note = special;
    CHECK(field(s, Column::Instr, &empty) == "--");
    CHECK(empty);
  }
  s.note = model::kNoteEmpty;
  s.instr = 1;

  CHECK(field(s, Column::Vol, &empty) == "--");
  CHECK(empty);
  s.vol = 0;
  CHECK(field(s, Column::Vol, &empty) == "00");
  CHECK_FALSE(empty);
  s.vol = 0x7F;
  CHECK(field(s, Column::Vol) == "7F");
  s.vol = 0x80;  // above 127 is not a volume (255 is "not set"): carried, drawn as a question
  CHECK(field(s, Column::Vol) == "??");
  s.vol = 0xFE;
  CHECK(field(s, Column::Vol) == "??");

  CHECK(field(s, Column::Fx1, &empty) == "---");
  CHECK(empty);
  CHECK(field(s, Column::Fx2) == "---");
  s.fx[0] = {model::kFxNone, 7};  // type 0 is None: a leftover value is not shown
  CHECK(field(s, Column::Fx1, &empty) == "---");
  CHECK(empty);
  s.fx[0] = {1, 0};  // Off
  CHECK(field(s, Column::Fx1, &empty) == "100");
  CHECK_FALSE(empty);
  s.fx[0] = {27, 0x4A};  // base-36 id
  CHECK(field(s, Column::Fx1) == "R4A");
  s.fx[1] = {9, 0xFF};
  CHECK(field(s, Column::Fx2) == "9FF");
  const std::pair<std::uint8_t, char> types[] = {{10, 'A'}, {35, 'Z'}, {36, '!'}, {42, '+'}, {43, '?'},
                                                  {45, '?'}, {46, '@'}, {47, '^'}, {48, '~'}, {49, '<'}, {50, '>'}, {51, '='}, {52, '"'}, {53, '\''}, {54, ';'}, {55, '/'}, {56, ':'}, {57, '|'}, {58, '['}, {59, ']'}, {60, '('}, {61, ')'}, {62, '{'}, {63, '?'}, {127, '?'}, {128, 'a'}, {129, 'b'}, {154, 'a'}, {254, 'w'}};
  for (const auto& t : types) {
    s.fx[0].type = t.first;
    CHECK(field(s, Column::Fx1)[0] == t.second);
  }
}

TEST_CASE("pattern data: the demo pattern is deterministic and has the expected shape") {
  auto a = std::make_unique<app::PatternData>();
  auto b = std::make_unique<app::PatternData>();
  app::fill_demo(*a);
  app::fill_demo(*b);
  CHECK(std::memcmp(a.get(), b.get(), sizeof(app::PatternData)) == 0);
  CHECK(sizeof(app::Step) == 7);
  CHECK(sizeof(app::PatternData) == 12u * 64u * 7u);

  const app::Step& kick = a->steps[0][0];
  CHECK(field(kick, Column::Note) == "C-2");
  CHECK(field(kick, Column::Instr) == "01");
  CHECK(field(kick, Column::Vol) == "7F");
  CHECK(field(kick, Column::Fx1) == "---");
  CHECK(field(a->steps[0][1], Column::Note) == "---");  // every 4th step only
  CHECK(field(a->steps[0][4], Column::Note) == "C-2");
  CHECK(field(a->steps[3][1], Column::Instr) == "04");  // ACID plays every step
  // External-audio lanes carry volume and FX but no note and no instrument.
  const app::Step& ext = a->steps[10][0];
  CHECK(ext.note == model::kNoteEmpty);
  CHECK(ext.instr == 0);
  CHECK(field(ext, Column::Vol) == "40");
  CHECK(field(ext, Column::Fx1) == "R30");
  // Engine-declared FX (type 128) shows as a lower-case letter on the ACID track.
  bool saw_engine_fx = false;
  for (const app::Step& s : a->steps[3]) {
    saw_engine_fx = saw_engine_fx || s.fx[0].type == 128;
  }
  CHECK(saw_engine_fx);
  CHECK(std::string(app::track_name(0)) == "KICK");
  CHECK(app::track_type(8) == 'M');
  CHECK(app::track_type(10) == 'E');
  CHECK(std::string(app::track_name(99)) == "");
  CHECK(std::string(app::instrument_name(4)) == "ACID");
  CHECK(std::string(app::instrument_name(48)) == "");
}

TEST_CASE("pattern view: the cursor walks columns, then tracks, and stops at the ends") {
  PatternView v;
  CHECK((v.row() == 0 && v.track() == 0 && v.column() == Column::Note));
  CHECK_FALSE(v.move_left());
  CHECK_FALSE(v.move_up());
  const Column order[] = {Column::Instr, Column::Vol, Column::Fx1, Column::Fx2};
  for (Column c : order) {
    CHECK(v.move_right());
    CHECK((v.track() == 0 && v.column() == c));
  }
  CHECK(v.move_right());  // FX2 -> next track's Note
  CHECK((v.track() == 1 && v.column() == Column::Note));
  CHECK(v.move_left());   // and back
  CHECK((v.track() == 0 && v.column() == Column::Fx2));
  for (int i = 0; i < 5 * 11; ++i) {  // from (track 1, FX2) to (track 12, FX2)
    CHECK(v.move_right());
  }
  CHECK((v.track() == 11 && v.column() == Column::Fx2));
  CHECK_FALSE(v.move_right());
}

TEST_CASE("pattern view: rows clamp to the pattern and the window keeps the cursor on the centre slot, at both ends too (OS-CENTER, OS-PATCENTER)") {
  PatternView v;  // the demo's 64 steps, 16 shown
  CHECK((v.row() == 0 && v.top_row() == -7));  // OS-PATCENTER: the first row is on the centre slot (the 8th); the seven slots above it are blank
  CHECK(v.move_down());
  CHECK((v.row() == 1 && v.top_row() == -6));
  CHECK_FALSE(v.scroll_rows(0));
  CHECK(v.scroll_rows(6));
  CHECK((v.row() == 7 && v.top_row() == 0));  // the first row of the window is row 0 again: the blank slots are gone
  CHECK(v.move_down());
  CHECK((v.row() == 8 && v.top_row() == 1));  // the pattern scrolls and the cursor stays on the centre slot
  CHECK(v.scroll_rows(1000));
  CHECK((v.row() == 63 && v.top_row() == 56));  // the last row is on the centre slot; the eight slots below it are blank
  CHECK_FALSE(v.move_down());
  CHECK(v.move_up());
  CHECK((v.row() == 62 && v.top_row() == 55));  // the cursor never leaves the centre slot
  CHECK(v.scroll_rows(-8));
  CHECK((v.row() == 54 && v.top_row() == 47));
  CHECK(v.scroll_rows(-1000));
  CHECK((v.row() == 0 && v.top_row() == -7));
  CHECK_FALSE(v.move_up());
}

TEST_CASE("pattern view: the window is a pure function of the cursor's row: the row on the centre slot, for every pattern length and at both ends (OS-CENTER, OS-PATCENTER)") {
  using V = PatternView;
  CHECK(V::kCentreRow == 7);
  // Rows 0, the middle and the last of 64 steps.
  CHECK(V::window_top(0) == -7);
  CHECK(V::window_top(7) == 0);
  CHECK(V::window_top(8) == 1);
  CHECK(V::window_top(31) == 24);
  CHECK(V::window_top(55) == 48);
  CHECK(V::window_top(56) == 49);
  CHECK(V::window_top(63) == 56);
  // For every length the cursor of every row is on the centre slot: nothing depends on how long the pattern is.
  for (int steps = 1; steps <= 128; ++steps) {
    for (int row = 0; row < steps; ++row) {
      CHECK_MESSAGE(row - V::window_top(row) == V::kCentreRow, steps << " steps, row " << row);
    }
  }
  // Every way the cursor moves goes through it: the stored window is the function's.
  const V::Dims d{40, 6};
  V v;
  const auto agrees = [&](const char* what) { CHECK_MESSAGE(v.top_row() == V::window_top(v.row()), what); };
  CHECK(v.scroll_rows(25, d));
  agrees("scroll");
  CHECK(v.jump_row(39, d));
  agrees("jump to the last row");
  CHECK((v.row() == 39 && v.top_row() == 32));
  CHECK(v.move_up(d));
  agrees("up");
  CHECK(v.jump_row(10, d));
  agrees("jump");
  CHECK(v.jump_row(0, d));
  agrees("jump to the first row");
  CHECK(v.top_row() == -7);
  CHECK(v.move_track(3, d));
  agrees("track");
  CHECK(v.end(d));
  agrees("end");
  CHECK(v.home(d));
  agrees("home");
  CHECK(v.move_right(d));
  agrees("right");
  CHECK(v.move_left(d));
  agrees("left");
  // A pattern that got shorter under the cursor: the cursor goes back, and the window with it.
  CHECK(v.jump_row(39, d));
  CHECK(v.clamp_to(V::Dims{20, 6}));
  CHECK((v.row() == 19 && v.top_row() == 12));
  // Every zoom level shows the same 16 rows: the centre slot holds the first and the last row at 5, 8 and 12 tracks.
  for (int level = 0; level < V::kZoomCount; ++level) {
    V z;
    (void)z.set_zoom(level, d);
    CHECK(z.zoom() == level);
    (void)z.jump_row(0, d);
    CHECK_MESSAGE(z.top_row() == -V::kCentreRow, "zoom " << level);
    CHECK(z.jump_row(d.steps - 1, d));
    CHECK_MESSAGE(z.row() - z.top_row() == V::kCentreRow, "zoom " << level);
  }
}

TEST_CASE("pattern view: the track window shows 5 of 12 tracks and follows the cursor") {
  PatternView v;
  for (int i = 0; i < 5 * 5 - 1; ++i) {
    v.move_right();
  }
  CHECK((v.track() == 4 && v.column() == Column::Fx2 && v.left_track() == 0));
  v.move_right();  // track 5 (index 5): the window shifts by one track
  CHECK((v.track() == 5 && v.left_track() == 1));
  for (int i = 0; i < 5 * 6; ++i) {
    v.move_right();
  }
  CHECK((v.track() == 11 && v.left_track() == 7));  // tracks 8-12
  for (int i = 0; i < 5; ++i) {
    v.move_left();
  }
  CHECK((v.track() == 10 && v.left_track() == 7));  // moving back inside the window keeps it
  for (int i = 0; i < 5 * 4; ++i) {
    v.move_left();
  }
  CHECK((v.track() == 6 && v.left_track() == 6));
}

TEST_CASE("pattern view: column buttons") {
  PatternView v;
  CHECK(v.select_column(Column::Vol));
  CHECK_FALSE(v.select_column(Column::Vol));
  CHECK(v.cycle_fx_column());
  CHECK(v.column() == Column::Fx1);
  CHECK(v.cycle_fx_column());
  CHECK(v.column() == Column::Fx2);
  CHECK(v.cycle_fx_column());
  CHECK(v.column() == Column::Fx1);
}

namespace {
struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);

  void button(Button b, bool pressed = true) {
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), pressed));
  }
  void tap(Button b) {
    button(b);
    button(b, false);
  }
};
}  // namespace

TEST_CASE("app: draws once at start and then only when something changed") {
  Rig rig;
  CHECK(rig.app->dirty());
  rig.app->run_once();
  CHECK(rig.app->frames() == 1u);
  CHECK(rig.display.frames() == 1u);
  CHECK(rig.app->failed_frames() == 0u);
  CHECK_FALSE(rig.app->dirty());
  for (int i = 0; i < 10; ++i) {
    rig.clock.advance(1000);
    rig.app->run_once();
  }
  CHECK(rig.app->frames() == 1u);  // idle passes draw nothing

  rig.button(Button::Down);
  rig.app->run_once();
  CHECK(rig.app->view().row() == 1);
  CHECK(rig.app->frames() == 2u);
  rig.button(Button::Down, false);  // releases are ignored
  rig.button(Button::Play);          // unmapped buttons are ignored
  rig.app->run_once();
  CHECK(rig.app->frames() == 2u);
  rig.button(Button::Shift);         // but the Shift layer of the key row is a picture of its own (OS-009)
  rig.app->run_once();
  CHECK(rig.app->frames() == 3u);
  rig.button(Button::Shift, false);
  rig.button(Button::Up);
  rig.button(Button::Up);  // (OS-ROWWRAP: the second press at the top wraps to the last row; one change is as visible as two)
  rig.app->run_once();
  CHECK(rig.app->view().row() > 1);
  CHECK(rig.app->frames() == 4u);  // three events, one pass, one frame
}

TEST_CASE("app: a Shift edge redraws the key row of any screen without an overlay, press and release, and nothing under one (OS-UIFIX)") {
  Rig rig;
  rig.app->run_once();
  rig.tap(Button::Master);  // the Mixer: not the Pattern or the Song, which drew the layer before
  rig.app->run_once();
  CHECK(rig.app->state().screen == app::ScreenId::Mixer);
  std::uint32_t frames = rig.app->frames();
  rig.button(Button::Shift);
  rig.app->run_once();
  CHECK(rig.app->frames() == frames + 1u);  // the Shift row
  rig.button(Button::Shift, false);
  rig.app->run_once();
  CHECK(rig.app->frames() == frames + 2u);  // and the plain one again

  rig.button(Button::Vol);  // an overlay (Learn) owns the key row: Shift changes nothing there
  rig.button(Button::Fx);
  rig.app->run_once();
  CHECK(rig.app->overlay() == app::OverlayId::Learn);
  frames = rig.app->frames();
  rig.button(Button::Shift);
  rig.app->run_once();
  rig.button(Button::Shift, false);
  rig.app->run_once();
  CHECK(rig.app->frames() == frames);
}

TEST_CASE("app: arrows, column buttons, encoder and pads") {
  Rig rig;
  rig.app->run_once();
  rig.button(Button::Right);
  rig.button(Button::Right);
  rig.app->run_once();
  CHECK(rig.app->view().column() == Column::Vol);
  rig.button(Button::Left);
  rig.app->run_once();
  CHECK(rig.app->view().column() == Column::Instr);
  rig.tap(Button::Note);  // a tap is seen at the release now that a hold of Note opens its popup (OS-FU3)
  rig.app->run_once();
  CHECK(rig.app->view().column() == Column::Note);
  rig.tap(Button::Vol);  // Vol and Fx pressed in the same instant would be the Learn chord: tap them one by one
  rig.tap(Button::Fx);
  rig.app->run_once();
  CHECK(rig.app->view().column() == Column::Fx1);
  rig.tap(Button::Fx);
  rig.app->run_once();
  CHECK(rig.app->view().column() == Column::Fx2);

  rig.input.push(core::make_encoder_event(0, 0, 8));
  rig.app->run_once();
  CHECK(rig.app->view().row() == 8);
  rig.input.push(core::make_encoder_event(0, 0, -3));
  rig.app->run_once();
  CHECK(rig.app->view().row() == 5);
  const std::uint32_t frames = rig.app->frames();
  rig.input.push(core::make_encoder_event(0, 1, 5));  // there is only encoder 0
  rig.app->run_once();
  CHECK(rig.app->view().row() == 5);
  CHECK(rig.app->frames() == frames);

  // A pad previews in Browse, the default mode, in every column: in the Instr column too, where it no longer chooses the instrument (OS-PADHOLD).
  rig.tap(Button::Instr);
  rig.app->run_once();
  CHECK(rig.app->instrument() == 1);
  rig.input.push(core::make_pad_event(0, 3, 127));
  rig.app->run_once();
  CHECK(rig.app->instrument() == 1);
  CHECK(last_note_of(rig) == pad_note_of(rig, 3));
  rig.input.push(core::make_pad_event(0, 3, 0));   // release
  rig.input.push(core::make_pad_event(0, 48, 127));  // no such pad
  rig.input.push(core::make_pad_event(0, 47, 127));
  rig.app->run_once();
  CHECK(rig.app->instrument() == 1);
  CHECK(last_note_of(rig) == pad_note_of(rig, 47));
}

TEST_CASE("app: an F-key label is a notification: shown for 2000 ms, a second one waits its turn") {
  Rig rig;
  rig.app->run_once();
  CHECK(std::string(rig.app->message()) == "");
  CHECK(rig.app->overlay() == app::OverlayId::None);
  rig.tap(Button::F3);  // F1..F3 also have a hold binding (the choosers, OS-009): their tap is delivered at the release
  rig.app->run_once();
  CHECK(std::string(rig.app->message()) == "F3 JUMP");
  CHECK(rig.app->overlay() == app::OverlayId::Notify);
  const std::uint32_t frames = rig.app->frames();

  rig.clock.advance(1999 * core::kTicksPerMs);
  rig.app->run_once();
  CHECK(std::string(rig.app->message()) == "F3 JUMP");
  CHECK(rig.app->frames() == frames);

  rig.tap(Button::F2);  // queued behind the first: the strip keeps saying F3 until its time is up (F8 is UNDO now; F4 and F7 are "-" and say nothing: OS-CFGWIRE)
  rig.app->run_once();
  CHECK(std::string(rig.app->message()) == "F3 JUMP");
  CHECK(rig.app->state().notify.size() == 2u);
  rig.clock.advance(1 * core::kTicksPerMs);
  rig.app->run_once();
  CHECK(std::string(rig.app->message()) == "F2 LENGTH");  // its own 2000 ms start now
  rig.clock.advance(1999 * core::kTicksPerMs);
  rig.app->run_once();
  CHECK(std::string(rig.app->message()) == "F2 LENGTH");
  rig.clock.advance(1 * core::kTicksPerMs);
  rig.app->run_once();
  CHECK(std::string(rig.app->message()) == "");
  CHECK(rig.app->overlay() == app::OverlayId::None);
  CHECK(rig.app->frames() == frames + 3u);  // the second label's arrival (a frame to say "queued" is not needed: the strip
                                            // is the same), its turn, and the redraw when it expired
}

TEST_CASE("app: the message timer survives the 32-bit tick wrap") {
  Rig rig;
  rig.clock.set(0xFFFFFFFFu - 500u * core::kTicksPerMs);  // 500 ms before the wrap
  rig.app->run_once();
  rig.tap(Button::F1);
  rig.app->run_once();
  rig.clock.advance(1500 * core::kTicksPerMs);  // now past the wrap, 1.5 s after the press
  rig.app->run_once();
  CHECK(std::string(rig.app->message()) == "F1 PATTERN");
  rig.clock.advance(501 * core::kTicksPerMs);
  rig.app->run_once();
  CHECK(std::string(rig.app->message()) == "");
}

TEST_CASE("app: a busy display delays the frame without losing it") {
  Rig rig;
  rig.display.set_busy(true);
  rig.app->run_once();
  CHECK(rig.app->frames() == 0u);
  CHECK(rig.app->dirty());
  rig.button(Button::Down);
  rig.app->run_once();
  CHECK(rig.app->view().row() == 1);  // input is still handled
  CHECK(rig.app->dirty());
  rig.display.set_busy(false);
  rig.app->run_once();
  CHECK(rig.app->frames() == 1u);
  CHECK_FALSE(rig.app->dirty());
}

TEST_CASE("app: one pass handles at most 64 events") {
  Rig rig;
  rig.app->run_once();
  rig.tap(Button::Instr);  // (any column: a Browse pad remembers its note)
  rig.app->run_once();
  for (int i = 0; i < 64; ++i) {
    rig.input.push(core::make_pad_event(0, 1, 127));
  }
  rig.input.push(core::make_pad_event(0, 2, 127));
  rig.app->run_once();
  CHECK(last_note_of(rig) == pad_note_of(rig, 1));
  CHECK(rig.input.pending() == 1u);
  rig.app->run_once();
  CHECK(last_note_of(rig) == pad_note_of(rig, 2));
  CHECK(rig.input.pending() == 0u);
}

TEST_CASE("app: the demo pattern never overflows the display list") {
  Rig rig;
  rig.app->run_once();
  // Walk the cursor over the whole grid: every window position and row highlight is drawn and none overflows.
  for (int i = 0; i < 5 * 12; ++i) {
    rig.button(Button::Right);
    rig.input.push(core::make_encoder_event(0, 0, 7));
    rig.app->run_once();
  }
  CHECK(rig.app->frames() > 50u);
  CHECK(rig.app->failed_frames() == 0u);
  CHECK(rig.display.aborted() == 0u);
}

// ---- OS-002a: the input path HAL event -> gesture -> binding -> action, end to end -------------------------------

namespace {

core::KeymapResult load_user(Rig& rig, const std::string& text) {
  return rig.app->load_user_keymap(text.data(), text.size());
}
core::KeymapResult load_preset(Rig& rig, const std::string& text) {
  return rig.app->load_preset_keymap(text.data(), text.size());
}

}  // namespace

TEST_CASE("app: Vol and Fx together are the Learn chord, in either order, and neither selects a column") {
  Rig rig;
  rig.app->run_once();
  CHECK_FALSE(rig.app->learn());
  CHECK(std::string(rig.app->message()) == "");
  const std::uint32_t frames = rig.app->frames();

  rig.button(Button::Vol);
  rig.button(Button::Fx);
  rig.app->run_once();
  CHECK(rig.app->learn());
  CHECK(rig.app->overlay() == app::OverlayId::Learn);  // the Learn overlay, not a status text
  CHECK(std::string(rig.app->message()) == "");
  CHECK(rig.app->view().column() == Column::Note);  // no column change from either press
  CHECK(rig.app->frames() == frames + 1u);
  rig.button(Button::Vol, false);
  rig.button(Button::Fx, false);
  rig.clock.advance(100 * core::kTicksPerMs);
  rig.app->run_once();
  CHECK(rig.app->learn());                          // releases do nothing
  CHECK(rig.app->view().column() == Column::Note);

  rig.button(Button::Fx);  // the other order
  rig.clock.advance(10 * core::kTicksPerMs);
  rig.button(Button::Vol);
  rig.app->run_once();
  CHECK_FALSE(rig.app->learn());
  CHECK(std::string(rig.app->message()) == "");
  CHECK(rig.app->view().column() == Column::Note);
}

TEST_CASE("app: Vol held alone opens the FX1 list, not Learn; Vol and Fx together are still Learn and open no list") {
  Rig rig;
  rig.app->run_once();
  rig.button(Button::Vol);
  rig.clock.advance((core::kGestureTimings.hold_ms + 1) * core::kTicksPerMs);
  rig.app->run_once();
  CHECK(rig.app->overlay() == app::OverlayId::StepPopup);
  CHECK(rig.app->view().column() == Column::Fx1);
  CHECK_FALSE(rig.app->learn());
  rig.button(Button::Vol, false);
  rig.app->run_once();
  CHECK(rig.app->overlay() == app::OverlayId::None);
  CHECK_FALSE(rig.app->learn());

  rig.button(Button::Fx);  // the chord, Fx first and then Vol inside the window, and held well past the hold time
  rig.clock.advance(10 * core::kTicksPerMs);
  rig.button(Button::Vol);
  rig.clock.advance((core::kGestureTimings.hold_ms + 1) * core::kTicksPerMs);
  rig.app->run_once();
  CHECK(rig.app->overlay() == app::OverlayId::Learn);  // the Learn overlay, and no popup under it
  rig.button(Button::Fx, false);
  rig.button(Button::Vol, false);
  rig.app->run_once();
  CHECK(rig.app->overlay() == app::OverlayId::Learn);
}

TEST_CASE("app: Vol and Fx pressed more than the chord window apart are two column taps") {
  Rig rig;
  rig.app->run_once();
  rig.button(Button::Vol);
  rig.app->run_once();
  CHECK(rig.app->view().column() == Column::Note);  // held back: a chord partner may still come
  rig.clock.advance(39 * core::kTicksPerMs);
  rig.app->run_once();
  CHECK(rig.app->view().column() == Column::Note);
  rig.clock.advance(1 * core::kTicksPerMs);
  rig.app->run_once();
  CHECK(rig.app->view().column() == Column::Note);  // 40 ms: the window is over, but Vol also binds a hold (the FX1 list): its tap waits for the release
  rig.button(Button::Fx);                           // pressed while Vol is still down, but too late for the chord
  rig.clock.advance(40 * core::kTicksPerMs);
  rig.app->run_once();
  CHECK(rig.app->view().column() == Column::Note);  // Browse binds Fx held + the encoder (a scroll): Fx taps at its release
  rig.button(Button::Fx, false);
  rig.app->run_once();
  CHECK(rig.app->view().column() == Column::Fx1);
  CHECK_FALSE(rig.app->learn());
  rig.button(Button::Vol, false);                   // Vol's own tap, at its release
  rig.app->run_once();
  CHECK(rig.app->view().column() == Column::Vol);
  CHECK_FALSE(rig.app->learn());

  rig.button(Button::Vol);  // a tap that is released inside the window is not held back for all of it
  rig.button(Button::Vol, false);
  rig.app->run_once();
  CHECK(rig.app->view().column() == Column::Vol);
}

TEST_CASE("app: a notification posted while Learn is open waits behind it, and its time runs only once it shows") {
  Rig rig;
  rig.app->run_once();
  rig.button(Button::Vol);
  rig.button(Button::Fx);
  rig.app->run_once();
  CHECK(rig.app->learn());
  rig.button(Button::F3);  // the overlay captures the F-keys: no notification
  rig.tap(Button::Left);
  rig.app->run_once();
  CHECK(rig.app->state().notify.empty());
  CHECK(rig.app->view().column() == Column::Note);

  // Something that posts anyway (a script, later the SD loader): it queues, hidden.
  CHECK(rig.app->invoke(core::action_id("ui.softkey"), 2) == app::DispatchResult::Changed);
  rig.app->run_once();
  CHECK(std::string(rig.app->message()) == "");
  const std::uint32_t frames = rig.app->frames();
  rig.clock.advance(10000 * core::kTicksPerMs);  // however long Learn stays open, the queue does not run down
  rig.app->run_once();
  CHECK(rig.app->state().notify.size() == 1u);
  CHECK(rig.app->frames() == frames);

  rig.button(Button::Fx);  // the chord again closes Learn: now it shows, with its whole 2000 ms
  rig.button(Button::Vol);
  rig.app->run_once();
  CHECK_FALSE(rig.app->learn());
  CHECK(std::string(rig.app->message()) == "F3 JUMP");
  rig.clock.advance(1999 * core::kTicksPerMs);
  rig.app->run_once();
  CHECK(std::string(rig.app->message()) == "F3 JUMP");
  rig.clock.advance(1 * core::kTicksPerMs);
  rig.app->run_once();
  CHECK(std::string(rig.app->message()) == "");
}

TEST_CASE("app: invoke() runs an action by id without a gesture and redraws when something changed") {
  Rig rig;
  rig.app->run_once();
  CHECK_FALSE(rig.app->dirty());
  CHECK(rig.app->invoke(core::action_id("nav.scroll"), 5) == app::DispatchResult::Changed);
  CHECK(rig.app->view().row() == 5);
  CHECK(rig.app->dirty());
  rig.app->run_once();
  CHECK_FALSE(rig.app->dirty());
  CHECK(rig.app->invoke(core::action_id("nav.scroll"), 0) == app::DispatchResult::Unchanged);
  CHECK_FALSE(rig.app->dirty());
  CHECK(rig.app->invoke(core::action_id("no.such")) == app::DispatchResult::Unknown);
  CHECK(rig.app->invoke(core::action_id("instrument.select"), 9) == app::DispatchResult::Changed);
  CHECK(rig.app->instrument() == 9);
  CHECK(rig.app->invoke(core::action_id("learn.toggle")) == app::DispatchResult::Changed);
  CHECK(rig.app->learn());
}

TEST_CASE("app: a user keymap rebinds, unbinds and adds a hold, and a hold costs the tap only its release") {
  Rig rig;
  rig.app->run_once();
  const auto r = load_user(rig,
                           "o3keymap 1\n"
                           "pattern tap left nav.right\n"      // rebinds a built-in
                           "pattern tap right none\n"          // switches one off
                           "global hold f4 ui.softkey 7\n");   // a gesture the built-ins do not use (F1..F3 hold the choosers now)
  CHECK(r.ok());
  CHECK(r.count == 3u);

  rig.button(Button::Left);
  rig.button(Button::Left, false);
  rig.app->run_once();
  CHECK(rig.app->view().column() == Column::Instr);
  rig.button(Button::Right);
  rig.button(Button::Right, false);
  rig.app->run_once();
  CHECK(rig.app->view().column() == Column::Instr);  // unbound: nothing happens, and it does not fall through
  rig.button(Button::Down);                           // untouched built-ins still work
  rig.app->run_once();
  CHECK(rig.app->view().row() == 1);

  // F4 now also has a hold binding, so its tap is delivered at the release, not at the press.
  rig.button(Button::F4);
  rig.clock.advance(100 * core::kTicksPerMs);
  rig.app->run_once();
  CHECK(std::string(rig.app->message()) == "");
  rig.button(Button::F4, false);
  rig.app->run_once();
  CHECK(std::string(rig.app->message()) == "FILL: TURN EDIT ON");  // F4 is FILL since OS-PTOOLS: in Browse it says what it needs
  rig.clock.advance(2000 * core::kTicksPerMs);  // (nothing queued behind it)
  rig.app->run_once();
  CHECK(std::string(rig.app->message()) == "");

  // Held for 400 ms: the hold runs its action, and the release then adds nothing.
  rig.button(Button::F4);
  rig.clock.advance(399 * core::kTicksPerMs);
  rig.app->run_once();
  CHECK(std::string(rig.app->message()) == "");
  rig.clock.advance(1 * core::kTicksPerMs);
  rig.app->run_once();
  CHECK(std::string(rig.app->message()) == "F8 UNDO");
  rig.button(Button::F4, false);
  rig.app->run_once();
  CHECK(std::string(rig.app->message()) == "F8 UNDO");
  CHECK(rig.app->state().notify.size() == 1u);  // the release did not post a tap as well
  rig.clock.advance(2000 * core::kTicksPerMs);
  rig.app->run_once();
  // Other F-keys without a hold binding keep tapping at the press.
  rig.button(Button::F5);
  rig.app->run_once();
  CHECK(std::string(rig.app->message()) == "PREVIEW: NO AUDIO OUTPUT");  // (F5 is PREVIEW since OS-CFGWIRE; the bare app has no engine)
}

TEST_CASE("app: user over preset over built-in, and a layer can be emptied again") {
  Rig rig;
  rig.app->run_once();
  const std::string preset = "o3keymap 1\npattern tap left nav.down\npattern tap right nav.down\n";
  const std::string user = "o3keymap 1\npattern tap left nav.right\n";
  const std::string empty = "o3keymap 1\n";
  const auto left = [&] {
    rig.button(Button::Left);
    rig.button(Button::Left, false);
    rig.app->run_once();
  };
  CHECK(load_preset(rig, preset).ok());
  CHECK(load_user(rig, user).ok());
  left();  // user: right
  CHECK(rig.app->view().column() == Column::Instr);
  CHECK(rig.app->view().row() == 0);
  rig.button(Button::Right);  // preset: down (no user binding for it)
  rig.button(Button::Right, false);
  rig.app->run_once();
  CHECK(rig.app->view().row() == 1);

  CHECK(load_user(rig, empty).ok());
  left();  // preset: down
  CHECK(rig.app->view().row() == 2);
  CHECK(load_preset(rig, empty).ok());
  left();  // built-in: left, from Instr back to Note
  CHECK(rig.app->view().column() == Column::Note);
  CHECK(rig.app->view().row() == 2);
}

TEST_CASE("app: a rejected keymap leaves its layer empty and the built-in bindings in force") {
  Rig rig;
  rig.app->run_once();
  CHECK(load_user(rig, "o3keymap 1\npattern tap left nav.right\n").ok());
  const auto r = load_user(rig, "o3keymap 1\npattern tap left nav.right\npattern tap left nav.up\n");
  CHECK(r.error == core::KeymapError::Conflict);
  CHECK(r.line == 3);
  CHECK(r.other_line == 2);
  CHECK(rig.app->bindings().layers[0].count == 0u);
  rig.button(Button::Right);
  rig.button(Button::Right, false);
  rig.button(Button::Left);
  rig.button(Button::Left, false);
  rig.app->run_once();
  CHECK(rig.app->view().column() == Column::Note);  // right then left, as built in
  CHECK(rig.app->bindings().layers[2].count == app::builtin_bindings().count);

  CHECK(load_preset(rig, "pattern tap left nav.right\n").error == core::KeymapError::MissingHeader);
  CHECK(rig.app->bindings().layers[1].count == 0u);
}

TEST_CASE("app: Shift is a modifier, but holding it never kills a plain binding") {
  Rig rig;
  rig.app->run_once();
  CHECK(load_user(rig, "o3keymap 1\npattern tap shift+left nav.down\n").ok());
  rig.button(Button::Shift);
  rig.button(Button::Left);
  rig.button(Button::Left, false);
  rig.app->run_once();
  CHECK(rig.app->view().row() == 1);                 // the Shift binding wins over the plain one
  CHECK(rig.app->view().column() == Column::Note);
  rig.button(Button::Instr);                          // Shift+Instr is the 1/4 jump (OS-QJUMP): the column does not change
  rig.button(Button::Instr, false);
  rig.app->run_once();
  CHECK(rig.app->view().column() == Column::Note);
  CHECK(rig.app->view().row() == 16);
  rig.button(Button::Up);                             // (Shift+Up is the block's, since OS-009: it moves up all the same)
  rig.button(Button::Up, false);
  rig.app->run_once();
  CHECK(rig.app->view().row() == 15);
  rig.button(Button::Shift, false);
  rig.button(Button::Left);                           // and without Shift, Left is Left
  rig.button(Button::Left, false);
  rig.app->run_once();
  CHECK(rig.app->view().column() == Column::Note);
  CHECK(rig.app->view().row() == 15);
}

TEST_CASE("app: Shift+Note and Shift+Instr are the quarter jumps (OS-QJUMP), the column stays, and Browse's rows hold under Shift") {
  Rig rig;
  rig.app->run_once();
  rig.button(Button::Right);
  rig.button(Button::Right, false);
  rig.app->run_once();
  CHECK(rig.app->view().column() == Column::Instr);
  rig.button(Button::Shift);
  rig.button(Button::Note);
  rig.button(Button::Note, false);
  rig.app->run_once();
  CHECK(rig.app->view().column() == Column::Instr);  // not the Note button's column
  CHECK(rig.app->view().row() == 0);
  rig.button(Button::Instr);
  rig.button(Button::Instr, false);
  rig.input.push(core::make_encoder_event(rig.clock.now(), 0, 1));  // the encoder (a page of rows under Shift: OS-SHIFTENC) and the pads too: Browse's own rows hold under Shift
  rig.input.push(core::make_pad_event(rig.clock.now(), 30, 100));   // (pads 0..23 are mute and solo under Shift: pad 30 is free)
  rig.app->run_once();
  CHECK(rig.app->view().row() == 32);  // 1/4 of 64 is 16, then a detent is a page of 16
  CHECK(last_note_of(rig) == pad_note_of(rig, 30));
}

TEST_CASE("app: a button held while the encoder turns can be bound, and its release is then silent") {
  Rig rig;
  rig.app->run_once();
  CHECK(load_user(rig, "o3keymap 1\npattern holdenc instr+enc0 pattern.column 3\n").ok());
  rig.button(Button::Instr);
  rig.input.push(core::make_encoder_event(rig.clock.now(), 0, 2));
  rig.app->run_once();
  CHECK(rig.app->view().column() == Column::Fx1);
  CHECK(rig.app->view().row() == 0);  // the turn did not scroll
  rig.button(Button::Instr, false);
  rig.app->run_once();
  CHECK(rig.app->view().column() == Column::Fx1);  // no tap of Instr after it

  rig.button(Button::Note);  // without a binding for the pair, a turn with a button held is a plain turn
  rig.input.push(core::make_encoder_event(rig.clock.now(), 0, 4));
  rig.app->run_once();
  CHECK(rig.app->view().row() == 4);
}

TEST_CASE("app: pads and encoder go through the same path, with the pad index and the detents as arguments") {
  Rig rig;
  rig.app->run_once();
  rig.tap(Button::Instr);  // in Browse, the default mode, a pad previews (OS-PADHOLD: in the Instr column too; it does not choose the instrument)
  rig.input.push(core::make_pad_event(0, 9, 100));
  rig.app->run_once();
  CHECK(rig.app->instrument() == 1);
  CHECK(last_note_of(rig) == pad_note_of(rig, 9));
  CHECK(load_user(rig, "o3keymap 1\npattern.browse tap pad9 instrument.select 2\n").ok());  // a fixed argument for one pad
  rig.input.push(core::make_pad_event(0, 9, 100));
  rig.app->run_once();
  CHECK(rig.app->instrument() == 2);
  rig.input.push(core::make_pad_event(0, 8, 100));  // the other pads: the built-in wildcard
  rig.app->run_once();
  CHECK(rig.app->instrument() == 2);  // (unchanged: the built-in pad does not choose it)
  CHECK(last_note_of(rig) == pad_note_of(rig, 8));
  rig.input.push(core::make_encoder_event(0, 0, -1));
  rig.app->run_once();
  CHECK(rig.app->view().row() == 0);
  rig.input.push(core::make_encoder_event(0, 0, 3));
  rig.app->run_once();
  CHECK(rig.app->view().row() == 3);
}

// ---- the review round: context-first resolution, shadowed chords, capped passes, lost releases -----------------

TEST_CASE("app: a preset line in global does not override a screen's built-in binding, one in the screen does") {
  Rig rig;
  rig.app->run_once();
  rig.tap(Button::Instr);
  // OS-PADHOLD: the built-in pad of Browse previews (it remembers its note) and chooses nothing, so "the built-in ran" is read from the note of the pad it was given: a different pad each time.
  const auto pad = [&](unsigned n) {
    rig.input.push(core::make_pad_event(rig.clock.now(), static_cast<std::uint8_t>(n), 100));
    rig.app->run_once();
  };
  pad(3);
  CHECK(last_note_of(rig) == pad_note_of(rig, 3));    // the built-in pad* of Browse (the mode's own context)
  CHECK(rig.app->instrument() == 1);

  CHECK(load_preset(rig, "o3keymap 1\nglobal tap pad* instrument.select 20\n").ok());
  pad(4);
  CHECK(last_note_of(rig) == pad_note_of(rig, 4));    // global is outside the screen: the screen's own binding ended the walk
  CHECK(rig.app->instrument() == 1);
  CHECK(load_user(rig, "o3keymap 1\nglobal tap pad* instrument.select 30\n").ok());
  pad(5);
  CHECK(last_note_of(rig) == pad_note_of(rig, 5));    // not even the user layer, from global
  CHECK(rig.app->instrument() == 1);

  CHECK(load_preset(rig, "o3keymap 1\npattern tap pad* instrument.select 25\n").ok());
  pad(6);
  CHECK(last_note_of(rig) == pad_note_of(rig, 6));    // the screen's own context is outside the mode's: Browse's binding ended the walk
  CHECK(rig.app->instrument() == 1);
  CHECK(load_preset(rig, "o3keymap 1\npattern.browse tap pad* instrument.select 20\n").ok());
  pad(7);
  CHECK(rig.app->instrument() == 20);                 // in the mode's context the preset layer beats the built-in
  CHECK(load_user(rig, "o3keymap 1\npattern.browse tap pad* instrument.select 30\n").ok());
  pad(8);
  CHECK(rig.app->instrument() == 30);                 // and the user layer beats the preset
}

TEST_CASE("app: a chord switched off with `none` does not hold its buttons back, and they tap") {
  Rig rig;
  rig.app->run_once();
  CHECK(load_user(rig, "o3keymap 1\nglobal chord vol+fx none\n").ok());
  rig.button(Button::Vol);  // no chord is bound any more: Vol is not held back for a partner, and taps at its release (it binds a hold, the FX1 list)
  rig.app->run_once();
  CHECK(rig.app->view().column() == Column::Note);
  rig.button(Button::Fx);   // and so does Fx: not after the chord window, at its release (Browse binds Fx held + the encoder)
  rig.app->run_once();
  CHECK(rig.app->view().column() == Column::Note);
  rig.button(Button::Fx, false);
  rig.app->run_once();
  CHECK(rig.app->view().column() == Column::Fx1);
  CHECK_FALSE(rig.app->learn());
  rig.button(Button::Vol, false);
  rig.app->run_once();
  CHECK(rig.app->view().column() == Column::Vol);

  rig.button(Button::Vol);  // the same instant, the way the chord was made: still two taps
  rig.button(Button::Fx);
  rig.app->run_once();
  CHECK_FALSE(rig.app->learn());
  rig.button(Button::Vol, false);
  rig.button(Button::Fx, false);
  rig.app->run_once();
  CHECK(rig.app->view().column() == Column::Fx1);   // Vol selected the Vol column, Fx cycled on from it to FX1
}

TEST_CASE("app: after a pass cut short by the event cap, time does not overtake the events still queued") {
  Rig rig;
  rig.app->run_once();
  CHECK(load_user(rig, "o3keymap 1\nglobal hold f1 ui.softkey 7\n").ok());
  // 63 pad hits, then the press of F1, then (past the cap of 64) its release 399 ms after the press: a tap.
  // The clock is already well past the hold deadline, but the release is older than "now" and must come first.
  for (int i = 0; i < 63; ++i) {
    rig.input.push(core::make_pad_event(0, 1, 100));
  }
  rig.input.push(core::make_button_event(0, static_cast<std::uint8_t>(Button::F1), true));
  rig.input.push(core::make_button_event(399 * core::kTicksPerMs, static_cast<std::uint8_t>(Button::F1), false));
  rig.clock.set(1000 * core::kTicksPerMs);
  rig.app->run_once();  // handles 64 events and stops
  CHECK(rig.input.pending() == 1u);
  CHECK(std::string(rig.app->message()) == "");  // the hold must not have fired on the strength of "now"
  rig.app->run_once();
  CHECK(rig.input.pending() == 0u);
  CHECK(std::string(rig.app->message()) == "F1 PATTERN");  // the release made it a tap
  rig.clock.advance(1000 * core::kTicksPerMs);
  rig.app->run_once();
  CHECK(std::string(rig.app->message()) == "F1 PATTERN");  // and nothing fires later: no second notification queued
  CHECK(rig.app->state().notify.size() == 1u);
}

TEST_CASE("app: reset_input forgets a Shift whose release was lost") {
  Rig rig;
  rig.app->run_once();
  CHECK(load_user(rig, "o3keymap 1\npattern tap shift+down nav.right\n").ok());
  rig.button(Button::Shift);  // never released
  rig.button(Button::Down);
  rig.button(Button::Down, false);
  rig.app->run_once();
  CHECK(rig.app->view().column() == Column::Instr);  // still taken as Shift+Down
  CHECK(rig.app->view().row() == 0);
  rig.app->reset_input();
  rig.button(Button::Down);
  rig.button(Button::Down, false);
  rig.app->run_once();
  CHECK(rig.app->view().row() == 1);                  // plain Down again
  rig.button(Button::Shift, false);                   // the late release is harmless
  rig.app->run_once();
  CHECK(rig.app->view().row() == 1);
}

// ---- OS-003b: the colour theme ----------------------------------------------------------------------------------

TEST_CASE("app: Shift + Master is ui.theme.next: Amiga, Black, White, Amiga, and each switch redraws") {
  Rig rig;
  rig.app->run_once();
  CHECK(rig.app->theme() == ui::ThemeId::Amiga);  // the default
  const auto switch_theme = [&] {
    rig.button(Button::Shift);
    rig.button(Button::Master);
    rig.button(Button::Master, false);
    rig.button(Button::Shift, false);
    rig.app->run_once();
  };
  const std::uint32_t frames = rig.app->frames();
  switch_theme();
  CHECK(rig.app->theme() == ui::ThemeId::Black);
  CHECK(rig.app->frames() == frames + 1u);
  switch_theme();
  CHECK(rig.app->theme() == ui::ThemeId::White);
  switch_theme();
  CHECK(rig.app->theme() == ui::ThemeId::Amiga);
  CHECK(rig.app->frames() == frames + 3u);
  CHECK(rig.app->failed_frames() == 0u);
  // Master without Shift is the Mixer's button (OS-011b): it opens that screen; the theme stays.
  rig.tap(Button::Master);
  rig.app->run_once();
  CHECK(rig.app->theme() == ui::ThemeId::Amiga);
  CHECK(rig.app->state().screen == app::ScreenId::Mixer);
  CHECK(rig.app->frames() == frames + 4u);
  CHECK((rig.app->view().row() == 0 && rig.app->view().column() == Column::Note && rig.app->instrument() == 1));
}

TEST_CASE("app: the theme action by name and set_theme redraw once, and a keymap can move or remove the key") {
  Rig rig;
  rig.app->run_once();
  const core::ActionId next = core::action_id("ui.theme.next");
  CHECK(app::find_action(app::action_registry(), next) != nullptr);
  std::uint32_t frames = rig.app->frames();
  CHECK(rig.app->invoke(next) == app::DispatchResult::Changed);
  CHECK(rig.app->theme() == ui::ThemeId::Black);
  CHECK(rig.app->dirty());
  rig.app->run_once();
  CHECK(rig.app->frames() == ++frames);
  rig.app->set_theme(ui::ThemeId::White);
  CHECK(rig.app->dirty());
  rig.app->run_once();
  CHECK(rig.app->frames() == ++frames);
  rig.app->set_theme(ui::ThemeId::White);  // already there: nothing to redraw
  CHECK_FALSE(rig.app->dirty());

  // A user keymap moves the switch to F1 (in the pattern context, where F1 is bound), and F1 no longer shows its label...
  CHECK(load_user(rig, "o3keymap 1\npattern tap f1 ui.theme.next\n").ok());
  rig.tap(Button::F1);
  rig.app->run_once();
  CHECK(rig.app->theme() == ui::ThemeId::Amiga);
  CHECK(rig.app->state().notify.empty());
  // ...and a line of `none` takes Shift + Master away.
  CHECK(load_user(rig, "o3keymap 1\nglobal tap shift+master none\n").ok());
  rig.button(Button::Shift);
  rig.tap(Button::Master);
  rig.button(Button::Shift, false);
  rig.app->run_once();
  CHECK(rig.app->theme() == ui::ThemeId::Amiga);
}
