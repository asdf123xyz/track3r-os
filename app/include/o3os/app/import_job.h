// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The first real job (OS-002c): any registered reader (formats::FormatReader: the WAV importer, the OG files, the native project)
// as a core::Job. It turns the microsecond budget of a pass of the main loop into the number of source bytes one step() of the
// reader may consume, which is all the readers' own resumable shape needs; progress, failure text and cancel pass straight
// through. A reader that changes the project before it is done (everything but the WAV reader) is not cancellable: an abandoned
// pattern or project would be half applied.

#include <o3os/core/byte_io.h>
#include <o3os/core/fixed_string.h>
#include <o3os/core/job.h>
#include <o3os/formats/format.h>
#include <o3os/model/project.h>

#include <cstddef>
#include <cstdint>

namespace o3os::app {

// OS-LOADCH: the reason an AIFF with its SSND chunk before COMM fails with. The strip shows it alone (App::report_jobs): after the file name it would be cut to nothing.
inline constexpr char kSsndBeforeCommText[] = "AIFF: SSND BEFORE COMM NOT SUPPORTED";

// The reason a reader failed, in upper-case words ("TRUNCATED", "UNSUPPORTED FORMAT"); "" when it did not (OS-FILES: the module import words its failures the same way).
const char* import_failure_text(const formats::FormatReport& report) noexcept;

class ImportJob final : public core::Job {
 public:
  // The cost model: source bytes the reader moves per microsecond of budget. The SD card behind the file system reads at a
  // few MB/s, and the conversions are cheaper than that, so the card is what a step costs: 4 bytes/us is about 4 MB/s.
  // PROVISIONAL until the platform has a real source (hw-session item); the runner measures the real time of every step
  // (JobStats::max_step_us, overruns), which is what to tune it by.
  static constexpr std::uint32_t kBytesPerUs = 4;
  static constexpr std::size_t kMinStepBytes = 512;  // a step always moves something, however small the slice
  static constexpr std::size_t kMaxStepBytes = 65536;

  // Starts `reader` on `source` into `arena` with `params`. `label` is the file name shown in the notification ("KICK.WAV").
  // Everything must stay valid until the job has finished.
  void begin(formats::FormatReader& reader, core::ByteSource& source, model::ModelArena& arena, const char* label,
             const formats::ReadParams& params) noexcept;

  core::JobStatus step(std::uint32_t budget_us) noexcept override;
  unsigned progress() const noexcept override;
  bool cancellable() const noexcept override { return reader_ != nullptr && reader_->can_cancel(); }
  void cancel() noexcept override {
    if (reader_ != nullptr) {
      reader_->cancel();
    }
  }
  const char* name() const noexcept override { return name_.c_str(); }
  // After Failed: the reason in upper-case words ("TRUNCATED", "UNSUPPORTED FORMAT").
  const char* detail() const noexcept override;

  // The result: the reader's report (frames and channels of a sample, instruments, patterns, what was dropped).
  const formats::FormatReader* reader() const noexcept { return reader_; }
  const formats::FormatReport* report() const noexcept { return reader_ != nullptr ? &reader_->report() : nullptr; }

 private:
  formats::FormatReader* reader_ = nullptr;
  std::uint32_t size_hint_ = 0;
  core::FixedString<40> name_;
};

}  // namespace o3os::app
