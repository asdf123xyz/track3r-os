// SPDX-License-Identifier: GPL-3.0-or-later
// The DJ page of Perform (OS-DJ A1, A2, app/dj.h, docs/specs/OS-DJ.md): the crossfader (the encoder and pad row 4), the groups (Shift + row 1), CURVE and RESET, the filter (F4 or the FILT pad held +
// the encoder) with LATCH, and the cut and the filter told to the engine.
// Runtime state only, in ViewState::dj: nothing here is undoable and nothing is stored. The engine hears the levels from App::poll_dj (dj_publish), once a pass.
#include "action_handlers.h"

#include <o3os/app/buttons.h>
#include <o3os/app/recorder.h>
#include <o3os/app/sample_bank.h>
#include <o3os/audio/graph_params.h>
#include <o3os/model/params.h>
#include <o3os/seq/perform.h>

namespace o3os::app {

namespace {

constexpr unsigned kKeyBankDown = 0;  // F1
constexpr unsigned kKeyBankUp = 1;    // F2
constexpr unsigned kKeyCurve = 2;   // F3
constexpr unsigned kKeyFilter = 3;  // F4
constexpr unsigned kKeyScr = 4;     // F5
constexpr unsigned kKeyLatch = 5;   // F6
constexpr unsigned kKeyReset = 7;   // F8

bool on_dj(const ActionContext& ctx) noexcept { return ctx.view.dj_page(); }

bool dj_say(ActionContext& ctx, const char* text) noexcept {
  (void)ctx.view.notify.post(core::Severity::Info, text, "dj", ViewState::kMessageMs);
  return true;
}

// C1: SCR (what = On) or TAPE STOP (Tape) takes the arena's free tail as the ring and tells the engine. The arena has one owner at a time (the looper's take or the ring): the second to ask is refused with its name.
// Taken and given back here and in dj_scratch_poll only, the main loop. True when it started; a refusal says why.
bool dj_scratch_start(ActionContext& ctx, DjScr what) noexcept {
  DjState& dj = ctx.view.dj;
  if (dj.scratch == nullptr || ctx.samples == nullptr || ctx.params == nullptr) {
    return dj_say(ctx, "NO ENGINE FOR THE SCRATCH") && false;
  }
  if (dj.scratch->busy()) {  // (the render has not let go of the last ring: the tail is not free even if the bank forgot it)
    return dj_say(ctx, "NO ROOM: SCRATCH IS ENDING") && false;
  }
  if ((ctx.rec != nullptr && ctx.rec->arena_busy()) || ctx.samples->take_held() || ctx.samples->take().frames != 0u) {
    return dj_say(ctx, "NO ROOM: LOOPER IS RECORDING") && false;
  }
  std::uint32_t room = 0;
  std::int16_t* pcm = ctx.samples->take_begin(room);
  if (pcm == nullptr) {
    return dj_say(ctx, "NO ROOM FOR THE SCRATCH") && false;
  }
  if (!dj.scratch->stage(pcm, room / 2u)) {  // (the ring is stereo: two samples a frame; too small is refused)
    ctx.samples->take_end(0);
    return dj_say(ctx, "NO ROOM FOR THE SCRATCH") && false;
  }
  const unsigned tempo = ctx.view.play.tempo_x100 != 0u ? ctx.view.play.tempo_x100 : 12000u;
  const std::uint32_t beat = static_cast<std::uint32_t>(6000ull * core::kCpuHz * core::kRefCyclesPerSample.den / (std::uint64_t{core::kRefCyclesPerSample.num} * tempo));  // one beat in samples (6000 x the engine's rate / (BPM x 100))
  if (!ctx.params->push(core::ParamChange{audio::master_param(audio::graph_param::master::ScratchMode), what == DjScr::On ? 1.0f : 2.0f, beat})) {
    dj.scratch->unstage();
    ctx.samples->take_end(0);
    return dj_say(ctx, "SCRATCH BUSY: TRY AGAIN") && false;
  }
  dj.scr = what;
  dj.scr_beat = beat;
  if (what == DjScr::Tape && dj.stop_len != kDjStopLenDefault &&  // OS-DJSTOPLEN: the length of the last hold, told right behind the start (the engine rebases at the first block; a full ring leaves the beat and the default)
      !ctx.params->push(core::ParamChange{audio::master_param(audio::graph_param::master::ScratchMode), 3.0f, dj_stop_len_samples(beat, dj.stop_len)})) {
    dj.stop_len = kDjStopLenDefault;
  }
  return true;
}

// A TRANS pad (B2): while one runs any TRANS pad cancels it (the filter springs back, the queue stays). Playing only. Row 1's pick is the target; with none the Song order's next slot is
// queued (the live jump). HP / LP hold the switch back for whole loops until the sweep lasts a bar (dj_trans_loops): the pick is taken off the sequencer and given back at the last loop. The timer is dj_trans_publish.
bool dj_trans_tap(ActionContext& ctx, DjTrans kind) noexcept {
  DjState& dj = ctx.view.dj;
  const ViewState& v = ctx.view;
  if (dj.trans.kind != DjTrans::None) {  // any TRANS pad while one runs cancels it (a queue held back is given back by the next pass)
    dj.trans_end(false);
    (void)ctx.view.notify.post(core::Severity::Info, "TRANS CANCELLED", "dj", ViewState::kMessageMs);
    return true;
  }
  if (ctx.transport == nullptr) {
    return false;
  }
  if (!v.play.playing) {
    (void)ctx.view.notify.post(core::Severity::Info, "TRANS NEEDS PLAY", "dj", ViewState::kMessageMs);
    return true;
  }
  if (v.play.count_in != 0u) {  // the pre-roll's position is not the loop's (each count-in step would look like a wrap)
    // The count-in strip ("cntin", rewritten every beat) is the head, so a post waited behind it and never showed. ask() puts the refusal ahead of that Info head for kMessageMs; the next beat's upsert
    // (App::poll_count_in) posts the strip again behind it, and it returns when the refusal has run out.
    (void)ctx.view.notify.ask(core::Severity::Info, "TRANS NEEDS THE COUNT-IN OVER", "dj", ViewState::kMessageMs);
    return true;
  }
  const bool picked = v.edit.queued_pattern != 0u;
  unsigned row = audio::PlayPosition::kNoOrder;
  if (!picked && v.play.queued == audio::PlayPosition::kNoOrder) {
    if (const char* why = next_song_row(ctx, row)) {
      (void)ctx.view.notify.post(core::Severity::Info, why, "dj", ViewState::kMessageMs);
      return true;
    }
  }
  if (!dj_trans_start(dj, kind, v.play, dj_loop_steps(ctx.model.project, v.play.pattern, ctx.perform != nullptr ? &ctx.perform->bank : nullptr), v.edit.queued_pattern, row, *ctx.transport,
                     ctx.model.project != nullptr ? ctx.model.project->order_ops() : 0u)) {
    return false;
  }
  if (kind == DjTrans::Echo && ctx.model.project != nullptr && ctx.model.project->master().delay_volume == 0u) {
    (void)ctx.view.notify.post(core::Severity::Info, "DELAY LEVEL IS 0", "dj", ViewState::kMessageMs);
  }
  return true;
}

}  // namespace

unsigned dj_publish(DjState& dj, bool active, core::ParamRing* ring) noexcept {
  unsigned sent = 0;
  for (unsigned t = 0; t < kDjTracks && ring != nullptr; ++t) {
    const std::uint8_t want = dj.want(t, active);
    if (want != dj.sent[t] && ring->push(core::ParamChange{audio::strip_param(t, audio::graph_param::strip::XfGain), static_cast<float>(want), kDjGlideSamples})) {
      dj.sent[t] = want;
      ++sent;
    }
  }
  const int sweep = dj.trans_filter();  // B2: a TRANS sweep owns the filter while it is not 0; its steps glide 20 ms, the switch snaps (one block), the rest springs back
  const int want_filt = active ? dj.filt_now() : 0;
  const std::uint32_t ramp = want_filt == 0 ? (dj.trans.snap ? 1u : kDjSpringSamples) : (sweep != 0 ? kDjGlideSamples : kDjFiltGlideSamples);
  if (ring != nullptr && want_filt != dj.filt_sent && ring->push(core::ParamChange{audio::master_param(audio::graph_param::master::DjFilter), static_cast<float>(want_filt), ramp})) {
    dj.filt_sent = static_cast<std::int8_t>(want_filt);
    ++sent;
  }
  if (dj.filt_sent == want_filt) {  // the snap was told (or there was nothing to snap, a hand-set filter that stays): a later RESET springs back as usual
    dj.trans.snap = false;
  }
  const std::uint8_t want_kill = active ? dj.kill() : std::uint8_t{0};
  if (ring != nullptr && want_kill != dj.kill_sent && ring->push(core::ParamChange{audio::master_param(audio::graph_param::master::KillEq), static_cast<float>(want_kill), 1u})) {
    dj.kill_sent = want_kill;
    ++sent;
  }
  return sent;
}

void dj_scratch_poll(DjState& dj, bool active, SampleBank* bank, core::ParamRing* ring) noexcept {
  if (!active) {
    (void)dj.scr_end();
  }
  if (dj.scr == DjScr::Off) {
    return;
  }
  if (dj.scratch == nullptr) {
    dj.scr = DjScr::Off;
    return;
  }
  if (dj.scr != DjScr::Ending) {
    if (!dj.scratch->busy()) {  // the stage let go by itself (the export's abort): the ring is free
      if (bank != nullptr) {
        bank->take_end(0);
      }
      dj.scr = DjScr::Off;
    }
    return;
  }
  if (!dj.scr_stop_sent && ring != nullptr && ring->push(core::ParamChange{audio::master_param(audio::graph_param::master::ScratchMode), 0.0f, 1u})) {
    dj.scr_stop_sent = true;
  }
  if (dj.scr_stop_sent && !dj.scratch->busy()) {  // the release (or the backspin) has ended: the render has made its last access, the arena's tail goes back
    if (bank != nullptr) {
      bank->take_end(0);
    }
    dj.scr = DjScr::Off;
  }
}

bool dj_fx_publish(DjState& dj, bool active, std::uint32_t now_ms, unsigned tempo_x100, seq::PerformBank& bank) noexcept {
  dj.fx_sync(now_ms, dj_beat_ms(tempo_x100));
  bool changed = false;
  for (unsigned t = 0; t < kDjTracks; ++t) {
    const DjState::FxValues v = dj.fx_values(t, active);
    changed = bank.set_dj_layer(t, v.stutter, v.delay, v.gain) || changed;
  }
  return changed;
}

bool dj_trans_start(DjState& dj, DjTrans kind, const audio::PlayPosition::Value& play, unsigned loop_steps, std::uint8_t queued_pattern, unsigned next_row, TransportSink& transport, std::uint32_t list_ops) noexcept {
  const bool picked = queued_pattern != 0u;
  const bool jumped = play.queued != audio::PlayPosition::kNoOrder;
  const std::uint32_t len = dj_loop_len(loop_steps);
  const std::uint32_t pos = dj_loop_pos(play.step, play.ustep);
  DjTransition n;
  n.kind = kind;
  n.pattern0 = play.pattern;
  n.loops = static_cast<std::uint8_t>(kind == DjTrans::Echo ? 0u : dj_trans_loops(len > pos ? len - pos : 0u, len));
  n.from = pos;
  n.last = pos;
  n.list_ops = static_cast<std::uint16_t>(list_ops);
  n.pat = queued_pattern;
  n.jump = static_cast<std::uint16_t>(jumped ? play.queued : (picked ? audio::PlayPosition::kNoOrder : next_row));
  if (n.loops == 0u) {
    if (!picked && !jumped && !transport.queue_jump(next_row)) {
      return false;
    }
  } else {
    // The queue is taken off the sequencer (the pick, the live jump), and in Song mode the slot is held: a live jump to it makes it play once more. A refusal half-way gives back what was taken:
    // it stays owed (a transition of kind None), dj_trans_publish tells it again, and the pad did nothing.
    bool ok = true;
    bool took = false;
    if (picked) {
      ok = transport.queue_pattern(0u);
      took = ok;
    }
    if (ok && jumped) {
      ok = transport.queue_jump(0xFFFFu);
      took = took || ok;
    }
    if (ok && play.order != audio::PlayPosition::kNoOrder) {
      ok = transport.queue_jump(play.order);
      if (ok) {
        n.hold = play.order;
      }
    }
    if (!ok) {
      if (took) {
        n.kind = DjTrans::None;
        n.owes = true;
        dj.trans = n;
      }
      return false;
    }
    n.owes = true;
  }
  dj.trans = n;
  return true;
}

unsigned dj_loop_steps(const model::Project* project, unsigned pattern, const seq::PerformBank* bank) noexcept {
  const unsigned perf = bank != nullptr ? bank->told_length() : 0u;
  if (perf != 0u) {
    return perf;
  }
  if (project == nullptr) {
    return kDjStepsPerBar;
  }
  const unsigned n = project->pattern_length(pattern);
  return n != 0u ? n : project->default_pattern_length();
}

namespace {

// Tells the sequencer the queue a transition held back: the live jump (or the Song's next slot) and the row 1 pick, both, as they were (at the pattern's end the jump wins over the pick, as before the tap). In Song mode the
// hold jump may still wait: with no jump of its own it is cancelled here, or it would win and the pick would be lost. False: the sink refused; the next pass tries again.
bool trans_give(DjTransition& t, std::uint8_t& queued_pattern, TransportSink& sink) noexcept {
  const bool jump = t.jump != audio::PlayPosition::kNoOrder;
  if ((jump || t.hold != audio::PlayPosition::kNoOrder) && !sink.queue_jump(jump ? t.jump : 0xFFFFu)) {
    return false;
  }
  if (t.pat != 0u) {
    if (!sink.queue_pattern(t.pat)) {
      return false;
    }
    queued_pattern = t.pat;
  }
  t.hold = audio::PlayPosition::kNoOrder;
  t.owes = false;
  return true;
}

}  // namespace

void dj_trans_publish(DjState& dj, const audio::PlayPosition::Value& play, unsigned loop_steps, std::uint8_t& queued_pattern, TransportSink* transport, std::uint32_t list_ops) noexcept {
  DjTransition& t = dj.trans;
  // E-SONG-07: an edit of the song's order list since the tap (an insert, delete or move) leaves the order indices the transition holds (t.jump, t.hold) pointing at another slot, and the sequencer remaps only the
  // jump it holds itself. The transition ends: the stale index is dropped, the hold jump the sequencer may still wait with is cancelled, and the pick (a pattern id, still true) is given back. Not covered: a pad pressed
  // less than one block after an edit reads a published play.order from before it (the Value is the render's, one block old); that tap holds a slot one off until the next edit or pattern end ends it.
  if ((t.kind != DjTrans::None || t.owes) && t.list_ops != static_cast<std::uint16_t>(list_ops)) {
    t.list_ops = static_cast<std::uint16_t>(list_ops);
    t.jump = audio::PlayPosition::kNoOrder;
    t.owes = t.owes || t.hold != audio::PlayPosition::kNoOrder;
    dj.trans_end(false);
  }
  if (t.owes && (t.kind == DjTrans::None || t.wraps >= t.loops)) {  // the transition is over, or the last loop has begun: the queue goes now
    if (transport == nullptr) {
      t.owes = false;
    } else {
      (void)trans_give(t, queued_pattern, *transport);
    }
  }
  if (t.kind == DjTrans::None) {
    return;
  }
  if (!play.playing || play.count_in != 0u) {  // the sequencer drops its queue with the transport (a count-in is a new start, its position is not the loop's); nothing is owed
    dj.trans_end(false);
    t.owes = false;
    return;
  }
  const std::uint32_t len = dj_loop_len(loop_steps);
  const std::uint32_t pos = dj_loop_pos(play.step, play.ustep);
  const bool wrapped = pos < t.last;
  t.last = pos;
  if (play.pattern != t.pattern0) {  // another pattern plays: the switch (or somebody else's)
    if (t.hold != audio::PlayPosition::kNoOrder) {  // Song mode, the hold jump may still wait (the tap raced the pattern's end): trans_give puts it away, or it would bring the song back to the slot it has left
      t.pat = 0;
      t.jump = audio::PlayPosition::kNoOrder;
      t.owes = true;
      dj.trans_end(false);
      return;
    }
    dj.trans_end(!t.owes);
    t.owes = false;
    return;
  }
  if (wrapped) {
    if (t.wraps >= t.loops) {  // the loop that ended had the queue in force: this is the switch, the drop
      dj.trans_end(!t.owes);
      return;
    }
    ++t.wraps;
    // (E-SONG-07: the same test ends a transition when an edit of the song list moved the held slot, but only when the edit shows in play.order; an edit as such ends it at the top of this function: the order
    // indices held here are app-side and are not remapped across an edit)
    if (t.hold != audio::PlayPosition::kNoOrder && play.order != t.hold) {  // the tap raced the pattern's end: the song went on before the hold jump was taken, and the jump would bring it back
      t.jump = audio::PlayPosition::kNoOrder;  // (the Song position queued with it is stale; the pick stays owed)
      t.owes = true;
      dj.trans_end(false);
      return;
    }
    if (t.wraps < t.loops && t.hold != audio::PlayPosition::kNoOrder && transport != nullptr) {
      (void)transport->queue_jump(t.hold);  // Song mode: the slot plays once more
    }
    if (t.wraps >= t.loops && t.owes && transport != nullptr) {
      (void)trans_give(t, queued_pattern, *transport);
    }
  } else if (t.pat != 0u && queued_pattern != t.pat) {
    if (queued_pattern == 0u) {  // row 1's second tap withdrew the pick: the transition has nothing to end in
      dj.trans_end(false);
      t.owes = false;
      return;
    }
    t.pat = queued_pattern;  // another pick: it takes the place of the first; while it is held back the sequencer's own queue is taken off again
    if (t.owes && transport != nullptr) {
      (void)transport->queue_pattern(0);
    }
  }
  const std::uint32_t loops_left = t.wraps < t.loops ? t.loops - t.wraps : 0u;
  const std::uint32_t to_switch = loops_left * len + (len > pos ? len - pos : 0u);
  if (t.kind == DjTrans::Echo) {
    t.echo = to_switch <= kDjBeatMicrosteps;
    return;
  }
  const std::uint32_t total = t.loops * len + (len > t.from ? len - t.from : 1u);
  const std::uint32_t done = t.wraps * len + pos;
  const std::uint32_t elapsed = done > t.from ? done - t.from : 0u;
  const std::uint32_t pct = elapsed >= total ? 100u : elapsed * 100u / total;
  t.pct = static_cast<std::uint8_t>(pct);
}

bool dj_tempo_publish(DjState& dj, bool active, bool playing, bool slave, unsigned published, unsigned rate, seq::CommandSink& out) noexcept {
  const int dir = active && playing && !slave ? dj.nudge_dir() : 0;
  if (dir != 0 && dj.nudge_base == 0 && published != 0u) {
    dj.nudge_base = dj_nominal(published, rate);  // the tempo before the nudge, without the step rate (the override is the sequencer's nominal one)
  }
  const unsigned want = dir != 0 && dj.nudge_base != 0u ? dj_nudged(dj.nudge_base, dir) : 0u;
  bool told = false;
  if (want != dj.nudge_sent && out.push(seq::Command{seq::Command::Kind::PerfTempo, 0, 0, want})) {
    dj.nudge_sent = want;
    told = true;
  }
  if (dir == 0 && dj.nudge_sent == 0u && dj.nudge_base != 0u &&
      (!playing || (published != dj_published(dj_nudged(dj.nudge_base, -1), rate) && published != dj_published(dj_nudged(dj.nudge_base, 1), rate)))) {
    dj.nudge_base = 0;  // the override is gone and the tempo the sequencer says is not the nudged one any more: the next nudge starts from what it is then
  }
  return told;
}

// The 48 pads of the page. Row 2 (pads 13..24) is REP / ECHO / CUT of each group (B1); row 3's KILL pads and FILT pad (pad 28) give the master EQ and the encoder to the filter while they are down; row 4 (pads 37..48)
// is the fader in 12 steps, a tap jumps there with the glide; TRANS HP / LP / ECHO (pads 29-31, B2) start and cancel a transition; the rest of row 3 belongs to C1 and does nothing yet.
bool dj_pad(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (!on_dj(ctx)) {
    return false;
  }
  if (arg == static_cast<core::ActionArg>(kDjFiltPad)) {
    ctx.view.dj.filt_pad = true;
    return true;
  }
  if (arg >= static_cast<core::ActionArg>(kDjKillPad) && arg < static_cast<core::ActionArg>(kDjKillPad + kDjKills)) {
    return ctx.view.dj.kill_down(static_cast<unsigned>(arg) - kDjKillPad);  // KILL L / M / H (A3)
  }
  if (arg == static_cast<core::ActionArg>(kDjTapePad)) {  // row 3, pad 8: TAPE STOP (C1), held
    if (ctx.view.dj.scr == DjScr::On) {
      return dj_say(ctx, "SCRATCH IS ON");
    }
    return ctx.view.dj.scr == DjScr::Off && dj_scratch_start(ctx, DjScr::Tape);
  }
  if (arg >= static_cast<core::ActionArg>(kDjTransPad) && arg < static_cast<core::ActionArg>(kDjTransPad + 3u)) {  // row 3, pads 5-7: TRANS HP, LP and ECHO (B2)
    return dj_trans_tap(ctx, static_cast<DjTrans>(1u + static_cast<unsigned>(arg) - kDjTransPad));
  }
  if (arg >= static_cast<core::ActionArg>(kDjFxPad) && arg < static_cast<core::ActionArg>(kDjFxPad + kDjFxPads)) {  // row 2: REP, ECHO and CUT of the group (B1)
    const unsigned col = static_cast<unsigned>(arg) - kDjFxPad;
    if (dj_fx_kind(col) == DjFx::Echo && ctx.model.project != nullptr && ctx.model.project->master().delay_volume == 0u) {
      (void)ctx.view.notify.post(core::Severity::Info, "DELAY LEVEL IS 0", "dj", ViewState::kMessageMs);  // the send opens but the master DELAY level is 0: nothing to hear
    }
    return ctx.view.dj.fx_down(col);
  }
  if (arg >= 0 && arg < static_cast<core::ActionArg>(kDjBankSize)) {  // row 1: the pattern of the bank (A4)
    return pattern_queue_tap(ctx, ctx.view.dj_bank() * kDjBankSize + static_cast<unsigned>(arg) + 1u);
  }
  if (arg < 36 || arg > 47) {
    return false;
  }
  return ctx.view.dj.set_position(dj_pad_position(static_cast<unsigned>(arg) - 36u));
}

// The release of any pad (the App sends it as a raw event, as perform.pad_up: a pad let go after the page changed still ends its job): the FILT pad up lets the filter spring back, a KILL pad up ends its kill
// unless it is latched, a row 2 pad up ends its REP / ECHO / CUT the same way.
bool dj_pad_up(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (arg >= static_cast<core::ActionArg>(kDjKillPad) && arg < static_cast<core::ActionArg>(kDjKillPad + kDjKills)) {
    return ctx.view.dj.kill_up(static_cast<unsigned>(arg) - kDjKillPad);  // a KILL pad let go (A3): the held kills end, a latched one stays
  }
  if (arg >= static_cast<core::ActionArg>(kDjFxPad) && arg < static_cast<core::ActionArg>(kDjFxPad + kDjFxPads)) {
    return ctx.view.dj.fx_up(static_cast<unsigned>(arg) - kDjFxPad);  // a row 2 pad let go (B1): the held REP / ECHO / CUT ends, a latched one stays
  }
  if (arg == static_cast<core::ActionArg>(kDjTapePad)) {
    return ctx.view.dj.scr == DjScr::Tape && ctx.view.dj.scr_end();  // TAPE STOP let go: live again (a pad let go after the page changed is the same)
  }
  if (arg != static_cast<core::ActionArg>(kDjFiltPad) || !ctx.view.dj.filt_pad) {
    return false;
  }
  ctx.view.dj.filt_pad = false;
  (void)ctx.view.dj.release_filter();
  return true;
}

// F4 held + the encoder, and the encoder while the FILT pad is down (the App takes that ahead of the bindings: a pad has no HoldEncoder): 10 a detent.
// OS-DJSTOPLEN: the encoder while the TAPE STOP pad is held (taken by the App ahead of the bindings, like the FILT pad's): the stop's length, told to the engine as ScratchMode 3 (a refused push leaves the old length).
bool dj_stop_len(ActionContext& ctx, core::ActionArg arg) noexcept {
  DjState& dj = ctx.view.dj;
  if (!on_dj(ctx) || dj.scr != DjScr::Tape || dj.scr_beat == 0u || ctx.params == nullptr) {
    return false;
  }
  const std::uint8_t before = dj.stop_len;
  if (!dj.turn_stop_len(static_cast<int>(arg))) {
    return false;
  }
  if (!ctx.params->push(core::ParamChange{audio::master_param(audio::graph_param::master::ScratchMode), 3.0f, dj_stop_len_samples(dj.scr_beat, dj.stop_len)})) {
    dj.stop_len = before;
    return dj_say(ctx, "SCRATCH BUSY: TRY AGAIN") && false;
  }
  return true;
}
bool dj_filter(ActionContext& ctx, core::ActionArg arg) noexcept { return on_dj(ctx) && ctx.view.dj.turn_filter(static_cast<int>(arg)); }

// The release of F4 (the App sends every function key's release as a raw event: the gesture layer keeps a release silent after a turn): the filter springs back, unless LATCH is on or the FILT pad
// still holds it.
bool dj_key_up(ActionContext& ctx, core::ActionArg arg) noexcept {
  return arg == static_cast<core::ActionArg>(kKeyFilter) && !ctx.view.dj.filt_pad && ctx.view.dj.release_filter();
}

// The encoder: one turn is the whole travel; Shift + encoder is the fine one (the app sees detents, not the 96 counts: one position a detent).
bool dj_turn(ActionContext& ctx, core::ActionArg arg) noexcept { return on_dj(ctx) && ctx.view.dj.turn(static_cast<int>(arg), false); }
bool dj_fine(ActionContext& ctx, core::ActionArg arg) noexcept { return on_dj(ctx) && ctx.view.dj.turn(static_cast<int>(arg), true); }

// Row 1 of the TRACKS layer (Shift held) on the DJ page, arg = the pad's column = track 0..11: THRU -> A -> B (pads_track sends it here).
bool dj_group(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (!on_dj(ctx) || arg < 0 || !ctx.view.dj.cycle_group(static_cast<unsigned>(arg))) {
    return false;
  }
  static constexpr const char* kName[3] = {"THRU", "GROUP A", "GROUP B"};
  char text[24];
  unsigned n = 0;
  for (const char* p = "TRACK "; *p != '\0'; ++p) {
    text[n++] = *p;
  }
  const unsigned track = static_cast<unsigned>(arg) + 1u;
  if (track >= 10u) {
    text[n++] = static_cast<char>('0' + track / 10u);
  }
  text[n++] = static_cast<char>('0' + track % 10u);
  text[n++] = ':';
  text[n++] = ' ';
  for (const char* p = kName[ctx.view.dj.group[arg]]; *p != '\0'; ++p) {
    text[n++] = *p;
  }
  text[n] = '\0';
  (void)ctx.view.notify.post(core::Severity::Info, text, "dj", ViewState::kMessageMs);
  return true;
}

// The left / right arrow down (arg -1 / +1): the tempo nudge, -4 % / +4 % while held (the App tells the sequencer once a pass, dj_tempo_publish). Not in MIDI Slave, and only while the transport plays.
bool dj_nudge(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (!on_dj(ctx) || (arg != -1 && arg != 1)) {
    return false;
  }
  if (clock_slave(ctx)) {
    (void)ctx.view.notify.post(core::Severity::Info, "MIDI SLAVE: THE CLOCK SETS THE TEMPO", "dj", ViewState::kMessageMs);
    return true;
  }
  if (!ctx.view.play.playing) {
    (void)ctx.view.notify.post(core::Severity::Info, "NUDGE NEEDS PLAY", "dj", ViewState::kMessageMs);
    return true;
  }
  if (ctx.perform == nullptr) {
    return false;  // no engine behind the page (a bare test): nothing to nudge
  }
  ctx.view.dj.nudge_down(arg > 0 ? 1u : 0u);
  return true;
}

// The release of an arrow (the App sends every release as a raw event: an arrow let go after the page changed still ends its nudge).
bool dj_nudge_up(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (arg == static_cast<core::ActionArg>(Button::Left)) {
    return ctx.view.dj.nudge_up(0);
  }
  return arg == static_cast<core::ActionArg>(Button::Right) && ctx.view.dj.nudge_up(1);
}

// Shift + the arrow (arg -1 / +1): the project tempo by 0.1 BPM, kept (the Song screen's tempo step: the editor, one undo unit, and the sequencer's tempo param). Not in MIDI Slave.
bool dj_tempo(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (!on_dj(ctx) || (arg != -1 && arg != 1)) {
    return false;
  }
  if (clock_slave(ctx)) {
    (void)ctx.view.notify.post(core::Severity::Info, "MIDI SLAVE: THE CLOCK SETS THE TEMPO", "dj", ViewState::kMessageMs);
    return true;
  }
  return tempo_fine_step(ctx, static_cast<int>(arg));
}

// F1..F8 on the DJ page (arg 0..7). F1 / F2 the bank, F3 CURVE, F6 LATCH and F8 RESET; F4 is held (dj_filter, dj_key_up; a tap only says so); the rest is the later slices' (the other labels are blank).
bool dj_key(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (!on_dj(ctx)) {
    return false;
  }
  if (arg == static_cast<core::ActionArg>(kKeyBankDown) || arg == static_cast<core::ActionArg>(kKeyBankUp)) {
    return ctx.view.dj.move_bank(arg == static_cast<core::ActionArg>(kKeyBankUp) ? 1 : -1, ctx.view.dj_now());
  }
  if (arg == static_cast<core::ActionArg>(kKeyCurve)) {
    ctx.view.dj.toggle_curve();
    (void)ctx.view.notify.post(core::Severity::Info, ctx.view.dj.curve == DjCurve::Cut ? "CURVE: CUT" : "CURVE: SMOOTH", "dj", ViewState::kMessageMs);
    return true;
  }
  if (arg == static_cast<core::ActionArg>(kKeyFilter)) {  // OS-QAFIX-14: F4 is the filter's hold (dj_filter); a tap says how
    return dj_say(ctx, "FILTER: HOLD, TURN THE ENCODER");
  }
  if (arg == static_cast<core::ActionArg>(kKeyScr)) {  // C1: SCR, a toggle: the hand on the record
    if (ctx.view.dj.scr == DjScr::On) {
      (void)ctx.view.dj.scr_end();
      return dj_say(ctx, "SCR OFF");
    }
    return ctx.view.dj.scr == DjScr::Off && dj_scratch_start(ctx, DjScr::On) && dj_say(ctx, "SCR ON: THE ENCODER IS THE PLATTER");
  }
  if (arg == static_cast<core::ActionArg>(kKeyLatch)) {
    ctx.view.dj.toggle_latch();
    (void)ctx.view.notify.post(core::Severity::Info, ctx.view.dj.latch ? "LATCH ON: FILTER AND PAD EFFECTS STAY" : "LATCH OFF", "dj", ViewState::kMessageMs);
    return true;
  }
  if (arg == static_cast<core::ActionArg>(kKeyReset)) {
    (void)ctx.view.dj.reset();
    (void)ctx.view.notify.post(core::Severity::Info, "DJ RESET", "dj", ViewState::kMessageMs);
    return true;
  }
  return false;
}

}  // namespace o3os::app
