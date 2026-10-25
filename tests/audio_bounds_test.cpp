// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-BUG-WTLFO: no sample read leaves the sample. A seeded property test over every play mode of the sampler voices (one shot, the three loops, SLICE, BEAT SLICE, WAVETABLE, GRANULAR) with random
// parameters, random modulators (ADSR and LFO on the pan, the finetune, the amp, the cutoff and the position), live window / position / loop edits, retriggers and a clock that is stopped, started and
// restarted. The oracle needs no sanitizer: a sample is a window inside a buffer whose surround is filled with a guard pattern, and every scenario is run twice with two different guards: a voice that
// read one sample outside its window makes the two runs differ (their output is hashed). Under ASan the exact-size heap buffer of each run catches the same read as well. Bounded: kScenarios x 2
// runs of at most kBlocks blocks. Host and Cortex-M4F.
#include <doctest/doctest.h>

#include <o3os/audio/voices.h>
#include <o3os/hal/audio.h>

#include "support/static_slot.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

namespace audio = o3os::audio;
namespace ip = audio::inst_param;
using audio::SampleDesc;
using audio::VoicePool;

constexpr std::uint32_t kBlk = o3os::hal::audio::kBlock;
constexpr unsigned kScenarios = 240;
constexpr unsigned kBlocks = 36;
constexpr unsigned kGuard = 96;  // samples of guard on each side of the window

struct Rng {
  std::uint32_t s;
  std::uint32_t next() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  }
  unsigned below(unsigned n) { return n == 0u ? 0u : next() % n; }
  float unit() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }
  float range(float lo, float hi) { return lo + (hi - lo) * unit(); }
  bool coin(unsigned percent = 50) { return below(100) < percent; }
};

enum Mode : unsigned { kOneShot, kFwd, kBwd, kPing, kSlice, kBeat, kWave, kGran, kModes };

audio::FilterMod random_mod(Rng& r, bool lfo_only = false) {
  audio::FilterMod m;
  const unsigned k = r.below(3);
  m.kind = k == 0u ? audio::kFmodNone : (k == 1u && !lfo_only ? audio::kFmodEnv : audio::kFmodLfo);
  m.shape = static_cast<std::uint8_t>(r.below(6));
  m.rate = static_cast<std::uint8_t>(r.below(32));
  m.amount = r.range(-1.5f, 1.5f);
  m.lfo_amount = m.amount;
  m.sustain = r.unit();
  m.delay = static_cast<std::uint16_t>(r.below(20));
  m.attack = static_cast<std::uint16_t>(r.below(60));
  m.hold = static_cast<std::uint16_t>(r.below(20));
  m.decay = static_cast<std::uint16_t>(r.below(60));
  m.release = static_cast<std::uint16_t>(r.below(60));
  return m;
}

// One scenario with the guard pattern `guard`: the hash of everything the voices put out.
std::uint32_t run(std::uint32_t seed, std::int16_t guard) {
  Rng r{seed | 1u};
  static o3os::test::StaticSlot<VoicePool> slot;
  VoicePool& p = slot.make();
  const unsigned mode = r.below(kModes);

  // The window: `len` samples inside a buffer of len + 2 x kGuard that the allocation holds exactly (ASan's redzone is the next byte past it).
  unsigned wt_size = 0;
  unsigned windows = 0;
  unsigned len;
  if (mode == kWave) {
    wt_size = 32u << r.below(7);  // 32 .. 2048
    windows = 2u + r.below(7);
    len = wt_size * windows;
  } else {
    len = 2u + r.below(mode == kGran ? 6000u : 3000u);
  }
  std::vector<std::int16_t> buf(len + 2u * kGuard);
  Rng content{seed ^ 0x9E3779B9u};
  for (unsigned i = 0; i < buf.size(); ++i) {
    const bool inside = i >= kGuard && i < kGuard + len;
    buf[i] = inside ? static_cast<std::int16_t>(static_cast<std::int32_t>(content.below(20001)) - 10000) : guard;
  }
  std::int16_t* const window = buf.data() + kGuard;

  SampleDesc d{window, len, 0, 0, 65536};
  d.gain = static_cast<std::int32_t>(r.below(131072));
  d.tune = static_cast<std::int8_t>(static_cast<int>(r.below(49)) - 24);
  d.fine_tune = static_cast<std::int8_t>(static_cast<int>(r.below(201)) - 100);
  std::vector<std::uint16_t> table;
  switch (mode) {
    case kFwd:
    case kBwd:
    case kPing: {
      d.loop_start = r.below(len);
      d.loop_end = d.loop_start + 2u + r.below(len);  // may leave the sample: set_sample drops it
      d.loop_mode = static_cast<std::uint8_t>(mode - kFwd);
      break;
    }
    case kSlice: {  // the window is a slice of a larger sample: the neighbours are the sample's own audio
      break;
    }
    case kBeat: {
      d.beat_slice = true;
      const unsigned n = r.below(10);
      table.resize(2u * (n + 1u));
      std::uint32_t b = 0;
      for (unsigned i = 0; i <= n; ++i) {
        b = i == n && r.coin(70) ? len : (r.coin(15) ? r.next() : b + r.below(len / (n + 1u) + 1u));  // sometimes garbage boundaries: the voices clamp them
        table[2u * i] = static_cast<std::uint16_t>(b & 0xFFFFu);
        table[2u * i + 1u] = static_cast<std::uint16_t>(b >> 16);
      }
      d.slice_count = static_cast<std::uint8_t>(n);
      d.slices = n != 0u ? table.data() : nullptr;
      break;
    }
    case kWave: {
      d.length = wt_size;
      d.loop_start = 0;
      d.loop_end = wt_size;
      d.wt_size = static_cast<std::uint16_t>(wt_size);
      d.wt_windows = windows;
      d.wt_pos = r.below(windows);
      d.data = window + d.wt_pos * wt_size;
      break;
    }
    case kGran: {
      d.granular = true;
      d.gr_len = static_cast<std::uint16_t>(44u + r.below(44100u - 44u));
      d.gr_pos = static_cast<std::uint16_t>(r.below(65536u));
      d.gr_shape = static_cast<std::uint8_t>(r.below(4));
      d.loop_mode = static_cast<std::uint8_t>(r.below(4));
      break;
    }
    default: break;
  }
  if (r.coin(40)) {
    d.filter.enable = d.filter.wanted = true;
    d.filter.type = static_cast<std::uint8_t>(r.below(3));
    d.filter.cutoff = r.unit();
    d.filter.resonance = r.range(0.0f, 4.3f);
    d.filter.mod = random_mod(r);
  }
  if (r.coin(50)) d.pan_mod = random_mod(r);
  if (r.coin(50)) d.tune_mod = random_mod(r);
  if (r.coin(50)) d.amp_mod = random_mod(r, true);
  if (r.coin(80)) d.pos_mod = random_mod(r);
  if (r.coin(30)) {
    d.env.enable = true;
    d.env.attack = static_cast<std::uint16_t>(1u + r.below(200));
    d.env.decay = static_cast<std::uint16_t>(1u + r.below(200));
    d.env.release = static_cast<std::uint16_t>(1u + r.below(200));
    d.env.sustain = static_cast<std::uint32_t>(r.unit() * 1073741823.0f);
  }
  p.set_sample(0, d);
  p.set_tempo_x100(2000u + r.below(60000u));
  if (r.coin(50)) {
    p.restart_clock();
    p.on_step(r.below(128));
  }

  std::uint32_t hash = 2166136261u;
  const unsigned note = r.below(128);
  p.note_on(0, note, 0, r.coin(30) ? r.below(128) : audio::kVelocityDefault);
  for (unsigned b = 0; b < kBlocks; ++b) {
    // events between blocks: live edits, a retrigger, a note off, the clock
    if (r.coin(15)) {
      std::uint32_t pan = 0;
      const unsigned which = r.below(5);
      if (which == 0u) {
        (void)p.apply_param(0, ip::WtWindow, static_cast<float>(r.below(12)) - 2.0f, pan);
      } else if (which == 1u) {
        (void)p.apply_param(0, ip::GranPos, static_cast<float>(r.below(70000)) - 2000.0f, pan);
      } else if (which == 2u) {
        (void)p.apply_param(0, ip::LoopStart, static_cast<float>(r.below(len + 40)), pan);
      } else if (which == 3u) {
        (void)p.apply_param(0, ip::LoopEnd, static_cast<float>(r.below(len + 40)), pan);
      } else {
        (void)p.apply_param(0, ip::Tune, static_cast<float>(r.below(49)) - 24.0f, pan);
      }
    }
    if (r.coin(8)) p.note_on(0, r.below(128), 0, audio::kVelocityDefault);
    if (r.coin(5)) p.note_off(0);
    if (r.coin(10)) p.on_step(r.below(128));
    if (r.coin(5)) p.transport_stopped();
    p.begin_block();
    p.render(0, kBlk / 2);
    p.render(kBlk / 2, kBlk);
    p.end_block();
    const std::int16_t* f = p.feed(0);
    for (std::uint32_t i = 0; i < kBlk; ++i) {
      hash = (hash ^ static_cast<std::uint16_t>(f != nullptr ? f[i] : std::int16_t{0})) * 16777619u;
    }
  }
  return hash;
}

}  // namespace

TEST_CASE("bounds: no play mode reads a sample outside its window, whatever the parameters, the modulation and the live edits (two guards, one output)") {
  unsigned bad = 0;
  std::uint32_t first_bad = 0;
  for (unsigned i = 0; i < kScenarios; ++i) {
    const std::uint32_t seed = 0x1000u + i * 7919u;
    if (run(seed, 0) != run(seed, std::int16_t{0x5A5B})) {
      first_bad = bad == 0u ? seed : first_bad;
      ++bad;
    }
  }
  CHECK_MESSAGE(bad == 0u, bad << " scenarios read outside their sample; the first has seed " << first_bad);
}
