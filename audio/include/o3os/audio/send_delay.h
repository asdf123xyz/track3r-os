// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// SND A, the delay (OS-SENDFX; radar card `send-fx`; ADR 0013 amendment 3). NEW COMMUNITY OS BEHAVIOUR: the control set is ping-pong, sync, rate, time and feedback, the
// sound is ours. A SendFxFn (graph.h): the send bus (mono Q15) in, the two return planes out, stereo.
//
// THE RING. One ring of interleaved stereo frames, a frame being one 32-bit word (left in the low half, right in the high half, two int16): kRingFrames = 176,468 frames = 4.0 s at
// 44,117 Hz = 705,872 B, in the SDRAM delay region (mem_map.h kDelayBase, 768 KiB; the caller attaches it). The longest time is 3.5 s (the page's top); a synced time longer than the ring is held to it.
//   write frame t:  (Lw, Rw)                 read frame t:  (L, R) = (ring[t - dL].left, ring[t - dR].right)
//   ping-pong     Lw = in + g f(R)   Rw = g f(L)         dL = dR = D   the first repeat is on the left, the next on the right, and so on, D apart
//   stereo, mono  Lw = in + g f(L)   Rw = in + g f(R)    dL = D, dR = D (MONO) or 3/4 D (STEREO)
//   f is the feedback filter: a one-pole low-pass (TONE) and a DC blocker (a pole at 0.995, a 35 Hz high-pass), g the feedback gain, capped at 0.98 so that the loop gain stays below unity
//   at every frequency (the DC blocker's gain peaks at 1.0025 at Nyquist). The written words are saturated to int16 (a clipped loop is bounded, never wrapped) and rounded TOWARDS ZERO, so a
//   recirculating tail reaches exactly 0 (no limit cycle from rounding): silence in gives silence out.
//
// BLOCKS AND SDRAM. A block is 64 frames. A delay of at least 64 frames reads only what was written before the block began, so the block is: copy the 64 frames of the read region
// from the ring to a 256 B scratch (one burst, split in two at the ring's end), do all the arithmetic in SRAM, write the 64 frames back in one burst. The delay is held to [64, ring - 64].
// A time below 64 frames (1.45 ms) is therefore 64 frames; the shortest sync rate (1/32 step) is 4 ms at 120 BPM and reaches 64 frames only above ~330 BPM.
//
// TIME. Synced: D = round(rate x fs x 15 / bpm), the rate in steps (kDelayRates96 / 96) and fs the engine's rate; an INTEGER number of frames, no interpolation, the error is at most half a frame
// (11 us). Free: round(ms x fs / 1000). The tempo is the engine's (Graph::set_send_fx_tempo, BPM x 100, once a block when it changed; 120 BPM before it is told). A change of D (a knob, the
// tempo, the rate, a mode) does not jump: the old taps and the new ones are cross-faded over kXfade = 1,024 frames (23 ms) with an LINEAR pair of gains that sum to 1 (the repeats of one signal are correlated, so the level holds, and the loop gain stays below unity through a fade; an uncorrelated pair dips 3 dB in the middle), and a change that comes while one
// is running waits for it to end (the newest target wins). The feedback gain and the tone are slewed (a one-pole over about 7 ms of blocks, a linear ramp inside the block), the time is not.
//
// QUIET. When the last kRingFrames frames written were all zero the whole ring is zero: the effect then skips its work (the outputs are zero, the cost is a 64-sample test of the input)
// until the input is not zero again. That is exact, not a gate: nothing a tail would still say is cut.
//
// Real time: no heap, no libm, no double, bounded loops (n <= 64), float only, denormals flushed once a block. Control rate: parameters are applied (set_param) in the render context at block
// start (the graph's drain) or before start, and read once per block here. It runs from flash (the SRAM_L hot budget has no room: the code is about 1.4 KiB at -O3), called through the graph's
// SendFxFn pointer; SDRAM latency is unmeasured (HW-B).

#include <o3os/audio/graph_params.h>
#include <o3os/hal/audio.h>

#include <cstdint>

namespace o3os::audio {

class SendDelay {
 public:
  static constexpr std::uint32_t kBlock = hal::audio::kBlock;
  static constexpr std::uint32_t kRingFrames = 176468u;                // 4.0 s at 44,117 Hz
  static constexpr std::uint32_t kRingBytes = kRingFrames * 4u;        // 705,872
  static constexpr std::uint32_t kMinDelay = kBlock;                   // a block of the read region must all be in the past
  static constexpr std::uint32_t kXfade = 1024u;                       // frames of a cross-fade between two times
  static constexpr std::uint32_t kClearFrames = 2048u;               // frames of the ring a block clears while it is being cleared (8 KB)
  static constexpr float kMaxFeedback = 0.98f;                         // the loop gain never reaches unity (see the header)

  SendDelay() noexcept = default;
  SendDelay(const SendDelay&) = delete;
  SendDelay& operator=(const SendDelay&) = delete;

  // Forget what the ring holds (a project load): cleared kClearFrames a block, the return silent meanwhile (a block never clears the whole 705 KB), then the state is as after attach().
  // The same as set_param(sendfx::kClearField, 1).
  void begin_clear() noexcept;
  bool clearing() const noexcept { return clearing_; }

  // Before start (or with the render stopped): the ring, `frames` words (at least 4 blocks) at a 4-byte aligned address; it is cleared here (SDRAM holds whatever it held), the state is
  // reset and every parameter is back at its default (sendfx::). Without a ring the effect returns silence. (The members are all zero until then, so that the object is .bss, not .data.)
  void attach(std::uint32_t* ring, std::uint32_t frames) noexcept;

  // A parameter (sendfx::delay field numbers, plus sendfx::kTempoField); false when the field is not one or the value is out of range (nothing changes). Render context, or before start.
  bool set_param(unsigned field, float value) noexcept;

  // One block of n <= kBlock frames; the SendFxFn of the graph is process_fx with this object as its state.
  void process(const std::int16_t* in, std::int16_t* out_l, std::int16_t* out_r, std::uint32_t n) noexcept;
  static void process_fx(void* self, const std::int16_t* in, std::int16_t* out_l, std::int16_t* out_r, std::uint32_t n) noexcept {
    static_cast<SendDelay*>(self)->process(in, out_l, out_r, n);
  }
  static bool param_fx(void* self, unsigned field, float value) noexcept { return static_cast<SendDelay*>(self)->set_param(field, value); }

  // For tests and `diag`.
  bool idle() const noexcept { return idle_; }
  std::uint32_t delay_left() const noexcept { return cur_l_; }  // the frames of the time in force (the target while a cross-fade runs)
  std::uint32_t delay_right() const noexcept { return cur_r_; }
  bool fading() const noexcept { return xf_left_ != 0u; }
  bool finite() const noexcept;  // every state word is a finite number

 private:
  void retarget() noexcept;
  void reset_state() noexcept;
  void stage_read(std::uint32_t from, std::uint32_t n) noexcept;
  void stage_write(std::uint32_t at, std::uint32_t n) noexcept;
  void read_taps(float* l, float* r, std::uint32_t dl, std::uint32_t dr, std::uint32_t n) noexcept;

  std::uint32_t* ring_ = nullptr;
  std::uint32_t frames_ = 0;
  std::uint32_t pos_ = 0;  // the frame the next block writes first, 0..frames_ - 1

  // The parameters as told (render context).
  float time_ms_ = 0.0f;  // (the defaults are put at attach(): sendfx::kDelayTimeDefault 500, feedback 50, tone 100, 120 BPM)
  float feedback_pct_ = 0.0f;
  float tone_ = 0.0f;
  float tempo_x100_ = 0.0f;
  std::uint8_t sync_ = 0;
  std::uint8_t rate_ = 0;
  std::uint8_t ping_ = 0;
  std::uint8_t mode_ = 0;
  bool dirty_ = false;

  // The time: the frames in force, the ones a cross-fade leaves, the ones asked for, the frames of the fade still to go.
  std::uint32_t cur_l_ = 0;  // 0: no time yet (the first block takes the target at once)
  std::uint32_t cur_r_ = 0;
  std::uint32_t old_l_ = 0;
  std::uint32_t old_r_ = 0;
  std::uint32_t tgt_l_ = 0;
  std::uint32_t tgt_r_ = 0;
  std::uint32_t xf_left_ = 0;

  // The loop: gain and low-pass coefficient (slewed), the filter states per channel (low-pass, DC blocker input and output), and the quiet bookkeeping.
  float gain_ = 0.0f;
  float lp_a_ = 0.0f;
  float lp_[2] = {0.0f, 0.0f};
  float hp_x_[2] = {0.0f, 0.0f};
  float hp_y_[2] = {0.0f, 0.0f};
  std::uint32_t zero_run_ = 0;  // consecutive frames written as zero words (capped at frames_)
  bool idle_ = false;
  bool clearing_ = false;
  std::uint32_t clear_pos_ = 0;

  // Scratch in SRAM: the block's staged words, then the taps as floats (and, during a cross-fade, the old taps).
  alignas(8) std::uint32_t stage_[kBlock] = {};
  alignas(8) float lo_[kBlock] = {};
  alignas(8) float ro_[kBlock] = {};
  alignas(8) float xl_[kBlock] = {};
  alignas(8) float xr_[kBlock] = {};
};

}  // namespace o3os::audio
