// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-LIMIT: the master limiter (audio/master_limiter.h) on its own, on the host and on the Cortex-M4F (`qemu_audio_limiter`). It is a pure function of the planes it is given and of the
// controls it was told, so every case states what must come out:
//   * the ceiling: sines at +12 dB over a range of frequencies, steps, DC, impulses, a Nyquist square wave, noise, and random control changes every block, never above kCeiling;
//   * transparency: a signal under the ceiling and the threshold comes out bit for bit, kLatency samples late, also after a loud passage has been and gone (the release lands on 1.0);
//   * the fast stage against a double-precision brute-force model of the same arithmetic (a sliding maximum and a moving average written out sample by sample), through the block boundaries;
//   * the timings: the attack is a linear ramp over exactly kBlock samples that ends as the peak leaves the delay line; the fast stage's release and the slow stage's attack and release
//     are the 10 % to 90 % times they say, per preset;
//   * the controls (ranges, defaults, NaN, the switch, the mode), silence, DC, no NaN or infinity, no denormal, the report.
#include <doctest/doctest.h>

#include <o3os/audio/diag.h>
#include <o3os/audio/master_limiter.h>
#include <o3os/hal/audio.h>

#include "support/rng.h"
#include "support/static_slot.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace {

using namespace o3os::audio;
namespace lm = o3os::audio::limiter;

constexpr std::uint32_t kB = MasterLimiter::kBlock;
constexpr double kRate = static_cast<double>(o3os::hal::audio::sample_rate_mhz()) / 1000.0;  // the engine's rate: 44,100 Hz (44,117.647 Hz with -DO3OS_RATE_44100=OFF)
constexpr double kPi = 3.14159265358979323846;
constexpr double kCeil = static_cast<double>(MasterLimiter::kCeiling);

MasterLimiter& fresh() {
  static o3os::test::StaticSlot<MasterLimiter> slot;  // 1 KiB: not on the target's stack
  MasterLimiter& m = slot.make();
  m.reset();
  return m;
}

struct Lr {
  float l;
  float r;
};

// What the output looked like: the largest magnitude, and whether anything was not a normal finite number or zero.
struct Probe {
  double peak = 0.0;
  bool sane = true;
  void see(float v) {
    const double a = std::fabs(static_cast<double>(v));
    peak = a > peak ? a : peak;
    const int c = std::fpclassify(v);
    sane = sane && (c == FP_NORMAL || c == FP_ZERO);
  }
};

// Blocks of a generator through a limiter: `gen(n)` is the input at sample n of the stream, `sink(n, l, r)` sees the output of sample n.
struct Stream {
  MasterLimiter& m;
  std::uint64_t n = 0;
  alignas(8) float l[kB];
  alignas(8) float r[kB];
  explicit Stream(MasterLimiter& lim) : m(lim) {}
  template <typename Gen, typename Sink>
  void blocks(unsigned count, Gen gen, Sink sink) {
    for (unsigned b = 0; b < count; ++b) {
      for (std::uint32_t k = 0; k < kB; ++k) {
        const Lr s = gen(n + k);
        l[k] = s.l;
        r[k] = s.r;
      }
      m.process(l, r, kB);
      for (std::uint32_t k = 0; k < kB; ++k) {
        sink(n + k, l[k], r[k]);
      }
      n += kB;
    }
  }
  template <typename Gen>
  Probe run(unsigned count, Gen gen) {
    Probe p;
    blocks(count, gen, [&](std::uint64_t, float a, float b) {
      p.see(a);
      p.see(b);
    });
    return p;
  }
};

Lr both(double v) { return Lr{static_cast<float>(v), static_cast<float>(v)}; }
double db(double d) { return std::pow(10.0, d / 20.0); }

}  // namespace

TEST_CASE("limiter: the constants are what the header says") {
  CHECK(MasterLimiter::kBlock == 64u);
  CHECK(MasterLimiter::kLatency == 63u);
  CHECK(std::fabs(kCeil - db(-0.3)) < 1.0e-7);
  CHECK(MasterLimiter::kCeiling < 1.0f);
  CHECK(static_cast<double>(kB) / kRate * 1000.0 == doctest::Approx(1.45).epsilon(0.01));  // the look-ahead, ms
  CHECK(kMeterWindowBlocks == 22u);
}

TEST_CASE("limiter: a signal under the ceiling comes out bit for bit, kLatency samples late") {
  for (const bool on : {false, true}) {
    for (const unsigned mode : {0u, 1u}) {
      MasterLimiter& m = fresh();
      CHECK(m.set_param(lm::Switch, on ? 1.0f : 0.0f));
      CHECK(m.set_param(lm::Mode, static_cast<float>(mode)));
      o3os::test::Rng rng(1000u + mode);
      float want[4096];
      for (float& w : want) {
        w = (static_cast<float>(rng.below(2001)) - 1000.0f) / 2000.0f;  // +-0.5: under the ceiling and under the 92 % threshold
      }
      Stream s(m);
      std::uint64_t bad = 0;
      s.blocks(64, [&](std::uint64_t n) { return Lr{want[n % 4096u], -want[(n * 7u) % 4096u]}; },
               [&](std::uint64_t n, float a, float b) {
                 if (n >= MasterLimiter::kLatency) {
                   const std::uint64_t t = n - MasterLimiter::kLatency;
                   bad += (a == want[t % 4096u] ? 0u : 1u) + (b == -want[(t * 7u) % 4096u] ? 0u : 1u);
                 } else {
                   bad += (a == 0.0f && b == 0.0f) ? 0u : 1u;  // before the first sample arrives: silence
                 }
               });
      CHECK(bad == 0u);
      CHECK(m.gain() == 1.0f);
    }
  }
}

TEST_CASE("limiter: every kind of signal at +12 dB stays under the ceiling, whatever the controls") {
  const double amp = db(12.0);
  for (const bool on : {false, true}) {
    for (const unsigned mode : {0u, 1u}) {
      for (const int kind : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}) {
        MasterLimiter& m = fresh();
        CHECK(m.set_param(lm::Switch, on ? 1.0f : 0.0f));
        CHECK(m.set_param(lm::Mode, static_cast<float>(mode)));
        if (on) {
          CHECK(m.set_param(lm::Threshold, kind % 2 == 0 ? 50.0f : 100.0f));
          CHECK(m.set_param(lm::Attack, kind % 3 == 0 ? 1.0f : 300.0f));
          CHECK(m.set_param(lm::Release, kind % 4 == 0 ? 1.0f : 800.0f));
        }
        o3os::test::Rng rng(7u + static_cast<unsigned>(kind));
        Stream s(m);
        Probe p = s.run(400, [&](std::uint64_t n) -> Lr {
          const double t = static_cast<double>(n);
          switch (kind) {
            case 0: return both(amp * std::sin(2.0 * kPi * 100.0 * t / kRate));
            case 1: return both(amp * std::sin(2.0 * kPi * 1000.0 * t / kRate));
            case 2: return both(amp * std::sin(2.0 * kPi * 7919.0 * t / kRate));
            case 3: return both(amp * std::sin(2.0 * kPi * 20000.0 * t / kRate));
            case 4: return both(n < 5000 ? 0.0 : amp);                         // a step to DC
            case 5: return both(n % 997 == 0 ? 8.0 : 0.0);                     // impulses (+18 dB)
            case 6: return both((n & 1u) != 0u ? amp : -amp);                  // a Nyquist square wave
            case 7: return Lr{static_cast<float>(amp * std::sin(2.0 * kPi * 440.0 * t / kRate)), static_cast<float>(-3.0 * std::sin(2.0 * kPi * 441.0 * t / kRate))};
            case 8: return Lr{static_cast<float>((static_cast<double>(rng.below(20001)) - 10000.0) / 10000.0 * 3.0), static_cast<float>((static_cast<double>(rng.below(20001)) - 10000.0) / 10000.0 * 3.0)};
            default: return both(((n / 3000u) % 2u) != 0u ? 0.02 : amp * std::sin(2.0 * kPi * 3000.0 * t / kRate));  // bursts with gaps
          }
        });
        CHECK(p.peak <= kCeil);
        CHECK(p.sane);
        CHECK(m.finite());
      }
    }
  }
}

TEST_CASE("limiter: an impulse at any position of a block, and its mirror image on the other channel, never passes the ceiling") {
  // The sliding maximum is made of two halves (the block before's suffix and this block's prefix): every position of the peak against the block boundary is a case of its own.
  for (std::uint32_t pos = 0; pos < 3u * kB; ++pos) {
    for (const bool on_left : {true, false}) {
      MasterLimiter& m = fresh();
      Stream s(m);
      Probe p = s.run(8, [&](std::uint64_t n) -> Lr {
        const float quiet = 0.05f;
        const float v = n == 2u * kB + pos ? 6.0f : quiet;
        return on_left ? Lr{v, quiet} : Lr{quiet, -v};
      });
      CHECK(p.peak <= kCeil);
      CHECK(p.sane);
    }
  }
}

TEST_CASE("limiter: the ceiling holds through random control changes every block") {
  MasterLimiter& m = fresh();
  o3os::test::Rng rng(99);
  Stream s(m);
  Probe p;
  for (unsigned b = 0; b < 1500; ++b) {
    (void)m.set_param(lm::Switch, static_cast<float>(rng.below(2)));
    (void)m.set_param(lm::Mode, static_cast<float>(rng.below(2)));
    (void)m.set_param(lm::Threshold, 1.0f + static_cast<float>(rng.below(100)));
    (void)m.set_param(lm::Attack, 1.0f + static_cast<float>(rng.below(1000)));
    (void)m.set_param(lm::Release, 1.0f + static_cast<float>(rng.below(1000)));
    const double a = db(static_cast<double>(rng.below(300)) / 10.0 - 10.0);  // -10 .. +20 dB
    const unsigned pick = rng.below(3);
    s.blocks(1,
             [&](std::uint64_t n) -> Lr {
               const double t = static_cast<double>(n);
               if (pick == 0) return both(a * std::sin(2.0 * kPi * 300.0 * t / kRate));
               if (pick == 1) return Lr{static_cast<float>(a * (static_cast<double>(rng.below(2001)) - 1000.0) / 1000.0), static_cast<float>(a * std::sin(t))};
               return both((n % 64u) == 3u ? a : 0.0);
             },
             [&](std::uint64_t, float x, float y) {
               p.see(x);
               p.see(y);
             });
  }
  CHECK(p.peak <= kCeil);
  CHECK(p.sane);
  CHECK(m.finite());
}

TEST_CASE("limiter: the output converts to int16 at or under the ceiling's code") {
  MasterLimiter& m = fresh();
  Stream s(m);
  std::int32_t top = 0;
  s.blocks(300, [&](std::uint64_t n) { return both(db(12.0) * std::sin(2.0 * kPi * 1500.0 * static_cast<double>(n) / kRate)); },
           [&](std::uint64_t, float a, float) {
             const std::int32_t q = static_cast<std::int32_t>(std::lround(static_cast<double>(a) * 32768.0));
             top = (q < 0 ? -q : q) > top ? (q < 0 ? -q : q) : top;
           });
  CHECK(top <= 31658);  // 0.96605 * 32768 = 31,657.1
  CHECK(top >= 31600);  // and it is not much lower: the limiter holds the peak AT the ceiling, it does not give a level away
}

TEST_CASE("limiter: the attack is a linear ramp over kBlock samples that ends as the peak leaves the delay line") {
  // DC at 0.4, then from sample T0 a DC of 2.0: the gain that holds 2.0 to the ceiling is kCeiling / 2. The old signal's output goes down in a straight line over the 64 samples before
  // the new level's first sample leaves the line, and that one is at the ceiling exactly.
  MasterLimiter& m = fresh();
  constexpr std::uint64_t T0 = 3u * kB + 17u;
  Stream s(m);
  double worst = 0.0;
  double at_peak = 0.0;
  const double low = kCeil / 2.0;
  s.blocks(10, [&](std::uint64_t n) { return both(n < T0 ? 0.4 : 2.0); },
           [&](std::uint64_t n, float a, float) {
             if (n < MasterLimiter::kLatency + 20u) {
               return;
             }
             const std::uint64_t t = n - MasterLimiter::kLatency;  // the input sample this output is
             if (t < T0 && t + kB >= T0) {                         // the 64 outputs before the step
               const double i = static_cast<double>(t + kB - T0);  // 0..63
               const double want = 0.4 * (1.0 - i / 64.0 * (1.0 - low));  // the average of (64 - i) unities and i lows
               worst = std::fmax(worst, std::fabs(static_cast<double>(a) - want));
             } else if (t == T0) {
               at_peak = static_cast<double>(a);
             }
           });
  CHECK(worst < 1.0e-6);
  CHECK(at_peak == doctest::Approx(kCeil).epsilon(1.0e-6));
}

TEST_CASE("limiter: both stages against a brute-force double model of the same arithmetic, through the block boundaries and the quiet blocks") {
  for (const bool on : {false, true}) {
    for (const unsigned mode : {0u, 1u}) {
      MasterLimiter& m = fresh();
      CHECK(m.set_param(lm::Mode, static_cast<float>(mode)));
      CHECK(m.set_param(lm::Switch, on ? 1.0f : 0.0f));  // OFF: the slow stage is the identity
      CHECK(m.set_param(lm::Threshold, 60.0f));
      CHECK(m.set_param(lm::Attack, 7.0f));
      CHECK(m.set_param(lm::Release, 50.0f));
      constexpr unsigned kBlocks = 240;
      constexpr std::uint32_t kN = kBlocks * kB;
      static float inl[kN], inr[kN];
      static double ref_l[kN], ref_r[kN], mg[kN];
      o3os::test::Rng rng(500u + mode + (on ? 10u : 0u));
      static const double amps[] = {0.05, 0.4, 1.0, 2.0, 4.0, 0.7, 0.58, 0.02};
      double a = 0.4;
      for (std::uint32_t n = 0; n < kN; ++n) {
        if (rng.below(300) == 0) {
          a = amps[rng.below(8)];
        }
        inl[n] = static_cast<float>(a * (static_cast<double>(rng.below(2001)) - 1000.0) / 1000.0);
        inr[n] = static_cast<float>(a * (static_cast<double>(rng.below(2001)) - 1000.0) / 1000.0);
      }
      // the model: the slow stage's follower and gain, then the sliding maximum and the moving average of the fast stage, written out sample by sample
      const auto step = [](double ms) {
        const double x = 2200.0 / (kRate * ms);
        return x / (1.0 + 0.5 * x);
      };
      const double scale = mode == 0 ? 1.0 : 0.25;
      const double ca = step(7.0 * scale), cr = step(50.0 * scale), cb = step(mode == 0 ? 100.0 : 30.0);
      const double ts = on ? 0.6 : 1.0e6;
      static double xl[kN], xr[kN], pp[kN];
      double env = 0.0;
      for (std::uint32_t n = 0; n < kN; ++n) {
        const double l = static_cast<double>(inl[n]), r = static_cast<double>(inr[n]);
        const double p = std::fmax(std::fabs(l), std::fabs(r));
        if (on) {
          env += (p > env ? ca : cr) * (p - env);
        }
        const double g = env > ts ? ts / env : 1.0;
        xl[n] = l * g;
        xr[n] = r * g;
        pp[n] = p * g;
      }
      double gp = 1.0;
      for (std::uint32_t n = 0; n < kN; ++n) {
        double pm = 0.0;
        for (std::uint32_t j = 0; j < kB && j <= n; ++j) {
          pm = pp[n - j] > pm ? pp[n - j] : pm;
        }
        mg[n] = pm > kCeil ? kCeil / pm : 1.0;
        double sum = 0.0;
        for (std::uint32_t j = 0; j < kB; ++j) {
          sum += n >= j ? mg[n - j] : 1.0;
        }
        const double g = sum / 64.0;
        double rel = gp + cb * (1.0 - gp);
        rel = rel > 0.9999 ? 1.0 : rel;  // (the limiter snaps at the end of a block: the same value, 63 samples later at most: the check's tolerance covers the step)
        gp = g < rel ? g : rel;
        ref_l[n] = n >= MasterLimiter::kLatency ? xl[n - MasterLimiter::kLatency] * gp : 0.0;
        ref_r[n] = n >= MasterLimiter::kLatency ? xr[n - MasterLimiter::kLatency] * gp : 0.0;
      }
      Stream s(m);
      double worst = 0.0;
      double lowest = 1.0;
      std::uint64_t over = 0;
      s.blocks(kBlocks, [&](std::uint64_t n) { return Lr{inl[n], inr[n]}; },
               [&](std::uint64_t n, float a2, float b2) {
                 worst = std::fmax(worst, std::fmax(std::fabs(static_cast<double>(a2) - ref_l[n]), std::fabs(static_cast<double>(b2) - ref_r[n])));
                 lowest = std::fmin(lowest, mg[n]);
                 over += (std::fabs(static_cast<double>(a2)) > kCeil || std::fabs(static_cast<double>(b2)) > kCeil) ? 1u : 0u;
               });
      CHECK(worst < 3.0e-4);  // the two stages' float against double (the follower's coefficient, a division, the Q24 flooring) and the end-of-block snap of 1e-4
      CHECK(over == 0u);
      CHECK(lowest < 0.5);    // the case did reduce
    }
  }
}

TEST_CASE("limiter: the fast stage's release is its 10 % to 90 % time, per preset") {
  for (const unsigned mode : {0u, 1u}) {
    MasterLimiter& m = fresh();
    CHECK(m.set_param(lm::Mode, static_cast<float>(mode)));
    Stream s(m);
    // quiet DC, one loud sample, quiet DC again: the gain goes to kCeiling / 4 and recovers from there
    constexpr std::uint64_t T0 = 5u * kB;
    const double gmin = kCeil / 4.0;
    double t10 = -1.0, t90 = -1.0;
    for (unsigned b = 0; b < 400; ++b) {
      s.blocks(1, [&](std::uint64_t n) { return both(n == T0 ? 4.0 : 0.2); }, [](std::uint64_t, float, float) {});
      if (b < 8) {
        continue;  // the dip and its ramp-up are over by block 8 (the window's two ramps are 128 samples)
      }
      const double rec = (static_cast<double>(m.gain()) - gmin) / (1.0 - gmin);  // 0 at the bottom, 1 recovered
      const double ms = static_cast<double>(s.n) / kRate * 1000.0;
      if (t10 < 0.0 && rec >= 0.1) t10 = ms;
      if (t90 < 0.0 && rec >= 0.9) t90 = ms;
    }
    const double want = mode == 0 ? 100.0 : 30.0;
    CHECK(t10 > 0.0);
    CHECK(t90 > t10);
    CHECK(t90 - t10 == doctest::Approx(want).epsilon(0.08));  // +-1.45 ms of block resolution on the difference of two readings, and the bilinear step
  }
}

TEST_CASE("limiter: gain 1.0 again exactly after a loud passage: the quiet signal that follows is transparent") {
  MasterLimiter& m = fresh();
  Stream s(m);
  (void)s.run(100, [&](std::uint64_t n) { return both(db(12.0) * std::sin(2.0 * kPi * 900.0 * static_cast<double>(n) / kRate)); });
  CHECK(m.gain() < 0.5f);
  std::uint64_t bad = 0;
  std::uint64_t checked = 0;
  const std::uint64_t quiet_from = s.n;
  s.blocks(1500, [&](std::uint64_t n) { return both(0.3 * std::sin(2.0 * kPi * 250.0 * static_cast<double>(n) / kRate)); },
           [&](std::uint64_t n, float a, float) {
             if (n < quiet_from + 900u * kB) {  // (give the release 900 blocks, 1.3 s: 29 time constants of the slowest preset, ln(1e4) = 9.2 are needed)
               return;
             }
             const std::uint64_t t = n - MasterLimiter::kLatency;
             const float want = static_cast<float>(0.3 * std::sin(2.0 * kPi * 250.0 * static_cast<double>(t) / kRate));
             ++checked;
             bad += a == want ? 0u : 1u;
           });
  CHECK(checked > 10000u);
  CHECK(bad == 0u);
  CHECK(m.gain() == 1.0f);
}

TEST_CASE("limiter: the slow stage's attack and release are the 10 % to 90 % times of the controls, per preset") {
  for (const unsigned mode : {0u, 1u}) {
    MasterLimiter& m = fresh();
    CHECK(m.set_param(lm::Switch, 1.0f));
    CHECK(m.set_param(lm::Mode, static_cast<float>(mode)));
    CHECK(m.set_param(lm::Threshold, 50.0f));
    CHECK(m.set_param(lm::Attack, 200.0f));
    CHECK(m.set_param(lm::Release, 400.0f));
    const double scale = mode == 0 ? 1.0 : 0.25;
    Stream s(m);
    double level = 0.9;  // DC; the follower rises towards it, then falls to 0.1
    double ta10 = -1.0, ta90 = -1.0, tr10 = -1.0, tr90 = -1.0;
    double out_settled = 0.0;
    for (unsigned b = 0; b < 1500; ++b) {
      if (b == 500) {
        level = 0.1;
      }
      s.blocks(1, [&](std::uint64_t) { return both(level); },
               [&](std::uint64_t, float a, float) {
                 if (b == 499) out_settled = static_cast<double>(a);
               });
      const double e = static_cast<double>(m.envelope());
      const double ms = static_cast<double>(s.n) / kRate * 1000.0;
      if (b < 500) {
        if (ta10 < 0.0 && e >= 0.09) ta10 = ms;
        if (ta90 < 0.0 && e >= 0.81) ta90 = ms;
      } else {
        if (tr10 < 0.0 && e <= 0.9 - 0.08) tr10 = ms;
        if (tr90 < 0.0 && e <= 0.9 - 0.72) tr90 = ms;
      }
    }
    CHECK(ta90 - ta10 == doctest::Approx(200.0 * scale).epsilon(0.08));
    CHECK(tr90 - tr10 == doctest::Approx(400.0 * scale).epsilon(0.08));
    CHECK(out_settled == doctest::Approx(0.5).epsilon(1.0e-3));  // held to the 50 % threshold
  }
}

TEST_CASE("limiter: the slow stage's follower against the double recursion") {
  MasterLimiter& m = fresh();
  CHECK(m.set_param(lm::Switch, 1.0f));
  CHECK(m.set_param(lm::Attack, 30.0f));
  CHECK(m.set_param(lm::Release, 120.0f));
  const double xa = 2200.0 / (kRate * 30.0), xr = 2200.0 / (kRate * 120.0);
  const double ca = xa / (1.0 + 0.5 * xa), cr = xr / (1.0 + 0.5 * xr);
  o3os::test::Rng rng(31);
  Stream s(m);
  double env = 0.0;
  double worst = 0.0;
  std::uint64_t n = 0;
  float in_l[kB], in_r[kB];
  for (unsigned b = 0; b < 600; ++b) {
    const double a = (b / 100u) % 2u == 0u ? 0.8 : 0.05;
    for (std::uint32_t k = 0; k < kB; ++k, ++n) {
      in_l[k] = static_cast<float>(a * (static_cast<double>(rng.below(2001)) - 1000.0) / 1000.0);
      in_r[k] = static_cast<float>(a * (static_cast<double>(rng.below(2001)) - 1000.0) / 1000.0);
      const double p = std::fmax(std::fabs(static_cast<double>(in_l[k])), std::fabs(static_cast<double>(in_r[k])));
      env += (p > env ? ca : cr) * (p - env);
      env = env < 1.0e-6 ? 0.0 : env;
    }
    s.blocks(1, [&](std::uint64_t i) { return Lr{in_l[i % kB], in_r[i % kB]}; }, [](std::uint64_t, float, float) {});
    worst = std::fmax(worst, std::fabs(env - static_cast<double>(m.envelope())));
  }
  CHECK(worst < 1.0e-4);
}

TEST_CASE("limiter: the controls: defaults, ranges, refusals, and the switch decides whether the slow stage acts") {
  MasterLimiter& m = fresh();
  CHECK_FALSE(m.switched_on());
  CHECK(m.mode() == 0u);
  CHECK(lm::kThresholdDefault == 92.0f);
  CHECK(lm::kAttackDefault == 10.0f);
  CHECK(lm::kReleaseDefault == 100.0f);
  // refusals change nothing
  CHECK_FALSE(m.set_param(lm::Switch, 2.0f));
  CHECK_FALSE(m.set_param(lm::Mode, -1.0f));
  CHECK_FALSE(m.set_param(lm::Threshold, 0.5f));
  CHECK_FALSE(m.set_param(lm::Threshold, 100.5f));
  CHECK_FALSE(m.set_param(lm::Attack, 0.0f));
  CHECK_FALSE(m.set_param(lm::Release, 1000.5f));
  CHECK_FALSE(m.set_param(lm::Threshold, std::nanf("")));
  CHECK_FALSE(m.set_param(lm::Attack, INFINITY));
  CHECK_FALSE(m.set_param(99u, 1.0f));
  CHECK_FALSE(m.switched_on());
  // the ends are in
  CHECK(m.set_param(lm::Threshold, lm::kThresholdMin));
  CHECK(m.set_param(lm::Threshold, lm::kThresholdMax));
  CHECK(m.set_param(lm::Attack, lm::kTimeMin));
  CHECK(m.set_param(lm::Release, lm::kTimeMax));

  // DC at 0.95: under the ceiling. OFF: it passes at 0.95. ON at the default 92 %: the slow stage holds it to 0.92.
  for (const bool on : {false, true}) {
    MasterLimiter& x = fresh();
    CHECK(x.set_param(lm::Switch, on ? 1.0f : 0.0f));
    Stream s(x);
    float last = 0.0f;
    s.blocks(600, [](std::uint64_t) { return both(0.95); }, [&](std::uint64_t, float a, float) { last = a; });
    CHECK(static_cast<double>(last) == doctest::Approx(on ? 0.92 : 0.95).epsilon(2.0e-3));
  }
}

TEST_CASE("limiter: silence, DC and a full-scale impulse leave the state finite and bounded; silence in is silence out") {
  MasterLimiter& m = fresh();
  Stream s(m);
  Probe p = s.run(2000, [](std::uint64_t) { return both(0.0); });
  CHECK(p.peak == 0.0);
  CHECK(m.envelope() == 0.0f);
  CHECK(m.gain() == 1.0f);
  // DC at +4 for a long time: the output settles AT the ceiling and stays there
  CHECK(m.set_param(lm::Switch, 1.0f));
  float last_l = 0.0f, last_r = 0.0f;
  p = Probe{};
  s.blocks(3000, [](std::uint64_t) { return Lr{4.0f, -4.0f}; }, [&](std::uint64_t, float a, float b) {
    p.see(a);
    p.see(b);
    last_l = a;
    last_r = b;
  });
  CHECK(p.peak <= kCeil);
  CHECK(p.sane);
  CHECK(static_cast<double>(last_l) <= kCeil);
  CHECK(static_cast<double>(last_l) >= 0.9 * 0.92);  // held at the slow stage's 92 %, which is under the ceiling
  CHECK(static_cast<double>(last_r) == doctest::Approx(-static_cast<double>(last_l)).epsilon(1.0e-6));  // one gain for both channels
  // and a long silence afterwards: nothing decays into a denormal, the follower is exactly 0 and the gain exactly 1
  (void)s.run(2, [](std::uint64_t) { return both(0.0); });  // (the delay line empties)
  p = s.run(8000, [](std::uint64_t) { return both(0.0); });
  CHECK(p.peak == 0.0);
  CHECK(p.sane);
  CHECK(m.envelope() == 0.0f);
  CHECK(m.gain() == 1.0f);
  CHECK(m.finite());
}

TEST_CASE("limiter: one gain for both channels: the image does not move") {
  MasterLimiter& m = fresh();
  Stream s(m);
  double worst = 0.0;
  s.blocks(300, [](std::uint64_t n) { const double v = 3.0 * std::sin(2.0 * kPi * 600.0 * static_cast<double>(n) / kRate); return Lr{static_cast<float>(v), static_cast<float>(0.25 * v)}; },
           [&](std::uint64_t, float a, float b) {
             if (a != 0.0f) worst = std::fmax(worst, std::fabs(static_cast<double>(b) / static_cast<double>(a) - 0.25));
           });
  CHECK(worst < 1.0e-5);
}

TEST_CASE("limiter: a block of another size is left alone, and the report counts windows and reductions") {
  MasterLimiter& m = fresh();
  float l[32], r[32];
  for (unsigned i = 0; i < 32; ++i) {
    l[i] = 5.0f;
    r[i] = -5.0f;
  }
  m.process(l, r, 32);
  CHECK(l[0] == 5.0f);
  CHECK(r[31] == -5.0f);
  CHECK(m.report().windows == 0u);

  Stream s(m);
  LimiterReport rep = m.report();
  CHECK(rep.gain_now_q16 == 65536u);
  CHECK(rep.gain_hold_q16 == 65536u);
  (void)s.run(kMeterWindowBlocks, [](std::uint64_t) { return both(0.1); });  // a whole window, no reduction
  rep = m.report();
  CHECK(rep.windows == 1u);
  CHECK(rep.reduced == 0u);
  CHECK(rep.gain_now_q16 == 65536u);
  (void)s.run(kMeterWindowBlocks, [](std::uint64_t n) { return both(n % 500u == 0u ? 4.0 : 0.1); });  // impulses: the gain goes to 0.24
  rep = m.report();
  CHECK(rep.windows == 2u);
  CHECK(rep.reduced == 1u);
  CHECK(rep.gain_now_q16 < 20000u);
  CHECK(rep.gain_hold_q16 == rep.gain_now_q16);
  (void)s.run(kMeterWindowBlocks * 40u, [](std::uint64_t) { return both(0.1); });
  rep = m.report();
  CHECK(rep.windows == 42u);
  CHECK(rep.gain_now_q16 == 65536u);              // released
  CHECK(rep.gain_hold_q16 < 20000u);              // the lowest is remembered
  CHECK(rep.reduced >= 1u);
  m.reset();
  CHECK(m.report().windows == 0u);
  CHECK(m.report().gain_hold_q16 == 65536u);
}

TEST_CASE("limiter: `diag mix` has the stable limiter line right after the header, and the old format is unchanged") {
  MeterSnapshot snap{};
  snap.windows = 1;
  snap.frames = 1408;
  snap.ch[meter_ch::kTrack0].peak = 16384;
  snap.ch[meter_ch::kTrack0].hold = 16384;
  char plain[1024], with[1024];
  const std::size_t np = diag::format_mix(snap, plain, sizeof plain);
  CHECK(np == std::strlen(plain));
  CHECK(std::strstr(plain, "limiter") == nullptr);  // the three-argument form is as it was
  const char* const header = "mix  windows 1  frames 1408  (dBFS; clip = windows at full scale)\n";
  LimiterReport rep{0u, 0u, 65536u, 65536u, 0u};
  std::size_t n = diag::format_mix(snap, rep, with, sizeof with);
  CHECK(n == std::strlen(with));
  CHECK_MESSAGE(std::strncmp(with, header, std::strlen(header)) == 0, with);
  const char* line = with + std::strlen(header);
  CHECK_MESSAGE(std::strncmp(line, "limiter: gr 0.0 dB max 0.0 dB, mode subtle, ceiling -0.3 dBFS\n", 61) == 0, line);  // before the first window: no reduction
  rep = LimiterReport{340u, 12u, 32768u, 7000u, 1u};  // 6.0 dB now, 20 * log10(65536 / 7000) = 19.4 dB at the worst, mode Extreme
  n = diag::format_mix(snap, rep, with, sizeof with);
  line = with + std::strlen(header);
  CHECK_MESSAGE(std::strncmp(line, "limiter: gr 6.0 dB max 19.4 dB, mode hard, ceiling -0.3 dBFS\n", 59) == 0, line);
  CHECK(std::strstr(with, "  T01  pk") != nullptr);  // the channel lines follow
  const char* const eol = std::strchr(line, '\n');
  CHECK((eol != nullptr && static_cast<std::size_t>(eol - line) <= 64u));  // at most 64 columns
  // the extremes: no gain at all is held to 99.9, one hair under unity is 0.0
  rep = LimiterReport{9u, 9u, 0u, 0u, 0u};
  (void)diag::format_mix(snap, rep, with, sizeof with);
  CHECK(std::strstr(with, "limiter: gr 99.9 dB max 99.9 dB, mode subtle, ceiling -0.3 dBFS\n") != nullptr);
  rep = LimiterReport{9u, 0u, 65535u, 65535u, 0u};
  (void)diag::format_mix(snap, rep, with, sizeof with);
  CHECK(std::strstr(with, "limiter: gr 0.0 dB max 0.0 dB, mode subtle") != nullptr);
  char small[40];
  (void)diag::format_mix(snap, rep, small, sizeof small);
  CHECK(std::strlen(small) == sizeof small - 1u);  // bounded and terminated
  // with nothing measured the line is still there
  MeterSnapshot empty{};
  rep = LimiterReport{0u, 0u, 65536u, 65536u, 0u};
  (void)diag::format_mix(empty, rep, with, sizeof with);
  CHECK(std::strstr(with, "nothing measured yet") != nullptr);
  CHECK(std::strstr(with, "limiter: gr 0.0 dB max 0.0 dB, mode subtle, ceiling -0.3 dBFS\n") != nullptr);
  CHECK(std::fabs(kCeil - db(-0.3)) < 1.0e-7);  // the text's "-0.3" is the constant

  // from a real limiter: the mode follows set_param, the numbers follow the signal
  MasterLimiter& m = fresh();
  CHECK(m.report().mode == 0u);
  CHECK(m.set_param(lm::Mode, 1.0f));
  CHECK(m.report().mode == 1u);
  Stream s(m);
  (void)s.run(kMeterWindowBlocks * 3u, [](std::uint64_t n2) { return both(n2 % 500u == 0u ? 4.0 : 0.1); });
  rep = m.report();
  (void)diag::format_mix(snap, rep, with, sizeof with);
  CHECK_MESSAGE(std::strstr(with, "mode hard, ceiling -0.3 dBFS") != nullptr, with);
  CHECK_MESSAGE(std::strstr(with, "max 12.") != nullptr, with);  // the impulse of 4.0 at the ceiling: 20 * log10(4 / 0.966) = 12.3 dB
}

TEST_CASE("limiter: flush empties the look-ahead and the gain and keeps the controls (the export gives the engine back with nothing of its last block in the limiter)") {
  MasterLimiter& m = fresh();
  CHECK(m.set_param(lm::Switch, 1.0f));
  CHECK(m.set_param(lm::Mode, 1.0f));
  CHECK(m.set_param(lm::Threshold, 80.0f));
  Stream s(m);
  const Probe loud = s.run(40, [&](std::uint64_t n) { return both(db(9.0) * std::sin(2.0 * kPi * 440.0 * static_cast<double>(n) / kRate)); });
  CHECK(loud.peak > 0.5);
  CHECK(m.gain() < 1.0f);  // the loud passage is being held down
  m.flush();
  CHECK(m.gain() == 1.0f);
  // the first block after it is silence, not the last block of the loud passage 63 samples late
  float peak = 0.0f;
  s.blocks(3, [&](std::uint64_t) { return both(0.0); }, [&](std::uint64_t, float a, float b) { peak = std::fmax(peak, std::fmax(std::fabs(a), std::fabs(b))); });
  CHECK(peak == 0.0f);
  // the controls are as they were, and a signal under the ceiling still comes out untouched
  Stream t(m);
  float want = 0.25f;
  std::uint64_t bad = 0;
  t.blocks(4, [&](std::uint64_t) { return Lr{want, want}; }, [&](std::uint64_t n, float a, float) { bad += (n >= MasterLimiter::kLatency && a != want) ? 1u : 0u; });
  CHECK(bad == 0u);
  const Probe again = t.run(40, [&](std::uint64_t n) { return both(db(9.0) * std::sin(2.0 * kPi * 440.0 * static_cast<double>(n) / kRate)); });
  CHECK(again.peak <= kCeil + 1.0e-6);
  CHECK(m.gain() < 1.0f);  // still on, still in the mode and at the threshold it had
}
