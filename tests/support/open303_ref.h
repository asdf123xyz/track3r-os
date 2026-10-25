// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The double-precision oracle of the TB-303 module (OS-012a, the dsp-port skill's gate 2): wrappers over the vendored Open303 classes
// (third_party/open303, MIT, Robin Schmidt), so that a test sees plain types and none of the original's global macros (PI, TINY, INLINE, EPS ...).
// Host only. Each wrapper is the original object configured the way rosic::Open303's constructor configures it.

#include <cstdint>
#include <memory>

namespace open303_ref {

// rosic::Open303 itself, with the parameters of the module; sample() is its getSample().
class Voice {
 public:
  explicit Voice(double sample_rate);
  ~Voice();
  void set_waveform(double blend);
  void set_cutoff(double hz);
  void set_resonance(double percent);
  void set_env_mod(double percent);
  void set_decay(double ms);
  void set_accent(double percent);
  void set_slide_time(double ms);
  void set_volume(double db);
  void note_on(int note, int velocity);  // velocity 0 is a note off, as in the original
  double sample();
  // The first half of getSample() (the envelopes, the cutoff's modulation and the de-clicked amplitude), on the same object, advancing it by one
  // sample: `cutoff` is the filter's instantaneous cutoff in Hz, `amp` the amplitude after the de-clicker. Do not mix with sample().
  void control(double& cutoff, double& amp);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// rosic::TeeBeeFilter in TB_303 mode.
class Ladder {
 public:
  explicit Ladder(double sample_rate);
  ~Ladder();
  void set_resonance(double percent);
  void set_cutoff(double hz);
  double sample(double in);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// rosic::OnePoleFilter (mode 'h' high-pass, 'a' all-pass) and rosic::BiquadFilter ('n' band-reject, 'l' 12 dB low-pass with the given Q in dB gain).
class OnePole {
 public:
  OnePole(char mode, double cutoff_hz, double sample_rate);
  ~OnePole();
  double sample(double in);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
class Biquad {
 public:
  Biquad(char mode, double frequency_hz, double bandwidth_octaves, double q_gain_db, double sample_rate);
  ~Biquad();
  double sample(double in);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// rosic::BlendOscillator with the SAW303 and SQUARE303 mip-mapped tables, at `sample_rate` (Open303 runs it at 4 x 44.1 kHz).
class Oscillator {
 public:
  Oscillator(double sample_rate, double blend);
  ~Oscillator();
  void set_frequency(double hz);
  double sample();  // the next sample (phase starts at 0)

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace open303_ref
