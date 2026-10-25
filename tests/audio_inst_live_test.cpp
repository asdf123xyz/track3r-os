// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-LIVEP: instrument parameter edits reach the sound. A ParamChange of kind Instrument, pushed on the engine's ring by the main loop (app::send_param_change), is drained at the
// start of the graph stage of a block (so a voice hears it from the next block), updates the PARAMETER fields of the engine's descriptor of the slot (VoicePool::apply_param, which
// shares apply_instrument_param with app::make_sample_desc) and the voices playing that instrument: gain (glided, no step), pan (the graph's one-block ramp), pitch, the filter. The
// engine's rejected counter does not rise for them. The oracles are written here from the descriptor's own definitions (the level law, the pitch law, the envelope's units). Host and
// Cortex-M4F; the equality with make_sample_desc is in inst_live_app_test.cpp (it needs the model).
#include <doctest/doctest.h>

#include <o3os/audio/engine.h>
#include <o3os/audio/event_source.h>
#include <o3os/audio/smoothing.h>
#include <o3os/audio/voices.h>
#include <o3os/core/param.h>
#include <o3os/hal/audio.h>

#include "support/need.h"
#include "support/static_slot.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {

namespace audio = o3os::audio;
namespace haudio = o3os::hal::audio;
namespace ip = audio::inst_param;
using audio::Engine;
using audio::EventKind;
using audio::LevelLaw;
using audio::SampleDesc;
using audio::SeqEvent;
using audio::VoicePool;
using o3os::core::ParamChange;
using o3os::core::ParamKind;

constexpr std::uint32_t kBlk = haudio::kBlock;
constexpr unsigned kSlot = 3;  // the instrument; the voices play it on track 0
constexpr std::uint32_t kLen = 8000;

std::uint32_t fake_cycles_counter = 0;
std::uint32_t fake_cycles() noexcept { return fake_cycles_counter += 7u; }

// The events the test hands the engine at the start of the next block (the sequencer's place).
struct Script {
  SeqEvent q[8];
  std::uint32_t n = 0;
  void note_on(unsigned note, unsigned slot, unsigned velocity = audio::kVelocityDefault) {
    SeqEvent e{};
    e.kind = EventKind::NoteOn;
    e.track = 0;
    e.a = static_cast<std::uint8_t>(note);
    e.b = static_cast<std::uint8_t>(slot);
    e.c = static_cast<std::uint8_t>(velocity);
    q[n++] = e;
  }
  void note_off() {
    SeqEvent e{};
    e.kind = EventKind::NoteOff;
    e.track = 0;
    q[n++] = e;
  }
};
Script g_script;

std::uint32_t scripted_step(void*, std::uint64_t, std::uint32_t, SeqEvent* out, std::uint32_t cap) noexcept {
  const std::uint32_t n = g_script.n < cap ? g_script.n : cap;
  for (std::uint32_t i = 0; i < n; ++i) {
    out[i] = g_script.q[i];
  }
  g_script.n = 0;
  return n;
}

struct Rig {
  Engine& e;
  VoicePool& p;
  alignas(8) std::int16_t in[haudio::kBlockSamples] = {};
  alignas(8) std::int16_t out[haudio::kBlockSamples] = {};
  std::int16_t feed[kBlk] = {};  // track 0's voice block of the last block

  template <int N = 0>
  static Rig make() {
    static o3os::test::StaticSlot<Engine> engine_slot;
    static o3os::test::StaticSlot<VoicePool> pool_slot;
    static audio::TapMem taps;
    static SeqEvent events[audio::kMaxEventsPerBlock];
    Engine& eng = engine_slot.make(&fake_cycles, taps);
    VoicePool& pool = pool_slot.make();
    eng.attach_voices(&pool);
    audio::EventSource src;
    src.step = &scripted_step;
    src.buffer = events;
    src.capacity = audio::kMaxEventsPerBlock;
    eng.attach_events(src);
    g_script.n = 0;
    return Rig{eng, pool};
  }
  void block() {
    haudio::BlockReport report{};
    const haudio::BlockIo io{in, out, 0, &report};
    e.render_block(io);
    const std::int16_t* f = p.feed(0);
    if (f != nullptr) {
      std::memcpy(feed, f, sizeof feed);
    } else {
      std::memset(feed, 0, sizeof feed);
    }
  }
  void blocks(unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
      block();
    }
  }
  bool push(unsigned field, float value) {
    return e.params().push(ParamChange{o3os::core::param_id(ParamKind::Instrument, kSlot, field), value, 0u});
  }
};

std::int16_t g_const[kLen];  // a constant level: the voice's output is its amplitude
std::int16_t g_ramp[kLen];   // one unit a frame: the voice's output slope is its pitch increment
std::int16_t g_nyq[kLen];    // a +-16000 square of period 8 (5.5 kHz): well inside a wide open low-pass, far above a closed one

void init_pcm() {
  for (std::uint32_t i = 0; i < kLen; ++i) {
    g_const[i] = 20000;
    g_ramp[i] = static_cast<std::int16_t>(i);
    g_nyq[i] = (i & 4u) != 0u ? -16000 : 16000;
  }
}

// A looping desc over `pcm`, as app::make_sample_desc makes one for an instrument whose fields are at their defaults and whose volume is `volume`.
SampleDesc desc_of(const std::int16_t* pcm, unsigned volume, bool loop = true) {
  init_pcm();
  SampleDesc d{pcm, kLen, 0, loop ? kLen : 0u, 65536};
  d.gain = audio::level_gain_q16(volume, LevelLaw::Og12);
  return d;
}

std::int32_t abs_max_step(const std::int16_t* a, std::uint32_t n) {
  std::int32_t m = 0;
  for (std::uint32_t i = 1; i < n; ++i) {
    const std::int32_t d = std::abs(static_cast<std::int32_t>(a[i]) - static_cast<std::int32_t>(a[i - 1]));
    m = d > m ? d : m;
  }
  return m;
}

double rms(const std::int16_t* a, std::uint32_t n) {
  double s = 0.0;
  for (std::uint32_t i = 0; i < n; ++i) {
    s += static_cast<double>(a[i]) * static_cast<double>(a[i]);
  }
  return std::sqrt(s / static_cast<double>(n));
}

}  // namespace

TEST_CASE("live instrument params: through the engine's ring the descriptor of the slot takes the value, and the rejected counter does not rise") {
  Rig r = Rig::make();
  r.p.set_sample(kSlot, desc_of(g_const, 50));
  r.p.set_law(LevelLaw::Og12);
  const std::uint32_t rejected = r.e.graph().params_rejected();
  NEED(r.push(ip::Volume, 80.0f));
  NEED(r.push(ip::Panning, 20.0f));
  NEED(r.push(ip::Tune, -7.0f));
  NEED(r.push(ip::FineTune, 33.0f));
  NEED(r.push(ip::FilterType, 1.0f));
  NEED(r.push(ip::FilterEnable, 1.0f));
  NEED(r.push(ip::Cutoff, 0.25f));
  NEED(r.push(ip::Resonance, 2.5f));
  NEED(r.push(ip::envelope(0, ip::EnvAttack), 100.0f));
  NEED(r.push(ip::envelope(0, ip::EnvDecay), 250.0f));
  NEED(r.push(ip::envelope(0, ip::EnvSustain), 0.5f));
  NEED(r.push(ip::envelope(0, ip::EnvRelease), 1000.0f));
  NEED(r.push(ip::envelope(0, ip::EnvAmount), 0.5f));
  NEED(r.push(ip::envelope(0, ip::EnvEnable), 1.0f));
  NEED(r.push(ip::Glide, 5.0f));       // valid, held by nothing of the descriptor: accepted, not rejected
  NEED(r.push(ip::DelaySend, 40.0f));
  r.block();
  const SampleDesc& d = r.p.sample(kSlot);
  CHECK(d.gain == audio::level_gain_q16(80, LevelLaw::Og12));
  CHECK(d.amiga_vol == 64u);  // level 80 is +14.4 dB: over Paula's full scale, held to 64 (OS-BUG-PASTE24)
  CHECK(d.pan == 20u);
  CHECK(d.tune == -7);
  CHECK(d.fine_tune == 33);
  CHECK(d.filter.enable);
  CHECK(d.filter.type == audio::kFilterHigh);
  CHECK(d.filter.cutoff == 0.25f);
  CHECK(d.filter.resonance == 2.5f);
  CHECK(d.env.enable);
  CHECK(d.env.attack == audio::env_units(100));
  CHECK(d.env.decay == audio::env_units(250));
  CHECK(d.env.release == audio::env_units(1000));
  CHECK(d.env.sustain == static_cast<std::uint32_t>(0.5f * 1073741823.0f));
  CHECK(d.env.amount == 32768);
  CHECK(r.e.graph().params_rejected() == rejected);
  // A value that names nothing is still refused: an unknown field, a slot past the table, a NaN.
  NEED(r.e.params().push(ParamChange{o3os::core::param_id(ParamKind::Instrument, kSlot, 0x90), 1.0f, 0u}));
  NEED(r.e.params().push(ParamChange{o3os::core::param_id(ParamKind::Instrument, 0, ip::kBase), 1.0f, 0u}));
  NEED(r.push(ip::Volume, std::nanf("")));
  r.block();
  CHECK(r.e.graph().params_rejected() == rejected + 3u);
  CHECK(r.p.sample(kSlot).gain == audio::level_gain_q16(80, LevelLaw::Og12));
}

TEST_CASE("live instrument params: an edit while a note plays is heard on the next block, the gain glides with no step, and a note that names a velocity keeps its own level") {
  Rig r = Rig::make();
  r.p.set_sample(kSlot, desc_of(g_const, 50));
  g_script.note_on(60, kSlot);
  r.blocks(4);  // past the attack ramp: steady at the instrument's gain
  const std::int32_t g50 = audio::level_gain_q16(50, LevelLaw::Og12);
  CHECK(r.feed[10] == static_cast<std::int16_t>((20000 * static_cast<std::int64_t>(g50) + 0x8000) >> 16));
  NEED(r.push(ip::Volume, 25.0f));
  r.block();  // the block that drains the change: its voice was rendered before the drain
  CHECK(r.feed[10] == static_cast<std::int16_t>((20000 * static_cast<std::int64_t>(g50) + 0x8000) >> 16));
  r.block();  // the next one is the glide: from the old level to the new over kDeclickSamples (one block), no step above the slope
  const std::int32_t g25 = audio::level_gain_q16(25, LevelLaw::Og12);
  const std::int32_t from = static_cast<std::int32_t>((20000 * static_cast<std::int64_t>(g50) + 0x8000) >> 16);
  const std::int32_t to = static_cast<std::int32_t>((20000 * static_cast<std::int64_t>(g25) + 0x8000) >> 16);
  CHECK(std::abs(static_cast<std::int32_t>(r.feed[0]) - from) <= (from - to) / 32 + 2);
  CHECK(abs_max_step(r.feed, kBlk) <= (from - to) / 32 + 2);  // twice the average slope
  CHECK(std::abs(static_cast<std::int32_t>(r.feed[kBlk - 1]) - to) <= (from - to) / 64 + 2);  // the last sample is one step short of the target
  r.block();
  CHECK(r.feed[0] == to);
  CHECK(r.feed[kBlk - 1] == to);
  // Up again; a note with a velocity is not the instrument's level, so the volume leaves it as it is.
  NEED(r.push(ip::Volume, 50.0f));
  r.blocks(3);
  CHECK(std::abs(static_cast<std::int32_t>(r.feed[5]) - from) <= 2);
  g_script.note_on(60, kSlot, 64);
  r.blocks(3);
  const std::int32_t vel = static_cast<std::int32_t>((20000 * static_cast<std::int64_t>((64u * 65536u + 63u) / 127u) + 0x8000) >> 16);
  CHECK(r.feed[5] == vel);
  NEED(r.push(ip::Volume, 100.0f));
  r.blocks(3);
  CHECK(r.feed[5] == vel);
}

TEST_CASE("live instrument params: the amp envelope's amount glides the voice, and its release time is the edited one when the note ends") {
  Rig r = Rig::make();
  SampleDesc d = desc_of(g_const, 50);
  d.env.enable = true;
  d.env.attack = 2;
  d.env.release = audio::env_units(20);
  r.p.set_sample(kSlot, d);
  g_script.note_on(60, kSlot);
  r.blocks(6);  // delay, attack and hold are a few groups of 8 samples; the decay is one group; sustain at kEnvOne
  const std::int32_t g50 = audio::level_gain_q16(50, LevelLaw::Og12);
  const std::int32_t full = static_cast<std::int32_t>((20000 * static_cast<std::int64_t>(g50) + 0x8000) >> 16);
  CHECK(std::abs(static_cast<std::int32_t>(r.feed[7]) - full) <= 2);
  NEED(r.push(ip::envelope(0, ip::EnvAmount), 0.5f));
  r.blocks(3);
  const std::int32_t half = static_cast<std::int32_t>((20000 * static_cast<std::int64_t>((static_cast<std::int64_t>(g50) * 32768 + 0x8000) >> 16) + 0x8000) >> 16);
  CHECK(std::abs(static_cast<std::int32_t>(r.feed[kBlk - 1]) - half) <= 3);
  CHECK(abs_max_step(r.feed, kBlk) <= (full - half) / 8 + 3);  // glided: no step of the whole change
  // The release time while the note is held: 100 ms from now on, so the note is still sounding well past the 20 ms the descriptor had at the note on.
  NEED(r.push(ip::envelope(0, ip::EnvRelease), 100.0f));
  r.blocks(2);
  g_script.note_off();
  const unsigned old_blocks = 1 + (20u * 441u / 10u) / kBlk;  // 20 ms
  r.blocks(old_blocks + 2);
  CHECK(r.p.voices_in_use() == 1u);
  r.blocks((100u * 441u / 10u) / kBlk + 4);
  CHECK(r.p.voices_in_use() == 0u);
}

TEST_CASE("live instrument params: the pitch follows the tune and fine tune of a note that plays, from the next block") {
  Rig r = Rig::make();
  r.p.set_sample(kSlot, desc_of(g_ramp, 50, false));
  g_script.note_on(60, kSlot, 127);  // velocity: the level is unity whatever the instrument's volume
  r.blocks(3);
  const auto slope = [&]() { return static_cast<double>(r.feed[kBlk - 1] - r.feed[0]) / static_cast<double>(kBlk - 1); };
  CHECK(std::fabs(slope() - 1.0) < 1e-3);
  NEED(r.push(ip::Tune, 12.0f));
  r.blocks(2);
  CHECK(std::fabs(slope() - 2.0) < 1e-3);
  NEED(r.push(ip::Tune, 0.0f));
  NEED(r.push(ip::FineTune, 100.0f));  // a semitone up
  r.blocks(2);
  CHECK(std::fabs(slope() - std::exp2(1.0 / 12.0)) < 0.02);  // the block's 64 samples are whole numbers: a slope is good to 1.6 %
  NEED(r.push(ip::FineTune, 0.0f));
  r.blocks(2);
  CHECK(std::fabs(slope() - 1.0) < 1e-3);
}

TEST_CASE("live instrument params: the cutoff of a voice that sounds moves at once (the state is kept), and a filter turned on or off takes the voice") {
  Rig r = Rig::make();
  SampleDesc d = desc_of(g_nyq, 100);
  d.filter.wanted = true;
  d.filter.enable = true;
  d.filter.type = audio::kFilterLow;
  d.filter.cutoff = 1.0f;
  r.p.set_sample(kSlot, d);
  g_script.note_on(60, kSlot, 127);
  r.blocks(6);
  const double open = rms(r.feed, kBlk);
  CHECK(open > 8000.0);  // a wide open low-pass passes the Nyquist tone
  NEED(r.push(ip::Cutoff, 0.1f));
  r.block();  // drained after this block's voices
  CHECK(std::fabs(rms(r.feed, kBlk) - open) < 1.0);
  r.block();  // the next block's voice has it, and keeps its state: the 38 Hz low-pass lets the old state ring down over a few ms (no step)
  CHECK(r.p.filter_cutoff(0) == doctest::Approx(0.1f));
  CHECK(abs_max_step(r.feed, kBlk) < 6000);
  r.blocks(40);
  CHECK(rms(r.feed, kBlk) < open * 0.05);
  NEED(r.push(ip::FilterEnable, 0.0f));  // off: bypassed, the square is back
  r.blocks(2);
  CHECK(r.p.filter_cutoff(0) < 0.0f);
  CHECK(rms(r.feed, kBlk) > 15000.0);
  NEED(r.push(ip::FilterEnable, 1.0f));
  r.blocks(40);
  CHECK(r.p.filter_cutoff(0) == doctest::Approx(0.1f));
  CHECK(rms(r.feed, kBlk) < open * 0.05);
  NEED(r.push(ip::FilterType, 1.0f));  // high-pass: the square passes again
  r.blocks(3);
  CHECK(rms(r.feed, kBlk) > 8000.0);
}

TEST_CASE("live instrument params: the pan of a voice that plays moves the track's pan within the next block, to what a note starting there would have") {
  Rig r = Rig::make();
  r.p.set_sample(kSlot, desc_of(g_const, 100));  // pan 50: both channels
  g_script.note_on(60, kSlot, 127);
  r.blocks(6);
  const auto lr = [&]() { return std::pair<int, int>{r.out[haudio::kBlockSamples - 2], r.out[haudio::kBlockSamples - 1]}; };
  const std::pair<int, int> centre = lr();
  CHECK(centre.first == centre.second);
  CHECK(centre.first != 0);
  NEED(r.push(ip::Panning, 100.0f));
  r.blocks(4);
  const std::pair<int, int> edited = lr();
  // What a fresh note of an instrument at pan 100 plays (the same engine, a second pool state).
  Rig f = Rig::make<1>();
  SampleDesc d = desc_of(g_const, 100);
  d.pan = 100;
  f.p.set_sample(kSlot, d);
  g_script.note_on(60, kSlot, 127);
  f.blocks(6);
  const std::pair<int, int> fresh = std::pair<int, int>{f.out[haudio::kBlockSamples - 2], f.out[haudio::kBlockSamples - 1]};
  CHECK(edited == fresh);
  CHECK(edited.first != edited.second);
}

TEST_CASE("live instrument params: more changes than the block drains wait for the next one, and an empty slot only checks the field") {
  Rig r = Rig::make();
  r.p.set_sample(kSlot, desc_of(g_const, 50));
  const unsigned many = audio::kMaxParamsPerBlock + 5u;
  for (unsigned i = 0; i < many; ++i) {
    NEED(r.push(ip::Volume, static_cast<float>(i % 2u == 0u ? 60 : 70)));
  }
  r.block();
  CHECK(r.e.params_applied() == audio::kMaxParamsPerBlock);
  r.block();
  CHECK(r.e.params_applied() == many);
  CHECK(r.p.sample(kSlot).gain == audio::level_gain_q16(((many - 1u) % 2u == 0u) ? 60 : 70, LevelLaw::Og12));
  // Slot 9 holds nothing: the change is accepted (the field is valid) and nothing is bound.
  const std::uint32_t rejected = r.e.graph().params_rejected();
  NEED(r.e.params().push(ParamChange{o3os::core::param_id(ParamKind::Instrument, 9, ip::Cutoff), 0.3f, 0u}));
  r.block();
  CHECK(r.e.graph().params_rejected() == rejected);
  CHECK(r.p.sample(9).length == 0u);
}
