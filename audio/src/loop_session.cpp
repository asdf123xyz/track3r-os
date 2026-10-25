// SPDX-License-Identifier: GPL-3.0-or-later
// The loop player's overdub session (OS-LOOPER L2c): the state machine around the kernels of loop_player.cpp. In flash at the default optimisation level: a command is taken a few times a second, the
// end of a block does a few tests, the hot step (write: one read-modify-write of the frames just played) calls the -O3 kernels. See loop_player.h and docs/specs/OS-LOOPER.md 3c.
#include <o3os/audio/loop_player.h>

#include <o3os/audio/loop_overdub.h>
#include <o3os/audio/simd.h>

#include <cstdint>

namespace o3os::audio {
namespace {
using simd::NativeOps;
}  // namespace

// An overdub command, in the block it is taken (a release in this block wins: no session starts and nothing is restored). Acknowledged by flush_ack() once the flags it caused are published.
void LoopPlayer::take_command(std::uint32_t oc, bool dropping, std::uint32_t frames) noexcept {
  const std::uint32_t seq = oc >> 4;
  const std::uint32_t kind = oc & 15u;
  const std::uint32_t fade_c = frames < kFadeFrames ? frames : kFadeFrames;
  ack_due_ = seq;
  if (!dropping) {
    if (kind == kCmdOn) {
      if (!ov_on_ && !restoring_) {  // a new session drops the old undo: no layer until its first copy
        ov_on_ = true;
        ov_end_ = false;
        layer_ = false;
      }
    } else if (ov_on_) {  // quiesce, or undo: the session punches out, and for undo the restore follows
      end_session(fade_c);
      undo_after_ = kind == kCmdUndo;
    } else if (kind == kCmdUndo && layer_ && !restoring_) {
      start_restore();
    }
  }
}

// The end of a serve(): the punch-out that has finished ends the session (before any restore step), a restore step runs once the loop is silent, and the flags go out before the acknowledgement.
void LoopPlayer::end_block(CycleSource cycles, std::uint32_t block_start) noexcept {
  if (ov_end_ && (wleft_ == 0u || !running_ || pos_ >= len_)) {  // the punch-out is done (or nothing more will be written): the session ends here, before any restore step
    ov_on_ = false;
    ov_end_ = false;
    wg_ = 0;
    wdg_ = 0;
    wt_ = 0;
    wleft_ = 0;
    if (undo_after_) {
      undo_after_ = false;
      if (layer_) {
        start_restore();
      }
    }
  }
  if (restoring_ && gain_ == 0 && ramp_left_ == 0u) {
    restore_blocks(cycles, block_start);
  }
  publish_flags();
  flush_ack();
}

// ---- The overdub session (OS-LOOPER L2c; the header and spec 3c.2 to 3c.6) ------------------------------------------------------------------

void LoopPlayer::publish_flags() noexcept {
  const std::uint8_t f = static_cast<std::uint8_t>((ov_on_ ? kFlagOverdub : 0u) | (restoring_ ? kFlagRestore : 0u) | (layer_ ? kFlagLayer : 0u));
  if (f != flags_last_) {
    flags_last_ = f;
    flags_pub_.store(f, std::memory_order_release);
  }
}

// The acknowledgement goes out after the flags it caused (publish_flags() first) and, for a command that ends the session, only once it has ended.
void LoopPlayer::flush_ack() noexcept {
  if (ack_due_ != 0u && !ov_end_) {
    ack_.store(ack_due_, std::memory_order_release);
    ack_due_ = 0;
  }
}

void LoopPlayer::begin_wramp(std::int32_t target, std::uint32_t frames) noexcept {
  wt_ = target;
  const std::int32_t step = frames != 0u ? (target - wg_) / static_cast<std::int32_t>(frames) : 0;
  if (step == 0) {
    wg_ = target;
    wdg_ = 0;
    wleft_ = 0;
    return;
  }
  wdg_ = step;
  wleft_ = frames;
}

void LoopPlayer::wsync(std::uint32_t fade) noexcept {
  const bool want = ov_on_ && !ov_end_ && running_ && !silent_;
  const std::int32_t t = want ? kFbUnity : 0;
  if (silent_) {  // a muted render writes nothing: the write gain is 0 and ramps in again when the sound is back
    wg_ = 0;
    wdg_ = 0;
    wt_ = 0;
    wleft_ = 0;
  } else if (t != wt_) {
    begin_wramp(t, fade);
  }
}

void LoopPlayer::end_session(std::uint32_t fade) noexcept {
  ov_end_ = true;
  if (!running_ || pos_ >= len_) {  // nothing will be written before the loop restarts: the session ends at the end of this block
    wg_ = 0;
    wdg_ = 0;
    wt_ = 0;
    wleft_ = 0;
  } else {
    begin_wramp(0, fade);
  }
}

void LoopPlayer::start_restore() noexcept {
  restoring_ = true;
  rc_ = 0;
  skips_ = 0;
  begin_ramp(0, kFadeFrames);  // the loop fades out; the copy-back begins once it is silent (the block after this one)
}

// One render block of the restore. The step is bounded three ways: kRestoreStepCopies blocks copied, kRestoreStepWords bitmap halfwords read, kRestoreCycles spent after the first copy. A block that
// has used more than kRestoreSkipCycles skips it, but never more than kRestoreForceAfter in a row.
void LoopPlayer::restore_blocks(CycleSource cycles, std::uint32_t block_start) noexcept {
  const std::uint32_t blocks = (len_ + loop_ov::kBlockFrames - 1u) / loop_ov::kBlockFrames;
  if (cycles != nullptr && skips_ < kRestoreForceAfter && cycles() - block_start > kRestoreSkipCycles) {
    ++skips_;
    return;
  }
  skips_ = 0;
  const std::uint32_t start = cycles != nullptr ? cycles() : 0u;
  std::uint32_t words = loop_ov::kRestoreStepWords;
  for (std::uint32_t i = 0; i < loop_ov::kRestoreStepCopies && rc_ < blocks && words != 0u; ++i) {
    const std::uint32_t next = loop_ov::loop_restore_step(rw_, undo_, bm_, len_, rc_, 1u, words);
    const std::uint32_t read = (next >> 4) - (rc_ >> 4) + 1u;  // (the halfwords looked at, rounded up)
    words = read >= words ? 0u : words - read;
    rc_ = next;
    if (cycles != nullptr && cycles() - start >= kRestoreCycles) {
      break;
    }
  }
  if (rc_ >= blocks) {  // every marked block is back: the layer is gone, and the loop returns at its next restart
    restoring_ = false;
    layer_ = false;
    resume_ = true;
    rc_ = 0;
  }
}

// Frames [pos0, end) of the loop get the input frames from `from`: each 64-frame block that the session has not yet touched is copied to the undo buffer first (and its bit set), then the
// read-modify-write: through the ramp while the write gain moves (punch in / out, a Stop, a Play), by the kernels at rest.
void LoopPlayer::write(std::uint32_t from, std::uint32_t pos0, std::uint32_t end) noexcept {
  // A punch-in ramp starts at g = 0: its first frame is written back unchanged (kept = x, added = 0), so it alone marks no block and sets no layer (F5 would light with nothing to undo).
  const std::uint32_t touch0 = wg_ == 0 && wleft_ != 0u ? pos0 + 1u : pos0;
  for (std::uint32_t b = touch0 / loop_ov::kBlockFrames; touch0 < end && b * loop_ov::kBlockFrames < end; ++b) {
    std::uint16_t& w = bm_[b >> 4];
    const std::uint32_t bit = 1u << (b & 15u);
    if ((w & bit) == 0u) {
      const std::uint32_t at = b * loop_ov::kBlockFrames;
      const std::uint32_t rest = len_ - at;
      loop_ov::loop_block_copy(undo_ + at, rw_ + at, rest < loop_ov::kBlockFrames ? rest : loop_ov::kBlockFrames);
      w = static_cast<std::uint16_t>(w | bit);
      layer_ = true;
    }
  }
  std::int16_t* lp = rw_ + pos0;
  const std::int16_t* ip = in_ + from;
  std::uint32_t n = end - pos0;
  if (wleft_ != 0u) {
    const std::uint32_t r = n < wleft_ ? n : wleft_;
    ramp_rmw(lp, ip, r);
    lp += r;
    ip += r;
    n -= r;
    wleft_ -= r;
    if (wleft_ == 0u) {
      wg_ = wt_;
      wdg_ = 0;
    }
  }
  if (n != 0u && wg_ != 0) {
    if (fb_blk_ >= kFbUnity) {
      loop_ov::loop_rmw_add(lp, ip, n);
    } else {
      loop_ov::loop_rmw(lp, ip, n, fb_blk_);
    }
  }
}

// The frames of a punch in / out: input and feedback both move with the write gain g (0: the loop is kept whole, no input; unity: the session's feedback and the whole input), so that neither
// the layer nor the decay begins with a step.
void LoopPlayer::ramp_rmw(std::int16_t* lp, const std::int16_t* ip, std::uint32_t n) noexcept {
  std::int32_t g = wg_;
  const std::int32_t inv = kFbUnity - fb_blk_;  // the part of the loop the feedback takes away at full write gain
  for (std::uint32_t i = 0; i < n; ++i, g += wdg_) {
    const std::int32_t fbe = kFbUnity - (((inv >> 1) * (g >> 1)) >> 14);  // inv x g >> 16
    const std::int32_t kept = (static_cast<std::int32_t>(lp[i]) * fbe) >> 16;
    const std::int32_t add = (static_cast<std::int32_t>(ip[i]) * g) >> 16;
    lp[i] = static_cast<std::int16_t>(NativeOps::ssat16(kept + add));
  }
  wg_ = g;
}

}  // namespace o3os::audio
