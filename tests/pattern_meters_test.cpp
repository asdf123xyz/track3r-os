// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-PVU: the Pattern screen's track-header meters are the engine's (the mixer board's ballistics), not the simulator's demo constants. A level the board holds shows in the header of
// a visible track; the app asks for a frame only when a visible track's bar or tick moved, so a steady or silent signal costs none and a track that is not on the screen costs none.
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/project_fixture.h"

#include <o3os/app/app.h>
#include <o3os/app/mix_board.h>
#include <o3os/app/pattern_source.h>
#include <o3os/audio/kernels.h>
#include <o3os/audio/meters.h>
#include <o3os/model/editor.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/sim/demo_project.h>

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <utility>

using namespace o3os;

namespace {

alignas(8) std::uint8_t g_ring[64 * 1024];

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  std::unique_ptr<app::MixBoard> board = std::make_unique<app::MixBoard>();
  audio::MeterStore store;
  audio::MeterBank bank{store};

  Rig() {
    model::Project& p = test::arena(1).project;
    sim::fill_demo_project(p);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    board->meters = &bank;
    app->attach_mix(board.get());
    settle();
  }
  void pass(unsigned ms = 16) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  // Passes until nothing asks for a frame (the first frames, the first look at the meters).
  void settle() {
    for (unsigned i = 0; i < 8; ++i) {
      pass();
    }
  }
  // One meter window in which each listed track peaked at the given value (of 32768) with the RMS of a signal as loud, and every other track was silent.
  using Level = std::pair<unsigned, std::int32_t>;
  void window(std::initializer_list<Level> levels) {
    const std::uint64_t frames = audio::kMeterWindowBlocks * hal::audio::kBlock;
    for (unsigned b = 0; b < audio::kMeterWindowBlocks; ++b) {
      if (b + 1u == audio::kMeterWindowBlocks) {
        for (const Level& l : levels) {
          const std::uint64_t rms = static_cast<std::uint64_t>(l.second) * static_cast<std::uint64_t>(l.second);
          bank.acc(l.first) = audio::kernel::MeterAcc{static_cast<std::uint32_t>(l.second), frames * rms};
        }
      }
      bank.end_block();
    }
  }
  app::PatternSource source() const { return app::PatternSource(app->model(), 1); }
  std::uint32_t frames() const { return app->frames(); }
};

}  // namespace

TEST_CASE("pattern meters: a level on the mixer board shows in the header of a visible track") {
  Rig rig;
  NEED(rig.app->state().screen == app::ScreenId::Pattern);
  NEED(rig.app->state().pattern.left_track() == 0);
  CHECK(rig.source().meter_level(3) == 0);  // silent: flat
  CHECK(rig.source().meter_peak(3) == 0);

  rig.window({{3, 16384}});  // a half-scale peak: -6 dB, on a 0 to -60 dB scale that is 90 % of the bar
  const std::uint32_t before = rig.frames();
  rig.pass();
  CHECK(rig.frames() == before + 1);  // the bar moved: one frame
  const unsigned level = rig.source().meter_level(3);
  const unsigned peak = rig.source().meter_peak(3);
  CHECK(level > 150);
  CHECK(level <= rig.board->scale.position(1.0f) / 256u);
  CHECK(peak >= level);  // the tick is never below the bar
  CHECK(rig.source().meter_level(2) == 0);  // another track: still flat
  CHECK(rig.source().meter_level(40) == 0);  // a track the board has not got
  CHECK(rig.source().meter_level(-1) == 0);
  // and it is the board's value: the same number as the Mixer page's bar on the dB scale
  const unsigned pos = rig.board->scale.position(rig.board->meter[3].rms());
  CHECK(level == (pos >> 8));
}

TEST_CASE("pattern meters: an unchanged meter makes no frame, a track that is not on the screen makes none, a change does") {
  Rig rig;
  rig.window({{3, 16384}});
  rig.pass();
  // The same loud window again: the RMS bar climbs towards it for a few windows (the ballistics), so the frames follow it, and then it
  // stands still. Once it does, polling again makes no frame.
  for (unsigned i = 0; i < 80; ++i) {
    rig.window({{3, 16384}});
    rig.pass();
  }
  const std::uint32_t steady = rig.frames();
  rig.pass();
  rig.pass();
  CHECK(rig.frames() == steady);  // no new window: nothing moved

  // Track 9 is not one of the five on the screen (tracks 1-5): its meter moves, the header does not, no frame.
  rig.window({{3, 16384}, {9, 16384}});
  rig.pass();
  for (unsigned i = 0; i < 80; ++i) {  // (track 9's own ballistics settle, whatever the screen does)
    rig.window({{3, 16384}, {9, 16384}});
    rig.pass();
  }
  const std::uint32_t before_hidden = rig.frames();
  rig.window({{3, 16384}, {9, 8192}});
  rig.pass();
  CHECK(rig.board->meter[9].rms() > 0.0f);
  CHECK(rig.frames() == before_hidden);

  // A visible track going quiet: its bar falls, which is a frame.
  rig.window({{9, 8192}});
  rig.pass();
  CHECK(rig.frames() > before_hidden);
}

TEST_CASE("pattern meters: a project without a board shows flat headers and the demo alone keeps its fixed levels") {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  app::App a(input, clock, display);
  model::Project& p = test::arena(1).project;
  sim::fill_demo_project(p);
  model::UndoJournal journal;
  journal.begin(g_ring, sizeof g_ring);
  model::Editor editor(p, journal);
  a.attach_document(&editor);
  for (unsigned i = 0; i < 4; ++i) {
    clock.advance(16 * core::kTicksPerMs);
    a.run_once();
  }
  const app::PatternSource doc(a.model(), 1);
  CHECK_FALSE(doc.is_demo());
  for (int t = 0; t < 12; ++t) {
    CHECK(doc.meter_level(t) == 0);
    CHECK(doc.meter_peak(t) == 0);
  }
  app::PatternData demo;
  app::fill_demo(demo);
  const app::PatternSource d(demo);
  CHECK(d.is_demo());
  CHECK(d.meter_level(0) == app::track_level(0));
  CHECK(d.meter_peak(3) == app::track_peak(3));
}
