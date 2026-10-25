// SPDX-License-Identifier: GPL-3.0-or-later
//
// See open303_ref.h. Open303Probe's control() is the first half of rosic::Open303::getSample() (third_party/open303/rosic_Open303.h, Robin Schmidt, MIT)
// written out against the protected members: the envelope and cutoff arithmetic, line for line, so that the port's envelopes can be held to it
// sample by sample.
#include "open303_ref.h"

#include "rosic_BlendOscillator.h"
#include "rosic_BiquadFilter.h"
#include "rosic_MipMappedWaveTable.h"
#include "rosic_OnePoleFilter.h"
#include "rosic_Open303.h"
#include "rosic_TeeBeeFilter.h"

namespace open303_ref {

namespace {

class Probe : public rosic::Open303 {
 public:
  void control(double& inst_cutoff, double& amp_out) {
    double instFreq = pitchSlewLimiter.getSample(oscFreq);
    oscillator.setFrequency(instFreq * pitchWheelFactor);
    oscillator.calculateIncrement();
    double mainEnvOut = mainEnv.getSample();
    double tmp1 = n1 * rc1.getSample(mainEnvOut);
    double tmp2 = 0.0;
    if (accentGain > 0.0) {
      tmp2 = mainEnvOut;
    }
    tmp2 = n2 * rc2.getSample(tmp2);
    tmp1 = envScaler * (tmp1 - envOffset);
    tmp2 = accentGain * tmp2;
    inst_cutoff = cutoff * pow(2.0, tmp1 + tmp2);
    double ampEnvOut = ampEnv.getSample();
    if (ampEnv.isNoteOn()) {
      ampEnvOut += (0.45 + 4 * accentGain) * mainEnvOut;
    }
    amp_out = ampDeClicker.getSample(ampEnvOut);
  }
};

}  // namespace

struct Voice::Impl {
  Probe o;
};
Voice::Voice(double fs) : impl_(new Impl) {
  impl_->o.setSampleRate(fs);
  impl_->o.setSlideTime(60.0);  // the constructor leaves the slew's own time constant at 60 ms, setSlideTime makes it 0.2 x: the module's parameter
}
Voice::~Voice() = default;
void Voice::set_waveform(double v) { impl_->o.setWaveform(v); }
void Voice::set_cutoff(double v) { impl_->o.setCutoff(v); }
void Voice::set_resonance(double v) { impl_->o.setResonance(v); }
void Voice::set_env_mod(double v) { impl_->o.setEnvMod(v); }
void Voice::set_decay(double v) { impl_->o.setDecay(v); }
void Voice::set_accent(double v) { impl_->o.setAccent(v); }
void Voice::set_slide_time(double v) { impl_->o.setSlideTime(v); }
void Voice::set_volume(double v) { impl_->o.setVolume(v); }
void Voice::note_on(int note, int velocity) { impl_->o.noteOn(note, velocity); }
double Voice::sample() { return impl_->o.getSample(); }
void Voice::control(double& cutoff, double& amp) { impl_->o.control(cutoff, amp); }

struct Ladder::Impl {
  rosic::TeeBeeFilter f;
};
Ladder::Ladder(double fs) : impl_(new Impl) {
  impl_->f.setSampleRate(fs);
  impl_->f.setMode(rosic::TeeBeeFilter::TB_303);
}
Ladder::~Ladder() = default;
void Ladder::set_resonance(double v) { impl_->f.setResonance(v); }
void Ladder::set_cutoff(double v) { impl_->f.setCutoff(v); }
double Ladder::sample(double in) { return impl_->f.getSample(in); }

struct OnePole::Impl {
  rosic::OnePoleFilter f;
};
OnePole::OnePole(char mode, double hz, double fs) : impl_(new Impl) {
  impl_->f.setSampleRate(fs);
  impl_->f.setMode(mode == 'h' ? rosic::OnePoleFilter::HIGHPASS : rosic::OnePoleFilter::ALLPASS);
  impl_->f.setCutoff(hz);
}
OnePole::~OnePole() = default;
double OnePole::sample(double in) { return impl_->f.getSample(in); }

struct Biquad::Impl {
  rosic::BiquadFilter f;
};
Biquad::Biquad(char mode, double hz, double bw, double gain_db, double fs) : impl_(new Impl) {
  impl_->f.setSampleRate(fs);
  impl_->f.setMode(mode == 'n' ? rosic::BiquadFilter::BANDREJECT : rosic::BiquadFilter::LOWPASS12);
  impl_->f.setGain(gain_db);
  impl_->f.setFrequency(hz);
  impl_->f.setBandwidth(bw);
}
Biquad::~Biquad() = default;
double Biquad::sample(double in) { return impl_->f.getSample(in); }

struct Oscillator::Impl {
  rosic::MipMappedWaveTable t1, t2;
  rosic::BlendOscillator osc;
};
Oscillator::Oscillator(double fs, double blend) : impl_(new Impl) {
  impl_->osc.setWaveTable1(&impl_->t1);
  impl_->osc.setWaveForm1(rosic::MipMappedWaveTable::SAW303);
  impl_->osc.setWaveTable2(&impl_->t2);
  impl_->osc.setWaveForm2(rosic::MipMappedWaveTable::SQUARE303);
  impl_->osc.setSampleRate(fs);
  impl_->osc.setPulseWidth(50.0);
  impl_->osc.setBlendFactor(blend);
  impl_->osc.resetPhase();
}
Oscillator::~Oscillator() = default;
void Oscillator::set_frequency(double hz) {
  impl_->osc.setFrequency(hz);
  impl_->osc.calculateIncrement();
}
double Oscillator::sample() { return impl_->osc.getSample(); }

}  // namespace open303_ref
