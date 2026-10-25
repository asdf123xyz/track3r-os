// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Paula model (audio/paula.h, OS-005h) against pt2-clone's own Paula emulator (third_party/pt2-clone, BSD-3-Clause: the original's three files, built for this
// test only): the same register writes, the same output rate, the same samples out. The model keeps the original's arithmetic in the original's order, so the
// raw channel is expected to be BIT-EXACT (tolerance asserted: 0), the filters within one float rounding (1e-6: the coefficients are folded at compile time with
// the compiler's own exp and tan, the original's are computed at run time). Host only (the oracle is not built for the target).
#include <doctest/doctest.h>

#include <o3os/audio/paula.h>
#include <o3os/core/audio_clock.h>
#include <o3os/hal/audio.h>

extern "C" {
#include "pt2_paula.h"
}

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace orig {
#include O3_PT2_PERIOD_TABLE
}

namespace {

using namespace o3os::audio::paula;

const double kOutputRate = static_cast<double>(o3os::hal::audio::sample_rate_mhz()) / 1000.0;

constexpr std::uint32_t kAud0Dat = 0xDFF0A0, kAud0Len = 0xDFF0A4, kAud0Per = 0xDFF0A6, kAud0Vol = 0xDFF0A8, kDmacon = 0xDFF096;

// A repeatable 8-bit test signal: a saw with a little noise, so the steps are of every size.
std::vector<std::int8_t> make_wave(std::size_t n, std::uint32_t seed) {
  std::vector<std::int8_t> w(n);
  std::uint32_t r = seed;
  for (std::size_t i = 0; i < n; ++i) {
    r = r * 1664525u + 1013904223u;
    const int saw = static_cast<int>((i * 7u) % 200u) - 100;
    w[i] = static_cast<std::int8_t>(saw + static_cast<int>((r >> 24) % 21u) - 10);
  }
  return w;
}

// One channel on both sides. `words` is the intro length in words, the loop the same bytes unless `loop_words` is set (>= 1: the loop is that many words from `loop_at`).
struct Pair {
  std::vector<std::int8_t> bytes;
  std::vector<std::int16_t> wide;  // the same samples as the model reads them: the 8 bits on top, noise below (the model takes the top byte)
  Channel ch;
  float oracle_buf[2048];
  float mine_buf[2048];

  explicit Pair(std::size_t n, std::uint32_t seed, Model model = Model::A500, bool filters = false) : bytes(make_wave(n, seed)) {
    wide.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
      wide[i] = static_cast<std::int16_t>(bytes[i] * 256 + static_cast<int>((i * 37u) & 0xFFu));
    }
    paulaWriteByte(0xBFE001, 0);  // the LED switch is a static too
    paulaSetup(kOutputRate, model == Model::A1200 ? MODEL_A1200 : MODEL_A500);
    if (!filters) {
      paulaDisableFilters();
    }
    // The original keeps its voices in statics that paulaSetup() does not reset (phase, period, the BLEP's last phase): what a test left behind would reach the next
    // one's first sample. Both sides run the same short silent note first, which leaves each in a state that depends on nothing before it.
    const std::vector<std::int16_t> zero(8, 0);
    paulaWritePtr(kAud0Dat, nullptr);
    paulaWriteWord(kAud0Len, 4);
    paulaWriteWord(kAud0Per, 113);
    paulaWriteWord(kAud0Vol, 0);
    paulaWriteWord(kDmacon, 0x8001);
    ch.set_period(113);
    ch.set_volume(0);
    ch.start(zero.data(), 0, 8, 0, 8);
    float a[64], b[64];
    paulaGenerateSamples(a, b, 64);
    ch.run(mine_buf, 64, kMinBlep);
    paulaWriteWord(kDmacon, 0x0001);
    ch.stop();
    paulaSetup(kOutputRate, model == Model::A1200 ? MODEL_A1200 : MODEL_A500);  // the filters' and the BLEP's state clean again
    if (!filters) {
      paulaDisableFilters();
    }
  }

  // DMA on: `words` of intro, then a loop of `loop_words` words from byte `loop_at` (0: the whole intro again; loop_words 1 at null: a one shot).
  void start(std::uint32_t period, std::uint32_t vol, std::uint32_t words, std::uint32_t loop_at, std::uint32_t loop_words, bool one_shot) {
    paulaWritePtr(kAud0Dat, bytes.data());
    paulaWriteWord(kAud0Len, static_cast<std::uint16_t>(words));
    paulaWriteWord(kAud0Per, static_cast<std::uint16_t>(period));
    paulaWriteWord(kAud0Vol, static_cast<std::uint16_t>(vol));
    paulaWriteWord(kDmacon, 0x8001);
    if (one_shot) {
      paulaWritePtr(kAud0Dat, nullptr);  // the null sample: zeros
      paulaWriteWord(kAud0Len, 1);
    } else {
      paulaWritePtr(kAud0Dat, bytes.data() + loop_at);
      paulaWriteWord(kAud0Len, static_cast<std::uint16_t>(loop_words));
    }
    ch.set_period(period);
    ch.set_volume(vol);
    ch.start(wide.data(), 0, words * 2u, one_shot ? 0u : loop_at, one_shot ? 0u : loop_at + loop_words * 2u);
  }

  // `n` samples of both; the largest difference (in float) is returned. The oracle writes its left buffer; the right is scratch.
  float run(std::uint32_t n, std::uint32_t chunk = 64) {
    float worst = 0.0f;
    float scratch[2048];
    std::uint32_t done = 0;
    while (done < n) {
      const std::uint32_t m = std::min(chunk, n - done);
      paulaGenerateSamples(oracle_buf, scratch, static_cast<int32_t>(m));
      ch.run(mine_buf, m, kMinBlep);
      for (std::uint32_t i = 0; i < m; ++i) {
        worst = std::max(worst, std::fabs(oracle_buf[i] - mine_buf[i]));
      }
      done += m;
    }
    return worst;
  }

  float run_filtered(std::uint32_t n, Filter& f) {
    float worst = 0.0f;
    float scratch[2048];
    std::uint32_t done = 0;
    while (done < n) {
      const std::uint32_t m = std::min<std::uint32_t>(64u, n - done);
      paulaGenerateSamples(oracle_buf, scratch, static_cast<int32_t>(m));
      ch.run(mine_buf, m, kMinBlep);
      f.process(mine_buf, m);
      for (std::uint32_t i = 0; i < m; ++i) {
        worst = std::max(worst, std::fabs(oracle_buf[i] - mine_buf[i]));
      }
      done += m;
    }
    return worst;
  }
};

}  // namespace

TEST_CASE("paula: the output rate is above the model's 31,389 Hz floor and the step constant is the clock over it") {
  CHECK(kOutputRate > 31389.0);
  CHECK(kOutputRate == doctest::Approx(static_cast<double>(o3os::core::kRefRateHz)).epsilon(1e-6));  // 44,100 Hz by default, 44,117.647 Hz with -DO3OS_RATE_44100=OFF
  CHECK(kPeriodToDeltaDiv == doctest::Approx(static_cast<double>(kPalClockHz) / kOutputRate).epsilon(1e-6));  // 80.4244 at 44,100 Hz, 80.3963 at 44,117.647 Hz
}

TEST_CASE("paula oracle: a looped sample at the usual periods is bit exact") {
  const std::uint32_t periods[] = {856, 428, 214, 113, 124, 300, 1000, 5000};
  for (std::uint32_t period : periods) {
    Pair p(2048, 11u + period);
    p.start(period, 64, 600, 0, 600, false);
    CHECK(p.run(2048) == 0.0f);
  }
}

TEST_CASE("paula oracle: a loop that is not the whole sample, and a one shot") {
  {
    Pair p(1024, 3);
    p.start(300, 64, 200, 128, 100, false);  // 200 words, then 100 words from byte 128
    CHECK(p.run(4000) == 0.0f);
  }
  {
    Pair p(1024, 5);
    p.start(250, 48, 100, 0, 0, true);
    CHECK(p.run(2000) == 0.0f);
  }
}

TEST_CASE("paula oracle: period, volume and restarts between blocks, in blocks of any size") {
  const std::uint32_t chunks[] = {64, 1, 7, 128, 33};
  for (std::uint32_t chunk : chunks) {
    Pair p(2048, 21);
    p.start(428, 64, 500, 0, 500, false);
    float worst = p.run(500, chunk);
    // a new period lands at the next sample fetch, a new volume at the next sample
    paulaWriteWord(kAud0Per, 380);
    p.ch.set_period(380);
    paulaWriteWord(kAud0Vol, 17);
    p.ch.set_volume(17);
    worst = std::max(worst, p.run(500, chunk));
    paulaWriteWord(kAud0Per, 0);  // 0 is 65536
    p.ch.set_period(0);
    worst = std::max(worst, p.run(300, chunk));
    paulaWriteWord(kAud0Per, 50);  // under the minimum: 113
    p.ch.set_period(50);
    paulaWriteWord(kAud0Vol, 200);  // above 64: 64
    p.ch.set_volume(200);
    worst = std::max(worst, p.run(500, chunk));
    // the note restarts (DMA off and on again), the held sample and the ring carry over
    p.start(214, 40, 500, 0, 500, false);
    worst = std::max(worst, p.run(700, chunk));
    CHECK(worst == 0.0f);
  }
}

TEST_CASE("paula oracle: a fuzz of register writes at random times stays bit exact") {
  std::uint32_t r = 12345u;
  auto next = [&r]() { r = r * 1664525u + 1013904223u; return r >> 8; };
  Pair p(4096, 99);
  p.start(428, 64, 800, 0, 800, false);
  float worst = 0.0f;
  for (int step = 0; step < 400; ++step) {
    worst = std::max(worst, p.run(1 + next() % 90u, 64));
    switch (next() % 4u) {
      case 0: {
        const std::uint32_t per = 113u + next() % 900u;
        paulaWriteWord(kAud0Per, static_cast<std::uint16_t>(per));
        p.ch.set_period(per);
        break;
      }
      case 1: {
        const std::uint32_t vol = next() % 65u;
        paulaWriteWord(kAud0Vol, static_cast<std::uint16_t>(vol));
        p.ch.set_volume(vol);
        break;
      }
      case 2:
        p.start(113u + next() % 700u, next() % 65u, 100u + next() % 600u, 0, 100u + next() % 100u, false);
        break;
      default:
        break;
    }
  }
  CHECK(worst == 0.0f);
}

TEST_CASE("paula oracle: run_q15, the render's fused pass, equals the three passes (run, process, to_q15) for every model and LED setting, and with no filter it is the oracle rounded") {
  for (Model model : {Model::Off, Model::A500, Model::A1200}) {
    for (bool led : {false, true}) {
      Pair p(2048, 77, Model::A500, false);  // the oracle without its filters
      Filter f3, f1;
      f3.set(model, led);
      f1.set(model, led);
      p.start(300, 64, 700, 0, 700, false);
      Channel fused = p.ch;  // the same state
      float scratch[64];
      std::int16_t q[64];
      unsigned bad_ref = 0, bad_oracle = 0;
      for (unsigned blk = 0; blk < 120; ++blk) {
        if (blk == 40 || blk == 80) {  // a new period and volume land at the next fetch, in both
          const std::uint32_t per = blk == 40 ? 150u : 113u;
          paulaWriteWord(kAud0Per, static_cast<std::uint16_t>(per));
          paulaWriteWord(kAud0Vol, blk == 40 ? 33 : 64);
          p.ch.set_period(per);
          fused.set_period(per);
          p.ch.set_volume(blk == 40 ? 33 : 64);
          fused.set_volume(blk == 40 ? 33 : 64);
        }
        paulaGenerateSamples(p.oracle_buf, scratch, 64);
        p.ch.run(p.mine_buf, 64, kMinBlep);
        f3.process(p.mine_buf, 64);
        fused.run_q15(q, 64, kMinBlep, f1);
        for (unsigned i = 0; i < 64; ++i) {
          bad_ref += q[i] != Channel::to_q15(p.mine_buf[i]) ? 1u : 0u;
          bad_oracle += (model == Model::Off && !led && q[i] != Channel::to_q15(p.oracle_buf[i])) ? 1u : 0u;
        }
      }
      CHECK(bad_ref == 0u);
      CHECK(bad_oracle == 0u);
    }
  }
}

TEST_CASE("paula oracle: the A500 and A1200 filters and the LED filter, within one float rounding") {
  for (Model model : {Model::A500, Model::A1200}) {
    for (bool led : {false, true}) {
      Pair p(2048, 31, model, true);
      Filter f;
      f.set(model, led);
      if (led) {
        paulaWriteByte(0xBFE001, 2);  // CIA-A bit 1: the LED filter on
      }
      p.start(300, 64, 700, 0, 700, false);
      CHECK(p.run_filtered(6000, f) < 1e-6f);
    }
  }
}

TEST_CASE("paula oracle: the LED filter switched mid-stream clears its state, as the original's does") {
  Pair p(2048, 41, Model::A500, true);
  Filter f;
  f.set(Model::A500, false);
  p.start(280, 64, 600, 0, 600, false);
  float worst = p.run_filtered(1500, f);
  paulaWriteByte(0xBFE001, 2);
  f.set(Model::A500, true);
  worst = std::max(worst, p.run_filtered(1500, f));
  paulaWriteByte(0xBFE001, 0);
  f.set(Model::A500, false);
  worst = std::max(worst, p.run_filtered(1500, f));
  CHECK(worst < 1e-6f);
}

TEST_CASE("paula: the filters' cut-offs, by their response at the corners") {
  // A one-pole RC at its cut-off is -3.01 dB (0.7071); the LED filter (Q 0.660) is 0.660 at its own. Measured with sines through the model's filter.
  auto gain_at = [](Model m, bool led, double hz) {
    Filter f;
    f.set(m, led);
    constexpr std::uint32_t kN = 8192;
    std::vector<float> x(kN);
    for (std::uint32_t i = 0; i < kN; ++i) {
      x[i] = static_cast<float>(std::sin(2.0 * 3.14159265358979 * hz * i / kOutputRate));
    }
    f.process(x.data(), kN);
    double peak = 0.0;
    for (std::uint32_t i = kN / 2; i < kN; ++i) {
      peak = std::max(peak, static_cast<double>(std::fabs(x[i])));
    }
    return peak;
  };
  CHECK(gain_at(Model::A500, false, 1000.0) > 0.97);                       // well inside the pass band (the high-pass is at 5 Hz)
  CHECK(gain_at(Model::A500, false, 4421.0) < 0.74);                       // near the -3 dB point of the A500 low-pass (the sampled bilinear-free one-pole is a little off)
  CHECK(gain_at(Model::A500, false, 4421.0) > 0.68);
  CHECK(gain_at(Model::A1200, false, 15000.0) > 0.9);                      // the A1200 has no audible low-pass
  CHECK(gain_at(Model::A500, true, 10000.0) < gain_at(Model::A500, false, 10000.0));  // the LED filter takes more off the top
}

TEST_CASE("paula: fade_out ends the channel without a step and the ring runs dry") {
  Pair p(1024, 7);
  p.start(300, 64, 400, 0, 400, false);
  float buf[64];
  for (int i = 0; i < 10; ++i) {
    p.ch.run(buf, 64, kMinBlep);
  }
  p.ch.fade_out();
  float biggest_step = 0.0f;
  float prev = buf[63];
  const float held = std::fabs(prev);
  CHECK(held > 0.2f);  // the test signal was not near zero when it stopped (a hard cut would be a step of `held`)
  for (int i = 0; i < 4; ++i) {
    p.ch.run(buf, 64, kMinBlep);
    for (int k = 0; k < 64; ++k) {
      biggest_step = std::max(biggest_step, std::fabs(buf[k] - prev));
      prev = buf[k];
    }
  }
  CHECK(!p.ch.active());
  CHECK(prev == 0.0f);
  CHECK(biggest_step < 0.95f * held);
}

TEST_CASE("paula: the period table is the original's, and note_period follows it") {
  for (unsigned ft = 0; ft < 16; ++ft) {
    for (unsigned n = 0; n < 36; ++n) {
      CHECK(kPeriodTable[ft][n] == orig::periodTable[ft * 37 + n]);
    }
  }
  CHECK(note_period(60, 0) == 428);   // the unity note
  CHECK(note_period(48, 0) == 856);
  CHECK(note_period(83, 0) == 113);
  CHECK(note_period(72, 7) == 214 - 10);  // finetune +7 row, C-3 is 204
  CHECK(note_period(72, -8) == 226);
  CHECK(note_period(36, 0) == 1712);   // an octave below the table: doubled
  CHECK(note_period(95, 0) == 113);    // above it: held to the hardware minimum
  CHECK(note_period(0, 0) == 13696);   // four octaves below the table (the last the note range has)
  CHECK(note_period(127, 0) == 113);
}

TEST_CASE("paula: the float constants are the floats the original computes in double (bit for bit)") {
  const double pi = 3.14159265358979323846264338327950288;
  const double rate = kOutputRate;
  auto one_pole = [&](double r, double c, float& a0, float& b1) {
    double cut = 1.0 / ((2.0 * pi) * r * c);
    if (cut >= rate / 2.0) {
      cut = (rate / 2.0) - 1e-4;
    }
    const double b = std::exp((-2.0 * pi) * cut / rate);
    a0 = static_cast<float>(1.0 - b);
    b1 = static_cast<float>(b);
  };
  float a0 = 0, b1 = 0;
  one_pole(360.0, 1e-7, a0, b1);
  CHECK(a0 == coef::kA500Lo.a0);
  CHECK(b1 == coef::kA500Lo.b1);
  one_pole(1390.0, 2.233e-5, a0, b1);
  CHECK(a0 == coef::kA500Hi.a0);
  CHECK(b1 == coef::kA500Hi.b1);
  one_pole(1360.0, 2.2e-5, a0, b1);
  CHECK(a0 == coef::kA1200Hi.a0);
  CHECK(b1 == coef::kA1200Hi.b1);
  const double root = std::sqrt(10000.0 * 10000.0 * 6.8e-9 * 3.9e-9);
  const double cut = 1.0 / ((2.0 * pi) * root);
  const double q = root / (3.9e-9 * (10000.0 + 10000.0));
  const double a = 1.0 / std::tan((pi * cut) / rate);
  const double r = 1.0 / q;
  const double c1 = 1.0 / (1.0 + r * a + a * a);
  CHECK(static_cast<float>(c1) == coef::kLed.a1);
  CHECK(static_cast<float>(2.0 * c1) == coef::kLed.a2);
  CHECK(static_cast<float>(2.0 * (1.0 - a * a) * c1) == coef::kLed.b1);
  CHECK(static_cast<float>((1.0 - r * a + a * a) * c1) == coef::kLed.b2);
  CHECK(kPeriodToDeltaDiv == static_cast<float>(static_cast<double>(kPalClockHz) / kOutputRate));
}
