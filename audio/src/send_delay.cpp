// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/audio/send_delay.h>

#include <o3os/audio/hot.h>
#include <o3os/audio/send_fx_util.h>

namespace o3os::audio {

namespace {

// The engine's rate (hal/audio.h: the one constant), as float, computed by the compiler.
constexpr float kFs = static_cast<float>(hal::audio::sample_rate_mhz()) * 1.0e-3f;
constexpr float kFramesPerMs = kFs * 1.0e-3f;
// Synced time in frames = rate96 / 96 steps x (15 / bpm) s x fs = rate96 x kSyncK / (bpm x 100): a step is a 16th note, 15 / bpm seconds.
constexpr float kSyncK = 1500.0f * kFs / 96.0f;

constexpr float kDcPole = 0.995f;  // the feedback loop's DC blocker: y = x - x1 + R y1, a 35 Hz high-pass
constexpr float kSlew = 0.2f;      // the per-block one-pole of the feedback gain and the tone (about 7 ms)

constexpr std::uint32_t kMaxRate = sendfx::kDelayRateCount - 1u;

using sendfx_util::flush;
using sendfx_util::lane_l;
using sendfx_util::lane_r;
using sendfx_util::pack;
using sendfx_util::trunc16;
O3OS_AUDIO_INLINE float slew(float now, float target) noexcept { return sendfx_util::slew(now, target, kSlew); }

}  // namespace

void SendDelay::attach(std::uint32_t* ring, std::uint32_t frames) noexcept {
  ring_ = frames >= 4u * kBlock ? ring : nullptr;
  frames_ = ring_ != nullptr ? frames : 0u;
  for (std::uint32_t i = 0; i < frames_; ++i) {
    ring_[i] = 0u;
  }
  clearing_ = false;
  reset_state();
  time_ms_ = sendfx::kDelayTimeDefault;
  feedback_pct_ = sendfx::kDelayFeedbackDefault;
  tone_ = 100.0f;
  tempo_x100_ = 12000.0f;
  sync_ = rate_ = ping_ = mode_ = 0;
  dirty_ = true;
}

// The state of a delay whose ring is all zero: no time yet, no loop state, nothing to do until the first sound (the parameters are not touched).
void SendDelay::reset_state() noexcept {
  pos_ = 0;
  cur_l_ = cur_r_ = old_l_ = old_r_ = tgt_l_ = tgt_r_ = 0;
  xf_left_ = 0;
  gain_ = 0.0f;
  lp_a_ = 1.0f;
  for (unsigned c = 0; c < 2u; ++c) {
    lp_[c] = hp_x_[c] = hp_y_[c] = 0.0f;
  }
  dirty_ = true;
  zero_run_ = frames_;  // the ring is all zero: nothing to do until the first sound (the quiet state, see the header)
  idle_ = ring_ != nullptr;
}

void SendDelay::begin_clear() noexcept {
  if (ring_ != nullptr && !idle_ && !clearing_) {  // (a delay that sleeps holds nothing: the ring is all zero already)
    clearing_ = true;
    clear_pos_ = 0;
    idle_ = false;
  }
}

bool SendDelay::set_param(unsigned field, float v) noexcept {
  switch (field) {
    case sendfx::kClearField:
      if (v != 1.0f) return false;
      begin_clear();
      return true;
    case sendfx::delay::Time:
      if (!(v >= sendfx::kDelayTimeMin && v <= sendfx::kDelayTimeMax)) return false;
      time_ms_ = v;
      break;
    case sendfx::delay::Feedback:
      if (!(v >= 0.0f && v <= 100.0f)) return false;
      feedback_pct_ = v;
      break;
    case sendfx::delay::Sync:
      if (v != 0.0f && v != 1.0f) return false;
      sync_ = v != 0.0f ? 1u : 0u;
      break;
    case sendfx::delay::Rate:
      if (!(v >= 0.0f && v <= static_cast<float>(kMaxRate))) return false;
      rate_ = static_cast<std::uint8_t>(v + 0.5f);
      break;
    case sendfx::delay::PingPong:
      if (v != 0.0f && v != 1.0f) return false;
      ping_ = v != 0.0f ? 1u : 0u;
      break;
    case sendfx::delay::Tone:
      if (!(v >= 0.0f && v <= 100.0f)) return false;
      tone_ = v;
      break;
    case sendfx::delay::Mode:
      if (v != 0.0f && v != 1.0f) return false;
      mode_ = v != 0.0f ? 1u : 0u;
      break;
    case sendfx::kTempoField:
      if (!(v >= 1000.0f && v <= 100000.0f)) return false;  // 10 .. 1000 BPM
      tempo_x100_ = v;
      break;
    default:
      return false;
  }
  dirty_ = true;
  return true;
}

bool SendDelay::finite() const noexcept {
  bool ok = __builtin_isfinite(gain_) && __builtin_isfinite(lp_a_);
  for (unsigned c = 0; c < 2u; ++c) {
    ok = ok && __builtin_isfinite(lp_[c]) && __builtin_isfinite(hp_x_[c]) && __builtin_isfinite(hp_y_[c]);
  }
  return ok;
}

// The two times in frames from the parameters: [kMinDelay, ring - kMinDelay], the right one 3/4 of the left in STEREO with PING-PONG off.
void SendDelay::retarget() noexcept {
  const float d = sync_ != 0u ? static_cast<float>(sendfx::kDelayRates96[rate_]) * kSyncK / tempo_x100_ : time_ms_ * kFramesPerMs;
  const std::uint32_t lo = kMinDelay;
  const std::uint32_t hi = frames_ - kMinDelay;
  std::uint32_t dl = d >= static_cast<float>(hi) ? hi : static_cast<std::uint32_t>(d + 0.5f);
  dl = dl < lo ? lo : (dl > hi ? hi : dl);
  std::uint32_t dr = dl;
  if (ping_ == 0u && mode_ != 0u) {
    dr = (3u * dl + 2u) >> 2;
    dr = dr < lo ? lo : dr;
  }
  tgt_l_ = dl;
  tgt_r_ = dr;
}

// n frames of the ring from frame `from` (wrapping) into stage_.
void SendDelay::stage_read(std::uint32_t from, std::uint32_t n) noexcept {
  const std::uint32_t head = frames_ - from;
  const std::uint32_t first = n < head ? n : head;
  for (std::uint32_t i = 0; i < first; ++i) {
    stage_[i] = ring_[from + i];
  }
  for (std::uint32_t i = first; i < n; ++i) {
    stage_[i] = ring_[i - first];
  }
}

// stage_ into the ring from frame `at` (wrapping).
void SendDelay::stage_write(std::uint32_t at, std::uint32_t n) noexcept {
  const std::uint32_t head = frames_ - at;
  const std::uint32_t first = n < head ? n : head;
  for (std::uint32_t i = 0; i < first; ++i) {
    ring_[at + i] = stage_[i];
  }
  for (std::uint32_t i = first; i < n; ++i) {
    ring_[i - first] = stage_[i];
  }
}

// The n frames this block reads: the left channel `dl` frames behind the write position and the right one `dr`, as floats in int16 units. A delay of at least kBlock frames reads
// nothing this block writes.
void SendDelay::read_taps(float* l, float* r, std::uint32_t dl, std::uint32_t dr, std::uint32_t n) noexcept {
  std::uint32_t from = pos_ + frames_ - dl;
  from = from >= frames_ ? from - frames_ : from;
  stage_read(from, n);
  if (dl == dr) {
    for (std::uint32_t i = 0; i < n; ++i) {
      l[i] = lane_l(stage_[i]);
      r[i] = lane_r(stage_[i]);
    }
    return;
  }
  for (std::uint32_t i = 0; i < n; ++i) {
    l[i] = lane_l(stage_[i]);
  }
  from = pos_ + frames_ - dr;
  from = from >= frames_ ? from - frames_ : from;
  stage_read(from, n);
  for (std::uint32_t i = 0; i < n; ++i) {
    r[i] = lane_r(stage_[i]);
  }
}

void SendDelay::process(const std::int16_t* in, std::int16_t* out_l, std::int16_t* out_r, std::uint32_t n) noexcept {
  n = n < kBlock ? n : kBlock;
  if (ring_ == nullptr) {
    for (std::uint32_t i = 0; i < n; ++i) {
      out_l[i] = 0;
      out_r[i] = 0;
    }
    return;
  }
  if (clearing_) {  // a project load: a slice of the ring a block, the return silent, the dry untouched
    const std::uint32_t end = clear_pos_ + kClearFrames < frames_ ? clear_pos_ + kClearFrames : frames_;
    for (std::uint32_t i = clear_pos_; i < end; ++i) {
      ring_[i] = 0u;
    }
    clear_pos_ = end;
    if (clear_pos_ >= frames_) {
      clearing_ = false;
      reset_state();
    }
    for (std::uint32_t i = 0; i < n; ++i) {
      out_l[i] = 0;
      out_r[i] = 0;
    }
    return;
  }
  if (idle_) {
    std::int32_t any = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
      any |= in[i];
    }
    if (any == 0) {
      for (std::uint32_t i = 0; i < n; ++i) {
        out_l[i] = 0;
        out_r[i] = 0;
      }
      return;
    }
    idle_ = false;
    zero_run_ = 0;
  }

  // ---- control rate: the time, the gain and the tone, once for the block ------------------------------------------------------------------------------
  if (dirty_) {
    retarget();
    dirty_ = false;
  }
  const bool first = cur_l_ == 0u;
  if (first) {
    cur_l_ = tgt_l_;
    cur_r_ = tgt_r_;
  } else if (xf_left_ == 0u && (tgt_l_ != cur_l_ || tgt_r_ != cur_r_)) {
    old_l_ = cur_l_;
    old_r_ = cur_r_;
    cur_l_ = tgt_l_;
    cur_r_ = tgt_r_;
    xf_left_ = kXfade;
  }
  const float g_target = feedback_pct_ * 0.01f < kMaxFeedback ? feedback_pct_ * 0.01f : kMaxFeedback;
  const float u = tone_ * 0.01f;
  const float a_target = 0.04f + 0.96f * u * u;  // the low-pass pole's step: 0.04 (about 290 Hz) .. 1 (open)
  const float g_next = first ? g_target : slew(gain_, g_target);
  if (first) {
    gain_ = g_target;
    lp_a_ = a_target;
  } else {
    lp_a_ = slew(lp_a_, a_target);
  }

  // ---- the taps -----------------------------------------------------------------------------------------------------------------------------------
  read_taps(lo_, ro_, cur_l_, cur_r_, n);
  if (xf_left_ != 0u) {
    read_taps(xl_, xr_, old_l_, old_r_, n);
    const float step = 1.0f / static_cast<float>(kXfade);
    float f = static_cast<float>(kXfade - xf_left_) * step;  // the share of the new taps at the block's first frame
    for (std::uint32_t i = 0; i < n; ++i) {
      const float fn = f < 1.0f ? f : 1.0f;
      const float fo = 1.0f - fn;
      lo_[i] = fn * lo_[i] + fo * xl_[i];  // a linear pair: the gains sum to 1, so correlated taps keep their level and the loop gain stays below unity through the fade
      ro_[i] = fn * ro_[i] + fo * xr_[i];
      f += step;
    }
    xf_left_ = xf_left_ > n ? xf_left_ - n : 0u;
  }

  // ---- the loop ---------------------------------------------------------------------------------------------------------------------------------
  const float a = lp_a_;
  const float dg = (g_next - gain_) * (1.0f / static_cast<float>(n));
  float g = gain_;
  float lpl = lp_[0], lpr = lp_[1];
  float xl1 = hp_x_[0], xr1 = hp_x_[1];
  float yl1 = hp_y_[0], yr1 = hp_y_[1];
  const bool ping = ping_ != 0u;
  std::uint32_t any = 0;
  for (std::uint32_t i = 0; i < n; ++i) {
    const float lo = lo_[i];
    const float ro = ro_[i];
    out_l[i] = static_cast<std::int16_t>(trunc16(lo));
    out_r[i] = static_cast<std::int16_t>(trunc16(ro));
    lpl += a * (lo - lpl);
    lpr += a * (ro - lpr);
    const float yl = lpl - xl1 + kDcPole * yl1;
    const float yr = lpr - xr1 + kDcPole * yr1;
    xl1 = lpl;
    xr1 = lpr;
    yl1 = yl;
    yr1 = yr;
    g += dg;
    const float fl = g * yl;
    const float fr = g * yr;
    const float x = static_cast<float>(in[i]);
    const float wl = ping ? x + fr : x + fl;
    const float wr = ping ? fl : x + fr;
    const std::uint32_t w = pack(trunc16(wl), trunc16(wr));
    stage_[i] = w;
    any |= w;
  }
  gain_ = g_next;
  lp_[0] = flush(lpl);
  lp_[1] = flush(lpr);
  hp_x_[0] = flush(xl1);
  hp_x_[1] = flush(xr1);
  hp_y_[0] = flush(yl1);
  hp_y_[1] = flush(yr1);

  stage_write(pos_, n);
  pos_ += n;
  pos_ = pos_ >= frames_ ? pos_ - frames_ : pos_;

  // The whole ring is zero once frames_ zero frames were written in a row: the effect may sleep (nothing it would still say is lost).
  if (any == 0u) {
    zero_run_ = zero_run_ + n < frames_ ? zero_run_ + n : frames_;
    if (zero_run_ >= frames_) {
      idle_ = true;
      xf_left_ = 0;
      for (unsigned c = 0; c < 2u; ++c) {
        lp_[c] = hp_x_[c] = hp_y_[c] = 0.0f;
      }
    }
  } else {
    zero_run_ = 0;
  }
}

}  // namespace o3os::audio
