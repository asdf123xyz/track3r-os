// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/preview_job.h>

#include <o3os/core/fast_math.h>


namespace o3os::app {

hal::storage::Status PreviewJob::begin(hal::storage::Storage& storage, const char* path, std::int16_t* pcm, std::uint32_t capacity, std::uint8_t channel) noexcept {
  close();
  done_ = false;
  hal::storage::FileHandle file;
  const hal::storage::Status st = storage.open(path, file);
  if (st != hal::storage::Status::Ok) {
    return st;
  }
  storage_ = &storage;
  file_ = file;
  source_.reset();
  source_.attach(storage, file);
  importer_.begin(source_, pcm, capacity, false, true, channel);
  return hal::storage::Status::Ok;
}

core::JobStatus PreviewJob::step(std::uint32_t budget_us) noexcept {
  std::size_t bytes = budget_us > kMaxStepBytes / kBytesPerUs ? kMaxStepBytes : static_cast<std::size_t>(budget_us) * kBytesPerUs;
  bytes = bytes < kMinStepBytes ? kMinStepBytes : bytes;
  switch (importer_.step(bytes)) {
    case formats::wav::Progress::Running:
      return core::JobStatus::Running;
    case formats::wav::Progress::Done:
      done_ = true;
      close();
      return core::JobStatus::Done;
    case formats::wav::Progress::Failed:
      break;
  }
  close();
  return core::JobStatus::Failed;
}

const char* PreviewJob::detail() const noexcept {
  switch (importer_.report().error) {
    case formats::wav::Error::None: return "";
    case formats::wav::Error::Truncated: return "TRUNCATED";
    case formats::wav::Error::BadMagic: return "UNKNOWN FILE TYPE";
    case formats::wav::Error::Unsupported: return "UNSUPPORTED FORMAT";
    case formats::wav::Error::LimitExceeded: return "TOO BIG FOR MEMORY";
    case formats::wav::Error::Malformed: return "DAMAGED FILE";
    case formats::wav::Error::SourceFailed: return "READ ERROR";
    case formats::wav::Error::Cancelled: return "CANCELLED";
  }
  return "";
}

void preview_tuning(std::uint32_t rate_hz, std::int8_t& tune, std::int8_t& fine) noexcept {
  tune = 0;
  fine = 0;
  if (rate_hz == 0u || rate_hz == 44100u) {
    return;
  }
  float s = 12.0f * core::log2_fast(static_cast<float>(rate_hz) / 44100.0f);
  s = s < -48.0f ? -48.0f : (s > 48.0f ? 48.0f : s);
  const int n = static_cast<int>(static_cast<long>(s < 0.0f ? s - 0.5f : s + 0.5f));
  const float target = core::exp2_fast(s / 12.0f);
  const float base = core::exp2_fast(static_cast<float>(n) / 12.0f);
  const bool up = target >= base;
  const float other = core::exp2_fast(static_cast<float>(up ? n + 1 : n - 1) / 12.0f);
  const float f = (up ? target - base : base - target) / (up ? other - base : base - other);
  int fine_n = static_cast<int>(static_cast<long>(f * 100.0f + 0.5f));
  fine_n = up ? fine_n : -fine_n;
  tune = static_cast<std::int8_t>(n);
  fine = static_cast<std::int8_t>(fine_n > 100 ? 100 : (fine_n < -100 ? -100 : fine_n));
}

}  // namespace o3os::app
