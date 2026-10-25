// SPDX-License-Identifier: GPL-3.0-or-later
//
// An imported-song shaped pattern end to end (OS-005h, OS-008c): the sequencer as the event source with its Tick events on (core FX 43, default 6 ticks a step), the engine FX
// 128 + a MED command in the step's FX column, the Amiga voices and the graph, rendered block by block at 120 BPM. It holds the contracts the voices are built on: the row's FX
// reach the voice before its note (the note's tick 0 runs the command), the Tick events come after, a command holds until its column changes, and the E00 00 the importer writes
// ends it. Host only (the project is 1.9 MB).
#include <doctest/doctest.h>

#include <o3os/audio/amiga_voices.h>
#include <o3os/audio/engine.h>
#include <o3os/audio/voices.h>
#include <o3os/hal/audio.h>
#include <o3os/model/project.h>
#include <o3os/seq/engine_link.h>
#include <o3os/seq/sequencer.h>

#include "support/need.h"
#include "support/static_slot.h"

#include <cstdint>

namespace {

namespace haudio = o3os::hal::audio;
using namespace o3os;

std::uint32_t fake_cycles_counter = 0;
std::uint32_t fake_cycles() noexcept { return fake_cycles_counter += 11u; }

model::Step step(unsigned note_byte, unsigned instr, unsigned fx0 = 0, unsigned v0 = 0, unsigned fx1 = 0, unsigned v1 = 0) {
  model::Step s = model::kEmptyStep;
  s.note = static_cast<std::uint8_t>(note_byte);
  s.instr = static_cast<std::uint8_t>(instr);
  s.fx[0] = {static_cast<std::uint8_t>(fx0), static_cast<std::uint8_t>(v0)};
  s.fx[1] = {static_cast<std::uint8_t>(fx1), static_cast<std::uint8_t>(v1)};
  return s;
}

}  // namespace

TEST_CASE("amiga song: engine FX and Tick events from the sequencer drive the Paula voice like MED does") {
  static model::ModelArena arena;
  model::Project& project = arena.project;
  project.reset();
  CHECK(project.create_pattern(1, 8) == model::Status::Ok);
  CHECK(project.append_order(1) == model::Status::Ok);
  model::Instrument inst;
  model::init_instrument(inst, model::InstrumentKind::Sample);
  CHECK(project.set_instrument(0, inst) == model::Status::Ok);
  // row 0: C-2 (note byte 61) with slide up 4 (E01 04) | row 1: nothing, the terminator E00 00 | row 2: a note with vibrato E04 46 | row 3: the terminator
  CHECK(project.set_step(1, 0, 0, step(61, 1, 129, 4)) == model::Status::Ok);
  CHECK(project.set_step(1, 0, 1, step(0, 0, 128, 0)) == model::Status::Ok);
  CHECK(project.set_step(1, 0, 2, step(61, 1, 132, 0x46)) == model::Status::Ok);
  CHECK(project.set_step(1, 0, 3, step(0, 0, 128, 0)) == model::Status::Ok);

  static std::int16_t pcm[800];
  for (unsigned i = 0; i < 800; ++i) {
    pcm[i] = static_cast<std::int16_t>(((i % 64) * 700) - 20000);
  }
  static test::StaticSlot<audio::VoicePool> pool_slot;
  static test::StaticSlot<audio::AmigaVoices> amiga_slot;
  static test::StaticSlot<audio::Engine> engine_slot;
  static test::StaticSlot<seq::Sequencer> seq_slot;
  static audio::TapMem taps;
  static audio::SeqEvent events[audio::kMaxEventsPerBlock];
  audio::VoicePool& pool = pool_slot.make();
  audio::AmigaVoices& amiga = amiga_slot.make();
  audio::Engine& engine = engine_slot.make(&fake_cycles, taps);
  seq::Sequencer& sequencer = seq_slot.make(project);
  audio::SampleDesc d;
  d.data = pcm;
  d.length = 800;
  d.loop_end = 800;
  d.engine = audio::kEngineAmiga;
  pool.set_sample(0, d);
  engine.attach_voices(&pool);
  engine.attach_amiga(&amiga);
  engine.attach_events(seq::event_source(sequencer, events, audio::kMaxEventsPerBlock));
  seq::Sequencer::Config cfg = sequencer.config();
  cfg.emit_ticks = true;
  sequencer.configure(cfg);

  alignas(8) std::int16_t in[haudio::kBlockSamples] = {};
  alignas(8) std::int16_t out[haudio::kBlockSamples] = {};
  haudio::BlockReport report;
  const auto block = [&]() {
    report = haudio::BlockReport{};
    const haudio::BlockIo io{in, out, 0, &report};
    engine.render_block(io);
  };
  CHECK(sequencer.commands().push(seq::Command{seq::Command::Kind::PlayPattern, 1, 0, 0}));
  // Rows are counted by Tick events (six a row): the period is read when the sixth, twelfth, ... tick of the song has run.
  std::uint32_t at_row_end[4] = {1, 1, 1, 1};
  std::uint32_t seen = 0;
  for (std::uint32_t b = 0; b < 86u * 5u && seen < 4u; ++b) {
    block();
    const std::uint32_t t = amiga.stats().ticks;
    if (t >= 6u * (seen + 1u)) {
      at_row_end[seen++] = amiga.track_state(0).period();
    }
  }
  CHECK(amiga.stats().note_ons == 2);
  CHECK(amiga.stats().ticks >= 18);
  CHECK(at_row_end[0] == 428 - 6 * 4);  // the note's tick 0 and ticks 1..5: six slides of 4
  CHECK(at_row_end[1] == 428 - 6 * 4);  // the terminator E00 00 ended the slide: the period stays
  CHECK(at_row_end[2] == 428);          // a new note; the vibrato (E04) moves the output period, not the track's
  CHECK(at_row_end[3] == 428);
  CHECK(amiga.voices_in_use() == 1);
  CHECK(pool.voices_in_use() == 0);
  CHECK(out[2 * 63] != 0);  // sound reached the graph's output
}
