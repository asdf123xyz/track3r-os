// SPDX-License-Identifier: MIT
//
// "Tuner" (OS-H2b, new Community OS behaviour, ABI 1.4 until OS-APPUX S3, now 1.10, capability audio_tap): a chromatic tuner for the line input (default) or the master output. It reads the OS's audio tap with the gapless
// tap_stream, decimates it by 4 to 11,025 Hz with its own low-pass, keeps a window of 2,048 decimated frames and finds the pitch with a normalised autocorrelation. It makes no sound and does no
// audio work: everything here runs in the 33 ms app frame (view_draw), never in the render.
//
//   shows     the note with its octave, the cents off the nearest note (-50..+50) as a needle, the frequency in Hz with one decimal, and the confidence (the height of the correlation peak);
//             NO SIGNAL when the level is below -50 dBFS or no clear pitch is found.   Up / Down, the encoder, Shift + the encoder, Delete (the grammar of o3m_edit.h, OS-APPUX S3): the two rows at the top right, kept with the
//             project: A4 reference 415..466 Hz (440), SOURCE 1 = INPUT, 2 = MASTER.
//   range     30 Hz to 2.1 kHz (a bass B0 is 30.9 Hz; the longest lag searched is 367 frames of 11,025 Hz = 30.0 Hz and a period at its edge is refused; the 4 kHz of the low-pass leaves the first
//             harmonics of the top notes).
//
// THE PITCH DETECTOR (written from scratch; the method is the published, textbook one, no third-party code):
//   1. FIR low-pass (33 taps, Hann-windowed sinc, cut-off 4.2 kHz, Q15) and keep every 4th output: 11,025 Hz. Stream state is carried across frames (frames the filter had no time for wait for
//      the next one), so the signal is gapless; a `lost` gap from the tap, a source change, a filter more than 46 ms behind or a `frames == 0` frame cannot put a seam into a window (a gap
//      restarts the window and the readings).
//   2. Every analysis takes the newest 1,536 decimated frames (139 ms), removes the mean, applies a Hann window and computes the autocorrelation through the FFT (Wiener-Khinchin, 2,048 points
//      zero-padded: two forward transforms, because the power spectrum is real and symmetric), see ../common/o3m_fft.h. The correlation is divided by that of the window itself (the analytic Hann
//      autocorrelation: Boersma's correction) and by its value at lag 0, so a periodic signal peaks at 1.0 whatever its level or its length in the window.
//   3. The pitch is the FIRST peak, after the initial lobe, whose (parabola-interpolated) height reaches 0.9 of the highest one (a harmonic-rich note cannot be read an octave low: the
//      peak at 2 periods is as high as the one at 1), and at least 0.6 (else there is no pitch: noise, a chord, a click).
//   4. The period is then refined on the peaks at 2, 4, 8, ... periods (each search is +-1 sample around the previous estimate times two): the interpolation error of a peak is the same few
//      hundredths of a sample whatever its lag, so the period gets 1/m of it. This is what keeps the 55 Hz .. 1.76 kHz sines within 0.1 cent and a saw within 0.3 cent in the host test.
//      A window whose strongest spectral line is above 3.6 kHz (the power spectrum of step 2 is at hand) is out of range and gives no reading: a tone there has its correlation peaks on the
//      wrong lags at 11 kHz (a 4 kHz tone would read as a 1 kHz note).
//   5. The last 5 readings go through a median (at least 3 valid, else NO SIGNAL) and the note name has a hysteresis of 8 cents, so the needle does not jitter on a held note.
//
// COST per 33 ms frame (ESTIMATE from the compiled loops and the operation counts, owed to the unit: QA-MOD-033). The app's state is in SDRAM, which the core does not cache (about 28 cycles a load),
// so every pass over an array is slow, and the work is built around that: the filter copies its taps, history and new frames to the stack and filters from there (about 365 outputs a frame,
// 12 kMACs, estimated 0.3 to 0.5 ms), in chunks of 32 outputs with a look at the clock between them (it stops at 1 ms and carries the rest to the next frame); the analysis is 120 chunks of at
// most 256 butterflies (a butterfly is 40 to 50 cycles in cache, up to 350 from SDRAM: 0.1 to 0.5 ms a chunk) or 512 array elements, at most 20 chunks AND 2 ms of the clock a frame, with the
// clock checked BEFORE every chunk (the first too). The tuner's slice is therefore about 1.3 ms nominal and cannot run past the 2 ms guard plus one chunk, so view_draw (filter + slice + drawing)
// stays under its 4 ms (O3M_VIEW_MAX_US) if the estimates hold, and if they do not the guard cuts the work instead (a slower reading, never a quarantine). A reading takes about 6 frames
// (200 ms) when the clock never trips; more on a slower unit.
#include <o3module.h>

#include "o3m_edit.h"
#include "o3m_fft.h"
#include "o3m_text.h"
#include "tuner_gen.h"

namespace {

constexpr uint32_t kTaps = 33;
constexpr uint32_t kTail = kTaps - 1u;      // input frames kept from one frame to the next
constexpr uint32_t kDecim = 4;
constexpr uint32_t kInMax = 2048;           // frames asked of tap_stream a frame (the tap holds 93 ms: a frame never loses any at 33 ms)
constexpr uint32_t kInCap = 2u * kInMax;    // input frames waiting for the filter: what a frame could not filter stays here for the next one
constexpr uint32_t kChunkOut = 32;          // outputs of the filter between two looks at the clock (about 130 input frames)
constexpr uint32_t kRing = 2048;            // decimated frames kept (186 ms)
constexpr uint32_t kWin = 1536;             // the analysis window, decimated frames
constexpr uint32_t kFft = 2048;             // kWin + the longest lag (367) fits: no wrap-around in the correlation
constexpr uint32_t kLagMax = 367;           // 11,025 / 30 Hz
constexpr uint32_t kChunksPerFrame = 20;    // chunks of analysis a frame at most (256 butterflies or 512 array elements each)...
constexpr uint32_t kGuardMs = 2;            // ...and none once the clock has moved this much since the frame began (checked BEFORE every chunk)
constexpr uint32_t kFirMs = 1;              // the filter stops (and carries the rest to the next frame) once the clock has moved this much
constexpr uint32_t kFftChunk = 256;         // butterflies (or bit-reversal steps) of an FFT chunk
constexpr uint32_t kPassChunk = 512;        // array elements of a pass of the analysis
constexpr uint32_t kStallMs = 500;          // no samples for this long: the reading is dropped
constexpr float kMinMeanSq = 10000.0f;      // rms 100 of 32768: -50.3 dBFS
constexpr float kClarityMin = 0.6f;
constexpr float kPickRatio = 0.9f;
constexpr float kHystNotes = 0.58f;         // a note is kept until the pitch is this far (semitones) from it: 8 cents past the half-way point
constexpr uint32_t kHist = 5;
constexpr uint32_t kBandBin = 669;         // 3.6 kHz in bins of the 2,048-point transform at 11,025 Hz

// 33 taps, Q15, DC gain 1 (sum 32767), sum |h| = 43,199: the largest accumulator is 43,199 * 32,768 = 1.4e9, inside int32.
const int16_t kFir[kTaps] = {-1,   10,   48,   105,  135,  78,   -112, -412, -712, -824, -547, 250,  1546, 3144, 4704, 5842, 6259,
                             5842, 4704, 3144, 1546, 250,  -547, -824, -712, -412, -112, 78,   135,  105,  48,   10,   -1};

enum Phase : uint32_t { kIdle = 0, kCopy = 1, kCenter = 2, kWindow = 3, kFft1 = 4, kPower = 5, kFft2 = 6, kPick = 7 };

struct State {
  const o3m_host* host;  // the math
  const o3m_io_api* io;  // NULL: the host lent no io table
  uint32_t source;       // O3M_TAP_INPUT or O3M_TAP_MASTER, as the parameter says
  uint32_t selected;     // what the tap was last told (0: nothing yet)
  uint32_t a4;           // Hz
  uint32_t row;          // the row of the two the cursor is on
  float rate;            // the decimated rate, Hz
  // the stream
  uint32_t started;      // 1 after the first poll of the page
  uint32_t last_data;    // now_ms of the last frame that brought samples
  uint32_t rd;           // `in[rd .. rd + have)` waits for the filter
  uint32_t have;
  uint32_t wr;           // decimated frames written to the ring since the restart (free running)
  uint32_t filled;       // the same, saturating at kRing
  // the analysis
  uint32_t phase;
  uint32_t idx;          // progress through the current pass
  uint32_t astart;       // ring position of the window's first frame
  int32_t asum;
  float asq, amean;
  float pm;              // the strongest line of the power spectrum so far, and its bin
  uint32_t pk;
  o3fft::Hann hann;
  o3fft::Job job;
  float r0inv;
  uint32_t hi_peak;      // 1: the strongest line of the window is above 3.6 kHz (kBandBin): out of range, NO SIGNAL (a tone near a quarter of the sample rate has correlation peaks at the wrong lags, and would read as a note a few octaves down)
  // the readings
  float hist[kHist];     // log2 of the frequency of each of the last readings, newest first
  uint32_t ok[kHist];    // 1 where that reading was valid
  uint32_t locked;       // 1: at least 3 of the last 5 were valid
  float l2;              // the median, log2 Hz
  float clarity;         // height of the last valid peak, 0.6..1.1
  int32_t note;          // MIDI note shown, -1: none
  alignas(4) int16_t in[kInCap];
  int16_t tail[kTail];   // the last 32 input frames the filter has consumed
  int16_t ring[kRing];
  float re[kFft];
  float im[kFft];
  float tw_c[kFft / 2];
  float tw_s[kFft / 2];
  float rinv[kLagMax + 2];  // 1 / the Hann window's own normalised autocorrelation, lags 0..kLagMax + 1
};
static_assert(sizeof(State) <= TUNER_STATE_BYTES, "module.json: state_bytes is too small for State");

bool has_tap(const State* s) {
  return s->io != nullptr && s->io->now_ms != nullptr && s->io->size >= __builtin_offsetof(o3m_io_api, tap_stream) + sizeof(s->io->tap_stream) && s->io->tap_select != nullptr &&
         s->io->tap_stream != nullptr;
}

// Boersma's autocorrelation of a Hann window of length 1 at the lag x (a fraction of the window): the correction that makes a periodic signal's windowed correlation peak at 1.
float hann_autocorr(const o3m_host* h, float x) {
  const float a = 6.2831853f * x;
  return (1.0f - x) * (0.6666667f + 0.3333333f * h->cos_f(a)) + h->sin_f(a) * 0.15915494f;
}

void restart(State* s) {
  s->phase = kIdle;
  s->rd = 0;
  s->have = 0;
  s->wr = 0;
  s->filled = 0;
  for (uint32_t i = 0; i < kTail; ++i) {
    s->tail[i] = 0;
  }
  for (uint32_t i = 0; i < kHist; ++i) {
    s->hist[i] = 0.0f;
    s->ok[i] = 0;
  }
  s->locked = 0;
  s->note = -1;
}

int init(void* raw, const o3m_host* host) {
  State* s = static_cast<State*>(raw);
  s->host = host;
  s->io = O3M_HOST_HAS(host, io) ? host->io : nullptr;
  s->source = O3M_TAP_INPUT;
  s->selected = 0;
  s->started = 0;
  s->last_data = 0;
  s->a4 = 440;
  s->rate = host->sample_rate * (1.0f / static_cast<float>(kDecim));
  s->clarity = 0.0f;
  s->l2 = 0.0f;
  s->r0inv = 0.0f;
  s->hi_peak = 0;
  restart(s);
  if (host->draw == nullptr || host->log2_f == nullptr || host->exp2_f == nullptr || host->sin_f == nullptr || host->cos_f == nullptr) {
    return 1;
  }
  o3fft::init_twiddle(host, s->tw_c, s->tw_s, kFft);
  for (uint32_t t = 0; t < kLagMax + 2u; ++t) {
    const float a = hann_autocorr(host, static_cast<float>(t) * (1.0f / static_cast<float>(kWin)));
    s->rinv[t] = 1.0f / a;  // a is 1 at lag 0 and 0.68 at the longest lag: never near 0
  }
  return 0;
}

uint32_t clamp_int(float v, float lo, float hi) {
  const float x = v >= lo ? (v <= hi ? v : hi) : lo;  // a NaN falls to the lower bound
  return static_cast<uint32_t>(x + 0.5f);
}

void set_param(void* raw, uint32_t id, float value) {
  State* s = static_cast<State*>(raw);
  if (id == TUNER_PARAM_A4) {
    s->a4 = clamp_int(value, 415.0f, 466.0f);
  } else if (id == TUNER_PARAM_SOURCE) {
    s->source = clamp_int(value, 1.0f, 2.0f);
  }
}

// A4 and SOURCE come back before enter() (set_param: the project's kept values or the manifest's defaults); enter() leaves them alone.
void enter(void* raw) {
  State* s = static_cast<State*>(raw);
  s->row = 0;
  s->selected = 0;
  s->started = 0;
  restart(s);
}

enum Row : uint32_t { kRowA4, kRowSource, kRowCount };
constexpr int32_t kCoarse[kRowCount] = {5, 1};  // Shift + the encoder: A4 by five Hz a detent; SOURCE is two choices

void input(void* raw, const o3m_ui_event* ev) {
  State* s = static_cast<State*>(raw);
  const o3edit::Input in = o3edit::grammar(ev, &s->row, kRowCount, nullptr, 1);
  if (in.move == o3edit::kMoveValue || in.move == o3edit::kMoveDefault) {
    const int32_t d = in.move == o3edit::kMoveValue ? o3edit::steps(in, kCoarse[s->row]) : 0;
    const bool reset = in.move == o3edit::kMoveDefault;
    if (s->row == kRowA4) {
      s->a4 = reset ? 440u : static_cast<uint32_t>(o3edit::clamp_i(static_cast<int32_t>(s->a4) + d, 415, 466));
    } else {
      s->source = reset ? static_cast<uint32_t>(O3M_TAP_INPUT) : static_cast<uint32_t>(o3edit::clamp_i(static_cast<int32_t>(s->source) + d, 1, 2));
    }
    o3edit::own_set(s->io, s->row, static_cast<float>(s->row == kRowA4 ? s->a4 : s->source));  // kept with the project
  }
}

// ---- the stream: low-pass and keep every 4th ---------------------------------------------------------------------------------------------

// The filter, in chunks of kChunkOut outputs (4 input frames each) between looks at the clock: everything the app owns is in SDRAM, uncached, so a chunk first copies the 32 frames of history and
// its new frames to the stack (sequential loads), and the taps too (the table is in SDRAM as well), and filters out of the stack. What is left when the time is up stays in `in` for the next frame,
// so the stream stays gapless (it only falls behind; if it falls more than kInMax frames behind, poll() calls it a gap).
void decimate(State* s, const o3m_io_api* io, uint32_t t0) {
  int16_t taps[kTaps];
  for (uint32_t i = 0; i < kTaps; ++i) {
    taps[i] = kFir[i];
  }
  int16_t buf[kTail + kDecim * kChunkOut];
  while (s->have >= kDecim && io->now_ms() - t0 < kFirMs) {
    const uint32_t groups = s->have / kDecim < kChunkOut ? s->have / kDecim : kChunkOut;
    const uint32_t m = groups * kDecim;
    for (uint32_t i = 0; i < kTail; ++i) {
      buf[i] = s->tail[i];
    }
    for (uint32_t i = 0; i < m; ++i) {
      buf[kTail + i] = s->in[s->rd + i];
    }
    for (uint32_t g = 0; g < groups; ++g) {
      const int16_t* p = buf + g * kDecim + (kDecim - 1u);  // the taps end on the last frame of the group
      int32_t acc = 0;
      for (uint32_t i = 0; i < kTaps; ++i) {
        acc += static_cast<int32_t>(taps[i]) * p[i];
      }
      acc >>= 15;
      acc = acc > 32767 ? 32767 : (acc < -32768 ? -32768 : acc);
      s->ring[s->wr & (kRing - 1u)] = static_cast<int16_t>(acc);
      ++s->wr;
      s->filled = s->filled < kRing ? s->filled + 1u : kRing;
    }
    for (uint32_t i = 0; i < kTail; ++i) {
      s->tail[i] = buf[m + i];
    }
    s->rd += m;
    s->have -= m;
  }
  if (s->have == 0u) {
    s->rd = 0;
  }
}

// ---- the analysis -----------------------------------------------------------------------------------------------------------------------

// The normalised correlation at lag t, once the second transform is done: re[t] is N times the circular correlation of the windowed signal.
float nsd(const State* s, uint32_t t) { return s->re[t] * s->rinv[t] * s->r0inv; }

struct Peak {
  float d;  // offset of the vertex from the sample, -1..1
  float h;  // interpolated height
};

Peak parabola(const State* s, uint32_t t) {
  const float ym = nsd(s, t - 1u), y0 = nsd(s, t), yp = nsd(s, t + 1u);
  const float dd = ym - 2.0f * y0 + yp;
  Peak p;
  p.d = 0.0f;
  p.h = y0;
  if (dd < -1.0e-6f) {
    float d = 0.5f * (ym - yp) / dd;
    d = d > 1.0f ? 1.0f : (d < -1.0f ? -1.0f : d);
    float h = y0 - (yp - ym) * (yp - ym) / (8.0f * dd);
    h = h > 1.1f ? 1.1f : h;  // a flat-topped peak makes the vertex run away: no correlation is above 1
    p.d = d;
    p.h = h > y0 ? h : y0;
  }
  return p;
}

void push_reading(State* s, bool valid, float l2, float clarity) {
  for (uint32_t i = kHist - 1u; i > 0u; --i) {
    s->hist[i] = s->hist[i - 1u];
    s->ok[i] = s->ok[i - 1u];
  }
  s->hist[0] = l2;
  s->ok[0] = valid ? 1u : 0u;
  float v[kHist];
  uint32_t n = 0;
  for (uint32_t i = 0; i < kHist; ++i) {
    if (s->ok[i] != 0u) {
      uint32_t j = n++;  // insertion sort
      while (j > 0u && v[j - 1u] > s->hist[i]) {
        v[j] = v[j - 1u];
        --j;
      }
      v[j] = s->hist[i];
    }
  }
  s->locked = n >= 3u ? 1u : 0u;
  if (s->locked != 0u) {
    s->l2 = v[n / 2u];
  }
  if (valid) {
    s->clarity = clarity;
  }
}

// After the second transform: find the pitch and push the reading.
void pick(State* s) {
  s->r0inv = s->re[0] > 1.0e-20f ? 1.0f / s->re[0] : 0.0f;
  bool found = false;
  float period = 0.0f, clarity = 0.0f;
  if (s->r0inv > 0.0f && s->hi_peak == 0u) {
    uint32_t t = 1;
    while (t < kLagMax && nsd(s, t) > 0.0f) {  // the initial lobe around lag 0
      ++t;
    }
    float gmax = 0.0f;
    for (uint32_t u = t; u <= kLagMax; ++u) {
      const float y = nsd(s, u);
      gmax = y > gmax ? y : gmax;
    }
    if (gmax >= kClarityMin) {
      // the first positive stretch whose peak reaches kPickRatio of the highest
      uint32_t best = 0;
      float best_y = 0.0f;
      for (uint32_t u = t; u <= kLagMax + 1u && !found; ++u) {
        const float y = u <= kLagMax ? nsd(s, u) : 0.0f;
        if (y > 0.0f) {
          if (best == 0u || y > best_y) {
            best = u;
            best_y = y;
          }
        } else if (best != 0u) {
          const Peak p = parabola(s, best);
          if (p.h >= kPickRatio * gmax) {
            found = true;
            period = static_cast<float>(best) + p.d;
            clarity = p.h;
          }
          best = 0;
        }
      }
      // refine on the peaks at 2, 4, 8, ... periods
      if (found) {
        float m = 1.0f;
        for (;;) {
          const float m2 = m * 2.0f;
          const float lag = m2 * period;
          if (lag + 1.5f > static_cast<float>(kLagMax)) {
            break;
          }
          const uint32_t tc = static_cast<uint32_t>(lag + 0.5f);
          uint32_t b = tc;
          for (uint32_t k = tc - 1u; k <= tc + 1u; ++k) {
            b = nsd(s, k) > nsd(s, b) ? k : b;
          }
          if (nsd(s, b) < 0.5f * clarity || nsd(s, b) < nsd(s, b - 1u) || nsd(s, b) < nsd(s, b + 1u)) {
            break;
          }
          period = (static_cast<float>(b) + parabola(s, b).d) / m2;
          m = m2;
        }
      }
    }
  }
  bool valid = false;
  float l2 = 0.0f;
  if (found && clarity >= kClarityMin) {
    const float f = s->rate / period;
    if (period < static_cast<float>(kLagMax - 1u) && f <= 2100.0f) {  // (kLagMax is the longest lag searched, 30.0 Hz: a period at its edge has no peak to interpolate)
      valid = true;
      l2 = s->host->log2_f(f);
    }
  }
  push_reading(s, valid, l2, clarity);
}

// One chunk of the analysis (at most kFftChunk butterflies or kPassChunk array elements, so far under a millisecond even from SDRAM), then back to the clock.
void analysis_chunk(State* s) {
  switch (s->phase) {
    case kIdle:
      if (s->filled >= kWin + 16u) {  // (+16: the filter's start-up is behind the window)
        s->astart = s->wr - kWin;
        s->idx = 0;
        s->asum = 0;
        s->phase = kCopy;
      }
      break;
    case kCopy: {  // the newest kWin decimated frames from the ring, and their sum
      const uint32_t end = s->idx + kPassChunk < kWin ? s->idx + kPassChunk : kWin;
      for (uint32_t j = s->idx; j < end; ++j) {
        const int16_t v = s->ring[(s->astart + j) & (kRing - 1u)];
        s->re[j] = static_cast<float>(v);
        s->asum += v;
      }
      s->idx = end;
      if (end == kWin) {
        s->amean = static_cast<float>(s->asum) * (1.0f / static_cast<float>(kWin));
        s->asq = 0.0f;
        s->idx = 0;
        s->phase = kCenter;
      }
      break;
    }
    case kCenter: {  // the mean out, and the energy
      const uint32_t end = s->idx + kPassChunk < kWin ? s->idx + kPassChunk : kWin;
      for (uint32_t j = s->idx; j < end; ++j) {
        const float v = s->re[j] - s->amean;
        s->re[j] = v;
        s->asq += v * v;
      }
      s->idx = end;
      if (end == kWin) {
        if (!(s->asq >= kMinMeanSq * static_cast<float>(kWin))) {  // silence, DC, or too quiet
          push_reading(s, false, 0.0f, 0.0f);
          s->phase = kIdle;
        } else {
          o3fft::hann_begin(s->host, &s->hann, kWin);
          s->idx = 0;
          s->phase = kWindow;
        }
      }
      break;
    }
    case kWindow: {  // the Hann window over the first kWin points, zeros after them, the imaginary part cleared
      const uint32_t end = s->idx + kPassChunk < kFft ? s->idx + kPassChunk : kFft;
      for (uint32_t j = s->idx; j < end; ++j) {
        s->re[j] = j < kWin ? s->re[j] * o3fft::hann_next(&s->hann) : 0.0f;
        s->im[j] = 0.0f;
      }
      s->idx = end;
      if (end == kFft) {
        o3fft::begin(&s->job, kFft);
        s->phase = kFft1;
      }
      break;
    }
    case kFft1:
      if (o3fft::step(&s->job, s->re, s->im, s->tw_c, s->tw_s, kFftChunk)) {
        s->idx = 0;
        s->pm = 0.0f;
        s->pk = 0;
        s->phase = kPower;
      }
      break;
    case kPower: {  // the power spectrum is real and symmetric: the forward transform of it is the correlation (times N)
      const uint32_t end = s->idx + kPassChunk < kFft ? s->idx + kPassChunk : kFft;
      for (uint32_t k = s->idx; k < end; ++k) {
        const float p = s->re[k] * s->re[k] + s->im[k] * s->im[k];
        s->re[k] = p;
        s->im[k] = 0.0f;
        if (k >= 1u && k <= kFft / 2u && p > s->pm) {
          s->pm = p;
          s->pk = k;
        }
      }
      s->idx = end;
      if (end == kFft) {
        s->hi_peak = s->pk >= kBandBin ? 1u : 0u;
        o3fft::begin(&s->job, kFft);
        s->phase = kFft2;
      }
      break;
    }
    case kFft2:
      if (o3fft::step(&s->job, s->re, s->im, s->tw_c, s->tw_s, kFftChunk)) {
        s->phase = kPick;
      }
      break;
    default:
      pick(s);
      s->phase = kIdle;
      break;
  }
}

// Once a frame (view_draw): the tap's point, the stream, and a bounded slice of analysis.
void poll(State* s) {
  if (!has_tap(s)) {
    return;
  }
  const o3m_io_api* io = s->io;
  const uint32_t t0 = io->now_ms();
  if (s->selected != s->source) {
    if (io->tap_select(s->source) != 0) {
      return;
    }
    s->selected = s->source;
    restart(s);
  }
  if (s->have > kInMax) {
    restart(s);  // the filter fell more than 46 ms behind: that is a gap in the signal too
  }
  if (s->have != 0u && s->rd + s->have + kInMax > kInCap) {  // room for a whole frame's frames: move the waiting ones to the front
    for (uint32_t i = 0; i < s->have; ++i) {
      s->in[i] = s->in[s->rd + i];
    }
    s->rd = 0;
  }
  uint32_t lost = 0;
  const uint32_t at = s->rd + s->have;
  uint32_t n = io->tap_stream(s->in + at, kInMax, &lost);
  n = n <= kInMax ? n : 0u;  // (a host that answers more than it was given is not believed)
  if (s->started == 0u) {
    s->started = 1;
    s->last_data = t0;
  }
  if (n != 0u) {
    s->last_data = t0;
  } else if (t0 - s->last_data >= kStallMs) {
    restart(s);  // the tap has brought nothing for a while (the transport is stopped, the point is silent): the last note is not the present one
    s->last_data = t0;
  }
  if (lost != 0u) {
    restart(s);  // a gap in the signal: nothing before it may be in a window; the frames that follow the gap are good
    for (uint32_t i = 0; i < n; ++i) {  // (dst below src: a forward copy is safe)
      s->in[i] = s->in[at + i];
    }
    s->have = n;
  } else {
    s->have += n;
  }
  decimate(s, io, t0);
  // A bounded slice of the analysis: at most kChunksPerFrame chunks, none once the clock has moved kGuardMs (checked before EVERY chunk, the first too), and one reading at most (the next
  // starts in the next frame).
  uint32_t chunks = 0;
  while (chunks < kChunksPerFrame && (s->phase != kIdle || s->filled >= kWin + 16u) && io->now_ms() - t0 < kGuardMs) {
    analysis_chunk(s);
    ++chunks;
    if (s->phase == kIdle) {
      break;
    }
  }
}

// ---- the note ------------------------------------------------------------------------------------------------------------------------

struct Reading {
  bool live;
  int32_t note;
  float cents;  // -58..58
  float hz;
};

Reading read_note(State* s) {
  Reading r;
  r.live = false;
  r.note = 0;
  r.cents = 0.0f;
  r.hz = 0.0f;
  if (s->locked == 0u) {
    s->note = -1;
    return r;
  }
  const float x = 69.0f + 12.0f * (s->l2 - s->host->log2_f(static_cast<float>(s->a4)));  // fractional MIDI note
  int32_t n = static_cast<int32_t>(x + 0.5f);
  if (s->note >= 0) {
    const float d = x - static_cast<float>(s->note);
    if (d < kHystNotes && d > -kHystNotes) {
      n = s->note;
    }
  }
  n = n < 0 ? 0 : (n > 127 ? 127 : n);
  s->note = n;
  r.live = true;
  r.note = n;
  r.cents = (x - static_cast<float>(n)) * 100.0f;
  r.hz = s->host->exp2_f(s->l2);
  return r;
}

// ---- drawing ----------------------------------------------------------------------------------------------------------------------------

void view_draw(void* raw, uint32_t, o3m_canvas* c) {
  State* s = static_cast<State*>(raw);
  poll(s);
  const Reading r = read_note(s);
  const o3m_draw_api* api = c->draw;
  const int32_t w = c->width;
  const int32_t h = c->height;
  char text[64];
  api->rect(c, 0, 0, w, h, O3M_COLOR_PANE);
  api->text(c, 8, 4, "TUNER", O3M_FACE_DISPLAY, O3M_COLOR_TEXT);
  // the two rows at the top right (the grammar of o3m_edit.h): A4 and SOURCE
  o3edit::PageRow rows[kRowCount];
  rows[kRowA4].name = "A4";
  rows[kRowA4].dim = false;
  uint32_t n = o3text::put_num(rows[kRowA4].value, 0, s->a4);
  n = o3text::put_text(rows[kRowA4].value, n, " HZ");
  rows[kRowA4].value[n] = '\0';
  rows[kRowSource].name = "SOURCE";
  rows[kRowSource].dim = false;
  n = o3text::put_text(rows[kRowSource].value, 0, s->source == O3M_TAP_INPUT ? "INPUT" : "MASTER");
  rows[kRowSource].value[n] = '\0';
  o3edit::draw_rows(c, w - 236, 4, 224, rows, kRowCount, s->row);
  if (!has_tap(s)) {
    api->text(c, 8, 60, "NO AUDIO TAP FOR THIS APP", O3M_FACE_DATA, O3M_COLOR_ACCENT);
    return;
  }

  // the note
  if (r.live) {
    static const char kNames[25] = "C C#D D#E F F#G G#A A#B ";
    const uint32_t semi = static_cast<uint32_t>(r.note) % 12u;
    n = 0;
    text[n++] = kNames[2u * semi];
    if (kNames[2u * semi + 1u] != ' ') {
      text[n++] = kNames[2u * semi + 1u];
    }
    const int32_t oct = r.note / 12 - 1;
    if (oct < 0) {
      text[n++] = '-';
    }
    n = o3text::put_num(text, n, static_cast<uint32_t>(oct < 0 ? -oct : oct));
    text[n] = '\0';
    api->text(c, w / 2 - api->text_width(c, text, O3M_FACE_DISPLAY) / 2, 56, text, O3M_FACE_DISPLAY, O3M_COLOR_TEXT);
  } else {
    api->text(c, w / 2 - api->text_width(c, "NO SIGNAL", O3M_FACE_DISPLAY) / 2, 56, "NO SIGNAL", O3M_FACE_DISPLAY, O3M_COLOR_TEXT_DIM);
  }

  // the cents needle: -50 .. +50 across the area
  const int32_t x0 = 12;
  const int32_t bw = w - 24;
  const int32_t by = 112;
  const int32_t bh = 40;
  api->rect(c, x0, by, bw, bh, O3M_COLOR_EMPTY);
  api->rect(c, x0 + bw * 45 / 100, by, bw / 10 + 1, bh, O3M_COLOR_VOL);  // +-5 cents: in tune
  for (int32_t i = 0; i <= 10; ++i) {
    api->rect(c, x0 + bw * i / 10, by + bh, 1, (i % 5) == 0 ? 10 : 5, O3M_COLOR_TEXT_DIM);
  }
  static const int8_t kMarks[5] = {-50, -25, 0, 25, 50};
  for (int32_t i = 0; i < 5; ++i) {
    n = o3text::put_signed(text, 0, kMarks[i]);
    text[n] = '\0';
    const int32_t tw = api->text_width(c, text, O3M_FACE_MICRO);
    int32_t lx = x0 + bw * i / 4 - tw / 2;
    lx = lx < 0 ? 0 : (lx + tw > w ? w - tw : lx);
    api->text(c, lx, by + bh + 12, text, O3M_FACE_MICRO, O3M_COLOR_TEXT_DIM);
  }
  if (r.live) {
    float cn = r.cents;
    cn = cn > 50.0f ? 50.0f : (cn < -50.0f ? -50.0f : cn);
    const int32_t nx = x0 + static_cast<int32_t>((cn + 50.0f) * 0.01f * static_cast<float>(bw));
    const bool in_tune = r.cents > -5.0f && r.cents < 5.0f;
    api->rect(c, nx - 2, by - 6, 5, bh + 12, in_tune ? O3M_COLOR_PLAY : O3M_COLOR_ACCENT);
  }

  // the readouts
  n = o3text::put_text(text, 0, "FREQ ");
  if (r.live) {
    const uint32_t tenths = static_cast<uint32_t>(r.hz * 10.0f + 0.5f);
    n = o3text::put_num(text, n, tenths / 10u);
    text[n++] = '.';
    text[n++] = static_cast<char>('0' + tenths % 10u);
    n = o3text::put_text(text, n, " HZ");
  } else {
    n = o3text::put_text(text, n, "--");
  }
  text[n] = '\0';
  api->text(c, 12, 196, text, O3M_FACE_DATA, O3M_COLOR_TEXT);
  n = o3text::put_text(text, 0, "CENTS ");
  if (r.live) {
    const int32_t ci = static_cast<int32_t>(r.cents + (r.cents < 0.0f ? -0.5f : 0.5f));
    n = o3text::put_signed(text, n, ci);
  } else {
    n = o3text::put_text(text, n, "--");
  }
  text[n] = '\0';
  api->text(c, w - 12 - api->text_width(c, text, O3M_FACE_DATA), 196, text, O3M_FACE_DATA, O3M_COLOR_TEXT);

  // the confidence: the height of the correlation peak, 0..100 %
  float cf = s->locked != 0u ? s->clarity : 0.0f;
  cf = cf > 1.0f ? 1.0f : cf;
  api->text(c, 12, 236, "CONFIDENCE", O3M_FACE_LABEL, O3M_COLOR_TEXT_DIM);
  api->rect(c, 120, 232, bw - 108 - 64, 20, O3M_COLOR_EMPTY);
  api->rect(c, 120, 232, static_cast<int32_t>(cf * static_cast<float>(bw - 108 - 64)), 20, O3M_COLOR_PLAY);
  n = o3text::put_num(text, 0, static_cast<uint32_t>(cf * 100.0f + 0.5f));
  n = o3text::put_text(text, n, " %");
  text[n] = '\0';
  api->text(c, w - 12 - api->text_width(c, text, O3M_FACE_DATA), 234, text, O3M_FACE_DATA, O3M_COLOR_TEXT);

  api->text(c, 8, h - 20, "SIGNAL BELOW -50 DBFS, OR NO CLEAR PITCH: NO SIGNAL", O3M_FACE_LABEL, O3M_COLOR_TEXT_DIM);
}

void leave(void*) {}
void shutdown(void*) {}

const o3m_ui kUi = {O3M_UI_SIZE(TUNER_ABI_MINOR), enter, input, view_draw, leave, nullptr};

}  // namespace

extern "C" O3M_EXPORT const o3m_module o3m_entry __attribute__((section(".o3m_entry"))) = {
    TUNER_ENTRY_HEAD, sizeof(State), TUNER_ENTRY_PARAMS, init, nullptr, set_param, nullptr, nullptr, shutdown, TUNER_ENTRY_VIEWS(&kUi),
};
