// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host bench of the sequencer's step() (seq/sequencer.h, OS-PFX): RELATIVE figures only (the host is not the MK66; the cycle figure is the unit's `diag cpu`). It
// answers one question: what does one 64-sample block of the sequencer cost, for an idle pattern, a dense one (a note on every step of every track) and the same
// dense pattern carrying the OS-PFX step FX (Chord, Arp, Tune, Slide), so that a change to the tick can be compared before and after on the same machine.
//
//   o3os_seq_bench [--smoke]   the table (build with -DCMAKE_BUILD_TYPE=Release for numbers); --smoke runs each case once, as ctest does
#include <o3os/audio/events.h>
#include <o3os/seq/sequencer.h>

#include "../support/seq_fixture.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace o3os;
using namespace o3os::test;
using Clock = std::chrono::steady_clock;

namespace {

bool g_smoke = false;
volatile std::uint64_t g_sink = 0;

constexpr std::uint32_t kBlock = 64;
constexpr std::uint32_t kTracks = 12;
constexpr std::uint32_t kSteps = 16;
constexpr std::uint32_t kMidiSlot = 10;  // fresh_project(): a MIDI instrument on channel 3

enum class Load { Idle, Dense, DenseFx };

// 12 tracks of 16 steps: tracks 0..7 sample instruments, tracks 8..11 the MIDI one. Dense: a note on every step. DenseFx: the same notes with the OS-PFX FX: MIDI tracks
// Chord (+ Arp every fourth step), sample tracks Tune in the first column and Slide up in the second.
void build(Load load) {
  fresh_project(kTracks, 4);
  model::Project& p = seq_project();
  (void)p.set_tempo_x100(14000);
  (void)p.create_pattern(1, kSteps);
  if (load == Load::Idle) {
    return;
  }
  for (std::uint32_t t = 0; t < kTracks; ++t) {
    for (std::uint32_t r = 0; r < kSteps; ++r) {
      const std::uint8_t note = static_cast<std::uint8_t>(48 + (t * 3 + r) % 24);
      const bool midi = t >= 8;
      const std::uint8_t instr = midi ? static_cast<std::uint8_t>(kMidiSlot + 1) : static_cast<std::uint8_t>(1 + t % 4);
      model::Step s = make_step(note, instr);
      if (load == Load::DenseFx) {
        if (midi) {
          s.fx[0] = {14, static_cast<std::uint8_t>(1 + r % 15)};  // Chord
          if (r % 4 == 3) {
            s.fx[1] = {21, static_cast<std::uint8_t>(1 + r % 33)};  // Arp
          }
        } else {
          s.fx[0] = {40, static_cast<std::uint8_t>(18 + r % 13)};  // Tune
          s.fx[1] = {41, static_cast<std::uint8_t>(16 + 8 * (r % 4))};  // Slide up
        }
      }
      (void)p.set_step(1, t, r, s);
    }
  }
}

struct Result {
  double mean_ns;        // every block, the best of the runs
  double step_start_ns;  // the blocks that hold a step's first microstep (the notes start there), the best of the runs; the cost of the two clock reads is taken off
};

// Nanoseconds of the clock reads that bracket a block, so that the step-start figure is the sequencer's.
double clock_overhead_ns() {
  double best = 1e18;
  for (unsigned r = 0; r < 5u; ++r) {
    const unsigned n = 20000;
    std::uint64_t acc = 0;
    for (unsigned i = 0; i < n; ++i) {
      const Clock::time_point a = Clock::now();
      const Clock::time_point b = Clock::now();
      acc += static_cast<std::uint64_t>((b - a).count());
    }
    const double ns = static_cast<double>(acc) / static_cast<double>(n);
    best = ns < best ? ns : best;
  }
  return best;
}

Result measure(Load load, unsigned reps, unsigned blocks, double overhead) {
  build(load);
  static audio::SeqEvent out[audio::kMaxEventsPerBlock];
  Result best{1e18, 1e18};
  for (unsigned r = 0; r < (g_smoke ? 1u : reps); ++r) {
    seq::Sequencer& s = seq_slot().make(seq_project());
    s.play_pattern(1);
    std::uint64_t t = 0;
    const unsigned n = g_smoke ? 200u : blocks;
    double total = 0.0;
    double starts = 0.0;
    unsigned start_blocks = 0;
    for (unsigned b = 0; b < n; ++b) {
      const Clock::time_point t0 = Clock::now();
      const std::uint32_t c = s.step(t, kBlock, out, audio::kMaxEventsPerBlock);
      const double ns = std::chrono::duration<double, std::nano>(Clock::now() - t0).count() - overhead;
      total += ns;
      g_sink += c;
      for (std::uint32_t i = 0; i < c; ++i) {
        if (out[i].kind == audio::EventKind::Step) {
          starts += ns;
          ++start_blocks;
          break;
        }
      }
      t += kBlock;
    }
    const double mean = total / static_cast<double>(n);
    const double step = start_blocks != 0u ? starts / static_cast<double>(start_blocks) : 0.0;
    best.mean_ns = mean < best.mean_ns ? mean : best.mean_ns;
    best.step_start_ns = step < best.step_start_ns ? step : best.step_start_ns;
  }
  return best;
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--smoke") == 0) {
      g_smoke = true;
    }
  }
  const unsigned kReps = 7;
  const unsigned kBlocks = 40000;  // about 58 s of audio at 44.1 kHz: some 140 bars at 140 BPM
  const double overhead = clock_overhead_ns();
  std::printf("o3os_seq_bench: step() of one %u-sample block, %u tracks, best of %u runs (ns, host; clock reads %.0f ns taken off)\n", kBlock, kTracks, kReps, overhead);
  std::printf("  %-20s %12s %18s\n", "", "every block", "step-start block");
  const Result idle = measure(Load::Idle, kReps, kBlocks, overhead);
  const Result dense = measure(Load::Dense, kReps, kBlocks, overhead);
  const Result fx = measure(Load::DenseFx, kReps, kBlocks, overhead);
  std::printf("  %-20s %9.1f ns %15.1f ns\n", "idle pattern", idle.mean_ns, idle.step_start_ns);
  std::printf("  %-20s %9.1f ns %15.1f ns\n", "dense, no FX", dense.mean_ns, dense.step_start_ns);
  std::printf("  %-20s %9.1f ns %15.1f ns   (%+.1f %% / %+.1f %% against dense)\n", "dense, OS-PFX FX", fx.mean_ns, fx.step_start_ns,
              dense.mean_ns > 0.0 ? 100.0 * (fx.mean_ns - dense.mean_ns) / dense.mean_ns : 0.0,
              dense.step_start_ns > 0.0 ? 100.0 * (fx.step_start_ns - dense.step_start_ns) / dense.step_start_ns : 0.0);
  return g_sink == ~std::uint64_t{0} ? 1 : 0;
}
