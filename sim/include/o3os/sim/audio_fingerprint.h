// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Audio fingerprints (OS-E2E-4, docs/specs/OS-E2E.md): "a sound that passed has not changed without a reason". A scenario opts a render in with the line
//
//   fingerprint <render> [nof0]  (after the `render` or `wav` that makes it; a stems render's parts are `<name>.t1` .. `<name>.t8` and `<name>.m`)
//
// `nof0` is for a render with no steady fundamental: several notes at once (a master of four tracks: YIN answers a pitch between the notes, which a small change flips), or a voice whose pitch the
// estimator cannot hold to 10 cents (a resonant TB-303 line). The print keeps no f0 track for it (`"nof0": 1` in the file, no `f0_hz`); the pitch of a journey is asserted with `assert audio ... pitch=`.
//
// and o3sim keeps the compact description of that render in tests/audio_fingerprints/<scenario>.json (the key is the scenario's file name without its extension; one file holds every render of
// the scenario that opted in, by name). The description is made of the same measurements `assert audio` uses (audio_check.h), by frames of 100 ms of the render, from its start (a last frame of
// at least 50 ms is kept; a shorter tail is not): 24 band levels, the fundamental, the peak, the RMS level, the stereo balance, the DC offset and the click count, and the onsets of the whole render.
// SMOOTH FRAMES: the band levels, the peak, the RMS, the balance and the DC of a frame are those of a Hann window of 200 ms centred on the frame (so the windows overlap by half; the render is silence outside
// its ends). With a plain 100 ms cut, a note that a change moves by a millisecond across a frame boundary would take a frame from silence to sound and fail it, whatever the 1 ms the onset rule allows.
// Every number is stored rounded to what the file writes (fingerprint_render does the rounding), so a print equals its own re-read, and the text of a print is the same on every run: o3sim --twice
// compares it. The values are floats of one machine and compiler; the comparison has tolerances and is never a hash.
//
//   bands_db   24 bands spaced evenly in log frequency from 30 Hz to 20 kHz (a ratio of 1.31 a band: 4.7 semitones), each the power of the window's mid channel ((L + R) / 2) as a mean square in dB
//              re full scale (audio_check.h band_levels_db: a full-scale sine is -3.01 dB), 0.1 dB, held to -90 dB. The floor keeps the 16-bit rounding noise (-98 dB over the whole spectrum) and the
//              engine's exact zeros out of the distance: two bands under it are equal.
//   f0_hz      the fundamental of the frame's mid channel, 0.01 Hz: YIN refined on the spectrum (pitch_hz, 50 Hz to 5 kHz) on each half of the frame, the geometric mean of the two when they agree to
//              20 cents; 0 when the frame is under -70 dB RMS, has no periodic part (noise, a chord YIN cannot settle on, silence) or changes note in the middle (steady_f0 in the .cpp says why).
//   peak_db, rms_db   the largest |sample| of either channel (each weighted by the window) and the level over both (the window's weighting of the squares), dB re full scale, 0.1 dB, held to -120 dB.
//   pan_db     20 log10(rms L / rms R) of the window, held to +-40 dB, 0.1 dB; 0 for a frame under -70 dB RMS. The mid channel cannot see a swap of the channels: this does.
//   dc_lsb     the larger |window-weighted mean| of the two channels in 16-bit steps, 0.1. Over 100 ms the mean of a tone is its last partial period's: A / (2 pi n) with n periods in the frame (60 steps
//              for a half-scale 441 Hz tone, 240 for 110 Hz), so a moved note moves it; only an offset (a real DC bug is some 1000 steps) is a failure.
//   clicks     count_clicks over the frame's samples alone (its windows are cut at the frame's edges: an edge is judged by the frame). A sawtooth's wraps count as clicks, and their number moves with
//              the note's phase.
//   onsets_ms  where a sound starts, in ms from the start of the render, 0.01 ms: a peak follower of both channels (the largest |sample|, decaying with a time constant of 10 ms) that rises through
//              -50 dB re full scale and at least 4 times (12 dB) its lowest value since it was last armed; it is armed again when it falls under a quarter of the peak it reached since the onset, so a
//              note starts on a render of silence, after a gap and after a decay of 12 dB, and a retrigger that has not fallen that far is not an onset (a legato note, a note over a tail: the pro
//              session's sample track changes note with a dip of 1 dB, and has one onset). At most 64.
//
// The fingerprint of a render FAILS against the stored one when:
//   - the spectral distance of a frame (the root of the mean square of the 24 band differences, in dB) is above 1.5 dB at its maximum over the frames, or above 0.5 dB on average. A band more than
//     50 dB under the loudest band of either frame counts as being at that level (compare_fingerprints): the skirt of a window and the splash of an attack are 40 to 80 dB under a note, and a note
//     that moves by a millisecond moves them by tens of dB, which is masked by the note;
//   - the number of onsets is not the same, or one moves by more than 1 ms;
//   - the f0 of a frame moves by more than 10 cents, or a frame that had a fundamental has none (or the reverse);
//   - the number of frames is not the same.
// And, additions to the spec's rules (the spectrum does not see these): the peak, the RMS or the balance of a frame moves by more than 1 dB, the DC by more than 330 steps (1 % of full scale), or the click
// count of a frame by more than a quarter of the larger of the two (a clean frame must stay clean).
//
// A stored fingerprint is changed only by `update_audio_fingerprints` (tools/update_audio_fingerprints.py), which writes fingerprint_diff.md and the WAVs to listen to, and the commit that changes it
// must also change a firmware source directory (app/, audio/, core/, model/, seq/, formats/, modules/, module/), the scenario or the rig, and say why in a body of two lines at least
// (tools/check_fingerprint_guard.py, ctest fingerprint_guard).

#include <o3os/sim/audio_check.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace o3os::sim {

inline constexpr int kFpVersion = 1;            // the file's "version": another version is rejected ("run update_audio_fingerprints")
inline constexpr unsigned kFpBands = 24;
inline constexpr double kFpFrameMs = 100.0;
inline constexpr double kFpTailMs = 50.0;       // a last frame shorter than this is dropped
inline constexpr double kFpBandLoHz = 30.0;
inline constexpr double kFpBandHiHz = 20000.0;
inline constexpr double kFpBandFloorDb = -90.0;
inline constexpr double kFpLevelFloorDb = -120.0;
inline constexpr double kFpQuietDb = -70.0;     // a frame under this RMS has no f0 and no balance
inline constexpr double kFpPanLimitDb = 40.0;
inline constexpr std::size_t kFpMaxOnsets = 64;

// The rules of the comparison (see above).
inline constexpr double kFpBandRangeDb = 50.0;  // in the distance, a band this far under the loudest band of either frame counts as being at that level
inline constexpr double kFpLsdMaxDb = 1.5;
inline constexpr double kFpLsdMeanDb = 0.5;
inline constexpr double kFpOnsetMs = 1.0;
inline constexpr double kFpF0Cents = 10.0;
inline constexpr double kFpLevelDb = 1.0;       // peak, RMS and balance
inline constexpr double kFpDcLsb = 330.0;       // 1 % of full scale

struct FpFrame {
  std::array<double, kFpBands> bands{};
  double f0 = 0.0;
  double peak_db = kFpLevelFloorDb;
  double rms_db = kFpLevelFloorDb;
  double pan_db = 0.0;
  double dc_lsb = 0.0;
  unsigned clicks = 0;
};

struct FpRender {
  std::vector<FpFrame> frames;
  std::vector<double> onsets_ms;
  bool nof0 = false;  // no f0 track (f0 of every frame is 0 and is not compared)
};

using FpSet = std::map<std::string, FpRender>;  // by render name

// The fingerprint of a render (every value rounded to what the file stores).
FpRender fingerprint_render(const Pcm& pcm, bool nof0 = false);

// The band [lo, hi) in Hz.
void fp_band_edges(unsigned band, double& lo_hz, double& hi_hz);

// The file's text: the same bytes for the same prints. One line a frame.
std::string fingerprint_json(const std::string& key, const FpSet& set);
// false: `error` says what is wrong (not JSON, another version or key, a missing field, a count that does not fit).
bool parse_fingerprint(const std::string& text, const std::string& key, FpSet& set, std::string& error);

struct FpCompare {
  std::vector<std::string> failures;  // each one a line; empty: the fingerprint holds
  bool identical = true;              // every value the same, not only within the rules
  std::string markdown;               // what moved, for fingerprint_diff.md (a render that did not move is one line)
};
// `want` is the stored set, `got` the fresh one. A render in only one of them is a failure.
FpCompare compare_fingerprints(const std::string& key, const FpSet& want, const FpSet& got);

}  // namespace o3os::sim
