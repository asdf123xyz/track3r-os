// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-303PERF (M1): the TB-303's render was restructured for the Cortex-M4F's registers (modules/tb303/tb303_dsp.h voice_render: two loops, the filter chain's state in locals) and
// must not have changed a single bit of what it makes. This is the old sample loop, frozen as it was at rc/0.4.10 (b3203e1), run beside the new one over random scripts: the same notes,
// accents, slides and legato, parameter changes, and block cuts of every size (a cut at every phase of the coefficient grid, in the middle of the idle test's count, and a block of nothing).
// After every call the outputs, the return value and ALL of the voice's state (the whole struct, byte for byte) must be equal. The oracle test (tb303_oracle_test.cpp) holds the voice to the
// original Open303 in double precision; this one holds the restructure to the voice that the oracle tested.
//
// Host only: the comparison is of two compiled shapes of the same float arithmetic, which holds where no fused multiply-add can be formed (x86-64 without FMA; the module itself is built
// -ffp-contract=off). The target's proof is the QEMU suite that runs the packed module (module_builtin_run_test.cpp): it hashes the output and the state of the module the image embeds over scripts
// like these, and compares the hash with a constant taken from the old loop (b3203e1) on the same target.
#include <doctest/doctest.h>

#if !defined(__arm__)

#include "../modules/tb303/tb303_dsp.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace {

using tb303::Voice;

float exp2_libm(float x) { return std::exp2(x); }
float log2_libm(float x) { return std::log2(x); }
float sin_libm(float x) { return std::sin(x); }
float cos_libm(float x) { return std::cos(x); }

// voice_render as it was before OS-303PERF (b3203e1), copied: one loop, a test of the grid in every sample, hp1 / fbhp / de-clicker / decimator read and written through the voice.
std::uint32_t voice_render_v0(Voice& v, float* __restrict out, std::uint32_t n) {
  using namespace tb303;
  if (v.idle) {
    for (std::uint32_t i = 0; i < n; ++i) {
      out[i] = 0.0f;
    }
    return 0;
  }
  const float blend = v.p_wave;
  const float amp_scaler = v.amp_scaler;
  const float inv_fs2 = v.inv_fs2;
  const float slide_d = v.slide_d;
  const float cutoff = v.p_cutoff;
  const float freq_target = v.freq_target;
  const bool gate = v.gate != 0u;
  float phase = v.phase;
  float freq = v.freq;
  std::uint32_t ctl = v.ctl;
  Env env = v.env;
  Ladder lad = v.lad;
  std::uint32_t quiet = v.quiet;
  std::uint32_t done = n;
  bool went_idle = false;

  for (std::uint32_t i = 0; i < n; ++i) {
    freq += slide_d * (freq_target - freq);
    const float f = freq < 20000.0f ? (freq > 0.0f ? freq : 0.0f) : 20000.0f;
    const float dt = f * inv_fs2;

    float expo, amp_out;
    env_step(v, env, gate, expo, amp_out);
    if (ctl == 0) {
      ctl = kCtl;
      detail::filter_coefficients(v, cutoff * v.sv.exp2(expo));
      lad.b0 = v.lad.b0;
      lad.k = v.lad.k;
      lad.g2 = v.lad.g2;
    }
    --ctl;
    amp_out = detail::run_svf<false>(v.declick, amp_out);

    float x = 0.0f;
    for (int pass = 0; pass < 2; ++pass) {
      float s = -detail::oscillator(phase, dt, blend);
      phase += dt;
      phase -= phase >= 1.0f ? 1.0f : 0.0f;
      s = detail::run(v.hp1, s);
      x = detail::run(v.dec[1], detail::run(v.dec[0], ladder_step(lad, v.fbhp, s)));
    }

    x = detail::run(v.ap, x);
    x = detail::run(v.hp2, x);
    x = detail::run_svf<true>(v.notch, x);
    x *= amp_out * amp_scaler;
    out[i] = x;
    if (detail::absf(amp_out) < 1e-6f && env.trig == 0u) {
      if (++quiet >= kQuietSamples) {
        done = i + 1u;
        went_idle = true;
        break;
      }
    } else {
      quiet = 0;
    }
  }

  v.phase = phase;
  v.freq = freq;
  v.ctl = ctl;
  v.env = env;
  v.lad.y1 = lad.y1;
  v.lad.y2 = lad.y2;
  v.lad.y3 = lad.y3;
  v.lad.y4 = lad.y4;
  v.quiet = quiet;
  if (went_idle) {
    for (std::uint32_t i = done; i < n; ++i) {
      out[i] = 0.0f;
    }
    v.idle = 1;
    return 0;
  }
  return 1;
}

struct Rng {
  std::uint64_t s;
  std::uint32_t next() {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<std::uint32_t>(s >> 33);
  }
  float range(float lo, float hi) { return lo + (hi - lo) * (static_cast<float>(next()) / 2147483648.0f); }
};

struct Pair {
  std::unique_ptr<Voice> a, b;  // a: the old loop, b: the new one
  explicit Pair(float fs) : a(new Voice), b(new Voice) {
    const tb303::Services sv = {exp2_libm, log2_libm, sin_libm, cos_libm};
    std::memset(a.get(), 0, sizeof(Voice));
    std::memset(b.get(), 0, sizeof(Voice));
    tb303::voice_init(*a, sv, fs);
    tb303::voice_init(*b, sv, fs);
  }
  void param(std::uint32_t id, float v) {
    tb303::voice_set_param(*a, id, v);
    tb303::voice_set_param(*b, id, v);
  }
  void note_on(std::uint32_t note, std::uint32_t vel, bool legato) {
    tb303::voice_note_on(*a, note, vel, legato);
    tb303::voice_note_on(*b, note, vel, legato);
  }
  void note_off() {
    tb303::voice_note_off(*a);
    tb303::voice_note_off(*b);
  }
};

}  // namespace

TEST_CASE("tb303 refactor: the two-level render is the old sample loop bit for bit, outputs and every byte of the state, over random scripts and block cuts") {
  std::vector<float> out_a(4096), out_b(4096);
  std::uint64_t samples = 0, idle_returns = 0, calls = 0, mismatches = 0, accents = 0;
  for (int seed = 1; seed <= 40; ++seed) {
    Rng r{static_cast<std::uint64_t>(seed) * 2654435761ull + 7ull};
    Pair p((seed & 1) ? 44100.0f : 44117.647f);
    const float waves[] = {0.0f, 1.0f, 0.5f, 0.25f};
    p.param(tb303::kWaveform, (r.next() & 3u) < 3u ? waves[r.next() & 3u] : r.range(0.0f, 1.0f));
    p.param(tb303::kCutoff, r.range(314.0f, 2394.0f));
    p.param(tb303::kResonance, r.range(0.0f, 100.0f));
    p.param(tb303::kEnvMod, r.range(0.0f, 100.0f));
    p.param(tb303::kDecay, r.range(200.0f, 2000.0f));
    p.param(tb303::kAccent, r.range(0.0f, 100.0f));
    p.param(tb303::kSlide, r.range(0.0f, 300.0f));
    p.param(tb303::kVolume, r.range(-24.0f, 6.0f));
    p.param(tb303::kLegato, (r.next() & 1u) ? 1.0f : 0.0f);
    bool held = false;
    const int total = 40000;
    int pos = 0;
    while (pos < total) {
      const std::uint32_t ev = r.next() % 100u;
      if (ev < 12u) {
        const std::uint32_t note = 24u + r.next() % 48u;
        const std::uint32_t vel = (r.next() & 1u) ? 100u + r.next() % 28u : 1u + r.next() % 99u;
        accents += vel >= 100u ? 1u : 0u;
        p.note_on(note, vel, held && (r.next() & 1u));
        held = true;
      } else if (ev < 20u) {
        p.note_off();
        held = false;
      } else if (ev < 26u) {
        const std::uint32_t id = r.next() % 9u;
        const float lo[] = {0, 314, 0, 0, 200, 0, 0, -24, 0}, hi[] = {1, 2394, 100, 100, 2000, 100, 300, 6, 1};
        p.param(id, r.range(lo[id], hi[id]));
      }
      const std::uint32_t cuts[] = {1, 2, 3, 4, 5, 7, 16, 33, 63, 64, 65, 127, 128, 129, 0, 4, 8, 1000};
      std::uint32_t n = cuts[r.next() % 18u];
      if (r.next() % 5u == 0u) {
        n = 64;
      }
      if (r.next() % 40u == 0u) {
        n = 3000;  // a long call lets a released voice go idle in the middle of it
      }
      for (std::uint32_t i = 0; i < n; ++i) {
        out_a[i] = 777.0f;  // (a sample the render does not write would show)
        out_b[i] = 777.0f;
      }
      const std::uint32_t ra = voice_render_v0(*p.a, out_a.data(), n);
      const std::uint32_t rb = tb303::voice_render(*p.b, out_b.data(), n);
      ++calls;
      samples += n;
      idle_returns += ra == 0u ? 1u : 0u;
      if (ra != rb || std::memcmp(out_a.data(), out_b.data(), n * sizeof(float)) != 0 || std::memcmp(p.a.get(), p.b.get(), sizeof(Voice)) != 0) {
        ++mismatches;
        if (mismatches <= 3) {
          MESSAGE("seed " << seed << " position " << pos << " n " << n << ": return " << ra << " / " << rb);
        }
      }
      pos += n != 0u ? static_cast<int>(n) : 1;
    }
  }
  CHECK(mismatches == 0u);
  CHECK(calls > 3000u);
  CHECK(samples > 1000000u);
  CHECK(idle_returns > 20u);  // the idle test ended voices at all sorts of phases of the grid and of the blocks
  CHECK(accents > 100u);
}

TEST_CASE("tb303 refactor: a voice that goes idle with every phase of the grid under it leaves the same state and silence behind") {
  std::vector<float> out_a(256), out_b(256);
  unsigned idle_at_phase[4] = {0, 0, 0, 0};  // ctl is the samples to the next coefficient update: where in the grid the idle test ended the voice
  unsigned idled = 0;
  for (std::uint32_t off_block = 1; off_block <= 8; ++off_block) {  // the note ends at a different sample: the idle point moves with it, and lands on every phase of the grid
    for (std::uint32_t decay = 200; decay <= 2000; decay += 450) {
      for (std::uint32_t note = 24; note < 72; note += 7) {
        Pair p(44100.0f);
        p.param(tb303::kDecay, static_cast<float>(decay));
        p.note_on(note, note & 1u ? 120u : 64u, false);
        for (int block = 0; block < 4000; ++block) {
          if (static_cast<std::uint32_t>(block) == off_block) {
            p.note_off();
          }
          const std::uint32_t n = 1u + static_cast<std::uint32_t>(block) % 61u;  // blocks of every size, so the cut moves as well
          const std::uint32_t ra = voice_render_v0(*p.a, out_a.data(), n);
          const std::uint32_t rb = tb303::voice_render(*p.b, out_b.data(), n);
          CHECK(ra == rb);
          CHECK(std::memcmp(out_a.data(), out_b.data(), n * sizeof(float)) == 0);
          CHECK(std::memcmp(p.a.get(), p.b.get(), sizeof(Voice)) == 0);
          if (ra == 0u) {
            ++idle_at_phase[p.b->ctl % 4u];
            ++idled;
            break;
          }
        }
      }
    }
  }
  CHECK(idled > 100u);
  for (unsigned phase = 0; phase < 4; ++phase) {
    CHECK(idle_at_phase[phase] > 0u);  // every phase of the grid was under the idle point at least once
  }
}

#endif  // !__arm__
