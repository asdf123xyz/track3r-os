// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-PERFFX P3: the whole road of a DELAY or REVERB slot, with nothing hand-built. The Perform bank's pad and encoder (seq/perform.h) push Commands onto the real Sequencer's ring, the
// sequencer's control table turns them into the ModSet events of the performance layer, the Engine hands those to the tracker FX processor and the processor to the strip's sends
// (Graph::set_perf_send); the step FX 30 and 32 of a real pattern go the same road (Graph::set_step_send). audio_perform_test holds the strips against hand-built SeqEvents; this
// holds that the sequencer and the bank speak the events the strips expect (the groups, the layer, the value). The sends are measured where the send effects are fed: two stand-in
// effects keep the peak of their input block, and the note is a looping DC, so "the same level" is "the same peak, to the sample". Host only (the sequencer's project is 1.9 MB).
#include <doctest/doctest.h>

#include <o3os/audio/engine.h>
#include <o3os/audio/voices.h>
#include <o3os/hal/audio.h>
#include <o3os/seq/engine_link.h>
#include <o3os/seq/fx.h>
#include <o3os/seq/perform.h>
#include <o3os/seq/sequencer.h>

#include "support/need.h"
#include "support/seq_fixture.h"
#include "support/static_slot.h"
#include "support/zero_bank.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace {

using namespace o3os;
namespace haudio = o3os::hal::audio;
using seq::PerfFx;

std::uint32_t g_cycles = 0;
std::uint32_t fake_cycles() noexcept { return g_cycles += 5u; }

std::int16_t g_dc[256];

audio::SampleDesc dc_desc(unsigned delay_send, unsigned reverb_send) {
  for (std::int16_t& v : g_dc) {
    v = 16384;
  }
  audio::SampleDesc d{g_dc, 256, 0, 256, 65536};
  d.delay_send = static_cast<std::uint8_t>(delay_send);
  d.reverb_send = static_cast<std::uint8_t>(reverb_send);
  return d;
}

// OS-PERFFX P5: a looping tone (one period over the 256 samples), so that two renders of the same pitch are the same samples and two pitches are not.
std::int16_t g_tone[256];

audio::SampleDesc tone_desc() {
  for (unsigned i = 0; i < 256u; ++i) {
    g_tone[i] = static_cast<std::int16_t>(std::lround(12000.0 * std::sin(2.0 * 3.14159265358979 * static_cast<double>(i) / 256.0)));
  }
  return audio::SampleDesc{g_tone, 256, 0, 256, 65536};
}

struct Tap {
  std::int32_t peak = 0;  // the largest |sample| of the last block the effect was given
};
Tap g_tap[2];

void tap_fx(void* state, const std::int16_t* in, std::int16_t* out_l, std::int16_t* out_r, std::uint32_t n) noexcept {
  std::int32_t m = 0;
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::int32_t v = in[i] < 0 ? -static_cast<std::int32_t>(in[i]) : in[i];
    m = v > m ? v : m;
    out_l[i] = in[i];
    out_r[i] = in[i];
  }
  static_cast<Tap*>(state)->peak = m;
}

struct Peaks {
  std::int32_t delay = 0;
  std::int32_t reverb = 0;
};

// What the bank's commands reach: the sequencer's ring.
struct Sink final : seq::CommandSink {
  seq::Sequencer* s = nullptr;
  bool push(const seq::Command& c) noexcept override { return s->commands().push(c); }
};

constexpr unsigned pad(unsigned slot, unsigned row) { return row * seq::kPerfSlots + slot; }

// The rig: pattern 1 has a note on track 0 at its first step (sample instrument 1, slot 0, whose sends are given), with the step FX given in its FX columns (0: none); the sequencer plays it into the
// engine through the real event source. A bar is 1,378 blocks, the measures below stay inside the first one (no second note start).
struct Rig {
  Rig(unsigned delay_send, unsigned reverb_send, std::uint8_t fx0 = 0, std::uint8_t v0 = 0, std::uint8_t fx1 = 0, std::uint8_t v1 = 0, std::uint8_t note = 60, bool tone = false, const audio::SampleDesc* custom = nullptr) : engine(engine_slot.make(&fake_cycles, taps)), pool(pool_slot.make()) {
    test::fresh_project(4, 1);
    model::Project& project = test::seq_project();
    NEED(project.create_pattern(1, 16) == model::Status::Ok);
    NEED(project.append_order(1) == model::Status::Ok);
    NEED(project.set_step(1, 0, 0, test::make_step(note, 1, fx0, v0, fx1, v1)) == model::Status::Ok);
    sequencer = &seq_slot.make(project);
    pool.set_sample(0, custom != nullptr ? *custom : tone ? tone_desc() : dc_desc(delay_send, reverb_send));
    engine.attach_voices(&pool);
    engine.attach_events(seq::event_source(*sequencer, events, audio::kMaxEventsPerBlock));
    g_tap[0] = Tap{};
    g_tap[1] = Tap{};
    engine.graph().install_send_fx(0, &tap_fx, &g_tap[0]);
    engine.graph().set_send_fx_bypass(0, false);
    engine.graph().install_send_fx(1, &tap_fx, &g_tap[1]);
    engine.graph().set_send_fx_bypass(1, false);
    sink.s = sequencer;
    NEED(sequencer->commands().push(seq::Command{seq::Command::Kind::PlayPattern, 1, 0, 0}));
  }
  void block() {
    haudio::BlockReport report{};
    const haudio::BlockIo io{in, out, 0, &report};
    engine.render_block(io);
  }
  // A dozen blocks for the smoothers, then the peaks of one more.
  Peaks level() {
    for (unsigned b = 0; b < 12u; ++b) {
      block();
    }
    g_tap[0].peak = 0;
    g_tap[1].peak = 0;
    block();
    return Peaks{g_tap[0].peak, g_tap[1].peak};
  }
  audio::Graph& graph() { return engine.graph(); }

  static inline test::StaticSlot<audio::Engine> engine_slot;
  static inline test::StaticSlot<audio::VoicePool> pool_slot;
  static inline test::StaticSlot<seq::Sequencer> seq_slot;
  static inline audio::TapMem taps;
  static inline audio::SeqEvent events[audio::kMaxEventsPerBlock];
  alignas(8) static inline std::int16_t in[haudio::kBlockSamples] = {};
  alignas(8) static inline std::int16_t out[haudio::kBlockSamples] = {};
  audio::Engine& engine;
  audio::VoicePool& pool;
  seq::Sequencer* sequencer = nullptr;
  Sink sink;
};

// The peaks of the note when the instrument's own sends are `delay` and `reverb` and nothing else is held (a rig remakes the engine: the references come first).
Peaks reference(unsigned delay, unsigned reverb) {
  Rig r(delay, reverb);
  return r.level();
}

}  // namespace

TEST_CASE("perform layer (P3): a DELAY or REVERB slot held on the Perform bank reaches the track's send through the sequencer, as an offset of the instrument's, and its release gives the send back") {
  const Peaks base = reference(40, 40);
  const Peaks p10 = reference(10, 10);
  const Peaks p70 = reference(70, 70);
  const Peaks p100 = reference(100, 100);
  NEED(base.delay > 0);
  NEED(base.reverb > 0);
  NEED(p10.delay < base.delay);
  NEED(base.delay < p70.delay);
  NEED(p70.delay < p100.delay);
  for (const bool reverb : {false, true}) {
    CAPTURE(reverb);
    const auto got = [&](const Peaks& p) { return reverb ? p.reverb : p.delay; };
    const auto other = [&](const Peaks& p) { return reverb ? p.delay : p.reverb; };
    const unsigned slot = reverb ? 7u : 6u;
    const unsigned which = reverb ? 1u : 0u;
    Rig r(40, 40);
    test::ZeroBank bank;
    NEED(bank.fx(slot) == (reverb ? PerfFx::ReverbSend : PerfFx::DelaySend));
    CHECK(bank.toggle_track(0, r.sink));  // track 0 is affected (nothing is sent: no FX is held yet)
    CHECK(got(r.level()) == got(base));
    CHECK(bank.pad_down(pad(slot, 1), r.sink));  // the slot's row 1 held, its stored value 0: no FX yet
    CHECK(got(r.level()) == got(base));
    CHECK(bank.turn(+30, r.sink));
    CHECK(bank.live(slot) == 30);
    Peaks now = r.level();
    CHECK(got(now) == got(p70));  // 40 + 30
    CHECK(other(now) == other(base));  // each slot moves its own send only
    CHECK(r.graph().perf_send(0, which) == 30);
    CHECK(r.graph().perf_send(0, 1u - which) == 0);
    CHECK(r.graph().perf_send(1, which) == 0);  // an unaffected track is left alone
    CHECK(bank.turn(+70, r.sink));
    CHECK(got(r.level()) == got(p100));  // +100 opens the send whole
    CHECK(bank.turn(-130, r.sink));
    CHECK(bank.live(slot) == -30);
    CHECK(got(r.level()) == got(p10));  // 40 - 30
    CHECK(bank.turn(-70, r.sink));
    now = r.level();
    CHECK(got(now) == 0);  // -100 closes it
    CHECK(other(now) == other(base));
    CHECK(r.graph().perf_send(0, which) == -100);
    CHECK(bank.pad_up(pad(slot, 1), r.sink));  // LATCH is on but the live value is not stored (not EDIT): the FX ends with the pad
    now = r.level();
    CHECK(got(now) == got(base));
    CHECK(r.graph().perf_send(0, which) == 0);
  }
}

TEST_CASE("perform layer (P3): both slots held at once, and the step FX 30 and 32 of a real pattern, which the slot adds to and the slot's release gives back") {
  const Peaks p20 = reference(20, 20);
  const Peaks p40 = reference(40, 40);
  const Peaks p60 = reference(60, 60);
  const Peaks p80 = reference(80, 80);
  NEED(p20.delay < p60.delay);
  NEED(p60.delay < p80.delay);
  NEED(p20.reverb < p40.reverb);
  {
    // the step FX in the pattern: 30 (Delay send) at 60 and 32 (Reverb send) at 20 on an instrument whose sends are 40: the track sends the FX's, not the instrument's
    Rig r(40, 40, seq::fx::kDelaySend, 60, seq::fx::kReverbSend, 20);
    const Peaks fx = r.level();
    CHECK(fx.delay == p60.delay);
    CHECK(fx.reverb == p20.reverb);
    CHECK(r.graph().step_send(0, 0) == 60);
    CHECK(r.graph().step_send(0, 1) == 20);
    test::ZeroBank bank;
    NEED(bank.fx(6) == PerfFx::DelaySend);
    NEED(bank.fx(7) == PerfFx::ReverbSend);
    CHECK(bank.toggle_track(0, r.sink));
    // both slots held, the encoder at +20 (it moves every held slot): DELAY over the FX's 60, REVERB over the FX's 20
    CHECK(bank.pad_down(pad(6, 1), r.sink));
    CHECK(bank.pad_down(pad(7, 1), r.sink));
    CHECK(bank.turn(+20, r.sink));
    const Peaks both = r.level();
    CHECK(both.delay == p80.delay);   // 60 + 20
    CHECK(both.reverb == p40.reverb);  // 20 + 20
    CHECK(r.graph().perf_send(0, 0) == 20);
    CHECK(r.graph().perf_send(0, 1) == 20);
    // the slots let go: the FX's sends are back, the pattern's FX still held
    CHECK(bank.pad_up(pad(6, 1), r.sink));
    CHECK(bank.pad_up(pad(7, 1), r.sink));
    const Peaks back = r.level();
    CHECK(back.delay == p60.delay);
    CHECK(back.reverb == p20.reverb);
    CHECK(r.graph().step_send(0, 0) == 60);
    CHECK(r.graph().perf_send(0, 0) == 0);
    CHECK(r.graph().perf_send(0, 1) == 0);
  }
}

namespace {

// `blocks` blocks of the master mix of the tone at `note` on track 0, with the Perform bank's TUNE slot (slot 2) held at `tune` on the track before the first block (0: not held).
void tone_blocks(std::uint8_t note, int tune, unsigned blocks, std::vector<std::int16_t>& mix) {
  Rig r(0, 0, 0, 0, 0, 0, note, true);
  test::ZeroBank bank;
  if (tune != 0) {
    NEED(bank.fx(2) == PerfFx::Tune);
    CHECK(bank.toggle_track(0, r.sink));
    CHECK(bank.pad_down(pad(2, 1), r.sink));
    CHECK(bank.turn(tune, r.sink));
    CHECK(bank.live(2) == tune);
  }
  mix.clear();
  for (unsigned b = 0; b < blocks; ++b) {
    r.block();
    mix.insert(mix.end(), Rig::out, Rig::out + haudio::kBlockSamples);
  }
  if (tune != 0) {
    CHECK(r.pool.perf_held(0, audio::kModGroupTune));
    CHECK(r.pool.perf_value(0, audio::kModGroupTune) == tune);
    CHECK_FALSE(r.pool.perf_held(1, audio::kModGroupTune));  // an unaffected track
    CHECK(bank.pad_up(pad(2, 1), r.sink));  // LATCH is on but the live value is not stored (not EDIT): the FX ends with the pad
    r.block();
    r.block();
    CHECK_FALSE(r.pool.perf_held(0, audio::kModGroupTune));
  }
}

}  // namespace

TEST_CASE("perform layer (P5): a TUNE slot held on the Perform bank moves the track's note by that many semitones through the sequencer, bit for bit, and its release lets it go") {
  std::vector<std::int16_t> plain60;
  tone_blocks(60, 0, 24, plain60);
  NEED(plain60.size() == 24u * haudio::kBlockSamples);
  std::int32_t peak = 0;
  for (const std::int16_t v : plain60) {
    peak = std::max<std::int32_t>(peak, v < 0 ? -v : v);
  }
  CHECK(peak > 500);  // the note sounded
  for (const int semitones : {12, 7, -12, -5, 48, -48}) {
    CAPTURE(semitones);
    std::vector<std::int16_t> held;
    std::vector<std::int16_t> moved;
    tone_blocks(60, semitones, 24, held);
    tone_blocks(static_cast<std::uint8_t>(60 + semitones), 0, 24, moved);
    CHECK(held == moved);  // TUNE s on a note is the note s semitones away
    CHECK(held != plain60);
  }
}

// OS-PERFFX P7: SMP POS (slot 8) and SMP END (slot 9) of the real bank reach the voices through the sequencer's ring, the control table and the Engine (the groups Position and SampleEnd), are held per
// track (an unaffected track has none) and let go with the pad.
TEST_CASE("perform layer (P7): SMP POS and SMP END slots held on the Perform bank reach the voices' slots through the sequencer, and their release lets them go") {
  Rig r(0, 0, 0, 0, 0, 0, 60, true);
  test::ZeroBank bank;
  NEED(bank.fx(8) == PerfFx::SamplePosition);
  NEED(bank.fx(9) == PerfFx::SampleEnd);
  CHECK(bank.toggle_track(0, r.sink));
  CHECK(bank.pad_down(pad(8, 1), r.sink));
  CHECK(bank.turn(40, r.sink));
  CHECK(bank.pad_down(pad(9, 1), r.sink));
  CHECK(bank.turn(-25, r.sink));
  r.block();
  r.block();
  CHECK(r.pool.perf_held(0, audio::kModGroupPosition));
  CHECK(r.pool.perf_value(0, audio::kModGroupPosition) == 15);  // the encoder turns every held slot: 40, then -25 more while SMP END is held too
  CHECK(r.pool.perf_held(0, audio::kModGroupSampleEnd));
  CHECK(r.pool.perf_value(0, audio::kModGroupSampleEnd) == -25);
  CHECK_FALSE(r.pool.perf_held(1, audio::kModGroupPosition));  // an unaffected track
  CHECK(bank.pad_up(pad(8, 1), r.sink));
  CHECK(bank.pad_up(pad(9, 1), r.sink));
  r.block();
  r.block();
  CHECK_FALSE(r.pool.perf_held(0, audio::kModGroupPosition));
  CHECK_FALSE(r.pool.perf_held(0, audio::kModGroupSampleEnd));
}

namespace {

// OS-PERF-REVERSE (P8): a one shot ramp of a different value in nearly every frame, and the same frames reversed. Note 60 is the whole-frame step, so a backward note of the first is bit for bit a forward note of the second.
constexpr std::uint32_t kRampLen = 2000;  // 31 blocks of 64 frames
std::int16_t g_ramp[kRampLen];
std::int16_t g_back[kRampLen];

void make_ramps() {
  for (std::uint32_t i = 0; i < kRampLen; ++i) {
    g_ramp[i] = static_cast<std::int16_t>(static_cast<int>((i * 37u) % 20011u) - 10000);
  }
  for (std::uint32_t i = 0; i < kRampLen; ++i) {
    g_back[i] = g_ramp[kRampLen - 1u - i];
  }
}

std::int32_t peak_of(const std::vector<std::int16_t>& v) {
  std::int32_t m = 0;
  for (const std::int16_t x : v) {
    m = std::max<std::int32_t>(m, x < 0 ? -x : x);
  }
  return m;
}

void append_blocks(Rig& r, unsigned blocks, std::vector<std::int16_t>& mix) {
  for (unsigned b = 0; b < blocks; ++b) {
    r.block();
    mix.insert(mix.end(), Rig::out, Rig::out + haudio::kBlockSamples);
  }
}

}  // namespace

// P8: the step FX 26 of a real pattern step reaches the voices through the sequencer and the Engine: the note is the reversed frames played forward, to the sample, and FX 26 = 0 is forward.
TEST_CASE("perform layer (P8): a pattern step with the FX 26 plays its note backward through the sequencer and the Engine, bit for bit the reversed frames forward") {
  make_ramps();
  const audio::SampleDesc ramp{g_ramp, kRampLen, 0, 0, 65536};
  const audio::SampleDesc back{g_back, kRampLen, 0, 0, 65536};
  std::vector<std::int16_t> fwd;
  std::vector<std::int16_t> rev;
  std::vector<std::int16_t> ref;
  std::vector<std::int16_t> zero;
  {
    Rig r(0, 0, 0, 0, 0, 0, 60, false, &ramp);
    append_blocks(r, 40, fwd);
  }
  {
    Rig r(0, 0, seq::fx::kReverse, 1, 0, 0, 60, false, &ramp);
    append_blocks(r, 40, rev);
    CHECK(r.pool.step_reverse(0));
    CHECK_FALSE(r.pool.step_reverse(1));
  }
  {
    Rig r(0, 0, 0, 0, 0, 0, 60, false, &back);
    append_blocks(r, 40, ref);
  }
  {
    Rig r(0, 0, seq::fx::kReverse, 0, 0, 0, 60, false, &ramp);
    append_blocks(r, 40, zero);
    CHECK_FALSE(r.pool.step_reverse(0));
  }
  NEED(peak_of(fwd) > 500);
  NEED(peak_of(ref) > 500);
  CHECK(rev == ref);
  CHECK(rev != fwd);
  CHECK(zero == fwd);
}

// P8: like the slide FX the step FX 26 is held through Stop (a live pad note after it is still backward) and ends at the next fresh Play (FxReset), whose pattern may no longer carry it.
TEST_CASE("perform layer (P8): a held step FX 26 survives Stop, a note after it is backward, and the next Play (FxReset) ends it") {
  make_ramps();
  const audio::SampleDesc ramp{g_ramp, kRampLen, 0, 0, 65536};
  const audio::SampleDesc back{g_back, kRampLen, 0, 0, 65536};
  const auto after_stop = [&](bool fx, std::vector<std::int16_t>& live) {
    Rig r(0, 0, fx ? seq::fx::kReverse : 0, fx ? 1 : 0, 0, 0, 60, false, fx ? &ramp : &back);
    std::vector<std::int16_t> first;
    append_blocks(r, 2, first);
    NEED(r.sequencer->commands().push(seq::Command{seq::Command::Kind::Stop, 0, 0, 0}));
    append_blocks(r, 40, first);  // the pattern's note ends on its own
    NEED(!r.pool.track_playing(0));
    CHECK(r.pool.step_reverse(0) == fx);  // Stopped does not end the step FX
    NEED(r.pool.note_on(0, 60, 0, 127));  // a live pad note
    append_blocks(r, 40, live);
    if (fx) {
      // the next fresh Play, of a pattern whose step no longer has the FX: FxReset ends it
      NEED(test::seq_project().set_step(1, 0, 0, test::make_step(60, 1, 0, 0, 0, 0)) == model::Status::Ok);
      NEED(r.sequencer->commands().push(seq::Command{seq::Command::Kind::PlayPattern, 1, 0, 0}));
      append_blocks(r, 2, first);
      CHECK_FALSE(r.pool.step_reverse(0));
    }
  };
  std::vector<std::int16_t> rev_live;
  std::vector<std::int16_t> ref_live;
  after_stop(true, rev_live);
  after_stop(false, ref_live);
  NEED(peak_of(ref_live) > 500);
  CHECK(rev_live == ref_live);  // the live note is the reversed frames forward, as the pattern's own was
}
