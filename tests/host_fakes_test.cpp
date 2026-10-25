// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <o3os/hal/clock.h>
#include <o3os/hal/display.h>
#include <o3os/hal/input.h>
#include <o3os/platform/host/fakes.h>

#include <cstdint>

using namespace o3os;
using namespace o3os::platform::host;

TEST_CASE("fake clock: time moves only when told, and wraps like a real tick") {
  FakeClock clock;
  const hal::Clock& hal_clock = clock;  // consumers see the contract only
  CHECK(hal_clock.now() == 0u);
  clock.advance(1500);
  CHECK(hal_clock.now() == 1500u);
  clock.set(0xFFFFFFF0u);
  const core::Tick before = hal_clock.now();
  clock.advance(0x20);
  CHECK(hal_clock.now() == 0x10u);
  CHECK(core::tick_elapsed(hal_clock.now(), before) == 0x20u);
}

TEST_CASE("fake clock: uptime_ms keeps counting where the 32-bit tick has wrapped") {
  FakeClock clock;
  const hal::Clock& hal_clock = clock;
  CHECK(hal_clock.uptime_ms() == 0u);
  clock.advance(1500);  // 1.5 ms
  CHECK(hal_clock.uptime_ms() == 1u);

  clock.set(0);
  clock.advance(2000000000u);
  clock.advance(2000000000u);
  const core::Tick before_wrap = hal_clock.now();  // 4.0e9 us: not yet wrapped
  CHECK(hal_clock.uptime_ms() == 4000000u);
  clock.advance(2000000000u);  // 6.0e9 us in total: past 2^32 (about 4295 s)
  CHECK(hal_clock.now() == 1705032704u);  // 6e9 - 2^32: the tick wrapped...
  CHECK(hal_clock.now() < before_wrap);
  CHECK(hal_clock.uptime_ms() == 6000000u);  // ...the 64-bit uptime did not
}

TEST_CASE("fake input: replays events oldest first and then reports empty") {
  FakeInput input;
  hal::Input& hal_input = input;
  core::InputEvent out{};
  CHECK_FALSE(hal_input.poll(out));

  input.push(core::make_button_event(1, 5, true));
  input.push(core::make_encoder_event(2, 0, -2));
  CHECK(input.pending() == 2u);
  CHECK(hal_input.poll(out));
  CHECK(out == core::make_button_event(1, 5, true));
  CHECK(hal_input.poll(out));
  CHECK(out == core::make_encoder_event(2, 0, -2));
  CHECK_FALSE(hal_input.poll(out));
  CHECK(out == core::make_encoder_event(2, 0, -2));  // untouched on failure
}

TEST_CASE("fake display: frames are committed on end_frame and replace each other") {
  FakeDisplay display;
  hal::Display& sink = display;
  const std::uint32_t a[] = {1, 2, 3};
  const std::uint32_t b[] = {9};

  CHECK(sink.begin_frame());
  CHECK(sink.write(a, 2));
  CHECK(sink.write(a + 2, 1));  // successive writes concatenate
  CHECK(display.last_frame().empty());
  sink.end_frame();
  CHECK(display.last_frame() == std::vector<std::uint32_t>{1, 2, 3});

  CHECK(sink.begin_frame());
  CHECK(sink.write(b, 1));
  sink.end_frame();
  CHECK(display.last_frame() == std::vector<std::uint32_t>{9});
  CHECK(display.frames() == 2u);
}

TEST_CASE("fake display: contract edge cases") {
  FakeDisplay display(4);
  hal::Display& sink = display;
  const std::uint32_t w[] = {1, 2, 3, 4, 5};

  CHECK_FALSE(sink.write(w, 1));  // no frame open
  sink.end_frame();               // no frame open: ignored
  CHECK(display.frames() == 0u);

  CHECK(sink.begin_frame());
  CHECK(sink.write(w, 3));
  CHECK_FALSE(sink.write(w, 2));  // 3 + 2 > 4: refused whole
  CHECK(sink.write(w, 1));        // the room that is left still works
  sink.end_frame();
  CHECK(display.last_frame() == std::vector<std::uint32_t>{1, 2, 3, 1});

  display.set_busy(true);
  CHECK_FALSE(sink.begin_frame());
  CHECK_FALSE(display.in_frame());
  display.set_busy(false);

  CHECK(sink.begin_frame());
  CHECK(sink.write(w, 2));
  CHECK(sink.begin_frame());  // abandons the uncommitted frame
  sink.end_frame();
  CHECK(display.last_frame().empty());
}

TEST_CASE("fake display: abort_frame discards the frame and keeps the committed one") {
  FakeDisplay display;
  hal::Display& sink = display;
  const std::uint32_t good[] = {7, 8};
  const std::uint32_t bad[] = {1, 2, 3};

  sink.abort_frame();  // no frame open: ignored
  CHECK(display.aborted() == 0u);

  CHECK(sink.begin_frame());
  CHECK(sink.write(good, 2));
  sink.end_frame();

  CHECK(sink.begin_frame());
  CHECK(sink.write(bad, 3));
  sink.abort_frame();
  CHECK_FALSE(display.in_frame());
  CHECK(display.aborted() == 1u);
  CHECK(display.frames() == 1u);
  CHECK(display.last_frame() == std::vector<std::uint32_t>{7, 8});

  sink.end_frame();  // nothing open any more: must not commit the aborted words
  CHECK(display.frames() == 1u);
  CHECK(display.last_frame() == std::vector<std::uint32_t>{7, 8});
  CHECK(sink.begin_frame());  // and the sink is usable again
  CHECK(sink.write(good, 1));
  sink.end_frame();
  CHECK(display.last_frame() == std::vector<std::uint32_t>{7});
}
