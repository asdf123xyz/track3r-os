// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The first seconds of a WAV of the card as a core::Job (OS-FU2, the Sample load audition; new Community OS behaviour: the file plays
// while the key is held). It owns the file it opens: the WAV importer (formats/wav) reads it, a slice of the main loop's pass at a
// time, into a caller-given buffer (the audition's SDRAM buffer), clipped to the buffer (Importer::begin `clip`) and downmixed to mono. Nothing of the project is touched: the
// audio is no instrument. The file is closed when the job is done, failed or cancelled (and by the destructor).
//
// Contexts: main loop only. No heap. The importer is about 1.1 KB: the platform keeps the job in SDRAM (SampleBankMem).

#include <o3os/app/og_folder.h>
#include <o3os/core/job.h>
#include <o3os/formats/wav.h>
#include <o3os/hal/storage.h>

#include <cstdint>

namespace o3os::app {

class PreviewJob final : public core::Job {
 public:
  // Source bytes per microsecond of budget: as ImportJob's (4 MB/s of SD, provisional).
  static constexpr std::uint32_t kBytesPerUs = 4;
  static constexpr std::size_t kMinStepBytes = 512;
  static constexpr std::size_t kMaxStepBytes = 65536;

  PreviewJob() = default;
  ~PreviewJob() { close(); }

  // Opens `path` and starts reading into `pcm` (`capacity` int16 values). The storage's answer when the file does not open; Ok starts the job (the caller runs it).
  hal::storage::Status begin(hal::storage::Storage& storage, const char* path, std::int16_t* pcm, std::uint32_t capacity, std::uint8_t channel = 0) noexcept;  // channel (OS-LOADCH): of a stereo file, 0 mix, 1 left, 2 right

  core::JobStatus step(std::uint32_t budget_us) noexcept override;
  unsigned progress() const noexcept override { return done_ ? core::kJobProgressMax : importer_.permille(); }
  void cancel() noexcept override {
    importer_.cancel();
    close();
  }
  const char* name() const noexcept override { return "PREVIEW"; }
  // After Failed: why, in upper-case words.
  const char* detail() const noexcept override;

  // After Done: the frames read (mono) and the file's rate.
  std::uint32_t frames() const noexcept { return importer_.info().frames; }
  std::uint32_t sample_rate() const noexcept { return importer_.info().sample_rate; }
  // While it runs (OS-BUG-PREVLAT, the streamed start): the frames the buffer will hold (0 until the data header is read; the buffer's size caps it) and the whole frames written so far, from 0.
  // Main loop only, between two steps: the job is the buffer's one writer, it writes at `written()` on and never behind it.
  std::uint32_t planned() const noexcept { return importer_.planned_frames(); }
  std::uint32_t written() const noexcept { return importer_.info().frames; }

 private:
  void close() noexcept {
    if (storage_ != nullptr && file_.is_open()) {
      storage_->close(file_);
    }
    file_ = hal::storage::FileHandle{};
    source_.reset();
  }

  formats::wav::Importer importer_;
  StorageSource source_;
  hal::storage::Storage* storage_ = nullptr;
  hal::storage::FileHandle file_{};
  bool done_ = false;
};

// The tune (semitones) and fine tune (-100..100, SampleDesc units: linear in pitch between the neighbouring semitones, tuned_inc_q32) that make the voices play a sample recorded at
// `rate_hz` as it sounds, the engine reading every sample at 44,100 Hz (the importer keeps the file's rate and resamples nothing). 0 Hz or 44,100 Hz gives 0 and 0; the shift is held
// to four octaves. Main loop (single-precision libm).
void preview_tuning(std::uint32_t rate_hz, std::int8_t& tune, std::int8_t& fine) noexcept;

}  // namespace o3os::app
