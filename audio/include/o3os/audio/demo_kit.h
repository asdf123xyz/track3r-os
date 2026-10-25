// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The demo kit (OS-005d lite): four short sounds made at boot into memory, so that a pattern plays before the SD card can load a project's samples
// (OS-007e). A kick (a sine whose pitch falls), a snare (noise and a tone), a hat (noise through a first-difference high-pass) and a bass (a band-
// limited saw, one cycle, looped). Cold code: it runs once, from flash, and the PCM goes where the caller says (the sample arena in SDRAM on the MK66), so
// the sounds cost no data in flash. The samples are made at the engine's sample rate, so a note at the root plays them at their own pitch; the bass
// cycle is a C at the root (130.8 Hz), so note 60 is a C3 and the pattern's note names are the pitch classes they say.
//
// Float only (single precision; sin_turns and core::exp2_fast, no libm), no heap, deterministic: the same bytes on every run of one build.

#include <o3os/audio/voices.h>

#include <cstdint>

namespace o3os::audio {

inline constexpr unsigned kDemoKitSounds = 4;
inline constexpr unsigned kDemoKick = 0;
inline constexpr unsigned kDemoSnare = 1;
inline constexpr unsigned kDemoHat = 2;
inline constexpr unsigned kDemoBass = 3;

struct DemoKit {
  SampleDesc sample[kDemoKitSounds];
  std::uint32_t samples = 0;  // int16 samples used from the buffer
};

// Writes the four sounds one after another into `dst` (4-byte aligned, `capacity` samples) and describes them in `kit`. False, with `kit` empty, when
// they do not fit (about 30,300 samples, 61 KB).
bool generate_demo_kit(std::int16_t* dst, std::uint32_t capacity, DemoKit& kit) noexcept;

}  // namespace o3os::audio
