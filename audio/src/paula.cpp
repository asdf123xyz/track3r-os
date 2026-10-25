// SPDX-License-Identifier: GPL-3.0-or-later
// The Paula model: see audio/paula.h. A re-implementation of the model of pt2-clone's Paula emulator (BSD-3-Clause, (c) 2010-2026 Olav Sorensen;
// THIRD_PARTY.md): the arithmetic is kept in the original's order so that the host test, which runs both, sees the same floats.
#include <o3os/audio/paula.h>

namespace o3os::audio::paula {
namespace {

using coef::kA1200Hi;
using coef::kA500Hi;
using coef::kA500Lo;
using coef::kLed;

}  // namespace

// ---- tables and periods --------------------------------------------------------------------------------------------------------

std::uint32_t note_period(int note, int finetune) noexcept {
  const int ft = finetune < -8 ? -8 : (finetune > 7 ? 7 : finetune);
  const std::uint16_t* row = kPeriodTable[ft < 0 ? ft + 16 : ft];
  const int idx = note - static_cast<int>(kTableFirstNote);
  std::uint32_t p;
  if (idx < 0) {
    const int k = (11 - idx) / 12;  // octaves below the table
    p = static_cast<std::uint32_t>(row[idx + 12 * k]) << k;
  } else if (idx >= static_cast<int>(kTableNotes)) {
    const int k = (idx - static_cast<int>(kTableNotes)) / 12 + 1;  // octaves above it
    p = (static_cast<std::uint32_t>(row[idx - 12 * k]) + (1u << (k - 1))) >> k;
  } else {
    p = row[idx];
  }
  return p < kMinPeriod ? kMinPeriod : (p > kMaxPeriod ? kMaxPeriod : p);
}

// ---- the filters ---------------------------------------------------------------------------------------------------------------

void Filter::clear() noexcept {
  lo_ = hi_ = 0.0f;
  led_s_[0] = led_s_[1] = led_s_[2] = led_s_[3] = 0.0f;
}

void Filter::set(Model model, bool led) noexcept {
  if (led != led_) {
    led_s_[0] = led_s_[1] = led_s_[2] = led_s_[3] = 0.0f;
  }
  model_ = model;
  led_ = led;
}

void Filter::process(float* x, std::uint32_t n) noexcept {
  if (model_ == Model::Off && !led_) {
    return;
  }
  const bool low = model_ == Model::A500;
  const bool high = model_ != Model::Off;
  const coef::OnePole hp = model_ == Model::A1200 ? kA1200Hi : kA500Hi;
  float lo = lo_, hi = hi_;
  float x1 = led_s_[0], x2 = led_s_[1], y1 = led_s_[2], y2 = led_s_[3];
  for (std::uint32_t i = 0; i < n; ++i) {
    float v = x[i];
    if (low) {
      lo = (v * kA500Lo.a0) + (lo * kA500Lo.b1);
      v = lo;
    }
    if (led_) {
      const float out = (v * kLed.a1) + (x1 * kLed.a2) + (x2 * kLed.a1) - (y1 * kLed.b1) - (y2 * kLed.b2);
      x2 = x1;
      x1 = v;
      y2 = y1;
      y1 = out;
      v = out;
    }
    if (high) {
      hi = (v * hp.a0) + (hi * hp.b1);
      v = v - hi;
    }
    x[i] = v;
  }
  lo_ = lo;
  hi_ = hi;
  led_s_[0] = x1;
  led_s_[1] = x2;
  led_s_[2] = y1;
  led_s_[3] = y2;
}

void Filter::insert(void* state, std::int16_t* io, std::uint32_t n) noexcept {
  Filter& f = *static_cast<Filter*>(state);
  float tmp[16];
  for (std::uint32_t base = 0; base < n; base += 16u) {
    const std::uint32_t m = n - base < 16u ? n - base : 16u;
    for (std::uint32_t i = 0; i < m; ++i) {
      tmp[i] = static_cast<float>(io[base + i]) * (1.0f / 32768.0f);
    }
    f.process(tmp, m);
    for (std::uint32_t i = 0; i < m; ++i) {
      io[base + i] = Channel::to_q15(tmp[i]);
    }
  }
}

// ---- the channel ---------------------------------------------------------------------------------------------------------------

void Channel::start(const std::int16_t* data, std::uint32_t begin, std::uint32_t end, std::uint32_t loop_start, std::uint32_t loop_end) noexcept {
  data_ = data;
  loc_ = begin;
  left_ = end > begin ? end - begin : 0u;
  loop_start_ = loop_start;
  loop_len_ = loop_end > loop_start ? loop_end - loop_start : 0u;
  silent_ = false;
  fading_ = false;
  // refetchPeriod(), then the phase restarts: the first sample is fetched at once.
  blep_phase_ = phase_;
  blep_delta_ = delta_;
  delta_ = stored_delta_;
  next_stage_ = true;
  phase_ = 0.0f;
  active_ = data != nullptr;
}

void Channel::set_period(std::uint32_t period) noexcept {
  std::uint32_t p = period;
  if (p == 0u) {
    p = 65536u;
  } else if (p < kMinPeriod) {
    p = kMinPeriod;
  }
  period_ = p;
  stored_delta_ = kPeriodToDeltaDiv / static_cast<float>(p);
  if (blep_delta_ == 0.0f) {
    blep_delta_ = delta_;  // the original's edge case
  }
}

void Channel::fade_out() noexcept {
  vol_ = 0.0f;
  if (last_ != 0.0f) {
    blep_add(kMinBlep, 0.0f, last_);
    last_ = 0.0f;
  }
  sample_ = 0.0f;
  fading_ = true;
}

// OS-CPU10 A1. The 16 taps go to 16 consecutive ring slots from ring_index_, wrapping at 32: at most two straight runs (the slots of a run are plain pointer steps, no mask per
// tap), each in groups of four with the table's reads (a tap's two points are 16 floats from the previous tap's) all taken before the four stores. Every slot gets the original's
// expression, `ring + amplitude * (s0 + ((s1 - s0) * f))`, from the same two table points: the output is bit-equal (tests/paula_oracle_test.cpp, tolerance 0).
void Channel::blep_add(const float* table, float offset, float amplitude) noexcept {
  float f = offset * static_cast<float>(kBlepSp);
  const std::int32_t f_int = static_cast<std::int32_t>(f);
  const float* src = table + f_int;
  f -= static_cast<float>(f_int);
  const std::uint32_t i = static_cast<std::uint32_t>(ring_index_) & (kBlepRing - 1u);
  const std::uint32_t first = kBlepRing - i < kBlepNs ? kBlepRing - i : kBlepNs;  // taps before the ring wraps
  float* dst = ring_ + i;
  std::uint32_t n = first;
  for (unsigned run = 0; run < 2u; ++run) {
    for (; n >= 4u; n -= 4u) {
      const float a0 = src[0], b0 = src[1];
      const float a1 = src[kBlepSp], b1 = src[kBlepSp + 1u];
      const float a2 = src[2u * kBlepSp], b2 = src[2u * kBlepSp + 1u];
      const float a3 = src[3u * kBlepSp], b3 = src[3u * kBlepSp + 1u];
      dst[0] += amplitude * (a0 + ((b0 - a0) * f));
      dst[1] += amplitude * (a1 + ((b1 - a1) * f));
      dst[2] += amplitude * (a2 + ((b2 - a2) * f));
      dst[3] += amplitude * (a3 + ((b3 - a3) * f));
      src += 4u * kBlepSp;
      dst += 4;
    }
    for (; n > 0u; --n) {
      dst[0] += amplitude * (src[0] + ((src[1] - src[0]) * f));
      src += kBlepSp;
      ++dst;
    }
    n = kBlepNs - first;  // the rest, from the ring's start
    dst = ring_;
  }
  ring_left_ = static_cast<std::int32_t>(kBlepNs);
}

// OS-CPU10 A3. The sample loop of the original's order (fetch at the wrap of the phase, the ring added to the held value, the phase stepped), with the hot state in locals: the
// phase, the step, the held value, the BLEP's phase and step, the ring's index and count. They are loaded once, written back once. The fetch (the former next_sample) is inline.
// `sink.put(s)` takes each output sample (a float into a block, or the fused filter and Q15 conversion).
template <class Sink>
O3OS_AUDIO_INLINE void Channel::generate(Sink& sink, std::uint32_t n, const float* blep) noexcept {
  if (!active_) {
    for (std::uint32_t j = 0; j < n; ++j) {
      sink.put(0.0f);
    }
    return;
  }
  float phase = phase_;
  float delta = delta_;
  float sample = sample_;
  float bphase = blep_phase_;
  float bdelta = blep_delta_;
  std::int32_t ri = ring_index_;
  std::int32_t rleft = ring_left_;
  bool stage = next_stage_;
  const float stored = stored_delta_;
  for (std::uint32_t j = 0; j < n; ++j) {
    if (stage) {
      stage = false;
      std::int32_t b = 0;
      if (!silent_) {
        if (left_ == 0u) {
          if (loop_len_ < 2u) {
            silent_ = true;
          } else {
            loc_ = loop_start_;
            left_ = loop_len_;
          }
        }
        if (!silent_) {
          b = static_cast<std::int32_t>(data_[loc_]) >> 8;
          ++loc_;
          --left_;
        }
      }
      sample = static_cast<float>(b) * vol_;
      if (sample != last_) {
        if (bdelta > bphase) {
          ring_index_ = ri;  // blep_add reads it
          blep_add(blep, bphase / bdelta, last_ - sample);
          rleft = static_cast<std::int32_t>(kBlepNs);
        }
        last_ = sample;
      }
    }
    float s = sample;
    if (rleft > 0) {
      s += ring_[ri];
      ring_[ri] = 0.0f;
      ri = (ri + 1) & static_cast<std::int32_t>(kBlepRing - 1u);
      --rleft;
    }
    sink.put(s);
    phase += delta;
    if (phase >= 1.0f) {
      phase -= 1.0f;
      bphase = phase;
      bdelta = delta;
      delta = stored;
      stage = true;
    }
  }
  phase_ = phase;
  delta_ = delta;
  sample_ = sample;
  blep_phase_ = bphase;
  blep_delta_ = bdelta;
  ring_index_ = ri;
  ring_left_ = rleft;
  next_stage_ = stage;
  // Over: faded out, or the end of a one shot with nothing left to ring.
  if (rleft == 0 && last_ == 0.0f && (fading_ || silent_)) {
    active_ = false;
  }
}

namespace {

// The float block (Channel::run).
struct FloatSink {
  float* out;
  O3OS_AUDIO_INLINE void put(float v) noexcept { *out++ = v; }
};

// The filter's arithmetic of Filter::process, expression for expression, then Channel::to_q15, for one sample at a time: the model's flags are read once, the state is in
// locals and goes back to the filter at the end. Off with the LED off changes nothing (process() returns at once), as here with all three flags clear.
struct FusedSink {
  std::int16_t* out;
  bool low, led, high;
  float lo, hi, x1, x2, y1, y2;
  coef::OnePole hp;

  O3OS_AUDIO_INLINE void put(float v) noexcept {
    if (low) {
      lo = (v * kA500Lo.a0) + (lo * kA500Lo.b1);
      v = lo;
    }
    if (led) {
      const float o = (v * kLed.a1) + (x1 * kLed.a2) + (x2 * kLed.a1) - (y1 * kLed.b1) - (y2 * kLed.b2);
      x2 = x1;
      x1 = v;
      y2 = y1;
      y1 = o;
      v = o;
    }
    if (high) {
      hi = (v * hp.a0) + (hi * hp.b1);
      v = v - hi;
    }
    *out++ = Channel::to_q15(v);
  }
};

}  // namespace

void Channel::run(float* out, std::uint32_t n, const float* blep) noexcept {
  FloatSink sink{out};
  generate(sink, n, blep);
}

void Channel::run_q15(std::int16_t* out, std::uint32_t n, const float* blep, Filter& f) noexcept {
  FusedSink sink{out, f.model_ == Model::A500, f.led_, f.model_ != Model::Off, f.lo_, f.hi_, f.led_s_[0], f.led_s_[1], f.led_s_[2], f.led_s_[3],
                 f.model_ == Model::A1200 ? kA1200Hi : kA500Hi};
  generate(sink, n, blep);
  f.lo_ = sink.lo;
  f.hi_ = sink.hi;
  f.led_s_[0] = sink.x1;
  f.led_s_[1] = sink.x2;
  f.led_s_[2] = sink.y1;
  f.led_s_[3] = sink.y2;
}

}  // namespace o3os::audio::paula
