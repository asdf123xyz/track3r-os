// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/slice_job.h>

namespace o3os::app {

bool SliceJob::begin(const std::int16_t* pcm, std::uint32_t frames, TransientDetector::Onset* storage, const char* label) noexcept {
  if (pcm == nullptr || frames == 0u || storage == nullptr) {
    return false;
  }
  detector_.begin(pcm, frames, storage);
  cancelled_ = false;
  name_.assign("SLICE ");
  name_.append(label);
  return true;
}

core::JobStatus SliceJob::step(std::uint32_t budget_us) noexcept {
  if (cancelled_) {
    return core::JobStatus::Failed;
  }
  const std::uint64_t frames = static_cast<std::uint64_t>(budget_us) * kFramesPerUs;
  return detector_.step(frames > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<std::uint32_t>(frames)) ? core::JobStatus::Done : core::JobStatus::Running;
}

unsigned SliceJob::progress() const noexcept {
  return core::permille_of(detector_.position(), detector_.frames());
}

}  // namespace o3os::app
