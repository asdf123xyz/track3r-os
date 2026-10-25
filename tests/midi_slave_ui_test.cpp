// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-MIDISLV: with MIDI clock slave on, an external Start / Continue / Stop must drive the same UI state as the Play key: the PLAY badge (ViewState::playing, what the status strip's PLAY / STOP
// says and what the Play key toggles from), the tempo the strip shows (the master's, followed) and the playhead (the sequencer plays the pattern in view). The real chain, host only (it holds
// two models and a MidiLink, which the target's RAM does not): a simulated master stamps its clock, Start, Stop and Continue into the DIN ring, seq::MidiLink and the sequencer render block
// by block, and the App reads the sequencer's published position once every few blocks, as the main loop does. Before the fix the badge stayed STOP (only the Play key set it) and an external
// Start played pattern 0, which no project has, so there was no playhead on the pattern in view.
#include <doctest/doctest.h>

#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/view_state.h>
#include <o3os/audio/events.h>
#include <o3os/core/action.h>
#include <o3os/core/input_event.h>
#include <o3os/core/qtime.h>
#include <o3os/midi/config.h>
#include <o3os/midi/message.h>
#include <o3os/model/editor.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/seq/midi_link.h>

#include <cstdint>
#include <memory>

#include "support/need.h"
#include "support/project_fixture.h"
#include "support/seq_fixture.h"
#include "support/static_slot.h"

using namespace o3os;

namespace {

alignas(8) std::uint8_t g_ring[model::kUndoRegionBytes];
constexpr double kCycD = static_cast<double>(core::kRefCyclesPerSample.num) / static_cast<double>(core::kRefCyclesPerSample.den);
constexpr double kClockPeriod = 3750000.0;  // cycles: 24 ppqn at 120 BPM
constexpr double kByteStamp = 57600.0;      // a byte time after its start, as the UART interrupt stamps it
constexpr unsigned kPassEvery = 8;          // the main loop's pass is far slower than the render's block

// The Play key's sink as the platform's is (a command on the sequencer's ring), recording what the App told it.
class Sink final : public app::TransportSink {
 public:
  seq::Sequencer* s = nullptr;
  unsigned selects = 0;
  bool last_song = false;
  unsigned last_pattern = 0;
  bool play(unsigned pattern) noexcept override { return s->commands().push(seq::Command{seq::Command::Kind::PlayPattern, static_cast<std::uint8_t>(pattern), 0, 0}); }
  bool play_song(unsigned row) noexcept override { return s->commands().push(seq::Command{seq::Command::Kind::PlaySong, 0, static_cast<std::uint16_t>(row), 0}); }
  bool stop() noexcept override { return s->commands().push(seq::Command{seq::Command::Kind::Stop, 0, 0, 0}); }
  bool select(bool song, unsigned pattern) noexcept override {
    ++selects;
    last_song = song;
    last_pattern = pattern;
    return s->commands().push(seq::Command{seq::Command::Kind::Select, static_cast<std::uint8_t>(song ? 1u : 0u), static_cast<std::uint16_t>(pattern), 0});
  }
};

test::StaticSlot<seq::MidiLink> g_link_slot;

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  seq::Sequencer* s = nullptr;
  seq::MidiLink* link = nullptr;
  Sink sink;
  std::uint64_t sample = 0;
  std::uint32_t next_clock = 0;
  double t0 = 3.0e6;
  bool clocks = true;  // the master's clock runs
  struct Extra {
    double t;
    std::uint8_t byte;
  };
  Extra extra[8];
  unsigned extra_n = 0;
  unsigned blocks_run = 0;

  explicit Rig(std::uint32_t project_tempo_x100 = 10500) {
    model::Project& p = test::arena(0).project;
    p.reset();
    (void)p.create_pattern(1, 64);
    (void)p.create_pattern(2, 64);
    (void)p.append_order(1);
    (void)p.set_tempo_x100(project_tempo_x100);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    s = &test::seq_slot().make(p);
    seq::MidiLinkHooks hooks;
    link = &g_link_slot.make(*s, hooks);  // no heard_at: the counter reads the timeline's own cycles
    midi::MidiConfig cfg;
    cfg.mode = midi::SyncMode::Slave;
    cfg.in_port = midi::Port::Din;
    link->set_config(cfg);
    sink.s = s;
    app->attach_transport(&sink);
    app->attach_play_position(&s->play_position());
    app->run_once();
  }

  double now_cycles() const { return static_cast<double>(sample) * kCycD; }
  void master_sends(double at_cycles, std::uint8_t byte) { extra[extra_n++] = Extra{at_cycles, byte}; }
  // The master's byte `byte` `after` cycles from now.
  void master_in(double after, std::uint8_t byte) { master_sends(now_cycles() + after, byte); }

  void feed_master() {
    for (;;) {
      unsigned best = extra_n;
      for (unsigned i = 0; i < extra_n; ++i) {
        if (best == extra_n || extra[i].t < extra[best].t) best = i;
      }
      const double clock_t = t0 + static_cast<double>(next_clock) * kClockPeriod + kByteStamp;
      const bool clock_ready = clocks && clock_t < now_cycles();
      const bool extra_ready = best != extra_n && extra[best].t + kByteStamp < now_cycles();
      if (extra_ready && (!clock_ready || extra[best].t <= t0 + static_cast<double>(next_clock) * kClockPeriod)) {
        CHECK(link->din_in().push(midi::RtItem{static_cast<std::uint32_t>(static_cast<std::uint64_t>(extra[best].t + kByteStamp)), extra[best].byte, 0, 0}));
        extra[best] = extra[extra_n - 1u];
        --extra_n;
        continue;
      }
      if (!clock_ready) break;
      CHECK(link->din_in().push(midi::RtItem{static_cast<std::uint32_t>(static_cast<std::uint64_t>(clock_t)), midi::kClock, 0, 0}));
      ++next_clock;
    }
  }
  // `n` render blocks; the App's pass runs after every kPassEvery-th.
  void run(unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
      audio::SeqEvent out[audio::kMaxEventsPerBlock];
      feed_master();
      (void)link->step(sample, 64, out, audio::kMaxEventsPerBlock);
      sample += 64;
      if (++blocks_run % kPassEvery == 0u) {
        app->run_once();
        clock.advance(2000);
      }
    }
    app->run_once();
  }
  // Seconds of render: 44,100 / 64 blocks a second.
  void run_s(double seconds) { run(static_cast<unsigned>(seconds * 44100.0 / 64.0)); }

  const app::ViewState& view() const { return app->state(); }
  int bar() const { return app::play_row(view().play, view().edit.pattern); }
};

}  // namespace

TEST_CASE("midi slave UI: an external Start shows PLAY, the master's tempo and the playhead of the pattern in view; Stop shows STOP") {
  Rig rig;  // the project's tempo is 105.0, the master's 120.0
  rig.run_s(0.4);  // the loop locks on the clock; nothing plays, nothing is shown as playing
  CHECK_FALSE(rig.view().playing);
  CHECK_FALSE(rig.view().play.playing);
  CHECK(rig.view().edit.pattern == 1);
  rig.master_in(2.0e6, midi::kStart);
  rig.run_s(1.6);
  NEED(rig.s->playing());
  CHECK(rig.s->following());
  // the sequencer's word, as the App read it
  CHECK(rig.view().play.playing);
  CHECK(rig.view().play.pattern == 1);  // the pattern in view, not a pattern 0 that no project has
  CHECK(rig.view().play.tempo_x100 >= 11990u);
  CHECK(rig.view().play.tempo_x100 <= 12010u);  // the master's 120.0, not the project's 105.0
  // the UI state the Play key sets: the badge (what the strip draws and what the Play key toggles from) and the playhead
  CHECK(rig.view().playing);
  CHECK(rig.bar() >= 0);
  const int bar_a = rig.bar();
  rig.run_s(0.6);
  CHECK(rig.bar() != bar_a);  // it moves with the master's clock
  CHECK(rig.view().playing);
  // the master stops
  rig.master_in(1.0e5, midi::kStop);
  rig.run_s(0.3);
  CHECK_FALSE(rig.s->playing());
  CHECK_FALSE(rig.view().play.playing);
  CHECK_FALSE(rig.view().playing);
  CHECK(rig.bar() == -1);
  // and Continue plays on, with the badge back
  rig.master_in(1.0e5, midi::kContinue);
  rig.run_s(1.2);
  CHECK(rig.s->playing());
  CHECK(rig.view().playing);
  CHECK(rig.bar() >= 0);
}

TEST_CASE("midi slave UI: the Play key still toggles from what the sequencer does, whoever started it") {
  Rig rig;
  rig.run_s(0.4);
  rig.master_in(2.0e6, midi::kStart);
  rig.run_s(1.2);
  NEED(rig.view().playing);
  // the master started it; the Play key now stops it (the badge said PLAY): a Stop command, taken at the next block
  CHECK(rig.app->invoke(core::action_id("transport.play"), 0) == app::DispatchResult::Changed);
  CHECK_FALSE(rig.view().playing);  // at once, as for a local Play
  rig.run_s(0.2);
  CHECK_FALSE(rig.s->playing());
  CHECK_FALSE(rig.view().playing);
  // the Play key again, under the master's clock: it starts on the master's next clock and the badge is PLAY from the key to the run
  CHECK(rig.app->invoke(core::action_id("transport.play"), 0) == app::DispatchResult::Changed);
  CHECK(rig.view().playing);
  rig.run_s(0.4);
  CHECK(rig.s->playing());
  CHECK(rig.view().playing);
  // a local stop that the sequencer takes later does not bring the badge back
  CHECK(rig.app->invoke(core::action_id("transport.play"), 0) == app::DispatchResult::Changed);
  CHECK_FALSE(rig.view().playing);
  rig.run(kPassEvery);
  CHECK_FALSE(rig.view().playing);
  rig.run_s(0.2);
  CHECK_FALSE(rig.s->playing());
  CHECK_FALSE(rig.view().playing);
}

TEST_CASE("midi slave UI: the App tells the sequencer what an external Start plays: the pattern in view, or the song on the Song screen") {
  Rig rig;
  rig.run_s(0.2);
  CHECK(rig.sink.selects >= 1u);
  CHECK_FALSE(rig.sink.last_song);
  CHECK(rig.sink.last_pattern == 1u);
  const unsigned before = rig.sink.selects;
  rig.run_s(0.2);
  CHECK(rig.sink.selects == before);  // told once, not on every pass
  // another pattern in view while stopped: told again
  NEED(rig.app->open_pattern(2));
  rig.run_s(0.1);
  CHECK(rig.sink.selects == before + 1u);
  CHECK(rig.sink.last_pattern == 2u);
  // the Song screen: the Play key plays the song from the cursor's row, so an external Start plays the song
  rig.input.push(core::make_button_event(rig.clock.now(), static_cast<std::uint8_t>(app::Button::Song), true));
  rig.app->run_once();
  rig.input.push(core::make_button_event(rig.clock.now(), static_cast<std::uint8_t>(app::Button::Song), false));
  rig.app->run_once();
  NEED(rig.view().screen == app::ScreenId::Song);
  rig.run_s(0.1);
  CHECK(rig.sink.last_song);
  // and what an external Start then plays is the song
  rig.master_in(2.0e5, midi::kStart);
  rig.run_s(1.5);
  NEED(rig.s->playing());
  CHECK(rig.view().play.order != audio::PlayPosition::kNoOrder);
  CHECK(rig.view().playing);
}

// The OS-MIDISLV review: select() must not make a Song-mode transport play the pattern in view. A Start runs the song from its first position (MIDI Start: from the beginning); a Continue
// before anything has run starts the same way (there is nothing to resume); a Continue after a Stop resumes where it stopped.
TEST_CASE("midi slave UI: on the Song screen a Start runs the song from position 0, a cold Continue the same, a Continue after a Stop resumes") {
  for (const std::uint8_t first : {midi::kStart, midi::kContinue}) {
    Rig rig;
    (void)test::arena(0).project.append_order(2);  // the song is [1, 2]; the pattern in view is the second
    NEED(rig.app->open_pattern(2));
    rig.input.push(core::make_button_event(rig.clock.now(), static_cast<std::uint8_t>(app::Button::Song), true));
    rig.app->run_once();
    rig.input.push(core::make_button_event(rig.clock.now(), static_cast<std::uint8_t>(app::Button::Song), false));
    rig.app->run_once();
    NEED(rig.view().screen == app::ScreenId::Song);
    rig.run_s(0.4);
    CHECK(rig.sink.last_song);
    CHECK(rig.sink.last_pattern == 2u);  // told the pattern in view all the same: the sequencer must not take it in Song mode
    rig.master_in(2.0e6, first);
    rig.run_s(0.8);
    NEED(rig.s->playing());
    CHECK(rig.view().play.order == 0);    // the first position of the song,
    CHECK(rig.view().play.pattern == 1);  // whose pattern is the song's, not the one in view
    CHECK(rig.view().playing);
    rig.run_s(0.8);
    const unsigned step = rig.view().play.step;
    CHECK(step >= 4u);
    // Stop, then Continue: on from where it stopped, not from the beginning
    rig.master_in(1.0e5, midi::kStop);
    rig.run_s(0.3);
    CHECK_FALSE(rig.s->playing());
    CHECK_FALSE(rig.view().playing);
    rig.master_in(1.0e5, midi::kContinue);
    rig.run_s(0.4);
    NEED(rig.s->playing());
    CHECK(rig.view().play.order == 0);
    CHECK(rig.view().play.step >= step);
  }
}
