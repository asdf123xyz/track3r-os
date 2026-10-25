// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-SENDFX on the Cortex-M4F (`qemu_audio_send_fx`; it runs on the host as well): a compact pass over the two send effects with small static memory (the QEMU machine has 4 MiB), so that
// what only the target does is exercised: the float to int16 write is the instruction pair VCVT + SSAT there (audio/send_fx_util.h), the multiplies may be fused, and the word copies of the
// reverb's staging run on a real core. The full property suite, with the 4 s ring, is audio_send_fx_test.cpp on the host.
#include <doctest/doctest.h>

#include <o3os/audio/send_delay.h>
#include <o3os/audio/send_fx_util.h>
#include <o3os/audio/send_reverb.h>
#include <o3os/hal/audio.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace {

using o3os::audio::SendDelay;
using o3os::audio::SendReverb;
namespace dl = o3os::audio::sendfx::delay;
namespace rv = o3os::audio::sendfx::reverb;

constexpr std::uint32_t kBlk = o3os::hal::audio::kBlock;
constexpr std::uint32_t kFrames = 4096;

std::uint32_t g_ring[kFrames];
std::int16_t g_mem[SendReverb::kMemSamples];
SendDelay g_delay;
SendReverb g_reverb;

struct Lcg {
  std::uint32_t s = 99991u;
  std::int16_t next() {
    s = s * 1664525u + 1013904223u;
    return static_cast<std::int16_t>(s >> 12);
  }
};

}  // namespace

TEST_CASE("send fx (target): the float to int16 write saturates and cuts towards zero") {
  using o3os::audio::sendfx_util::trunc16;
  CHECK(trunc16(0.0f) == 0);
  CHECK(trunc16(1.9f) == 1);
  CHECK(trunc16(-1.9f) == -1);
  CHECK(trunc16(0.99f) == 0);
  CHECK(trunc16(32766.5f) == 32766);
  CHECK(trunc16(40000.0f) == 32767);
  CHECK(trunc16(-40000.0f) == -32768);
  CHECK(trunc16(1.0e20f) == 32767);
  CHECK(trunc16(-1.0e20f) == -32768);
  CHECK(trunc16(std::numeric_limits<float>::infinity()) == 32767);
  CHECK(trunc16(-std::numeric_limits<float>::infinity()) == -32768);
}

TEST_CASE("send fx (target): the delay's time is exact, a loop at the top feedback saturates, and a tail ends in silence") {
  g_delay.attach(g_ring, kFrames);
  CHECK(g_delay.set_param(dl::Time, 10.0f));
  CHECK(g_delay.set_param(dl::Feedback, 0.0f));
  std::int16_t in[kBlk] = {};
  std::int16_t l[kBlk], r[kBlk];
  const std::uint32_t d = 441;  // round(10 ms x 44,117.647 Hz)
  std::uint32_t frame = 0;
  std::int32_t other = 0;
  for (unsigned b = 0; b < 12; ++b) {
    for (std::uint32_t i = 0; i < kBlk; ++i) in[i] = (frame + i == 3u) ? std::int16_t{20000} : std::int16_t{0};
    g_delay.process(in, l, r, kBlk);
    for (std::uint32_t i = 0; i < kBlk; ++i) {
      const std::uint32_t f = frame + i;
      if (f == 3u + d) {
        CHECK(l[i] == 20000);
        CHECK(r[i] == 20000);
      } else {
        other += std::abs(l[i]) + std::abs(r[i]);
      }
    }
    frame += kBlk;
  }
  CHECK(other == 0);
  CHECK(g_delay.delay_left() == d);

  // a Nyquist square wave at 100 % feedback and an even delay builds up until it clips
  g_delay.attach(g_ring, kFrames);
  CHECK(g_delay.set_param(dl::Time, 4.0f));  // 176 frames
  CHECK(g_delay.set_param(dl::Feedback, 100.0f));
  std::uint32_t clipped = 0, checked = 0;
  for (unsigned b = 0; b < 700; ++b) {
    for (std::uint32_t i = 0; i < kBlk; ++i) in[i] = (i & 1u) == 0u ? std::int16_t{32767} : std::int16_t{-32767};
    g_delay.process(in, l, r, kBlk);
    if (b >= 650) {
      for (std::uint32_t i = 0; i < kBlk; ++i) {
        ++checked;
        clipped += (std::abs(l[i]) >= 32767 && ((l[i] > 0) == ((i & 1u) == 0u))) ? 1u : 0u;
      }
    }
  }
  CHECK(clipped == checked);
  CHECK(g_delay.finite());
  // the input stops: the loop's gain is 0.98 a pass, the tail dies to exact zero and the effect sleeps
  std::int16_t zero[kBlk] = {};
  for (unsigned b = 0; b < 1500 && !g_delay.idle(); ++b) {
    g_delay.process(zero, l, r, kBlk);
  }
  CHECK(g_delay.idle());
  for (std::uint32_t i = 0; i < kBlk; ++i) {
    CHECK(l[i] == 0);
    CHECK(r[i] == 0);
  }
}

TEST_CASE("send fx (target): the reverb at both rates is finite, decays and ends in silence, and a quality change clears it") {
  for (unsigned q = 0; q < 2; ++q) {
    g_reverb.attach(g_mem, SendReverb::kMemSamples);
    CHECK(g_reverb.set_param(rv::Size, 0.0f));
    CHECK(g_reverb.set_param(rv::Quality, static_cast<float>(q)));
    Lcg rng;
    std::int16_t in[kBlk], l[kBlk], r[kBlk];
    double early = 0, late = 0;
    for (unsigned b = 0; b < 700; ++b) {  // 1 s of noise, then silence
      for (std::uint32_t i = 0; i < kBlk; ++i) in[i] = b < 345u ? static_cast<std::int16_t>(rng.next() / 4) : std::int16_t{0};
      g_reverb.process(in, l, r, kBlk);
      for (std::uint32_t i = 0; i < kBlk; ++i) {
        const double e = static_cast<double>(l[i]) * l[i] + static_cast<double>(r[i]) * r[i];
        if (b >= 300 && b < 345) early += e;
        if (b >= 600 && b < 645) late += e;
      }
    }
    INFO("quality " << q);
    CHECK(g_reverb.finite());
    CHECK(early > 1.0e6);
    CHECK(late < early);
    std::uint32_t blocks = 0;
    while (!g_reverb.idle() && blocks++ < 4000u) {  // decay 0.25: well under a few seconds
      g_reverb.process(in, l, r, kBlk);
    }
    CHECK(g_reverb.idle());
    CHECK(g_reverb.half_rate() == (q == 0u));
  }
  CHECK(g_reverb.set_param(rv::Quality, 0.0f));  // back to HALF: the lines are repartitioned, the old tail is gone
  std::int16_t in[kBlk] = {}, l[kBlk], r[kBlk];
  in[0] = 20000;
  g_reverb.process(in, l, r, kBlk);
  CHECK(g_reverb.half_rate());
  CHECK(g_reverb.finite());
}
