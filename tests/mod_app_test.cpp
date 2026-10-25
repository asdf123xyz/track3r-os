// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-MOD, the app's side: the descriptor the loader builds for the pan (slot 1), finetune (slot 5) and amp LFO (slot 0) modulators of an instrument (og_samples.h make_sample_desc; the
// field-by-field equality with the live path is in inst_live_app_test.cpp), the ParamChanges that move a sounding instrument's loop (live_loop_moves, pure), and the Wave screen's
// nudge of a loop point reaching the voice that plays it through the engine's ring while the sample bank's handover is still waiting for the voice to end (voices.h LIVE LOOP).
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/og_samples.h>
#include <o3os/app/sample_bank.h>
#include <o3os/audio/voices.h>
#include <o3os/core/action.h>
#include <o3os/core/input_event.h>
#include <o3os/core/param.h>
#include <o3os/model/editor.h>
#include <o3os/platform/host/fakes.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

using namespace o3os;
using app::Button;

namespace {

alignas(8) std::uint8_t g_ring[256 * 1024];

constexpr unsigned kSlot = 4;

model::Instrument sample_instrument() {
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Sample);
  return in;
}

model::Instrument loop_instrument(unsigned mode, std::uint16_t start, std::uint16_t ls, std::uint16_t le, std::uint16_t end) {
  model::Instrument in = sample_instrument();
  in.sampler.play_mode = static_cast<std::uint8_t>(mode);
  in.sampler.start = start;
  in.sampler.loop_start = ls;
  in.sampler.loop_end = le;
  in.sampler.end = end;
  return in;
}

class FakePreview final : public app::PreviewSink {
 public:
  void preview(unsigned, unsigned) noexcept override {}
  bool live_idle() const noexcept override { return true; }
  void hold_note(bool, unsigned, unsigned, unsigned) noexcept override {}
  void audition(bool, unsigned) noexcept override {}
  std::uint32_t live_position() const noexcept override { return 0; }
};

}  // namespace

TEST_CASE("modulator descriptors: the pan, finetune and amp slots become pan_mod, tune_mod and amp_mod, an LFO slot by its loop byte, the amp slot's ADSR staying `env`") {
  static std::int16_t pcm[4000];
  model::Instrument in = sample_instrument();
  model::SamplerParams& p = in.sampler;
  model::Envelope& pe = p.envelope[1];  // an ADSR on the pan
  pe.enable = 1;
  pe.loop = 0;
  pe.amount = 0.5f;
  pe.delay = 3;
  pe.attack = 20;
  pe.hold = 5;
  pe.decay = 30;
  pe.sustain = 0.4f;
  pe.release = 50;
  model::Envelope& te = p.envelope[5];  // an LFO on the finetune
  te.enable = 1;
  te.loop = 1;
  p.lfo[5].shape = 2;
  p.lfo[5].speed = 18;
  p.lfo[5].amount = 0.3f;
  model::Envelope& ae = p.envelope[0];  // an LFO on the amp, with a speed past the amp table's 24 rates
  ae.enable = 1;
  ae.loop = 1;
  p.lfo[0].shape = 3;
  p.lfo[0].speed = 30;
  p.lfo[0].amount = 0.7f;
  std::unique_ptr<audio::VoicePool> pool = std::make_unique<audio::VoicePool>();
  pool->set_sample(0, app::make_sample_desc(in, pcm, 4000, nullptr));
  const audio::SampleDesc& d = pool->sample(0);
  CHECK(d.pan_mod.kind == audio::kFmodEnv);
  CHECK(d.pan_mod.amount == 0.5f);
  CHECK(d.pan_mod.delay == 3);
  CHECK(d.pan_mod.attack == 20);
  CHECK(d.pan_mod.hold == 5);
  CHECK(d.pan_mod.decay == 30);
  CHECK(d.pan_mod.sustain == 0.4f);
  CHECK(d.pan_mod.release == 50);
  CHECK(d.tune_mod.kind == audio::kFmodLfo);
  CHECK(d.tune_mod.shape == 2);
  CHECK(d.tune_mod.rate == 18);
  CHECK(d.tune_mod.amount == 0.3f);
  CHECK(d.amp_mod.kind == audio::kFmodLfo);
  CHECK(d.amp_mod.shape == 3);
  CHECK(d.amp_mod.rate == 23);  // held to the amp table
  CHECK(d.amp_mod.amount == 0.7f);
  CHECK_FALSE(d.env.enable);  // the amp slot is the tremolo, not an ADSR: the plain gain
  CHECK(d.filter.mod.kind == audio::kFmodNone);
  // an ADSR on the amp slot is `env`, no tremolo
  ae.loop = 0;
  pool->set_sample(0, app::make_sample_desc(in, pcm, 4000, nullptr));
  CHECK(pool->sample(0).env.enable);
  CHECK(pool->sample(0).amp_mod.kind == audio::kFmodNone);
  // disabled slots make no modulator
  pe.enable = 0;
  te.enable = 0;
  pool->set_sample(0, app::make_sample_desc(in, pcm, 4000, nullptr));
  CHECK(pool->sample(0).pan_mod.kind == audio::kFmodNone);
  CHECK(pool->sample(0).tune_mod.kind == audio::kFmodNone);
}

TEST_CASE("live loop moves: a loop point moved inside the same window is one ParamChange in samples from the window's start, in the order that keeps each valid; anything else is the handover's") {
  using app::LoopMove;
  namespace ip = audio::inst_param;
  constexpr std::uint32_t kF = 65535;  // frames == points: a point is its own sample index
  LoopMove m[2];
  const model::Instrument base = loop_instrument(1, 0, 20000, 40000, 65535);
  const auto moves = [&](const model::Instrument& after) { return app::live_loop_moves(base, after, kF, m); };
  CHECK(moves(base) == 0u);
  CHECK(moves(loop_instrument(1, 0, 20000, 45000, 65535)) == 1u);
  CHECK(m[0].field == ip::LoopEnd);
  CHECK(m[0].value == 45000.0f);
  CHECK(moves(loop_instrument(1, 0, 10000, 40000, 65535)) == 1u);
  CHECK(m[0].field == ip::LoopStart);
  CHECK(m[0].value == 10000.0f);
  // the loop jumps right, clear of the old one: the end first, or the start would be held to the old end
  CHECK(moves(loop_instrument(1, 0, 50000, 60000, 65535)) == 2u);
  CHECK(m[0].field == ip::LoopEnd);
  CHECK(m[0].value == 60000.0f);
  CHECK(m[1].field == ip::LoopStart);
  CHECK(m[1].value == 50000.0f);
  // ... or only just past it (2 samples apart is the rule)
  CHECK(moves(loop_instrument(1, 0, 39999, 41000, 65535)) == 2u);
  CHECK(m[0].field == ip::LoopEnd);
  // the loop jumps left: the start first
  CHECK(moves(loop_instrument(1, 0, 1000, 5000, 65535)) == 2u);
  CHECK(m[0].field == ip::LoopStart);
  CHECK(m[0].value == 1000.0f);
  CHECK(m[1].field == ip::LoopEnd);
  // relative to the window: the descriptor's data starts at the instrument's start
  const model::Instrument windowed = loop_instrument(1, 5000, 20000, 40000, 60000);
  CHECK(app::live_loop_moves(windowed, loop_instrument(1, 5000, 21000, 40000, 60000), kF, m) == 1u);
  CHECK(m[0].field == ip::LoopStart);
  CHECK(m[0].value == 16000.0f);
  // not the ring's: the window, the mode, a loop the descriptor does not have, on either side
  CHECK(moves(loop_instrument(1, 100, 20000, 40000, 65535)) == 0u);   // start moved
  CHECK(moves(loop_instrument(1, 0, 20000, 40000, 60000)) == 0u);     // end moved
  CHECK(moves(loop_instrument(2, 0, 20000, 45000, 65535)) == 0u);     // another loop mode
  CHECK(moves(loop_instrument(0, 0, 20000, 45000, 65535)) == 0u);     // one shot after
  CHECK(app::live_loop_moves(loop_instrument(0, 0, 20000, 40000, 65535), base, kF, m) == 0u);  // one shot before
  CHECK(moves(loop_instrument(1, 0, 20000, 20001, 65535)) == 0u);     // under 2 samples: no loop
  CHECK(moves(loop_instrument(1, 0, 40000, 20000, 65535)) == 0u);     // inverted: no loop
  model::Instrument other = base;
  other.sampler.play_mode = 6;  // a wavetable
  CHECK(app::live_loop_moves(other, other, kF, m) == 0u);
}

namespace {

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<audio::VoicePool> voices = std::make_unique<audio::VoicePool>();
  std::unique_ptr<model::ModelArena> arena = std::make_unique<model::ModelArena>();
  std::vector<std::int16_t> memory = std::vector<std::int16_t>(400000);
  std::vector<std::int16_t> peaks = std::vector<std::int16_t>(40000);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  std::unique_ptr<app::SampleBankMem> mem = std::make_unique<app::SampleBankMem>();
  app::SampleBank bank;
  FakePreview preview;
  core::ParamRing ring;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  unsigned applied = 0;

  Rig() {
    arena->project.reset();
    bank.init(*voices, *arena, *mem, memory.data(), 400000, peaks.data(), peaks.size());
    model::Instrument in = loop_instrument(1, 0, 20000, 40000, 65535);
    std::strcpy(in.name, "LOOP");
    CHECK(arena->project.set_instrument(kSlot, in) == model::Status::Ok);
    std::uint32_t cap = 0;
    std::int16_t* p = bank.reserve(cap);
    NEED_VOID(p != nullptr);
    for (std::uint32_t i = 0; i < 60000; ++i) {
      p[i] = static_cast<std::int16_t>(i % 200u);
    }
    NEED_VOID(bank.commit_load(kSlot, p, 60000));
    bank.rebind_now(kSlot);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(arena->project, journal);
    app->attach_document(editor.get());
    app->attach_samples(&bank);
    app->attach_preview(&preview);
    app->attach_param_ring(&ring);
    (void)app->invoke(core::action_id("instrument.select"), static_cast<core::ActionArg>(kSlot + 1));
    (void)app->invoke(core::action_id("nav.screen"), static_cast<core::ActionArg>(app::ScreenId::Wave));
    pass();
  }
  static void NEED_VOID(bool ok) { CHECK_MESSAGE(ok, "rig setup"); }
  // The render's side: the ring drained into the voices between two passes, then a block of the voices.
  void drain() {
    core::ParamChange c;
    while (ring.pop(c)) {
      std::uint32_t pan_tracks = 0;
      applied += voices->apply_param(core::param_index(c.id), core::param_field(c.id), c.value, pan_tracks) ? 1u : 0u;
    }
  }
  void block() {
    voices->begin_block();
    voices->render(0, audio::VoicePool::kBlock);
    voices->end_block();
  }
  void pass(unsigned ms = 16) {
    clock.advance(ms * core::kTicksPerMs);
    drain();
    app->run_once();
  }
  void passes(unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
      pass();
    }
  }
  void tap(Button b) {
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true));
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false));
    pass();
  }
  void invoke(const char* name, core::ActionArg arg) {
    (void)app->invoke(core::action_id(name), arg);
    pass();
  }
  const model::SamplerParams& sp() const { return arena->project.instrument(kSlot)->sampler; }
  const audio::SampleDesc& desc() const { return voices->sample(kSlot); }
};

}  // namespace

TEST_CASE("live loop (app): nudging a loop point on the Wave screen while the instrument sounds moves the descriptor's loop at once, the handover waiting; when the voice ends the rebuild is the same") {
  Rig rig;
  NEED(rig.desc().length == 60000u);
  NEED(rig.desc().loop_end != 0u);
  const std::uint32_t ls0 = rig.desc().loop_start;
  const std::uint32_t le0 = rig.desc().loop_end;
  CHECK(le0 > ls0 + 2u);
  CHECK(rig.voices->note_on(0, 60, kSlot, 127));  // the instrument sounds
  rig.block();
  rig.tap(Button::F2);  // LOOP START
  rig.invoke("wave.zc", 0);
  rig.invoke("wave.nudge", 3);
  CHECK(rig.sp().loop_start > 20000u);
  rig.passes(2);
  CHECK(rig.applied == 1u);  // one ParamChange reached the render
  CHECK(rig.desc().loop_start > ls0);
  CHECK(rig.desc().loop_end == le0);
  CHECK(rig.bank.pending(kSlot));  // the handover waits for the voice: the live path did it
  // the voice takes it: the position never leaves the new loop once it has wrapped
  for (int b = 0; b < 400; ++b) {
    rig.block();
  }
  // LOOP END
  rig.tap(Button::F3);
  rig.invoke("wave.nudge", -4);
  rig.passes(2);
  CHECK(rig.applied == 2u);
  CHECK(rig.desc().loop_end < le0);
  CHECK(rig.bank.pending(kSlot));
  const std::uint32_t live_ls = rig.desc().loop_start;
  const std::uint32_t live_le = rig.desc().loop_end;
  // a moved START or END (the window) is not the ring's: nothing more is sent
  rig.tap(Button::F4);
  rig.invoke("wave.nudge", -2);
  rig.passes(2);
  CHECK(rig.applied == 2u);
  // the voice ends; the handover rebuilds from the project (END moved, loop points as they were): the loop is the one the live moves made
  rig.voices->cut(0);
  rig.block();
  rig.block();
  rig.passes(8);
  CHECK_FALSE(rig.bank.pending(kSlot));
  CHECK(rig.desc().loop_start == live_ls);
  CHECK(rig.desc().loop_end == live_le);
}

TEST_CASE("live loop (app): after a window edit waits for the handover, a loop edit sends no live move (counted from the new window it would land in the old one); after the handover it does again") {
  Rig rig;
  NEED(rig.desc().loop_end != 0u);
  CHECK(rig.voices->note_on(0, 60, kSlot, 127));
  rig.block();
  rig.tap(Button::F4);  // END: the window moves, the handover waits for the voice
  rig.invoke("wave.zc", 0);
  rig.invoke("wave.nudge", -2);
  rig.passes(2);
  CHECK(rig.bank.stale(kSlot));
  rig.tap(Button::F2);  // LOOP START, while the descriptor still has the old window
  rig.invoke("wave.nudge", 3);
  rig.passes(2);
  CHECK(rig.applied == 0u);  // no live move: the handover takes it
  CHECK(rig.bank.pending(kSlot));
  rig.voices->cut(0);  // the voice ends: the handover rebuilds window and loop from the project
  rig.block();
  rig.block();
  rig.passes(8);
  CHECK_FALSE(rig.bank.pending(kSlot));
  CHECK_FALSE(rig.bank.stale(kSlot));
  const std::uint32_t ls = rig.desc().loop_start;
  CHECK(rig.voices->note_on(0, 60, kSlot, 127));  // sounding again: a loop edit is live again
  rig.block();
  rig.invoke("wave.nudge", 3);
  rig.passes(2);
  CHECK(rig.applied == 1u);
  CHECK(rig.desc().loop_start > ls);
  CHECK_FALSE(rig.bank.stale(kSlot));
}
