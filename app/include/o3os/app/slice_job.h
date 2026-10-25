// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The AUTO slicer of the Chop page as a core::Job (OS-011a): the transient detector (transient.h) over the PCM of one instrument, a slice of the main loop's
// pass at a time, so a long sample never holds the screen. The App starts it when the AUTO action asks, and when it is Done chooses the slices (choose_slices)
// and hands them to the internal action wave.auto_apply, which makes them one undo unit. The PCM must stay as it is while it runs (the App abandons it when the
// sample changes).

#include <o3os/app/transient.h>
#include <o3os/core/fixed_string.h>
#include <o3os/core/job.h>

#include <cstdint>

namespace o3os::app {

class SliceJob final : public core::Job {
 public:
  // The cost model: frames the detector reads per microsecond of budget (a frame is a load from SDRAM, a multiply and an add; the windows, the block refinement
  // of a hit and the bookkeeping make it about 30 cycles at 180 MHz). PROVISIONAL until measured (the runner counts the steps that overrun their slice).
  static constexpr std::uint32_t kFramesPerUs = 4;

  // `storage` keeps the hits (TransientDetector::kMaxOnsets). False (no job to run) when there is nothing to read. `label` names it in the strip ("SLICE BREAK_A 40%").
  bool begin(const std::int16_t* pcm, std::uint32_t frames, TransientDetector::Onset* storage, const char* label) noexcept;

  core::JobStatus step(std::uint32_t budget_us) noexcept override;
  unsigned progress() const noexcept override;
  void cancel() noexcept override { cancelled_ = true; }
  const char* name() const noexcept override { return name_.c_str(); }

  const TransientDetector& detector() const noexcept { return detector_; }

 private:
  TransientDetector detector_;
  bool cancelled_ = false;
  core::FixedString<40> name_;
};

}  // namespace o3os::app
