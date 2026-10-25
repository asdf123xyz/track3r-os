// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-GOV: the CPU governor of the module instruments (audio/module_governor.h): the admission truth table, the steal order, the drop, the cost estimator (a moving max with slow decay, a cold
// instance), the safety net (mute after N blocks over the budget, the cool-down, the recovery, a reload), and 12 notes at once through the whole engine, admitted only up to the budget.
// The modules are native ones that cost what the test tells them to: their render() advances a fake cycle counter (the stand-in for the DWT counter), so the governor measures them as it
// would on the unit. Host and Cortex-M4 (QEMU): the real TB-303 variant of the 12-note case runs where the module code can run (the target suite), with its cost simulated the same way.
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/null_storage.h"
#include "support/static_slot.h"

#include <o3os/audio/diag.h>
#include <o3os/audio/engine.h>
#include <o3os/audio/module_governor.h>
#include <o3os/audio/module_voices.h>
#include <o3os/audio/taps.h>
#include <o3os/audio/voices.h>
#include <o3os/module/loader.h>
#if defined(__arm__)
#include <o3os/platform/qemu/clock.h>
#endif

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {

using namespace o3os;
using audio::Engine;
using audio::EventKind;
using audio::ModuleVoices;
using audio::SeqEvent;
using audio::VoicePool;
namespace gov = o3os::audio::gov;
namespace haudio = o3os::hal::audio;

constexpr std::uint32_t kBlk = haudio::kBlock;
constexpr std::uint32_t kTb303 = 42700;  // the TB-303's measured cycles for a whole block, one voice (OS-PERF3, on the unit)

// ---- a module that costs what it is told ----------------------------------------------------------------------------------------------
std::uint32_t g_clock = 0;  // the fake cycle counter: the modules' render() advances it by their cost, a read adds g_step (the engine's own stamps)
std::uint32_t g_step = 0;
std::uint32_t g_reads = 0;  // and how many reads there were (OS-303PERF: the split's extra stamps are not read until it is switched on)
std::uint32_t clock_read() noexcept {
  ++g_reads;
  return g_clock += g_step;
}

struct alignas(16) CostState {
  std::uint32_t gate;
  std::uint32_t cost;  // cycles of a whole block (render() of n frames takes cost * n / 64)
  std::uint32_t notes;
  std::uint32_t offs;
  std::uint32_t last_note;
};
int cost_init(void*, const o3m_host*) { return 0; }
std::uint32_t cost_render(void* raw, const float*, float* out, std::uint32_t n) {
  CostState* c = static_cast<CostState*>(raw);
  for (std::uint32_t i = 0; i < n; ++i) {
    out[i] = c->gate != 0u ? 0.1f : 0.0f;
  }
  g_clock += c->cost * n / kBlk;
  return c->gate != 0u ? 1u : 0u;
}
void cost_set_param(void*, std::uint32_t, float) {}
void cost_note_on(void* raw, std::uint32_t note, std::uint32_t, std::uint32_t) {
  CostState* c = static_cast<CostState*>(raw);
  c->gate = 1;
  c->last_note = note;
  ++c->notes;
}
void cost_note_off(void* raw, std::uint32_t) {
  CostState* c = static_cast<CostState*>(raw);
  c->gate = 0;
  ++c->offs;
}
void cost_shutdown(void*) {}
const o3m_module kCost = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_INSTRUMENT, sizeof(o3m_module), "cost", sizeof(CostState), 0, nullptr,
                          cost_init, cost_render, cost_set_param, cost_note_on, cost_note_off, cost_shutdown, nullptr, 0};

// The module voices alone, a block driven by hand: the notes between begin_block() and the render, `base` the cycles the rest of the engine takes (the sample voices, the graph).
struct Unit {
  alignas(16) std::uint8_t scratch[ModuleVoices::kScratchBytes];
  alignas(16) CostState st[audio::kMaxModuleInstances];
  ModuleVoices* mv;

  explicit Unit(bool with_clock = true) {
    static o3os::test::StaticSlot<ModuleVoices> slot;
    mv = &slot.make();
    g_clock = 1000;
    g_step = 0;
    std::memset(scratch, 0, sizeof scratch);
    std::memset(st, 0, sizeof st);
    (void)mv->attach_scratch(scratch, sizeof scratch);
    if (with_clock) {
      mv->set_cycles(&clock_read);
    }
  }
  bool bind(unsigned slot, std::uint32_t cost) {
    st[slot].cost = cost;
    return mv->bind(slot, kCost, static_cast<std::uint16_t>(0x8100 + slot), &st[slot], nullptr, 0) == ModuleVoices::Bind::Ok;
  }
  template <typename F>
  void block(std::uint32_t base, F&& notes) {
    const std::uint32_t c0 = g_clock;
    mv->begin_block();
    notes();
    mv->render(0, kBlk);
    mv->end_block();
    mv->governor().observe_block(base + (g_clock - c0));
  }
  void block(std::uint32_t base = 0) {
    block(base, [] {});
  }
  ModuleVoices::Info info(unsigned slot) const {
    ModuleVoices::Info out{};
    for (unsigned i = 0; i < audio::kMaxModuleInstances; ++i) {
      ModuleVoices::Info in{};
      if (mv->info(i, in) && in.slot == slot) {
        out = in;
      }
    }
    return out;
  }
};

}  // namespace

// ---- the pure rules ----------------------------------------------------------------------------------------------------------------------

TEST_CASE("governor: the knobs: 90 % of the 261,120-cycle block (a tuning of the earlier 85 %), TB-303 16 %") {
  CHECK(gov::kBlockCycles == haudio::kCyclesPerBlock);
  CHECK(gov::kThresholdPercent == 90);
  CHECK(gov::kThresholdCycles == gov::kBlockCycles / 100u * 90u + gov::kBlockCycles % 100u * 90u / 100u);
  if (gov::kBlockCycles == 261120u) {
    CHECK(gov::kThresholdCycles == 235008u);
  }
  CHECK(gov::tenths_of_block(kTb303) / 10u == 16u);  // "TB-303 ~16 % per voice"
  CHECK(gov::tenths_of_block(0) == 0u);
  CHECK(gov::tenths_of_block(gov::kBlockCycles) == 1000u);
  CHECK(gov::tenths_of_block(gov::kBlockCycles * 3u) == 1000u);
  CHECK(gov::kColdCycles > kTb303);  // a module that was never measured is assumed heavier than the TB-303
}

TEST_CASE("governor: admission truth table (decide): a retrigger of a sounding instrument is always admitted, a new voice only if it fits") {
  constexpr std::uint32_t kThr = 200;
  struct Row {
    std::uint32_t load, cost;
    bool voice;
    gov::Verdict want;
  };
  const Row rows[] = {
      {100, 50, false, gov::Verdict::Admit},  // fits, a new voice
      {100, 50, true, gov::Verdict::Admit},   // fits, the instance has one (it is taken over: nothing is added)
      {150, 50, false, gov::Verdict::Admit},  // exactly on the line is under it
      {151, 50, false, gov::Verdict::Drop},   // one over: nothing starts
      {151, 50, true, gov::Verdict::Admit},   // one over, but it is a retrigger: no voice is added, no load either (the voice is in `load` already)
      {180, 50, false, gov::Verdict::Drop},
      {180, 50, true, gov::Verdict::Admit},
      {201, 50, true, gov::Verdict::Admit},   // already over the line: a retrigger is still free (refusing it would shed nothing: the old voice goes on sounding)
      {250, 50, false, gov::Verdict::Drop},
      {250, 50, true, gov::Verdict::Admit},
      {0, 200, false, gov::Verdict::Admit},   // a note that fills the block exactly
      {0, 201, false, gov::Verdict::Drop},
      {0, 201, true, gov::Verdict::Admit},
      {0xFFFFFFFFu, 1, false, gov::Verdict::Drop},  // no sum wraps
      {1, 0xFFFFFFFFu, false, gov::Verdict::Drop},
  };
  for (const Row& r : rows) {
    CAPTURE(r.load);
    CAPTURE(r.cost);
    CAPTURE(r.voice);
    CHECK(gov::decide(r.load, r.cost, r.voice, kThr) == r.want);
  }
  static_assert(gov::decide(0, 0, false, 0) == gov::Verdict::Admit, "");
  // The unit's reference (OS-PERF3, 0.4.7): song + 12 pads about 72 % of the block, a TB-303 voice 16 %: a NEW 303 voice over that mix is admitted at the 90 % line (72 + 16.4 = 88.4) and not over a
  // 76 % one (92.4); its retrigger while it sounds always is.
  const std::uint32_t seventy_two = gov::cycles_of_percent(72);
  const std::uint32_t seventy_six = gov::cycles_of_percent(76);
  CHECK(gov::decide(seventy_two, kTb303, false, gov::kThresholdCycles) == gov::Verdict::Admit);
  CHECK(gov::decide(seventy_six, kTb303, false, gov::kThresholdCycles) == gov::Verdict::Drop);
  CHECK(gov::decide(seventy_six + kTb303, kTb303, true, gov::kThresholdCycles) == gov::Verdict::Admit);
}

TEST_CASE("governor: the estimator is a moving max that ages with the blocks, idle ones included; per voice and per whole block") {
  // the max of a series up at once, down slowly (the base and the recent worst render)
  CHECK(gov::track_max(0, 40000, gov::kWorstDecayShift) == 40000u);
  CHECK(gov::track_max(40000, 50000, gov::kWorstDecayShift) == 50000u);
  CHECK(gov::track_max(50000, 10000, gov::kWorstDecayShift) == 50000u - 50000u / 256u);
  // an estimate halves every 256 blocks, linearly in between, whatever the blocks were
  CHECK(gov::decayed_cost(50000, 0) == 50000u);
  CHECK(gov::decayed_cost(50000, 256) == 25000u);
  CHECK(gov::decayed_cost(50000, 512) == 12500u);
  CHECK(gov::decayed_cost(50000, 1024) == 3125u);
  CHECK(gov::decayed_cost(50000, 128) < 50000u);
  CHECK(gov::decayed_cost(50000, 128) > 25000u);
  CHECK(gov::decayed_cost(50000, 255 + 256 * 40) == 0u);
  CHECK(gov::decayed_cost(0xFFFFFFFFu, 100) <= 0xFFFFFFFFu);  // (no overflow at the largest estimate)
  std::uint32_t last = 140000;
  for (std::uint32_t e = 1; e < 3000; ++e) {
    const std::uint32_t d = gov::decayed_cost(140000, e);
    CHECK(d <= last);  // never rises
    last = d;
  }
  // what admission uses: aged, but not below min(estimate, the cold assumption): a stale outlier is forgiven down to the cold assumption, a long-idle 303 is still a 303, a cheap module keeps its figure
  CHECK(gov::aged_cost(0, 100000) == 0u);  // never measured stays "never measured"
  CHECK(gov::aged_cost(kTb303, 0) == kTb303);
  CHECK(gov::aged_cost(kTb303, 100000) == kTb303);  // (under the cold assumption: not aged at all)
  CHECK(gov::aged_cost(140000, 0) == 140000u);
  CHECK(gov::aged_cost(140000, 256) == 70000u);
  CHECK(gov::aged_cost(140000, 1024) == gov::kColdCycles);
  CHECK(gov::aged_cost(140000, 100000) == gov::kColdCycles);
  CHECK(gov::aged_cost(8000, 100000) == 8000u);  // (a 3 % module)
  // the figure of one block: scaled to a whole block, divided by the voices, too few frames say nothing
  CHECK(gov::voice_block_cost(42700, 64, 1) == 42700u);
  CHECK(gov::voice_block_cost(21350, 32, 1) == 42700u);  // half a block of a note that starts mid-block
  CHECK(gov::voice_block_cost(85400, 64, 2) == 42700u);  // two voices
  CHECK(gov::voice_block_cost(1000, 31, 1) == 0u);       // under kMinFrames: skipped
  CHECK(gov::voice_block_cost(1000, 64, 0) == 0u);       // no voice
  CHECK(gov::voice_block_cost(0xFFFFFFF0u, 64, 1) == 0xFFFFFFFFu / 64u);  // a module that never came back: a huge figure (250 blocks of cycles), not a wrapped one
  CHECK(gov::voice_block_cost(0xFFFFFFF0u, 64, 1) > 100u * gov::kBlockCycles);
}

// ---- the module voices under the governor ---------------------------------------------------------------------------------------------------

TEST_CASE("governor: a cold instance is judged on the cold assumption until it has been measured (two blocks), then on what it measured") {
  Unit u;
  for (unsigned s = 0; s < 6; ++s) {
    NEED(u.bind(s, kTb303));
  }
  CHECK(u.mv->voice_cost(0) == 0u);  // never measured
  // Block 0: six notes at once on six cold instances at 20 % each: four fit under the 90 % line (4 x 20 = 80), the fifth and sixth do not.
  bool started[6] = {};
  u.block(0, [&] {
    for (unsigned s = 0; s < 6; ++s) {
      started[s] = u.mv->note_on(s, 60 + s, s, 100);
    }
  });
  CHECK(started[0]);
  CHECK(started[1]);
  CHECK(started[2]);
  CHECK(started[3]);
  CHECK(!started[4]);
  CHECK(!started[5]);
  const gov::Snapshot g = u.mv->governor().snapshot();
  CHECK(g.admitted == 4u);
  CHECK(g.drops == 2u);
  CHECK(g.last == gov::Event::Drop);
  CHECK(g.last_slot == 5u);
  CHECK(u.mv->voice_cost(0) == 0u);  // one block says nothing yet: the estimate takes min(this block, the previous one)
  u.block(0);
  // The second block confirmed the four: the figure is the module's own, whatever the cold assumption was.
  for (unsigned s = 0; s < 4; ++s) {
    CHECK(u.mv->voice_cost(s) == kTb303);
  }
  CHECK(u.mv->voice_cost(4) == 0u);  // refused, so never measured
  CHECK(u.mv->governor().snapshot().worst == 4u * kTb303);
  CHECK(u.mv->governor().snapshot().base == 0u);  // (what the render takes besides the modules: nothing in this rig)
}

TEST_CASE("governor: one voice an instrument: a note for a sounding instrument is a retrigger and always admitted; a new voice that does not fit is dropped and releases its track") {
  Unit u;
  NEED(u.bind(0, kTb303));  // A
  NEED(u.bind(1, kTb303));  // B
  NEED(u.bind(2, kTb303));  // C, never sounded
  u.block(0, [&] {
    CHECK(u.mv->note_on(0, 60, 0, 100));  // A on track 1
    CHECK(u.mv->note_on(1, 62, 1, 100));  // B on track 2
  });
  u.block(0);
  // A heavy mix besides the two voices: base 120,000 + 2 x 42,700 = 205,400 < the 235,008 line.
  for (unsigned b = 0; b < 4; ++b) {
    u.block(120000);
  }
  CHECK(u.mv->governor().snapshot().worst == 120000u + 2u * kTb303);
  CHECK(u.mv->governor().snapshot().base == 120000u);  // the modules' share is not in it: a voice's own cost is never counted twice
  const gov::Snapshot before = u.mv->governor().snapshot();
  // C has no voice and a new one does not fit (120,000 + the two that sound 85,400 + its cold 52,224 = 257,624): dropped, and the note that B was ringing on track 2 (where C's note was meant to go) ends.
  // A has one: its note from another track is a retrigger, admitted without a word.
  bool c_started = true;
  bool a_started = false;
  u.block(120000, [&] {
    c_started = u.mv->note_on(1, 64, 2, 100);  // C's note on track 2, B's track
    a_started = u.mv->note_on(5, 65, 0, 100);  // A again, from another track
  });
  CHECK(!c_started);
  CHECK(a_started);
  const gov::Snapshot g = u.mv->governor().snapshot();
  CHECK(g.drops == before.drops + 1u);
  CHECK(g.admitted == before.admitted);  // the retrigger added no voice: it is not counted
  CHECK(g.last == gov::Event::Drop);
  CHECK(g.last_slot == 2u);
  CHECK(u.st[2].notes == 0u);  // C never got a note
  CHECK(!u.info(2).held);
  CHECK(u.st[1].offs == 1u);   // B's note on the track the dropped note was for was released: nothing is left ringing on a track whose note was replaced
  CHECK(!u.info(1).held);
  CHECK(u.st[0].notes == 2u);  // A: its first note, and the retrigger
  CHECK(u.st[0].offs == 1u);   // the old note was ended first (a new note, not a slide)
  CHECK(u.st[0].last_note == 65u);
  CHECK(u.info(0).held);
  CHECK(u.info(0).track == 5u);  // the voice moved to the new track: the old track's block went silent
  CHECK(u.mv->feed(0) == nullptr);
  CHECK(u.mv->feed(5) != nullptr);
  // Over the line already (a heavier mix): a retrigger adds nothing, so it is still admitted (a drop would not shed the old voice), and still not a drop.
  for (unsigned b = 0; b < 3; ++b) {
    u.block(200000);
  }
  CHECK(u.mv->governor().snapshot().worst > gov::kThresholdCycles);
  const std::uint32_t notes_before = u.st[0].notes;
  bool again = false;
  u.block(200000, [&] { again = u.mv->note_on(6, 67, 0, 100); });
  CHECK(again);
  CHECK(u.st[0].notes == notes_before + 1u);
  CHECK(u.info(0).held);
  CHECK(u.info(0).track == 6u);
  CHECK(u.mv->governor().snapshot().drops == g.drops);
}

TEST_CASE("governor: without a cycle counter there is no measure and no governor: every note starts") {
  Unit u(false);
  for (unsigned s = 0; s < 8; ++s) {
    NEED(u.bind(s, kTb303));
  }
  unsigned started = 0;
  u.block(0, [&] {
    for (unsigned s = 0; s < 8; ++s) {
      started += u.mv->note_on(s, 60, s, 100) ? 1u : 0u;
    }
  });
  CHECK(started == 8u);
  const gov::Snapshot g = u.mv->governor().snapshot();
  CHECK(g.admitted == 0u);
  CHECK(g.drops == 0u);
  CHECK(u.mv->voice_cost(0) == 0u);
}

TEST_CASE("governor: a note that starts in the last frames of a block says too little; a split block is brought back to a whole one") {
  Unit u;
  NEED(u.bind(0, kTb303));
  u.mv->begin_block();
  u.mv->render(0, 40);  // nothing sounds yet
  CHECK(u.mv->note_on(0, 60, 0, 100));
  u.mv->render(40, kBlk);  // 24 frames of the instance: under kMinFrames
  u.mv->end_block();
  CHECK(u.mv->voice_cost(0) == 0u);  // not measured from 24 frames
  u.block(0, [] {});                 // a whole block of the sounding voice: the first figure, which waits for its successor
  CHECK(u.mv->voice_cost(0) == 0u);
  u.block(0, [] {});
  CHECK(u.mv->voice_cost(0) == kTb303);
  // a note at frame 20: 44 frames of the instance are enough, and scale back to the whole block (within the rounding of the cycles per frame)
  Unit v;
  NEED(v.bind(0, kTb303));
  v.mv->begin_block();
  v.mv->render(0, 20);
  CHECK(v.mv->note_on(0, 60, 0, 100));
  v.mv->render(20, kBlk);
  v.mv->end_block();
  v.block(0, [] {});
  CHECK(v.mv->voice_cost(0) >= kTb303 - 70u);  // min(the split block's figure, the whole block's)
  CHECK(v.mv->voice_cost(0) <= kTb303);
}

TEST_CASE("governor: an instance over its time budget for N blocks in a row is muted, alone, and flagged; a refused note waits out the cool-down; a later note clears it") {
  Unit u;
  NEED(u.bind(0, 140000));  // X: 54 % of the block, over the 50 % budget
  NEED(u.bind(1, 20000));   // Y: a light one
  u.block(0, [&] {
    CHECK(u.mv->note_on(0, 60, 0, 100));
    CHECK(u.mv->note_on(1, 62, 1, 100));
  });
  CHECK(!u.mv->muted(0));  // 1 block over
  u.block(0);
  u.block(0);
  CHECK(!u.mv->muted(0));  // 3 blocks over: N - 1
  CHECK(u.mv->governor().snapshot().mutes == 0u);
  u.block(0);
  CHECK(u.mv->muted(0));  // 4: muted, at the end of that block
  CHECK(u.mv->governor().snapshot().mutes == 1u);
  CHECK(u.mv->governor().snapshot().last == gov::Event::Mute);
  CHECK(u.mv->governor().snapshot().last_slot == 0u);
  CHECK(u.info(0).muted);
  CHECK(!u.info(0).held);
  CHECK(u.st[0].gate == 0u);       // its note was ended
  CHECK(u.st[0].offs == 1u);
  CHECK(u.mv->feed(0) != nullptr);  // this block's audio of it is faded out, not cut (the fade test below)
  // Y plays on, untouched
  CHECK(!u.mv->muted(1));
  CHECK(u.info(1).held);
  CHECK(u.mv->feed(1) != nullptr);
  CHECK(u.st[1].offs == 0u);
  const std::uint32_t y_cost = u.mv->voice_cost(1);
  CHECK(y_cost == 20000u);
  // The muted instance is not rendered any more: the block's cost is Y's alone.
  const std::uint32_t before = g_clock;
  u.block(0);
  CHECK(g_clock - before == 20000u);
  CHECK(u.mv->feed(0) == nullptr);
  // A note during the cool-down is refused (and not counted as a drop: the mute already told)
  bool started = true;
  u.block(0, [&] { started = u.mv->note_on(0, 61, 0, 100); });
  CHECK(!started);
  CHECK(u.st[0].notes == 1u);
  CHECK(u.mv->governor().snapshot().refused == 1u);
  CHECK(u.mv->governor().snapshot().drops == 0u);
  CHECK(u.mv->muted(0));
  // Out of the cool-down: the note is the retrigger that clears the mute, and X is judged anew (muted again after N blocks while it is still heavy)
  for (unsigned b = 0; b < gov::kMuteCooldownBlocks; ++b) {
    u.block(0);
  }
  u.block(0, [&] { started = u.mv->note_on(0, 62, 0, 100); });
  CHECK(started);
  CHECK(!u.mv->muted(0));
  CHECK(u.st[0].notes == 2u);
  for (unsigned b = 0; b < gov::kMuteBlocks; ++b) {
    u.block(0);
  }
  CHECK(u.mv->muted(0));
  CHECK(u.mv->governor().snapshot().mutes == 2u);
  // Recovery: the module got cheaper (a parameter, say); after the next cool-down its note plays and it stays unmuted
  u.st[0].cost = 20000;
  for (unsigned b = 0; b < gov::kMuteCooldownBlocks; ++b) {
    u.block(0);
  }
  u.block(0, [&] { started = u.mv->note_on(0, 64, 0, 100); });
  CHECK(started);
  for (unsigned b = 0; b < 40; ++b) {
    u.block(0);
  }
  CHECK(!u.mv->muted(0));
  CHECK(u.info(0).held);
  CHECK(u.mv->governor().snapshot().mutes == 2u);
  // A reload (bind again after a project change) starts the instance cold and unmuted
  u.st[0].cost = 140000;
  u.block(0, [&] { (void)u.mv->note_on(0, 66, 0, 100); });
  for (unsigned b = 0; b < gov::kMuteBlocks + 1u; ++b) {
    u.block(0);
  }
  CHECK(u.mv->muted(0));
  u.mv->retire_all();
  u.mv->release_all();
  CHECK(!u.mv->muted(0));
  NEED(u.bind(0, 20000));
  CHECK(!u.mv->muted(0));
  CHECK(u.mv->voice_cost(0) == 0u);
  bool after_reload = false;
  u.block(0, [&] { after_reload = u.mv->note_on(0, 60, 0, 100); });
  CHECK(after_reload);
}

TEST_CASE("governor: an isolated heavy block does not mute: the blocks over the budget must be N in a row") {
  Unit u;
  NEED(u.bind(0, 100000));
  u.block(0, [&] { CHECK(u.mv->note_on(0, 60, 0, 100)); });
  for (unsigned round = 0; round < 6; ++round) {
    for (unsigned b = 0; b + 1u < gov::kMuteBlocks; ++b) {  // N - 1 heavy blocks...
      u.st[0].cost = 140000;
      u.block(0);
    }
    u.st[0].cost = 100000;  // ...then one under the budget
    u.block(0);
  }
  CHECK(!u.mv->muted(0));
  CHECK(u.mv->governor().snapshot().mutes == 0u);
}

TEST_CASE("governor: the budget is a knob: lowered, a 16 % instance is forced heavy and muted alone while a 5 % one is not") {
  Unit u;
  NEED(u.bind(0, kTb303));
  NEED(u.bind(1, 13000));
  u.mv->governor().set_budget_percent(10);
  CHECK(u.mv->governor().budget() == gov::cycles_of_percent(10));
  u.block(0, [&] {
    CHECK(u.mv->note_on(0, 60, 0, 100));
    CHECK(u.mv->note_on(1, 60, 1, 100));
  });
  for (unsigned b = 0; b < 8; ++b) {
    u.block(0);
  }
  CHECK(u.mv->muted(0));
  CHECK(!u.mv->muted(1));
  CHECK(u.info(1).held);
  u.mv->governor().set_budget_percent(3);  // clamped to 10
  CHECK(u.mv->governor().budget() == gov::cycles_of_percent(10));
  u.mv->governor().set_budget_percent(500);  // clamped to 100
  CHECK(u.mv->governor().budget() == gov::cycles_of_percent(100));
  u.mv->governor().set_threshold_percent(500);  // a threshold is held to 50..95 %: nothing starves below, the interrupts keep their room above
  CHECK(u.mv->governor().threshold() == gov::cycles_of_percent(95));
  u.mv->governor().set_threshold_percent(5);
  CHECK(u.mv->governor().threshold() == gov::cycles_of_percent(50));
  u.mv->governor().set_threshold_percent(gov::kThresholdPercent);
  CHECK(u.mv->governor().threshold() == gov::kThresholdCycles);
}

TEST_CASE("governor: the mute fades the block it was heard in out, and the next block carries nothing (no hard cut)") {
  Unit u;
  NEED(u.bind(0, 140000));
  u.block(0, [&] { CHECK(u.mv->note_on(0, 60, 0, 100)); });
  for (unsigned b = 0; b + 1u < gov::kMuteBlocks; ++b) {
    CHECK(!u.mv->muted(0));
    const std::int16_t* f = u.mv->feed(0);
    NEED(f != nullptr);
    CHECK(f[0] == 3276);
    CHECK(f[kBlk - 1] == 3276);  // an ordinary block: the module's 0.1 at full length
    u.block(0);
  }
  CHECK(u.mv->muted(0));  // the block that muted it
  const std::int16_t* last = u.mv->feed(0);
  NEED(last != nullptr);  // still carried: it is faded, not dropped
  CHECK(last[0] == 3276);  // the ramp starts at the full level...
  CHECK(last[kBlk - 1] < 100);  // ...and ends at the floor of the 64-step ramp (3276 / 64 = 51)
  for (std::uint32_t k = 1; k < kBlk; ++k) {
    CHECK(last[k] <= last[k - 1]);  // monotone
  }
  u.block(0);
  CHECK(u.mv->feed(0) == nullptr);  // from the next block on, nothing
}

// ---- a module that goes idle while its gate is held (the TB-303 after its amplitude has died: the sequencer sends no note-off) ---------------------------------------------------------------

namespace {

struct alignas(16) IdleState {
  std::uint32_t gate;
  std::uint32_t cost;
  std::uint32_t life;      // frames it sounds for once a note starts, then it reports itself idle (render() returns 0) with the gate still held
  std::uint32_t left;
  std::uint32_t notes;
  std::uint32_t offs;
};
int idle_init(void*, const o3m_host*) { return 0; }
std::uint32_t idle_render(void* raw, const float*, float* out, std::uint32_t n) {
  IdleState* c = static_cast<IdleState*>(raw);
  const std::uint32_t used = c->left < n ? c->left : n;
  for (std::uint32_t i = 0; i < n; ++i) {
    out[i] = i < used ? 0.1f : 0.0f;
  }
  g_clock += c->cost * used / kBlk;
  c->left -= used;
  return c->left != 0u ? 1u : 0u;  // the gate is not looked at: a decayed voice is silent and idle whether or not the note was released
}
void idle_set_param(void*, std::uint32_t, float) {}
void idle_note_on(void* raw, std::uint32_t, std::uint32_t, std::uint32_t) {
  IdleState* c = static_cast<IdleState*>(raw);
  c->gate = 1;
  c->left = c->life;
  ++c->notes;
}
void idle_note_off(void* raw, std::uint32_t) {
  IdleState* c = static_cast<IdleState*>(raw);
  c->gate = 0;
  ++c->offs;
}
void idle_shutdown(void*) {}
const o3m_module kIdling = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_INSTRUMENT, sizeof(o3m_module), "idling", sizeof(IdleState), 0, nullptr,
                            idle_init, idle_render, idle_set_param, idle_note_on, idle_note_off, idle_shutdown, nullptr, 0};

// `count` instances of the idling module bound to slots 0..count-1 (cost 42,700, a life of 3 blocks), each started in its own block and measured (two blocks), then left to go idle with the gate held.
struct IdleHeld {
  Unit u;
  alignas(16) IdleState st[8];
  explicit IdleHeld(unsigned count) {
    std::memset(st, 0, sizeof st);
    for (unsigned s = 0; s < count; ++s) {
      st[s].cost = kTb303;
      st[s].life = 3u * kBlk;
      CHECK(u.mv->bind(s, kIdling, static_cast<std::uint16_t>(0x8300 + s), &st[s], nullptr, 0) == ModuleVoices::Bind::Ok);
    }
    for (unsigned s = 0; s < count; ++s) {
      u.block(0, [&] { CHECK(u.mv->note_on(s, 60, s, 100)); });
      for (unsigned b = 0; b < 3; ++b) {
        u.block(0);
      }
    }
  }
};

}  // namespace

TEST_CASE("governor: an instance that went idle with its gate still held has no voice: a note for it adds one, and it is no load for the others") {
  // (1) the bypass: three held-idle instances, a 60 % mix (156,672), a chord of three notes at once: ONE voice fits (156,672 + 42,700 = 199,372; the second 242,072 > 235,008). With the gate counted as a
  // voice the three were "retriggers", all admitted, and the block reached 109 %.
  {
    IdleHeld h(3);
    for (unsigned s = 0; s < 3; ++s) {
      CHECK(h.st[s].gate == 1u);        // the gate is held...
      CHECK(h.u.info(s).held);
      CHECK(!h.u.info(s).active);       // ...and nothing renders
      CHECK(h.u.mv->voice_cost(s) == kTb303);  // measured
    }
    const std::uint32_t mix = gov::cycles_of_percent(60);
    for (unsigned b = 0; b < 30; ++b) {
      h.u.block(mix);  // the mix plays, the idle instances cost nothing
    }
    const gov::Snapshot before = h.u.mv->governor().snapshot();
    CHECK(before.base >= mix);
    bool started[3] = {};
    h.u.block(mix, [&] {
      for (unsigned s = 0; s < 3; ++s) {
        started[s] = h.u.mv->note_on(s, 62, s, 100);
      }
    });
    const gov::Snapshot g = h.u.mv->governor().snapshot();
    std::printf("[governor] chord of three held-idle instances over a 60 %% mix: admitted %u, dropped %u, worst %.1f %%\n", static_cast<unsigned>(g.admitted - before.admitted),
                static_cast<unsigned>(g.drops - before.drops), 100.0 * g.worst / 261120.0);
    CHECK(started[0]);
    CHECK(!started[1]);
    CHECK(!started[2]);
    CHECK(g.admitted - before.admitted == 1u);  // one voice was added, and it was counted
    CHECK(g.drops - before.drops == 2u);
    CHECK(g.worst <= gov::kThresholdCycles);
    CHECK(h.u.info(0).active);
    CHECK(!h.u.info(1).active);
  }
  // (2) the phantom load: five held-idle instances on an EMPTY mix do not make a sixth instrument's note refused (it was: 5 x 42,700 counted as sounding)
  {
    IdleHeld h(5);
    NEED(h.u.bind(5, kTb303));
    h.u.block(0, [&] { CHECK(h.u.mv->note_on(5, 60, 5, 100)); });
    h.u.block(0);
    h.u.block(0);
    h.u.block(0, [&] { h.u.mv->note_off(5); });
    for (unsigned b = 0; b < 4; ++b) {
      h.u.block(0);
    }
    NEED(h.u.mv->voice_cost(5) == kTb303);  // the sixth is measured, and idle again
    for (unsigned s = 0; s < 5; ++s) {
      CHECK(h.u.info(s).held);
      CHECK(!h.u.info(s).active);
    }
    const gov::Snapshot before = h.u.mv->governor().snapshot();
    bool sixth = false;
    h.u.block(0, [&] { sixth = h.u.mv->note_on(5, 64, 5, 100); });
    CHECK(sixth);
    CHECK(h.u.mv->governor().snapshot().drops == before.drops);
  }
}

// ---- the reviewer's repros (RV1..RV5): what a unit-like mix does to a 303 line, before the fix and after ------------------------------------------

TEST_CASE("governor RV1: a sounding 303 retriggered every step over the unit's 88.3 % stress goes on: every retrigger starts, none is dropped or counted") {
  Unit u;
  NEED(u.bind(0, kTb303));
  const std::uint32_t base = static_cast<std::uint32_t>(0.883 * 261120.0) - kTb303;  // everything but the 303
  unsigned started = 0;
  unsigned dropped = 0;
  u.block(0, [&] { CHECK(u.mv->note_on(0, 60, 0, 100)); });
  ++started;
  for (unsigned b = 1; b < 400; ++b) {
    if (b % 24 == 0) {  // a sequencer step: a new note, no note-off of its own
      bool ok = false;
      u.block(base, [&] { ok = u.mv->note_on(0, 60 + (b / 24) % 12, 0, 100); });
      (ok ? started : dropped)++;
    } else {
      u.block(base);
    }
  }
  const gov::Snapshot g = u.mv->governor().snapshot();
  std::printf("[RV1] started %u dropped %u | admitted %u drops %u | worst %u (%.1f%%)\n", started, dropped, static_cast<unsigned>(g.admitted), static_cast<unsigned>(g.drops), static_cast<unsigned>(g.worst), 100.0 * g.worst / 261120.0);
  CHECK(started == 17u);  // the first note and the sixteen steps
  CHECK(dropped == 0u);
  CHECK(g.admitted == 1u);  // one new voice; the retriggers add none
  CHECK(g.drops == 0u);
  CHECK(g.last == gov::Event::None);  // nothing worth a toast
  CHECK(u.info(0).held);
  CHECK(u.st[0].notes == 17u);
}

TEST_CASE("governor RV1b: over a 76 % mix (without the 303) a NEW 303 voice is not admitted at the 90 % line: the load stays under it, measured or cold; at the unit's 72 % it is") {
  const std::uint32_t base = gov::cycles_of_percent(76);
  for (const bool warm : {false, true}) {
    CAPTURE(warm);
    Unit u;
    NEED(u.bind(0, kTb303));
    if (warm) {  // measured first, on a quiet mix, then idle
      u.block(0, [&] { CHECK(u.mv->note_on(0, 60, 0, 100)); });
      for (unsigned b = 0; b < 3; ++b) {
        u.block(0);
      }
      u.block(0, [&] { u.mv->note_off(0); });
      u.block(0);
      NEED(u.mv->voice_cost(0) == kTb303);
    }
    for (unsigned b = 0; b < 30; ++b) {
      u.block(base);  // the mix is playing (the base is learned from the blocks that went by)
    }
    unsigned started = 0;
    for (unsigned b = 0; b < 600; ++b) {
      if (b % 24 == 0) {
        bool ok = false;
        u.block(base, [&] { ok = u.mv->note_on(0, 60, 0, 100); });
        started += ok ? 1u : 0u;
      } else {
        u.block(base);
      }
    }
    const gov::Snapshot g = u.mv->governor().snapshot();
    CHECK(started == 0u);
    CHECK(g.drops == 25u);
    CHECK(!u.info(0).held);
    CHECK(g.base >= base);
    CHECK(g.base + 5000u < gov::kThresholdCycles);  // the block is the mix alone: 76 %
  }
  // the unit's own stress mix (88.3 % with the 303, so 72 % without): at the 90 % line a MEASURED new 303 voice fits (72 + 16.4 = 88.4) and is admitted (a never measured one is assumed 20 %: 92 %, refused)
  Unit u;
  NEED(u.bind(0, kTb303));
  u.block(0, [&] { CHECK(u.mv->note_on(0, 60, 0, 100)); });
  for (unsigned b = 0; b < 3; ++b) {
    u.block(0);
  }
  u.block(0, [&] { u.mv->note_off(0); });
  u.block(0);
  NEED(u.mv->voice_cost(0) == kTb303);
  const std::uint32_t mix72 = static_cast<std::uint32_t>(0.883 * 261120.0) - kTb303;
  for (unsigned b = 0; b < 30; ++b) {
    u.block(mix72);
  }
  bool ok = false;
  u.block(mix72, [&] { ok = u.mv->note_on(0, 60, 0, 100); });
  CHECK(ok);
  for (unsigned b = 0; b < 40; ++b) {
    u.block(mix72);
  }
  CHECK(u.mv->governor().snapshot().worst < gov::kThresholdCycles);
  CHECK(u.mv->governor().snapshot().drops == 0u);
  CHECK(u.mv->governor().snapshot().worst > gov::cycles_of_percent(85));  // (the unit's 88.3 %: over the old 85 % line, under the new one)
}

TEST_CASE("governor RV2: at 70..82 % no retrigger of a sounding 303 is a governor event (it was a 'voice stolen' toast every 3 s)") {
  for (unsigned extra_pct : {0u, 4u, 8u, 12u}) {
    CAPTURE(extra_pct);
    Unit u;
    NEED(u.bind(0, kTb303));
    const std::uint32_t base = 141000 + extra_pct * 2611;
    unsigned dropped = 0;
    u.block(0, [&] { CHECK(u.mv->note_on(0, 60, 0, 100)); });
    for (unsigned b = 1; b < 600; ++b) {
      if (b % 24 == 0) {
        bool ok = false;
        u.block(base, [&] { ok = u.mv->note_on(0, 60 + (b / 24) % 12, 0, 100); });
        dropped += ok ? 0u : 1u;
      } else {
        u.block(base);
      }
    }
    const gov::Snapshot g = u.mv->governor().snapshot();
    std::printf("[RV2] extra %u%%: dropped %u, admitted %u, worst %.1f%%\n", extra_pct, dropped, static_cast<unsigned>(g.admitted), 100.0 * g.worst / 261120.0);
    CHECK(dropped == 0u);
    CHECK(g.admitted == 1u);
    CHECK(g.drops == 0u);
    CHECK(g.last == gov::Event::None);
  }
}

TEST_CASE("governor RV3: a staccato 303 over a 54 % song is admitted at every gap: the gone voice's cost is not left in the load") {
  for (unsigned gap : {2u, 5u, 10u, 20u, 40u, 80u}) {
    CAPTURE(gap);
    Unit u;
    NEED(u.bind(0, kTb303));
    const std::uint32_t base = 141000;
    unsigned started = 0;
    unsigned dropped = 0;
    for (unsigned step = 0; step < 40; ++step) {
      bool ok = false;
      u.block(base, [&] { ok = u.mv->note_on(0, 60, 0, 100); });
      (ok ? started : dropped)++;
      for (unsigned b = 0; b < 10; ++b) {
        u.block(base);
      }
      u.block(base, [&] { u.mv->note_off(0); });
      for (unsigned b = 0; b < gap; ++b) {
        u.block(base);
      }
    }
    std::printf("[RV3] gap %u blocks: started %u dropped %u\n", gap, started, dropped);
    CHECK(started == 40u);
    CHECK(dropped == 0u);
  }
}

TEST_CASE("governor RV4: one long first block (a cold cache, an interrupt) does not make the instrument expensive for the rest of the song") {
  for (unsigned outlier_pct : {0u, 15u, 25u, 40u}) {  // the first block costs kTb303 + outlier % of the block
    CAPTURE(outlier_pct);
    Unit u;
    NEED(u.bind(0, kTb303));
    const std::uint32_t base = 141000;
    unsigned started = 0;
    unsigned dropped = 0;
    for (unsigned step = 0; step < 60; ++step) {
      if (step == 0) {
        u.st[0].cost = kTb303 + outlier_pct * 2611;
      }
      bool ok = false;
      u.block(base, [&] { ok = u.mv->note_on(0, 60, 0, 100); });
      u.st[0].cost = kTb303;
      (ok ? started : dropped)++;
      for (unsigned b = 0; b < 10; ++b) {
        u.block(base);
      }
      u.block(base, [&] { u.mv->note_off(0); });
      for (unsigned b = 0; b < 20; ++b) {
        u.block(base);
      }
    }
    std::printf("[RV4] first-block outlier +%u%%: started %u dropped %u, cost now %u (%u%%)\n", outlier_pct, started, dropped, static_cast<unsigned>(u.mv->voice_cost(0)),
                static_cast<unsigned>(100u * u.mv->voice_cost(0) / 261120u));
    CHECK(started == 60u);
    CHECK(dropped == 0u);
    CHECK(u.mv->voice_cost(0) == kTb303);  // the outlier never entered the estimate: min(this block, the previous one)
  }
}

TEST_CASE("governor RV4b: a figure that WAS high for two blocks in a row ages away with the idle blocks, and the instrument is admitted again") {
  Unit u;
  NEED(u.bind(0, kTb303));
  const std::uint32_t base = 141000;
  u.st[0].cost = kTb303 + 25u * 2611u;  // 25 % over, for the whole first note (a sustained fault, not one block)
  u.block(base, [&] { CHECK(u.mv->note_on(0, 60, 0, 100)); });
  for (unsigned b = 0; b < 10; ++b) {
    u.block(base);
  }
  u.block(base, [&] { u.mv->note_off(0); });
  const std::uint32_t high = u.mv->voice_cost(0);
  CHECK(high > kTb303 + 20u * 2611u);
  u.st[0].cost = kTb303;
  unsigned started = 0;
  unsigned dropped = 0;
  for (unsigned step = 0; step < 60; ++step) {
    bool ok = false;
    u.block(base, [&] { ok = u.mv->note_on(0, 60, 0, 100); });
    (ok ? started : dropped)++;
    for (unsigned b = 0; b < 10; ++b) {
      u.block(base);
    }
    u.block(base, [&] { u.mv->note_off(0); });
    for (unsigned b = 0; b < 20; ++b) {
      u.block(base);
    }
  }
  std::printf("[RV4b] high estimate %u (%u%%): started %u dropped %u, cost now %u\n", static_cast<unsigned>(high), static_cast<unsigned>(100u * high / 261120u), started, dropped, static_cast<unsigned>(u.mv->voice_cost(0)));
  CHECK(dropped <= 4u);  // the estimate (108k + 141k > the line) ages 11 % per note cycle: a few notes, not 59 of 60
  CHECK(started >= 56u);
  CHECK(u.mv->voice_cost(0) == kTb303);
}

TEST_CASE("governor RV5: after a mute from an interrupt storm and the cool-down the instrument is audible again in a 54 % song") {
  Unit u;
  NEED(u.bind(0, kTb303));
  const std::uint32_t base = 141000;
  u.block(base, [&] { CHECK(u.mv->note_on(0, 60, 0, 100)); });
  u.st[0].cost = 140000;  // 4 blocks of an interrupt storm
  for (unsigned b = 0; b < 4; ++b) {
    u.block(base);
  }
  CHECK(u.mv->muted(0));
  u.st[0].cost = kTb303;
  unsigned started = 0;
  unsigned dropped = 0;
  for (unsigned step = 0; step < 100; ++step) {
    bool ok = false;
    for (unsigned b = 0; b < 100; ++b) {
      u.block(base, [&] {
        if (b == 0) {
          ok = u.mv->note_on(0, 60, 0, 100);
        }
      });
    }
    (ok ? started : dropped)++;
  }
  std::printf("[RV5] after the storm: started %u dropped %u (muted=%d cost=%u)\n", started, dropped, u.mv->muted(0), static_cast<unsigned>(u.mv->voice_cost(0)));
  CHECK(dropped <= 11u);  // the cool-down is 1,024 blocks: the notes of the first ten steps (100 blocks each) wait it out, the rest play
  CHECK(started >= 89u);
  CHECK(!u.mv->muted(0));
  CHECK(u.info(0).held);
  CHECK(u.mv->governor().snapshot().mutes == 1u);
}

// ---- twelve notes at once through the whole engine -----------------------------------------------------------------------------------------

namespace {

struct Cue {
  std::uint32_t block;
  SeqEvent ev;
};
constexpr std::uint32_t kMaxCues = 32;
Cue g_cues[kMaxCues];
std::uint32_t g_cue_count = 0;
SeqEvent note_event(std::uint16_t offset, EventKind kind, std::uint8_t track, std::uint8_t note = 0, std::uint8_t slot = 0, std::uint8_t vel = 0) {
  SeqEvent e{};
  e.offset = offset;
  e.kind = kind;
  e.track = track;
  e.a = note;
  e.b = slot;
  e.c = vel;
  return e;
}
void cue(std::uint32_t block, const SeqEvent& e) { g_cues[g_cue_count++] = Cue{block, e}; }
std::uint32_t g_chord_first = 0xFFFFFFFFu;  // from this block on, EVERY block starts the same twelve notes (instruments 1..8, then 1..4 again, on tracks 1..12)
std::uint32_t scripted_step(void*, std::uint64_t block_start, std::uint32_t n, SeqEvent* out, std::uint32_t cap) noexcept {
  const std::uint32_t block = static_cast<std::uint32_t>(block_start / n);
  std::uint32_t count = 0;
  for (std::uint32_t k = 0; block >= g_chord_first && k < 12 && count < cap; ++k) {
    out[count++] = note_event(0, EventKind::NoteOn, static_cast<std::uint8_t>(k), static_cast<std::uint8_t>(60 + k), static_cast<std::uint8_t>(k % 8), 100);
  }
  for (std::uint32_t i = 0; i < g_cue_count && count < cap; ++i) {
    if (g_cues[i].block == block) {
      out[count++] = g_cues[i].ev;
    }
  }
  return count;
}

struct Rig {
  Engine* engine = nullptr;
  VoicePool* pool = nullptr;
  ModuleVoices* mv = nullptr;
  haudio::BlockReport report{};
  alignas(8) std::int16_t in[haudio::kBlockSamples] = {};
  alignas(8) std::int16_t out[haudio::kBlockSamples] = {};
  std::uint32_t block_no = 0;

  Rig() {
    static o3os::test::StaticSlot<Engine> engine_slot;
    static o3os::test::StaticSlot<VoicePool> pool_slot;
    static o3os::test::StaticSlot<ModuleVoices> mv_slot;
    alignas(16) static std::uint8_t scratch[ModuleVoices::kScratchBytes];
    static audio::TapMem taps;
    static SeqEvent events[audio::kMaxEventsPerBlock];
    g_clock = 1000;
    g_step = 7;  // the engine's stamps (the graph's stages and the governor's) each cost a few cycles, as a counter read does
    engine = &engine_slot.make(&clock_read, taps);
    pool = &pool_slot.make();
    mv = &mv_slot.make();
    std::memset(scratch, 0, sizeof scratch);
    (void)mv->attach_scratch(scratch, sizeof scratch);
    engine->attach_voices(pool);
    engine->attach_modules(mv);  // hands the engine's counter to the module voices: the governor measures with it
    audio::EventSource src;
    src.step = &scripted_step;
    src.buffer = events;
    src.capacity = audio::kMaxEventsPerBlock;
    engine->attach_events(src);
    g_cue_count = 0;
    g_chord_first = 0xFFFFFFFFu;
  }
  void block(unsigned n = 1) {
    for (unsigned i = 0; i < n; ++i) {
      report = haudio::BlockReport{};
      const haudio::BlockIo io{this->in, this->out, 0, &report};
      engine->render_block(io);
      ++block_no;
    }
  }
  unsigned sounding() const {
    unsigned n = 0;
    for (unsigned i = 0; i < audio::kMaxModuleInstances; ++i) {
      ModuleVoices::Info inf{};
      n += mv->info(i, inf) && inf.active ? 1u : 0u;
    }
    return n;
  }
};

// The scenario shared by the native cost module and the real TB-303: eight instruments bound (slots 0..7), warmed one by one (a solo note each, so that every instance has a measured cost),
// the load allowed to fall back to idle, then TWELVE notes in one block: instruments 1..8 once and 1..4 again. Only as many voices as fit under 90 % of the block start; the notes of the
// instruments that already have a voice take it over; the rest are dropped.
void twelve_at_once(Rig& r, unsigned per_voice_cycles) {
  const gov::Snapshot start = r.mv->governor().snapshot();
  // warm-up: instrument s alone for 3 blocks, then released
  for (unsigned s = 0; s < 8; ++s) {
    cue(r.block_no, note_event(0, EventKind::NoteOn, static_cast<std::uint8_t>(s), 48, static_cast<std::uint8_t>(s), 100));
    cue(r.block_no + 3u, note_event(0, EventKind::NoteOff, static_cast<std::uint8_t>(s)));
    r.block(4);
    CHECK(r.mv->voice_cost(s) >= per_voice_cycles);  // measured (plus the few cycles of the counter reads), never the declared figure
    CHECK(r.mv->voice_cost(s) < per_voice_cycles + 100u);
    unsigned tail = 0;
    for (; r.sounding() != 0u && tail < 8000u; ++tail) {
      r.block(1);  // a real module's release runs on: wait it out, so that the next instance is warmed alone (a native one is idle at once)
    }
    CHECK(tail < 8000u);
    std::printf("[governor] instrument %u: cost %u cycles, release tail %u blocks\n", s + 1u, static_cast<unsigned>(r.mv->voice_cost(s)), tail);
  }
  r.block(1500);  // the recent worst render falls back (1/256 a block) and the releases end
  NEED(r.sounding() == 0u);
  const gov::Snapshot idle = r.mv->governor().snapshot();
  CHECK(idle.worst < 3000u);
  CHECK(idle.drops == start.drops);

  // twelve notes at once, one block
  const unsigned fit = gov::kThresholdCycles / (per_voice_cycles + 20u);  // 5 for 42.7k: 5 x 42.7k = 213.5k <= 221.9k < 6 x 42.7k
  CHECK(fit == 5u);
  g_chord_first = r.block_no;
  r.block(1);
  const gov::Snapshot g = r.mv->governor().snapshot();
  CHECK(g.admitted - idle.admitted == fit);              // notes 1..5: new voices, up to the budget
  CHECK(g.drops - idle.drops == 3u);                     // notes 6..8: instruments with no voice and no room
                                                         // notes 9..12: the instruments 1..4 already sound: retriggers, admitted without a word (and not counted)
  CHECK(r.sounding() == fit);                            // five voices, not twelve
  CHECK(g.worst <= gov::kThresholdCycles);               // the block the engine measured is under the line
  CHECK(g.worst >= fit * per_voice_cycles);
  // The chord keeps coming (the same twelve notes every block for the next 60 ms): the load never passes the line, and nothing mutes
  for (unsigned b = 0; b < 40; ++b) {
    r.block(1);
    CHECK(r.mv->governor().snapshot().worst <= gov::kThresholdCycles);
    CHECK(r.sounding() <= fit);
  }
  g_chord_first = 0xFFFFFFFFu;
  CHECK(r.mv->governor().snapshot().mutes == start.mutes);
  CHECK(r.mv->governor().snapshot().drops - idle.drops >= 3u * 41u);
}

}  // namespace

TEST_CASE("governor: twelve module notes at once through the engine are admitted only up to the budget (native modules costing a TB-303)") {
  Rig r;
  static CostState states[8];
  std::memset(states, 0, sizeof states);
  for (unsigned s = 0; s < 8; ++s) {
    states[s].cost = kTb303;
    NEED(r.mv->bind(s, kCost, static_cast<std::uint16_t>(0x8200 + s), &states[s], nullptr, 0) == ModuleVoices::Bind::Ok);
  }
  twelve_at_once(r, kTb303);
  r.mv->retire_all();
  r.mv->release_all();
}

// OS-WAVEXP: an export renders on the main loop's wall clock; the governor must not judge it. Twelve notes a block on eight instruments that each cost a TB-303 would be five voices and three
// drops under the governor (the test above): offline all eight sound, nothing is dropped or muted, two passes are the same blocks, and the governor is back as it was after the export.
TEST_CASE("governor: an offline render is ungoverned - no note dropped, no instance muted, two passes the same - and the governor is restored") {
  Rig r;
  static CostState states[8];
  std::memset(states, 0, sizeof states);
  for (unsigned s = 0; s < 8; ++s) {
    states[s].cost = kTb303;
    NEED(r.mv->bind(s, kCost, static_cast<std::uint16_t>(0x8300 + s), &states[s], nullptr, 0) == ModuleVoices::Bind::Ok);
  }
  NEED(r.mv->governed());
  const gov::Snapshot start = r.mv->governor().snapshot();
  static SeqEvent export_events[audio::kMaxEventsPerBlock];
  audio::EventSource src;
  src.step = &scripted_step;
  src.buffer = export_events;
  src.capacity = audio::kMaxEventsPerBlock;
  constexpr unsigned kPasses = 6;  // (a set of stems is a pass for every file: the governor is taken off and put back each time, and nothing builds up across them)
  std::int32_t first[kPasses] = {};
  unsigned sounding[kPasses] = {};
  std::uint32_t notes[kPasses] = {};
  for (unsigned pass = 0; pass < kPasses; ++pass) {
    g_chord_first = 0;  // from the first block: twelve notes, every block
    r.engine->offline_request();
    r.engine->offline_enter(src);
    CHECK_FALSE(r.mv->governed());
    const std::uint32_t before = r.mv->stats().note_ons;
    std::int32_t sum = 0;
    for (unsigned b = 0; b < 40; ++b) {
      r.report = haudio::BlockReport{};
      const haudio::BlockIo io{r.in, r.out, 0, &r.report};
      r.engine->render_offline(io);
      for (std::uint32_t i = 0; i < haudio::kBlockSamples; ++i) {
        sum += r.out[i];
      }
    }
    first[pass] = sum;
    sounding[pass] = r.sounding();
    notes[pass] = r.mv->stats().note_ons - before;
    r.engine->offline_leave();
    CHECK(r.mv->governed());  // restored
  }
  g_chord_first = 0xFFFFFFFFu;
  // The first live block after the export is measured by itself, not as the sum of the export's blocks (the accumulators were cleared when the governor came back).
  cue(0, note_event(0, EventKind::NoteOn, 0, 60, 0, 100));
  r.block(1);
  // The governor's first sample is that block's alone: what the render took besides the module voices is measured (it is zero if the export's sum had been taken off the block's total), and
  // two more blocks give the instance's cost as a TB-303's.
  CHECK(r.mv->governor().snapshot().base > 0u);
  r.block(3);
  CHECK(r.mv->voice_cost(0) >= kTb303);
  CHECK(r.mv->voice_cost(0) < kTb303 + 100u);
  for (unsigned pass = 0; pass < kPasses; ++pass) {
    CHECK(sounding[pass] == 8u);  // all eight instruments sound: nothing was refused for lack of room
    CHECK(notes[pass] == notes[0]);
    CHECK(first[pass] == first[0]);  // pass to pass: the same blocks
  }
  CHECK(notes[0] >= 8u * 40u);  // (every block starts the twelve notes again: none was dropped)
  const gov::Snapshot end = r.mv->governor().snapshot();
  CHECK(end.drops == start.drops);
  CHECK(end.mutes == start.mutes);
  r.mv->retire_all();
  r.mv->release_all();
}

// The real TB-303 code, where it can run (the Cortex-M4 target suite; the host cannot call Thumb code and only checks the load): the same scenario, its cost simulated by the render() wrapper
// (the QEMU cycle counter says nothing about the unit).
namespace {

const o3m_module* g_real_303 = nullptr;
o3m_module g_wrapped_303;
std::uint32_t wrapped_render(void* state, const float* in, float* out, std::uint32_t n) {
  const std::uint32_t more = g_real_303->render(state, in, out, n);
  g_clock += kTb303 * n / kBlk;
  return more;
}
constexpr std::uint32_t kSlabBytes = 14u * 1024u - ModuleVoices::kScratchBytes;
alignas(64) std::uint8_t g_slab[kSlabBytes];
alignas(64) std::uint8_t g_cold[64 * 1024];
alignas(64) std::uint8_t g_state[4096];

}  // namespace

TEST_CASE("governor: twelve notes at once on eight real TB-303 instances (target): admitted only up to the budget") {
  Rig r;
  o3os::test::NullStorage card;
  module::ModuleSet set;
  module::ModuleLoader loader;
  set.set_builtins(module::kBuiltinModules, module::kBuiltinModuleCount);
  set.hot().init(g_slab, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(g_slab)), sizeof g_slab);
  set.cold().init(g_cold, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(g_cold)), sizeof g_cold);
  set.state().init(g_state, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(g_state)), sizeof g_state);
  NEED(loader.begin(card, set, "tb303", nullptr));
  for (unsigned n = 0; n < 100000 && loader.state() == module::ModuleLoader::State::Running; ++n) {
    (void)loader.step(2048);
  }
  NEED(loader.state() == module::ModuleLoader::State::Done);
  const module::LoadedModule& lm = *set.at(static_cast<unsigned>(set.find("tb303")));
  if (lm.entry == nullptr) {
    return;  // the host cannot call Thumb code: the target suite runs the module
  }
  g_real_303 = lm.entry;
  g_wrapped_303 = *lm.entry;
  g_wrapped_303.render = &wrapped_render;
  for (unsigned s = 0; s < 8; ++s) {
    std::uint8_t* state = nullptr;
    std::uint32_t addr = 0;
    NEED(set.take_state(lm.entry->state_bytes, state, addr));
    std::memset(state, 0, lm.entry->state_bytes);
    NEED(r.mv->bind(s, g_wrapped_303, module::fault_id("tb303"), state, nullptr, 0) == ModuleVoices::Bind::Ok);
  }
  twelve_at_once(r, kTb303);
  r.mv->retire_all();
  r.mv->release_all();
}

// ---- OS-303PERF: the perf cells (audio/module_perf.h): the render's own split of an instance's block, min / mean / max over full blocks --------------------------------------------------

namespace {

constexpr std::uint32_t kHook = 40;  // what the crash-context stamp (enter / leave) costs in these tests
std::uint16_t perf_enter(std::uint16_t id) {
  g_clock += kHook;
  return id;
}
// With g_step = s (a counter read costs s) and a module that takes R cycles in render(), a segment of a first-touched block reads: zero-fill s; t0 -> t1 = hook + s; t1 -> t2 = R + s; t2 -> t3 = hook + s;
// t3 -> t4 = s. So the segment is  loop 2 hook + 5 s + R (the governor's 2 hook + 4 s + R, and the zero-fill s), render R + s, out 2 s (the zero-fill and the conversion), hooks 2 hook + 2 s.
constexpr std::uint32_t kStep = 3;

struct PerfRig : Unit {
  struct ModulePerfRead {
    bool found = false;
    audio::ModulePerf p;
  };
  explicit PerfRig(bool with_clock = true, bool on = true) : Unit(with_clock) {
    g_step = kStep;
    if (on) {
      mv->reset_perf();  // (the split is off from boot: `diag cpu reset` is what turns it on)
    }
    ModuleVoices::Hooks h;
    h.enter = &perf_enter;
    mv->set_hooks(h);
  }
  void play(unsigned track, unsigned slot) {
    mv->begin_block();
    CHECK(mv->note_on(track, 60, slot, 100));
    mv->render(0, kBlk);
    mv->end_block();
  }
  ModulePerfRead read(unsigned slot) const {
    ModulePerfRead r;
    for (unsigned i = 0; i < audio::kMaxModuleInstances; ++i) {
      ModuleVoices::Info in{};
      if (mv->info(i, in) && in.slot == slot) {
        r.found = mv->perf(i, r.p);
      }
    }
    return r;
  }
};

}  // namespace

TEST_CASE("module perf: a full block splits into render alone, the output side and the hooks; min, mean and max over the blocks") {
  PerfRig u;
  NEED(u.bind(0, 1000));
  const std::uint32_t costs[] = {1000, 1200, 800};
  u.st[0].cost = costs[0];
  u.play(0, 0);  // the note-on and the first block
  for (unsigned b = 1; b < 3; ++b) {
    u.st[0].cost = costs[b];
    u.block(0);
  }
  const PerfRig::ModulePerfRead r = u.read(0);
  NEED(r.found);
  const audio::ModulePerf& p = r.p;
  CHECK(p.blocks == 3u);
  const auto loop = [](std::uint32_t cost) { return 2u * kHook + 5u * kStep + cost; };
  CHECK(p.loop_min == loop(800));
  CHECK(p.loop_max == loop(1200));
  CHECK(p.loop_sum == loop(1000) + loop(1200) + loop(800));
  CHECK(p.loop_mean() == loop(1000));
  CHECK(p.render_min == 800u + kStep);
  CHECK(p.render_max == 1200u + kStep);
  CHECK(p.render_mean() == 1000u + kStep);
  CHECK(p.out_min == 2u * kStep);
  CHECK(p.out_max == 2u * kStep);
  CHECK(p.out_mean() == 2u * kStep);
  CHECK(p.hooks_mean() == 2u * kHook + 2u * kStep);  // what is left of the loop once the module and the output side are taken off
  ModuleVoices::Info info{};
  NEED(u.mv->info(0, info));
  CHECK(info.state == &u.st[0]);  // `diag mod` says where it lives from this
}

TEST_CASE("module perf: only a block of kBlock frames of the instance is a sample; a block in two segments is one, one cut short is none") {
  PerfRig u;
  NEED(u.bind(0, 640));  // 10 cycles a frame
  u.mv->begin_block();
  CHECK(u.mv->note_on(0, 60, 0, 100));
  u.mv->render(0, 40);  // a block that ends at frame 40 (a mid-block event, then the transport stopped, say): too little to say anything
  u.mv->end_block();
  CHECK(u.read(0).p.blocks == 0u);
  u.mv->begin_block();
  u.mv->render(0, 20);  // two segments of one block: the loop is both of them, the zero-fill once
  u.mv->render(20, kBlk);
  u.mv->end_block();
  const audio::ModulePerf p = u.read(0).p;
  CHECK(p.blocks == 1u);
  CHECK(p.loop_min == 4u * kHook + 9u * kStep + 640u);  // two segments of 2 hook + 4 s + their share of the 640, and the zero-fill
  CHECK(p.render_min == 640u + 2u * kStep);
  CHECK(p.out_min == 3u * kStep);  // the zero-fill and two conversions
  CHECK(p.loop_min == p.loop_max);
}

TEST_CASE("module perf: a reset starts the figures again at once for the reader, and the render serves it with the next block that sounded") {
  PerfRig u;
  NEED(u.bind(0, 1000));
  u.play(0, 0);
  u.block(0);
  CHECK(u.read(0).p.blocks == 2u);
  u.mv->reset_perf();
  CHECK(u.read(0).found);
  CHECK(u.read(0).p.blocks == 0u);  // (the render has not run since: the reader does not show what the reset threw away)
  u.st[0].cost = 500;
  u.block(0);
  const audio::ModulePerf p = u.read(0).p;
  CHECK(p.blocks == 1u);  // this block is the first of the new run
  CHECK(p.render_min == 500u + kStep);
  CHECK(p.render_max == p.render_min);
}

TEST_CASE("module perf: off from boot: no extra stamps are read and nothing is kept until a reset turns it on; the block it turns on in is measured whole") {
  PerfRig u(true, false);
  NEED(u.bind(0, 1000));
  CHECK(!u.mv->perf_enabled());
  u.play(0, 0);
  const std::uint32_t before = g_reads;
  u.block(0);
  CHECK(g_reads - before == 2u);  // the governor's two stamps of a segment (t0 and the end), as before OS-303PERF
  CHECK(u.read(0).p.blocks == 0u);
  u.mv->reset_perf();
  CHECK(u.mv->perf_enabled());
  u.mv->begin_block();  // the block starts with the split on: its first segment is stamped too
  u.mv->render(0, 20);
  u.mv->render(20, kBlk);
  u.mv->end_block();
  CHECK(u.read(0).p.blocks == 1u);
  const std::uint32_t reads = g_reads;
  u.block(0);
  CHECK(g_reads - reads == 6u);  // tz, t0, t1, t2, t3, t4
}

TEST_CASE("module perf: `cpu perf off` stops the extra stamps at once and keeps the figures; a reset turns it on again") {
  PerfRig u;
  NEED(u.bind(0, 1000));
  u.play(0, 0);
  u.block(0);
  CHECK(u.mv->perf_enabled());
  CHECK(u.read(0).p.blocks == 2u);
  u.mv->stop_perf();
  CHECK(!u.mv->perf_enabled());
  const std::uint32_t reads = g_reads;
  u.block(0);
  u.block(0);
  CHECK(g_reads - reads == 4u);  // two stamps a segment again (t0 and the end), nothing else
  CHECK(u.read(0).p.blocks == 2u);  // nothing was folded, nothing was cleared
  CHECK(u.read(0).p.loop_min == 2u * kHook + 5u * kStep + 1000u);
  u.mv->reset_perf();
  CHECK(u.mv->perf_enabled());
  u.block(0);
  CHECK(u.read(0).p.blocks == 1u);  // restarted
}

TEST_CASE("module perf: while it is on the governor's own figure carries the three extra counter reads of a segment, and with it off it does not") {
  std::uint32_t cost[2] = {0, 0};  // [on]: the per-voice cost the governor measured of the same module in the same blocks
  for (unsigned on = 0; on < 2; ++on) {
    PerfRig u(true, on != 0u);
    NEED(u.bind(0, 1000));
    u.play(0, 0);
    u.block(0);
    u.block(0);
    cost[on] = u.info(0).cost;
  }
  CHECK(cost[0] != 0u);
  CHECK(cost[1] - cost[0] == 3u * kStep);  // t1, t2 and t3 are inside the window from t0 to the last stamp (tz, before t0, is not)
}

TEST_CASE("module perf: nothing is measured without a cycle counter, and a rebind starts empty") {
  {
    PerfRig u(false);
    NEED(u.bind(0, 1000));
    u.play(0, 0);
    u.block(0);
    CHECK(u.read(0).p.blocks == 0u);
  }
  PerfRig u;
  NEED(u.bind(0, 1000));
  u.play(0, 0);
  u.block(0);
  CHECK(u.read(0).p.blocks == 2u);
  u.mv->retire_all();
  u.mv->release_all();
  CHECK(!u.read(0).found);  // nobody is bound
  NEED(u.bind(0, 1000));
  CHECK(u.read(0).p.blocks == 0u);  // the new instance starts with no figures
}

TEST_CASE("module perf: the rounding of a mean, a hooks figure that cannot go negative, and the text of the line") {
  audio::ModulePerf p;
  p.blocks = 4;
  p.loop_sum = 10;  // 2.5 rounds up
  CHECK(p.loop_mean() == 3u);
  p.render_sum = 8;
  p.out_sum = 8;
  CHECK(p.hooks_mean() == 0u);  // 3 - 2 - 2: the means are rounded one by one, never negative
  audio::ModulePerf zero;
  CHECK(zero.loop_mean() == 0u);
  p.blocks = 41400;
  p.loop_min = 55210;
  p.loop_max = 61300;
  p.loop_sum = 57032ull * 41400ull;
  p.render_min = 43100;
  p.render_max = 49000;
  p.render_sum = 44010ull * 41400ull;
  p.out_sum = 3150ull * 41400ull;
  char buf[200];
  const std::size_t n = audio::diag::format_module_perf(1, p, buf, sizeof buf);
  CHECK(std::strcmp(buf, "mod: perf 1: 41400 blocks, loop min 55210 mean 57032 max 61300, render min 43100 mean 44010 max 49000, out mean 3150, hooks 9872") == 0);
  CHECK(n == std::strlen(buf));
  CHECK(n < 160u);  // the console's line buffer
  CHECK(audio::diag::format_module_perf(12, audio::ModulePerf{}, buf, sizeof buf) == std::strlen("mod: perf 12: no full block measured"));
  CHECK(std::strcmp(buf, "mod: perf 12: no full block measured") == 0);
  char small[12];
  CHECK(audio::diag::format_module_perf(1, p, small, sizeof small) == 11u);  // cut, never over the end; the NUL is always there
  CHECK(small[11] == '\0');
}

// ---- OS-303PERF (M4): the output side of the render: the Q15 conversion (VCVT + SSAT on the target) against the loop it replaced, and the word-wise zero-fill ------------------------------------

namespace {

float g_vals[kBlk];  // what the "grid" module renders: the test's own samples
std::uint32_t grid_render(void* raw, const float*, float* out, std::uint32_t n) {
  (void)raw;
  for (std::uint32_t i = 0; i < n; ++i) {
    out[i] = g_vals[i];
  }
  return 1;
}
const o3m_module kGrid = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_INSTRUMENT, sizeof(o3m_module), "grid", sizeof(CostState), 0, nullptr,
                          cost_init, grid_render, cost_set_param, cost_note_on, cost_note_off, cost_shutdown, nullptr, 0};

// The loop before OS-303PERF, as it was (module_voices.cpp render()): the clamp by two compares, NaN to 0, the cast of a value in range. `sat` counts the compares that clamped.
std::int16_t convert_v0(float x, std::uint32_t& sat) {
  float y = x * 32768.0f;
  if (y > 32767.0f) {
    y = 32767.0f;
    ++sat;
  } else if (y < -32768.0f) {
    y = -32768.0f;
    ++sat;
  } else if (!(y == y)) {
    y = 0.0f;
  }
  return static_cast<std::int16_t>(static_cast<std::int32_t>(y));
}

// What a sample counts as in the stats now: its integer part left the int16 range (a NaN never does).
bool left_range(float x) {
  if (!(x == x)) {
    return false;
  }
  const double t = std::trunc(static_cast<double>(x) * 32768.0);  // (exact: a power of two)
  return t > 32767.0 || t < -32768.0;
}

}  // namespace

TEST_CASE("module voices M4: the Q15 conversion gives the integer the old loop gave for every float, NaN is silence, infinities saturate; the clip count is the integer part leaving int16") {
  Unit u(false);
  CHECK(u.mv->bind(0, kGrid, 1, &u.st[0], nullptr, 0) == ModuleVoices::Bind::Ok);
  constexpr std::size_t kMaxValues = 6400;  // (a static: the target suites have no heap to speak of)
  static float values[kMaxValues];
  std::size_t count = 0;
  const float edge[] = {0.0f, -0.0f, 1e-30f, -1e-30f, 1.4e-45f, -1.4e-45f, 1e-38f, 0.5f / 32768.0f, -0.5f / 32768.0f, 1.0f / 32768.0f, -1.0f / 32768.0f, 1.5f / 32768.0f, -1.5f / 32768.0f,
                        0.99999994f, -0.99999994f, 1.0f, -1.0f, 1.00000012f, -1.00000012f, 32767.0f / 32768.0f, 32767.5f / 32768.0f, 32768.0f / 32768.0f, -32768.0f / 32768.0f,
                        -32768.5f / 32768.0f, -32769.0f / 32768.0f, -32769.5f / 32768.0f, 2.0f, -2.0f, 1.0e9f, -1.0e9f, 3.4028235e38f, -3.4028235e38f, 65536.0f, -65536.0f, 65535.0f / 32768.0f,
                        INFINITY, -INFINITY, std::nanf(""), -std::nanf(""), std::nanf("0x1234"), __builtin_bit_cast(float, 0x7FA00000u) /* a signalling NaN */, __builtin_bit_cast(float, 0xFFFFFFFFu)};
  for (float e : edge) {
    values[count++] = e;
  }
  for (int k = -3; k <= 3; ++k) {  // the integers round the rails, and a step either side of each
    for (float rail : {32767.0f, -32768.0f, 32768.0f, -32769.0f, 0.0f, 1.0f, -1.0f}) {
      const float x = (rail + static_cast<float>(k) * 0.25f) / 32768.0f;
      values[count++] = x;
      values[count++] = std::nextafter(x, 1.0f);
      values[count++] = std::nextafter(x, -1.0f);
    }
  }
  std::uint32_t seed = 12345;
  for (unsigned i = 0; i < 6000; ++i) {
    seed = seed * 1664525u + 1013904223u;
    values[count++] = (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f) * 1.3f;  // -1.3 .. 1.3: through both rails
  }
  while (count % kBlk != 0) {
    values[count++] = 0.0f;
  }
  NEED(count <= kMaxValues);
  std::uint32_t total_expected_clips = 0;
  std::uint32_t wrong = 0;
  std::uint32_t first_wrong = 0xFFFFFFFFu;
  for (std::size_t base = 0; base < count; base += kBlk) {
    for (std::uint32_t i = 0; i < kBlk; ++i) {
      g_vals[i] = values[base + i];
    }
    const std::uint32_t before = u.mv->stats().saturated;
    u.mv->begin_block();
    if (base == 0) {
      CHECK(u.mv->note_on(0, 60, 0, 100));
    }
    u.mv->render(0, kBlk);
    u.mv->end_block();
    const std::int16_t* feed = u.mv->feed(0);
    NEED(feed != nullptr);
    std::uint32_t expected_clips = 0;
    for (std::uint32_t i = 0; i < kBlk; ++i) {
      std::uint32_t old_sat = 0;
      const std::int16_t want = convert_v0(g_vals[i], old_sat);
      if (feed[i] != want && first_wrong == 0xFFFFFFFFu) {
        first_wrong = static_cast<std::uint32_t>(base + i);
      }
      wrong += feed[i] != want ? 1u : 0u;
      expected_clips += left_range(g_vals[i]) ? 1u : 0u;
    }
    CHECK(u.mv->stats().saturated - before == expected_clips);
    total_expected_clips += expected_clips;
  }
  CHECK_MESSAGE(wrong == 0u, "first different sample: input " << first_wrong);
  CHECK(total_expected_clips > 1000u);  // the grid had plenty past the rails
  // the named cases, by value
  const struct {
    float in;
    std::int16_t out;
  } named[] = {{std::nanf(""), 0}, {-std::nanf(""), 0}, {INFINITY, 32767}, {-INFINITY, -32768}, {1.0f, 32767}, {-1.0f, -32768}, {1.0e9f, 32767}, {-1.0e9f, -32768},
               {1.5f / 32768.0f, 1}, {-1.5f / 32768.0f, -1}, {0.99999994f, 32767}, {-0.5f / 32768.0f, 0}, {0.25f, 8192}, {-0.25f, -8192}};
  for (std::uint32_t i = 0; i < kBlk; ++i) {
    g_vals[i] = i < sizeof named / sizeof named[0] ? named[i].in : 0.0f;
  }
  u.mv->begin_block();
  u.mv->render(0, kBlk);
  u.mv->end_block();
  const std::int16_t* feed = u.mv->feed(0);
  NEED(feed != nullptr);
  for (std::uint32_t i = 0; i < sizeof named / sizeof named[0]; ++i) {
    CAPTURE(i);
    CHECK(feed[i] == named[i].out);
  }
}

TEST_CASE("module voices M4: the block is zeroed a word at a time, and a segment that starts at an odd frame writes only its own frames") {
  Unit u(false);
  CHECK(u.mv->bind(0, kGrid, 1, &u.st[0], nullptr, 0) == ModuleVoices::Bind::Ok);
  for (std::uint32_t i = 0; i < kBlk; ++i) {
    g_vals[i] = 0.5f;
  }
  u.mv->begin_block();
  CHECK(u.mv->note_on(0, 60, 0, 100));
  u.mv->render(0, kBlk);
  u.mv->end_block();
  NEED(u.mv->feed(0) != nullptr);
  CHECK(u.mv->feed(0)[kBlk - 1] == 16384);  // the block is full of the previous audio
  u.mv->begin_block();
  u.mv->render(37, kBlk);  // the next block: a note that starts at frame 37 (an odd one: the stores are halfwords)
  u.mv->end_block();
  const std::int16_t* f = u.mv->feed(0);
  NEED(f != nullptr);
  for (std::uint32_t i = 0; i < 37; ++i) {
    CHECK(f[i] == 0);  // exactly silent before its sample
  }
  for (std::uint32_t i = 37; i < kBlk; ++i) {
    CHECK(f[i] == 16384);
  }
}

// ---- what the accounting costs (target only: QEMU runs with -icount shift=5, so the guest's milliseconds are instructions: 31,250 of them each) --------------------------------------------
#if defined(__arm__)
namespace {

// 8000 blocks with `instances` sounding instances of a module that does nothing but advance the fake counter: the governor's whole work (the stamps in render(), end_block's account_block,
// the engine's block_done) is what separates the governed run from the ungoverned one. The notes start three blocks apart, so that the cold assumption never refuses one.
std::uint32_t instructions_per_block(bool governed, unsigned instances, bool perf = false) {
  Unit u(governed);
  if (perf) {
    u.mv->reset_perf();  // OS-303PERF: the split on (it is off from boot)
  }
  for (unsigned s = 0; s < instances; ++s) {
    CHECK(u.bind(s, 20000));
  }
  std::uint32_t stages[haudio::kStageCount] = {};
  stages[haudio::kStageRender] = 20000u * instances;
  for (unsigned s = 0; s < instances; ++s) {
    u.block(0, [&] { CHECK(u.mv->note_on(s, 60, s, 100)); });
    u.block(0);  // two blocks measure it: until then a sounding instance counts as the cold assumption (20 % each: the fifth of eight would be refused)
    u.block(0);
  }
  constexpr unsigned kBlocks = 8000;
  const std::uint64_t t0 = platform::qemu::system_clock().uptime_ms();
  for (unsigned b = 0; b < kBlocks; ++b) {
    u.mv->begin_block();
    u.mv->render(0, kBlk);
    u.mv->end_block();
    if (governed) {
      u.mv->block_done(stages);
    }
  }
  return static_cast<std::uint32_t>((platform::qemu::system_clock().uptime_ms() - t0) * 31250u / kBlocks);
}

}  // namespace

TEST_CASE("governor: the accounting costs a few hundred instructions a block, on the target (QEMU icount)") {
  const std::uint32_t base0 = instructions_per_block(false, 0);
  const std::uint32_t gov0 = instructions_per_block(true, 0);
  const std::uint32_t base1 = instructions_per_block(false, 1);
  const std::uint32_t gov1 = instructions_per_block(true, 1);
  const std::uint32_t base8 = instructions_per_block(false, 8);
  const std::uint32_t gov8 = instructions_per_block(true, 8);
  std::printf("[governor] accounting overhead per block, instructions: no instance %d, one instance %d, eight instances %d (blocks of %u..%u instructions without it)\n",
              static_cast<int>(gov0 - base0), static_cast<int>(gov1 - base1), static_cast<int>(gov8 - base8), static_cast<unsigned>(base0), static_cast<unsigned>(base8));
  CHECK(gov0 - base0 < 150u);    // no module sounding: block_done's sum of eight stages (measured 66; the resolution is 4 instructions: 8000 blocks of 31,250 per ms)
  CHECK(gov1 - base1 < 400u);    // one instance (measured 203): well under 0.2 % of a 261,120-cycle block
  CHECK(gov8 - base8 < 1000u);   // eight (measured 672 before OS-303PERF; the split is off here: one flag read a segment more): under 0.4 %
  // OS-303PERF: the split on (`diag cpu reset`): three counter reads, the adds and one fold a block for each sounding instance
  const std::uint32_t perf1 = instructions_per_block(true, 1, true);
  const std::uint32_t perf8 = instructions_per_block(true, 8, true);
  std::printf("[governor] with the perf split on, instructions per block: one instance %d, eight instances %d (the governed run without it: %d, %d)\n", static_cast<int>(perf1 - base1),
              static_cast<int>(perf8 - base8), static_cast<int>(gov1 - base1), static_cast<int>(gov8 - base8));
  CHECK(perf1 - base1 < 500u);
  CHECK(perf8 - base8 < 2300u);  // (measured 1972: about 150 instructions an instance and block, 0.07 % of a 261,120-cycle block)
}
#endif
