// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/formats/volume_level.h>

#include <o3os/core/fast_math.h>

namespace o3os::formats {

std::uint8_t volume_level_of_tracker(unsigned tracker_volume) noexcept {
  if (tracker_volume == 0u) {
    return 0u;
  }
  if (tracker_volume >= 64u) {
    return 50u;
  }
  // level = 50 + round(20 log10(v / 64) / 0.48) = 50 + round(log2(v / 64) * 12.5429...), in float (the FPU is single precision)
  const float steps = core::log2_fast(static_cast<float>(tracker_volume) * (1.0f / 64.0f)) * 12.5429f;  // 20 log10(2) / 0.48
  const int level = 50 + static_cast<int>(steps - 0.5f);  // steps is negative: truncation toward zero after -0.5 rounds half away from zero
  return static_cast<std::uint8_t>(level < 1 ? 1 : level);
}

}  // namespace o3os::formats
