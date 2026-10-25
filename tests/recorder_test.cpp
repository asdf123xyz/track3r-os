// SPDX-License-Identifier: GPL-3.0-or-later
//
// The sample recorder (app/recorder.h, OS-011c): the plan it tells the engine for every source and tap, the take's life (arm, aligned start, record, stop, review, trim, save to a
// slot, the take lost to a project load, a full arena), the hardware calls it makes (source, gain, the settle after a switch, the radio), and what it gives back when it lets go
// of the engine's input path. Through the real engine, graph and sample bank, a fake codec/radio and a scripted sequencer. Pure logic: host only.
#include <doctest/doctest.h>

#include "support/static_slot.h"

#include <o3os/app/mix_board.h>
#include <o3os/app/recorder.h>
#include <o3os/app/sample_bank.h>
#include <o3os/audio/engine.h>
#include <o3os/audio/event_source.h>
#include <o3os/audio/voices.h>
#include <o3os/hal/capture.h>
#include <o3os/model/instrument.h>
#include <o3os/model/project.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace {

namespace ha = o3os::hal::audio;
namespace cap = o3os::hal::capture;
namespace gps = o3os::audio::graph_param::strip;
using namespace o3os;
using namespace o3os::app;
using audio::RecTap;
namespace route = o3os::audio::route;

constexpr std::uint32_t kB = ha::kBlock;

std::uint32_t g_cycles = 0;
std::uint32_t fake_cycles() noexcept { return ++g_cycles; }

// A fake codec and radio: what was asked, in order.
class FakeHw final : public cap::CaptureHw {
 public:
  bool select_source(cap::Source s, std::uint8_t g) noexcept override {
    ++selects;
    last_source = s;
    last_gain = g;
    return ok;
  }
  bool set_gain(cap::Source s, std::uint8_t g) noexcept override {
    ++gains;
    last_source = s;
    last_gain = g;
    return ok;
  }
  void radio_power(bool on) noexcept override { powered = on; }
  void radio_region(cap::Region r) noexcept override { region = r; ++regions; }
  void radio_tune(std::uint16_t f) noexcept override { tuned = f; ++tunes; }
  void radio_seek(bool up) noexcept override { seeks += up ? 1 : -1; }
  void service(std::uint64_t) noexcept override {}
  cap::RadioStatus radio_status() const noexcept override { return status; }

  bool ok = true;
  unsigned selects = 0;
  unsigned gains = 0;
  cap::Source last_source = cap::Source::Count;
  std::uint8_t last_gain = 255;
  bool powered = false;
  cap::Region region = cap::Region::Europe;
  unsigned regions = 0;
  std::uint16_t tuned = 0;
  unsigned tunes = 0;
  int seeks = 0;
  cap::RadioStatus status;
};

// A scripted sequencer: a Step event at `block`, `offset`, with `value`; and (OS-REC2) any further events at absolute samples (g_script, in time order, at most one a block).
std::uint32_t g_step_block = 0xFFFFFFFFu;
std::uint32_t g_step_offset = 0;
std::int32_t g_step_value = 0;
struct ScriptEv {
  std::uint64_t at;
  audio::EventKind kind;
  std::int32_t value;
};
std::vector<ScriptEv> g_script;
std::uint32_t scripted(void*, std::uint64_t block_start, std::uint32_t n, audio::SeqEvent* out, std::uint32_t cap_events) noexcept {
  std::uint32_t count = 0;
  if (static_cast<std::uint32_t>(block_start / kB) == g_step_block && cap_events >= 1u) {
    audio::SeqEvent step{};
    step.kind = audio::EventKind::Step;
    step.track = audio::kNoTrack;
    step.offset = static_cast<std::uint16_t>(g_step_offset);
    step.value = g_step_value;
    out[count++] = step;
  }
  for (const ScriptEv& e : g_script) {
    if (e.at >= block_start && e.at < block_start + n && count < cap_events) {
      audio::SeqEvent ev{};
      ev.kind = e.kind;
      ev.track = audio::kNoTrack;
      ev.offset = static_cast<std::uint16_t>(e.at - block_start);
      ev.value = e.value;
      out[count++] = ev;
    }
  }
  return count;
}

struct Rig {
  audio::Engine& e;
  std::unique_ptr<model::ModelArena> project = std::make_unique<model::ModelArena>();
  std::vector<std::int16_t> memory;
  std::unique_ptr<app::SampleBankMem> bank_mem = std::make_unique<app::SampleBankMem>();
  SampleBank bank;
  std::unique_ptr<MixBoard> mix = std::make_unique<MixBoard>();
  FakeHw hw;
  Recorder rec;
  alignas(8) std::int16_t in[ha::kBlockSamples] = {};
  alignas(8) std::int16_t out[ha::kBlockSamples] = {};
  ha::BlockReport report = {};
  std::uint64_t now = 1000;
  std::uint32_t block = 0;

  static audio::Engine& make_engine() {
    static o3os::test::StaticSlot<audio::Engine> slot;
    static audio::TapMem taps;
    g_cycles = 0;
    return slot.make(&fake_cycles, taps);
  }
  static audio::VoicePool& make_pool() {
    static o3os::test::StaticSlot<audio::VoicePool> slot;
    return slot.make();
  }

  explicit Rig(std::uint32_t arena_samples = 40000) : e(make_engine()), memory(arena_samples + 8) {
    project->project.reset();
    audio::VoicePool& pool = make_pool();
    e.attach_voices(&pool);
    static audio::SeqEvent events[audio::kMaxEventsPerBlock];
    audio::EventSource src;
    src.step = &scripted;
    src.buffer = events;
    src.capacity = audio::kMaxEventsPerBlock;
    e.attach_events(src);
    g_step_block = 0xFFFFFFFFu;
    g_script.clear();
    bank.init(pool, *project, *bank_mem, memory.data(), arena_samples);
    Recorder::Wiring w;
    w.hw = &hw;
    w.graph = &e.graph();
    w.params = &e.params();
    w.bank = &bank;
    w.mix = mix.get();
    rec.attach(w);
  }

  // The RX block of block `n`: left = 1000 + n * 64 + i, right = -left / 2.
  void rx(std::uint32_t n) {
    for (std::uint32_t i = 0; i < kB; ++i) {
      const int l = 1000 + static_cast<int>(n * kB + i);
      in[2u * i] = static_cast<std::int16_t>(l);
      in[2u * i + 1u] = static_cast<std::int16_t>(-l / 2);
    }
  }
  void render(unsigned blocks = 1) {
    for (unsigned b = 0; b < blocks; ++b) {
      rx(block++);
      report = ha::BlockReport{};
      const ha::BlockIo io{in, out, 0, &report};
      e.render_block(io);
    }
  }
  bool silent_out() const {
    for (std::uint32_t i = 0; i < ha::kBlockSamples; ++i) {
      if (out[i] != 0) return false;
    }
    return true;
  }
  // A poll of the app's frame (16 ms), with a block or two rendered in between so that the parameter changes reach the engine.
  bool poll(bool in_view, RecMode mode = RecMode::Sources, bool playing = false) {
    now += 16;
    const bool changed = rec.poll(now, in_view, mode, playing);
    return changed;
  }
  // The screen is in view and the source has settled: ready to arm.
  void settle(RecMode mode = RecMode::Sources) {
    poll(true, mode);
    for (int i = 0; i < 20; ++i) {
      poll(true, mode);
      render();
    }
  }
  static std::int16_t left(std::uint32_t at) { return static_cast<std::int16_t>(1000 + at); }
  static std::int16_t mixed(std::uint32_t at) {
    const std::int32_t l = 1000 + static_cast<std::int32_t>(at);
    return static_cast<std::int16_t>((l + (-l / 2) + 1) >> 1);
  }
};

}  // namespace

TEST_CASE("recorder: the plan of every source feeds the input strip alone into REC, at unity, monitored or not") {
  MixBoard board;
  RecorderConfig cfg;
  for (unsigned s = 0; s < cap::kSourceCount; ++s) {
    for (bool monitor : {false, true}) {
      cfg.source = static_cast<cap::Source>(s);
      cfg.monitor = monitor;
      const EnginePlan p = make_plan(cfg, RecMode::Sources, true, false, &board);
      const SourceInfo& info = kSources[s];
      CHECK(p.feed == info.feed);
      CHECK(p.channel == info.channel);
      CHECK(p.rec_source == RecTap::Source::Bus);
      CHECK(p.in_level == 100);
      CHECK(p.route[audio::kInputStrip] == (route::kRec | (monitor ? route::kMain | route::kCue : 0)));
      for (unsigned t = 0; t < kMixTracks; ++t) {
        CHECK((p.route[t] & route::kRec) == 0);  // no track is in the take
        CHECK((p.route[t] & ~route::kRec) == (board.route[t] & ~route::kRec));
      }
      // Settling: the input is held silent.
      CHECK(make_plan(cfg, RecMode::Sources, true, true, &board).in_level == 0);
    }
  }
  // The table: the microphone is the LEFT channel, the radio and the line the mix, USB alone is the return.
  CHECK(kSources[static_cast<unsigned>(cap::Source::MicLow)].channel == RecTap::Channel::Left);
  CHECK(kSources[static_cast<unsigned>(cap::Source::MicHigh)].channel == RecTap::Channel::Left);
  CHECK(kSources[static_cast<unsigned>(cap::Source::LineMix)].channel == RecTap::Channel::Mix);
  CHECK(kSources[static_cast<unsigned>(cap::Source::LineLeft)].channel == RecTap::Channel::Left);
  CHECK(kSources[static_cast<unsigned>(cap::Source::LineRight)].channel == RecTap::Channel::Right);
  CHECK(kSources[static_cast<unsigned>(cap::Source::Radio)].channel == RecTap::Channel::Mix);
  CHECK(kSources[static_cast<unsigned>(cap::Source::Usb)].feed == audio::InputFeed::ReturnOnly);
  CHECK(kSources[static_cast<unsigned>(cap::Source::Usb)].gain_bank == kNoGain);
  // The radio page always records the radio, whatever the source list says.
  cfg.source = cap::Source::MicHigh;
  CHECK(mode_source(cfg, RecMode::Radio) == cap::Source::Radio);
  CHECK(make_plan(cfg, RecMode::Radio, true, false, &board).feed == audio::InputFeed::CodecOnly);
  // Not owned: the Mixer's own values, the engine's own feed, whatever the config says.
  const EnginePlan idle = make_plan(cfg, RecMode::Sources, false, false, &board);
  CHECK(idle.feed == audio::InputFeed::Sum);
  CHECK(idle.channel == RecTap::Channel::Mix);
  CHECK(idle.rec_source == RecTap::Source::Bus);
  CHECK(idle.in_level == board.in_level);
  for (unsigned i = 0; i < kMixStrips; ++i) {
    CHECK(idle.route[i] == board.route[i]);
  }
  // Without a board: the engine's defaults.
  const EnginePlan none = make_plan(cfg, RecMode::Sources, false, false, nullptr);
  CHECK(none.in_level == 0);
  CHECK(none.route[0] == (route::kMain | route::kUsb));
  CHECK(none.route[audio::kInputStrip] == (route::kMain | route::kCue));
}

TEST_CASE("recorder: the resample taps: the master, the REC bus as routed, or one track and no other") {
  MixBoard board;
  board.route[2] = route::kMain | route::kRec;  // the user sent T3 to REC on the Routing page
  board.route[audio::kInputStrip] = route::kMain | route::kRec;
  RecorderConfig cfg;
  cfg.tap = RecTapKind::Master;
  EnginePlan p = make_plan(cfg, RecMode::Resample, true, false, &board);
  CHECK(p.rec_source == RecTap::Source::Master);
  CHECK(p.in_level == board.in_level);  // the input strip is the Mixer's
  for (unsigned i = 0; i < kMixStrips; ++i) {
    CHECK(p.route[i] == board.route[i]);
  }
  cfg.tap = RecTapKind::RecBus;
  p = make_plan(cfg, RecMode::Resample, true, false, &board);
  CHECK(p.rec_source == RecTap::Source::Bus);
  for (unsigned i = 0; i < kMixStrips; ++i) {
    CHECK(p.route[i] == board.route[i]);  // what the Routing page says, untouched
  }
  cfg.tap = RecTapKind::Track;
  cfg.track = 6;
  p = make_plan(cfg, RecMode::Resample, true, false, &board);
  CHECK(p.rec_source == RecTap::Source::Bus);
  for (unsigned t = 0; t < kMixTracks; ++t) {
    CHECK(((p.route[t] & route::kRec) != 0) == (t == 6));
    CHECK((p.route[t] & ~route::kRec) == (board.route[t] & ~route::kRec));  // the other bits are the Mixer's
  }
  CHECK((p.route[audio::kInputStrip] & route::kRec) == 0);
  CHECK((p.route[audio::kInputStrip] & ~route::kRec) == (board.route[audio::kInputStrip] & ~route::kRec));
}

TEST_CASE("recorder: aligned modes fall back to Free while the transport is stopped; the clock of a take") {
  CHECK(effective_align(RecAlign::Bar, false) == RecTap::Align::Free);
  CHECK(effective_align(RecAlign::EachStep, false) == RecTap::Align::Free);
  CHECK(effective_align(RecAlign::Bar, true) == RecTap::Align::Bar);
  CHECK(effective_align(RecAlign::EachStep, true) == RecTap::Align::Step);
  CHECK(effective_align(RecAlign::Pattern, true) == RecTap::Align::Pattern);
  CHECK(effective_align(RecAlign::Free, true) == RecTap::Align::Free);
  CHECK(Recorder::tenths(0) == 0);
  CHECK(Recorder::tenths(4412) == 1);
  CHECK(Recorder::tenths(44118) == 10);
  CHECK(Recorder::tenths(44118u * 60u) == 600);
}

TEST_CASE("recorder: entering the screen owns the input, points the codec, holds the input silent for the ADC to settle, and arming waits for it") {
  Rig rig;
  CHECK(rig.rec.state() == Recorder::State::Idle);
  CHECK_FALSE(rig.rec.owned());
  CHECK(rig.rec.arm(false) == Recorder::ArmResult::NotReady);  // not in view
  rig.poll(false);
  CHECK(rig.hw.selects == 0);  // the engine and the codec are not touched until the screen is shown
  rig.rec.cfg.source = cap::Source::MicLow;
  rig.rec.cfg.gain[1] = 40;
  rig.poll(true);
  CHECK(rig.rec.owned());
  CHECK(rig.hw.selects == 1);
  CHECK(rig.hw.last_source == cap::Source::MicLow);
  CHECK(rig.hw.last_gain == 40);
  CHECK(rig.rec.settling());
  CHECK(rig.rec.arm(false) == Recorder::ArmResult::Settling);
  // 200 ms later the input is let through.
  for (int i = 0; i < 20; ++i) {
    rig.poll(true);
    rig.render();
  }
  CHECK_FALSE(rig.rec.settling());
  CHECK(rig.e.graph().input_feed() == audio::InputFeed::CodecOnly);
  CHECK(rig.e.graph().rec().channel() == RecTap::Channel::Left);  // a microphone is the left channel
  // The gain moves without another switch.
  rig.rec.step_gain(5);
  rig.poll(true);
  CHECK(rig.hw.selects == 1);
  CHECK(rig.hw.gains == 1);
  CHECK(rig.hw.last_gain == 45);
  // Another source is a switch, and another settle.
  rig.rec.step_source(1);  // MIC HG
  rig.poll(true);
  CHECK(rig.hw.selects == 2);
  CHECK(rig.hw.last_source == cap::Source::MicHigh);
  CHECK(rig.rec.settling());
}

TEST_CASE("recorder: a take of the line in is recorded whether or not it is monitored, and what the monitor does is the input strip's route") {
  for (bool monitor : {false, true}) {
    Rig rig;
    rig.rec.cfg.monitor = monitor;
    rig.settle();
    const std::uint32_t b0 = rig.block;  // the take starts at the next block
    CHECK(rig.rec.arm(false) == Recorder::ArmResult::Ok);
    CHECK(rig.rec.state() == Recorder::State::Recording);  // not playing: Free
    bool heard = false;
    for (int b = 0; b < 30; ++b) {
      rig.render();
      heard = heard || !rig.silent_out();
      rig.poll(true);
    }
    CHECK(heard == monitor);
    CHECK(rig.rec.frames() > 20u * kB);
    CHECK(rig.rec.state() == Recorder::State::Recording);
    rig.rec.stop();
    rig.render();
    rig.poll(true);
    CHECK(rig.rec.state() == Recorder::State::Review);
    const std::uint32_t frames = rig.rec.frames();
    CHECK(frames > 20u * kB);
    CHECK(rig.rec.trim_start() == 0u);
    CHECK(rig.rec.trim_end() == frames);
    // The take is the mean of the two channels of the RX blocks it saw, sample for sample, from some block on.
    const std::int16_t* take = rig.rec.take_data();
    for (std::uint32_t i = 0; i < frames; ++i) {
      CHECK(take[i] == Rig::mixed(b0 * kB + i));
    }
    // The overview has all of it: its largest value is the take's.
    const TakeOverview& ov = rig.rec.overview();
    CHECK(ov.fed() == frames);
    std::int16_t hi = -32768;
    for (std::uint32_t s = 0; s < ov.slots(); ++s) {
      hi = ov.data()[2u * s] > hi ? ov.data()[2u * s] : hi;
    }
    CHECK(hi == take[frames - 1]);  // the signal rises
  }
}

TEST_CASE("recorder: the take starts at the bar's own sample while the pattern plays, and waits (armed) until the step comes") {
  Rig rig;
  rig.rec.cfg.align = RecAlign::Bar;
  rig.settle();
  g_step_block = rig.block + 5u;
  g_step_offset = 17;
  g_step_value = 16;  // the second bar of a pattern
  const std::uint32_t at = g_step_block * kB + g_step_offset;
  CHECK(rig.rec.arm(true) == Recorder::ArmResult::Ok);
  CHECK(rig.rec.state() == Recorder::State::Armed);
  CHECK(rig.rec.wait_align() == RecTap::Align::Bar);  // OS-METROALIGN: what the strip's countdown reads; the start step above is the same with or without it
  for (int b = 0; b < 5; ++b) {
    rig.render();
    rig.poll(true, RecMode::Sources, true);
    CHECK(rig.rec.state() == Recorder::State::Armed);
    CHECK(rig.rec.frames() == 0u);
  }
  rig.render(4);
  rig.poll(true, RecMode::Sources, true);
  CHECK(rig.rec.state() == Recorder::State::Recording);
  CHECK(rig.rec.wait_align() == RecTap::Align::Free);  // started: nothing to wait for
  rig.rec.stop();
  rig.render();
  rig.poll(true, RecMode::Sources, true);
  CHECK(rig.rec.state() == Recorder::State::Review);
  const std::int16_t* take = rig.rec.take_data();
  CHECK(rig.rec.frames() == 4u * kB - g_step_offset);  // the step's block from its sample on, three whole blocks; the stop block is not written
  for (std::uint32_t i = 0; i < rig.rec.frames(); ++i) {
    CHECK(take[i] == Rig::mixed(at + i));  // frame 0 IS the sample of the step
  }
  // Not playing: an aligned mode starts at once.
  rig.rec.discard();
  CHECK(rig.rec.arm(false) == Recorder::ArmResult::Ok);
  CHECK(rig.rec.state() == Recorder::State::Recording);
  rig.rec.stop();
  rig.render();
  rig.poll(true);
}

// OS-METROALIGN: the steps a take armed on a running transport still waits for, from the step now running (a pure function of the alignment, the step and the pattern's length), and the beat the strip shows.
TEST_CASE("recorder: wait_steps_left counts to the step the alignment accepts, and wait_beat shows it only from one beat on") {
  using A = RecTap::Align;
  // Bar: the next multiple of 16, or the pattern's end when it comes first. The running step counts; on the bar line itself the whole bar is left.
  CHECK(wait_steps_left(A::Bar, 0, 64) == 16u);
  CHECK(wait_steps_left(A::Bar, 3, 64) == 13u);
  CHECK(wait_steps_left(A::Bar, 15, 64) == 1u);
  CHECK(wait_steps_left(A::Bar, 16, 64) == 16u);
  CHECK(wait_steps_left(A::Bar, 60, 64) == 4u);
  CHECK(wait_steps_left(A::Bar, 20, 24) == 4u);  // a pattern of 24 steps: its own step 0 comes first (a bar line at 32 never does)
  CHECK(wait_steps_left(A::Bar, 5, 8) == 3u);
  // Pattern: step 0 of the next pass.
  CHECK(wait_steps_left(A::Pattern, 0, 64) == 64u);
  CHECK(wait_steps_left(A::Pattern, 3, 64) == 61u);
  CHECK(wait_steps_left(A::Pattern, 63, 64) == 1u);
  // Step: the next step, always less than a beat. Free waits for nothing. A step past the pattern's end (a stale position) is nothing.
  CHECK(wait_steps_left(A::Step, 7, 64) == 1u);
  CHECK(wait_steps_left(A::Free, 7, 64) == 0u);
  CHECK(wait_steps_left(A::Bar, 64, 64) == 0u);
  CHECK(wait_steps_left(A::Bar, 0, 0) == 0u);
  // The beat: ceil(steps / 4), the countdown's 4..1 in each bar; under one beat nothing starts, and one that has begun goes on to its end.
  CHECK(wait_beat(16, false) == 4u);
  CHECK(wait_beat(13, false) == 4u);
  CHECK(wait_beat(12, false) == 3u);
  CHECK(wait_beat(5, false) == 2u);
  CHECK(wait_beat(4, false) == 1u);
  CHECK(wait_beat(3, false) == 0u);
  CHECK(wait_beat(1, false) == 0u);
  CHECK(wait_beat(3, true) == 1u);
  CHECK(wait_beat(1, true) == 1u);
  CHECK(wait_beat(0, true) == 0u);
}

TEST_CASE("recorder: save puts the trimmed window into a slot through the sample bank's handover, in fresh memory") {
  Rig rig;
  rig.settle();
  // A sample already in slot 0 that a voice may be playing.
  model::Instrument inst;
  model::init_instrument(inst, model::InstrumentKind::Sample);
  CHECK(rig.project->project.set_instrument(0, inst) == model::Status::Ok);
  std::uint32_t cap_samples = 0;
  std::int16_t* old = rig.bank.reserve(cap_samples);
  for (int i = 0; i < 500; ++i) {
    old[i] = 321;
  }
  CHECK(rig.bank.commit_load(0, old, 500));
  rig.bank.poll(0, true, true);  // the handover: the voices have it
  CHECK(rig.bank.poll(SampleBank::kSettleMs + 1u, true, true));
  CHECK_FALSE(rig.bank.pending(0));
  CHECK(rig.rec.arm(false) == Recorder::ArmResult::Ok);
  CHECK(rig.rec.state() == Recorder::State::Recording);
  rig.render(40);
  rig.poll(true);
  rig.rec.stop();
  rig.render();
  rig.poll(true);
  CHECK(rig.rec.state() == Recorder::State::Review);
  const std::uint32_t frames = rig.rec.frames();
  // The trim: markers in take frames, never closer than the smallest window.
  rig.rec.set_trim_start(frames);  // too far: clamped
  CHECK(rig.rec.trim_start() == frames - Recorder::kMinWindow);
  rig.rec.set_trim_end(0);
  CHECK(rig.rec.trim_end() == frames);  // end stays: it cannot cross the start
  rig.rec.set_trim_start(0);
  rig.rec.set_trim_start(501);
  rig.rec.set_trim_end(frames - 1000);
  CHECK(rig.rec.trim_start() == 501u);
  CHECK(rig.rec.trim_end() == frames - 1000u);
  rig.rec.step_trim_start(-3, 64);
  CHECK(rig.rec.trim_start() == 501u - 192u);
  const std::uint32_t start = rig.rec.trim_start();
  const std::uint32_t window = rig.rec.trim_end() - rig.rec.trim_start();
  const std::int16_t* take = rig.rec.take_data();
  CHECK(rig.bank.pcm(0).data == old);  // until the commit, the slot's audio is untouched
  CHECK(rig.rec.commit(0));
  CHECK(rig.rec.state() == Recorder::State::Idle);
  const SampleBank::Pcm p = rig.bank.pcm(0);
  CHECK(p.frames == window);
  CHECK(p.data == take + (start & ~1u));  // the window, in the take's own memory, from an even frame
  CHECK(p.data != old);
  for (int i = 0; i < 500; ++i) {
    CHECK(old[i] == 321);  // what a voice may be reading is not rewritten
  }
  CHECK(rig.bank.pending(0));  // the voices get it at the next safe point
  CHECK_FALSE(rig.rec.commit(0));
}

TEST_CASE("recorder: a take the project load swept away ends as lost, and a full arena ends it as full") {
  {
    Rig rig;
    rig.settle();
    CHECK(rig.rec.arm(false) == Recorder::ArmResult::Ok);
    rig.render(10);
    rig.poll(true);
    rig.bank.clear();  // a project load
    rig.poll(true);
    rig.render();
    rig.poll(true);
    CHECK(rig.rec.state() == Recorder::State::Idle);
    CHECK(rig.rec.note() == Recorder::Note::Lost);
    CHECK(rig.rec.take_data() == nullptr);
  }
  {
    Rig rig(2000);  // 31 blocks of room
    rig.settle();
    CHECK(rig.rec.arm(false) == Recorder::ArmResult::Ok);
    CHECK(rig.rec.capacity() == 2000u);
    rig.render(40);
    rig.poll(true);
    CHECK(rig.rec.state() == Recorder::State::Review);
    CHECK(rig.rec.note() == Recorder::Note::Full);
    CHECK(rig.rec.frames() == 2000u);
  }
  {
    Rig rig(4);  // no room for a take
    rig.settle();
    std::uint32_t room = 0;
    rig.bank.reserve(room);
    CHECK(rig.rec.arm(false) == Recorder::ArmResult::Ok);  // 4 samples
    CHECK(rig.rec.capacity() == 4u);
    rig.render(2);
    rig.poll(true);
    CHECK(rig.rec.state() == Recorder::State::Review);
    CHECK(rig.rec.arm(false) == Recorder::ArmResult::Ok);  // the unsaved take is reused
    CHECK(rig.rec.capacity() == 4u);
  }
}

TEST_CASE("recorder: leaving the screen gives the Mixer's own routing back, and a take armed or recording goes on without the screen") {
  Rig rig;
  rig.mix->route[3] = route::kMain | route::kRec;  // a user's routing that the recorder overrides while it owns the path
  rig.mix->in_level = 55;
  CHECK(rig.e.params().push(core::ParamChange{audio::strip_param(3, gps::Route), static_cast<float>(rig.mix->route[3]), 1u}));
  CHECK(rig.e.params().push(core::ParamChange{audio::strip_param(audio::kInputStrip, gps::Volume), 55.0f, 1u}));
  rig.render();
  rig.settle();
  CHECK(rig.e.graph().input_feed() == audio::InputFeed::CodecOnly);
  CHECK(rig.rec.arm(false) == Recorder::ArmResult::Ok);
  rig.render(3);
  rig.poll(false);  // the user left the screen: the take goes on and keeps the path
  CHECK(rig.rec.owned());
  CHECK(rig.e.graph().input_feed() == audio::InputFeed::CodecOnly);
  rig.rec.stop();
  rig.render();
  rig.poll(false);
  CHECK(rig.rec.state() == Recorder::State::Review);
  rig.render();
  rig.poll(false);
  CHECK_FALSE(rig.rec.owned());
  // Everything is the Mixer's again: the feed, the tap and the strips (the input strip monitors at its own 55 into MAIN and CUE, T4 reaches REC).
  CHECK(rig.e.graph().input_feed() == audio::InputFeed::Sum);
  CHECK(rig.e.graph().rec().channel() == RecTap::Channel::Mix);
  CHECK(rig.e.graph().rec().source() == RecTap::Source::Bus);
  rig.render(2);
  bool heard = false;
  for (int b = 0; b < 4; ++b) {
    rig.render();
    heard = heard || !rig.silent_out();
  }
  CHECK(heard);  // the input strip is at 55 into MAIN again
}

TEST_CASE("recorder: the master resample records what is heard, with the pattern's own timeline") {
  Rig rig;
  rig.mix->in_level = 100;
  rig.mix->route[audio::kInputStrip] = route::kMain;
  CHECK(rig.e.params().push(core::ParamChange{audio::strip_param(audio::kInputStrip, gps::Volume), 100.0f, 1u}));
  CHECK(rig.e.params().push(core::ParamChange{audio::strip_param(audio::kInputStrip, gps::Route), static_cast<float>(route::kMain), 1u}));
  CHECK(rig.e.params().push(core::ParamChange{audio::master_param(audio::graph_param::master::Volume), 100.0f, 1u}));
  rig.render();
  rig.rec.cfg.tap = RecTapKind::Master;
  rig.settle(RecMode::Resample);
  CHECK(rig.hw.selects == 0);  // the codec is not touched by a resample
  CHECK(rig.e.graph().rec().source() == RecTap::Source::Master);
  const std::uint32_t b0 = rig.block;
  CHECK(rig.rec.arm(false) == Recorder::ArmResult::Ok);
  rig.render(5);
  rig.poll(true, RecMode::Resample);
  rig.rec.stop();
  rig.render();
  rig.poll(true, RecMode::Resample);
  CHECK(rig.rec.state() == Recorder::State::Review);
  const std::uint32_t frames = rig.rec.frames();
  const std::int16_t* take = rig.rec.take_data();
  for (std::uint32_t i = 0; i < frames; ++i) {
    CHECK(take[i] == Rig::mixed(b0 * kB + i));
  }
  CHECK(rig.rec.meter_channel() == audio::meter_ch::kMainL);
}

TEST_CASE("recorder: a codec that does not answer is said, not hidden, and is retried; the radio page tunes through the hardware") {
  Rig rig;
  rig.hw.ok = false;
  rig.poll(true);
  CHECK(rig.rec.note() == Recorder::Note::CodecFailed);
  CHECK(rig.rec.arm(false) != Recorder::ArmResult::Ok);
  const unsigned tries = rig.hw.selects;
  for (int i = 0; i < 80; ++i) {  // 1.3 s: a retry every half second
    rig.poll(true);
  }
  CHECK(rig.hw.selects > tries);
  CHECK(rig.hw.selects <= tries + 4u);
  rig.hw.ok = true;
  for (int i = 0; i < 40; ++i) {
    rig.poll(true);
    rig.render();
  }
  CHECK(rig.rec.note() == Recorder::Note::None);

  // The radio page: the receiver is pointed at the config's region and frequency, the encoder tunes, a seek asks the chip, the region is one of PAR-085's four.
  Rig radio;
  radio.rec.cfg.region = cap::Region::Japan;
  radio.rec.cfg.radio_freq = 8000;
  radio.poll(true, RecMode::Radio);
  CHECK(radio.hw.last_source == cap::Source::Radio);
  CHECK(radio.hw.region == cap::Region::Japan);
  CHECK(radio.hw.tuned == 8000);
  radio.rec.radio_step(5);
  CHECK(radio.rec.cfg.radio_freq == 8050);
  CHECK(radio.hw.tuned == 8050);
  radio.rec.radio_step(-1000);
  CHECK(radio.rec.cfg.radio_freq == 7600);  // clamped to the Japanese band
  radio.rec.radio_seek(true);
  radio.rec.radio_seek(false);
  CHECK(radio.hw.seeks == 0);
  radio.rec.radio_region_step(-2);
  CHECK(radio.rec.cfg.region == cap::Region::Usa);
  CHECK(radio.hw.region == cap::Region::Usa);
  CHECK(radio.rec.cfg.radio_freq == 8750);  // 76.0 MHz is off the USA's band: its bottom
  // A seek that ended on a station moves the config to it; the chip's status is what the screen shows.
  radio.hw.status.state = cap::RadioStatus::State::Ready;
  radio.hw.status.freq = 9590;
  radio.hw.status.rssi = 33;
  radio.hw.status.generation = 7;
  radio.poll(true, RecMode::Radio);
  CHECK(radio.rec.cfg.radio_freq == 9590);
  CHECK(radio.rec.radio().rssi == 33);
  CHECK(radio.rec.radio().state == cap::RadioStatus::State::Ready);
}

TEST_CASE("recorder: `radio retry` is allowed only when the radio is the selected source and no take from it is armed or recording (OS-RAD1 review)") {
  using R = Recorder::RadioRetry;
  Rig line;
  CHECK(line.rec.radio_retry() == R::NotSelected);  // line in is the default source: a retry would power up a radio nobody wants
  line.rec.cfg.source = cap::Source::MicLow;
  line.settle();
  CHECK(line.rec.radio_retry() == R::NotSelected);

  Rig radio;
  radio.settle(RecMode::Radio);
  CHECK(radio.rec.source() == cap::Source::Radio);
  CHECK(radio.rec.radio_retry() == R::Allowed);
  CHECK(radio.rec.arm(false) == Recorder::ArmResult::Ok);
  CHECK(radio.rec.state() == Recorder::State::Recording);
  CHECK(radio.rec.radio_retry() == R::TakeRunning);  // the reset would put ~0.7 s of silence into the take
  radio.render(3);
  radio.poll(true, RecMode::Radio);
  radio.rec.stop();
  radio.render();
  radio.poll(true, RecMode::Radio);
  CHECK(radio.rec.state() == Recorder::State::Review);
  CHECK(radio.rec.radio_retry() == R::Allowed);  // the take ended

  // The source list's own Radio entry counts as selected too; with an armed take that waits for its step the retry is refused as well.
  Rig listed;
  listed.rec.cfg.source = cap::Source::Radio;
  listed.rec.cfg.align = RecAlign::Bar;
  listed.settle();
  CHECK(listed.rec.radio_retry() == R::Allowed);
  g_step_block = listed.block + 50u;
  CHECK(listed.rec.arm(true) == Recorder::ArmResult::Ok);
  CHECK(listed.rec.state() == Recorder::State::Armed);
  CHECK(listed.rec.radio_retry() == R::TakeRunning);

  // No board to point (a test, the simulator): nothing to retry.
  Recorder bare;
  CHECK(bare.radio_retry() == R::NoRadio);
}

TEST_CASE("recorder: the engine's own state is untouched until the screen is first shown, and a refused ring push is retried") {
  Rig rig;
  for (int i = 0; i < 10; ++i) {
    rig.poll(false);
  }
  CHECK(rig.e.graph().input_feed() == audio::InputFeed::Sum);
  CHECK(rig.e.params().size() == 0u);
  // Fill the ring: the plan cannot be pushed; nothing is lost, the poll goes on trying.
  while (rig.e.params().push(core::ParamChange{audio::master_param(audio::graph_param::master::CueLevel), 100.0f, 1u})) {
  }
  rig.poll(true);
  rig.poll(true);
  rig.render(4);  // the engine drains up to 64 changes a block
  for (int i = 0; i < 40; ++i) {
    rig.poll(true);
    rig.render();
  }
  rig.render(4);
  bool heard = false;
  rig.rec.toggle_monitor();
  for (int i = 0; i < 10; ++i) {
    rig.poll(true);
    rig.render();
    heard = heard || !rig.silent_out();
  }
  CHECK(heard);  // the input strip reached MAIN at unity once the ring had room
}

TEST_CASE("recorder: the config steps clamp (source, gain, align, tap), and a source without a gain has none") {
  Rig rig;
  rig.rec.step_source(-5);
  CHECK(rig.rec.cfg.source == cap::Source::LineMix);
  rig.rec.step_source(100);
  CHECK(rig.rec.cfg.source == cap::Source::Usb);
  CHECK_FALSE(rig.rec.has_gain());
  rig.rec.step_gain(10);
  for (unsigned b = 0; b < kGainBanks; ++b) {
    CHECK(rig.rec.cfg.gain[b] == 0);
  }
  rig.rec.step_source(-6);
  rig.rec.step_gain(150);
  CHECK(rig.rec.cfg.gain[0] == 100);
  rig.rec.step_gain(-1000);
  CHECK(rig.rec.cfg.gain[0] == 0);
  rig.rec.step_gain(30);
  rig.rec.step_source(1);  // LINE L: the same line gain
  CHECK(rig.rec.gain() == 30);
  rig.rec.reset_gain();
  CHECK(rig.rec.gain() == 0);
  rig.rec.step_align(-10);
  CHECK(rig.rec.cfg.align == RecAlign::Free);
  rig.rec.step_align(10);
  CHECK(rig.rec.cfg.align == RecAlign::Level);  // (OS-SREC-LEVEL: LEVEL follows PATTERN)
  rig.rec.step_align(-1);
  CHECK(rig.rec.cfg.align == RecAlign::Pattern);
  rig.rec.step_tap(-5);
  CHECK(rig.rec.cfg.tap == RecTapKind::Master);
  rig.rec.step_tap(1);
  CHECK(rig.rec.cfg.tap == RecTapKind::RecBus);
  rig.rec.step_tap(1);
  CHECK(rig.rec.cfg.tap == RecTapKind::Track);
  CHECK(rig.rec.cfg.track == 0);
  rig.rec.step_tap(100);
  CHECK(rig.rec.cfg.track == 15);
}

// ---- OS-REC2: STOP AT ----------------------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("recorder: STOP AT steps through MANUAL, STEP, BAR, PATTERN and 1..16 BARS and clamps; it is Manual while the transport is stopped, and only a running one has steps to count") {
  Rig rig;
  CHECK(rig.rec.cfg.stop == RecStop::Manual);
  rig.rec.step_stop(1);
  CHECK(rig.rec.cfg.stop == RecStop::EachStep);
  rig.rec.step_stop(2);
  CHECK(rig.rec.cfg.stop == RecStop::Pattern);
  rig.rec.step_stop(1);
  CHECK(rig.rec.cfg.stop == RecStop::Bars);
  CHECK(rig.rec.cfg.stop_bars == 1);
  rig.rec.step_stop(3);
  CHECK(rig.rec.cfg.stop_bars == 4);
  rig.rec.step_stop(-1);
  CHECK(rig.rec.cfg.stop_bars == 3);
  rig.rec.step_stop(-3);  // back into the named ones: 1 BAR, then PATTERN
  CHECK(rig.rec.cfg.stop == RecStop::Pattern);
  rig.rec.step_stop(100);
  CHECK(rig.rec.cfg.stop == RecStop::Bars);
  CHECK(rig.rec.cfg.stop_bars == kRecMaxBars);
  rig.rec.step_stop(-100);
  CHECK(rig.rec.cfg.stop == RecStop::Manual);
  CHECK(rig.rec.cfg.stop_bars == 1);
  unsigned seen = 1;  // the number of choices
  for (rig.rec.step_stop(1); rig.rec.cfg.stop != RecStop::Manual && seen < 100; rig.rec.step_stop(1)) {
    ++seen;
    if (rig.rec.cfg.stop == RecStop::Bars && rig.rec.cfg.stop_bars == kRecMaxBars) {
      break;
    }
  }
  CHECK(seen == kRecStopChoices);
  // The pure rules.
  CHECK(effective_stop(RecStop::Bar, false) == RecStop::Manual);
  CHECK(effective_stop(RecStop::Bar, true) == RecStop::Bar);
  CHECK(stop_boundary(RecStop::EachStep) == RecTap::Align::Step);
  CHECK(stop_boundary(RecStop::Bar) == RecTap::Align::Bar);
  CHECK(stop_boundary(RecStop::Pattern) == RecTap::Align::Pattern);
  CHECK(stop_boundary(RecStop::Manual) == RecTap::Align::Free);
  CHECK(stop_boundary(RecStop::Bars) == RecTap::Align::Free);  // N bars end by themselves; F1 stops at once
  CHECK(stop_auto_steps(RecStop::Bars, 4, true) == 64u);
  CHECK(stop_auto_steps(RecStop::Bars, 4, false) == 0u);
  CHECK(stop_auto_steps(RecStop::Bars, 99, true) == 16u * kRecMaxBars);
  CHECK(stop_auto_steps(RecStop::Bar, 4, true) == 0u);
}

TEST_CASE("recorder: F1 at STOP AT BAR ends the take at the bar's own sample; a second F1 stops at once; Manual and a stopped transport are the old stop") {
  const auto run = [](RecStop stop, bool second_stop, bool transport) {
    struct Out {
      std::uint32_t frames, start, at;
      bool pending;
      bool review;
      std::int16_t last;
    };
    Rig rig;
    rig.rec.cfg.stop = stop;
    rig.rec.cfg.align = RecAlign::Free;
    rig.settle();
    rig.poll(true, RecMode::Sources, transport);
    CHECK(rig.rec.arm(transport) == Recorder::ArmResult::Ok);
    const std::uint32_t start = rig.block * kB;  // the take's first frame: the next block's first sample
    rig.render(6);
    rig.poll(true, RecMode::Sources, transport);
    CHECK(rig.rec.state() == Recorder::State::Recording);
    const std::uint32_t at = (rig.block + 3u) * kB + 29u;  // the bar's sample, three blocks on, inside its block
    g_script = {{at, audio::EventKind::Step, 32}};  // step 32: a bar
    rig.rec.stop();
    Out o{};
    o.pending = rig.rec.stop_pending();
    if (second_stop) {
      rig.render(1);
      rig.rec.stop();  // the second press: at once
    }
    rig.render(6);
    rig.poll(true, RecMode::Sources, transport);
    o.review = rig.rec.state() == Recorder::State::Review;
    o.frames = rig.rec.frames();
    o.start = start;
    o.at = at;
    o.last = o.frames != 0u ? rig.rec.take_data()[o.frames - 1u] : std::int16_t{0};
    return o;
  };
  {
    const auto o = run(RecStop::Bar, false, true);
    CHECK(o.pending);
    CHECK(o.review);
    CHECK(o.frames == o.at - o.start);  // the take is [its first sample, the bar's sample)
    CHECK(o.last == Rig::mixed(o.at - 1u));
  }
  {
    const auto o = run(RecStop::Bar, true, true);  // pending, then a second F1: the next block's start (one block rendered since the first)
    CHECK(o.pending);
    CHECK(o.review);
    CHECK(o.frames == 7u * kB);  // 6 blocks, then the one rendered before the second press
  }
  {
    const auto o = run(RecStop::Manual, false, true);  // as before: the next block
    CHECK_FALSE(o.pending);
    CHECK(o.review);
    CHECK(o.frames == 6u * kB);
  }
  {
    const auto o = run(RecStop::Bar, false, false);  // the transport is stopped: nothing to wait for
    CHECK_FALSE(o.pending);
    CHECK(o.review);
    CHECK(o.frames == 6u * kB);
  }
  {
    const auto o = run(RecStop::Bars, false, true);  // N bars: F1 is the early end
    CHECK_FALSE(o.pending);
    CHECK(o.frames == 6u * kB);
  }
}

TEST_CASE("recorder: N bars end the take by themselves on the timeline; a stop that finds the transport stopped ends a pending boundary at the Stopped event's sample") {
  {
    Rig rig;
    rig.rec.cfg.stop = RecStop::Bars;
    rig.rec.cfg.stop_bars = 1;
    rig.rec.cfg.align = RecAlign::Free;
    rig.settle();
    rig.poll(true, RecMode::Sources, true);
    CHECK(rig.rec.arm(true) == Recorder::ArmResult::Ok);
    CHECK(rig.rec.ends_by_itself());
    const std::uint32_t start = rig.block * kB;
    g_script.clear();
    for (std::uint32_t k = 0; k < 24; ++k) {  // a Step event every 700 samples from the second block on
      g_script.push_back({static_cast<std::uint64_t>(start) + 2u * kB + 10u + k * 700u, audio::EventKind::Step, static_cast<std::int32_t>(k % 64u)});
    }
    rig.render(200);
    rig.poll(true, RecMode::Sources, true);
    CHECK(rig.rec.state() == Recorder::State::Review);
    CHECK_FALSE(rig.rec.stop_pending());
    CHECK(rig.rec.frames() == static_cast<std::uint32_t>(g_script[15].at - start));  // the 16th Step event after the take began: one bar
    CHECK(rig.rec.take_data()[0] == Rig::mixed(start));
    CHECK(rig.rec.take_data()[rig.rec.frames() - 1u] == Rig::mixed(static_cast<std::uint32_t>(g_script[15].at - 1u)));
  }
  {  // the transport is stopped when it is armed: nothing ends it by itself
    Rig rig;
    rig.rec.cfg.stop = RecStop::Bars;
    rig.rec.cfg.align = RecAlign::Free;
    rig.settle();
    CHECK(rig.rec.arm(false) == Recorder::ArmResult::Ok);
    CHECK_FALSE(rig.rec.ends_by_itself());
    rig.render(20);
    rig.poll(true, RecMode::Sources, false);
    CHECK(rig.rec.state() == Recorder::State::Recording);
    rig.rec.stop();
    rig.render(1);
    rig.poll(true, RecMode::Sources, false);
    CHECK(rig.rec.state() == Recorder::State::Review);
  }
  {  // pending at a pattern start, the transport stops first: the Stopped event ends the take
    Rig rig;
    rig.rec.cfg.stop = RecStop::Pattern;
    rig.rec.cfg.align = RecAlign::Free;
    rig.settle();
    rig.poll(true, RecMode::Sources, true);
    CHECK(rig.rec.arm(true) == Recorder::ArmResult::Ok);
    const std::uint32_t start = rig.block * kB;
    rig.render(5);
    rig.poll(true, RecMode::Sources, true);
    const std::uint64_t stopped = (rig.block + 2u) * kB + 41u;
    g_script = {{stopped, audio::EventKind::Stopped, 0}};
    rig.rec.stop();
    CHECK(rig.rec.stop_pending());
    rig.render(6);
    rig.poll(true, RecMode::Sources, false);
    CHECK(rig.rec.state() == Recorder::State::Review);
    CHECK(rig.rec.frames() == static_cast<std::uint32_t>(stopped - start));
  }
}

TEST_CASE("recorder: arm is refused while the DJ scratch ring holds the arena's tail, and the bank-clear check covers that owner too (OS-DJ C1 fold)") {
  Rig rig;
  rig.settle();
  CHECK_FALSE(rig.bank.clear_busy());
  std::vector<std::int16_t> ring(2u * 5000u);
  audio::MasterScratch& stage = rig.e.graph().scratch();
  CHECK(stage.stage(ring.data(), 5000));
  CHECK(rig.bank.clear_busy());  // a loader that waits for the bank-clear check waits for the scratch as well
  CHECK(rig.rec.arm(false) == Recorder::ArmResult::Busy);
  CHECK(rig.rec.state() == Recorder::State::Idle);
  stage.unstage();  // the render has let go
  CHECK_FALSE(rig.bank.clear_busy());
  CHECK(rig.rec.arm(false) == Recorder::ArmResult::Ok);
}

// ---- OS-SREC-LEVEL (E-SREC-11): START ON LEVEL, the recorder and the tap's ring ----------------------------------------------------------------------------------------------------------------
namespace {

// The RX signal rises by one a frame (left = 1000 + frame), so the threshold t is first reached at frame t - 1000 and every frame is told from its neighbours. `a0` is the frame the audio of the take
// begins with (index `floor` of the take): from there the take is the mix of the frames in order, to its end; what precedes `floor` is zeros.
void check_in_order(const std::int16_t* take, std::uint32_t frames, std::uint32_t floor, std::uint32_t a0) {
  CHECK_MESSAGE(floor <= frames, "floor ", floor, " frames ", frames);
  if (floor > frames) {
    return;
  }
  for (std::uint32_t i = 0; i < floor; ++i) {
    if (take[i] != 0) {
      CHECK_MESSAGE(false, "not zero before the floor at ", i);
      return;
    }
  }
  for (std::uint32_t i = floor; i < frames; ++i) {
    if (take[i] != Rig::mixed(a0 + (i - floor))) {
      CHECK_MESSAGE(false, "not in time order at ", i, " floor ", floor, " got ", take[i], " want ", Rig::mixed(a0 + (i - floor)));
      return;
    }
  }
}

void level_take(int db, unsigned preroll) {
  Rig rig(120000);
  rig.rec.cfg.align = RecAlign::Level;
  rig.rec.cfg.level_db = static_cast<std::int8_t>(db);
  rig.rec.cfg.preroll = static_cast<std::uint8_t>(preroll);
  rig.settle();
  const std::uint32_t t = static_cast<std::uint32_t>(rec_level_threshold(db));
  const std::uint32_t pre = static_cast<std::uint32_t>(static_cast<std::uint64_t>(ha::sample_rate_mhz()) * kRecPreRollMs[preroll] / 1000000u);
  const std::uint32_t ring = RecTap::ring_frames(pre);
  const std::uint32_t pre_e = ring - kB;
  const std::uint32_t first = rig.block * kB;  // the first frame the tap sees
  const std::uint32_t hit = t - 1000u > first ? t - 1000u : first;  // the frame that reaches the level (the signal may be above it already)
  CHECK(rig.rec.arm(false) == Recorder::ArmResult::Ok);  // the transport is stopped: a level needs no timeline
  CHECK(rig.rec.state() == Recorder::State::Armed);
  CHECK(rig.rec.wait_align() == RecTap::Align::Level);
  const std::uint32_t trig_block = hit / kB;
  while (rig.block <= trig_block) {
    rig.render();
    rig.poll(true);
    if (rig.block <= trig_block) {
      CHECK(rig.rec.state() == Recorder::State::Armed);  // listening: no take yet
      CHECK(rig.rec.frames() == 0u);
    }
  }
  CHECK(rig.rec.state() == Recorder::State::Recording);
  rig.render(3);
  rig.poll(true);
  CHECK(rig.rec.state() == Recorder::State::Recording);
  const std::uint32_t heard_before = hit - first;                 // frames of signal that came before the trigger frame
  const std::uint32_t want_pre = heard_before < pre_e ? heard_before : pre_e;  // the pre-roll that was heard
  const std::uint32_t after = kB - hit % kB;                       // the trigger frame and the rest of its block
  const std::uint32_t floor = rig.rec.trim_floor();
  CHECK(floor <= ring);
  CHECK((floor & 1u) == 0u);
  const std::uint32_t kept = ring - floor;  // frames of the ring that are audio
  CHECK(kept >= want_pre + after);
  CHECK(kept <= want_pre + after + 1u);  // one frame more of pre-roll to start on an even frame, only if it was heard
  CHECK(kept - after <= heard_before);
  const std::uint32_t a0 = hit - (kept - after);
  // Live, while recording: the ring is already in order (the overview reads it so).
  CHECK(rig.rec.frames() > ring);
  check_in_order(rig.rec.take_data(), rig.rec.frames(), floor, a0);
  rig.rec.stop();
  rig.render();
  rig.poll(true);
  CHECK(rig.rec.state() == Recorder::State::Review);
  const std::uint32_t frames = rig.rec.frames();
  CHECK(frames > ring);
  check_in_order(rig.rec.take_data(), frames, floor, a0);
  CHECK(rig.rec.trim_floor() == floor);
  CHECK(rig.rec.trim_start() == floor);  // the marker starts where the audio does; it was not moved by the pre-roll logic
  CHECK(rig.rec.trim_end() == frames);
  rig.rec.set_trim_start(0);  // the frames before the floor are no audio: the marker cannot go there
  CHECK(rig.rec.trim_start() == floor);
  const TakeOverview& ov = rig.rec.overview();
  CHECK(ov.fed() == frames);
  std::int16_t hi = -32768;
  for (std::uint32_t s = 0; s < ov.slots(); ++s) {
    hi = ov.data()[2u * s] > hi ? ov.data()[2u * s] : hi;
  }
  CHECK(hi == rig.rec.take_data()[frames - 1u]);  // the signal rises: the last frame is the largest, wherever the ring went
  if (floor != 0u) {
    CHECK(rig.rec.crop());  // the zeros go
    CHECK(rig.rec.trim_floor() == 0u);
    CHECK(rig.rec.trim_start() == 0u);
    check_in_order(rig.rec.take_data(), rig.rec.frames(), 0u, a0);
  }
}

}  // namespace

TEST_CASE("recorder: a LEVEL take starts on the level with its pre-roll before it, in time order, whether the ring wrapped or not") {
  level_take(-30, 3);  // the signal is above the level at once: the trigger is the first frame, a ring of nothing but the trigger block
  level_take(-18, 4);  // 100 ms: less than that was heard when the level came, and the ring never wrapped
  level_take(-12, 3);  // 50 ms: the ring wrapped many times before the level came (the trigger lies inside a block)
  level_take(-12, 0);  // no pre-roll: the trigger frame and the rest of its block
  level_take(-12, 1);  // 10 ms
  level_take(-9, 2);   // 20 ms
}

TEST_CASE("recorder: F1 and a stopped transport while listening for a level: the take goes on or gives no take, and the memory is free again") {
  Rig rig(120000);
  rig.rec.cfg.align = RecAlign::Level;
  rig.rec.cfg.level_db = -6;  // the signal needs 120 blocks to get there
  rig.settle();
  CHECK(rig.rec.arm(true) == Recorder::ArmResult::Ok);
  CHECK(rig.rec.state() == Recorder::State::Armed);
  CHECK(rig.bank.clear_busy());  // a loader waits for the listening tap (bank_clear_busy covers Listening)
  rig.render(5);
  rig.poll(true, RecMode::Sources, true);
  rig.poll(true, RecMode::Sources, false);  // the transport stops: a level take has no timeline to lose
  rig.render();
  rig.poll(true, RecMode::Sources, false);
  CHECK(rig.rec.state() == Recorder::State::Armed);
  CHECK(rig.rec.frames() == 0u);
  CHECK(rig.rec.wait_align() == RecTap::Align::Level);
  rig.rec.stop();  // F1
  rig.render();
  rig.poll(true);
  CHECK(rig.rec.state() == Recorder::State::Idle);  // no take
  CHECK(rig.rec.frames() == 0u);
  CHECK(rig.rec.trim_floor() == 0u);
  CHECK_FALSE(rig.bank.clear_busy());
  CHECK(rig.rec.arm(false) == Recorder::ArmResult::Ok);  // the arena went back: the next take can be armed
  CHECK(rig.rec.state() == Recorder::State::Armed);
}

TEST_CASE("recorder: a LEVEL take is refused when the arena cannot hold its ring and a second, and the THRESHOLD and PRE-ROLL steps clamp") {
  Rig rig;  // 40000 samples: less than a second
  rig.rec.cfg.align = RecAlign::Level;
  rig.settle();
  CHECK(rig.rec.arm(false) == Recorder::ArmResult::NoRoom);
  CHECK(rig.rec.state() == Recorder::State::Idle);
  CHECK(rig.rec.note() == Recorder::Note::NoRoom);
  CHECK_FALSE(rig.bank.clear_busy());
  CHECK(rig.rec.cfg.level_db == -30);
  CHECK(rig.rec.cfg.preroll == kRecPreRollDefault);
  rig.rec.step_threshold(1);
  CHECK(rig.rec.cfg.level_db == -27);
  rig.rec.step_threshold(100);
  CHECK(rig.rec.cfg.level_db == -6);
  rig.rec.step_threshold(-100);
  CHECK(rig.rec.cfg.level_db == -60);
  rig.rec.step_preroll(100);
  CHECK(rig.rec.cfg.preroll == kRecPreRolls - 1u);
  rig.rec.step_preroll(-100);
  CHECK(rig.rec.cfg.preroll == 0u);
  CHECK(rec_level_threshold(-60) == 33);
  CHECK(rec_level_threshold(-6) == 16423);
  CHECK(rec_level_threshold(-100) == 33);  // out of range: clamped
  CHECK(rec_level_threshold(0) == 16423);
  CHECK(effective_align(RecAlign::Level, false) == RecTap::Align::Level);  // no fall back to Free: a level needs no timeline
  CHECK(effective_align(RecAlign::Level, true) == RecTap::Align::Level);
  CHECK(wait_steps_left(RecTap::Align::Level, 3, 64) == 0u);
}
