// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-AUDIT-40 / OS-AUDIT-26 (E-LOAD-11, E-LOAD-24, E-GLOB-07; new Community OS behaviour): a pad auditions an instrument on the screens that give it no other job. Sample load: the pad chooses
// the target instrument (pad n = instrument n + 1) and plays it; Mixer, Config and File: the pad plays the current instrument. The sound is the Pattern pads' path (PreviewSink::pad_down,
// ended by pad_up on the pad's release, whatever screen is in view then). Song and Pattern keep their own pad job (pad_slice_app_test.cpp, song and pattern tests).
#include <doctest/doctest.h>

#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/pad_board.h>
#include <o3os/model/editor.h>
#include <o3os/platform/host/fakes.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "support/need.h"
#include "support/project_fixture.h"

using namespace o3os;
using app::ScreenId;

namespace {

alignas(8) std::uint8_t g_ring[model::kUndoRegionBytes];

// What the pads asked of the voices.
struct PadLog final : app::PreviewSink {
  struct Down {
    unsigned pad, track, instrument, note;
  };
  std::vector<Down> downs;
  std::vector<unsigned> ups;
  void preview(unsigned, unsigned) noexcept override {}
  void pad_down(unsigned pad, unsigned track, unsigned instrument, unsigned note) noexcept override { downs.push_back({pad, track, instrument, note}); }
  void pad_up(unsigned pad) noexcept override { ups.push_back(pad); }
};

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  PadLog log;

  Rig() {
    model::Project& p = test::arena(0).project;
    p.reset();
    (void)p.create_pattern(1, 16);
    (void)p.append_order(1);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    app->attach_preview(&log);
    app->run_once();
  }
  void pass(unsigned ms = 1) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  void go(ScreenId id) {
    (void)app->invoke(core::action_id("nav.screen"), static_cast<core::ActionArg>(id));
    pass();
  }
  void pad_down(unsigned pad) {
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(pad), 100));
    pass();
  }
  void pad_up(unsigned pad) {
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(pad), 0));
    pass();
  }
  void tap_pad(unsigned pad) {
    pad_down(pad);
    pad_up(pad);
  }
};

}  // namespace

TEST_CASE("pad audition: Mixer, Config and File play the current instrument, and the release ends the note") {
  const app::PadBoard board;
  for (const ScreenId screen : {ScreenId::Mixer, ScreenId::Config, ScreenId::File}) {
    Rig rig;
    rig.go(screen);
    NEED(rig.app->state().screen == screen);
    const int inst = rig.app->state().instrument;
    rig.pad_down(5);
    NEED(rig.log.downs.size() == 1);
    CHECK(rig.log.downs[0].pad == 5);
    CHECK(rig.log.downs[0].instrument == static_cast<unsigned>(inst));
    CHECK(rig.log.downs[0].note == static_cast<unsigned>(board.note(5)));
    CHECK(rig.log.ups.empty());  // held until the release
    rig.pad_up(5);
    NEED(rig.log.ups.size() == 1);
    CHECK(rig.log.ups[0] == 5);
    CHECK(rig.app->state().instrument == inst);  // these screens do not choose an instrument
  }
}

TEST_CASE("pad audition: Sample load chooses the target instrument and plays it") {
  const app::PadBoard board;
  Rig rig;
  rig.go(ScreenId::SampleLoad);
  rig.pad_down(6);
  CHECK(rig.app->state().instrument == 7);
  NEED(rig.log.downs.size() == 1);
  CHECK(rig.log.downs[0].instrument == 7);
  CHECK(rig.log.downs[0].note == static_cast<unsigned>(board.note(6)));
  rig.pad_up(6);
  NEED(rig.log.ups.size() == 1);
  CHECK(rig.log.ups[0] == 6);
  rig.tap_pad(0);  // pad 0: instrument 1
  CHECK(rig.app->state().instrument == 1);
  CHECK(rig.log.downs.size() == 2);
  CHECK(rig.log.downs[1].instrument == 1);
}

TEST_CASE("pad audition: a pad held across a screen change still ends, none sticks") {
  Rig rig;
  rig.go(ScreenId::Mixer);
  rig.pad_down(3);
  NEED(rig.log.downs.size() == 1);
  rig.go(ScreenId::Config);
  rig.pad_down(4);
  NEED(rig.log.downs.size() == 2);
  rig.go(ScreenId::Pattern);
  rig.pad_up(3);
  rig.pad_up(4);
  NEED(rig.log.ups.size() == 2);
  CHECK(rig.log.ups[0] == 3);
  CHECK(rig.log.ups[1] == 4);
  CHECK(rig.log.downs.size() == 2);  // the release on Pattern started nothing
}

TEST_CASE("pad audition: the screens with another pad job do not audition") {
  Rig rig;
  rig.go(ScreenId::Song);
  rig.tap_pad(2);
  CHECK(rig.log.downs.empty());
  rig.go(ScreenId::Pattern);
  rig.tap_pad(2);  // Pattern / Browse keeps its own preview (pad_preview), one note
  CHECK(rig.log.downs.size() == 1);
}
