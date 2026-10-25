// SPDX-License-Identifier: MIT
//
// OS-H2b (new Community OS behaviour): the radix-2 FFT shared by the two apps that read the audio tap (TUNER, SCOPE / SPECTRUM). Written from scratch (the textbook decimation-in-time
// butterfly, nothing copied), single precision, no libc, no static data: the tables and the work arrays are the app's own state.
//
// It runs in UNITS, and a unit in CHUNKS, so that an app can keep to the 4 ms its view_draw may take (O3M_VIEW_MAX_US) even with its arrays in SDRAM: unit 0 is the bit-reversal permutation, units
// 1..log2(n) are the butterfly stages, each of n / 2 butterflies. step() does at most `budget` work items of the current unit and says whether the transform is complete; the caller watches the clock
// between calls and decides how many chunks a frame can afford.
// Forward transform, X[k] = sum x[j] exp(-2 pi i j k / n), unnormalised.
//
// Cost (estimate from the compiled loop, `arm-none-eabi-objdump` of the packed module at -Os: the butterfly loop is 36 instructions, 8 loads, 4 stores, 8 FP operations; the unit's figure is
// owed, QA-MOD-026): about 40 to 50 cycles a butterfly with the data in cache or SRAM. A stage of n = 1024 is 512 butterflies = 20 to 26 kcycles (0.11 to 0.14 ms at 180 MHz), the whole
// 1024-point transform (11 units) about 1.4 ms; a stage of n = 2048 is 1024 butterflies = 41 to 51 kcycles (0.23 to 0.28 ms), the transform (12 units) about 3 ms.
#include <o3module.h>

#include <stdint.h>

namespace o3fft {

constexpr uint32_t kMaxN = 2048;  // the largest transform the apps use

// The twiddle factors for a transform of `n` points (a power of two, 4..kMaxN): c[k] = cos(2 pi k / n), s[k] = sin(2 pi k / n), k < n / 2. One quarter of them comes from the host's cos (the rest
// is symmetry: sin x = cos(pi/2 - x), cos(pi - x) = -cos x), so initialising a table of 2048 costs 513 host calls.
inline void init_twiddle(const o3m_host* host, float* c, float* s, uint32_t n) {
  const uint32_t q = n / 4u;
  const float step = 6.2831853f / static_cast<float>(n);
  for (uint32_t k = 0; k <= q; ++k) {
    const float v = host->cos_f(step * static_cast<float>(k));
    c[k] = v;
    s[q - k] = v;
  }
  for (uint32_t k = q + 1u; k < n / 2u; ++k) {
    c[k] = -c[n / 2u - k];
    s[k] = s[n / 2u - k];
  }
}

struct Job {
  uint32_t n;     // points, a power of two
  uint32_t unit;  // the next unit: 0 = bit reversal, 1..log2(n) = stages; n_units() when complete
  uint32_t at;    // work items of the current unit already done (butterflies of a stage, indices of the bit reversal)
  uint32_t rev;   // the bit-reversed counter of the bit reversal at `at`
};

inline uint32_t log2u(uint32_t n) {
  uint32_t l = 0;
  while ((1u << l) < n) {
    ++l;
  }
  return l;
}

inline void begin(Job* j, uint32_t n) {
  j->n = n;
  j->unit = 0;
  j->at = 0;
  j->rev = 0;
}

inline bool done(const Job* j) { return j->unit > log2u(j->n); }

// Up to `budget` work items of the current unit on (re, im), in place: a unit is n - 1 steps of the bit reversal or n / 2 butterflies, so a caller that watches the clock can stop between
// calls (a stage of 2,048 points is 1,024 butterflies, several milliseconds if the arrays are in SDRAM). Returns true when the whole transform is complete (also if it already was).
inline bool step(Job* job, float* re, float* im, const float* c, const float* s, uint32_t budget) {
  const uint32_t n = job->n;
  if (job->unit > log2u(n)) {
    return true;
  }
  if (job->unit == 0u) {
    uint32_t j = job->rev;
    uint32_t i = job->at + 1u;  // (index 0 maps to itself: the counter starts at 1)
    for (uint32_t left = budget; left != 0u && i < n; --left, ++i) {
      uint32_t bit = n >> 1;
      for (; (j & bit) != 0u; bit >>= 1) {
        j ^= bit;
      }
      j ^= bit;
      if (i < j) {
        const float tr = re[i], ti = im[i];
        re[i] = re[j];
        im[i] = im[j];
        re[j] = tr;
        im[j] = ti;
      }
    }
    job->rev = j;
    job->at = i - 1u;
    if (i >= n) {
      ++job->unit;
      job->at = 0;
    }
  } else {
    const uint32_t half = 1u << (job->unit - 1u);
    const uint32_t len = half * 2u;
    const uint32_t stride = n / len;  // the table is for n points: w = exp(-2 pi i k / len) = table[k * n / len]
    uint32_t block = job->at / half;
    uint32_t k = job->at % half;
    uint32_t todo = n / 2u - job->at;
    todo = todo < budget ? todo : budget;
    job->at += todo;
    for (; todo != 0u; --todo) {
      const float wr = c[k * stride], ws = s[k * stride];  // w = wr - i ws
      const uint32_t a = block * len + k, b = a + half;
      const float tr = wr * re[b] + ws * im[b];
      const float ti = wr * im[b] - ws * re[b];
      re[b] = re[a] - tr;
      im[b] = im[a] - ti;
      re[a] += tr;
      im[a] += ti;
      if (++k == half) {
        k = 0;
        ++block;
      }
    }
    if (job->at >= n / 2u) {
      ++job->unit;
      job->at = 0;
    }
  }
  return job->unit > log2u(n);
}

// The periodic Hann window, w[j] = (1 - cos(2 pi j / n)) / 2, one sample at a time so that a caller can spread a long window over several chunks. The cosine comes from the rotation recurrence
// with the small increments (alpha = 2 sin^2(d/2), beta = sin d: cos(a + d) = cos a - (alpha cos a + beta sin a), sin(a + d) = sin a - (alpha sin a - beta cos a)), which keeps the angle error
// far below a float rounding of cos(d) itself; two host calls for the whole window. The error against the exact window is under 1e-5 for n up to 2048 (the host test checks it).
struct Hann {
  float c, s, alpha, beta;
};

inline void hann_begin(const o3m_host* host, Hann* h, uint32_t n) {
  const float d = 6.2831853f / static_cast<float>(n);
  const float sh = host->sin_f(0.5f * d);
  h->alpha = 2.0f * sh * sh;
  h->beta = host->sin_f(d);
  h->c = 1.0f;
  h->s = 0.0f;
}

// The next sample of the window.
inline float hann_next(Hann* h) {
  const float w = 0.5f * (1.0f - h->c);
  const float nc = h->c - (h->alpha * h->c + h->beta * h->s);
  const float ns = h->s - (h->alpha * h->s - h->beta * h->c);
  h->c = nc;
  h->s = ns;
  return w;
}

// Applies the window to x[0..n).
inline void hann(const o3m_host* host, float* x, uint32_t n) {
  Hann h;
  hann_begin(host, &h, n);
  for (uint32_t j = 0; j < n; ++j) {
    x[j] *= hann_next(&h);
  }
}

}  // namespace o3fft
