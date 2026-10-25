// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared by audio_wtlfo_test.cpp and audio_lfolive_test.cpp: a voice pool with a looping descriptor whose one modulator is an LFO, for each of the six destinations that share the transport-phase
// generator (the tremolo, the pan, the finetune, the cutoff, the wavetable window, the granular position), and what one block says about each. Every test file has its own copy (anonymous namespace).
#pragma once

#include <o3os/audio/voices.h>
#include <o3os/hal/audio.h>

#include "static_slot.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#define LFO_REQ(cond) \
  do {            \
    if (!(cond)) { \
      FAIL_CHECK("precondition failed: " #cond); \
      return;     \
    }             \
  } while (0)

namespace {

namespace audio = o3os::audio;
namespace haudio = o3os::hal::audio;
namespace ip = audio::inst_param;
using audio::SampleDesc;
using audio::VoicePool;

constexpr std::uint32_t kBlk = haudio::kBlock;
constexpr unsigned kWin = 256;
constexpr unsigned kWindows = 8;
constexpr std::uint32_t kTempo = 30000;  // 300 BPM: a step is 50 ms
constexpr std::uint8_t kRate8 = 8;       // the LFO rate index of 8 steps: a period of 0.4 s = about 276 blocks at 300 BPM
constexpr unsigned kPeriodBlocks = 280;

std::int16_t g_stack[kWin * kWindows];
std::int16_t g_const[4096];
std::int16_t g_noise[4096];
std::int16_t g_ramp[400000];

[[maybe_unused]] void fill() {
  for (unsigned w = 0; w < kWindows; ++w) {
    for (unsigned i = 0; i < kWin; ++i) {
      g_stack[w * kWin + i] = static_cast<std::int16_t>((w + 1u) * 1000u);
    }
  }
  std::uint32_t s = 7u;
  for (unsigned i = 0; i < 4096; ++i) {
    s = s * 1664525u + 1013904223u;
    g_const[i] = 10000;
    g_noise[i] = static_cast<std::int16_t>(static_cast<std::int32_t>(s >> 17) - 16384);
  }
  for (unsigned i = 0; i < 400000; ++i) {
    g_ramp[i] = static_cast<std::int16_t>(i % 20000u);
  }
}

[[maybe_unused]] audio::FilterMod lfo(std::uint8_t shape, std::uint8_t rate, float amount) {
  audio::FilterMod m;
  m.kind = audio::kFmodLfo;
  m.shape = shape;
  m.rate = rate;
  m.amount = amount;
  m.lfo_amount = amount;
  m.lfo = true;
  return m;
}

// The destinations: the six slots, each a descriptor whose one modulator is a saw LFO of 8 steps and amount 1, and what a block says about it.
enum Dest : unsigned { kAmp, kPan, kTune, kCut, kWt, kGran, kDests };

[[maybe_unused]] SampleDesc desc_for(Dest dest, const audio::FilterMod& m) {
  fill();
  SampleDesc d;
  switch (dest) {
    case kWt:
      d = SampleDesc{g_stack + 4 * kWin, kWin, 0, kWin, 65536};  // window 4 of 8: the saw swings it over the whole stack
      d.wt_size = kWin;
      d.wt_windows = kWindows;
      d.wt_pos = 4;
      d.pos_mod = m;
      break;
    case kGran:
      d = SampleDesc{g_ramp, 20000, 0, 0, 65536};  // a ramp: where the grain sits is told by its level
      d.granular = true;
      d.gr_len = 441;
      d.gr_pos = 32768;
      d.pos_mod = m;
      break;
    case kAmp:
      d = SampleDesc{g_const, 4096, 0, 4096, 65536};
      d.amp_mod = m;
      break;
    case kPan:
      d = SampleDesc{g_const, 4096, 0, 4096, 65536};
      d.pan_mod = m;
      break;
    case kTune:
      d = SampleDesc{g_ramp, 400000, 0, 0, 65536};
      d.tune_mod = m;
      break;
    default:
      d = SampleDesc{g_noise, 4096, 0, 4096, 65536};
      d.filter.enable = d.filter.wanted = true;
      d.filter.type = audio::kFilterLow;
      d.filter.cutoff = 0.5f;
      d.filter.resonance = 0.0f;
      d.filter.mod = m;
      break;
  }
  return d;
}

[[maybe_unused]] VoicePool& pool(unsigned which) {
  static o3os::test::StaticSlot<VoicePool> a, b, c;
  return which == 0 ? a.make() : (which == 1 ? b.make() : c.make());
}

[[maybe_unused]] double block_mean(const std::int16_t* f) {
  double s = 0;
  for (std::uint32_t i = 0; i < kBlk; ++i) {
    s += f[i];
  }
  return s / kBlk;
}

[[maybe_unused]] double block_rms(const std::int16_t* f) {
  double s = 0;
  for (std::uint32_t i = 0; i < kBlk; ++i) {
    s += static_cast<double>(f[i]) * f[i];
  }
  return std::sqrt(s / kBlk);
}

// One block of track 0; what the destination makes of it: the tremolo's level (the block's mean), the autopan's pan, the finetune's advance in samples (the position of the live voice), the cutoff's
// rms, the window (its level), the granular position (the block's mean level).
struct Probe {
  Dest dest;
  std::uint32_t last_pos = 0;
};

[[maybe_unused]] double step_block(VoicePool& p, Probe& pr) {
  p.begin_block();
  p.render(0, kBlk);
  p.end_block();
  const std::int16_t* f = p.feed(0);
  if (f == nullptr) {
    return -1.0;
  }
  switch (pr.dest) {
    case kPan: return static_cast<double>(p.pan_mod_value(0));
    case kTune: {
      const std::uint32_t pos = p.live_position() & 0xFFFFFFu;
      const double adv = static_cast<double>(pos >= pr.last_pos ? pos - pr.last_pos : 0u);
      pr.last_pos = pos;
      return adv;
    }
    case kCut: return block_rms(f);
    case kWt: return static_cast<double>((f[kBlk - 1] + 500) / 1000 - 1);
    default: return block_mean(f);
  }
}

// The series of `blocks` blocks of a voice that sounds from block 0, the transport as it is (stopped: nothing told; running: restart_clock and one Step event).
[[maybe_unused]] std::vector<double> series(Dest dest, const audio::FilterMod& m, unsigned blocks, bool running) {
  VoicePool& p = pool(0);
  p.set_sample(0, desc_for(dest, m));
  p.set_tempo_x100(kTempo);
  if (running) {
    p.restart_clock();
    p.on_step(0);
  } else {
    p.restart_clock();  // (the pool is reused: a start, then a stop, is a clock at rest)
    p.transport_stopped();
  }
  p.note_on(0, 60, 0, audio::kVelocityDefault);
  p.mark_live();
  Probe pr{dest, 0};
  std::vector<double> out;
  for (unsigned b = 0; b < blocks; ++b) {
    out.push_back(step_block(p, pr));
  }
  return out;
}

[[maybe_unused]] double spread(const std::vector<double>& v, std::size_t from) {
  double lo = 1e30, hi = -1e30;
  for (std::size_t i = from; i < v.size(); ++i) {
    lo = std::min(lo, v[i]);
    hi = std::max(hi, v[i]);
  }
  return hi - lo;
}

[[maybe_unused]] unsigned distinct(const std::vector<double>& v, std::size_t from, double quantum) {
  std::vector<long> q;
  for (std::size_t i = from; i < v.size(); ++i) {
    q.push_back(std::lround(v[i] / quantum));
  }
  std::sort(q.begin(), q.end());
  return static_cast<unsigned>(std::unique(q.begin(), q.end()) - q.begin());
}

}  // namespace

