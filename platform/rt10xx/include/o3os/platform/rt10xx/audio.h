// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The audio path of the rt10xx image (OS-PORT-RT10XX phase 3, src/audio.cpp): the hal::audio::Device over SAI1, the eDMA and the SGTL5000,
// the engine and the voices as statics, the status the console reads. The same offer as platform/mk66 audio.h, on the other chip.
// Implemented only in the image build; the declarations compile everywhere.

#include <o3os/audio/engine.h>
#include <o3os/audio/pipeline.h>
#include <o3os/audio/status.h>
#include <o3os/audio/voices.h>
#include <o3os/hal/audio.h>
#include <o3os/platform/mk66/sgtl5000.h>

#include <cstdint>

namespace o3os::platform::rt10xx {

hal::audio::Device& audio_device() noexcept;
bool audio_input_apply(const o3os::platform::mk66::sgtl5000::InputPlan& plan) noexcept;
void audio_stop() noexcept;
o3os::audio::Engine& audio_engine() noexcept;
o3os::audio::VoicePool& audio_voices() noexcept;
o3os::audio::AmigaVoices& audio_amiga() noexcept;
const o3os::audio::AudioStatus& audio_status() noexcept;
void audio_cpu_reset() noexcept;
void audio_main_pass() noexcept;
const o3os::audio::Pipeline& audio_pipeline() noexcept;
std::uint8_t audio_last_result() noexcept;

// Where a start() stopped on a clock fault, with the raw registers then.
struct AudioClockFault {
  enum Where : std::uint8_t { None, PllLock, NoBoundaries };
  std::uint8_t where = None;
  std::uint32_t pll = 0;   // CCM_ANALOG_PLL_AUDIO as read
  std::uint32_t rcsr = 0;  // the receiver's status (the clock master) as read
  std::uint32_t tcsr = 0;
};
AudioClockFault audio_clock_fault() noexcept;
bool audio_running() noexcept;
// True while the last mute() or stop() could not write the codec (this board has no amplifier key: the outputs may still be open).
bool audio_mute_failed() noexcept;
const char* audio_result_name(std::uint8_t result) noexcept;

}  // namespace o3os::platform::rt10xx
