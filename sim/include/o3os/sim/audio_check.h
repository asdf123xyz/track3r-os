// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The measurements behind the scenario command `assert audio` (OS-E2E-2, docs/specs/OS-E2E.md), and the grammar of its checks. Pure: every function takes the samples of a
// render and returns a number or a verdict; nothing here touches the simulator, the engine or the clock. `double` is fine in sim/ (the float ban covers core, hal, eve, ui, app, model
// and formats). A render is the master as the codec gets it: interleaved 16-bit L, R frames at the engine's rate (hal/audio.h: 44,117.647 Hz). Full scale is 1.0 = 32768.
//
//   assert audio <name> [in=<from>-<to>] <check> ...
//
// <name> is a `render <name> <ms>` that has ended. `in=<from>-<to>` (ms from the render's start; `in=200-end`) limits every check of the line to that part of the render. All
// checks of a line must hold. A value <V> is a number, or `@<other>` (the same measurement on another render, over the same window) with an optional `+n`, `-n` or `*n`
// (`rms<@open-6`: at least 6 dB under the open render; `centroid<@open*0.5`). `±` can be written `+-`. A failure reads "<check>: measured <x>, wanted <y>".
// A check that has nothing to measure is an error, never a pass: pitch, onset and centroid on a render that holds no tone or never starts; pan, zipper and null on silence; alias and zipper on a
// window shorter than their frame; null with a lag that leaves no frame. (`clicks=0` on silence still holds: add `sound` to a line that needs the render to be there.)
//
//   pitch=<Hz>±<n>c        the fundamental, in cents (1200 log2 ratio). <Hz> is a number, `m<note>` (MIDI note, m69 = 440 Hz) or `@<other>`. The loudest stable part of the render
//                          (a run of 10 ms frames within 6 dB of the loudest, up to 8192 samples of it): YIN with an absolute threshold of 0.1 finds the period, then the strongest bin of the
//                          Hann-windowed spectrum (zero-padded 8 times) within 3 % of it, with a parabola through the logs of three bins, refines it: 50 Hz to 5 kHz, within a cent on a clean
//                          tone (tests/audio_check_test.cpp). Above 5.5 kHz the period is too few samples and the answer is a sub-multiple. No periodic part is a failure.
//   rms=<dB>±<d>          the level over both channels, dB re full scale (a full-scale sine is -3.01 dB); also rms<V and rms>V
//   peak=<dB>±<d>          the largest |sample| of either channel, dB re full scale; also peak<V and peak>V
//   dc<x                   the largest |mean| of the two channels, as a fraction of full scale (also dc=x±d). A tone of period p over n periods leaves a mean of about A/(2 pi n): take a
//                          window of 100 periods or more and x of 0.002 (-54 dB)
//   silent  /  sound       every sample within 1 LSB of zero (the engine's silence is exact zeros; the 1 LSB is the rounding of the last stage); `sound` is its opposite
//   clicks=N               clicks<N: the second difference d2[i] = x[i+1] - 2 x[i] + x[i-1] of each channel against its local spread: an outlier is |d2| above 13 x the 90th
//                          percentile of |d2| over the 129 samples around it (taken every 32 samples; the limit is the largest of the windows 128 samples before, at and after, so that
//                          the edge of a note is judged by the note; a percentile, not the MAD, because a tone played slower than it was recorded has d2 = 0 at every second sample) and above 24 LSB (-62.7 dB); outliers within 4 samples are one click. The engine's declick ramps (32 samples in, 256 out) are kinks of about
//                          8 times the tone's own d2 at 441 Hz, under the limit; a step of 1/20 of the tone's amplitude or more is over it. A click is seen in material whose own d2 is small (a tone below a
//                          few kHz, a decay, silence); noise hides one under its own spread. THE LIMIT BELOW ABOUT 350 HZ: a tone's own d2 falls with the square of its frequency and the ramp's kink does not (1/32 of the
//                          tone's value where the fade-in ends), so a note of 220 Hz or less is counted as a click at its start (the kink is 32 times the tone's d2 at 220 Hz, 127 times at 110 Hz, against the limit of 13), and a note
//                          below about 120 Hz at its end too (the fade-out's kink is 8 times smaller). A step is found from smaller sizes the lower the tone (about 65 LSB at 110 Hz, 330 at 220 Hz, 980 at 441 Hz) and at every
//                          phase (tests/audio_check_test.cpp), so for a low note measure the steady part, past the fade-in and before the fade-out: `in=<from>-<to>` with 2 ms and 8 ms cut at either end of the note
//   onset=<ms>±<d>         the first frame at which either channel reaches -50 dB re full scale, in ms from the start of the window
//   envelope=adsr(a=<ms>,d=<ms>,s=<0..1>,r=<ms>,off=<ms>)±<e>
//                          the level curve against a model: the peak envelope (the largest |sample| of either channel over 3 ms, every 1 ms), divided by its own maximum, against the
//                          engine's amplitude envelope (voices.h AMP ENVELOPE: linear segments, attack 0 to 1, decay to s, sustain, release from where it is to 0 over r) that starts at
//                          the onset and releases `off` ms after it. A window's peak is the largest of the sound in it, so the model is read the same way (its largest value over the 3 ms
//                          window); the largest difference over the curve, in fractions of the peak, must be within e (0.05 is enough for the engine's ramps: its stages are on a grid of 8 samples)
//   centroid<V  centroid>V the spectral centroid in Hz: the mean of the magnitude spectra of 2048-sample Hann frames (hop 1024, frames above 1/100 of the loudest frame's energy), over
//                          the bins above DC, of the mid channel. Brighter is higher: a filter closed or a bit depth lowered moves it, a tone's own pitch does too, so compare like with like
//   pan=L  pan=R           the far channel's level is 40 dB or more under the near one;   pan=C±<d>: the two channels' levels differ by at most d dB;   pan=<dB>±<d>: 20 log10(L / R)
//   zipper=N               N spectral-flux spikes in the mid channel: 512-sample Hann frames every 64 samples, flux = the sum of the magnitude increases over the frame before, divided by
//                          that frame's total magnitude; a spike is above 0.05 and above the median + 8 x 1.4826 x MAD of the 49 frames around it; spikes within 8 frames are one. A tone
//                          with a smooth gain has none, a tone whose gain steps by 10 % or more has one at every step. As for clicks, noise hides it
//   alias<V  alias>V       the energy outside the harmonics of a sine (the strongest peak of the spectrum is the fundamental; the bins within 6 of each multiple of it are the signal, and the
//                          four lowest are DC), re the total, in dB, over a Blackman-Harris-windowed 8192-sample part from the middle of the render: what the resampler's interpolation and the
//                          bit reduction add. The window's own skirt and the 16-bit rounding set the floor: about -90 dB for a tone at half scale
//   null=<other>[+<other>...],<dB>[,lag=<n>]
//                          the residual of this render minus another (or the sum of several: `null=n.t1+n.t2,-60`), as the level of the residual re this render's, at most <dB> (-60: a residual 60 dB
//                          under). A mono render counts in both channels. `lag=<n>` (frames, up to 4096): this render is n frames later than the others (the master limiter looks 63 samples
//                          ahead, so the master is the stems' sum 63 frames late: `lag=63`). Same window, and the others as long as this one, less the lag
//
// Exact things stay exact in the scenario grammar (`assert state`, `assert model`, `assert file`); the numbers here are measurements with tolerances, never sample hashes: the float results are
// reproducible on one machine and compiler (the simulator proves it: o3sim --twice compares the hash of every render), not across them.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace o3os::sim {

// ---- the samples -------------------------------------------------------------------------------------------------------------------------

// What `render` captures: the master, interleaved L, R, 16-bit, at `rate` frames a second.
struct Pcm {
  std::vector<std::int16_t> lr;
  double rate = 44100.0;
  std::size_t frames() const { return lr.size() / 2u; }
};

// The same as numbers of full scale 1.0, a channel each.
struct Signal {
  std::vector<double> l;
  std::vector<double> r;
  double rate = 44100.0;
  std::size_t size() const { return l.size(); }
  std::vector<double> mid() const;  // (L + R) / 2
};

// The part of `pcm` from `from_ms` to `to_ms` (to_ms < 0: to the end), clipped to what there is.
Signal to_signal(const Pcm& pcm, double from_ms = 0.0, double to_ms = -1.0);

// ---- the measurements (pure) -------------------------------------------------------------------------------------------------------------

inline constexpr double kLsb = 1.0 / 32768.0;
inline constexpr double kSilenceDb = -200.0;  // what a level of zero reads as
inline constexpr double kPanLimitDb = 100.0;  // pan_balance_db is held to +-this: a channel of exact zeros is a balance of 100 dB, not 200
inline constexpr std::size_t kZipperFrame = 512;  // count_zipper's frame and hop
inline constexpr std::size_t kZipperHop = 64;
inline constexpr std::size_t kZipperMinFrames = kZipperFrame + 2u * kZipperHop;  // a signal of fewer samples has no flux to count
inline constexpr std::size_t kAliasFrame = 8192;   // alias_db's frame: a signal of fewer samples cannot be measured

double db_of(double amplitude);                 // 20 log10, held to kSilenceDb
double rms_db(const Signal& s);                 // over both channels
double peak_db(const Signal& s);
double dc_offset(const Signal& s);              // the larger |mean| of the two channels
bool is_silent(const Signal& s);                // every |sample| <= 1 LSB
unsigned count_clicks(const std::vector<double>& x);  // one channel
unsigned count_clicks(const Signal& s);               // both channels
double onset_ms(const Signal& s, double floor_db = -50.0);  // -1: never reaches it
double pitch_hz(const std::vector<double>& x, double rate);  // 0: no periodic part
double centroid_hz(const std::vector<double>& x, double rate);  // 0: silence
double pan_balance_db(const Signal& s);          // 20 log10(rms L / rms R), held to +-kPanLimitDb
unsigned count_zipper(const std::vector<double>& x, double rate);
double alias_db(const std::vector<double>& x, double rate);  // NaN when there is nothing to measure: fewer than kAliasFrame samples, or silence
// OS-E2E-4 (the audio fingerprints, audio_fingerprint.h): the power of x[from, from + len) in `bands` bands spaced evenly in log frequency from lo_hz to hi_hz, each in dB re full scale as a mean square
// (a full-scale sine is -3.01 dB; the bands add up to the signal's power, the Hann leakage being inside a band), held to floor_db. A Hann window over the len samples, zero-padded to the next power of two
// of at least 8192 (5.4 Hz a bin at the engine's rate); a bin counts in the band whose [lo, hi) holds its centre frequency, DC and the bins outside lo_hz..hi_hz count in none. `from + len` is cut to the signal.
std::vector<double> band_levels_db(const std::vector<double>& x, std::size_t from, std::size_t len, double rate, unsigned bands, double lo_hz, double hi_hz, double floor_db);
// 20 log10(rms(a[i + lag] - b[i]) / rms(a)) over the frames both have (kSilenceDb for equal, +200 when a is silent and b is not; NaN when there is no frame to compare, or both are silent).
double null_db(const Signal& a, const Signal& b, std::size_t lag = 0);
Signal sum_signals(const std::vector<Signal>& parts);  // the channels added frame by frame, as long as the shortest
struct Adsr {
  double attack_ms = 0.0;
  double decay_ms = 0.0;
  double sustain = 1.0;
  double release_ms = 0.0;
  double off_ms = 0.0;  // the release starts this long after the onset
};
double adsr_level(const Adsr& m, double t_ms);  // the model at t ms after the onset (0..1)
// The largest difference between the measured peak envelope (divided by its maximum) and the model, over the curve; -1 when the signal never starts.
double envelope_error(const Signal& s, const Adsr& model);

// ---- the grammar -------------------------------------------------------------------------------------------------------------------------

struct AudioCheck {
  enum class Kind : std::uint8_t { Pitch, Rms, Peak, Dc, Silent, Sound, Clicks, Onset, Envelope, Centroid, Pan, Zipper, Alias, Null };
  enum class Cmp : std::uint8_t { Near, Less, Greater };  // =x±tol, <x, >x
  enum class PanKind : std::uint8_t { Left, Right, Centre, Db };
  Kind kind = Kind::Rms;
  Cmp cmp = Cmp::Near;
  // A value: a number, or `@ref` over another render, then `op` (0 none, '+', '-', '*') and `operand`.
  double value = 0.0;
  std::string ref;
  char op = 0;
  double operand = 0.0;
  double tol = 0.0;        // ±: cents (pitch), dB (rms, peak, pan), ms (onset), the fraction (envelope)
  Adsr model;              // envelope
  PanKind pan = PanKind::Centre;
  std::vector<std::string> others;  // null: the renders it is compared with (their sum)
  std::uint32_t lag = 0;   // null: frames this render is later than the others
  std::string text;        // the check as written, for the messages
};

struct AudioAssert {
  std::string name;
  double from_ms = 0.0;
  double to_ms = -1.0;     // < 0: to the end
  std::vector<AudioCheck> checks;
};

// The words after `assert audio`. false: `error` says what is wrong (no render name, an unknown check, a value that is not a number, a tolerance missing).
bool parse_audio_assert(const std::vector<std::string>& words, AudioAssert& out, std::string& error);

// Looks up a render by name (nullptr: there is none, or it has not ended).
using PcmLookup = std::function<const Pcm*(const std::string& name)>;

// "" when every check holds; else the first that does not, in words. `summary` (when not null) is what was measured, one phrase a check ("pitch 441.2 Hz, rms -9.1 dB"), for the run's log.
std::string audio_problem(const AudioAssert& a, const PcmLookup& lookup, std::string* summary = nullptr);

// ---- files -------------------------------------------------------------------------------------------------------------------------------

// A 16-bit stereo PCM WAV of `pcm` (RIFF header, one fmt chunk, one data chunk). The rate field is the nearest whole Hz.
std::vector<std::uint8_t> wav_bytes(const Pcm& pcm);
// Lower-case hex SHA-256 of the samples (the bytes of `lr`, little-endian): what --twice compares between two runs.
std::string pcm_sha256(const Pcm& pcm);

}  // namespace o3os::sim
