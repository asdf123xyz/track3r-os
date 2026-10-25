// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-FU5: undo and redo of an engine change (the PLAY ENGINE row, instrument.engine) rebind the slot at once, from any screen: the voices follow the instrument the history just changed.
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/sample_bank.h>
#include <o3os/audio/voices.h>
#include <o3os/core/action.h>
#include <o3os/model/editor.h>
#include <o3os/platform/host/fakes.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

using namespace o3os;

namespace {

alignas(8) std::uint8_t g_ring[64 * 1024];

TEST_CASE("engine undo: undo and redo of an engine change request a rebind of that slot, from the Pattern screen too") {
  constexpr unsigned kSlot = 4;
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  auto voices = std::make_unique<audio::VoicePool>();
  auto arena = std::make_unique<model::ModelArena>();
  auto mem = std::make_unique<app::SampleBankMem>();
  std::vector<std::int16_t> memory(100000);
  std::vector<std::int16_t> peaks(10000);
  app::SampleBank bank;
  arena->project.reset();
  bank.init(*voices, *arena, *mem, memory.data(), static_cast<std::uint32_t>(memory.size()), peaks.data(), peaks.size());
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Sample);
  NEED(arena->project.set_instrument(kSlot, in) == model::Status::Ok);
  std::uint32_t cap = 0;
  std::int16_t* pcm = bank.reserve(cap);
  NEED(pcm != nullptr);
  for (unsigned i = 0; i < 1000; ++i) {
    pcm[i] = static_cast<std::int16_t>(i * 13);
  }
  NEED(bank.commit_load(kSlot, pcm, 1000));
  const auto drain = [&]() {
    std::uint64_t t = 1000;
    for (unsigned i = 0; i < 4; ++i) {
      (void)bank.poll(t, true);
      t += 10000;
    }
  };
  drain();
  model::UndoJournal journal;
  journal.begin(g_ring, sizeof g_ring);
  model::Editor editor(arena->project, journal);
  auto app = std::make_unique<app::App>(input, clock, display);
  app->attach_document(&editor);
  app->attach_samples(&bank);
  app->invoke(core::action_id("instrument.select"), static_cast<core::ActionArg>(kSlot + 1));
  CHECK(app->state().screen != app::ScreenId::Wave);
  CHECK(app->state().screen != app::ScreenId::SampleEdit);
  CHECK_FALSE(bank.rebind_pending());
  CHECK(app->invoke(core::action_id("instrument.engine"), model::kInstEngineModern) == app::DispatchResult::Changed);
  CHECK(bank.rebind_pending());  // the choice
  drain();
  CHECK_FALSE(bank.rebind_pending());
  CHECK(app->invoke(core::action_id("edit.undo")) == app::DispatchResult::Changed);
  CHECK(arena->project.instrument(kSlot)->engine == model::kInstEngineAuto);
  CHECK(bank.rebind_pending());  // its undo
  drain();
  CHECK(app->invoke(core::action_id("edit.redo")) == app::DispatchResult::Changed);
  CHECK(arena->project.instrument(kSlot)->engine == model::kInstEngineModern);
  CHECK(bank.rebind_pending());  // its redo
}

}  // namespace
