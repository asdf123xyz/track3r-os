// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// AMIGA conversion (OS-AMG1, new Community OS behaviour, no historical parity): the Sample edit action that turns the sample of an instrument into one the Amiga voice plays at the
// pitch it had, with a band-limited resampler that runs as the Sample edit job (sample_edit_job.h), a slice of the main loop's pass at a time, into a spare buffer of the bank's edit pool.
// Never in the audio path; no heap; the 8 KB of kernel table lives in the job (SDRAM on the device).
//
// WHAT "THE PITCH STAYS" MEANS. The Modern voice reads a sample of 44,100 Hz (or one that records no rate) at the unity note (60, C-4) one sample per output sample, at the engine's rate
// (44,117.647 Hz; sample_bank.h), and any other recorded rate at that rate's pitch (OS-RATEPITCH: SampleDesc::rate_q28). The Amiga voice at note 60 plays period 428, 3,546,895 / 428 = 8,287.14 Hz (amiga_voices.h; no
// rate compensation). So the conversion resamples by (the rate the sample plays at now) / (the rate it will play at): Modern -> Amiga 8,287: 44,117.647 / 8,287.14 = 5.3236; an
// instrument that is already Amiga plays at 8,287.14 and has its own ratio (1 for the 8,287 conversion, 0.5 for 16,574). The same sound at C-4, the same notes in every pattern.
//   16,574 Hz (period 214, one octave up of 8,287, more top end): the voice at tune 0 would play the data at 8,287 Hz, an octave low. The conversion adds 12 semitones to the instrument's
//   TUNE (the Amiga voice takes the note + the instrument's tune, amiga_voices.cpp note_on: period 214 at note 72), which plays the data at exactly 16,574.28 Hz: pitch kept, no new field,
//   undone with the conversion. The cost: notes above 71 (B-4) reach Paula's floor (period 113) and clamp; refused when the tune cannot take +12 (the OG instrument file's -24..24).
//   Rate and engine: the instrument's sample_rate becomes 8,287 / 16,574 (the saved WAV carries it, so an OctaMED-style sample reloads as it was left) and its engine AMIGA.
//
// THE RESAMPLER. A windowed-sinc low-pass fused with the interpolation: output frame m reads the source around position m * step (Q32.32 from the start, so there is no drift),
// weights h(t) from a table (Kaiser window, beta 8, 32 output frames each side, cutoff 0.45 of the OUTPUT rate, of the SOURCE rate when that is the lower; 64 table steps per output frame, linear in between), stretched by
// the ratio when it is above 1 (decimation) and not stretched when it is below (interpolation). The weights are normalised by their own sum, so DC is exact and the table's
// interpolation ripple does not tilt the level. Outside the sample the source is silence. Cost: about 2 * 32 * ratio taps a frame, 340 at 5.32: about 25 us a frame on the M4F.
//   Measured on the host (tests/amiga_convert_test.cpp, both rates): tones at 0.10, 0.30 and 0.40 of the new rate come back within 0.01 dB, 0.45 is -6 dB (the corner), 0.48 is -39 dB;
//   a tone at 0.52, 0.55, 0.60 or 0.70 of it (which would fold to 0.48, 0.45, 0.40, 0.30) is down 90 dB or more.
// 8-BIT (optional): Paula plays the TOP BYTE of each 16-bit sample (audio/paula.h, SAMPLE: floor(x / 256), so an unquantised sample is truncated, with half a step of DC offset);
// the quantise rounds to the nearest 8-bit level instead, plain, with no dither (deterministic, as an 8-bit sampler stored it; the voice then plays exactly the level stored), and
// keeps the sample 16-bit in memory (level * 256).
//
// Points, loop and slices are the instrument's 0..65535 fractions of the sample's length (wave_math.h), so they follow the new length by themselves (to a frame of the new rate); a
// loop whose length is not a whole number of the new frames has a seam of up to half a frame.

#include <o3os/audio/paula.h>
#include <o3os/audio/voices.h>
#include <o3os/hal/audio.h>

#include <cstdint>

namespace o3os::app {

inline constexpr unsigned kAmigaPeriod8 = 428;   // 8,287 Hz: the unity note
inline constexpr unsigned kAmigaPeriod16 = 214;  // 16,574 Hz: one octave up
inline constexpr int kAmigaTune16 = 12;          // the tune the 16,574 conversion adds (semitones)

// The data rate in Hz the converted sample is labelled with: 8,287 and 16,574.
constexpr std::uint32_t amiga_rate_hz(unsigned period) noexcept {
  return (audio::paula::kPalClockHz + period / 2u) / period;
}

// Source frames per output frame, Q32.32: the rate the sample plays at at the unity note now over the one the Amiga voice plays `period` at. `src_amiga`: the instrument plays on the Amiga
// voice already (8,287.14 Hz); else the engine's rate. Worked out with integers (a long division, as paula.h period_to_delta_div), no double.
// OS-RATEPITCH: a Modern sample plays at its own rate now (SampleDesc::rate_q28), so `src_rate_hz` (the instrument's sample_rate, 0 or 44,100 = the engine's) is the rate it plays at: the
// engine's over 44,100 times it. Unused for an Amiga source (its pitch is the period's).
constexpr std::uint64_t amiga_step_q32(unsigned period, bool src_amiga, std::uint32_t src_rate_hz = 0) noexcept {
  const std::uint64_t play_mhz = (src_rate_hz == 0u || src_rate_hz == audio::kEngineRateHz) ? std::uint64_t{hal::audio::sample_rate_mhz()}
                                                                                          : std::uint64_t{audio::rate_factor_q28(src_rate_hz)} * hal::audio::sample_rate_mhz() >> 28;
  const std::uint64_t num = src_amiga ? period : static_cast<std::uint64_t>(period) * play_mhz;
  const std::uint64_t den = src_amiga ? kAmigaPeriod8 : static_cast<std::uint64_t>(audio::paula::kPalClockHz) * 1000u;
  std::uint64_t q = num / den;
  std::uint64_t rem = num % den;
  for (unsigned i = 0; i < 32u; ++i) {
    rem <<= 1;
    q <<= 1;
    if (rem >= den) {
      rem -= den;
      q |= 1u;
    }
  }
  return q;
}

// The converted sample's length in frames: frames / step, at least 1.
constexpr std::uint32_t amiga_frames(std::uint32_t frames, std::uint64_t step_q32) noexcept {
  const std::uint64_t n = (static_cast<std::uint64_t>(frames) << 32) / step_q32;
  return n == 0u ? 1u : static_cast<std::uint32_t>(n);
}

// The kernel table: half of the symmetric low-pass, built a few hundred entries a step (sinf and a Bessel series each: the job builds it first, once).
class AmigaKernel {
 public:
  static constexpr unsigned kHalf = 32;                 // output frames each side of the centre
  static constexpr unsigned kRes = 64;                  // table steps per output frame
  static constexpr unsigned kLimit = kHalf * kRes;      // the table's reach in steps
  static constexpr unsigned kSize = kLimit + 2;         // + the entry a linear read at the last step touches
  static constexpr float kCutoff = 0.45f;               // cycles per output frame
  static constexpr float kBeta = 8.0f;                  // the Kaiser window's

  bool ready() const noexcept { return built_ >= kSize; }
  // Builds up to `n` more entries; true when the table is complete.
  bool build(unsigned n) noexcept;
  const float* table() const noexcept { return tab_; }

 private:
  float tab_[kSize];
  unsigned built_ = 0;
};

// Output frames [from, from + n) of the conversion of `src` (`frames` long) into dst[0..n): dst is the chunk. `step_q32` is amiga_step_q32(); `q8` rounds every frame to an 8-bit level.
// `from + n` is held to amiga_frames(). The kernel must be ready().
void amiga_resample(const AmigaKernel& kernel, const std::int16_t* src, std::uint32_t frames, std::uint64_t step_q32, bool q8, std::int16_t* dst, std::uint32_t from,
                    std::uint32_t n) noexcept;

// Output frames the job may do in `budget_us`: about 14 cycles a tap at 180 MHz, never fewer than 8 (PROVISIONAL, DWT on the unit).
std::uint32_t amiga_frames_per_budget(std::uint64_t step_q32, std::uint32_t budget_us) noexcept;

// sin(2 pi r) without libm (a quarter-turn reduction and a Taylor series, |r| < 32); cos(2 pi r) is sin_turns(r + 0.25f). The Sample edit filter (sample_dsp.cpp) reuses it.
float sin_turns(float r) noexcept;

}  // namespace o3os::app
