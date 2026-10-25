// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// SND B, the reverb (OS-SENDFX; radar card `send-fx`; ADR 0013 amendment 3). NEW COMMUNITY OS BEHAVIOUR: the control set is size, damping, pre-delay and diffusion, the sound is
// ours. A SendFxFn (graph.h): the send bus (mono Q15) in, the two return planes out, stereo.
//
// THE ALGORITHM. The "plate" of J. Dattorro, "Effect Design, Part 1: Reverberator and Other Filters", J. Audio Eng. Soc. 45(9), 1997 (after Griesinger's work), written here from the paper's
// structure, in our own code; its constants are the paper's published ones (the delay lengths of the four input allpasses, of the two branches of the tank and the 14 output taps: send_reverb.cpp),
// WRITTEN DOWN FROM THE AUTHOR'S READING OF THE PAPER WITHOUT THE PAPER AT HAND: they are to be audited against it (the properties tests/audio_send_fx_test.cpp holds do not depend on them:
// finite, stable and decaying at every corner, exact silence after the tail, a dense and flat response). The lengths are used as they are, in samples, at 44.1 kHz instead of the paper's 29.76
// kHz: a plate about 1.5 times smaller (and the same 45 KB). Mutable Instruments' Clouds reverb (MIT, (c) 2014 Emilie Gillet) was read for the TOPOLOGY only; nothing of it (code or numbers) is used,
// so there is no notice to carry (THIRD_PARTY.md has nothing to add).
//
//   send -> [pre-delay] -> 4 allpasses in series (the input diffusion) -> the TANK, two branches that feed each other:
//       left : (in + decay x right's end) -> modulated allpass -> delay -> damping low-pass x decay -> allpass -> delay -+
//       right: (in + decay x left's end)  -> modulated allpass -> delay -> damping low-pass x decay -> allpass -> delay -+  (each branch's end is the other's input)
//   the left and right returns are sums of seven taps (signed) out of the tank's lines, as in the paper's output table. The two modulated allpasses (the paper's) have their delay moved by a
//   slow LFO (0.8 and 1.1 Hz, +-15 samples at 44.1 kHz) with a linear interpolation, which breaks the metallic ringing up. Each branch applies `decay` once (after its damping filter); the loop
//   gain is that, at most 0.91: stable at every setting.
//
// THE CONTROLS (graph_params.h sendfx::reverb): SIZE 0..100 is the decay of the tank (0.25 .. 0.91 a half-turn of the tank, a half-turn being 0.24 s: an RT60 of about 1.2 s to 18 s, measured
// on the host); DAMP 0..100 is the one-pole low-pass in each branch (a pole of 0.0005 .. 0.85, squared: a tail that loses its highs
// the faster the more);
// PREDELAY 0..100 is 64 frames (1.45 ms) .. 4,096 frames (93 ms), the input moved by a cross-fade over one block; DIFFUSION 0..100 is a coefficient 0..0.85, mapped so that its default 0.68 (here 80)
// gives the paper's allpass gains (0.75, 0.625; 0.70 in the tank), the maximum 0.94 and 0.88. Every control is slewed once a block. QUALITY (new) 0 HALF / 1 FULL.
//
// FULL AND HALF. FULL runs the plate at the engine's rate; its estimated cost is over the 5 % line (ADR 0013 amendment 3 has the figures), so HALF is the default: the plate runs at half the rate (22 kHz): the send is
// low-passed by a 23-tap half-band filter (a Kaiser window: flat to 7.7 kHz, -55 dB from 14.3 kHz, 6 dB down at 11 kHz) and decimated, the plate runs on 32 samples a block with every delay
// and tap offset halved (the same times, half the samples and half the memory), and its two outputs are interpolated back (the same filter, polyphase). The damping pole is squared so the tail's
// colour is the same in hertz. A change of quality clears the tail (the lines are repartitioned): a slice of the memory a block, the return silent for about 13 blocks.
//
// MEMORY AND BLOCKS. int16 delay lines in SDRAM (the reverb region, mem_map.h kReverbBase, 256 KiB: this is kMemBytes = 53,264 B, the FULL layout, HALF uses the first half of each line's
// share), staged through SRAM scratch in blocks: every line is at least a block long (the allpasses and delays are read and rewritten in place: the read is a copy of the next n slots, the write
// goes to the same n), every tap offset is at least a block (it reads what was written before the block began), so a block is: copy the slots in, do the arithmetic in float in SRAM, copy
// the new slots out. About 3 KB of SRAM scratch (the object), no heap. Every word written is saturated to int16 and cut towards zero: a recirculating tail shrinks to exactly zero (no limit
// cycle), and the effect then sleeps (the last frames written to every line are all zero for a whole ring: idle() true, the cost a 64-sample test of the input).
//
// Real time: no heap, no libm (one sqrtf, an instruction, once a block), no double, bounded loops (n <= 64), float only, denormals flushed once a block. Control rate: set_param is called in the
// render context, from the graph's drain (after the block's voices have rendered: a change lands from the next block) or before start. It runs from flash (the SRAM_L hot budget has no room), through the graph's SendFxFn pointer; SDRAM latency unmeasured (HW-B).

#include <o3os/audio/graph_params.h>
#include <o3os/hal/audio.h>

#include <cstdint>

namespace o3os::audio {

class SendReverb {
 public:
  static constexpr std::uint32_t kBlock = hal::audio::kBlock;
  static constexpr unsigned kLines = 13;
  static constexpr std::uint32_t kClearSamples = 2048u;  // samples of the lines a block clears while they are being cleared (4 KB)
  // The ring length of each line at the FULL rate, in samples: the pre-delay, the four input allpasses, then the tank's left branch (modulated allpass, delay, allpass, delay) and its right
  // branch. The two modulated allpasses (690 = 672 + 16 + 2, 926 = 908 + 16 + 2) hold their nominal delay plus the LFO's reach.
  static constexpr std::uint16_t kRingFull[kLines] = {4096, 142, 107, 379, 277, 690, 4453, 1800, 3720, 926, 4217, 2656, 3163};
  // The lines in the memory, each starting on a word boundary (an odd length is padded by a sample): 26,632 samples.
  static constexpr std::uint32_t kMemSamples = 4096u + 142u + 108u + 380u + 278u + 690u + 4454u + 1800u + 3720u + 926u + 4218u + 2656u + 3164u;
  static constexpr std::uint32_t kMemBytes = kMemSamples * 2u;  // 53,264

  SendReverb() noexcept = default;
  SendReverb(const SendReverb&) = delete;
  SendReverb& operator=(const SendReverb&) = delete;

  // Before start (or with the render stopped): the lines' memory, kMemSamples int16 (4-byte aligned; SDRAM), cleared here; the state is reset and every parameter is back at its default
  // (sendfx::). Without it (or with too little) the effect returns silence. (The members are all zero until then, so that the object is .bss, not .data.)
  void attach(std::int16_t* mem, std::uint32_t samples) noexcept;

  // Forget what the lines hold (a project load, and a change of quality): cleared kClearSamples a block, the return silent meanwhile (a block never clears the 53 KB), then the state is as after
  // attach() with the parameters kept. The same as set_param(sendfx::kClearField, 1).
  void begin_clear() noexcept;
  bool clearing() const noexcept { return clearing_; }

  // A parameter (sendfx::reverb field numbers); false when it is not one or the value is out of range. Render context, or before start. The tempo field is refused (no use for it).
  bool set_param(unsigned field, float value) noexcept;

  // One block: n = kBlock frames (a shorter block is treated as the start of a block of silence-padded input and its state advances by a whole block).
  void process(const std::int16_t* in, std::int16_t* out_l, std::int16_t* out_r, std::uint32_t n) noexcept;
  static void process_fx(void* self, const std::int16_t* in, std::int16_t* out_l, std::int16_t* out_r, std::uint32_t n) noexcept {
    static_cast<SendReverb*>(self)->process(in, out_l, out_r, n);
  }
  static bool param_fx(void* self, unsigned field, float value) noexcept { return static_cast<SendReverb*>(self)->set_param(field, value); }

  // For tests and `diag`.
  bool idle() const noexcept { return idle_; }
  bool half_rate() const noexcept { return shift_ != 0u; }
  bool finite() const noexcept;

 private:
  struct Line {
    std::int16_t* base = nullptr;
    std::uint32_t len = 0;  // the ring length at the rate in force
    std::uint32_t pos = 0;  // the slot the next block writes first
  };
  void run(const std::int16_t* in, std::int16_t* out_l, std::int16_t* out_r) noexcept;
  void configure() noexcept;
  void controls() noexcept;
  void decimate(const std::int16_t* in) noexcept;
  void interpolate(const float* y, std::int16_t* out, float* hist) noexcept;
  void input_block(const std::int16_t* zp, const std::int16_t* zq, bool fade, std::uint32_t n) noexcept;
  void tank_block(unsigned side, const std::int16_t* cross, std::uint32_t n) noexcept;
  // The staging copies (called some forty times a block: one copy of the loop each). copy_out returns where the first of the n slots is in the scratch (dst + the slot's parity); the buffer
  // that is written into a line is wbuf(k, line), the scratch w_[k] at the line's write position's parity, so that the copy in moves words too.
  [[gnu::noinline]] static const std::int16_t* copy_out(const Line& ln, std::uint32_t start, std::uint32_t n, std::int16_t* dst) noexcept;
  // OS-PERF3: the n slots from `start` to READ in a loop that writes nowhere in the line until it is over (every caller below writes the line only through copy_in, after its loop): the slots
  // themselves when they do not wrap past the ring's end (no copy at all: the line is read from where it is, which the code cache serves), else the staged copy of copy_out in `dst`. Not for
  // the two cross feeds (the first branch overwrites the last delay the second branch reads) nor the pre-delay's taps (written before they are used).
  [[gnu::always_inline]] static const std::int16_t* view(const Line& ln, std::uint32_t start, std::uint32_t n, std::int16_t* dst) noexcept {
    return start + n <= ln.len ? ln.base + start : copy_out(ln, start, n, dst);
  }
  [[gnu::noinline]] static std::uint32_t copy_in(Line& ln, const std::int16_t* src, std::uint32_t n) noexcept;
  std::int16_t* wbuf(unsigned k, const Line& ln) noexcept { return w_[k] + (ln.pos & 1u); }
  static std::uint32_t start_of(const Line& ln, std::uint32_t k) noexcept {
    const std::uint32_t s = ln.pos + ln.len - k;
    return s >= ln.len ? s - ln.len : s;
  }

  std::int16_t* mem_ = nullptr;
  Line line_[kLines];
  std::uint32_t tap_k_[2][7] = {};  // the tap offsets at the rate in force

  // The rate in force: shift 0 FULL, 1 HALF; nb the internal block (64 or 32), exc the LFO's excursion, nom the modulated allpasses' nominal delays, all in internal samples.
  std::uint32_t shift_ = 0;
  std::uint32_t nb_ = 0;
  std::uint32_t exc_ = 0;
  std::uint32_t nom_[2] = {0, 0};
  float em_ = 0.0f;
  std::uint32_t idle_after_ = 0;  // frames of zero writes after which every line is zero

  // The parameters as told.
  float size_ = 0.0f, damp_ = 0.0f, predelay_ = 0.0f, diffusion_ = 0.0f;  // (the defaults are put at attach(): 50, 50, 50 and 80)
  std::uint8_t quality_ = 0;  // 0 HALF, 1 FULL: what was asked for; shift_ is what is in force
  bool dirty_ = false;

  // The controls in force (slewed): the tank's decay, the damping pole's complement, the allpass gains, the output gain; the pre-delay in internal samples (in force, asked for).
  float decay_ = 0.0f, damp_q_ = 0.0f, g_in1_ = 0.0f, g_in2_ = 0.0f, g_t1_ = 0.0f, g_t2_ = 0.0f, out_gain_ = 0.0f;
  std::uint32_t pd_cur_ = 0, pd_tgt_ = 0;

  // The states.
  float lp_[2] = {0.0f, 0.0f};
  float lfo_phase_[2] = {0.0f, 0.0f};
  float lfo_m_[2] = {0.0f, 0.0f};
  float dec_hist_[22] = {};
  float int_hist_[2][11] = {};
  std::uint32_t zero_run_ = 0;
  std::uint32_t nz_ = 0;
  bool idle_ = false;
  bool clearing_ = false;
  std::uint32_t clear_pos_ = 0;

  // Scratch in SRAM (about 3.6 KB): the diffused input, the staged old content of the lines (z_) and the new content on its way back (w_), each two samples longer than a block (a staged
  // region starts at its slot's parity), the output taps, the half-band converters' work arrays and the LFO window.
  alignas(8) float d_[kBlock] = {};
  alignas(8) float y_[2][kBlock / 2] = {};
  alignas(8) float ext_[22 + kBlock] = {};
  alignas(8) std::int16_t z_[7][kBlock + 2] = {};
  alignas(8) std::int16_t w_[4][kBlock + 2] = {};
  alignas(8) std::int16_t tapbuf_[7][kBlock + 2] = {};
  alignas(8) std::int16_t win_[kBlock + 2 * 16 + 2 + 2] = {};
};

}  // namespace o3os::audio
