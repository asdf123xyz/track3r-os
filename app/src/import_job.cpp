// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/import_job.h>

#include <o3os/formats/iff_sample.h>
#include <o3os/formats/wav.h>
#include <o3os/model/limits.h>

#include <cstring>

namespace o3os::app {

static_assert(formats::wav::kMaxSlices == model::kSlices, "the importer keeps as many cue points as an instrument has slices");

void ImportJob::begin(formats::FormatReader& reader, core::ByteSource& source, model::ModelArena& arena, const char* label,
                      const formats::ReadParams& params) noexcept {
  name_.assign("IMPORT ");
  name_.append(label != nullptr ? label : "");
  reader_ = &reader;
  size_hint_ = params.size_hint;
  reader.begin(source, arena, params);
}

core::JobStatus ImportJob::step(std::uint32_t budget_us) noexcept {
  if (reader_ == nullptr) {
    return core::JobStatus::Failed;
  }
  // (the budget is bounded before it is multiplied: a 32-bit size_t must not wrap)
  std::size_t bytes = budget_us > kMaxStepBytes / kBytesPerUs ? kMaxStepBytes : static_cast<std::size_t>(budget_us) * kBytesPerUs;
  bytes = bytes < kMinStepBytes ? kMinStepBytes : bytes;
  switch (reader_->step(bytes)) {
    case formats::Progress::Running:
      return core::JobStatus::Running;
    case formats::Progress::Done:
      return core::JobStatus::Done;
    case formats::Progress::Failed:
      break;
  }
  return core::JobStatus::Failed;
}

unsigned ImportJob::progress() const noexcept {
  if (reader_ == nullptr) {
    return 0;
  }
  if (reader_->progress() == formats::Progress::Done) {
    return core::kJobProgressMax;
  }
  unsigned p = reader_->permille();  // a reader that knows says so (the WAV reader: the share of the audio converted)
  if (p == 0 && size_hint_ != 0) {   // else the bytes read of the size the platform told
    p = core::permille_of(reader_->report().bytes, size_hint_);
  }
  return p >= core::kJobProgressMax ? core::kJobProgressMax - 1u : p;
}

const char* import_failure_text(const formats::FormatReport& report) noexcept {
  switch (report.error) {
    case formats::FormatError::None: return "";
    case formats::FormatError::Truncated: return "TRUNCATED";
    case formats::FormatError::BadMagic: return "UNKNOWN FILE TYPE";
    case formats::FormatError::BadCrc: return "BAD CHECKSUM";
    case formats::FormatError::UnsupportedVersion: return "NEWER VERSION";
    case formats::FormatError::Unsupported: return "UNSUPPORTED FORMAT";
    case formats::FormatError::LimitExceeded: return "TOO BIG FOR MEMORY";
    case formats::FormatError::Malformed:
      // OS-LOADCH: an AIFF whose SSND comes before its COMM is not damaged, it is a known limit of the streamed reader; say so
      return std::strcmp(report.detail, formats::iff::kSsndBeforeComm) == 0 ? kSsndBeforeCommText : "DAMAGED FILE";
    case formats::FormatError::SourceFailed: return "READ ERROR";
    case formats::FormatError::SinkFailed: return "WRITE ERROR";
    case formats::FormatError::Modified: return "CHANGED WHILE WRITING";
    case formats::FormatError::Internal: return "INTERNAL ERROR";
    case formats::FormatError::Cancelled: return "CANCELLED";
  }
  return "";
}

const char* ImportJob::detail() const noexcept { return reader_ == nullptr ? "" : import_failure_text(reader_->report()); }

}  // namespace o3os::app
