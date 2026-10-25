// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/audio/demo_kit.h>

#include <o3os/audio/test_signal.h>
#include <o3os/core/audio_clock.h>
#include <o3os/core/fast_math.h>

namespace o3os::audio {
namespace {

// The engine's sample rate, from the one rate constant.
constexpr float kRate = static_cast<float>(core::kCpuHz) * static_cast<float>(core::kRefCyclesPerSample.den) / static_cast<float>(core::kRefCyclesPerSample.num);
constexpr float kInvRate = 1.0f / kRate;

constexpr float kLevel = 24000.0f;  // the sounds are made near full scale, then normalised to their own peak
constexpr float kBassHz = 130.8128f;  // C3
constexpr unsigned kBassHarmonics = 8;
constexpr std::uint32_t kEndFade = 256;  // the last samples of a one shot fade to zero: no click at its end

// Peak of each sound after normalising, in int16 units: kick 0.40, snare 0.28, hat 0.15, bass 0.20 of full scale. They add up to 0.998 of it: the pan
// law's centre is unity on both channels (smoothing.h), so tracks at their centre add, and the demo must not clip even when all four peak on one sample.
constexpr std::int32_t kPeak[kDemoKitSounds] = {13107, 9175, 4915, 6554};

std::int16_t to_i16(float v) noexcept {
  const float r = v + (v >= 0.0f ? 0.5f : -0.5f);
  return static_cast<std::int16_t>(r > 32767.0f ? 32767.0f : (r < -32768.0f ? -32768.0f : r));
}

// x - floor(x) for x >= 0 (a phase in turns).
float turns(float x) noexcept { return x - static_cast<float>(static_cast<std::uint32_t>(x)); }

// The per-sample factor of an exponential decay with time constant `seconds`.
float decay(float seconds) noexcept { return core::exp2_fast(-1.44269504f * kInvRate / seconds); }

std::uint32_t frames(float seconds) noexcept { return static_cast<std::uint32_t>(seconds * kRate); }

struct Noise {
  std::uint32_t state = 0x2545F491u;
  float next() noexcept {  // xorshift32 as a signed fraction in [-1, 1)
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return static_cast<float>(static_cast<std::int32_t>(state)) * (1.0f / 2147483648.0f);
  }
};

void fade_out(std::int16_t* p, std::uint32_t n, std::uint32_t len) noexcept {
  len = len > n ? n : len;
  for (std::uint32_t k = 0; k < len; ++k) {
    std::int16_t& s = p[n - len + k];
    s = static_cast<std::int16_t>((static_cast<std::int32_t>(s) * static_cast<std::int32_t>(len - k)) / static_cast<std::int32_t>(len));
  }
}

void normalise(std::int16_t* p, std::uint32_t n, std::int32_t peak) noexcept {
  std::int32_t max = 0;
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::int32_t a = p[i] < 0 ? -static_cast<std::int32_t>(p[i]) : static_cast<std::int32_t>(p[i]);
    max = a > max ? a : max;
  }
  if (max == 0) {
    return;
  }
  const float scale = static_cast<float>(peak) / static_cast<float>(max);
  for (std::uint32_t i = 0; i < n; ++i) {
    p[i] = to_i16(static_cast<float>(p[i]) * scale);
  }
}

// A sine whose pitch falls from 180 to 45 Hz with a 35 ms constant, under a 140 ms decay.
std::uint32_t make_kick(std::int16_t* p) noexcept {
  const std::uint32_t n = frames(0.35f);
  const float pitch_decay = decay(0.035f);
  const float amp_decay = decay(0.14f);
  float phase = 0.0f;
  float fall = 1.0f;
  float amp = 1.0f;
  for (std::uint32_t i = 0; i < n; ++i) {
    phase = turns(phase + (45.0f + 135.0f * fall) * kInvRate);
    p[i] = to_i16(sin_turns(phase) * amp * kLevel);
    fall *= pitch_decay;
    amp *= amp_decay;
  }
  fade_out(p, n, kEndFade);
  normalise(p, n, kPeak[kDemoKick]);
  return n;
}

// White noise under a 45 ms decay plus a 180 Hz tone under a 70 ms one.
std::uint32_t make_snare(std::int16_t* p) noexcept {
  const std::uint32_t n = frames(0.22f);
  const float noise_decay = decay(0.045f);
  const float tone_decay = decay(0.07f);
  Noise noise;
  float phase = 0.0f;
  float noise_amp = 1.0f;
  float tone_amp = 1.0f;
  for (std::uint32_t i = 0; i < n; ++i) {
    phase = turns(phase + 180.0f * kInvRate);
    p[i] = to_i16((0.7f * noise.next() * noise_amp + 0.5f * sin_turns(phase) * tone_amp) * kLevel);
    noise_amp *= noise_decay;
    tone_amp *= tone_decay;
  }
  fade_out(p, n, kEndFade);
  normalise(p, n, kPeak[kDemoSnare]);
  return n;
}

// White noise through the first difference (a high-pass of +6 dB an octave), under a 22 ms decay.
std::uint32_t make_hat(std::int16_t* p) noexcept {
  const std::uint32_t n = frames(0.11f);
  const float amp_decay = decay(0.022f);
  Noise noise;
  float prev = 0.0f;
  float amp = 1.0f;
  for (std::uint32_t i = 0; i < n; ++i) {
    const float x = noise.next();
    p[i] = to_i16((x - prev) * 0.5f * amp * kLevel);
    prev = x;
    amp *= amp_decay;
  }
  fade_out(p, n, kEndFade);
  normalise(p, n, kPeak[kDemoHat]);
  return n;
}

// One cycle of a saw of the first 8 harmonics (a sum of sines, 1/h each), which loops: its cycle is a C at the root.
std::uint32_t make_bass(std::int16_t* p) noexcept {
  const std::uint32_t n = static_cast<std::uint32_t>(kRate / kBassHz + 0.5f);
  for (std::uint32_t i = 0; i < n; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(n);
    float sum = 0.0f;
    for (unsigned h = 1; h <= kBassHarmonics; ++h) {
      sum += sin_turns(turns(static_cast<float>(h) * t)) / static_cast<float>(h);
    }
    p[i] = to_i16(sum * kLevel);
  }
  normalise(p, n, kPeak[kDemoBass]);
  return n;
}

}  // namespace

bool generate_demo_kit(std::int16_t* dst, std::uint32_t capacity, DemoKit& kit) noexcept {
  kit = DemoKit{};
  using Maker = std::uint32_t (*)(std::int16_t*) noexcept;
  static constexpr Maker kMakers[kDemoKitSounds] = {&make_kick, &make_snare, &make_hat, &make_bass};
  // Every sound's longest possible length, to refuse before writing a sample: the generators write no more than this.
  static constexpr float kMaxSeconds[kDemoKitSounds] = {0.35f, 0.22f, 0.11f, 1.0f / kBassHz + 0.001f};
  std::uint32_t used = 0;
  for (unsigned i = 0; i < kDemoKitSounds; ++i) {
    const std::uint32_t room = frames(kMaxSeconds[i]) + 2u;
    if (used + room > capacity) {
      kit = DemoKit{};
      return false;
    }
    const std::uint32_t n = kMakers[i](dst + used);
    kit.sample[i].data = dst + used;
    kit.sample[i].length = n;
    if (i == kDemoBass) {
      kit.sample[i].loop_start = 0;
      kit.sample[i].loop_end = n;  // one cycle, looped
    }
    used += (n + 1u) & ~1u;  // the next sound starts on a word
  }
  kit.samples = used;
  return true;
}

}  // namespace o3os::audio
