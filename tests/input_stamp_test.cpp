// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-STAMPCLAMP: the app clamps every input event's stamp to the clock before the INPUT TEST screen, the module app and the gesture
// recogniser see it (App::run_once). A source that stamps ahead of the clock (a future USB or MIDI path) would otherwise hold the
// recogniser's time back until the clock caught up: no hold, no auto-repeat, no chord window for as long as the stamp is ahead.
// The tests run one script with every stamp `ahead` microseconds in front of the clock and demand the very trace that the same
// script gives with exact stamps; the ahead values are 1 ms, 2^30 us and a stamp that crosses the wrap of the 32-bit tick.
#include <doctest/doctest.h>

#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/app/view_state.h>
#include <o3os/core/gesture.h>
#include <o3os/core/input_event.h>
#include <o3os/model/editor.h>
#include <o3os/platform/host/fakes.h>

#include "support/project_fixture.h"

#include <cstdint>
#include <memory>
#include <vector>

using namespace o3os;
using app::Button;
using app::OverlayId;

namespace {

alignas(8) std::uint8_t g_stamp_ring[model::kUndoRegionBytes];

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  std::uint32_t ahead = 0;  // every event is stamped this many us in front of the clock (wrapping like the tick)

  Rig(core::Tick start, std::uint32_t ahead_us) : ahead(ahead_us) {
    clock.set(start);
    model::Project& p = test::arena(0).project;
    p.reset();
    (void)p.create_pattern(1, 64);
    (void)p.append_order(1);
    journal.begin(g_stamp_ring, sizeof g_stamp_ring);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    app->run_once();
  }
  // The event reaches the app in the very pass that stamps it: with exact stamps (ahead 0) the stamp is the clock.
  void send(Button b, bool pressed) {
    input.push(core::make_button_event(static_cast<core::Tick>(clock.now() + ahead), static_cast<std::uint8_t>(b), pressed));
    app->run_once();
  }
  void wait(unsigned ms) {
    while (ms > 0) {
      const unsigned step = ms < 5 ? ms : 5;
      clock.advance(step * core::kTicksPerMs);
      app->run_once();
      ms -= step;
    }
  }
  struct Sample {
    OverlayId overlay;
    int row;
    bool operator==(const Sample& o) const { return overlay == o.overlay && row == o.row; }
  };
  Sample look() const { return Sample{app->state().overlay, app->view().row()}; }
  // One sample per 5 ms.
  void trace_for(unsigned ms, std::vector<Sample>& out) {
    for (unsigned t = 0; t < ms; t += 5) {
      wait(5);
      out.push_back(look());
    }
  }
};

// The script: F1 held 600 ms (the pattern chooser opens at hold_ms and closes at the release), Down held 1500 ms (a tap at the press,
// then the auto-repeats), Vol and Fx 20 ms apart (a chord inside the 40 ms window opens Learn), Vol alone then Fx 100 ms later (no chord).
std::vector<Rig::Sample> run_script(core::Tick start, std::uint32_t ahead) {
  Rig rig(start, ahead);
  std::vector<Rig::Sample> trace;
  rig.send(Button::F1, true);
  rig.trace_for(600, trace);
  rig.send(Button::F1, false);
  rig.trace_for(50, trace);

  rig.send(Button::Down, true);
  rig.trace_for(1500, trace);
  rig.send(Button::Down, false);
  rig.trace_for(50, trace);

  rig.send(Button::Vol, true);
  rig.trace_for(20, trace);
  rig.send(Button::Fx, true);
  rig.trace_for(100, trace);
  rig.send(Button::Fx, false);
  rig.send(Button::Vol, false);
  rig.trace_for(100, trace);

  rig.send(Button::Vol, true);
  rig.trace_for(100, trace);
  rig.send(Button::Fx, true);
  rig.trace_for(100, trace);
  rig.send(Button::Fx, false);
  rig.send(Button::Vol, false);
  rig.trace_for(100, trace);
  return trace;
}

constexpr core::Tick kStarts[] = {5000u * 1000u, 0xFFFFFFFFu - 2000u * 1000u, 0xFFFFFFFFu - 600u};  // mid-run, 2 s before a wrap, 600 us before one

}  // namespace

TEST_CASE("input stamp: the script has the effects that the clamp has to keep (a hold, repeats, a chord)") {
  const std::vector<Rig::Sample> trace = run_script(kStarts[0], 0);
  bool chooser = false;
  bool learn = false;
  int last_row = trace.front().row;
  for (const Rig::Sample& s : trace) {
    chooser = chooser || s.overlay == OverlayId::PatternChooser;
    learn = learn || s.overlay == OverlayId::Learn;
    last_row = s.row;
  }
  CHECK(chooser);                       // hold_ms 400 fired
  CHECK(learn);                         // the 40 ms chord window
  CHECK(last_row - trace.front().row >= 5);  // the tap and the auto-repeats of Down
}

TEST_CASE("input stamp: stamps ahead of the clock give the same hold, repeat and chord as exact stamps") {
  const std::uint32_t aheads[] = {1000u, 0x40000000u, 0x7FFFFFFFu};
  for (const core::Tick start : kStarts) {
    const std::vector<Rig::Sample> exact = run_script(start, 0);
    for (const std::uint32_t ahead : aheads) {
      INFO("start " << start << " ahead " << ahead);
      const std::vector<Rig::Sample> trace = run_script(start, ahead);
      CHECK(trace.size() == exact.size());
      bool same = trace.size() == exact.size();
      for (std::size_t i = 0; same && i < trace.size(); ++i) {
        same = trace[i] == exact[i];
        CHECK_MESSAGE(same, "first difference at sample " << i);
      }
    }
  }
}
