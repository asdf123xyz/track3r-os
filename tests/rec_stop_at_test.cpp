// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-REC2: STOP AT on the Record screen. The REC tap against the REAL sequencer's Step events (swing, a tempo change mid-take, several block sizes): a take told to stop at a step, a bar or a
// pattern start ends at EXACTLY the sample of that Step event (the take is [its first frame, the boundary sample) of the same clock), N bars end it by themselves at the 16 N-th Step event, a
// pending stop ends where the transport stops, and Manual (and a plain disarm) is what it was. The recorder's own rules (the choices, the stopped transport, a second F1) and the engine path
// (the Stopped event) are in recorder_test.cpp. Pure logic: host only.
#include <doctest/doctest.h>

#include <o3os/audio/events.h>
#include <o3os/audio/taps.h>
#include <o3os/hal/audio.h>
#include <o3os/seq/sequencer.h>

#include <cstdint>
#include <vector>

#include "support/need.h"
#include "support/seq_fixture.h"
#include "support/static_slot.h"

using namespace o3os;
using namespace o3os::test;
using audio::EventKind;
using audio::RecTap;
namespace fx = o3os::seq::fx;

namespace {

constexpr std::uint32_t kB = hal::audio::kBlock;
constexpr std::uint32_t kCapFrames = 2200000;  // 16 bars at 90 BPM

// A sequencer, a REC tap and a take, advanced a block at a time as the engine does: the sequencer's events of the block first (the Step events go to on_step with their offset, a Stopped event to
// on_stopped), then the block's audio into the tap. The audio is the sample clock itself (l[i] = the absolute sample, mod 2^16), so a take says where it began and ended.
struct Run {
  seq::Sequencer* s = nullptr;
  RecTap tap_obj;
  RecTap* tap = &tap_obj;
  std::vector<std::int16_t> take;
  alignas(4) std::int16_t l[1024];
  std::uint64_t t = 0;
  audio::SeqEvent out[audio::kMaxEventsPerBlock];
  struct Mark {
    std::uint64_t at;
    std::uint32_t value;  // the step index
  };
  std::vector<Mark> steps;    // every Step event, absolute sample
  std::vector<std::uint64_t> stops;  // every Stopped event

  Run() : take(kCapFrames + 8) {}

  void block(std::uint32_t n = kB) {
    const std::uint32_t c = s->step(t, n, out, audio::kMaxEventsPerBlock);
    for (std::uint32_t i = 0; i < c; ++i) {
      if (out[i].kind == EventKind::Step) {
        steps.push_back({t + out[i].offset, static_cast<std::uint32_t>(out[i].value)});
        tap->on_step(static_cast<std::uint32_t>(out[i].value), out[i].offset);
      } else if (out[i].kind == EventKind::Stopped) {
        stops.push_back(t + out[i].offset);
        tap->on_stopped(out[i].offset);
      }
    }
    for (std::uint32_t i = 0; i < n; ++i) {
      l[i] = static_cast<std::int16_t>(static_cast<std::uint16_t>(t + i));
    }
    tap->push(l, l, n);
    t += n;
  }
  void run_to(std::uint64_t until, std::uint32_t n = kB) {
    while (t < until) {
      block(n);
    }
  }
  bool arm(RecTap::Align align = RecTap::Align::Free, std::uint32_t auto_steps = 0) { return tap->arm(take.data(), kCapFrames, 1, align, auto_steps); }
  // The first block the tap is Done in (bounded).
  void until_done(std::uint32_t n = kB) {
    const std::uint64_t limit = t + 3000000;
    while (t < limit && tap->state() != RecTap::State::Done) {
      block(n);
    }
  }
  // The first Step event at or after `from` that `accept` takes.
  template <typename F>
  const Mark* first_step(std::uint64_t from, F accept) const {
    for (const Mark& m : steps) {
      if (m.at >= from && accept(m.value)) {
        return &m;
      }
    }
    return nullptr;
  }
  static std::int16_t clock(std::uint64_t at) { return static_cast<std::int16_t>(static_cast<std::uint16_t>(at)); }
};

// A pattern of `length` steps looping, swung and at 120 BPM.
void pattern_project(std::uint32_t length, std::uint32_t swing, std::uint32_t tempo_x100 = 12000) {
  fresh_project(4, 4);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(tempo_x100) == model::Status::Ok);
  CHECK(p.set_swing(swing) == model::Status::Ok);
  CHECK(p.create_pattern(1, length) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 0, make_step(60)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 3, make_step(62)) == model::Status::Ok);
}

seq::Sequencer& start(std::uint32_t length, std::uint32_t swing, std::uint32_t tempo_x100 = 12000) {
  pattern_project(length, swing, tempo_x100);
  seq::Sequencer& s = seq_slot().make(seq_project());
  s.play_pattern(1);
  return s;
}

const std::uint32_t kSizes[] = {64, 128, 32, 16, 64};

}  // namespace

TEST_CASE("stop at: a stop at STEP, BAR or PATTERN ends the take at the very sample of the next such Step event, through swing and a tempo change") {
  for (std::uint32_t swing : {50u, 58u, 66u}) {
    for (unsigned which = 0; which < 3; ++which) {
      for (std::uint32_t size : kSizes) {
        Run r;
        r.s = &start(48, swing);  // three bars a pattern: pattern starts every 48 steps, bars every 16
        r.run_to(3000, size);
        NEED(r.arm());  // Free: the take begins at the next block
        const std::uint64_t first = r.t;
        r.run_to(first + 41000, size);
        NEED(r.tap->state() == RecTap::State::Armed);
        // The tempo and the swing move under the take: the Step events carry the new times.
        CHECK(r.s->apply(core::ParamChange{seq::tempo_param(), 143.0f, 1}));
        CHECK(r.s->apply(core::ParamChange{seq::swing_param(), 62.0f, 1}));
        r.run_to(r.t + 5000, size);
        const std::uint64_t asked = r.t;
        const RecTap::Align a = which == 0 ? RecTap::Align::Step : (which == 1 ? RecTap::Align::Bar : RecTap::Align::Pattern);
        r.tap->stop_at(a);
        r.until_done(size);
        NEED(r.tap->state() == RecTap::State::Done);
        const auto accept = [which](std::uint32_t step) { return which == 0 || (which == 1 ? (step & 15u) == 0u : step == 0u); };
        const Run::Mark* end = r.first_step(asked, accept);
        NEED(end != nullptr);
        CHECK(r.tap->frames() == end->at - first);  // [the take's first sample, the boundary's sample)
        CHECK(r.tap->overrun_frames() == 0u);
        NEED(r.tap->frames() > 0u);
        CHECK(r.take[0] == Run::clock(first));
        CHECK(r.take[r.tap->frames() - 1u] == Run::clock(end->at - 1u));
        CHECK(r.take[r.tap->frames() / 2u] == Run::clock(first + r.tap->frames() / 2u));
      }
    }
  }
}

TEST_CASE("stop at: a take that starts on a bar and is told to stop at a bar is whole bars long, to the sample") {
  Run r;
  r.s = &start(64, 58);
  r.run_to(500, 64);
  NEED(r.arm(RecTap::Align::Bar));
  CHECK(r.tap->state() == RecTap::State::Waiting);
  r.run_to(100000, 64);
  NEED(r.tap->state() == RecTap::State::Armed);
  const Run::Mark* found = r.first_step(500, [](std::uint32_t v) { return (v & 15u) == 0u; });
  NEED(found != nullptr);
  const Run::Mark begin_mark = *found;  // (a copy: the list grows)
  const Run::Mark* begin = &begin_mark;
  r.tap->stop_at(RecTap::Align::Bar);
  const std::uint64_t asked = r.t;
  r.until_done(64);
  const Run::Mark* end = r.first_step(asked, [](std::uint32_t v) { return (v & 15u) == 0u; });
  NEED(end != nullptr);
  CHECK(r.tap->frames() == end->at - begin->at);
  CHECK(r.take[0] == Run::clock(begin->at));  // frame 0 IS the first bar's sample
  CHECK(r.take[r.tap->frames() - 1u] == Run::clock(end->at - 1u));
}

TEST_CASE("stop at: N bars end the take by itself at the 16 N-th Step event after it began, whatever the swing and tempo do, and the same for every block size") {
  for (std::uint32_t bars : {1u, 2u, 3u, 16u}) {
    std::uint64_t reference = 0;
    for (std::uint32_t size : kSizes) {
      Run r;
      r.s = &start(64, 60, 9000);
      r.run_to(777, size);
      NEED(r.arm(RecTap::Align::Bar, 16u * bars));
      r.run_to(777 + 6000, size);
      CHECK(r.s->apply(core::ParamChange{seq::tempo_param(), 190.0f, 1}));  // faster mid-take
      CHECK(r.s->apply(core::ParamChange{seq::swing_param(), 52.0f, 1}));
      r.until_done(size);
      NEED(r.tap->state() == RecTap::State::Done);
      // The start: the first bar Step event after 777; the end: the 16 N-th Step event after it.
      std::size_t first = r.steps.size();
      for (std::size_t i = 0; i < r.steps.size(); ++i) {
        if (r.steps[i].at >= 777 && (r.steps[i].value & 15u) == 0u) {
          first = i;
          break;
        }
      }
      NEED(first + 16u * bars < r.steps.size());
      const Run::Mark& end = r.steps[first + 16u * bars];
      CHECK((end.value & 15u) == 0u);  // a bar again
      CHECK(r.tap->frames() == end.at - r.steps[first].at);
      CHECK(r.take[0] == Run::clock(r.steps[first].at));
      CHECK(r.take[r.tap->frames() - 1u] == Run::clock(end.at - 1u));
      if (size == kSizes[0]) {
        reference = r.tap->frames();
      } else {
        CHECK(r.tap->frames() == reference);  // the block size does not move the end
      }
    }
  }
}

TEST_CASE("stop at: N bars of a take that began FREE count from the first Step event it sees") {
  Run r;
  r.s = &start(64, 50);
  r.run_to(5000, 64);
  NEED(r.arm(RecTap::Align::Free, 16));
  const std::uint64_t first = r.t;
  r.until_done(64);
  NEED(r.tap->state() == RecTap::State::Done);
  std::size_t i = 0;
  while (r.steps[i].at < first) {
    ++i;
  }
  CHECK(r.tap->frames() == r.steps[i + 15u].at - first);  // the 16th Step event at or after the first frame
  CHECK(r.take[0] == Run::clock(first));
}

TEST_CASE("stop at: a pending stop ends where the transport stops; an N bars take that the stop interrupts goes on until disarmed; Manual is a plain disarm") {
  {  // pending at a bar, the transport stops before it comes
    Run r;
    r.s = &start(64, 55);
    r.run_to(1000, 64);
    NEED(r.arm());
    const std::uint64_t first = r.t;
    r.run_to(first + 9000, 64);
    r.tap->stop_at(RecTap::Align::Bar);
    r.s->stop();
    r.until_done(64);
    NEED(r.tap->state() == RecTap::State::Done);
    NEED(!r.stops.empty());
    CHECK(r.tap->frames() == r.stops[0] - first);  // the Stopped event's own sample
    CHECK(r.take[r.tap->frames() - 1u] == Run::clock(r.stops[0] - 1u));
  }
  {  // N bars, then the transport stops: the take stays Armed (recording) and the disarm ends it at the next block
    Run r;
    r.s = &start(64, 55);
    r.run_to(1000, 64);
    NEED(r.arm(RecTap::Align::Free, 16 * 4));
    r.run_to(r.t + 9000, 64);
    r.s->stop();
    r.run_to(r.t + 64 * 40, 64);
    CHECK(r.tap->state() == RecTap::State::Armed);
    const std::uint32_t frames = r.tap->frames();
    CHECK(frames > 9000u);
    r.tap->disarm();
    r.block(64);
    CHECK(r.tap->state() == RecTap::State::Done);
    CHECK(r.tap->frames() == frames);  // the disarm writes nothing more: at the next block, as before
  }
  {  // Manual: disarm() ends at the next block's start; stop_at(Free) is the same call
    Run r;
    r.s = &start(64, 55);
    r.run_to(1000, 64);
    NEED(r.arm());
    r.run_to(r.t + 6400, 64);
    const std::uint32_t before = r.tap->frames();
    r.tap->stop_at(RecTap::Align::Free);
    r.block(64);
    CHECK(r.tap->state() == RecTap::State::Done);
    CHECK(r.tap->frames() == before);
  }
}

TEST_CASE("stop at: a take that began on a step, a take with no room, and a stop that was never asked for") {
  {  // a take that began on a Step event (START ON STEP) and is stopped at the next one
    Run r;
    r.s = &start(16, 50);
    r.run_to(100, 64);
    const std::uint64_t armed_at = r.t;
    NEED(r.arm(RecTap::Align::Step));
    r.run_to(r.t + 9000, 64);
    const Run::Mark* first_step = r.first_step(armed_at, [](std::uint32_t) { return true; });
    NEED(first_step != nullptr);
    const std::uint64_t begin = first_step->at;
    NEED(r.tap->state() == RecTap::State::Armed);
    r.tap->stop_at(RecTap::Align::Step);
    const std::uint64_t asked = r.t;
    r.until_done(64);
    const Run::Mark* end = r.first_step(asked, [](std::uint32_t) { return true; });
    NEED(end != nullptr);
    CHECK(r.tap->frames() == end->at - begin);
  }
  {  // the arena fills before the boundary: the take ends full, the cut frames are not counted as overrun
    Run r;
    r.s = &start(64, 50);
    r.run_to(100, 64);
    NEED(r.tap->arm(r.take.data(), 5000, 1, RecTap::Align::Free));
    r.tap->stop_at(RecTap::Align::Bar);
    r.until_done(64);
    CHECK(r.tap->frames() == 5000u);
    CHECK(r.tap->overrun_frames() > 0u);
  }
  {  // no request, no end: the take goes on past every Step event
    Run r;
    r.s = &start(16, 50);
    r.run_to(100, 64);
    NEED(r.arm());
    r.run_to(r.t + 60000, 64);
    CHECK(r.tap->state() == RecTap::State::Armed);
    CHECK(r.tap->frames() >= 60000u);
    r.tap->disarm();
    r.block(64);
    CHECK(r.tap->state() == RecTap::State::Done);
    r.tap->release();
    // a new take does not inherit the old request
    NEED(r.arm());
    r.run_to(r.t + 6000, 64);
    CHECK(r.tap->state() == RecTap::State::Armed);
  }
}
