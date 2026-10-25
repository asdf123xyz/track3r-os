// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/audio/loop_player.h>

#include <o3os/audio/kernels.h>
#include <o3os/audio/loop_mix.h>
#include <o3os/audio/loop_overdub.h>

#include <o3os/audio/simd.h>

#include <cstdint>
#include <cstring>

namespace o3os::audio {
namespace {
using simd::NativeOps;  // SSAT #16 on the M4F, its portable model elsewhere (audio/simd.h)
}  // namespace

namespace loop_mix {

namespace {
// One frame: both channels of the TX frame get the value (the packed saturating add equals ssat16(o + v) on each lane, for v inside int16).
O3OS_AUDIO_INLINE void add_frame(std::int16_t* o, std::int32_t v) noexcept {
  std::uint32_t w;
  std::memcpy(&w, o, sizeof w);
  const std::uint32_t both = static_cast<std::uint32_t>(static_cast<std::uint16_t>(v)) * 0x10001u;  // (a signed v * 0x10001 would smear the sign into the high lane)
  w = NativeOps::qadd16(w, both);
  std::memcpy(o, &w, sizeof w);
}

template <bool Ramp>
std::int32_t take_t(std::int16_t* o, const std::int16_t* p, std::uint32_t n, std::int32_t g, std::int32_t step) noexcept {
  if (n != 0u && (reinterpret_cast<std::uintptr_t>(p) & 2u) != 0u) {  // the take is only 2-aligned here: one frame by halfword, then words
    add_frame(o, (static_cast<std::int32_t>(*p) * g) >> 16);
    ++p;
    o += 2;
    --n;
    if (Ramp) {
      g += step;
    }
  }
  for (; n >= 2u; n -= 2u, p += 2, o += 4) {
    std::uint32_t w;
    std::memcpy(&w, p, sizeof w);  // two frames of the take: SMULWB / SMULWT read the halves and give (g * sample) >> 16 exactly
    const std::int32_t g1 = Ramp ? g + step : g;
    add_frame(o, NativeOps::smlawb(g, w, 0));
    add_frame(o + 2, NativeOps::smlawt(g1, w, 0));
    if (Ramp) {
      g = g1 + step;
    }
  }
  if (n != 0u) {
    add_frame(o, (static_cast<std::int32_t>(*p) * g) >> 16);
    if (Ramp) {
      g += step;
    }
  }
  return g;
}
}  // namespace

std::int32_t take(std::int16_t* out, const std::int16_t* p, std::uint32_t n, std::int32_t g, std::int32_t step) noexcept {
  static_assert(kernel::kUnity == 65536, "the loop's gain is Q16 unity at most: (p * g) >> 16 stays inside int16");
  return step != 0 ? take_t<true>(out, p, n, g, step) : take_t<false>(out, p, n, g, step);
}

}  // namespace loop_mix

void LoopPlayer::drop() noexcept {
  st_ = State::Idle;
  running_ = false;
  pcm_ = nullptr;
  len_ = 0;
  pos_ = 0;
  steps_ = 0;
  per_loop_ = 0;
  gain_ = 0;
  dgain_ = 0;
  target_ = 0;
  ramp_left_ = 0;
  restart_at_ = kNoOffset;
  stop_at_ = kNoOffset;
  ov_on_ = false;
  ov_end_ = false;
  undo_after_ = false;
  restoring_ = false;
  resume_ = false;
  layer_ = false;
  rw_ = nullptr;
  undo_ = nullptr;
  bm_ = nullptr;
  wg_ = 0;
  wdg_ = 0;
  wt_ = 0;
  wleft_ = 0;
  rc_ = 0;
  skips_ = 0;
  frames_pub_.store(0, std::memory_order_relaxed);
  publish_flags();
  publish(Phase::Idle);  // the render no longer reads the take's memory, and nothing writes it from here on
  flush_ack();
}

void LoopPlayer::begin_ramp(std::int32_t target, std::uint32_t frames) noexcept {
  target_ = target;
  const std::int32_t step = frames != 0u ? (target - gain_) / static_cast<std::int32_t>(frames) : 0;
  if (step == 0) {  // nothing to move, or less than a Q16 unit a frame: there
    gain_ = target;
    dgain_ = 0;
    ramp_left_ = 0;
    return;
  }
  dgain_ = step;
  ramp_left_ = frames;
}

void LoopPlayer::on_step(std::uint32_t step, std::uint32_t offset) noexcept {
  if (st_ != State::Loop || parked_) {
    return;
  }
  if (!running_) {
    // The transport was halted: the loop resumes on the first bar after it starts again (a start from a row in the middle of a bar waits for the next one).
    if ((step & (kStepsPerBar - 1u)) == 0u) {
      restart_at_ = offset;
      steps_ = 0;
    }
    return;
  }
  if (++steps_ >= per_loop_) {  // the loop's last step has gone by: the bar of its start
    restart_at_ = offset;
    steps_ = 0;
  }
}

void LoopPlayer::on_stopped(std::uint32_t offset) noexcept {
  if (st_ == State::Loop && running_ && !parked_) {
    stop_at_ = offset;
  }
}

void LoopPlayer::mix(std::int16_t* out, std::uint32_t from, std::uint32_t to) noexcept {
  const std::uint32_t n = to - from;
  if (n == 0u) {
    return;
  }
  const std::uint32_t len = len_;
  if (gain_ == 0 && ramp_left_ == 0) {  // nothing to hear: the position still goes with the transport
    if (running_) {
      const std::uint32_t room = len + kHoldFrames - pos_;
      pos_ += n < room ? n : room;
    }
    return;
  }
  std::int32_t g = gain_;
  std::uint32_t pos = pos_;
  const std::int16_t* const p = pcm_;
  std::int16_t* o = out + 2u * from;
  // One pass over `count` frames at a gain that moves by `step` a frame: the take's frames in a loop with no test in it, then the tail past its end (a bar a sample longer than the take): the last frame held for
  // kHoldFrames, then silence (a slower tempo's gap).
  const auto body = [&](std::uint32_t count, std::int32_t step) {
    std::uint32_t whole = pos < len ? len - pos : 0u;
    whole = whole < count ? whole : count;
    count -= whole;
    if (whole != 0u) {  // the take's frames: loop_mix::take(): a register loop, no state through references
      g = loop_mix::take(o, p + pos, whole, g, step);
      pos += whole;
      o += 2u * whole;
    }
    std::uint32_t held = pos < len + kHoldFrames ? len + kHoldFrames - pos : 0u;
    held = held < count ? held : count;
    count -= held;
    for (; held != 0u; --held, o += 2, g += step) {
      const std::int32_t v = (static_cast<std::int32_t>(p[len - 1u]) * g) >> 16;
      ++pos;
      o[0] = static_cast<std::int16_t>(NativeOps::ssat16(o[0] + v));
      o[1] = static_cast<std::int16_t>(NativeOps::ssat16(o[1] + v));
    }
    o += 2u * count;  // silence: the gain still moves
    g += step * static_cast<std::int32_t>(count);
  };
  const std::uint32_t ramped = n < ramp_left_ ? n : ramp_left_;
  body(ramped, dgain_);
  if (ramped != 0u) {
    ramp_left_ -= ramped;
    if (ramp_left_ == 0u) {  // the ramp ends on its target exactly
      g = target_;
      dgain_ = 0;
    }
  }
  body(n - ramped, 0);
  gain_ = g;
  pos_ = pos;
}

void LoopPlayer::service(std::int16_t* out, std::uint32_t frames, bool silence, std::int32_t gain_q16, const RecTap& rec, const std::int16_t* in, CycleSource cycles,
                         std::uint32_t block_start) noexcept {
  go_.fetch_and(static_cast<std::uint8_t>(~kGoRequest), std::memory_order_acq_rel);  // (before the exchange: a request that lands after it sets the flag again)
  in_ = in;
  serve(out, frames, silence, gain_q16, rec, cycles, block_start);
  in_ = nullptr;
  if (st_ != State::Idle) {
    go_.fetch_or(kGoActive, std::memory_order_release);
  } else {
    go_.fetch_and(static_cast<std::uint8_t>(~kGoActive), std::memory_order_release);
  }
}

void LoopPlayer::serve(std::int16_t* out, std::uint32_t frames, bool silence, std::int32_t gain_q16, const RecTap& rec, CycleSource cycles, std::uint32_t block_start) noexcept {
  const std::uint32_t rq = req_.exchange(0u, std::memory_order_acq_rel);
  const std::uint32_t oc = ovc_.load(std::memory_order_relaxed) != 0u ? ovc_.exchange(0u, std::memory_order_acq_rel) : 0u;  // an overdub command (post())
  bool dropping = false;  // SAVE / DISCARD: the loop fades out over this block, then the buffer is let go
  if (rq == kRelease && st_ == State::Loop && !silence && gain_ != 0) {
    dropping = true;
  } else if (rq != 0u) {
    drop();
    if (rq != kRelease) {
      per_loop_ = rq;
      st_ = State::Capturing;
      publish(Phase::Capturing);
    }
  }

  // The take of a loop ends on its boundary: from that sample the loop plays. Any other end (F1, the arena full, the transport stopped) is no loop (the recorder gives the take up).
  std::uint32_t begin = kNoOffset;
  if (st_ == State::Capturing && rec.state() == RecTap::State::Done) {
    if (rec.ended_on_boundary() && rec.overrun_frames() == 0u && rec.frames() != 0u && rec.data() != nullptr) {
      pcm_ = rec.data();
      len_ = rec.frames();
      rw_ = const_cast<std::int16_t*>(pcm_);  // the take's memory is the recorder's own, writable: the session is its only writer
      const std::uint32_t even_len = (len_ + 1u) & ~1u;
      undo_ = rw_ + even_len;                                  // SampleBank::take_end(frames, frames + bitmap): the undo buffer, then the bitmap
      bm_ = reinterpret_cast<std::uint16_t*>(rw_ + 2u * even_len);
      pos_ = 0;
      steps_ = 0;
      running_ = false;
      st_ = State::Loop;
      begin = rec.end_offset() < frames ? rec.end_offset() : frames - 1u;
      frames_pub_.store(len_, std::memory_order_relaxed);
      publish(Phase::Ready);
    } else {
      drop();
    }
  }
  if (st_ != State::Loop) {
    restart_at_ = kNoOffset;
    stop_at_ = kNoOffset;
    if (oc != 0u) {  // no loop: a command has nothing to do
      ack_due_ = oc >> 4;
      flush_ack();
    }
    return;
  }
  silent_ = silence;
  fb_blk_ = static_cast<std::int32_t>(fb_.load(std::memory_order_relaxed));
  if (fb_blk_ > kFbUnity) {
    fb_blk_ = kFbUnity;
  }
  if (oc != 0u) {  // an overdub command (loop_session.cpp)
    take_command(oc, dropping, frames);
  }

  // The events of the block, in order: the restart (a Step event, or the take's end) and the stop. The transport's start and stop within one block are not a case worth a rule: they apply in order.
  std::uint32_t rs = begin != kNoOffset ? begin : restart_at_;
  std::uint32_t sp = stop_at_;
  restart_at_ = kNoOffset;
  stop_at_ = kNoOffset;
  if (silence) {  // a muted render: nothing of the loop is heard, whatever ramp was running
    gain_ = 0;
    dgain_ = 0;
    target_ = 0;
    ramp_left_ = 0;
  }
  // LOOP VOL times the master level is at most Q16 unity by construction (the level curve and the master gain both are); mul_q16 clamps higher (kMaxGain), so the bound is made real here: loop_mix::take's
  // packed add is exact only for a gain of unity or less.
  const std::int32_t heard = silence || dropping || muted_.load(std::memory_order_relaxed) ? 0 : (gain_q16 > kernel::kUnity ? kernel::kUnity : (gain_q16 < 0 ? 0 : gain_q16));  // LOOP VOL times the master level, or nothing
  const std::int32_t level = restoring_ || resume_ ? 0 : heard;  // (the loop is silent while it is restored, and until its next restart after)
  const std::uint32_t fade = frames < kFadeFrames ? frames : kFadeFrames;
  if (dropping) {  // a release: no event matters any more, the loop fades out and the buffer goes at the end of the block
    rs = kNoOffset;
    sp = kNoOffset;
    running_ = false;
    begin_ramp(0, fade);
  } else if (running_ && ramp_left_ == 0u && gain_ != level) {
    begin_ramp(level, frames);  // LOOP VOL, the mute and the master level move the gain over one block
  }

  if (dropping) {  // (the session writes nothing in the block that releases the loop)
    ov_on_ = false;
  } else if (ov_on_) {
    wsync(fade);
  }

  std::uint32_t at = 0;
  for (;;) {
    const bool has_rs = rs != kNoOffset;
    const bool has_sp = sp != kNoOffset;
    if (!has_rs && !has_sp) {
      break;
    }
    const bool restart = has_rs && (!has_sp || rs <= sp);
    const std::uint32_t when = restart ? rs : sp;
    run(out, at, when);
    at = when;
    if (restart) {
      if (!running_) {  // a start from nothing (the take's end, a Play after a Stop): ramped in, so that it never begins with a step
        running_ = true;
        begin_ramp(restoring_ ? 0 : heard, fade);
      } else if (resume_) {  // the restore is done: the loop comes back at its restart
        begin_ramp(heard, fade);
      }
      resume_ = false;
      pos_ = 0;  // (a wrap is a dry cut: the loop's own restart)
      rs = kNoOffset;
      if (ov_on_) {
        wsync(fade);  // the write resumes with the transport, ramped in
      }
    } else {
      running_ = false;  // a Stop: fades out over kFadeFrames, into the next block when it is late in this one
      begin_ramp(0, kFadeFrames);
      sp = kNoOffset;
      if (ov_on_) {
        wsync(kFadeFrames);  // the session stays on; its write ramps out with the play head
      }
    }
  }
  run(out, at, frames);
  if (dropping) {
    drop();
    return;
  }
  end_block(cycles, block_start);  // the session's end, the restore step, the flags and the acknowledgement (loop_session.cpp)
}

// ---- The overdub session: its state machine is loop_session.cpp (flash, -Os: it runs a few times a second); only the hot step stays here ---------------------

void LoopPlayer::run(std::int16_t* out, std::uint32_t from, std::uint32_t to) noexcept {
  const std::uint32_t pos0 = pos_;
  mix(out, from, to);
  if (ov_on_ && in_ != nullptr && !silent_ && (wg_ != 0 || wleft_ != 0u) && pos0 < len_) {
    const std::uint32_t end = pos_ < len_ ? pos_ : len_;
    if (end > pos0) {
      write(from, pos0, end);
    }
  }
}

// ---- The overdub kernels (OS-LOOPER L2b; loop_overdub.h) ---------------------------------------------------------------------------------------

namespace loop_ov {

namespace {

// One frame: ssat16(((x * g) >> 16) + y). Mul false: ssat16(x + y).
template <bool Mul>
O3OS_AUDIO_INLINE std::int16_t rmw_one(std::int16_t x, std::int16_t y, std::int32_t g) noexcept {
  const std::int32_t kept = Mul ? (static_cast<std::int32_t>(x) * g) >> 16 : static_cast<std::int32_t>(x);
  return static_cast<std::int16_t>(NativeOps::ssat16(kept + y));
}

// The word body: `loop` is 4-aligned; `in` is 4-aligned (InWord) or only 2-aligned (two halfwords packed). Two frames a step: SMULWB / SMULWT keep the loop's lanes (Mul), PKHBT repacks them
// (each is inside int16 for g <= unity), one QADD16 adds the input's lanes with saturation; a trailing odd frame goes through rmw_one.
template <bool Mul, bool InWord>
void rmw_words(std::int16_t* loop, const std::int16_t* in, std::uint32_t n, std::int32_t g) noexcept {
  for (; n >= 2u; n -= 2u, loop += 2, in += 2) {
    std::uint32_t x;
    std::memcpy(&x, loop, sizeof x);
    std::uint32_t y;
    if (InWord) {
      std::memcpy(&y, in, sizeof y);
    } else {
      y = NativeOps::pack(in[0], in[1]);
    }
    if (Mul) {
      x = NativeOps::pack(NativeOps::smlawb(g, x, 0), NativeOps::smlawt(g, x, 0));
    }
    x = NativeOps::qadd16(x, y);
    std::memcpy(loop, &x, sizeof x);
  }
  if (n != 0u) {
    *loop = rmw_one<Mul>(*loop, *in, g);
  }
}

template <bool Mul>
void rmw_t(std::int16_t* loop, const std::int16_t* in, std::uint32_t n, std::int32_t g) noexcept {
  if (n != 0u && (reinterpret_cast<std::uintptr_t>(loop) & 2u) != 0u) {  // the loop is only 2-aligned here (an odd start): one frame by halfword, then words
    *loop = rmw_one<Mul>(*loop, *in, g);
    ++loop;
    ++in;
    --n;
  }
  if ((reinterpret_cast<std::uintptr_t>(in) & 2u) == 0u) {
    rmw_words<Mul, true>(loop, in, n, g);
  } else {
    rmw_words<Mul, false>(loop, in, n, g);
  }
}

}  // namespace

void loop_rmw(std::int16_t* loop, const std::int16_t* in, std::uint32_t n, std::int32_t fb) noexcept {
  rmw_t<true>(loop, in, n, fb < 0 ? 0 : (fb > 65536 ? 65536 : fb));  // (outside 0..65536 the packed lanes would truncate: clamped here, two instructions)
}

void loop_rmw_add(std::int16_t* loop, const std::int16_t* in, std::uint32_t n) noexcept { rmw_t<false>(loop, in, n, 0); }

void loop_block_copy(std::int16_t* dst, const std::int16_t* src, std::uint32_t n) noexcept { std::memcpy(dst, src, static_cast<std::size_t>(n) * sizeof(std::int16_t)); }

std::uint32_t loop_restore_step(std::int16_t* loop, const std::int16_t* undo, std::uint16_t* bitmap, std::uint32_t len, std::uint32_t from, std::uint32_t max_copies,
                                std::uint32_t max_words) noexcept {
  const std::uint32_t blocks = (len + kBlockFrames - 1u) / kBlockFrames;
  std::uint32_t b = from;
  max_copies = max_copies != 0u ? max_copies : 1u;  // a budget of 0 would make no progress for ever: a step always does one copy's and one word's worth
  max_words = max_words != 0u ? max_words : 1u;
  while (b < blocks && max_copies != 0u && max_words != 0u) {
    const std::uint32_t w = b >> 4;
    const std::uint32_t bits = static_cast<std::uint32_t>(bitmap[w]) >> (b & 15u);  // the blocks b .. the end of this halfword
    if (bits == 0u) {
      b = (w + 1u) << 4;  // a clean run to the next halfword
      --max_words;
      continue;
    }
    b += static_cast<std::uint32_t>(__builtin_ctz(bits));
    if (b >= blocks) {
      break;
    }
    const std::uint32_t at = b * kBlockFrames;
    const std::uint32_t rest = len - at;
    loop_block_copy(loop + at, undo + at, rest < kBlockFrames ? rest : kBlockFrames);
    bitmap[b >> 4] = static_cast<std::uint16_t>(bitmap[b >> 4] & ~(1u << (b & 15u)));
    ++b;
    --max_copies;
  }
  return b < blocks ? b : blocks;
}

}  // namespace loop_ov

}  // namespace o3os::audio
