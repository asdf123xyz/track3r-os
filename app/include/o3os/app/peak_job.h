// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The second job of an import (OS-002c): the peak-cache mipmap of the sample that was just imported (ui::plot::PeakCache, OS-003d)
// built cooperatively, so the waveform can be drawn at any zoom. The App chains it after the import job. The cache and its storage
// belong to the caller; this job only drives PeakCache::build_step(). Until it is Done the cache answers "no peak".

#include <o3os/core/fixed_string.h>
#include <o3os/core/job.h>
#include <o3os/ui/plot/peak_cache.h>

#include <cstddef>
#include <cstdint>

namespace o3os::app {

class PeakJob final : public core::Job {
 public:
  // The cost model: reads (a frame or an entry) per microsecond of budget. PROVISIONAL until measured on the MK66 (the
  // SSUB16 kernel reads two samples a word from SDRAM); the runner counts the steps that run over their slice.
  static constexpr std::size_t kReadsPerUs = 8;

  // Binds `cache` to `storage` and the sound and starts the build. False (no job to run) when the cache refuses: the storage is
  // too small, or the sound is empty or longer than the cache holds. `label` is the file name for the notification.
  bool begin(ui::plot::PeakCache& cache, std::int16_t* storage, std::size_t storage_elems, const std::int16_t* samples,
             ui::plot::Frame frames, unsigned channels, const char* label) noexcept;

  core::JobStatus step(std::uint32_t budget_us) noexcept override;
  unsigned progress() const noexcept override;
  void cancel() noexcept override { cancelled_ = true; }  // the cache stays unbuilt: its queries answer "no peak"
  const char* name() const noexcept override { return name_.c_str(); }

 private:
  ui::plot::PeakCache* cache_ = nullptr;
  std::uint64_t total_ = 0;  // reads the whole build costs (an estimate: what build_step() counts)
  std::uint64_t done_ = 0;
  bool cancelled_ = false;
  core::FixedString<40> name_;
};

}  // namespace o3os::app
