// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-LOOPER slice L1: the loop player (audio/loop_player.h) against the REAL sequencer's Step events and a REC tap, as the engine drives them (events of the block first, then the block's audio into the tap
// and the loop into the TX block). A take made with LOOP = N BARS is exactly N bars; the loop starts at the sample the take ended on and restarts at every 16 N-th Step event, sample for sample, for ten
// minutes of blocks (no drift); a Stop fades it out and the next bar after the start resumes it; a tempo change leaves its length in samples (cut when faster, a gap when slower); a take that did not end
// on its boundary is no loop; release lets go of the buffer; and, through the real Engine, it is in no REC or RESAMPLE take. Pure logic: host only (an Engine and a VoicePool are too big for QEMU).
#include <doctest/doctest.h>

#include <o3os/audio/engine.h>
#include <o3os/audio/events.h>
#include <o3os/audio/loop_overdub.h>
#include <o3os/audio/loop_player.h>
#include <o3os/audio/taps.h>
#include <o3os/audio/voices.h>
#include <o3os/hal/audio.h>
#include <o3os/seq/engine_link.h>
#include <o3os/seq/sequencer.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>

#include "support/need.h"
#include "support/seq_fixture.h"
#include "support/static_slot.h"

using namespace o3os;
using namespace o3os::test;
using audio::EventKind;
using audio::LoopPlayer;
using audio::RecTap;
namespace loop_ov = audio::loop_ov;

namespace {

constexpr std::uint32_t kB = hal::audio::kBlock;
constexpr std::int32_t kUnity = 65536;

// What the REC tap is fed: a value of the absolute sample that is never zero, so that a take says where it began and silence is silence.
std::int16_t level_at(std::uint64_t t) noexcept { return static_cast<std::int16_t>(1 + t % 30011u); }

struct Run {
  seq::Sequencer* s = nullptr;
  RecTap tap;
  LoopPlayer loop;
  std::vector<std::int16_t> take;
  std::vector<std::int16_t> heard;  // the left channel of the TX block, every sample since t = 0
  alignas(4) std::int16_t l[1024];
  alignas(4) std::int16_t out[2048];
  std::uint64_t t = 0;
  audio::SeqEvent ev[audio::kMaxEventsPerBlock];
  struct Mark {
    std::uint64_t at;
    std::uint32_t value;
  };
  std::vector<Mark> steps;
  std::vector<std::uint64_t> stops;
  bool release_tap = true;  // the recorder gives the finished take back (RecTap::release) once the loop has it
  bool flat = false;        // the audio is a constant (12,000): any step of the loop's contribution is then the loop's own
  std::uint32_t inject_stop = 0xFFFFFFFFu;  // a Stopped event this many frames into the next block (the sequencer's own come at the block start)
  // OS-LOOPER L2c: the overdub's input (a constant block, null when off) and the cycle counter the restore's load guard reads (null: no guard).
  bool feed_input = false;
  std::int16_t in_const = 100;
  alignas(4) std::int16_t inb[1024];
  LoopPlayer::CycleSource cyc = nullptr;

  explicit Run(std::size_t take_frames = 400000) : take(take_frames + 8) {}

  void block(std::uint32_t n = kB) {
    const std::uint32_t c = s->step(t, n, ev, audio::kMaxEventsPerBlock);
    for (std::uint32_t i = 0; i < c; ++i) {
      if (ev[i].kind == EventKind::Step) {
        steps.push_back({t + ev[i].offset, static_cast<std::uint32_t>(ev[i].value)});
        tap.on_step(static_cast<std::uint32_t>(ev[i].value), ev[i].offset);
        loop.on_step(static_cast<std::uint32_t>(ev[i].value), ev[i].offset);
      } else if (ev[i].kind == EventKind::Stopped) {
        stops.push_back(t + ev[i].offset);
        tap.on_stopped(ev[i].offset);
        loop.on_stopped(ev[i].offset);
      }
    }
    if (inject_stop != 0xFFFFFFFFu) {
      stops.push_back(t + inject_stop);
      tap.on_stopped(inject_stop);
      loop.on_stopped(inject_stop);
      inject_stop = 0xFFFFFFFFu;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
      l[i] = flat ? std::int16_t{12000} : level_at(t + i);
      out[2u * i] = 0;
      out[2u * i + 1u] = 0;
    }
    tap.push(l, l, n);
    if (loop.needs_service()) {
      const std::int16_t* ip = nullptr;
      if (feed_input) {
        for (std::uint32_t i = 0; i < n; ++i) {
          inb[i] = in_const;
        }
        ip = inb;
      }
      loop.service(out, n, false, kUnity, tap, ip, cyc, 0u);  // (the block started at cycle 0: a fake counter that returns a constant is the load the block has already used)
    }
    if (release_tap && loop.reading() && tap.state() == RecTap::State::Done) {
      tap.release();
    }
    for (std::uint32_t i = 0; i < n; ++i) {
      CHECK(out[2u * i] == out[2u * i + 1u]);  // the mono loop is on both channels
      heard.push_back(out[2u * i]);
    }
    t += n;
  }
  void run_to(std::uint64_t until, std::uint32_t n = kB) {
    while (t < until) {
      block(n);
    }
  }
  // A loop take of `bars` bars, from the next bar on: announced, armed bar-aligned with 16 N Step events. Runs until the loop exists. False: it never came.
  bool record_loop(unsigned bars, std::uint32_t n = kB) {
    loop.capture(16u * bars);
    if (!tap.arm(take.data(), static_cast<std::uint32_t>(take.size()) - 8u, 1, RecTap::Align::Bar, 16u * bars)) {
      return false;
    }
    const std::uint64_t limit = t + 3000000;
    while (t < limit && !loop.reading()) {
      block(n);
    }
    return loop.reading();
  }
  std::uint64_t step_at(std::size_t i) const { return steps[i].at; }
  // The index in `steps` of the first Step event at or after `from`.
  std::size_t first_step(std::uint64_t from) const {
    std::size_t i = 0;
    while (i < steps.size() && steps[i].at < from) {
      ++i;
    }
    return i;
  }
};

seq::Sequencer& start(std::uint32_t length, std::uint32_t swing, std::uint32_t tempo_x100 = 12000) {
  fresh_project(4, 4);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(tempo_x100) == model::Status::Ok);
  CHECK(p.set_swing(swing) == model::Status::Ok);
  CHECK(p.create_pattern(1, length) == model::Status::Ok);
  seq::Sequencer& s = seq_slot().make(seq_project());
  s.play_pattern(1);
  return s;
}

// The loop restarts at the Step events index `first`, `first + per`, ...: every one must play the take from its frame 0, for as long as the take is, or until the next restart; the rest of the
// way to the next restart (a slower tempo than the take's) is silence. Returns how many restarts were checked, and counts the cuts (a restart before the take's end) and the gaps.
struct Verdict {
  unsigned restarts = 0;
  unsigned cuts = 0;
  unsigned gaps = 0;
};
Verdict verify(const Run& r, std::size_t first, std::uint32_t per, std::size_t until_step, std::uint32_t len) {
  constexpr std::uint64_t kRampIn = LoopPlayer::kFadeFrames;  // the first start of a loop (after its take) is ramped in: its first frames are not the take's
  Verdict v;
  for (std::size_t k = first; k + per <= until_step && k + per < r.steps.size(); k += per) {
    const std::uint64_t a = r.steps[k].at;
    const std::uint64_t b = r.steps[k + per].at;
    const std::uint64_t played = (b - a) < len ? (b - a) : len;
    CHECK((r.steps[k].value & 15u) == 0u);  // a bar
    std::uint64_t wrong = 0;
    for (std::uint64_t j = k == first ? kRampIn : 0u; j < played; ++j) {
      wrong += r.heard[a + j] != r.take[j] ? 1u : 0u;
    }
    for (std::uint64_t j = played; j < b - a && (b - a) > len; ++j) {  // past the end of the take: its last frame held for two frames, then silence
      const std::int16_t want = j - len < LoopPlayer::kHoldFrames ? r.take[len - 1u] : std::int16_t{0};
      wrong += r.heard[a + j] != want ? 1u : 0u;
    }
    CHECK(wrong == 0u);
    ++v.restarts;
    v.cuts += (b - a) < len ? 1u : 0u;
    v.gaps += (b - a) > len ? 1u : 0u;
  }
  return v;
}

}  // namespace

TEST_CASE("loop: the take of a loop ends on its boundary and the loop plays from that very sample, sample for sample") {
  for (std::uint32_t size : {64u, 128u, 32u, 16u}) {
    Run r;
    r.s = &start(64, 58);
    r.run_to(1500, size);
    NEED(r.record_loop(2, size));
    const std::uint32_t len = r.tap.frames();
    const std::size_t f0 = r.first_step(1500);
    std::size_t begin = f0;  // the first bar event after 1500: where the take began
    while ((r.steps[begin].value & 15u) != 0u) {
      ++begin;
    }
    const std::size_t end = begin + 32u;
    CHECK(len == r.steps[end].at - r.steps[begin].at);  // exactly two bars
    CHECK(r.take[0] == level_at(r.steps[begin].at));
    CHECK(r.loop.frames() == len);
    r.run_to(r.t + 6u * len, size);
    for (std::uint64_t i = 0; i < r.steps[end].at; ++i) {
      if (r.heard[i] != 0) {
        CHECK(r.heard[i] == 0);  // nothing is heard before the take is over (and the take itself is in no TX block)
        break;
      }
    }
    const Verdict v = verify(r, end, 32u, r.steps.size(), len);
    CHECK(v.restarts >= 4u);
  }
}

TEST_CASE("loop: ten minutes of blocks at a tempo with a fractional bar length: every restart is the sequencer's own bar, no drift") {
  Run r;
  r.s = &start(64, 58, 13337);
  r.run_to(1500, 64);
  NEED(r.record_loop(4, 64));
  const std::uint32_t len = r.tap.frames();
  // The end event of the take = the first restart: the 64th Step event after the take's first.
  std::size_t begin = r.first_step(1500);
  while ((r.steps[begin].value & 15u) != 0u) {
    ++begin;
  }
  const std::size_t first = begin + 64u;
  const std::uint64_t ten_minutes = 10ull * 60ull * 44118ull;
  r.run_to(ten_minutes, 64);
  const Verdict v = verify(r, first, 64u, r.steps.size(), len);
  CHECK(v.restarts >= 70u);  // 4 bars are about 7.2 s at 133.37 BPM
  // The sequencer places a bar at a rounded sample, so N bars are not always the same number of samples: the restarts are what keeps the loop on the bars. A free-running modulo of the take's length would
  // have walked away from the last restart by the sum of the differences; here every restart is at the bar.
  std::int64_t walked = 0;
  for (std::size_t k = first; k + 64u < r.steps.size(); k += 64u) {
    walked += static_cast<std::int64_t>(r.steps[k + 64u].at - r.steps[k].at) - static_cast<std::int64_t>(len);
  }
  CHECK((walked > 2 || walked < -2));  // (the figure the modulo would have drifted by: it is not zero at this tempo)
}

TEST_CASE("loop: a Stop fades the loop out within two blocks, and it resumes at its start on the first bar after the transport starts again") {
  Run r;
  r.s = &start(64, 50);
  r.run_to(1500, 64);
  NEED(r.record_loop(1, 64));
  const std::uint32_t len = r.tap.frames();
  r.run_to(r.t + len + len / 2u, 64);  // into the second pass of the loop
  r.s->stop();
  r.run_to(r.t + 64u * 8u, 64);
  NEED(r.stops.size() == 1u);
  const std::uint64_t stopped = r.stops[0];
  std::uint64_t last = 0;
  for (std::uint64_t i = stopped; i < r.heard.size(); ++i) {
    if (r.heard[i] != 0) {
      last = i;
    }
  }
  CHECK(last >= stopped + LoopPlayer::kFadeFrames - 4u);  // the fade lasts its 32 frames, across the block boundary when the stop is late in a block
  CHECK(last < stopped + 2u * 64u);     // ... that ends within two blocks of the stop
  r.run_to(r.t + len, 64);
  for (std::uint64_t i = stopped + 2u * 64u; i < r.heard.size(); ++i) {
    if (r.heard[i] != 0) {
      CHECK(r.heard[i] == 0);  // silent while the transport is stopped
      break;
    }
  }
  // Play again: from step 0, a bar. The loop starts at that Step event's sample with its first frame.
  const std::size_t before = r.steps.size();
  r.s->play_pattern(1);
  r.run_to(r.t + len + 4096u, 64);
  NEED(r.steps.size() > before);
  CHECK(r.steps[before].value == 0u);
  const std::uint64_t again = r.steps[before].at;
  std::uint64_t wrong = 0;
  for (std::uint64_t j = LoopPlayer::kFadeFrames; j < len && again + j < r.heard.size() && j < 16u * 4096u; ++j) {
    const std::uint64_t next = r.steps[before + 16u].at;  // the loop is one bar: it restarts at the next bar
    if (again + j >= next) {
      break;
    }
    wrong += r.heard[again + j] != r.take[j] ? 1u : 0u;
  }
  CHECK(wrong == 0u);
  CHECK(r.heard[again] == 0);                                // ramped in: not a step at the bar
  CHECK(r.heard[again + LoopPlayer::kFadeFrames + 3u] == r.take[LoopPlayer::kFadeFrames + 3u]);
}

TEST_CASE("loop: a transport that starts in the middle of a bar waits for the next bar") {
  Run r;
  r.s = &start(64, 50);
  r.run_to(1500, 64);
  NEED(r.record_loop(1, 64));
  const std::uint32_t len = r.tap.frames();
  r.s->stop();
  r.run_to(r.t + 64u * 8u, 64);
  const std::size_t before = r.steps.size();
  r.s->play_pattern(1, 5);  // from step 5
  r.run_to(r.t + 2u * len, 64);
  NEED(r.steps.size() > before + 12u);
  CHECK(r.steps[before].value == 5u);
  std::size_t bar = before;
  while ((r.steps[bar].value & 15u) != 0u) {
    ++bar;
  }
  CHECK(r.steps[bar].value == 16u);  // step 5 is in the first bar: the next bar is step 16
  for (std::uint64_t i = r.steps[before].at; i < r.steps[bar].at; ++i) {
    if (r.heard[i] != 0) {
      CHECK(r.heard[i] == 0);  // not a sample before the bar
      break;
    }
  }
  CHECK(r.heard[r.steps[bar].at + LoopPlayer::kFadeFrames + 5u] == r.take[LoopPlayer::kFadeFrames + 5u]);  // (ramped in over the first frames)
}

TEST_CASE("loop: a tempo change leaves the length in samples: a faster tempo cuts the loop short, a slower one leaves a gap, and it restarts on every bar boundary either way") {
  Run r;
  r.s = &start(64, 52, 12000);
  r.run_to(1500, 64);
  NEED(r.record_loop(2, 64));
  const std::uint32_t len = r.tap.frames();
  std::size_t begin = r.first_step(1500);
  while ((r.steps[begin].value & 15u) != 0u) {
    ++begin;
  }
  const std::size_t first = begin + 32u;
  r.run_to(r.t + 3u * len, 64);
  CHECK(r.s->apply(core::ParamChange{seq::tempo_param(), 170.0f, 1}));  // faster: 2 bars are shorter than the take
  const std::size_t faster_from = r.steps.size();
  r.run_to(r.t + 6u * len, 64);
  CHECK(r.s->apply(core::ParamChange{seq::tempo_param(), 80.0f, 1}));   // slower: a gap at the end of the loop
  const std::size_t slower_from = r.steps.size();
  r.run_to(r.t + 8u * len, 64);
  // From the first restart on, every restart is a bar event 32 Step events after the last (the Step events carry the new times).
  const Verdict v = verify(r, first, 32u, r.steps.size(), len);
  CHECK(v.restarts >= 8u);
  CHECK(v.cuts >= 1u);
  CHECK(v.gaps >= 1u);
  (void)faster_from;
  (void)slower_from;
  CHECK(r.loop.frames() == len);  // the length in samples is what was recorded
}

TEST_CASE("loop: a take that did not end on its boundary is no loop, and a release lets go of the buffer after a fade") {
  {  // F1 in the middle of the take: the disarm ends it, not a boundary
    Run r;
    r.s = &start(64, 50);
    r.run_to(1500, 64);
    r.loop.capture(32u);
    NEED(r.tap.arm(r.take.data(), 300000, 1, RecTap::Align::Bar, 32u));
    r.run_to(r.t + 20000, 64);
    CHECK(r.loop.capturing());
    r.tap.disarm();
    r.run_to(r.t + 3u * 64u, 64);
    CHECK(r.tap.state() == RecTap::State::Done);
    CHECK_FALSE(r.loop.reading());
    CHECK(r.loop.phase() == LoopPlayer::Phase::Idle);
    CHECK_FALSE(r.loop.needs_service());
  }
  {  // the arena fills before the boundary
    Run r;
    r.s = &start(64, 50);
    r.run_to(1500, 64);
    r.loop.capture(32u);
    NEED(r.tap.arm(r.take.data(), 20000, 1, RecTap::Align::Bar, 32u));
    r.run_to(r.t + 150000, 64);  // (the take starts at the next bar and fills its 20,000 frames before the 32 steps are over)
    CHECK(r.tap.state() == RecTap::State::Done);
    CHECK_FALSE(r.loop.reading());
  }
  {  // a cancelled announcement
    Run r;
    r.s = &start(64, 50);
    r.run_to(1500, 64);
    r.loop.capture(16u);
    r.loop.release();
    r.block(64);
    CHECK(r.loop.phase() == LoopPlayer::Phase::Idle);
  }
  {  // a release of a loop that plays: faded within the block, the buffer let go at the end of it
    Run r;
    r.s = &start(64, 50);
    r.run_to(1500, 64);
    NEED(r.record_loop(1, 64));
    const std::uint32_t len = r.tap.frames();
    r.run_to(r.t + len / 2u, 64);
    CHECK(r.loop.reading());
    const std::uint64_t at = r.t;
    r.loop.release();
    r.block(64);
    CHECK_FALSE(r.loop.reading());  // dropped at the end of that block
    CHECK(r.heard[at] != 0);        // it was still sounding at the start of the block
    CHECK(r.heard[at + LoopPlayer::kFadeFrames + 1u] == 0);  // ... and faded out to nothing within the fade
    for (unsigned i = 1; i < 64; ++i) {
      CHECK(std::abs(static_cast<int>(r.heard[at + i]) - static_cast<int>(r.heard[at + i - 1u])) < 2000);  // a ramp, not a cut
    }
    r.run_to(r.t + len, 64);
    for (std::uint64_t i = at + 64u; i < r.heard.size(); ++i) {
      if (r.heard[i] != 0) {
        CHECK(r.heard[i] == 0);
        break;
      }
    }
    CHECK_FALSE(r.loop.needs_service());
  }
}

TEST_CASE("loop: the mute ramps over a block and a muted loop keeps its place on the bars") {
  Run r;
  r.s = &start(64, 50);
  r.run_to(1500, 64);
  NEED(r.record_loop(1, 64));
  const std::uint32_t len = r.tap.frames();
  r.run_to(r.t + 1000, 64);
  r.loop.set_muted(true);
  const std::uint64_t at = r.t;
  r.run_to(r.t + len / 3u, 64);
  r.loop.set_muted(false);
  const std::uint64_t unmuted = r.t;
  r.run_to(r.t + len, 64);
  // Muted: silent from the end of the first block (a ramp over it); unmuted: it is where the bars have it, not where it was muted.
  CHECK(r.heard[at + 64u] == 0);
  CHECK(r.heard[at + 100u] == 0);
  std::size_t k = r.first_step(unmuted + 256u);
  std::uint64_t restart = 0;
  for (; k < r.steps.size(); ++k) {
    if ((r.steps[k].value & 15u) == 0u) {
      restart = r.steps[k].at;
      break;
    }
  }
  NEED(restart != 0u);
  CHECK(r.heard[restart + 5u] == r.take[5]);
}

TEST_CASE("loop: a Stop late in a block fades over its 32 frames, across the block boundary, with no step") {
  for (const std::uint32_t phase : {0u, 20u, 50u, 63u}) {  // the Stop lands at several places of its block
    Run r;
    r.s = &start(64, 50);
    r.run_to(1500, 64);
    NEED(r.record_loop(1, 64));
    const std::uint32_t len = r.tap.frames();
    r.run_to(r.t + len / 2u, 64);
    r.inject_stop = phase;
    r.run_to(r.t + 64u * 6u, 64);
    NEED(r.stops.size() == 1u);
    const std::uint64_t stopped = r.stops[0];
    std::uint64_t last = 0;
    for (std::uint64_t i = stopped; i < r.heard.size(); ++i) {
      if (r.heard[i] != 0) {
        last = i;
      }
    }
    CHECK(last >= stopped + LoopPlayer::kFadeFrames - 4u);  // the fade lasts its 32 frames, whatever is left of the block
    CHECK(last < stopped + LoopPlayer::kFadeFrames + 2u);
    for (std::uint64_t i = stopped; i + 1u < stopped + LoopPlayer::kFadeFrames; ++i) {  // a ramp: no step bigger than its own slope
      CHECK(std::abs(static_cast<int>(r.heard[i + 1u]) - static_cast<int>(r.heard[i])) < 3000);
    }
  }
}

TEST_CASE("loop: at a fractional tempo the restart that comes a frame late holds the take's last frame: no step in the loop's contribution, ever") {
  unsigned late = 0;
  unsigned early = 0;
  unsigned ticks = 0;
  unsigned restarts = 0;
  for (const std::uint32_t tempo : {12300u, 12700u, 13100u, 11900u}) {  // the bar is not a whole number of samples: N bars are a sample longer or shorter than the take, loop by loop
    Run r;
    r.flat = true;
    r.s = &start(64, 50, tempo);
    r.run_to(1500, 64);
    NEED(r.record_loop(2, 64));
    const std::uint32_t len = r.tap.frames();
    std::size_t begin = r.first_step(1500);
    while ((r.steps[begin].value & 15u) != 0u) {
      ++begin;
    }
    const std::size_t first = begin + 32u;
    r.run_to(100ull * 44118ull, 64);
    for (std::size_t k = first + 32u; k + 32u < r.steps.size(); k += 32u) {  // (the first restart is the ramp-in's)
      const std::uint64_t at = r.steps[k].at;
      const std::uint64_t prev = r.steps[k - 32u].at;
      late += at - prev > len ? 1u : 0u;
      early += at - prev < len ? 1u : 0u;
      ++restarts;
      for (std::uint64_t i = at - 4u; i < at + 4u; ++i) {  // around the restart the contribution is the constant, whichever side the bar fell
        ticks += r.heard[i] != 12000 ? 1u : 0u;
      }
    }
  }
  CHECK(restarts > 80u);
  CHECK(late >= 1u);  // both cases are exercised
  CHECK(early >= 1u);
  CHECK(ticks == 0u);
}

TEST_CASE("loop: a take that ends exactly on the last free frame is a loop, one frame short of room is not") {
  std::uint32_t len = 0;
  {
    Run r;
    r.s = &start(64, 50);
    r.run_to(1500, 64);
    NEED(r.record_loop(1, 64));
    len = r.tap.frames();
  }
  for (const std::uint32_t room : {len, len - 1u}) {
    Run r(room);  // the take's memory holds `room` frames
    r.release_tap = false;
    r.s = &start(64, 50);
    r.run_to(1500, 64);
    r.loop.capture(16u);
    NEED(r.tap.arm(r.take.data(), room, 1, RecTap::Align::Bar, 16u));
    for (int i = 0; i < 40000 && r.tap.state() != RecTap::State::Done; ++i) {
      r.block(64);
    }
    r.block(64);
    CHECK(r.tap.state() == RecTap::State::Done);
    CHECK(r.loop.reading() == (room == len));  // exactly full is a loop; a frame that did not fit is not
    CHECK(r.tap.overrun_frames() == (room == len ? 0u : 1u));
  }
}

TEST_CASE("loop: a disarm in the very block of the boundary is no loop (the take lacks that block's frames)") {
  RecTap tap;
  LoopPlayer loop;
  std::vector<std::int16_t> take(70000);
  alignas(4) std::int16_t l[64];
  alignas(4) std::int16_t out[128] = {};
  for (std::uint32_t i = 0; i < 64; ++i) {
    l[i] = 500;
  }
  loop.capture(1);
  loop.service(out, 64, false, kUnity, tap);
  NEED(tap.arm(take.data(), 60000, 1, RecTap::Align::Free, 1));
  for (int i = 0; i < 20; ++i) {
    tap.push(l, l, 64);
  }
  tap.on_step(0, 10);  // the boundary falls in this block ...
  tap.disarm();        // ... and so does the F1
  tap.push(l, l, 64);
  CHECK(tap.state() == RecTap::State::Done);
  loop.service(out, 64, false, kUnity, tap);
  CHECK_FALSE(loop.reading());
  CHECK(loop.phase() == LoopPlayer::Phase::Idle);
}

TEST_CASE("loop: at a fractional tempo the held last frame is the take's own (a signal that is not flat), and a release asked while parked is done when it is given back") {
  unsigned late = 0;
  unsigned wrong = 0;
  for (const std::uint32_t tempo : {12300u, 12700u, 13100u, 11900u}) {
    Run r;
    r.s = &start(64, 50, tempo);
    r.run_to(1500, 64);
    NEED(r.record_loop(2, 64));
    const std::uint32_t len = r.tap.frames();
    std::size_t begin = r.first_step(1500);
    while ((r.steps[begin].value & 15u) != 0u) {
      ++begin;
    }
    const std::size_t first = begin + 32u;
    r.run_to(100ull * 44118ull, 64);
    for (std::size_t k = first + 32u; k + 32u < r.steps.size(); k += 32u) {
      const std::uint64_t at = r.steps[k].at;
      if (at - r.steps[k - 32u].at == len + 1u) {  // the bar a sample longer than the take
        ++late;
        wrong += r.heard[at - 1u] != r.take[len - 1u] ? 1u : 0u;  // the frame the take has no more of is its last
        wrong += r.heard[at] != r.take[0] ? 1u : 0u;
      }
    }
    // A release while an export has the engine waits, and is done (the buffer let go) when the engine is given back.
    r.loop.park(true);
    r.loop.release();
    r.run_to(r.t + 64u * 10u, 64);
    CHECK(r.loop.reading());
    r.loop.park(false);
    r.run_to(r.t + 64u * 3u, 64);
    CHECK_FALSE(r.loop.reading());
    CHECK(r.loop.phase() == LoopPlayer::Phase::Idle);
    CHECK_FALSE(r.loop.needs_service());
  }
  CHECK(late >= 1u);
  CHECK(wrong == 0u);
}

TEST_CASE("loop: an export parks the player: it hears nothing and moves nothing, and it is halted when the engine is given back") {
  Run r;
  r.s = &start(64, 50);
  r.run_to(1500, 64);
  NEED(r.record_loop(1, 64));
  const std::uint32_t len = r.tap.frames();
  r.run_to(r.t + len / 2u, 64);
  r.loop.park(true);
  CHECK_FALSE(r.loop.needs_service());
  const std::uint64_t parked_at = r.t;
  r.run_to(r.t + 3u * len, 64);
  for (std::uint64_t i = parked_at; i < r.heard.size(); ++i) {
    if (r.heard[i] != 0) {
      CHECK(r.heard[i] == 0);  // not a sample while the export has the engine
      break;
    }
  }
  r.loop.park(false);
  CHECK(r.loop.reading());  // the loop itself is untouched
  const std::uint64_t given_back = r.t;
  r.run_to(r.t + 2u * len, 64);
  std::size_t bar = r.first_step(given_back);
  while ((r.steps[bar].value & 15u) != 0u) {
    ++bar;
  }
  for (std::uint64_t i = given_back; i < r.steps[bar].at; ++i) {
    if (r.heard[i] != 0) {
      CHECK(r.heard[i] == 0);  // halted: silent until the first bar
      break;
    }
  }
  CHECK(r.heard[r.steps[bar].at + LoopPlayer::kFadeFrames + 5u] == r.take[LoopPlayer::kFadeFrames + 5u]);
}

namespace {

std::uint32_t g_cycles = 0;
std::uint32_t fake_cycles() noexcept { return ++g_cycles; }

}  // namespace

TEST_CASE("loop through the engine: heard on the line out at LOOP VOL, and in no REC take, no RESAMPLE take") {
  namespace ha = hal::audio;
  static StaticSlot<audio::Engine> engine_slot;
  static StaticSlot<audio::VoicePool> pool_slot;
  static audio::TapMem taps;
  static audio::SeqEvent events[audio::kMaxEventsPerBlock];
  g_cycles = 0;
  audio::Engine& e = engine_slot.make(&fake_cycles, taps);
  e.attach_voices(&pool_slot.make());
  seq::Sequencer& s = start(64, 50);
  e.attach_events(seq::event_source(s, events, audio::kMaxEventsPerBlock));
  // The input strip feeds the REC bus alone (what the recorder does for a source): route REC, level 100.
  CHECK(e.params().push(audio::ParamChange{audio::strip_param(audio::kInputStrip, audio::graph_param::strip::Route), static_cast<float>(audio::route::kRec), 1}));
  CHECK(e.params().push(audio::ParamChange{audio::strip_param(audio::kInputStrip, audio::graph_param::strip::Volume), 100.0f, 1}));
  alignas(8) std::int16_t in[ha::kBlockSamples] = {};
  alignas(8) std::int16_t out[ha::kBlockSamples] = {};
  ha::BlockReport report{};
  std::uint64_t t = 0;
  bool input_on = true;
  std::vector<std::int16_t> heard;  // the TX block's left channel
  const auto render = [&] {
    for (std::uint32_t i = 0; i < ha::kBlock; ++i) {
      const std::int16_t v = input_on ? level_at(t + i) : std::int16_t{0};
      in[2u * i] = v;
      in[2u * i + 1u] = v;
    }
    report = ha::BlockReport{};
    e.render_block(ha::BlockIo{in, out, 0, &report});
    for (std::uint32_t i = 0; i < ha::kBlock; ++i) {
      heard.push_back(out[2u * i]);
    }
    t += ha::kBlock;
  };
  for (int i = 0; i < 40; ++i) {
    render();
  }
  alignas(4) static std::int16_t take[200000];
  alignas(4) static std::int16_t second[40000];
  audio::RecTap& rec = e.graph().rec();
  audio::LoopPlayer& loop = e.graph().loop();
  loop.capture(16u);
  NEED(rec.arm(take, 200000, 1, RecTap::Align::Bar, 16u));
  for (int i = 0; i < 4000 && !loop.reading(); ++i) {
    render();
  }
  NEED(loop.reading());
  const std::uint32_t len = rec.frames();
  CHECK(len > 40000u);
  CHECK(rec.state() == RecTap::State::Done);
  rec.release();
  input_on = false;  // from here the input is silent: whatever a take holds now is what the loop put on a bus
  for (int i = 0; i < 40; ++i) {
    render();
  }
  // The loop is on the line out (left == right was checked by the player's own test; here the engine's chain: LOOP VOL 100 and the master level 100 are unity).
  const std::uint64_t t_listen = t;
  for (int i = 0; i < 900; ++i) {
    render();
  }
  unsigned heard_nonzero = 0;
  for (std::uint64_t i = t_listen; i < t; ++i) {
    heard_nonzero += heard[i] != 0 ? 1u : 0u;
  }
  CHECK(heard_nonzero > 40000u);
  // A RESAMPLE take (MAIN after the master level) and a REC bus take, while the loop plays: silent.
  for (const RecTap::Source src : {RecTap::Source::Master, RecTap::Source::Bus}) {
    rec.set_source(src);
    NEED(rec.arm(second, 40000, 1, RecTap::Align::Free));
    for (int i = 0; i < 600; ++i) {
      render();
    }
    rec.disarm();
    render();
    render();
    CHECK(rec.frames() > 30000u);
    unsigned loud = 0;
    for (std::uint32_t i = 0; i < rec.frames(); ++i) {
      loud += second[i] != 0 ? 1u : 0u;
    }
    CHECK(loud == 0u);  // the loop is in neither
    rec.release();
  }
  unsigned still = 0;
  for (std::uint64_t i = t - 600u * ha::kBlock; i < t; ++i) {
    still += heard[i] != 0 ? 1u : 0u;
  }
  CHECK(still > 10000u);  // ... and was playing all the time
  // The master level scales it: LOOP VOL goes through the level law (50 is about -20 dB).
  loop.set_volume(50);
  for (std::uint32_t i = 0; i < len / ha::kBlock + 8u; ++i) {
    render();
  }
  std::int32_t peak_full = 0;
  std::int32_t peak_half = 0;
  for (std::uint64_t i = t - len; i < t; ++i) {
    peak_half = std::max<std::int32_t>(peak_half, std::abs(heard[i]));
  }
  for (std::uint64_t i = t_listen; i < t_listen + len; ++i) {
    peak_full = std::max<std::int32_t>(peak_full, std::abs(heard[i]));
  }
  CHECK(peak_half > 0);
  CHECK(peak_half * 100 < peak_full * 15);  // level 50 of the mixer law is -20 dB
  CHECK(peak_half * 100 > peak_full * 5);
}

// ---- OS-LOOPER L2c: the overdub session, the punch ramps, the two-step end, the restore (spec 3c) ------------------------------------------------------------------------------------
namespace {

std::uint32_t g_fake_block_cycles = 0;
std::uint32_t fake_block_cycles() noexcept { return g_fake_block_cycles; }

// A loop of `bars` bars recorded and playing, its original kept, its bitmap clear (the main loop's job before a session).
struct Ov {
  Run r;
  std::uint32_t len = 0;
  bool ok = false;
  std::vector<std::int16_t> orig;
  explicit Ov(unsigned bars = 1, std::uint32_t tempo = 12000, std::size_t take_frames = 400000) : r(take_frames) {
    r.s = &start(64, 50, tempo);
    r.run_to(1500, 64);
    ok = r.record_loop(bars, 64);
    len = r.loop.frames();
    orig.assign(r.take.begin(), r.take.begin() + len);
    r.feed_input = true;
  }
  std::uint32_t even() const { return (len + 1u) & ~1u; }
  std::int16_t* undo() { return r.take.data() + even(); }
  std::uint16_t* bitmap() { return reinterpret_cast<std::uint16_t*>(r.take.data() + 2u * even()); }
  std::uint32_t bitmap_halfwords() const { return ((len + 63u) / 64u + 15u) / 16u; }
  void clear_bitmap() { std::memset(bitmap(), 0, bitmap_halfwords() * 2u); }
  unsigned dirty() {
    unsigned n = 0;
    for (std::uint32_t i = 0; i < bitmap_halfwords(); ++i) {
      n += static_cast<unsigned>(__builtin_popcount(bitmap()[i]));
    }
    return n;
  }
  // The main loop's side of a session: clear, post.
  std::uint32_t begin(unsigned fb = 100) {
    clear_bitmap();
    r.loop.set_feedback(fb);
    return r.loop.overdub_on();
  }
  bool run_until(const std::function<bool()>& done, unsigned blocks = 4000) {
    for (unsigned i = 0; i < blocks && !done(); ++i) {
      r.block(64);
    }
    return done();
  }
  // Ends the session and waits for the acknowledgement.
  bool end() {
    const std::uint32_t q = r.loop.quiesce();
    return run_until([&] { return r.loop.acked(q); }, 50);
  }
  std::vector<int> diff() const {
    std::vector<int> d(len);
    for (std::uint32_t i = 0; i < len; ++i) {
      d[i] = static_cast<int>(r.take[i]) - static_cast<int>(orig[i]);
    }
    return d;
  }
  bool original() const { return std::equal(orig.begin(), orig.end(), r.take.begin()); }
};

}  // namespace

TEST_CASE("loop overdub: a session writes loop + input where the write head passes, ramps in and out without a step, and the undo buffer holds the originals of the blocks it touched") {
  Ov o;
  NEED(o.ok);
  o.r.run_to(o.r.t + 300u * 64u, 64);  // a few blocks into the loop
  CHECK_FALSE(o.r.loop.overdubbing());
  const std::uint32_t seq = o.begin();
  CHECK_FALSE(o.r.loop.acked(seq));
  NEED(o.run_until([&] { return o.r.loop.acked(seq); }, 10));
  CHECK(o.r.loop.overdubbing());
  o.r.run_to(o.r.t + 100u * 64u, 64);  // 6,400 frames of a loop of 88,200: well under one pass
  CHECK(o.r.loop.layer());
  NEED(o.end());
  CHECK_FALSE(o.r.loop.overdubbing());
  const std::vector<int> d = o.diff();
  std::uint32_t first = 0;
  std::uint32_t last = 0;
  std::uint32_t full = 0;
  int worst_step = 0;
  for (std::uint32_t i = 0; i < o.len; ++i) {
    if (d[i] != 0) {
      first = first == 0u ? i : first;
      last = i;
    }
    full += d[i] == 100 ? 1u : 0u;
    if (i != 0u) {
      worst_step = std::max(worst_step, std::abs(d[i] - d[i - 1u]));
    }
    CHECK(d[i] >= 0);
    CHECK(d[i] <= 100);
  }
  CHECK(first != 0u);
  CHECK(last - first > 5000u);
  CHECK(full + 2u * LoopPlayer::kFadeFrames >= last - first);  // the whole span is at the input's level but the two ramps
  CHECK(full + 2u * LoopPlayer::kFadeFrames - 4u <= last - first + 4u * LoopPlayer::kFadeFrames);
  CHECK(worst_step <= 4);  // 100 over 32 frames: no step at the punch in, none at the punch out
  // Every touched block is in the undo buffer exactly as it was; the bitmap says which.
  for (std::uint32_t b = 0; b * 64u < o.len; ++b) {
    const bool marked = ((o.bitmap()[b >> 4] >> (b & 15u)) & 1u) != 0u;
    const std::uint32_t lo = b * 64u;
    const std::uint32_t hi = std::min(lo + 64u, o.len);
    bool touched = false;
    for (std::uint32_t i = lo; i < hi; ++i) {
      touched = touched || d[i] != 0;
    }
    CHECK((!touched || marked));  // a block that changed is marked
    if (marked) {
      for (std::uint32_t i = lo; i < hi; ++i) {
        CHECK(o.undo()[i] == o.orig[i]);
      }
    }
  }
}

// L2c review minor: the punch-in ramp starts at g = 0, so its first frame is written back unchanged; when that frame is the only one the block writes (the loop's last frame), nothing was changed and
// no block may be marked or the layer flagged (F5 would light with nothing to undo). The next pass, whose ramp really writes, does mark them.
TEST_CASE("loop overdub: a punch-in whose only frame in the block is the ramp's first (g = 0) marks nothing and flags no layer") {
  Ov o;
  NEED(o.ok);
  constexpr std::uint64_t kBar = 88200;  // 120 BPM, 16 steps of 5,512.5 frames: the loop restarts on every bar (its take is a little longer than the bar and is cut by the restart)
  std::uint64_t restart = 0;
  for (const auto& m : o.r.steps) {
    if (m.value % 16u == 0u && m.at <= o.r.t) {
      restart = m.at;
    }
  }
  const std::uint64_t target = restart + kBar - 1u;  // the play head is on the last frame before the next restart
  NEED(target > o.r.t);
  while (o.r.t < target) {
    o.r.block(static_cast<std::uint32_t>(std::min<std::uint64_t>(64u, target - o.r.t)));
  }
  const std::uint32_t seq = o.begin();
  o.r.block(1);  // the command is taken here: the ramp starts at g = 0 on that frame, and the block has no other frame to write
  CHECK(o.r.loop.acked(seq));
  CHECK(o.r.loop.overdubbing());
  CHECK_FALSE(o.r.loop.layer());
  CHECK(o.dirty() == 0u);
  CHECK(o.original());
  o.r.run_to(o.r.t + 2u * 64u, 64);  // the restart comes: the ramp goes on and writes
  NEED(o.run_until([&] { return o.r.loop.layer(); }, 400));
  CHECK(o.dirty() > 0u);
  NEED(o.end());
}

TEST_CASE("loop overdub: FEEDBACK 100 adds, 0 replaces, 99 keeps 99 percent of what is under the layer") {
  for (const unsigned fb : {100u, 99u, 50u, 0u}) {
    Ov o;
    NEED(o.ok);
    o.r.in_const = 3000;
    (void)o.begin(fb);
    o.r.run_to(o.r.t + 200u * 64u, 64);
    NEED(o.end());
    const std::int32_t q16 = fb >= 100u ? 65536 : static_cast<std::int32_t>(fb * 65536u / 100u);
    unsigned checked = 0;
    for (std::uint32_t i = 0; i < o.len; ++i) {
      if (o.r.take[i] != o.orig[i] || fb != 100u) {
        const int want = std::max(-32768, std::min(32767, ((static_cast<std::int32_t>(o.orig[i]) * q16) >> 16) + 3000));
        if (o.r.take[i] == want) {
          ++checked;
        }
      }
    }
    CHECK(checked > 9000u);  // the plateau of 200 blocks, minus the two ramps (a frame outside the session keeps its value: fb 100 only counts the changed ones)
  }
}

TEST_CASE("loop overdub: a quiesce beats an overdub-on that was not yet taken, and a session that never began writes nothing") {
  Ov o;
  NEED(o.ok);
  o.clear_bitmap();
  (void)o.r.loop.overdub_on();
  const std::uint32_t q = o.r.loop.quiesce();  // the latest command replaces the earlier one
  NEED(o.run_until([&] { return o.r.loop.acked(q); }, 10));
  o.r.run_to(o.r.t + 20u * 64u, 64);
  CHECK_FALSE(o.r.loop.overdubbing());
  CHECK_FALSE(o.r.loop.layer());
  CHECK(o.original());
  CHECK(o.dirty() == 0u);
}

TEST_CASE("loop overdub: saturation at both rails") {
  Ov o;
  NEED(o.ok);
  o.r.in_const = 32767;
  (void)o.begin();
  o.r.run_to(o.r.t + 200u * 64u, 64);
  NEED(o.end());
  unsigned at_rail = 0;
  for (std::uint32_t i = 0; i < o.len; ++i) {
    at_rail += o.r.take[i] == 32767 ? 1u : 0u;
    CHECK(o.r.take[i] >= o.orig[i]);  // a positive layer never lowers a frame (no wrap-around)
  }
  CHECK(at_rail > 10000u);
}

TEST_CASE("loop overdub: a layer on a layer saturates at the negative rail") {
  Ov o;
  NEED(o.ok);
  o.r.in_const = -32768;
  (void)o.begin();
  o.r.run_to(o.r.t + 1500u * 64u, 64);  // more than a pass of the loop: the frames the session wrote first are written again
  NEED(o.end());
  bool rail = false;
  for (std::uint32_t i = 0; i < o.len; ++i) {
    rail = rail || o.r.take[i] == -32768;
    CHECK(o.r.take[i] >= -32768);
  }
  CHECK(rail);
}

TEST_CASE("loop overdub: a Stop keeps the session, nothing is written while stopped, and the write resumes on the first bar after Play") {
  Ov o;
  NEED(o.ok);
  o.r.run_to(o.r.t + 100u * 64u, 64);
  (void)o.begin();
  o.r.run_to(o.r.t + 40u * 64u, 64);
  CHECK(o.r.loop.layer());
  o.r.s->stop();
  o.r.run_to(o.r.t + 16u * 64u, 64);  // the write ramps out with the play head
  CHECK(o.r.loop.overdubbing());     // a pedal keeps overdub armed
  const std::vector<std::int16_t> snap(o.r.take.begin(), o.r.take.end());
  o.r.run_to(o.r.t + 400u * 64u, 64);
  CHECK(std::equal(snap.begin(), snap.end(), o.r.take.begin()));  // not a frame written while the transport is stopped
  CHECK(o.r.loop.overdubbing());
  o.r.s->play_pattern(1);
  o.r.run_to(o.r.t + 100u * 64u, 64);
  CHECK_FALSE(std::equal(snap.begin(), snap.end(), o.r.take.begin()));  // and written again, from the loop's start
  NEED(o.end());
}

TEST_CASE("loop overdub: SAVE and DISCARD during a session or a restore: nothing is written once the phase says Idle, nor in the block that releases") {
  for (int variant = 0; variant < 4; ++variant) {
    Ov o;
    NEED(o.ok);
    o.r.run_to(o.r.t + 50u * 64u, 64);
    (void)o.begin();
    o.r.run_to(o.r.t + 200u * 64u, 64);
    if (variant >= 2) {  // a restore running
      (void)o.r.loop.undo();
      o.r.run_to(o.r.t + 10u * 64u, 64);
      CHECK(o.r.loop.restoring());
    }
    if (variant == 1 || variant == 3) {  // SAVE: quiesce, wait for the ack, then release
      const std::uint32_t q = o.r.loop.quiesce();
      if (variant == 1) {
        NEED(o.run_until([&] { return o.r.loop.acked(q); }, 50));
        CHECK_FALSE(o.r.loop.overdubbing());
      }
    }
    const std::vector<std::int16_t> before(o.r.take.begin(), o.r.take.end());  // the memory as the main loop posts the release
    o.r.loop.release();
    // Commands still pending when the loop goes (an overdub-on, an undo) change nothing either.
    (void)o.r.loop.overdub_on();
    NEED(o.run_until([&] { return o.r.loop.phase() == LoopPlayer::Phase::Idle; }, 20));
    CHECK(std::equal(before.begin(), before.end(), o.r.take.begin()));  // the block that released wrote nothing
    (void)o.r.loop.undo();
    for (int i = 0; i < 8; ++i) {
      o.r.block(64);
    }
    CHECK(o.r.loop.phase() == LoopPlayer::Phase::Idle);
    CHECK_FALSE(o.r.loop.overdubbing());
    CHECK_FALSE(o.r.loop.restoring());
    CHECK(std::equal(before.begin(), before.end(), o.r.take.begin()));  // the sentinel: bit for bit, loop, undo buffer and bitmap
  }
}

TEST_CASE("loop overdub: UNDO ends the session, fades the loop out, puts every marked block back bit for bit and brings it back on a bar; a second UNDO does nothing") {
  Ov o;
  NEED(o.ok);
  o.r.run_to(o.r.t + 50u * 64u, 64);
  (void)o.begin();
  o.r.run_to(o.r.t + 1500u * 64u, 64);  // more than a pass: every block is touched
  CHECK(o.r.loop.layer());
  CHECK_FALSE(o.original());
  const std::uint32_t u = o.r.loop.undo();  // while the session is still on: punch out, then restore
  NEED(o.run_until([&] { return o.r.loop.acked(u); }, 50));
  CHECK_FALSE(o.r.loop.overdubbing());
  CHECK(o.r.loop.restoring());
  NEED(o.run_until([&] { return !o.r.loop.restoring(); }, 4000));
  CHECK(o.original());
  CHECK(o.dirty() == 0u);
  CHECK_FALSE(o.r.loop.layer());
  // the loop comes back on a bar, ramped in, playing the original
  const std::size_t from = o.r.steps.size();
  o.r.run_to(o.r.t + 2u * o.len, 64);
  std::size_t bar = from;
  while (bar < o.r.steps.size() && (o.r.steps[bar].value & 15u) != 0u) {
    ++bar;
  }
  NEED(bar + 16u < o.r.steps.size());
  const std::uint64_t at = o.r.steps[bar].at;
  CHECK(o.r.heard[at] == 0);
  CHECK(o.r.heard[at + LoopPlayer::kFadeFrames + 3u] == o.orig[LoopPlayer::kFadeFrames + 3u]);
  // a second UNDO: no layer, nothing happens
  const std::uint32_t u2 = o.r.loop.undo();
  NEED(o.run_until([&] { return o.r.loop.acked(u2); }, 10));
  o.r.run_to(o.r.t + 20u * 64u, 64);
  CHECK_FALSE(o.r.loop.restoring());
  CHECK(o.original());
  // a new session drops the old undo: no layer until its first copy (a stopped transport writes nothing)
  o.r.s->stop();
  o.r.run_to(o.r.t + 30u * 64u, 64);
  (void)o.begin();
  o.r.run_to(o.r.t + 30u * 64u, 64);
  CHECK(o.r.loop.overdubbing());
  CHECK_FALSE(o.r.loop.layer());
  o.r.s->play_pattern(1);
  NEED(o.run_until([&] { return o.r.loop.layer(); }, 3000));
  NEED(o.end());
}

TEST_CASE("loop overdub: the restore is bounded: never more than kRestoreStepCopies blocks a render block, and a heavy block skips its step but never more than kRestoreForceAfter in a row") {
  for (const std::uint32_t load : {0u, LoopPlayer::kRestoreSkipCycles + 1u}) {
    Ov o;
    NEED(o.ok);
    g_fake_block_cycles = load;
    o.r.cyc = &fake_block_cycles;
    (void)o.begin();
    o.r.run_to(o.r.t + 1500u * 64u, 64);
    NEED(o.end());
    const unsigned dirty0 = o.dirty();
    CHECK(dirty0 > 1000u);
    (void)o.r.loop.undo();
    unsigned blocks = 0;
    unsigned run = 0;
    unsigned worst_run = 0;
    unsigned prev = dirty0;
    o.r.block(64);
    CHECK(o.r.loop.restoring());
    prev = o.dirty();
    while (o.r.loop.restoring() && blocks < 20000u) {
      o.r.block(64);
      ++blocks;
      const unsigned now = o.dirty();
      CHECK(prev - now <= loop_ov::kRestoreStepCopies);
      run = now == prev ? run + 1u : 0u;
      worst_run = std::max(worst_run, run);
      prev = now;
    }
    CHECK(blocks < 20000u);  // the restore ends whatever the load
    CHECK(o.original());
    if (load == 0u) {
      CHECK(blocks <= dirty0 / loop_ov::kRestoreStepCopies + 20u);
    } else {
      CHECK(worst_run <= LoopPlayer::kRestoreForceAfter + 2u);   // at least one step every kRestoreForceAfter + 1 blocks
      CHECK(blocks > dirty0 / loop_ov::kRestoreStepCopies);      // and it did skip
    }
  }
}

TEST_CASE("loop overdub: an export parks the player mid-session and mid-restore: flags cleared, nothing written, the layer kept and UNDO still undoes it") {
  for (int restoring = 0; restoring < 2; ++restoring) {
    Ov o;
    NEED(o.ok);
    (void)o.begin();
    o.r.run_to(o.r.t + 300u * 64u, 64);
    CHECK(o.r.loop.layer());
    if (restoring != 0) {
      (void)o.r.loop.undo();
      o.r.run_to(o.r.t + 20u * 64u, 64);
      CHECK(o.r.loop.restoring());
    }
    o.r.loop.park(true);
    CHECK_FALSE(o.r.loop.overdubbing());
    CHECK_FALSE(o.r.loop.restoring());
    CHECK(o.r.loop.layer());
    const std::vector<std::int16_t> snap(o.r.take.begin(), o.r.take.end());
    o.r.run_to(o.r.t + 200u * 64u, 64);  // (parked: needs_service() is false: the harness does not call it)
    CHECK(std::equal(snap.begin(), snap.end(), o.r.take.begin()));
    o.r.loop.park(false);
    const std::uint32_t u = o.r.loop.undo();
    NEED(o.run_until([&] { return o.r.loop.acked(u); }, 50));
    NEED(o.run_until([&] { return !o.r.loop.restoring(); }, 4000));
    CHECK(o.original());
  }
}

TEST_CASE("loop overdub: an odd length, a tempo cut while the write head runs, and a layer across the wrap never write beyond the loop") {
  // find a tempo whose bar is an odd number of frames
  std::uint32_t tempo = 0;
  for (std::uint32_t t = 12000; t < 12080 && tempo == 0u; ++t) {
    Ov probe(1, t);
    if (probe.ok && (probe.len & 1u) != 0u) {
      tempo = t;
    }
  }
  NEED(tempo != 0u);
  Ov o(1, tempo);
  NEED(o.ok);
  CHECK((o.len & 1u) != 0u);
  // a canary behind the bitmap and between the loop and its undo buffer (the odd frame)
  const std::uint32_t canary_at = 2u * o.even() + ((o.bitmap_halfwords() + 1u) & ~1u);
  for (std::uint32_t i = 0; i < 16u; ++i) {
    o.r.take[canary_at + i] = 0x5A5A;
  }
  (void)o.begin();
  o.r.run_to(o.r.t + 1200u * 64u, 64);                                                // a layer across the wrap of the loop
  CHECK(o.r.s->apply(core::ParamChange{seq::tempo_param(), 170.0f, 1}));              // faster: the loop restarts early
  o.r.run_to(o.r.t + 1200u * 64u, 64);
  CHECK(o.r.s->apply(core::ParamChange{seq::tempo_param(), 80.0f, 1}));               // slower: a gap at its end
  o.r.run_to(o.r.t + 2400u * 64u, 64);
  NEED(o.end());
  for (std::uint32_t i = 0; i < 16u; ++i) {
    CHECK(o.r.take[canary_at + i] == 0x5A5A);
  }
  const std::uint32_t u = o.r.loop.undo();
  NEED(o.run_until([&] { return o.r.loop.acked(u); }, 50));
  NEED(o.run_until([&] { return !o.r.loop.restoring(); }, 6000));
  CHECK(o.original());  // the undo buffer was never overwritten from the loop's side: the restore is bit for bit, odd last block included
}

TEST_CASE("loop overdub: a take of exactly the capacity is a loop when its end step is the first sample of the next block, and one that gets no step there is not") {
  alignas(4) std::int16_t buf[1024] = {};
  alignas(4) std::int16_t l[64];
  alignas(4) std::int16_t out[128] = {};
  for (std::uint32_t i = 0; i < 64; ++i) {
    l[i] = 500;
  }
  for (const bool step_next : {true, false}) {
    RecTap tap;
    LoopPlayer loop;
    loop.capture(1);
    loop.service(out, 64, false, kUnity, tap);
    NEED(tap.arm(buf, 256, 1, RecTap::Align::Free, 1));
    for (int i = 0; i < 4; ++i) {  // the capacity, to the frame, at the end of a block
      tap.push(l, l, 64);
    }
    CHECK(tap.frames() == 256u);
    CHECK(tap.state() == RecTap::State::Armed);  // not over: the boundary may be the next block's first sample
    if (step_next) {
      tap.on_step(0, 0);
    }
    tap.push(l, l, 64);
    CHECK(tap.state() == RecTap::State::Done);
    CHECK(tap.frames() == 256u);
    loop.service(out, 64, false, kUnity, tap);
    CHECK(loop.reading() == step_next);
    CHECK(tap.overrun_frames() == (step_next ? 0u : 64u));
  }
}

TEST_CASE("loop overdub: a long loop (a bitmap of more than one restore step) with two layers far apart is restored bit for bit, the clean stretch between them scanned over several blocks") {
  Ov o(3, 12000, 600000);
  NEED(o.ok);
  CHECK(o.len > 250000u);
  CHECK(o.bitmap_halfwords() > 2u * loop_ov::kRestoreStepWords);
  (void)o.begin();
  o.r.run_to(o.r.t + 200u * 64u, 64);
  NEED(o.end());
  const unsigned first = o.dirty();
  CHECK(first > 100u);
  o.r.run_to(o.r.t + 2500u * 64u, 64);  // far from the first layer
  (void)o.r.loop.overdub_on();           // (posted without the main loop's clear: the first layer's bits stay and this session adds to them, so one UNDO puts both back)
  o.r.run_to(o.r.t + 200u * 64u, 64);
  NEED(o.end());
  CHECK(o.dirty() > first);
  const std::uint32_t u = o.r.loop.undo();
  NEED(o.run_until([&] { return o.r.loop.acked(u); }, 50));
  unsigned blocks = 0;
  while (o.r.loop.restoring() && blocks < 20000u) {
    o.r.block(64);
    ++blocks;
  }
  CHECK(blocks < 20000u);
  CHECK(o.dirty() == 0u);
  CHECK(o.original());
}
