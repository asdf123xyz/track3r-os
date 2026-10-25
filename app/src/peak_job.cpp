// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/peak_job.h>

namespace o3os::app {

bool PeakJob::begin(ui::plot::PeakCache& cache, std::int16_t* storage, std::size_t storage_elems, const std::int16_t* samples,
                    ui::plot::Frame frames, unsigned channels, const char* label) noexcept {
  cache_ = nullptr;
  cancelled_ = false;
  done_ = 0;
  name_.assign("PEAKS ");
  name_.append(label != nullptr ? label : "");
  if (frames == 0 || !cache.init(storage, storage_elems, samples, frames, channels)) {
    return false;
  }
  // What build_step() will count: 64 raw frames an entry of level 1, four entries an entry of the levels above, per channel.
  total_ = 0;
  for (unsigned l = 1; l <= ui::plot::PeakCache::levels_for(frames); ++l) {
    total_ += static_cast<std::uint64_t>(ui::plot::PeakCache::entries_at(frames, l)) * (l == 1 ? 64u : 4u) * channels;
  }
  cache.build_begin();
  cache_ = &cache;
  return true;
}

core::JobStatus PeakJob::step(std::uint32_t budget_us) noexcept {
  if (cache_ == nullptr || cancelled_) {
    return core::JobStatus::Failed;
  }
  const std::size_t reads = static_cast<std::size_t>(budget_us) * kReadsPerUs;
  const bool built = cache_->build_step(reads);
  done_ += reads;
  return built ? core::JobStatus::Done : core::JobStatus::Running;
}

unsigned PeakJob::progress() const noexcept {
  if (cache_ != nullptr && cache_->built()) {
    return core::kJobProgressMax;
  }
  if (cache_ == nullptr || total_ == 0) {
    return 0;
  }
  const unsigned p = core::permille_of(done_, total_);
  return p >= core::kJobProgressMax ? core::kJobProgressMax - 1u : p;  // an estimate: never 100 % before Done
}

}  // namespace o3os::app
