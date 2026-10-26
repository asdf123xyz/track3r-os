// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-007d, the app side of the input: the raw picture of the INPUT TEST screen (input_test.h), the pad LED palette from the theme's tokens
// (pad_palette.h), the hidden screen in the registry and its capturing context, and the whole screen driven by scripted input
// (every control seen raw, none acting, the two chords). The platform's own tests are platform/mk66/tests/mk66_input_*_test.cpp; the
// two layers meet in the key map's names here.
#include <doctest/doctest.h>

#include <o3os/app/app.h>
#include <o3os/app/bindings.h>
#include <o3os/app/buttons.h>
#include <o3os/app/input_test.h>
#include <o3os/app/pad_palette.h>
#include <o3os/app/screens/ids.h>
#include <o3os/app/screens/input_test_screen.h>
#include <o3os/app/screens/screen.h>
#include <o3os/core/input_event.h>
#include <o3os/hal/leds.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/platform/mk66/input/input_map.h>
#include <o3os/platform/mk66/input/pad_leds.h>
#include <o3os/platform/mk66/input/scanner.h>
#include "../platform/mk66/tests/mk66_input_fakes.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

using namespace o3os;
using app::Button;
using app::ScreenId;
namespace mk66in = o3os::platform::mk66::input;

// ---- the raw picture -----------------------------------------------------------------------------------------------------------

TEST_CASE("input test: a press is counted once however the chip repeats it, a release clears the down bit") {
  app::InputTestState s;
  CHECK(s.observe(core::make_button_event(100, 23, true), 150));
  CHECK(s.observe(core::make_button_event(110, 23, true), 150));  // the same press again: still one
  CHECK(s.button_presses[23] == 1);
  CHECK(((s.buttons_down >> 23) & 1u) == 1u);
  CHECK(s.observe(core::make_button_event(200, 23, false), 250));
  CHECK(((s.buttons_down >> 23) & 1u) == 0u);
  CHECK(s.observe(core::make_button_event(300, 23, true), 350));  // a second touch
  CHECK(s.button_presses[23] == 2);
  CHECK(s.events == 4u);
}

TEST_CASE("input test: pads, the encoder and the age of an event") {
  app::InputTestState s;
  CHECK(s.observe(core::make_pad_event(1000, 47, 127), 1800));
  CHECK(((s.pads_down >> 47) & 1u) == 1u);
  CHECK(s.pad_presses[47] == 1);
  CHECK(s.last_age_us == 800u);
  CHECK(s.observe(core::make_pad_event(2000, 47, 0), 2100));
  CHECK(((s.pads_down >> 47) & 1u) == 0u);
  CHECK(s.max_age_us == 800u);  // the longest seen
  CHECK(s.observe(core::make_encoder_event(3000, 0, 5), 3100));
  CHECK(s.observe(core::make_encoder_event(3100, 0, -2), 3200));
  CHECK(s.encoder_total == 3);
  CHECK(s.encoder_last == -2);
  CHECK(s.encoder_events == 2u);
  // the age is taken across the 32-bit tick wrap
  CHECK(s.observe(core::make_pad_event(0xFFFFFF00u, 1, 127), 0x00000100u));
  CHECK(s.last_age_us == 0x200u);
}

TEST_CASE("input test: ids outside the matrices and kinds the panel has not are counted but change nothing") {
  app::InputTestState s;
  CHECK_FALSE(s.observe(core::make_button_event(0, 33, true), 0));  // 33 buttons: 0..32
  CHECK_FALSE(s.observe(core::make_pad_event(0, 48, 127), 0));      // 48 pads: 0..47
  CHECK_FALSE(s.observe(core::InputEvent{0, core::InputKind::MidiNote, 60, 100}, 0));
  CHECK_FALSE(s.observe(core::InputEvent{0, core::InputKind::PcKey, 4, 1}, 0));
  CHECK(s.buttons_down == 0u);
  CHECK(s.pads_down == 0u);
  CHECK(s.events == 4u);
}

TEST_CASE("input test: a count stops at 99, a clear forgets the counts and keeps what is physically down") {
  app::InputTestState s;
  for (int i = 0; i < 120; ++i) {
    (void)s.observe(core::make_pad_event(0, 3, 127), 0);
    (void)s.observe(core::make_pad_event(0, 3, 0), 0);
  }
  CHECK(s.pad_presses[3] == app::kInputTestCountMax);
  (void)s.observe(core::make_button_event(0, 29, true), 0);
  (void)s.observe(core::make_encoder_event(0, 0, 4), 0);
  s.reset();
  CHECK(s.pad_presses[3] == 0);
  CHECK(s.encoder_total == 0);
  CHECK(s.events == 0u);
  CHECK(((s.buttons_down >> 29) & 1u) == 1u);  // Shift is still held: the picture of the hardware stays
}

TEST_CASE("input test: health counters are taken over only when they differ") {
  app::InputTestState s;
  hal::InputStats h;
  CHECK_FALSE(s.set_stats(h));
  h.errors = 2;
  h.online = hal::InputStats::kPadsOnline;
  CHECK(s.set_stats(h));
  CHECK_FALSE(s.set_stats(h));
  CHECK(s.stats.errors == 2u);
  h.pad_bus.scanner.count = 1;  // a healthy bus's record is not drawn: not compared (OS-BUG-PADS)
  CHECK_FALSE(s.set_stats(h));
  h.pad_bus.scanner.first = 2;  // a bus in trouble is drawn: its record is part of what is compared
  CHECK(s.set_stats(h));
  CHECK_FALSE(s.set_stats(h));
}

TEST_CASE("input test: a healthy pad bus never makes the screen redraw, a bus in trouble only when the drawn line changes (OS-BUG-PADS)") {
  app::InputTestState s;
  hal::InputStats h;
  h.online = hal::InputStats::kPadsOnline | hal::InputStats::kButtonsOnline;
  h.pad_bus.scanner = {0, 0, 1};
  h.pad_bus.leds = {0, 0, 1};
  CHECK(s.set_stats(h));  // the first look
  // The pad scanner is polled every 10 ms: its attempts, the pin levels, the clears and the unused bits move on a healthy unit, and none of it is on screen.
  for (unsigned i = 0; i < 40; ++i) {
    h.pad_bus.scanner.count += 1;
    h.pad_bus.leds.count += 3;
    h.pad_bus.scanner.last = static_cast<std::uint8_t>(i % 3);  // acked first: a later refusal is not "trouble" and not drawn
    h.pad_bus.clear = (i % 2) != 0u ? hal::PadBusStats::kClearReleased : hal::BusProbe::kNone;
    h.pad_bus.clears = static_cast<std::uint8_t>(i);
    h.pad_bus.lines_first = static_cast<std::uint8_t>(i & 7u);
    h.pad_bus.lines_now = static_cast<std::uint8_t>(7u - (i & 7u));
    CHECK_FALSE(s.set_stats(h));
    CHECK_FALSE(s.pad_bus_trouble());
  }
  // What is drawn on the strip still counts, and so do the two online flags (the other bits of `online` are not drawn).
  h.errors = 1;
  CHECK(s.set_stats(h));
  h.online = static_cast<std::uint8_t>(h.online | 0x80u);
  CHECK_FALSE(s.set_stats(h));
  h.online = hal::InputStats::kButtonsOnline;  // the pad scanner went away: the bus line appears
  CHECK(s.set_stats(h));
  CHECK(s.pad_bus_trouble());
  // In trouble the line is drawn: every part of it counts, what it does not show does not.
  h.pad_bus.scanner.count += 1;
  CHECK(s.set_stats(h));
  h.pad_bus.lines_now = static_cast<std::uint8_t>(h.pad_bus.lines_now ^ hal::PadBusStats::kLineSda);
  CHECK(s.set_stats(h));
  h.pad_bus.lines_first = static_cast<std::uint8_t>(h.pad_bus.lines_first ^ hal::PadBusStats::kLineScl);
  CHECK(s.set_stats(h));
  h.pad_bus.clear = hal::PadBusStats::kClearHeld;
  CHECK(s.set_stats(h));
  h.pad_bus.leds.last = 1;
  CHECK(s.set_stats(h));
  CHECK_FALSE(s.set_stats(h));
  h.pad_bus.clears = static_cast<std::uint8_t>(h.pad_bus.clears + 1u);  // how many clears is not on the line
  h.pad_bus.lines_now = static_cast<std::uint8_t>(h.pad_bus.lines_now | 0xF8u);  // nor are bits above INT
  CHECK_FALSE(s.set_stats(h));
  h.pad_bus.scanner.count = 1000;  // past the end of what the line shows ("999+")
  CHECK(s.set_stats(h));
  h.pad_bus.scanner.count = 4000000000u;
  h.pad_bus.leds.count = 5000;
  CHECK(s.set_stats(h));
  h.pad_bus.scanner.count += 1u;
  h.pad_bus.leds.count += 1u;
  CHECK_FALSE(s.set_stats(h));  // stopped counting on screen: static again
}

TEST_CASE("input test: the pad bus line, with the pin levels, fits its row on every theme in the worst case (OS-BUG-PADS)") {
  hal::PadBusStats bus;
  bus.scanner = {2, 2, 4294967295u};  // every name is three letters, so the choice of results does not change the width
  bus.leds = {1, 3, 4294967295u};
  bus.clear = hal::PadBusStats::kClearHeld;
  bus.lines_first = hal::PadBusStats::kLineScl | hal::PadBusStats::kLineSda;
  bus.lines_now = hal::PadBusStats::kLineScl | hal::PadBusStats::kLineSda | hal::PadBusStats::kLineInt;
  app::InputTestBusLine line;
  app::input_test_bus_line(bus, line);
  CHECK(std::string(line.c_str()) == "BUS 34 NAK/NAK X999+  74 TMO/ARB X999+  CLR HELD  SCL SDA INT 110>111");
  CHECK(line.size() == 69u);
  CHECK(line.size() < line.capacity());  // not cut
  // The screen draws it from the left margin (3 gaps) on a row of its own: the line plus both margins fits the screen, in every theme.
  for (unsigned i = 0; i < ui::kThemeCount; ++i) {
    const ui::Theme& t = ui::theme(ui::theme_id_at(i));
    const int margin = 3 * t.metrics.gap;
    const int width = static_cast<int>(line.size()) * t.geometry(ui::Face::Label).cell_w;
    CHECK_MESSAGE(margin + width + margin <= ui::kScreenW, t.name << ": " << width << " px of " << ui::kScreenW - 2 * margin);
  }
  // The levels are on the line whatever else it holds, and read "---" when the pins were not read.
  bus = {};
  app::input_test_bus_line(bus, line);
  CHECK(std::string(line.c_str()) == "BUS 34 --/-- X0  74 --/-- X0  CLR --  SCL SDA INT --->---");
}

TEST_CASE("input test: the pad bus line shows only when a pad chip was tried and refused, or the pad scanner is offline (OS-BUG-PADS)") {
  app::InputTestState s;
  hal::InputStats h;
  h.online = hal::InputStats::kPadsOnline | hal::InputStats::kButtonsOnline;
  CHECK_FALSE(s.pad_bus_trouble());  // nothing tried
  h.pad_bus.scanner = {0, 0, 5};
  h.pad_bus.leds = {0, 0, 40};
  CHECK(s.set_stats(h));
  CHECK_FALSE(s.pad_bus_trouble());  // answered from the first transaction
  h.pad_bus.scanner.first = 2;       // NAK first, an ack since: shown, first and last both on the line
  CHECK(s.set_stats(h));
  CHECK(s.pad_bus_trouble());
  h.pad_bus.scanner.first = 0;
  h.online = hal::InputStats::kButtonsOnline;  // the pad scanner is offline although it once acked
  CHECK(s.set_stats(h));
  CHECK(s.pad_bus_trouble());
  h.pad_bus.scanner = {};  // offline and not tried yet (the gate is closed): nothing to say
  h.pad_bus.leds = {};
  CHECK(s.set_stats(h));
  CHECK_FALSE(s.pad_bus_trouble());
}

// ---- the pad LED palette -----------------------------------------------------------------------------------------------------

TEST_CASE("pad palette: front is brighter than back in every theme, both inside the palette's limits, from the theme's own tokens") {
  for (unsigned i = 0; i < ui::kThemeCount; ++i) {
    const ui::Theme& t = ui::theme(ui::theme_id_at(i));
    const unsigned front = app::pad_level(t, app::PadRole::Front);
    const unsigned back = app::pad_level(t, app::PadRole::Back);
    CHECK_MESSAGE(app::pad_level(t, app::PadRole::Off) == 0, t.name);
    CHECK_MESSAGE(back < front, t.name);
    CHECK_MESSAGE((front >= app::kPadFrontMin && front <= app::kPadFrontMax), t.name << " front " << front);
    CHECK_MESSAGE((back >= app::kPadBackMin && back <= app::kPadBackMax), t.name << " back " << back);
    // The levels are what the tokens give: the luminance of Accent and of Sel, spread over the bounds.
    CHECK(front == app::spread(app::token_luma(t.rgb(ui::Color::Accent)), app::kPadFrontMin, app::kPadFrontMax));
    CHECK(back == app::spread(app::token_luma(t.rgb(ui::Color::Sel)), app::kPadBackMin, app::kPadBackMax));
  }
  // Never brighter than this OS's limits: the PWM the driver writes for the front level is 255, and for the brightest
  // back level it is no more than 69, so a back pad stays well below a front one.
  CHECK(mk66in::pad_pwm_of_level(app::kPadFrontMax) == 255);
  CHECK(mk66in::pad_pwm_of_level(app::kPadBackMax) <= 69);
}

TEST_CASE("pad palette: the luminance function is monotonic and bounded") {
  CHECK(app::token_luma(ui::Rgb{0, 0, 0}) == 0u);
  CHECK(app::token_luma(ui::Rgb{255, 255, 255}) == 255u);
  CHECK(app::token_luma(ui::Rgb{255, 0, 0}) < app::token_luma(ui::Rgb{0, 255, 0}));  // green carries most of the brightness
  CHECK(app::spread(0, 40, 132) == 40);
  CHECK(app::spread(255, 40, 132) == 132);
}

// ---- the platform and the app agree on the panel ----------------------------------------------------------------------------

TEST_CASE("input test: the panel's counts and names agree between the platform's key map and the app") {
  CHECK(mk66in::kButtonCount == app::kButtonCount);
  CHECK(mk66in::kPadCount == app::kInputTestPads);
  CHECK(mk66in::kPadCount == hal::kPadLedCount);
  // Matrix key -> button code (platform) -> the name the INPUT TEST cell shows (app), for the corners of the control matrix.
  CHECK(std::strcmp(app::input_test_button_name(mk66in::button_of_key(77)), "F1") == 0);
  CHECK(std::strcmp(app::input_test_button_name(mk66in::button_of_key(70)), "F8") == 0);
  CHECK(std::strcmp(app::input_test_button_name(mk66in::button_of_key(0)), "PARAM") == 0);   // Params
  CHECK(std::strcmp(app::input_test_button_name(mk66in::button_of_key(44)), "RIGHT") == 0);
  CHECK(mk66in::button_of_key(mk66in::key_of_button(static_cast<unsigned>(Button::Shift))) == static_cast<unsigned>(Button::Shift));
  CHECK(std::strcmp(app::input_test_button_name(static_cast<unsigned>(Button::Shift)), "SHIFT") == 0);
  CHECK(std::strcmp(app::input_test_button_name(static_cast<unsigned>(Button::Play)), "PLAY") == 0);
  CHECK(std::strcmp(app::input_test_button_name(static_cast<unsigned>(Button::Left)), "LEFT") == 0);
  CHECK(std::strcmp(app::input_test_button_name(33), "?") == 0);
  // Every cell's short name begins with the letter of the button's own name (buttons.h kButtonNames, the simulator's words).
  for (unsigned b = 0; b < app::kButtonCount; ++b) {
    const char* shown = app::input_test_button_name(b);
    const char* own = app::kButtonNames[b].name;
    CHECK_MESSAGE((std::strlen(shown) >= 1u && std::strlen(shown) <= 5u), own);
    const char upper = static_cast<char>(own[0] >= 'a' && own[0] <= 'z' ? own[0] - ('a' - 'A') : own[0]);
    CHECK_MESSAGE(shown[0] == upper, own << " is shown as " << shown);
    CHECK(static_cast<unsigned>(app::kButtonNames[b].button) == b);
  }
}

// ---- the hidden screen in the app --------------------------------------------------------------------------------------------

namespace {

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);

  Rig() { app->run_once(); }
  void pass(unsigned ms = 1) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  void press(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true)); }
  void release(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false)); }
  void open() { app->invoke(core::action_id("nav.screen"), static_cast<core::ActionArg>(ScreenId::InputTest)); }
  ScreenId screen() const { return app->state().screen; }
  const app::InputTestState& test() const { return app->state().input_test; }
};

}  // namespace

TEST_CASE("input test screen: it is in the registry, hidden, and the Navigator does not list it") {
  const app::ScreenTable all = app::screen_registry();
  const app::ScreenEntry* e = app::find_screen(all, ScreenId::InputTest);
  CHECK(e != nullptr);
  if (e == nullptr) {
    return;  // this tree builds without exceptions: doctest's REQUIRE is unavailable
  }
  CHECK(e->hidden);
  CHECK(e->screen == &app::kInputTestScreen);
  CHECK(e->screen->captures());
  CHECK(e->screen->context() == app::Context::InputTest);
  const app::ScreenTable visible = app::navigator_screens();
  CHECK(visible.count == all.count - 2);  // OS-012b: the app page is hidden too
  CHECK(app::find_screen(visible, ScreenId::InputTest) == nullptr);
  CHECK(app::screens_valid(all.items, all.count));
}

TEST_CASE("input test screen: every button, pad and turn is seen raw and none of them acts") {
  Rig rig;
  rig.open();
  CHECK(rig.screen() == ScreenId::InputTest);
  // A screen button that would leave any other screen, the arrows, a pad that would choose an instrument, Shift + Master (the theme):
  for (const Button b : {Button::Song, Button::File, Button::Pattern, Button::Master, Button::Play, Button::Left}) {
    rig.press(b);
    rig.pass();
    rig.release(b);
    rig.pass();
    CHECK(rig.screen() == ScreenId::InputTest);
  }
  const ui::ThemeId theme = rig.app->theme();
  rig.press(Button::Shift);
  rig.pass();
  rig.press(Button::Master);
  rig.pass(500);
  rig.release(Button::Master);
  rig.release(Button::Shift);
  rig.pass();
  CHECK(rig.app->theme() == theme);  // Shift + Master did not cycle it
  rig.input.push(core::make_pad_event(rig.clock.now(), 17, 127));
  rig.input.push(core::make_encoder_event(rig.clock.now(), 0, -3));
  rig.pass();
  CHECK(rig.app->instrument() == 1);  // the pad did not select an instrument
  const app::InputTestState& t = rig.test();
  CHECK(t.button_presses[static_cast<unsigned>(Button::Song)] == 1);
  CHECK(t.button_presses[static_cast<unsigned>(Button::File)] == 1);
  CHECK(t.button_presses[static_cast<unsigned>(Button::Master)] == 2);  // once alone, once with Shift
  CHECK(t.button_presses[static_cast<unsigned>(Button::Shift)] == 1);
  CHECK(((t.pads_down >> 17) & 1u) == 1u);
  CHECK(t.encoder_total == -3);
  CHECK(rig.screen() == ScreenId::InputTest);
}

TEST_CASE("input test screen: F4 + F5 together leave for the pattern screen, F2 + F3 together clear the counts") {
  Rig rig;
  rig.open();
  rig.input.push(core::make_pad_event(rig.clock.now(), 2, 127));
  rig.input.push(core::make_pad_event(rig.clock.now(), 2, 0));
  rig.pass();
  CHECK(rig.test().pad_presses[2] == 1);
  rig.press(Button::F2);
  rig.press(Button::F3);
  rig.pass();
  rig.pass(100);
  CHECK(rig.test().pad_presses[2] == 0);  // cleared
  CHECK(rig.screen() == ScreenId::InputTest);
  rig.release(Button::F2);
  rig.release(Button::F3);
  rig.pass();
  // F4 and F5 alone are seen and do nothing; together they leave
  rig.press(Button::F4);
  rig.pass(200);
  rig.release(Button::F4);
  rig.pass();
  CHECK(rig.screen() == ScreenId::InputTest);
  rig.press(Button::F4);
  rig.press(Button::F5);
  rig.pass();
  rig.pass(100);
  CHECK(rig.screen() == ScreenId::Pattern);
  // and the pattern screen's own bindings work again
  rig.release(Button::F4);
  rig.release(Button::F5);
  rig.pass();
  rig.press(Button::Song);
  rig.pass();
  rig.release(Button::Song);
  rig.pass();
  CHECK(rig.screen() == ScreenId::Song);
}

TEST_CASE("input test screen: the boot chord's own buttons (Shift, F1, F8) do not leave it") {
  Rig rig;
  rig.open();
  rig.press(Button::Shift);
  rig.press(Button::F1);
  rig.press(Button::F8);
  rig.pass();
  rig.pass(500);
  CHECK(rig.screen() == ScreenId::InputTest);
  CHECK(rig.test().button_presses[static_cast<unsigned>(Button::F1)] == 1);
  CHECK(rig.test().button_presses[static_cast<unsigned>(Button::F8)] == 1);
}

TEST_CASE("input test screen: entering it starts from a clean picture, and it takes the input path's health counters") {
  Rig rig;
  rig.open();
  rig.input.push(core::make_pad_event(rig.clock.now(), 9, 127));
  rig.pass();
  CHECK(rig.test().pad_presses[9] == 1);
  app::ViewState view = rig.app->state();
  app::kInputTestScreen.enter(rig.app->model(), view);
  CHECK(view.input_test.pad_presses[9] == 0);
}

namespace {

// An input source with no events and health counters the test sets (the pad scanner's, as the unit reports them every pass).
class StatsInput final : public hal::Input {
 public:
  bool poll(core::InputEvent&) noexcept override { return false; }
  hal::InputStats stats() const noexcept override { return health; }
  hal::InputStats health;
};

}  // namespace

TEST_CASE("input test screen: an idle healthy screen is static while the pad bus record moves, a bus in trouble redraws on a drawn change (OS-BUG-PADS)") {
  StatsInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  input.health.online = hal::InputStats::kPadsOnline | hal::InputStats::kButtonsOnline;
  input.health.pad_bus.scanner = {0, 0, 1};
  input.health.pad_bus.leds = {0, 0, 1};
  const auto app = std::make_unique<app::App>(input, clock, display);
  const auto pass = [&](unsigned ms) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  };
  app->run_once();
  app->invoke(core::action_id("nav.screen"), static_cast<core::ActionArg>(ScreenId::InputTest));
  for (int i = 0; i < 5; ++i) {
    pass(10);
  }
  CHECK_FALSE(app->dirty());
  const std::uint32_t frames = app->frames();
  for (unsigned i = 0; i < 60; ++i) {  // 600 ms of polls, each one more attempt and other pins: nothing drawn changes
    input.health.pad_bus.scanner.count += 1;
    input.health.pad_bus.leds.count += 1;
    input.health.pad_bus.lines_now = static_cast<std::uint8_t>(i & 7u);
    pass(10);
    CHECK_FALSE(app->dirty());
  }
  CHECK(app->frames() == frames);
  input.health.pad_bus.scanner.first = 2;  // the first transaction was refused: the bus line appears
  pass(10);
  CHECK(app->frames() == frames + 1u);
  input.health.pad_bus.lines_now = static_cast<std::uint8_t>(input.health.pad_bus.lines_now ^ 1u);  // a pin level is on the line
  pass(10);
  CHECK(app->frames() == frames + 2u);
  pass(10);  // and nothing changed since
  CHECK(app->frames() == frames + 2u);
}

// ---- the scan and the app together: the order of two keys reported in one drain ------------------------------------------------

TEST_CASE("input: Shift then F8, read from the chip in one drain, is Shift + F8 (redo), not F8 (undo)") {
  namespace fake = mk66in::fake;
  fake::ManualClock clock;
  fake::Bus pad_bus;
  fake::Bus control_bus;
  fake::KeyChip pad_chip;
  fake::KeyChip button_chip;
  fake::Hardware hardware(pad_chip, button_chip);
  pad_bus.attach(0x34, pad_chip);
  control_bus.attach(0x34, button_chip);
  mk66in::Scanner scanner(pad_bus, control_bus, hardware, clock);
  platform::host::FakeDisplay display;
  auto app = std::make_unique<app::App>(scanner, clock, display);
  scanner.begin();
  const auto pass = [&](unsigned ms) {
    clock.advance_ms(ms);
    scanner.service();
    app->run_once();
  };
  pass(2);
  // F8 is button 7, Shift button 29: the lower number is the later key. Both are in the chip's FIFO when the scan reads it.
  button_chip.press(mk66in::key_of_button(static_cast<unsigned>(Button::Shift)));
  button_chip.press(mk66in::key_of_button(static_cast<unsigned>(Button::F8)));
  pass(1);
  pass(10);
  button_chip.release(mk66in::key_of_button(static_cast<unsigned>(Button::F8)));
  pass(10);
  button_chip.release(mk66in::key_of_button(static_cast<unsigned>(Button::Shift)));
  pass(10);
  pass(400);
  // No document is attached, so the key says what it would have done: F8 alone is UNDO, Shift + F8 is REDO.
  CHECK(std::string(app->message()) == "NOTHING TO REDO HERE");
}
