// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The pure parts of "export a project in the OG format" (OS-FILES part C, slices C1a and C1b, docs/specs/OS-FILES-C.md): no UI, no card write of its own. C1a: three units and the arithmetic they share:
//
//   1. THE FILE NAME of an instrument's .pti inside an OG folder: "NN <stem>" (NN = the slot + 1, two digits), at most 31 bytes in all and a name the card guard takes (og_pti_stem).
//   2. THE PLAN of a sample's PCM: how many 44,100 Hz frames it becomes, the resampling step, and whether it fits a .pti (og_pcm_plan for a Modern source, og_pcm_plan_amiga for an instrument on
//      the Amiga engine, whose data is read at Paula's 8,287.14 Hz whatever its recorded rate says). The loop of the instrument in frames (og_loop_of).
//   3. THE RESAMPLER with the edges done right (og_resample): the band-limited kernel of amiga_convert.h, but the source beyond its ends is the sample's own continuation, not silence: an
//      edge-extended one-shot, a wrapped (or reflected) loop, so that neither the first and last 32 frames fade nor a loop's seam breaks in the new rate.
//
// C1b wires the plan, the resampler and the Amiga fine tune into the standalone .pti save (SaveService::save_instrument through pti_load.h pti_check) with OgResampleSource, the resampler as a byte
// source for the OG instrument writer. What is NOT here: the level map of the 1.1.14 law (pti_load.h level_1114_to_12, S0), the whole-project file plan (C2), the card, the UI.
//
// No heap, no libm, no I/O: callable from a main-loop slice (og_resample does a bounded number of frames a call, like amiga_resample).

#include <o3os/app/amiga_convert.h>
#include <o3os/core/byte_io.h>
#include <o3os/formats/og.h>
#include <o3os/model/instrument.h>

#include <cstddef>
#include <cstdint>

namespace o3os::app {

inline constexpr std::uint32_t kOgRateHz = 44100;                                  // the rate of a .pti's PCM in an OG folder (docs/formats/og.md section 5, OS-FILES-C section 3)
inline constexpr std::uint32_t kOgMaxFrames = formats::og::kMaxPcmFrames;         // 5,898,240: one .pti may fill the whole sample memory
inline constexpr std::size_t kOgPtiStemMax = 31;                                   // the card guard's name limit (hal/save_guard.h kNameMax)

// ---- 1. the name -------------------------------------------------------------------------------------------------------------------------------

// "NN <stem>" for the sample instrument in `slot` (0..47: the OG's 48 sample slots) named `name`, into out[0..cap), NUL terminated; the length, or 0 when `slot` is out of range or `cap` is under 32.
// The stem is SaveService::instrument_file_stem's (letters, digits, space, '_', '-', '.': any other byte, UTF-8 included, becomes '_'; a name the guard refuses, CON and the like, gets a '_'; nothing
// left: "Instrument NN"), cut to 28 bytes so that "NN " plus it is at most 31, with no trailing space or dot after the cut. The whole is a name hal/save_guard.h valid_name takes, and the
// "NN " prefix keeps two instruments of one name apart. The file is that plus ".pti".
std::size_t og_pti_stem(unsigned slot, const char* name, char* out, std::size_t cap) noexcept;

// ---- 2. the plan ---------------------------------------------------------------------------------------------------------------------------------

enum class OgPcmStatus : std::uint8_t {
  Ok,
  Empty,    // no frames in
  TooLong,  // more than kOgMaxFrames out: that instrument is not written (OS-FILES-C section 3, default to confirm by the maintainer), never cut
};
struct OgPcmPlan {
  OgPcmStatus status = OgPcmStatus::Empty;
  std::uint32_t frames_out = 0;   // ceil(frames x 44,100 / source rate); exact (frames_in) for 44,100 Hz or no recorded rate
  std::uint64_t step_q32 = 0;     // source frames per output frame, Q32.32 (1.0 for the identity)
  bool identity = false;          // 44,100 Hz or none recorded: the PCM is written as it is, no resampler
};
// A Modern-engine (or 12 / 8 bit) sample recorded at `rate_hz` (0: not recorded = 44,100). The rate is held to the engine's own range, 1,000..384,000 Hz (audio::rate_factor_q28), as the voices do.
OgPcmPlan og_pcm_plan(std::uint32_t frames_in, std::uint32_t rate_hz) noexcept;
// An instrument on the Amiga engine: the voice reads the data at period 428 = 3,546,895 / 428 = 8,287.14 Hz at the unity note whatever `sample_rate` says (rate 0 included) and ADDS the
// instrument's tune to the note, so the data is resampled from 8,287.14 Hz and the tune is kept: the pitch is the same at every note (the +12 of a 16,574 Hz conversion needs no special case).
OgPcmPlan og_pcm_plan_amiga(std::uint32_t frames_in) noexcept;
// The fine tune (-100..100 hundredths of a semitone) a Modern note must carry to play where the Amiga voice plays it: the Amiga voice rounds it to a MED step, 1/8 of a semitone
// (12.5 hundredths), held to -8..7 steps (audio/src/amiga_voices.cpp), and the OG instrument file's fine tune is a whole number of hundredths: steps x 12.5, halves away from zero.
std::int8_t og_amiga_fine(int fine) noexcept;

// The loop of a sample instrument in whole-sample frames (the points are floor(point x (frames / 65535.0f)) in single precision, the arithmetic of the voices' binding, og_samples.cpp at_point, so
// that the frames and the active decision (a two-frame loop) are the voices' own; og_point_frame below is the exact integer form, which can differ by a frame on a long sample): `active` as og_samples.cpp bound_loop decides (a loop play mode, start < end,
// the loop inside [start, end] and at least 2 frames). The window [start, end) is the instrument's own; only the loop's edges matter to the resampler.
struct OgLoop {
  bool active = false;
  std::uint8_t mode = 0;          // the play mode: 1 forward, 2 backward, 3 ping-pong
  std::uint32_t start = 0;        // LOOP START in frames
  std::uint32_t end = 0;          // LOOP END in frames (exclusive)
};
OgLoop og_loop_of(const model::Instrument& in, std::uint32_t frames) noexcept;
// A point (0..65535) in a sample of `frames`, as the voices read it: floor(point x frames / 65535). Points are fractions of the length, so they are the same numbers in the resampled sample;
// the frame they name moves by under one output frame. This is the source frame of a point and the output frame of the same point.
std::uint32_t og_point_frame(std::uint32_t point, std::uint32_t frames) noexcept;

// ---- 3. the resampler ----------------------------------------------------------------------------------------------------------------------------

// Output frames [from, from + n) of `src` (`frames` long) resampled by `step_q32` source frames per output frame into dst[0..n). `from + n` is held to `frames_out` of the plan: the caller passes
// the plan's frames_out as `total`. The kernel must be ready() (AmigaKernel::build in slices, as the Amiga conversion does). Source frames outside [0, frames) are the sample's continuation:
//   no loop           the first and last frame repeated (edge-extended): nothing fades, an impulse at frame 0 keeps its level;
//   forward, backward past `loop.end`: the loop's start onward, wrapping; before `loop.start`, for an output frame whose centre is at or past the loop's start, the loop's tail (so a loop that
//                     starts at frame 0, a single cycle, has no click at its wrap); an output frame centred before the loop's start keeps the real pre-loop data;
//                     ping-pong: the loop reflected about its end and about its start;
//   the real tail      an output frame centred at or past `loop.end` (the part between the loop's end and the window's end, which a loop never plays) reads the real data, never the wrapped loop:
//                     the file keeps its tail as it was;
//   before 0          the first frame.
// The output loop's length is the loop's length times the ratio rounded twice (once for each of its two points), so it can differ from the ideal by about one output frame: the wrap is exact in
// the SOURCE and within that frame in the new rate (a seam of at most about one frame, not "to the half frame").
// The weights are normalised by their own sum (DC exact), the result is rounded to the nearest and saturated to 16 bits; `clipped`, when given, is incremented for every output that was
// saturated (a full-scale square rings past full scale at its edges). Returns the number of frames written.
std::uint32_t og_resample(const AmigaKernel& kernel, const std::int16_t* src, std::uint32_t frames, std::uint64_t step_q32, std::uint32_t total, const OgLoop& loop, std::int16_t* dst,
                          std::uint32_t from, std::uint32_t n, std::uint32_t* clipped = nullptr) noexcept;

// Output frames of one slice of the resampler (C1b): about kOgTapsPerStep taps (2 x 32 x max(ratio, 1) a frame), so a slice is a few hundred frames at most at a ratio under 1 and about 24 at 5.3.
// PROVISIONAL: the cost of a tap in og_resample (the bounds and loop tests of the fetch) is to be measured on the unit (DWT) before this is final; amiga_frames_per_budget's 14 cycles is the
// Amiga conversion's.
inline constexpr std::uint32_t kOgTapsPerStep = 8192;
inline constexpr std::uint32_t kOgMaxFramesPerStep = 256;
std::uint32_t og_frames_per_step(std::uint64_t step_q32) noexcept;

// The PCM of a .pti as a byte source: the sample resampled by `step_q32` to `total` frames of 16-bit little-endian mono, a slice at a time (C1b: the standalone .pti save). The kernel is built a few
// hundred entries a read first (read() says WouldBlock meanwhile), then each read returns up to the frames the slice allows and WouldBlock once they are spent: arm() opens the next slice, called by
// the owner before each step of the writer that pulls from it. No heap, no libm in the steps after the kernel.
class OgResampleSource final : public core::ByteSource {
 public:
  static constexpr unsigned kKernelPerStep = 256;  // kernel entries built a read (a sinf and a Bessel series each, as SampleEditJob)
  // `kernel`, `src` and `loop` must stay valid and unchanged until the source has ended. `total` is the plan's frames_out.
  void set(AmigaKernel& kernel, const std::int16_t* src, std::uint32_t frames, std::uint64_t step_q32, std::uint32_t total, const OgLoop& loop) noexcept;
  void arm() noexcept { left_ = og_frames_per_step(step_q32_); }
  core::IoResult read(void* dst, std::size_t max) noexcept override;
  std::uint32_t done() const noexcept { return pos_; }
  std::uint32_t clipped() const noexcept { return clipped_; }  // outputs saturated to 16 bits (a full-scale square rings past full scale at its edges)

 private:
  AmigaKernel* kernel_ = nullptr;
  const std::int16_t* src_ = nullptr;
  std::uint32_t frames_ = 0;
  std::uint64_t step_q32_ = 0;
  std::uint32_t total_ = 0;
  OgLoop loop_;
  std::uint32_t pos_ = 0;      // output frames made
  std::uint32_t left_ = 0;     // frames the current slice may still make
  std::uint32_t clipped_ = 0;
};

// (C2b: the whole-project export applies the same PCM stage and level conversion per instrument: og_export.h og_instrument_plan.)

}  // namespace o3os::app
