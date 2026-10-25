// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-CFGWIRE: the device settings that now have a consumer (DISPLAY and PAD LIGHTS brightness, NUMBERING, MIDDLE C), as pure functions and on the Pattern view. The Config screen's own rows are
// config_screen_test.cpp and the gallery; the soft keys (F5 PREVIEW, F6 INVERT, FILL, EXPORT) are pattern_edit_app_test.cpp.
#include <doctest/doctest.h>

#include <o3os/app/brightness.h>
#include <o3os/app/pad_palette.h>
#include <o3os/app/pattern_data.h>
#include <o3os/app/pattern_view.h>
#include <o3os/app/screens/pattern_screen.h>
#include <o3os/app/app.h>
#include <o3os/eve/display_list_builder.h>
#include <o3os/model/params.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/ui/assets.h>
#include <o3os/ui/renderer.h>
#include <o3os/ui/theme.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace o3os;
namespace ds = model::device_setting;

TEST_CASE("cfgwire: DISPLAY brightness is a PWM duty of 20, 50 or 128, PAD LIGHTS thirds of the level (a floor, never above the level)") {
  // The display brightness steps (this OS's table): 20, 50, 128.
  CHECK(app::backlight_duty(0) == 20);
  CHECK(app::backlight_duty(1) == 50);
  CHECK(app::backlight_duty(2) == 128);
  CHECK(app::backlight_duty(3) == 128);   // past the list: High
  CHECK(app::backlight_duty(99) == 128);
  CHECK(app::brightness_index(0.0f) == 0);
  CHECK(app::brightness_index(1.0f) == 1);
  CHECK(app::brightness_index(2.0f) == 2);
  CHECK(app::brightness_index(7.0f) == 2);
  // the pad levels times (setting + 1) / 3 (this OS's rule).
  for (unsigned level = 0; level <= 255; ++level) {
    const std::uint8_t l = static_cast<std::uint8_t>(level);
    CHECK(app::scale_pad_level(l, 2) == l);                                       // High changes nothing
    CHECK(app::scale_pad_level(l, 1) <= l);
    CHECK(app::scale_pad_level(l, 0) <= app::scale_pad_level(l, 1));
    CHECK((app::scale_pad_level(l, 0) == 0) == (level == 0));                     // a lit pad is never put out
    if (level >= app::kPadLevelFloor) {
      CHECK(app::scale_pad_level(l, 0) >= app::kPadLevelFloor);
    }
  }
  CHECK(app::scale_pad_level(255, 0) == 85);
  CHECK(app::scale_pad_level(255, 1) == 170);
  // Through the palette, in every theme: Front stays above Back at every brightness.
  for (unsigned t = 0; t < 3; ++t) {
    const ui::Theme& theme = ui::theme(ui::theme_id_at(t));
    for (unsigned b = 0; b < 3; ++b) {
      const std::uint8_t front = app::scale_pad_level(app::pad_level(theme, app::PadRole::Front), b);
      const std::uint8_t back = app::scale_pad_level(app::pad_level(theme, app::PadRole::Back), b);
      CHECK_MESSAGE(front > back, "theme " << t << " brightness " << b);
      CHECK(back > 0);
      CHECK(app::scale_pad_level(app::pad_level(theme, app::PadRole::Off), b) == 0);
    }
  }
}

TEST_CASE("cfgwire: NUMBERING: the row tile counts from 0, from 1 or in hex") {
  using app::Numbering;
  const auto tile = [](int row, Numbering n) { return app::row_number_tile(row, static_cast<unsigned>(n)); };
  CHECK(tile(0, Numbering::From0) == 0x00);
  CHECK(tile(9, Numbering::From0) == 0x09);
  CHECK(tile(10, Numbering::From0) == 0x10);  // packed decimal: the tile draws 10
  CHECK(tile(63, Numbering::From0) == 0x63);
  CHECK(tile(0, Numbering::From1) == 0x01);   // our default: the rows from 1 to the length
  CHECK(tile(8, Numbering::From1) == 0x09);
  CHECK(tile(9, Numbering::From1) == 0x10);
  CHECK(tile(63, Numbering::From1) == 0x64);
  CHECK(tile(99, Numbering::From1) == 0xA0);  // 100: the tens digit overflows into the hex digit A, as the plain bcd always did (rows past 99)
  CHECK(tile(0, Numbering::Hex) == 0x00);
  CHECK(tile(10, Numbering::Hex) == 0x0A);
  CHECK(tile(63, Numbering::Hex) == 0x3F);
  CHECK(tile(127, Numbering::Hex) == 0x7F);
  CHECK(app::row_number_tile(5, 77) == app::row_number_tile(5, static_cast<unsigned>(Numbering::From1)));  // unknown: from 1
  CHECK(app::row_number_tile(-3, 0) == 0x00);
}

TEST_CASE("cfgwire: MIDDLE C shifts the note names by octaves, and a name that has no tile is the unknown tile") {
  using app::note_tile_cell;
  // No shift: the plain cell of every byte.
  for (unsigned n = 0; n < 256; ++n) {
    CHECK(note_tile_cell(static_cast<std::uint8_t>(n), 0) == note_tile_cell(static_cast<std::uint8_t>(n)));
  }
  const auto name = [](unsigned cell) {
    char text[4];
    ui::note_tile_text(cell, text);
    return std::string(text);
  };
  // Note byte 61 is MIDI note 60: this OS's default names it C-5; the setting C-4 names it C-4, C-3 names it C-3, C-6 names it C-6.
  CHECK(name(note_tile_cell(61, 0)) == "C-5");
  CHECK(name(note_tile_cell(61, -1)) == "C-4");
  CHECK(name(note_tile_cell(61, -2)) == "C-3");
  CHECK(name(note_tile_cell(61, +1)) == "C-6");
  CHECK(name(note_tile_cell(62, -2)) == "C#3");
  // The ends: below C-0 and past G-10 there is no name.
  CHECK(note_tile_cell(1, -1) == ui::kNoteTileUnknown);
  CHECK(note_tile_cell(24, -2) == ui::kNoteTileUnknown);
  CHECK(name(note_tile_cell(25, -2)) == "C-0");
  CHECK(name(note_tile_cell(128, 0)) == "G10");
  CHECK(note_tile_cell(128, +1) == ui::kNoteTileUnknown);
  CHECK(name(note_tile_cell(116, +1)) == "G10");  // byte 116 is G-9: an octave up is the last name there is
  CHECK(note_tile_cell(117, +1) == ui::kNoteTileUnknown);
  // Empty, off, cut and fade have no octave.
  CHECK(note_tile_cell(0, -2) == 0u);
  CHECK(note_tile_cell(model::kNoteOff, -2) == ui::kNoteTileOff);
  CHECK(note_tile_cell(model::kNoteCut, +1) == ui::kNoteTileCut);
  CHECK(note_tile_cell(model::kNoteFade, +1) == ui::kNoteTileFade);
}

TEST_CASE("cfgwire: the Pattern view reads NUMBERING and MIDDLE C from the device settings; the defaults are ours") {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> a = std::make_unique<app::App>(input, clock, display);
  a->run_once();
  const app::Model& m = a->model();
  CHECK(app::pattern_numbering(m) == static_cast<unsigned>(app::Numbering::From1));  // our default counts the rows from 1
  CHECK(app::pattern_octave_shift(m) == 0);                                         // C-5: our name of MIDI note 60
  for (unsigned v = 0; v < 3; ++v) {
    CHECK(a->device_settings().set(ds::Numbering, static_cast<float>(v)) == model::Status::Ok);
    CHECK(app::pattern_numbering(a->model()) == v);
  }
  for (unsigned v = 0; v < 4; ++v) {
    CHECK(a->device_settings().set(ds::MiddleC, static_cast<float>(v)) == model::Status::Ok);
    CHECK(app::pattern_octave_shift(a->model()) == static_cast<int>(v) - 2);
  }
}
