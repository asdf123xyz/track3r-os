// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The metronome's click (OS-METRO, new Community OS behaviour; docs/specs/OS-METRO.md, decisions D2 and D4). The sequencer emits a Metro event on each beat of its timeline (events.h); the
// engine hands it to trigger() with the sample offset inside the block about to be rendered, and Graph::render() adds the click into the TX block, the interleaved pair that the codec plays, AFTER every tap
// has read the block: the meters, the scope, the stems (the USB stream), the RESAMPLE take and the REC bus never hear it, and nothing else reads the TX block. So the click is in no take, no stream
// and no export, by construction (D2). Which side of the line out it goes to is Graph's business (OUT MODE): see Graph::render().
//
// THE SOUND (D4). A short decaying sine, 15 ms, 1 kHz on a plain beat and 1.5 kHz on the first beat of a bar. It is a two-pole resonator, y[n] = a1 y[n-1] + a2 y[n-2], so there is no table, no
// libm and no division: two multiplies and an add a sample, in single precision. With a2 = -r^2 and a1 = 2 r cos w the output is A r^n sin(w n); r is 2^(-15/662), so after the 662 frames of the
// click the amplitude is 2^-15 of the start (96 dB down: below the least significant bit of the line out, so the click ends without a step). The frequencies are for 44,117.6 Hz (the default
// rate, core/audio_clock.h); at 44,100 Hz (-DO3OS_RATE_44100) they are 0.04 % lower. The level is METRO VOL, 0..100, a linear amplitude: 100 is 0.7 of full scale on the accent (-3 dBFS), 50 (the
// default) -9 dBFS; the plain beat is 0.8 of the accent. These are by-ear numbers (HW check of OS-METRO); the shape of the code does not depend on them.
//
// REAL TIME. Render context only (trigger() from the event dispatch, set_*() from the ParamChange drain, mix_out() from Graph::mix_extras, which Graph::render calls): one writer, no atomics, no allocation, no blocking, no double. mix()
// is one bounded loop of at most one block of frames and runs only while a click sounds (11 blocks of every beat at 120 BPM, none otherwise). trigger() and Graph::mix_extras() (which calls mix_out()) are in flash
// (O3OS_AUDIO_COLD, the exception list of hot.h: each called from SRAM_L through a long call, trigger once per beat and mix_extras once per block while a click sounds), so the SRAM_L hot budget pays for two calls and a test, not for
// the click. The state is a few words inside the Graph, outside the taps' memory.

#include <o3os/audio/hot.h>
#include <o3os/hal/audio.h>

#include <cstdint>

namespace o3os::audio {

class Metro {
 public:
  enum class Mode : std::uint8_t { Off = 0, Cue = 1, Main = 2 };

  static constexpr std::uint32_t kClickFrames = 662;  // 15 ms at 44,117.6 Hz
  static constexpr float kAccentPeak = 0.7f;          // of full scale, at METRO VOL 100
  static constexpr float kTickRatio = 0.8f;           // the plain beat against the accent

  // The ParamChange fields (graph_params.h master::MetroMode, MetroLevel). False, nothing changed, for a value outside the range (NaN too).
  bool set_mode(float v) noexcept {
    if (!(v >= 0.0f && v <= 2.0f)) {
      return false;
    }
    mode_ = v >= 1.5f ? Mode::Main : (v >= 0.5f ? Mode::Cue : Mode::Off);
    if (mode_ == Mode::Off) {
      cancel();  // switched off: what was triggered and what sounds ends here (a tail of a few milliseconds, cut at a level far below the least bit already)
    }
    return true;
  }
  bool set_level(float v) noexcept {
    if (!(v >= 0.0f && v <= 100.0f)) {
      return false;
    }
    level_ = v * (1.0f / 100.0f);
    return true;
  }
  Mode mode() const noexcept { return mode_; }

  // A beat `offset` frames into the block about to be rendered (the Metro event; `accent`: the first beat of a bar). Ignored while the metronome is off or its level is 0. A click that still sounds
  // is replaced (the beats are at least 18 ms apart at 800 BPM, longer than the click; only a backlog of late events can overlap them). At most one trigger a block is kept: the last. In flash
  // (O3OS_AUDIO_COLD), once per Metro event, a beat a few hundred milliseconds apart, so that the SRAM_L hot budget pays a call and not the body.
  O3OS_AUDIO_COLD void trigger(std::uint32_t offset, bool accent) noexcept;
  // Drops the click that was triggered and the one that sounds (a muted render, METRO switched off).
  void cancel() noexcept {
    pending_ = false;
    left_ = 0;
  }
  // True while there is something for mix() to do: a trigger waiting for the next block, or a click still sounding.
  bool busy() const noexcept { return pending_ || left_ != 0u; }

  // Adds the click into `out` (`frames` interleaved L, R frames of int16; the sum saturates), starting at the pending trigger's offset, to the left channel when `left`, to the right when `right`.
  // At most `frames` iterations. In flash, called from mix_out(), tests and the bench.
  void mix(std::int16_t* out, std::uint32_t frames, bool left, bool right) noexcept;
  // What Graph::mix_extras() calls (graph.cpp), once per block while busy(): a muted render (`silence`: the codec is muted, or recovering) drops the click; else it is mixed to MAIN's side of the line out (both
  // channels in OUT MODE Stereo, which has no CUE: `split` false; the left jack in the split OUT MODE) or to CUE's (the right jack, `split` true and the mode Cue). In flash, called from flash (Graph::mix_extras is the
  // render's one call).
  void mix_out(std::int16_t* out, std::uint32_t frames, bool silence, bool split) noexcept;

 private:
  Mode mode_ = Mode::Cue;
  bool pending_ = false;
  bool accent_ = false;
  float level_ = 0.5f;           // METRO VOL / 100 (the descriptor's default is 50)
  float a1_ = 0.0f;              // the resonator of the click that sounds
  float p1_ = 0.0f;              // y[n-1]
  float p2_ = 0.0f;              // y[n-2]
  std::uint32_t start_ = 0;      // the pending trigger's offset in the next block
  std::uint32_t left_ = 0;       // frames of the click still to play
};

}  // namespace o3os::audio
