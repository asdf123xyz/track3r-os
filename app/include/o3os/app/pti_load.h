// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// An OG instrument file (.pti) read into one slot's worth of memory, as a core::Job (OS-INSTUI S7, LOAD; new Community OS behaviour, the file format is the OG's and unchanged). A .pti is the 392-byte
// instrument structure (versions 4 to 9; formats/og InstrumentReader, in its standalone mode: the instrument is decoded and checked, not put into the project) followed by the mono 16-bit
// little-endian PCM at 44,100 Hz. The job reads the head, then the PCM straight into the memory it was given (the sample arena's free tail, which the bank holds for it while its file is open:
// sample_bank.h open_wav), a slice of the main loop's pass at a time. It touches neither the project nor the bank: the caller, when the job is Done, puts the instrument in through the Editor (one
// undo unit) and the audio in through SampleBank::commit_load_undoable, and closes the file.
//
// A file that is short, not an OG instrument, of a version the reader does not take, stereo, longer than the room, or whose header is not a valid instrument fails with a reason in upper-case words and
// changes nothing.
//
// Contexts: main loop only. No heap. About 1.5 KB (the reader): the platform keeps it in SDRAM (SampleBankMem).

#include <o3os/app/og_pcm.h>
#include <o3os/core/byte_io.h>
#include <o3os/core/fixed_string.h>
#include <o3os/core/job.h>
#include <o3os/formats/og.h>
#include <o3os/model/project.h>

#include <cstddef>
#include <cstdint>

namespace o3os::app {

class SampleBank;

// OS-BUG-PTIRATE / OS-FILES-C C1b: what the standalone .pti save (Shift + F5) must do for the sample instrument in `slot` so that the file sounds like the instrument. A .pti has no rate field: this OS plays
// its PCM at 44,100 Hz (what an official device does with the file is an open research question, OS-FILES-C section 3), and reads its volume byte through its model of the 1.2+ law (formats/og). So
//   * an instrument that plays at another recorded rate (0 is "not recorded": 44,100) is WRITTEN RESAMPLED to 44,100 Hz (`plan`: og_pcm.h, the rate rule of docs/specs/OS-FILES-C.md section 3; maintainer
//     decision Q7, 2026-10-16). A wavetable instrument is exempt (its pitch is its window's and takes no rate factor: written as it is), a granular one is not (the rate applies: audio_granular_test);
//   * an instrument on the AMIGA engine (chosen, or by its Paula rate) plays its data at period 428, 8,287.14 Hz, at the unity note whatever the rate says: it is written resampled FROM 8,287.14 Hz
//     with its tune kept (Q5) and the fine tune the Amiga voice rounds to (`fine`); the engine itself, its 8-bit rounding and its period clamp are gone. Only the modes Paula plays (one shot, forward
//     loop) are served: any other mode of an Amiga instrument is `AmigaEngine`, refused (a backward or ping-pong loop, slices, a wavetable have no Amiga meaning to carry over);
//   * the level of a project that plays through the 1.1.14 law is converted (S0); it is refused when it has none:
// Refused (nothing is written):
//   AmigaEngine  an Amiga-engine instrument in a mode other than one shot or forward loop;
//   BitEngine    the engine is 12 BIT or 8 BIT (a cap on the depth of the Modern engine): a .pti has no engine field, so the depth cap would be lost in the file (default, to confirm by the maintainer);
//   TooLong      the resampled sample is longer than a .pti holds (kMaxPcmFrames): never cut (OS-FILES-C section 3, default);
//   LevelClamps  the project plays through the 1.1.14 level law (Project::volume_law() == 1) and the level cannot be written as a 1.2+ level of the same gain: below -24 dB (level < 40) the
//                1.2+ law has nothing quieter than level 1 (-24 dB), so the conversion would play louder than the instrument does (the clamp direction of docs/specs/OS-BUG-PASTE24.md D2).
// `level` is the volume byte to write: the model's, or the converted one under the 1.1.14 law (Ok only).
enum class PtiVerdict : std::uint8_t { Ok, AmigaEngine, LevelClamps, BitEngine, TooLong };
struct PtiCheck {
  PtiVerdict verdict = PtiVerdict::Ok;
  OgPcmPlan plan;                 // the PCM stage: the frames written, the step, identity (Ok only; Empty when the bank holds no audio)
  std::uint32_t source_hz = 0;    // the rate resampled from (0: not resampled), for the notice: the recorded rate, or 8,287 for the Amiga engine
  std::int16_t level = -1;        // Ok under law 1: the converted level; -1: write the model's
  std::int16_t fine = formats::WriteParams::kNoFineOverride;  // Ok on the Amiga engine: the fine tune the file carries; else the model's
};
PtiCheck pti_check(const model::Project& project, const SampleBank& bank, unsigned slot, const model::Instrument& in) noexcept;
// The 1.2+ level of the same gain as the 1.1.14 level 0..100 (0.4 dB a step: -39.6..0 dB) in the 1.2+ law (0.48 dB a step around 50 = 0 dB; level 1 = -24 dB): rounded to the nearest step (at most
// 0.24 dB off, 0.48 dB at the bottom where the law's level 1 is -24.00 and level 2 -23.04), 0 stays 0. `clamped` is set, and 1 returned, for a level whose gain is under -24 dB (below 40).
std::uint8_t level_1114_to_12(unsigned level, bool& clamped) noexcept;

class PtiLoadJob final : public core::Job {
 public:
  static constexpr std::uint32_t kBytesPerUs = 4;  // as ImportJob's (4 MB/s of SD, provisional)
  static constexpr std::size_t kMinStepBytes = 512;
  static constexpr std::size_t kMaxStepBytes = 65536;

  PtiLoadJob() = default;

  // Reads `source` (the file, at its first byte; `file_size` bytes) for instrument slot `slot` (0..63; it only names the sample path the instrument gets) into `pcm` (`capacity` samples).
  // `arena` is the reader's project (not changed). `label` is the file name for the strip ("KICK.PTI"). Everything must stay valid until the job has ended.
  void begin(core::ByteSource& source, model::ModelArena& arena, std::uint32_t file_size, std::int16_t* pcm, std::uint32_t capacity, unsigned slot, const char* label) noexcept;

  core::JobStatus step(std::uint32_t budget_us) noexcept override;
  unsigned progress() const noexcept override;
  void cancel() noexcept override { phase_ = Phase::Failed; why_ = "CANCELLED"; }
  const char* name() const noexcept override { return name_.c_str(); }
  const char* detail() const noexcept override { return phase_ == Phase::Failed ? why_ : ""; }

  // After Done: the instrument the file describes (sample path "samples/instrNN.wav" for the slot) and the frames now at the `pcm` given to begin().
  const model::Instrument& instrument() const noexcept { return reader_.instrument(); }
  std::uint32_t frames() const noexcept { return frames_; }
  bool crc_mismatch() const noexcept { return reader_.report().crc_mismatch; }

 private:
  enum class Phase : std::uint8_t { Idle, Head, Pcm, Done, Failed };

  formats::og::InstrumentReader reader_{true};
  core::ByteSource* source_ = nullptr;
  std::uint8_t* dst_ = nullptr;
  std::uint32_t capacity_ = 0;
  std::uint32_t file_size_ = 0;
  std::uint32_t frames_ = 0;
  std::size_t got_ = 0;
  Phase phase_ = Phase::Idle;
  const char* why_ = "";
  core::FixedString<40> name_;
};

}  // namespace o3os::app
