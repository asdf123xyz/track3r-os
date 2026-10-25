// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The master limiter's controls on the Mixer's MASTER FX page (OS-LIMIT): the three rows after the send effects' (send_fx_rows.h): THRESHOLD in per
// cent of full scale and ATTACK and RELEASE in milliseconds, 1..1000 (a threshold of 0 would mute: 1 % is the least). The LIMITER switch is row 8 of the page (the model's master parameter)
// and the LIMITER MODE a project setting (Subtle or Extreme): both are the project's; THESE THREE ARE NOT STORED (the project has no field for them yet: model/params.h, "Engine FX parameters
// ... are not stored"): the board keeps them for the session, the engine starts at the same values (audio/graph_params.h `limiter`), and they are told by a ParamChange of kind Master (fields
// 0x140..0x142) at every edit. The sound is new Community OS behaviour (audio/master_limiter.h).
// The rows reuse the send effects' row type, stepping and text (send_fx_rows.h): a Number is a whole number a detent, Ms steps by the size of the value.

#include <o3os/app/send_fx_rows.h>
#include <o3os/audio/graph_params.h>

#include <cstdint>

namespace o3os::app {

inline constexpr unsigned kLimiterRows = 3;
inline constexpr SendFxRow kLimiterTable[kLimiterRows] = {
    {"LIM THRESH", gpm::LimiterThreshold, SendFxKind::Number, 1, 100, 92, "%", nullptr},
    {"LIM ATTACK", gpm::LimiterAttack, SendFxKind::Ms, 1, 1000, 10, "ms", nullptr},
    {"LIM RELEASE", gpm::LimiterRelease, SendFxKind::Ms, 1, 1000, 100, "ms", nullptr},
};

// The table is the limiter's own (graph_params.h limiter::): a board that is never touched says what the engine does.
static_assert(kLimiterTable[0].min == static_cast<std::uint16_t>(audio::limiter::kThresholdMin) && kLimiterTable[0].max == static_cast<std::uint16_t>(audio::limiter::kThresholdMax) &&
                  kLimiterTable[0].def == static_cast<std::uint16_t>(audio::limiter::kThresholdDefault) && kLimiterTable[1].min == static_cast<std::uint16_t>(audio::limiter::kTimeMin) &&
                  kLimiterTable[1].max == static_cast<std::uint16_t>(audio::limiter::kTimeMax) && kLimiterTable[2].min == kLimiterTable[1].min && kLimiterTable[2].max == kLimiterTable[1].max &&
                  kLimiterTable[1].def == static_cast<std::uint16_t>(audio::limiter::kAttackDefault) && kLimiterTable[2].def == static_cast<std::uint16_t>(audio::limiter::kReleaseDefault),
              "the limiter rows start where the limiter starts");

// The rows of the page that are controls of an effect, in order: the send effects' (kSendFxRows), then the limiter's.
inline constexpr unsigned kMasterFxControls = kSendFxRows + kLimiterRows;
inline const SendFxRow& master_fx_row(unsigned index) noexcept { return index < kSendFxRows ? kSendFxTable[index] : kLimiterTable[index - kSendFxRows]; }

}  // namespace o3os::app
