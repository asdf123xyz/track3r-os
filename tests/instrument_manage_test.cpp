// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSTUI S5: the type chooser of an empty slot, DELETE (Shift + F4) and INIT (Shift + Delete) of the Instrument screen. The module side of DELETE (the instance that stays bound and must
// never sound again) is in module_ui_test.cpp and module_voices_test.cpp.
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/screens/params_screen.h>
#include <o3os/audio/voices.h>
#include <o3os/core/input_event.h>
#include <o3os/model/editor.h>
#include <o3os/model/params.h>
#include <o3os/platform/host/fakes.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace o3os;
using app::Button;
using app::DispatchResult;
using core::ActionArg;
namespace ip = model::instrument_param;

namespace {

alignas(8) std::uint8_t g_ring[256 * 1024];

constexpr unsigned kUsed = 4;    // instrument 5: a sample instrument with audio, edited
constexpr unsigned kFree = 11;   // instrument 12: empty
constexpr unsigned kHigh = 60;   // instrument 61: empty, in the 49..64 range
constexpr std::uint32_t kFrames = 8000;

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<audio::VoicePool> voices = std::make_unique<audio::VoicePool>();
  std::unique_ptr<model::ModelArena> arena = std::make_unique<model::ModelArena>();
  std::vector<std::int16_t> memory;
  std::vector<std::int16_t> peaks;
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  std::unique_ptr<app::SampleBankMem> mem = std::make_unique<app::SampleBankMem>();
  app::SampleBank bank;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);

  Rig() : memory(100000), peaks(20000) {
    arena->project.reset();
    bank.init(*voices, *arena, *mem, memory.data(), 100000, peaks.data(), peaks.size());
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Sample);
    std::strcpy(in.name, "KICK");
    std::strcpy(in.sample_path, "samples/kick.wav");
    in.sample_rate = 22050;
    in.sampler.start = 1000;
    in.sampler.end = 60000;
    in.sampler.slice_count = 2;
    in.sampler.slices[0] = 20000;
    in.sampler.slices[1] = 40000;
    NEED(arena->project.set_instrument(kUsed, in) == model::Status::Ok);
    std::uint32_t cap = 0;
    std::int16_t* p = bank.reserve(cap);
    NEED(p != nullptr);
    for (std::uint32_t i = 0; i < kFrames; ++i) {
      p[i] = static_cast<std::int16_t>((i % 64u) * 400 - 12000);
    }
    NEED(bank.commit_load(kUsed, p, kFrames));
    bank.rebind_now(kUsed);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(arena->project, journal);
    app->attach_document(editor.get());
    app->attach_samples(&bank);
    select(kUsed);
    tap(Button::Params);
  }
  void pass(unsigned ms = 16) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  void press(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true)); }
  void release(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false)); }
  void tap(Button b) {
    press(b);
    pass(1);
    release(b);
    pass(1);
  }
  void shift_tap(Button b) {
    press(Button::Shift);
    pass(1);
    tap(b);
    release(Button::Shift);
    pass(1);
  }
  DispatchResult invoke(const char* name, ActionArg arg = 0) { return app->invoke(core::action_id(name), arg); }
  void select(unsigned slot) {
    const_cast<app::ViewState&>(app->state()).instrument = static_cast<int>(slot) + 1;  // (instrument.select takes 1..48; the tests go to 64)
    pass();
  }
  const model::Instrument* inst(unsigned slot) const { return arena->project.instrument(slot); }
  std::string note() const { return app->state().notify.empty() ? std::string() : std::string(app->state().notify.head().text.c_str()); }
  void clear_notes() {
    pass(10000);
    pass(10000);
  }
  std::size_t depth() const { return editor->journal().depth(); }
  // The bank's quiet handover, as the App's pass does it (stopped, no live note, voices silent for kSettleMs): the marks are applied and cleared.
  void settle() {
    std::uint64_t now = 1000000;
    for (int i = 0; i < 3 && bank.rebind_pending(); ++i) {
      (void)bank.poll(now, true, true, true);
      now += 100;
    }
  }
  app::ParamsKeyRow keys(bool shift = false) {
    app::ViewState v = app->state();
    v.shift = shift;
    app::ParamsKeyRow row;
    app::params_key_row(app->model(), v, row);
    return row;
  }
};

}  // namespace

TEST_CASE("chooser: an empty slot of an open project is the type chooser, with its own key row and context") {
  Rig rig;
  rig.select(kFree);
  rig.pass();
  CHECK(app::kParamsScreen.field_context(rig.app->model(), rig.app->state()) == app::Context::InstrumentEmpty);
  CHECK(app::kParamsScreen.count(rig.app->model(), rig.app->state()) == 0);  // no rows: nothing to edit
  const app::ParamsKeyRow row = rig.keys();
  CHECK(std::string(row.label[0]) == "SAMPLE");
  CHECK(std::string(row.label[1]) == "MIDI");
  CHECK(std::string(row.label[2]) == "PASTE");
  CHECK(std::string(row.label[3]) == "MODULE");
  CHECK(std::string(row.label[4]) == "LOAD");
  CHECK((row.dim & 0x1Fu) == (1u << 2));  // PASTE is dim while nothing is copied (S7); LOAD is live
  const app::ParamsKeyRow shifted = rig.keys(true);
  CHECK((shifted.dim & 0x08u) != 0u);  // DELETE has nothing to delete here
  // a used slot is not a chooser
  rig.select(kUsed);
  rig.pass();
  CHECK(app::kParamsScreen.field_context(rig.app->model(), rig.app->state()) == app::Context::None);
  CHECK((rig.keys(true).dim & 0x08u) == 0u);
}

TEST_CASE("chooser: F3 PASTE with nothing copied says so and changes nothing (S7 builds the rest: instrument_clip_test.cpp)") {
  Rig rig;
  rig.select(kFree);
  const std::size_t depth = rig.depth();
  rig.tap(Button::F3);
  CHECK(rig.note() == "NOTHING TO PASTE: SHIFT+F2 COPIES");
  CHECK(rig.inst(kFree) == nullptr);
  CHECK(rig.depth() == depth);
}

TEST_CASE("chooser: F2 MIDI makes a MIDI instrument on the slot's channel, in one undo unit") {
  Rig rig;
  rig.select(kFree);  // instrument 12: slots 1..48 are channel 1
  const std::size_t depth = rig.depth();
  rig.tap(Button::F2);
  NEED(rig.inst(kFree) != nullptr);
  CHECK(rig.inst(kFree)->kind == model::InstrumentKind::Midi);
  CHECK(rig.inst(kFree)->midi_channel == 1);
  CHECK(rig.note() == "MIDI INSTRUMENT ON CHANNEL 1");
  CHECK(rig.depth() == depth + 1u);
  CHECK(!model::validate_instrument(*rig.inst(kFree), 0));
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.inst(kFree) == nullptr);
  // 49..64 count from 1 again: instrument 61 is channel 13, 64 is 16, 49 is 1
  for (const unsigned slot : {48u, 59u, 63u}) {
    rig.select(slot);
    rig.tap(Button::F2);
    NEED(rig.inst(slot) != nullptr);
    CHECK(rig.inst(slot)->midi_channel == slot - 47u);
  }
  // the chooser fills an empty slot only (O7): on a used one the action does nothing
  rig.select(kUsed);
  const std::size_t d2 = rig.depth();
  CHECK(rig.invoke("instrument.new_midi") == DispatchResult::Unchanged);
  CHECK(rig.depth() == d2);
  CHECK(rig.inst(kUsed)->kind == model::InstrumentKind::Sample);
}

TEST_CASE("chooser: F1 SAMPLE opens Sample load for this slot, and leaving the screen any other way forgets the way back") {
  Rig rig;
  rig.select(kFree);
  rig.tap(Button::F1);
  CHECK(rig.app->state().screen == app::ScreenId::SampleLoad);
  CHECK(rig.app->state().wav.back_to_params);
  CHECK(rig.app->state().instrument == static_cast<int>(kFree) + 1);  // the load goes to the instrument in view
  rig.tap(Button::Params);
  CHECK(rig.app->state().screen == app::ScreenId::Instrument);
  CHECK(!rig.app->state().wav.back_to_params);
  // a used slot has no such key
  rig.select(kUsed);
  CHECK(rig.invoke("instrument.sample") == DispatchResult::Changed);  // (the action is the chooser's; the key is only bound on an empty slot)
  rig.tap(Button::Params);
  rig.select(kUsed);
  rig.tap(Button::F1);
  CHECK(rig.app->state().screen == app::ScreenId::Instrument);
}

TEST_CASE("delete: Shift + F4 takes the instrument out in one undo unit, the sample stays in the arena, undo and redo bring it and take it again") {
  Rig rig;
  rig.select(kUsed);
  const model::Instrument before = *rig.inst(kUsed);
  const std::size_t depth = rig.depth();
  rig.shift_tap(Button::F4);
  CHECK(rig.inst(kUsed) == nullptr);
  CHECK(rig.note() == "INS 05 DELETED - F8 UNDO");
  CHECK(rig.depth() == depth + 1u);
  CHECK(rig.bank.pcm(kUsed).frames == kFrames);  // nothing on the card or in the arena is touched
  rig.bank.rebind_now(kUsed);  // the next quiet handover
  CHECK(rig.voices->sample(kUsed).length == 0u);
  CHECK(app::kParamsScreen.field_context(rig.app->model(), rig.app->state()) == app::Context::InstrumentEmpty);  // the chooser
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  NEED(rig.inst(kUsed) != nullptr);
  CHECK(std::memcmp(rig.inst(kUsed), &before, sizeof before) == 0);
  CHECK(rig.bank.rebind_pending());  // undo asks for the handover: the slot is the sample's again
  rig.bank.rebind_now(kUsed);
  CHECK(rig.voices->sample(kUsed).length != 0u);
  CHECK(rig.invoke("edit.redo") == DispatchResult::Changed);
  CHECK(rig.inst(kUsed) == nullptr);
  CHECK(rig.depth() == depth + 1u);
  // nothing to delete on an empty slot
  rig.select(kFree);
  const std::size_t d2 = rig.depth();
  rig.clear_notes();
  rig.shift_tap(Button::F4);
  CHECK(rig.note() == "NOTHING TO DELETE");
  CHECK(rig.depth() == d2);
}

TEST_CASE("delete: a new instrument goes into the freed slot, and its sample load is not blocked") {
  Rig rig;
  rig.select(kUsed);
  rig.shift_tap(Button::F4);
  rig.tap(Button::F2);
  NEED(rig.inst(kUsed) != nullptr);
  CHECK(rig.inst(kUsed)->kind == model::InstrumentKind::Midi);
  CHECK(rig.inst(kUsed)->midi_channel == 1);
}

TEST_CASE("init: every parameter back to its default; kind, sample, name and the Wave data stay; one undo unit") {
  Rig rig;
  rig.select(kUsed);
  model::Instrument edited = *rig.inst(kUsed);
  edited.sampler.volume = 20;
  edited.sampler.cutoff = 0.2f;
  edited.sampler.filter_enable = 1;
  edited.sampler.resonance = 2.0f;
  edited.sampler.envelope[2].amount = 0.3f;
  edited.sampler.envelope[2].enable = 1;
  edited.sampler.lfo[1].amount = 0.8f;  // (OS-INSTUI S6: the LFOs are rows of the MOD page now: INIT resets them)
  edited.sampler.envelope[3].delay = 7;   // (the delay and the hold are no rows: INIT resets them too)
  edited.sampler.envelope[3].hold = 9;
  edited.sampler.envelope[1].loop = 1;
  edited.sampler.play_mode = 6;
  edited.sampler.sample_type = 1;
  edited.sampler.grain_length = 800;
  edited.sampler.selected_slice = 1;
  edited.engine = model::kInstEngineBits8;
  NEED(rig.editor->set_instrument(kUsed, edited) == model::Status::Ok);
  const std::size_t depth = rig.depth();
  const model::Instrument before = *rig.inst(kUsed);
  rig.shift_tap(Button::Delete);
  CHECK(rig.note() == "INS 05 INIT - F8 UNDO");
  CHECK(rig.depth() == depth + 1u);
  const model::Instrument& now = *rig.inst(kUsed);
  CHECK(now.kind == model::InstrumentKind::Sample);
  CHECK(std::strcmp(now.name, "KICK") == 0);
  CHECK(std::strcmp(now.sample_path, "samples/kick.wav") == 0);
  CHECK(now.sample_rate == 22050u);
  CHECK((now.sampler.start == 1000 && now.sampler.end == 60000));
  CHECK((now.sampler.slice_count == 2 && now.sampler.slices[1] == 40000));  // the Wave screen's data
  CHECK(rig.bank.pcm(kUsed).frames == kFrames);                              // and the audio
  // every descriptor of the tables is at its default (the slice count is the readout of what Wave made)
  unsigned checked = 0;
  for (unsigned slot = 0; slot < ip::kCount; ++slot) {
    const unsigned field = model::param_field_of_slot(core::ParamKind::Instrument, slot);
    const core::ParamId id = core::param_id(core::ParamKind::Instrument, kUsed, field);
    const core::ParamDesc* d = model::param_desc(id);
    if (d == nullptr || field == ip::Slices) {
      continue;
    }
    CHECK_MESSAGE(rig.arena->project.get_param(id) == d->def, d->name);
    ++checked;
  }
  CHECK(checked == ip::kCount - 1u);
  CHECK(now.sampler.sample_type == 0);
  CHECK(now.sampler.lfo[1].amount == 0.5f);  // our default
  CHECK((now.sampler.envelope[3].delay == 0 && now.sampler.envelope[3].hold == 0 && now.sampler.envelope[1].loop == 0));
  // again: already there
  rig.clear_notes();
  rig.shift_tap(Button::Delete);
  CHECK(rig.note() == "ALREADY AT THE DEFAULTS");
  CHECK(rig.depth() == depth + 1u);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(std::memcmp(rig.inst(kUsed), &before, sizeof before) == 0);
  // on an empty slot there is nothing to init
  rig.select(kFree);
  rig.clear_notes();
  rig.shift_tap(Button::Delete);
  CHECK(rig.note() == "NOTHING TO INIT");
}

TEST_CASE("init: a MIDI instrument keeps its channel and name") {
  Rig rig;
  rig.select(kHigh);
  rig.tap(Button::F2);
  NEED(rig.inst(kHigh) != nullptr);
  model::Instrument edited = *rig.inst(kHigh);
  std::strcpy(edited.name, "SYNTH");
  edited.sampler.volume = 10;
  NEED(rig.editor->set_instrument(kHigh, edited) == model::Status::Ok);
  rig.shift_tap(Button::Delete);
  CHECK(rig.inst(kHigh)->midi_channel == 13);
  CHECK(std::strcmp(rig.inst(kHigh)->name, "SYNTH") == 0);
  CHECK(rig.inst(kHigh)->sampler.volume == 50);
  CHECK(!model::validate_instrument(*rig.inst(kHigh), 0));
}

TEST_CASE("init: reset_instrument_params leaves what is not a parameter alone and gives init_instrument's values to what is") {
  model::Instrument fresh;
  model::init_instrument(fresh, model::InstrumentKind::Sample);
  model::Instrument x = fresh;
  std::strcpy(x.name, "N");
  x.sampler.volume = 3;
  x.sampler.glide = 100;
  x.sampler.envelope[0].attack = 900;
  x.sampler.envelope[5].release = 5;
  x.sampler.play_mode = 7;
  x.engine = model::kInstEngineAmiga;
  x.sampler.start = 7;
  model::reset_instrument_params(x);
  CHECK(x.sampler.volume == fresh.sampler.volume);
  CHECK(x.sampler.glide == fresh.sampler.glide);
  CHECK(x.sampler.envelope[0].attack == fresh.sampler.envelope[0].attack);
  CHECK(x.sampler.envelope[5].release == fresh.sampler.envelope[5].release);
  CHECK(x.sampler.play_mode == 0);
  CHECK(x.engine == model::kInstEngineAuto);
  CHECK(x.sampler.start == 7);
  CHECK(std::strcmp(x.name, "N") == 0);
}

TEST_CASE("undo and redo of INIT reach the voices: the slot is rebound to what the unit restored (not only the project)") {
  Rig rig;
  rig.select(kUsed);
  model::Instrument edited = *rig.inst(kUsed);
  edited.sampler.volume = 20;
  edited.sampler.play_mode = 6;  // WAVETABLE: the descriptor has a window
  edited.sampler.sample_type = 1;
  NEED(rig.editor->set_instrument(kUsed, edited) == model::Status::Ok);
  rig.bank.request_rebind(kUsed);
  rig.settle();
  const audio::SampleDesc edited_desc = rig.voices->sample(kUsed);
  CHECK(edited_desc.wt_size != 0u);
  rig.shift_tap(Button::Delete);  // INIT
  rig.settle();
  const audio::SampleDesc init_desc = rig.voices->sample(kUsed);
  CHECK(init_desc.wt_size == 0u);
  CHECK(init_desc.gain != edited_desc.gain);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.bank.rebind_pending());  // the unit replaced the whole instrument: the applied list does not carry it, the slot is rebound
  rig.settle();
  CHECK(rig.voices->sample(kUsed).wt_size == edited_desc.wt_size);
  CHECK(rig.voices->sample(kUsed).gain == edited_desc.gain);
  CHECK(rig.invoke("edit.redo") == DispatchResult::Changed);
  CHECK(rig.bank.rebind_pending());
  rig.settle();
  CHECK(rig.voices->sample(kUsed).wt_size == 0u);
  CHECK(rig.voices->sample(kUsed).gain == init_desc.gain);
}

TEST_CASE("delete during playback: the sample goes on sounding until the next quiet handover, which then silences the slot") {
  Rig rig;
  rig.select(kUsed);
  rig.voices->begin_block();
  CHECK(rig.voices->note_on(0, 60, kUsed, audio::kVelocityDefault));
  rig.voices->render(0, audio::VoicePool::kBlock);
  rig.voices->end_block();
  CHECK(rig.voices->voices_in_use() != 0u);
  rig.shift_tap(Button::F4);
  CHECK(rig.inst(kUsed) == nullptr);
  CHECK(rig.bank.rebind_pending());
  CHECK(rig.voices->sample(kUsed).length != 0u);  // the descriptor is not touched while a voice reads it
  CHECK(rig.voices->track_playing(0));
  // the handover waits for the voices: while one sounds, nothing is applied (poll refuses, however long it waits)
  CHECK(!rig.bank.poll(2000000, true, true, true));
  CHECK(!rig.bank.poll(2000100, true, true, true));
  CHECK(rig.voices->sample(kUsed).length != 0u);
  CHECK(rig.voices->track_playing(0));
  rig.voices->cut(0);  // playback ends (a stop cuts the voices) and the voice fades out in the render
  for (int i = 0; i < 200; ++i) {
    rig.voices->begin_block();
    rig.voices->render(0, audio::VoicePool::kBlock);
    rig.voices->end_block();
  }
  CHECK(rig.voices->voices_in_use() == 0u);
  CHECK(!rig.bank.poll(3000000, true, true, true));  // quiet now: the settle starts here
  CHECK(rig.bank.poll(3000000 + app::SampleBank::kSettleMs, true, true, true));
  CHECK(rig.voices->sample(kUsed).length == 0u);  // the slot is silent
}
