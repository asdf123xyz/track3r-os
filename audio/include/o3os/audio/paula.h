// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Paula model (OS-005h; ADR 0013; radar card `amiga-playback-mode`): one Amiga audio channel and the Amiga's output filters, in single-precision float,
// block based. A re-implementation of the model of pt2-clone's Paula emulator (https://github.com/8bitbubsy/pt2-clone, BSD-3-Clause, (c) 2010-2026 Olav Sorensen; the
// files read and the pinned commit are in THIRD_PARTY.md, the tables and their notice in src/paula_tables.cpp); the BLEP technique is aciddose's in that code. The
// host tests run the original (third_party/pt2-clone, the dsp-port skill's gate 2) next to this one and compare the output sample by sample.
//
// WHAT PAULA DOES, as modelled (the register and DMA plumbing of the original is dropped: there are no registers here):
//   PERIOD   an integer 113..65535 (0 means 65536) in units of the PAL colour clock, 3,546,895 Hz: the channel fetches one sample every `period` clock ticks. The step
//            per output sample is kPeriodToDeltaDiv / period, a float phase that wraps at 1.0; at the wrap the channel latches the period last written (a period
//            written now takes effect at the next sample fetch) and fetches the next 8-bit sample. The output rate must be above 31,389 Hz (113 -> 31,388 Hz).
//   SAMPLE   sample-and-hold, no interpolation: the value stays until the next fetch. Sample data here is int16 and Paula's 8 bits are its top byte (an imported 8-bit
//            sample is exact). The volume 0..64 (no PWM) is read at the fetch and multiplies the sample: a full-scale sample at volume 64 is -1.0 .. 0.992.
//   BLEP     every step between two held values is band limited: a minBLEP of 16 zero crossings at 16 taps is added at the sub-sample position of the step, so the
//            steps of the sample-and-hold do not alias. A 32-float ring holds the taps still to come; an idle ring costs nothing.
//   LOOP     the channel plays [begin, end) once, then repeats [loop_start, loop_end); a loop of less than 2 frames is silence (Paula looping on a zero word).
//   FILTERS  the Amiga 500's one-pole RC low-pass (4,421 Hz), the "LED" two-pole Sallen-Key low-pass (3,090.5 Hz, Q 0.660) and the one-pole high-pass (5.1 Hz); the
//            Amiga 1200 has no audible low-pass and a 5.3 Hz high-pass. They are linear, so one filter on the sum of channels is the same as one on each channel:
//            the Filter here is mono and runs on a channel's block (or as a graph insert).
// DIFFERENCES from pt2-clone: no 2x oversampling and half-band downsample (the radar card defers it to a listening test), mono channels (the graph pans them: the
// importer's hard L R R L is the track pan), the filters' coefficients are float constants worked out in double offline (the library has no double; the test recomputes them).
//
// Real time: no heap, no double, no libm and no flash call on the per-sample path (the hot functions are O3OS_AUDIO_HOT, SRAM_L on the MK66), bounded by the block.

#include <o3os/audio/hot.h>
#include <o3os/hal/audio.h>

#include <cstdint>

namespace o3os::audio::paula {

inline constexpr std::uint32_t kPalClockHz = 3546895;  // 28,375,160 / 8
inline constexpr std::uint32_t kMinPeriod = 113;       // below it the hardware (and the BLEP) stop being valid
inline constexpr std::uint32_t kMaxPeriod = 65535;
// Clock ticks per output sample (PAL clock over the engine's rate, 80.396286); the step of a period is this over the period. Worked out in integers (a long division to
// 40 fractional bits, one rounding to float: the same float pt2-clone's double division gives), so that the library holds no double.
constexpr float period_to_delta_div(std::uint32_t rate_mhz) noexcept {
  const std::uint64_t num = static_cast<std::uint64_t>(kPalClockHz) * 1000u;
  std::uint64_t q = num / rate_mhz;
  std::uint64_t rem = num % rate_mhz;
  for (unsigned i = 0; i < 40u; ++i) {
    rem <<= 1;
    q <<= 1;
    if (rem >= rate_mhz) {
      rem -= rate_mhz;
      q |= 1u;
    }
  }
  return static_cast<float>(q) * (1.0f / 1099511627776.0f);
}
inline constexpr float kPeriodToDeltaDiv = period_to_delta_div(hal::audio::sample_rate_mhz());

inline constexpr unsigned kBlepNs = 16;          // taps
inline constexpr unsigned kBlepSp = 16;          // table steps per tap
inline constexpr unsigned kBlepRing = 32;        // power of two above kBlepNs
inline constexpr unsigned kBlepTableLen = 257;   // 256 + the copy of the last point

extern const float kMinBlep[kBlepTableLen];           // flash
extern const std::uint16_t kPeriodTable[16][36];      // finetune 0..7, -8..-1; C-1 .. B-3

// MED / ProTracker notes: the model's note 48 (C-1 of the MED keyboard, the importer's byte n + 48 for n = 1) is table index 0; note 60 is period 428 at finetune 0.
inline constexpr unsigned kTableFirstNote = 48;
inline constexpr unsigned kTableNotes = 36;

// The period of `note` (0..127) at `finetune` (-8..7): the table inside its three octaves, the octave rule (a halving every 12 semitones) outside, held to
// kMinPeriod..kMaxPeriod. Cold code (note starts and ticks).
O3OS_AUDIO_COLD std::uint32_t note_period(int note, int finetune) noexcept;

// ---- the filters ---------------------------------------------------------------------------------------------------------------

// ---- the filters' coefficients (see the notes at each) ------------------------------------------------------------------------------
namespace coef {
// Float constants. pt2-clone computes them in double at setup (pt2_rcfilters.c setupOnePoleFilter / setupTwoPoleFilter) and keeps floats; the tree has no double, so the
// values below are those floats, worked out once for the engine's rate (hal::audio::sample_rate_mhz() = 44,100,000 or 44,117,647: tests/paula_oracle_test.cpp recomputes them in double
// with the original's formulas and asserts they are equal bit for bit, so a change of the rate fails there). One pole: b1 = exp(-2 pi fc / fs), a0 = 1 - b1.
//   Amiga 500 rev 6A: low-pass R 360 ohm C 0.1 uF (fc 4,420.97 Hz); high-pass R 1390 ohm (1000 + 390) C 22.33 uF (22 + 0.33) (fc 5.1276 Hz).
//   Amiga 1200 rev 1D4: high-pass R 1360 ohm C 22 uF (fc 5.3193 Hz); its low-pass (about 34.4 kHz) is not modelled.
//   The LED filter, the same on both: Sallen-Key, R1 = R2 = 10k, C1 = 6800 pF, C2 = 3900 pF (fc 3,090.53 Hz, Q 0.660225); a1 = 1 / (1 + a / Q + a^2), a2 = 2 a1,
//   b1 = 2 (1 - a^2) a1, b2 = (1 - a / Q + a^2) a1, a = 1 / tan(pi fc / fs). Component values as pt2_paula.c states them.
struct OnePole {
  float a0;
  float b1;
};
struct TwoPole {
  float a1, a2, b1, b2;
};
// Two sets, one per clock (core/audio_clock.h): 44,100,000 mHz (the default) and 44,117,647 mHz (-DO3OS_RATE_44100=OFF); the other is not compiled in (the ternary folds).
inline constexpr bool k44100 = hal::audio::sample_rate_mhz() == 44100000u;
static_assert(k44100 || hal::audio::sample_rate_mhz() == 44117647u, "the Paula filter coefficients are worked out for 44,100 Hz and 44,117.647 Hz only");
inline constexpr OnePole kA500Lo = k44100 ? OnePole{0.467345119f, 0.532654881f} : OnePole{0.467210919f, 0.532789111f};
inline constexpr OnePole kA500Hi = k44100 ? OnePole{0.000730296539f, 0.999269724f} : OnePole{0.000730004569f, 0.999270022f};
inline constexpr OnePole kA1200Hi = k44100 ? OnePole{0.000757591799f, 0.999242425f} : OnePole{0.000757288886f, 0.999242723f};
inline constexpr TwoPole kLed = k44100 ? TwoPole{0.0360552408f, 0.0721104816f, -1.36772919f, 0.511950195f} : TwoPole{0.0360301584f, 0.0720603168f, -1.36796749f, 0.51208818f};

}  // namespace coef

enum class Model : std::uint8_t { Off = 0, A500 = 1, A1200 = 2 };

class Filter {
 public:
  // The model's low-pass and high-pass, and the LED filter on top. Clears the LED's state when it changes (as the original does).
  O3OS_AUDIO_COLD void set(Model model, bool led) noexcept;
  void clear() noexcept;
  Model model() const noexcept { return model_; }
  bool led() const noexcept { return led_; }
  // In place, in the original's order: low-pass, LED, high-pass. The block is float, +-1.0 full scale (it may leave it: the high-pass overshoots).
  // OS-CPU10 A2: flash. The render does not call it any more: Channel::run_q15 holds the same arithmetic inside its sample loop (the specification of the pair is this and
  // Channel::run, tests/amiga_voices_test.cpp holds them bit-equal), so the three float passes of a voice are one.
  O3OS_AUDIO_COLD void process(float* x, std::uint32_t n) noexcept;
  // A graph InsertFn (graph.h): the same filter on a track's Q15 block, in place, in chunks so that no large block sits on the stack.
  // It runs from flash (an optional graph insert, not on the default path: SRAM_L is scarce).
  static void insert(void* state, std::int16_t* io, std::uint32_t n) noexcept;

 private:
  friend class Channel;  // run_q15 runs the filter inside the channel's loop on this state
  float lo_ = 0.0f;
  float hi_ = 0.0f;
  float led_s_[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // input history x1, x2, output history y1, y2
  Model model_ = Model::Off;
  bool led_ = false;
};

// ---- one channel ---------------------------------------------------------------------------------------------------------------

class Channel {
 public:
  // The channel's DMA starts: it plays frames [begin, end) of `data`, then the loop [loop_start, loop_end) for ever (a loop of under 2 frames: silence). `data` stays valid
  // while the channel is active. The held sample and the BLEP ring carry over from the previous note (a new note's first step is band limited too).
  O3OS_AUDIO_COLD void start(const std::int16_t* data, std::uint32_t begin, std::uint32_t end, std::uint32_t loop_start, std::uint32_t loop_end) noexcept;
  // AUDxPER: 0 is 65536, below kMinPeriod is kMinPeriod. Takes effect at the next sample fetch (the original's behaviour).
  O3OS_AUDIO_COLD void set_period(std::uint32_t period) noexcept;
  // AUDxVOL 0..64. Read at the next sample fetch.
  O3OS_AUDIO_INLINE void set_volume(std::uint32_t vol) noexcept { vol_ = static_cast<float>(vol > 64u ? 64u : vol) * (1.0f / (128.0f * 64.0f)); }
  // The held value goes to 0 now (a BLEP step at the current sub-sample position) and the channel ends when the ring has run out: a click-free stop.
  O3OS_AUDIO_COLD void fade_out() noexcept;
  void stop() noexcept { active_ = false; }
  bool active() const noexcept { return active_; }
  std::uint32_t period() const noexcept { return period_; }
  // The next `n` output samples (float, +-1.0 full scale) into out. `blep` is kMinBlep or a copy of it in fast memory. An inactive channel writes zeros.
  // OS-CPU10 A2: flash (the reference of run_q15 and what tests/paula_oracle_test.cpp holds against pt2-clone); the render calls run_q15.
  O3OS_AUDIO_COLD void run(float* out, std::uint32_t n, const float* blep) noexcept;
  // The render's pass (OS-CPU10 A2): run(), then `f.process()`, then to_q15() on each sample, in one loop and with no float block in between: the filter's state is `f`'s, the
  // result is bit-equal to the three passes (the model's flags are read once, the arithmetic is process()'s expression for expression). An inactive channel feeds zeros through
  // the filter (its tail rings out). The samples go to out[0..n).
  O3OS_AUDIO_HOT void run_q15(std::int16_t* out, std::uint32_t n, const float* blep, Filter& f) noexcept;

  // One float to Q15, rounded and saturated.
  O3OS_AUDIO_INLINE static std::int16_t to_q15(float x) noexcept {
    const float y = x * 32768.0f;
    const std::int32_t v = static_cast<std::int32_t>(y + (y >= 0.0f ? 0.5f : -0.5f));
    return static_cast<std::int16_t>(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
  }

 private:
  // The sample loop shared by run() and run_q15() (OS-CPU10 A3): the phase, the step, the held value, the ring's index and count live in locals, the fetch of the next sample is
  // inline, and `sink.put(float)` takes each output. Defined in paula.cpp, where both are.
  template <class Sink>
  O3OS_AUDIO_INLINE void generate(Sink& sink, std::uint32_t n, const float* blep) noexcept;
  // The BLEP step into the ring at ring_index_ (A1: two straight runs of taps, four at a time; the ring's slots and the order of the arithmetic are the original's).
  O3OS_AUDIO_HOT void blep_add(const float* table, float offset, float amplitude) noexcept;

  const std::int16_t* data_ = nullptr;
  std::uint32_t loc_ = 0;          // next frame to fetch
  std::uint32_t left_ = 0;         // frames left in the segment being played
  std::uint32_t loop_start_ = 0;
  std::uint32_t loop_len_ = 0;     // < 2: silence after the first pass
  std::uint32_t period_ = 0;       // as written (clamped)
  float vol_ = 0.0f;               // stored volume / 8192, read at the fetch
  float sample_ = 0.0f;            // the held value, volume applied
  float delta_ = 0.0f;
  float phase_ = 0.0f;
  float blep_delta_ = 0.0f;
  float blep_phase_ = 0.0f;
  float stored_delta_ = 0.0f;
  float last_ = 0.0f;              // the value the ring's steps lead to
  float ring_[kBlepRing] = {};
  std::int32_t ring_index_ = 0;
  std::int32_t ring_left_ = 0;
  bool next_stage_ = false;
  bool silent_ = false;            // past the end of a one shot
  bool fading_ = false;
  bool active_ = false;
};

}  // namespace o3os::audio::paula
