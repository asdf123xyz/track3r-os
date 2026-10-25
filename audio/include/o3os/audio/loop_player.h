// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The loop player (OS-LOOPER slice L1, docs/specs/OS-LOOPER.md; NEW COMMUNITY OS BEHAVIOUR: a take can be a loop). A take made with LOOP = N BARS is exactly N bars of
// the sequencer's timeline (RecTap: it began at a bar's Step event and ended at the 16 N-th Step event after, to the sample), and this plays it back in time with the bars: from the sample
// the take ended on, again at every 16 N-th Step event, for as long as the transport runs.
//
// THE CLOCK. The position inside the loop is a sample counter, restarted by the sequencer's own Step events, not a free-running modulo of the take's length: the sequencer places a bar to a
// rounded sample (the Step event's offset), so the length of N bars is not always the same number of samples (one more or one less every few loops at a fractional bar length), and a counter
// that wrapped by itself would walk away from the bars by that much each time. Restarting at the event keeps the loop on the bars for ever (no drift), and between two restarts the counter is
// exactly (t - t_restart), which at a steady tempo is (t - t0) mod length. A tempo change leaves the length in samples as it was recorded: the loop restarts at each N-bar boundary of the new
// tempo, cutting the loop short (faster) or leaving a gap at its end (slower) - the loop is never stretched. Stopping the transport (the Stopped event) fades the loop out over kFadeFrames (32, 0.7 ms; the fade goes on into the next block when the stop is late in one); it resumes
// at its start on the first bar (Step event with a step index multiple of 16) after the transport starts again, ramped in over the same 32 frames (a loop that starts from nothing, after the take or after a Stop,
// never starts with a step; the restarts of its own wrap are dry cuts, as a sample loop is). A restart that comes a frame or two after the end of the take (the bar a sample longer than the take) holds the take's last
// frame for kHoldFrames (2) before the silence of a longer gap, so the loop's contribution does not fall to zero for a frame (a tick) at a fractional tempo. Count-in steps never reach it (the engine does not offer them).
//
// ROUTING (ADR 0013 amendment, OS-LOOPER L1). The loop is added into the TX block, the interleaved pair the codec plays, AFTER every tap has read it (the same place as the metronome's click,
// metro.h): it is in no REC take, no RESAMPLE take (RecTap::Source::Master reads MAIN before this), no stem, no scope, no meter and no export, by construction - a RESAMPLE overdub cannot
// record the loop into itself (ADR 0012 amendment 6 states the same no-loopback rule for the USB return). It is heard on both channels of the line out and the headphones (MAIN on the left jack and CUE
// on the right one in the split OUT MODE: the mono loop goes to both) at LOOP VOL (the mixer level law, 100 = unity) times the master level, saturating. It bypasses the limiter and the master chain
// (they act on MAIN), as the click does.
//
// MEMORY. The loop reads the recorder's take where it lies in the sample arena (the held tail it was recorded into). The main loop must not give that memory to anything until the render has let go:
// release() is a request, reading() goes false when the render has dropped the buffer. OVERDUB (L2c, 3c of the spec): behind the take the bank holds an undo buffer as long as the loop and a bitmap of one bit
// per 64-frame block (SampleBank::take_end's tail; the layout is undo = take + even(frames), bitmap = take + 2 even(frames)); while a session is on, the render writes the loop in place and the first
// write to a block of the session copies it to the undo buffer and sets its bit; UNDO LAYER puts the marked blocks back, a few a render block. The loop is written ONLY inside serve(), after the
// block has been mixed, and never in a block that releases the loop: nothing is written after phase() says Idle.
//
// OVERDUB. The write head IS the play head (no latency constant: every layer lands exactly on the layer under it, whatever the source's round trip): loop[pos] = sat(loop[pos] x FEEDBACK + input[pos]),
// where `input` is the block of the REC bus (or MAIN, for RESAMPLE) as the tap records it. The main loop posts commands (overdub_on / quiesce / undo: one word, the latest wins, so a quiesce beats an
// overdub-on that was not yet taken) and watches the acknowledgement (acked(seq): the render publishes the sequence number of the last command it has completed, flags first, so a command that ends the
// session is acked only when the punch-out ramp is done and no frame will be written); the published flags are overdubbing() (a session is on, until its punch-out has finished), restoring() and layer()
// (a layer exists to undo). Punch in and out ramp the input and the feedback over kFadeFrames (no click); a Stop of the transport keeps the session (the write ramps out with the play head and ramps
// in again on the first bar after Play). UNDO = the session ends, the loop fades out, the marked blocks are copied back in steps bounded by kRestoreStepCopies, kRestoreStepWords and kRestoreCycles,
// a block that has already used more than 70 % of its cycles (kRestoreSkipCycles) skips its step but never more than kRestoreForceAfter blocks in a row, and the loop fades in at its next restart.
//
// CONTEXTS. The main loop calls capture(), release(), set_volume() and set_muted() (atomics: one word each, a relaxed store the render reads once a block; capture / release go through one
// request word the render takes with an exchange) and reads phase() (published by the render with release). Everything else is the render's alone: on_step() and on_stopped() from the engine's event
// dispatch, service() from Graph::render() once a block while needs_service(). The three are in flash (O3OS_AUDIO_COLD, the exception list of hot.h): on_step() once per Step event, service() once
// per block while a loop exists or is being captured; the render in SRAM_L pays a test and a call. No allocation, no blocking, one bounded loop of a block's frames, no double.

#include <o3os/audio/hot.h>
#include <o3os/audio/taps.h>

#include <atomic>
#include <cstdint>

namespace o3os::audio {

class LoopPlayer {
 public:
  static constexpr std::uint32_t kStepsPerBar = 16;  // four steps a beat, four beats a bar: the sequencer's grid (RecTap Align::Bar)
  static constexpr std::uint32_t kMaxBars = 16;
  static constexpr std::uint32_t kMaxSteps = kStepsPerBar * kMaxBars;
  static constexpr std::uint32_t kNoOffset = 0xFFFFFFFFu;
  static constexpr std::uint32_t kFadeFrames = 32;  // the fade out of a Stop or a release, the ramp in of a start from nothing
  static constexpr std::uint32_t kHoldFrames = 2;   // the take's last frame held past its end, before silence
  static constexpr std::int32_t kFbUnity = 65536;   // FEEDBACK 100 %, Q16 (the loop's gain scale, kernel::kUnity)
  // The restore (UNDO LAYER): a render block copies back at most kRestoreStepCopies blocks of 64 frames and reads at most kRestoreStepWords bitmap halfwords (loop_overdub.h), and stops copying once it has
  // spent kRestoreCycles (6,000: confirmed on the unit, restore_step avg 2.81k / max 4.34k cycles, about a 38 % margin). A block that has already used more than 70 % of its 261,120 cycles
  // (kRestoreSkipCycles) skips its step, but the ninth block in a row does it whatever the load (kRestoreForceAfter skips at most): the restore always ends.
  static constexpr std::uint32_t kRestoreCycles = 6000;
  static constexpr std::uint32_t kRestoreSkipCycles = 182784;
  static constexpr std::uint32_t kRestoreForceAfter = 8;
  using CycleSource = std::uint32_t (*)() noexcept;  // DWT CYCCNT (audio::CycleFn has the same type)

  // What the render publishes: Capturing (a loop take was announced, the render waits for its end), Ready (the loop exists: the render reads the take's memory).
  enum class Phase : std::uint8_t { Idle = 0, Capturing = 1, Ready = 2 };

  // ---- Main loop ----
  // Announces a loop take, to be armed right after: when it ends at its `steps`-th Step event (RecTap auto_steps, 16 N) the loop starts at that very sample. 1..kMaxSteps; anything else is ignored.
  void capture(std::uint32_t steps) noexcept {
    if (steps >= 1u && steps <= kMaxSteps) {
      req_.store(steps, std::memory_order_release);
      go_.fetch_or(kGoRequest, std::memory_order_release);
    }
  }
  // Lets go of the loop (or of the announcement): the render drops the buffer at its next block, and phase() is Idle from then on.
  void release() noexcept {
    req_.store(kRelease, std::memory_order_release);
    go_.fetch_or(kGoRequest, std::memory_order_release);
  }
  // OVERDUB (L2c). Each posts one command and returns its sequence number (never 0), to be watched with acked(); a later command replaces an earlier one the render has not yet taken. overdub_on: begin a
  // session at the next block (the caller has cleared the bitmap, and refuses it while overdubbing(), restoring() or a command is unacked); quiesce: end the session with its punch-out ramp (nothing is
  // written once it is acked; with no session it is acked at the next block); undo: end the session, then restore the layer (ignored when no layer exists).
  std::uint32_t overdub_on() noexcept { return post(kCmdOn); }
  std::uint32_t quiesce() noexcept { return post(kCmdQuiesce); }
  std::uint32_t undo() noexcept { return post(kCmdUndo); }
  bool acked(std::uint32_t seq) const noexcept { return seq == 0u || ((ack_.load(std::memory_order_acquire) - seq) & kSeqMask) < 0x08000000u; }
  // FEEDBACK 0..100 (%): the part of the loop kept under the layer while the write head passes (100: the layer adds; 0: it replaces). Heard from the next block.
  void set_feedback(unsigned percent) noexcept { fb_.store(percent >= 100u ? static_cast<std::uint32_t>(kFbUnity) : percent * 65536u / 100u, std::memory_order_relaxed); }
  unsigned feedback() const noexcept { return (fb_.load(std::memory_order_relaxed) * 100u + 32768u) / 65536u; }
  bool overdubbing() const noexcept { return (flags_pub_.load(std::memory_order_acquire) & kFlagOverdub) != 0u; }
  bool restoring() const noexcept { return (flags_pub_.load(std::memory_order_acquire) & kFlagRestore) != 0u; }
  bool layer() const noexcept { return (flags_pub_.load(std::memory_order_acquire) & kFlagLayer) != 0u; }
  // LOOP VOL 0..100 (the mixer level law) and the mute (F8 LOOP ON / MUTED): heard from the next block, ramped over it.
  void set_volume(unsigned percent) noexcept { volume_.store(static_cast<std::uint8_t>(percent > 100u ? 100u : percent), std::memory_order_relaxed); }
  void set_muted(bool muted) noexcept { muted_.store(muted, std::memory_order_relaxed); }
  unsigned volume() const noexcept { return volume_.load(std::memory_order_relaxed); }
  bool muted() const noexcept { return muted_.load(std::memory_order_relaxed); }
  Phase phase() const noexcept { return static_cast<Phase>(phase_.load(std::memory_order_acquire)); }
  // The render reads the take's memory (or is about to): the arena tail it lies in is not free.
  bool reading() const noexcept { return phase() == Phase::Ready; }
  bool capturing() const noexcept { return phase() == Phase::Capturing; }
  // The loop's length in frames, once Ready (the take's).
  std::uint32_t frames() const noexcept { return frames_pub_.load(std::memory_order_relaxed); }
  // Main loop, only while the live render is parked (Engine::offline_enter / offline_leave, OS-WAVEXP): the export renders through the same graph and must hear no loop and move none. Parked, the
  // player does nothing (on_step, on_stopped and service are ignored, needs_service() is false); unparked it is halted - silent, back at the first bar of the live transport.
  void park(bool on) noexcept {
    parked_ = on;
    if (on) {
      go_.fetch_or(kGoParked, std::memory_order_release);
      // The render is stopped: the session and the restore end here (the layer written so far stays, and UNDO still undoes it), nothing is written, and every command is acknowledged.
      ov_on_ = false;
      ov_end_ = false;
      undo_after_ = false;
      restoring_ = false;
      resume_ = false;
      wg_ = 0;
      wdg_ = 0;
      wt_ = 0;
      wleft_ = 0;
      ack_due_ = 0;
      ovc_.store(0u, std::memory_order_relaxed);
      publish_flags();
      ack_.store(cmd_seq_, std::memory_order_release);
    } else {
      resume_ = false;
      go_.fetch_and(static_cast<std::uint8_t>(~kGoParked), std::memory_order_release);
      running_ = false;
      gain_ = 0;
      dgain_ = 0;
      target_ = 0;
      ramp_left_ = 0;
      restart_at_ = kNoOffset;
      stop_at_ = kNoOffset;
    }
  }

  // ---- Render ----
  // True when there is something for service() to do: a request waits, a take is being captured, or a loop exists, and no export has parked the player. One byte, one load, one compare (the flags of
  // go_ below: 1, 2 or 3 are work, 0 is none, a parked 4 and over is none): the render in SRAM_L pays this and the metronome's test, and one call to flash for both (Graph::mix_extras).
  O3OS_AUDIO_INLINE bool needs_service() const noexcept { return static_cast<unsigned>(go_.load(std::memory_order_relaxed)) - 1u < 3u; }
  // A Step event `offset` frames into the block about to be rendered (index `step` in its pattern; never a count-in's). While a loop exists: the restart at the 16 N-th event, or, with the transport
  // halted, at the first bar. In flash: once per Step event.
  O3OS_AUDIO_COLD void on_step(std::uint32_t step, std::uint32_t offset) noexcept;
  // The transport stopped `offset` frames into the block about to be rendered: the loop fades out. In flash: once per stop.
  O3OS_AUDIO_COLD void on_stopped(std::uint32_t offset) noexcept;
  // The graph computes the overdub's input block (the mono REC bus) only when this says it will be read: a session is on or a command waits.
  bool wants_input() const noexcept { return ov_on_ || ovc_.load(std::memory_order_relaxed) != 0u; }
  // Once a block, after the taps have read `out` (`frames` interleaved L, R frames, the codec's TX block): takes the requests, hands the take over when it has ended on its boundary, and adds
  // the loop (`gain_q16`: LOOP VOL times the master level, Q16; `silence`: the render is muted). Not for SRAM_L: Graph::mix_extras() calls it from flash.
  // `in`: the overdub's input for this block (`frames` mono frames; null: none, the session writes nothing); `cycles`, `block_start`: the DWT counter and its value at the start of the render block (the restore's
  // load guard; null: no guard, tests).
  void service(std::int16_t* out, std::uint32_t frames, bool silence, std::int32_t gain_q16, const RecTap& rec, const std::int16_t* in = nullptr, CycleSource cycles = nullptr,
               std::uint32_t block_start = 0) noexcept;

 private:
  enum class State : std::uint8_t { Idle, Capturing, Loop };
  static constexpr std::uint32_t kRelease = 0x80000000u;

  static constexpr std::uint8_t kGoRequest = 1;  // go_: a request waits in req_ (the main loop sets it; the render clears it as it takes the request)
  static constexpr std::uint8_t kGoActive = 2;   // go_: the render holds a capture or a loop (the render's)
  static constexpr std::uint8_t kGoParked = 4;   // go_: an export has the engine (park())

  static constexpr std::uint32_t kCmdOn = 1;       // ovc_: the command in its low 4 bits, its sequence number above
  static constexpr std::uint32_t kCmdQuiesce = 2;
  static constexpr std::uint32_t kCmdUndo = 3;
  static constexpr std::uint32_t kSeqMask = 0x0FFFFFFFu;
  static constexpr std::uint8_t kFlagOverdub = 1;  // flags_pub_
  static constexpr std::uint8_t kFlagRestore = 2;
  static constexpr std::uint8_t kFlagLayer = 4;

  void serve(std::int16_t* out, std::uint32_t frames, bool silence, std::int32_t gain_q16, const RecTap& rec, CycleSource cycles, std::uint32_t block_start) noexcept;
  void drop() noexcept;
  // The gain moves to `target` over `frames` frames from where it is (a ramp that is longer than what is left of the block goes on in the next one).
  void begin_ramp(std::int32_t target, std::uint32_t frames) noexcept;
  void publish(Phase p) noexcept { phase_.store(static_cast<std::uint8_t>(p), std::memory_order_release); }
  // Adds [from, to) of the block at the current position and gain ramp.
  void mix(std::int16_t* out, std::uint32_t from, std::uint32_t to) noexcept;
  // mix(), then the overdub's read-modify-write of the loop frames it has just played (the write head is the play head).
  void run(std::int16_t* out, std::uint32_t from, std::uint32_t to) noexcept;
  void take_command(std::uint32_t oc, bool dropping, std::uint32_t frames) noexcept;  // (loop_session.cpp from here on)
  void end_block(CycleSource cycles, std::uint32_t block_start) noexcept;
  void write(std::uint32_t from, std::uint32_t pos0, std::uint32_t end) noexcept;
  void ramp_rmw(std::int16_t* lp, const std::int16_t* ip, std::uint32_t n) noexcept;
  void begin_wramp(std::int32_t target, std::uint32_t frames) noexcept;
  void wsync(std::uint32_t fade) noexcept;  // the write gain goes where the session wants it (on while it runs with the transport, else off)
  void end_session(std::uint32_t fade) noexcept;
  void start_restore() noexcept;
  void restore_blocks(CycleSource cycles, std::uint32_t block_start) noexcept;
  void publish_flags() noexcept;
  void flush_ack() noexcept;
  std::uint32_t post(std::uint32_t kind) noexcept {
    cmd_seq_ = (cmd_seq_ + 1u) & kSeqMask;
    cmd_seq_ = cmd_seq_ != 0u ? cmd_seq_ : 1u;
    ovc_.store((cmd_seq_ << 4) | kind, std::memory_order_release);
    go_.fetch_or(kGoRequest, std::memory_order_release);
    return cmd_seq_;
  }

  // Main loop -> render (one writer each).
  std::atomic<std::uint32_t> req_{0};  // 0: none; kRelease; else a capture request, the Step events of the loop
  std::atomic<std::uint8_t> go_{0};    // kGoRequest (main loop), kGoActive (render), kGoParked (main loop, render stopped): see needs_service()
  std::atomic<std::uint8_t> volume_{100};
  std::atomic<bool> muted_{false};
  std::atomic<std::uint32_t> ovc_{0};  // the overdub command word (post()): the main loop stores, the render takes it with an exchange. A later store replaces an untaken one.
  std::atomic<std::uint32_t> fb_{65536};  // FEEDBACK, Q16
  std::uint32_t cmd_seq_ = 0;          // main loop only: the last sequence number posted
  // Render -> main loop.
  std::atomic<std::uint8_t> phase_{0};
  std::atomic<std::uint32_t> frames_pub_{0};
  std::atomic<std::uint8_t> flags_pub_{0};  // kFlag*: the render stores, the main loop reads (single writer; park() writes it only while the render is stopped)
  std::atomic<std::uint32_t> ack_{0};       // the sequence number of the last command completed (published after the flags it caused)
  // Render only.
  State st_ = State::Idle;
  bool parked_ = false;            // an export has the engine (park()): written by the main loop while the live render is stopped
  bool running_ = false;           // the transport runs and the loop plays (false: halted, or fading out)
  const std::int16_t* pcm_ = nullptr;
  std::uint32_t len_ = 0;          // the loop's frames (the take's)
  std::uint32_t pos_ = 0;          // the next frame to play; len_ is the end (the last frame held for kHoldFrames, then silence until the next restart)
  std::uint32_t steps_ = 0;        // Step events since the loop's last restart
  std::uint32_t per_loop_ = 0;     // Step events per loop (16 N)
  std::uint32_t restart_at_ = kNoOffset;  // this block: the Step event that restarts the loop
  std::uint32_t stop_at_ = kNoOffset;     // this block: the Stopped event
  std::int32_t gain_ = 0;          // Q16, at the start of the segment being run
  std::int32_t dgain_ = 0;         // Q16 per frame while a ramp runs
  std::int32_t target_ = 0;        // where the ramp in progress ends
  std::uint32_t ramp_left_ = 0;    // frames of the ramp still to go (it may cross into the next block)
  // The overdub session (render only; park() clears it while the render is stopped).
  bool ov_on_ = false;             // a session is on (until its punch-out has finished)
  bool ov_end_ = false;            // the session is punching out: it ends when the write gain has reached 0
  bool undo_after_ = false;        // UNDO asked while the session ran: the restore starts when it has ended
  bool restoring_ = false;         // the restore runs (the loop is silent)
  bool resume_ = false;            // the restore is done: the loop fades in at its next restart
  bool layer_ = false;             // a block of the session has been copied to the undo buffer
  bool silent_ = false;            // this block: the render is muted
  std::int16_t* rw_ = nullptr;     // the loop (the take's memory), writable by the session only
  std::int16_t* undo_ = nullptr;   // the undo buffer: len_ frames behind the take
  std::uint16_t* bm_ = nullptr;    // the bitmap behind the undo buffer
  const std::int16_t* in_ = nullptr;  // this block's input (service()), valid in serve() only
  std::int32_t fb_blk_ = kFbUnity; // FEEDBACK as read at the start of the block
  std::int32_t wg_ = 0;            // the write gain (input and feedback ramp), Q16, 0 or kFbUnity at rest
  std::int32_t wdg_ = 0;
  std::int32_t wt_ = 0;            // where its ramp ends
  std::uint32_t wleft_ = 0;        // frames of the write ramp still to go
  std::uint32_t ack_due_ = 0;      // the sequence number to acknowledge once the flags it caused are published (and the session has ended)
  std::uint32_t rc_ = 0;           // the restore's cursor: the next bitmap block to look at
  std::uint32_t skips_ = 0;        // blocks in a row whose restore step the load guard skipped
  std::uint8_t flags_last_ = 0xFF; // flags_pub_ as last stored
};

}  // namespace o3os::audio
