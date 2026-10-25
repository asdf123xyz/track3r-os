// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/formats/format.h>
#include <o3os/formats/wav.h>
#include <o3os/model/instrument.h>
#include <o3os/model/project.h>

#include <cstddef>
#include <cstdint>

namespace o3os::formats::wav {

// The WAV importer as a registered format (OS-002c, FormatKind::Sample): a FormatReader that reads the file into
// `ReadParams::pcm` (the sample arena slot) and makes the sample instrument `params.index` from what the file says:
//
//   name         the file's name without its extension (ReadParams::name), printable ASCII only, cut at 32 characters
//   sample path  `samples/instrNN.wav` (NN = slot + 1), as the OG import names it: the path is ours, nothing in the file can
//                point outside the project folder
//   sample rate  Instrument::sample_rate: the file's rate (native INST 1.1; 0 = 44,100, what an OG instrument has)
//   loop         the first `smpl` loop, as the OG's positions (0..65535 of the sample's length, rounded to nearest) in
//                loop_start / loop_end, and the play mode that loops it the way the file says: forward = 1, backward = 2,
//                ping-pong = 3 (the OG's play modes); a file without a loop stays one-shot. A loop that the 16-bit positions
//                cannot tell apart (shorter than 1/65535 of the sample) is dropped and counted
//   slices       the `cue` points, as positions of the same kind, ascending, unique after the rounding (slice_count,
//                selected slice 0); the play mode is not changed to a slice mode: that is the user's choice
//   start / end  the whole sample (0 and 65535), whatever the instrument was before: the instrument is made anew
//
// WAVETABLE FRAMES (OS-GRAN, BASE-04; docs/specs/OS-005-audio-engine.md): the model's wavetable is a stack of equal windows in a sample, the instrument's wavetable_window_size (32..2048, a power of
// two: the sampler has pitch tables for exactly those) and its sample type 1 and play mode 6; the number of windows is the length over the size (the engine plays whole windows only, a
// tail shorter than one is not played), so no field is added. A WAV is made a wavetable by the rule in ReadParams::wavetable: by default (kWtAuto) when it CARRIES the marker, Serum's 'clm ' chunk
// ("<!>2048 ...", the frame size: Info::wt_frame); a WAV without it is a sample, because most WAVs are and a length that happens to be a multiple of 256 says nothing. The user's override
// (kWtDetect: "load as wavetable") takes a plain WAV whose length is a multiple of 2048, 1024 or 256 (the largest that divides it) as the wavetable it is, or an explicit size from the load
// screen. A marker that names a size the engine does not have (4096, 1000...) leaves the file a sample and is counted in FormatReport::sanitised. A file that is no wavetable, or one the
// rule cannot place, stays exactly what it was before: a sample (the mapping above).
//
// The root note of a `smpl` chunk is read but not stored: the model has no field for it and the instrument's pitch is `tune`.
// (Open question for the maintainer.) Nothing is written to the model before the file has been read and checked, so a failed or
// cancelled import leaves the project and the slot as they were (and `pcm` as garbage). Memory: the importer, 1.1 KB, and about 90 bytes.
class SampleReader final : public FormatReader {
 public:
  void begin(core::ByteSource& source, model::ModelArena& arena, const ReadParams& params) noexcept override;
  formats::Progress step(std::size_t byte_budget) noexcept override;
  formats::Progress progress() const noexcept override { return progress_; }
  const FormatReport& report() const noexcept override { return report_; }
  unsigned permille() const noexcept override { return importer_.permille(); }
  bool can_cancel() const noexcept override { return true; }
  void cancel() noexcept override;

  // What the file said: rate, frames, loop, slices, root note, what was dithered or dropped.
  const Importer& importer() const noexcept { return importer_; }

 private:
  void fail(FormatError e, const char* detail) noexcept;
  void finish() noexcept;

  Importer importer_;
  FormatReport report_;
  formats::Progress progress_ = formats::Progress::Failed;
  model::Project* project_ = nullptr;
  std::uint16_t slot_ = 0;
  std::uint16_t wavetable_ = kWtAuto;  // ReadParams::wavetable of this run
  char name_[64] = {};  // the file's name, kept long enough that its extension is still in it when the name is cut at 32
};

// The instrument a file makes (the mapping above), for the tests and for a platform that wants it without the reader.
void make_sample_instrument(const wav::Info& info, std::uint32_t slot, const char* file_name,
                            model::Instrument& out, std::uint16_t wavetable = kWtAuto) noexcept;

// The wavetable frame size WAVETABLE FRAMES says for a file of `frames` frames whose marker said `marker` (0: none) under the request `wavetable` (ReadParams::wavetable): 32..2048, or 0 when
// the file is a sample. Pure. `unusable` (when not null) is set when the file CARRIED a marker (or was asked a size) that the engine cannot play, so that the reader can count it.
unsigned wavetable_frame(std::uint32_t frames, unsigned marker, std::uint16_t wavetable, bool* unusable = nullptr) noexcept;

// What a play-mode-6 wavetable instrument is as the model holds it (the fields only; the caller names it and gives it its sample): sample type 1, play mode 6, window size `frame`, window 0.
void make_wavetable_fields(model::SamplerParams& s, unsigned frame) noexcept;

}  // namespace o3os::formats::wav
