// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The DJ page of Perform (OS-DJ, docs/specs/OS-DJ.md; new Community OS behaviour). A1: the groups, the crossfader and its two curves, and the end of every effect on leaving the page.
// A2: the master DJ filter (F4 held + the encoder, or the FILT pad held), LATCH. A3: the master kill EQ (the KILL L / M / H pads). A4: the pattern row, the tempo nudge and the fine tempo.
//
// The crossfader is a CUT of the tracks of two groups: A (the left of the fader) and B (the right). A track that is THRU is never touched. The position is 0 (all A) .. 96 (all B), one turn of the
// encoder (24 detents of 4; Shift + encoder is one position a detent). SMOOTH is equal power (the gain of A is cos, of B sin, of a quarter turn: 255 * cos at 97 positions, built at compile time, 97 bytes of
// flash); CUT is unity until the last position of each end (a scratch fader). Nothing boosts: the gains are 0..255 and 255 is the strip's own level (Graph strip field XfGain, a cut only, like the
// Perform layer's VOLUME). The DJ state is runtime only and is never stored: at power on every track is THRU and the fader is centred.
//
// DJ only on the DJ page (spec section 1): while the page is not in view the levels told to the engine are unity for every track, whatever the position was, and the position, the curve and
// whatever later slices add go back to rest (leave()). Only the groups stay until power off. Main loop only; the levels go out through the ParamChange ring, one pass at a time (dj_publish).
//
// The filter (A2) is one bipolar knob over the whole master, -100 (low-pass, closed) .. +100 (high-pass, closed), 0 bypassed; 10 per detent. It is held by the hand: F4 held + the encoder, or the FILT pad
// (row 3, pad 4) held with the encoder, and at the release it springs back to 0 (a 120 ms glide) unless LATCH is on. Unlatching springs back. The engine side is master_dj_filter.h.
//
// The kills (A3) are the first three pads of row 3, LOW, MID and HI: a band is killed while its pad is down; with LATCH on a tap toggles it and it stays (unlatching ends the latched ones). The
// spec's "Shift + pad latches" is LATCH here, because a held Shift turns the pads into the TRACKS layer. The mask goes to the engine as one master KillEq value (1 low, 2 mid, 4 high). The engine
// side is master_kill_eq.h (it ramps each band by itself, 5 ms).

//
// The pattern row (A4) is pad row 1: twelve patterns of a bank (F1 BANK -, F2 BANK +; bank n holds patterns 12 n + 1 .. 12 n + 12, 22 banks). A tap is the PATS pads' tap (pattern_queue_tap: queued at the end of the
// pattern that plays, a second tap on the queued one cancels, stopped it selects the pattern for editing): the sequencer's own queue, no second mechanism. A queued pattern stays queued when the page is left, as
// with NEXT PATTERN. The bank is the one of the pattern that plays until F1 / F2 move it, and goes back to that on leaving.
//
// The nudge (A4) is the left / right arrow held: the tempo -4 % / +4 % while held, through the sequencer's performance tempo override (seq::Command PerfTempo), which wins over the Tempo FX and the project tempo and
// is cleared at the release: the tempo after a nudge is exactly the one before (the override goes, nothing is added to the song's). Refused in MIDI Slave (the clock is the tempo) and while stopped. Leaving the page
// ends it. Shift + the arrow is the fine tempo, 0.1 BPM a press, kept in the project's tempo field (one undo unit), through the Song screen's tempo step.
//
// Row 2 (B1) is the held FX of each group, the left half group A and the right half group B (mirrored: REP 1/4, REP 1/8, REP 1/16, REP 1/32, ECHO, CUT | CUT, ECHO, REP 1/32 .. REP 1/4). A pad is momentary; with LATCH on a tap
// toggles it (one REP rate a group; the others stay as they are). REP is the performance stutter (the last note of each track of the group retriggered every note value), ECHO opens the group's DELAY send whole and
// cuts the group's dry level one beat later (the tail rings out), CUT silences the group. They are the Perform layer's own FX (PerformBank::set_dj_layer: STEP REPEATER, DELAY and VOLUME) put above the slots: no new
// engine feature. THRU tracks are never touched. Everything ends with RESET, with unlatching and with leaving the page.
//
// The transitions (B2) are pads 5-7 of row 3: TRANS HP and TRANS LP sweep the DJ filter from 0 to closed over the time left until the queued pattern starts (at least one bar; if less is left the switch
// waits for one more loop), TRANS ECHO opens the DELAY send of the whole mix in the last beat before it. An app-side timer against the published play position (DjTransition, dj_trans_publish): the queue
// is the sequencer's own (row 1's pick, else the Song order's next slot), no new engine feature. At the switch the filter and the echo snap back; the same pad again, RESET and leaving end it.

#include <o3os/audio/master_scratch.h>
#include <o3os/audio/play_position.h>
#include <o3os/core/param.h>
#include <o3os/model/limits.h>

#include <cstdint>

namespace o3os::model {
class Project;
}

namespace o3os::seq {
class CommandSink;
class PerformBank;
}

namespace o3os::app {

class SampleBank;

class TransportSink;

enum class DjGroup : std::uint8_t { Thru = 0, A = 1, B = 2 };
enum class DjCurve : std::uint8_t { Smooth = 0, Cut = 1 };

inline constexpr unsigned kDjTracks = 12;                 // the pads' columns: tracks 13-16 are not reachable, as on Pattern
inline constexpr unsigned kDjTravel = 96;                 // positions 0..96: the encoder's 96 counts a turn
inline constexpr unsigned kDjCentre = kDjTravel / 2;
inline constexpr std::uint8_t kDjUnity = 255;             // the engine's "no cut"
inline constexpr std::uint32_t kDjGlideSamples = 882;     // 20 ms at 44.1 kHz: ParamChange ramp, 14 blocks of 64
inline constexpr unsigned kDjPageIndex = 1;               // Perform's pages: 0 the slots, 1 DJ
inline constexpr int kDjFiltRange = 100;                  // the filter's travel each side of 0 (the engine's per cent)
inline constexpr int kDjFiltStep = 10;                    // per detent: ten detents from 0 to either end
inline constexpr std::uint32_t kDjFiltGlideSamples = 2646;  // 60 ms: a detent of the filter (the engine glides at least one block of 64)
inline constexpr std::uint32_t kDjSpringSamples = 5292;     // 120 ms: the spring-back to 0
inline constexpr unsigned kDjFiltPad = 27;                // the FILT pad: row 3, pad 4
inline constexpr unsigned kDjKillPad = 24;                // the KILL LOW pad: row 3, pad 1 (MID and HI follow)
inline constexpr unsigned kDjKills = 3;
inline constexpr unsigned kDjBankSize = 12;               // the pattern row: row 1 of the pads
inline constexpr unsigned kDjBanks = 22;                  // patterns 1..255 in banks of 12 (the last holds three)
inline constexpr unsigned kDjFxPad = 12;                  // B1: the first pad of row 2 (12 pads: the left six are group A, the right six group B)
inline constexpr unsigned kDjFxPads = 12;
inline constexpr unsigned kDjTransPad = 28;               // B2: TRANS HP, TRANS LP, TRANS ECHO: row 3, pads 5-7
inline constexpr unsigned kDjTapePad = 31;                // C1: TAPE STOP: row 3, pad 8
inline constexpr unsigned kDjMicrosteps = 48;             // a step (the sequencer's grid; PlayPosition::ustep counts them)
inline constexpr unsigned kDjStepsPerBar = 16;            // four steps a beat, four beats a bar (seq::kStepsPerBar)
inline constexpr std::uint32_t kDjBeatMicrosteps = 4u * kDjMicrosteps;
inline constexpr unsigned kDjNudgePercent = 4;            // the tempo nudge, each way
inline constexpr int kDjMaxDetents = 1 << 16;             // a turn larger than this is clamped before it is multiplied (an encoder fed from data: the sim, the console), as Song's tempo step does

// A signed count of detents held to +-kDjMaxDetents, so that the multiplication by a step (4, 10) cannot overflow an int.
constexpr int dj_detents(int detents) noexcept { return detents > kDjMaxDetents ? kDjMaxDetents : (detents < -kDjMaxDetents ? -kDjMaxDetents : detents); }

// The nudged tempo (BPM x 100) for a base tempo and a direction (-1, +1): base * (1 +- 4 %), rounded, held to the model's range.
constexpr unsigned dj_nudged(unsigned base, int dir) noexcept {
  const unsigned pct = dir < 0 ? 100u - kDjNudgePercent : (dir > 0 ? 100u + kDjNudgePercent : 100u);
  const unsigned v = (base * pct + 50u) / 100u;
  return v < model::kMinTempoX100 ? model::kMinTempoX100 : (v > model::kMaxTempoX100 ? model::kMaxTempoX100 : v);
}
// The sequencer publishes the tempo with the step rate in it (HALF TIME halves, DOUBLE doubles); the override it is told is the one without: these two convert (rate 0 normal, 1 half, 2 double).
constexpr unsigned dj_nominal(unsigned published, unsigned rate) noexcept { return rate == 1u ? published * 2u : (rate == 2u ? published / 2u : published); }
constexpr unsigned dj_published(unsigned nominal, unsigned rate) noexcept { return rate == 1u ? nominal / 2u : (rate == 2u ? nominal * 2u : nominal); }

// B1: the pads of row 2. A column 0..11 is a group and a kind; the right half mirrors the left, so each hand has the same order from the outside in.
enum class DjFx : std::uint8_t { Rep4 = 0, Rep8, Rep16, Rep32, Echo, Cut };
inline constexpr unsigned kDjFxKinds = 6;
constexpr DjGroup dj_fx_group(unsigned col) noexcept { return col < kDjFxKinds ? DjGroup::A : DjGroup::B; }
constexpr DjFx dj_fx_kind(unsigned col) noexcept { return static_cast<DjFx>(col < kDjFxKinds ? col : kDjFxPads - 1u - col); }
constexpr unsigned dj_fx_col(DjGroup group, DjFx kind) noexcept { return group == DjGroup::B ? kDjFxPads - 1u - static_cast<unsigned>(kind) : static_cast<unsigned>(kind); }
// The performance stutter value of a REP kind (seq::stutter_microsteps: a step is a 16th note, 48 microsteps): 1/4 note is 4 steps (5), 1/8 is 2 steps (7), 1/16 is one step (8), 1/32 is half a step (9).
constexpr std::uint8_t dj_rep_value(DjFx kind) noexcept { return kind == DjFx::Rep4 ? std::uint8_t{5} : (kind == DjFx::Rep8 ? std::uint8_t{7} : (kind == DjFx::Rep16 ? std::uint8_t{8} : (kind == DjFx::Rep32 ? std::uint8_t{9} : std::uint8_t{0}))); }
// The length of a beat in milliseconds at a tempo (BPM x 100), held to the model's range: ECHO's wait before the cut.
constexpr std::uint32_t dj_beat_ms(unsigned tempo_x100) noexcept {
  const unsigned t = tempo_x100 < model::kMinTempoX100 ? model::kMinTempoX100 : (tempo_x100 > model::kMaxTempoX100 ? model::kMaxTempoX100 : tempo_x100);
  return 6000000u / t;
}

// B2: the loop of the pattern that plays, in microsteps. `steps` is its length (the sequencer's: the model's, the default for a pattern the project lacks, or the PAT LEN FX's), held to 1..128.
constexpr std::uint32_t dj_loop_len(unsigned steps) noexcept { return (steps < 1u ? 1u : (steps > 128u ? 128u : steps)) * kDjMicrosteps; }
// ... and the playhead in it (the published step and microstep).
constexpr std::uint32_t dj_loop_pos(unsigned step, unsigned ustep) noexcept { return step * kDjMicrosteps + (ustep < kDjMicrosteps ? ustep : kDjMicrosteps - 1u); }
// The loops a TRANS has to hold the switch back so that its sweep lasts at least one bar: 0 when `left` microsteps of this loop are a bar or more, else the whole loops of `loop` microsteps to add.
constexpr unsigned dj_trans_loops(std::uint32_t left, std::uint32_t loop) noexcept {
  const std::uint32_t bar = kDjStepsPerBar * kDjMicrosteps;
  return left >= bar ? 0u : static_cast<unsigned>((bar - left + loop - 1u) / loop);
}

namespace dj_detail {

// cos on 0..pi/2 by its series: evaluated at compile time only, in float (nothing of it reaches the target but the 97 bytes).
constexpr float cos_series(float x) noexcept {
  float term = 1.0f;
  float sum = 1.0f;
  for (int n = 1; n < 10; ++n) {
    term *= -x * x / static_cast<float>((2 * n - 1) * (2 * n));
    sum += term;
  }
  return sum;
}

struct CosTable {
  std::uint8_t v[kDjTravel + 1];
};

constexpr CosTable make_cos() noexcept {
  CosTable t{};
  for (unsigned i = 0; i <= kDjTravel; ++i) {
    t.v[i] = static_cast<std::uint8_t>(cos_series(1.57079632679f * static_cast<float>(i) / static_cast<float>(kDjTravel)) * 255.0f + 0.5f);
  }
  return t;
}

inline constexpr CosTable kCos = make_cos();

}  // namespace dj_detail

// The cut (0..255) of a track of `group` with the fader at `pos` (held to 0..96): THRU is never cut; A fades out towards B, B towards A.
constexpr std::uint8_t dj_level(DjGroup group, unsigned pos, DjCurve curve) noexcept {
  const unsigned p = pos > kDjTravel ? kDjTravel : pos;
  if (group == DjGroup::Thru) {
    return kDjUnity;
  }
  const bool a = group == DjGroup::A;
  if (curve == DjCurve::Cut) {
    return a ? (p < kDjTravel ? kDjUnity : std::uint8_t{0}) : (p > 0u ? kDjUnity : std::uint8_t{0});
  }
  return dj_detail::kCos.v[a ? p : kDjTravel - p];
}

// Row 4 of the pads is the fader in 12 steps (OS-DJCENTRE): pad 1 is all A (0 %), pads 2-5 are 10 20 30 40 %, pads 6 AND 7 are the centre (50 %), pads 8-11 are 60 70 80 90 %, pad 12 is all B (100 %).
// In positions of 0..96, symmetric: 0 10 19 29 38 48 48 58 67 77 86 96.
inline constexpr std::uint8_t kDjPadPercent[12] = {0, 10, 20, 30, 40, 50, 50, 60, 70, 80, 90, 100};
constexpr unsigned dj_pad_position(unsigned column) noexcept { return (kDjPadPercent[column > 11u ? 11u : column] * kDjTravel + 50u) / 100u; }
// The pad of that row nearest to a position (the one lit): the left half for a position up to the centre, the right half above it (the centre is two pads: dj_pad_lit lights both).
constexpr unsigned dj_pad_column(unsigned pos) noexcept {
  const unsigned p = pos > kDjTravel ? kDjTravel : pos;
  const unsigned first = p > kDjCentre ? 7u : 0u;
  const unsigned last = p > kDjCentre ? 11u : 5u;
  unsigned best = first;
  unsigned best_d = kDjTravel + 1u;
  for (unsigned c = first; c <= last; ++c) {
    const unsigned at = dj_pad_position(c);
    const unsigned d = at > p ? at - p : p - at;
    if (d < best_d) {
      best_d = d;
      best = c;
    }
  }
  return best;
}
// Whether the pad of that column is lit at a position: the nearest one, and both centre pads while the fader is at the centre.
constexpr bool dj_pad_lit(unsigned column, unsigned pos) noexcept { return column == dj_pad_column(pos) || (pos == kDjCentre && column == 6u); }

// OS-DJSTOPLEN: the TAPE STOP pad held + the encoder sets how long the stop takes: 1/8, 1/4, 1/2, 1, 2 or 4 beats (default 1, the index 3). Runtime only, never stored.
inline constexpr unsigned kDjStopLens = 6;
inline constexpr unsigned kDjStopLenDefault = 3;
inline constexpr const char* kDjStopLenName[kDjStopLens] = {"1/8", "1/4", "1/2", "1", "2", "4"};
// The length in samples: the beat halved (3 - index) times, or doubled (index - 3) times.
constexpr std::uint32_t dj_stop_len_samples(std::uint32_t beat, unsigned index) noexcept {
  const unsigned i = index < kDjStopLens ? index : kDjStopLens - 1u;
  return i <= kDjStopLenDefault ? beat >> (kDjStopLenDefault - i) : beat << (i - kDjStopLenDefault);
}

// B2: the transition in force. HP / LP sweep the filter, ECHO opens the whole mix's send in the last beat. Runtime only (ViewState), never stored.
enum class DjTrans : std::uint8_t { None = 0, Hp, Lp, Echo };

struct DjTransition {
  DjTrans kind = DjTrans::None;
  bool echo = false;          // ECHO: the last beat has begun, the send is open
  bool snap = false;          // the switch came: the filter goes back to 0 at once, not in the spring-back (cleared by dj_publish)
  bool owes = false;          // a queue held back (pat / jump) is still to be told to the sequencer: at the last loop, or at once when the transition ends
  std::uint8_t pct = 0;       // HP / LP: how far the sweep is, 0..100
  std::uint8_t pattern0 = 0;  // the pattern that played at the tap: another one playing means the switch came
  std::uint8_t pat = 0;       // the pattern the row 1 pick queued (0: none)
  std::uint8_t loops = 0;     // whole loops the queue is held back (dj_trans_loops)
  std::uint8_t wraps = 0;     // loops that have ended since the tap
  std::uint16_t jump = 0xFFFF;  // the Song position the queue is (0xFFFF: none)
  std::uint16_t hold = 0xFFFF;  // Song mode: the order position the held-back loops replay (a live jump to the slot that plays), else 0xFFFF
  std::uint16_t list_ops = 0;   // E-SONG-07: Project::order_ops() (low 16 bits) at the tap: jump and hold are indices of that list (it fills the padding: the struct stays 24 bytes)
  std::uint32_t from = 0;     // the playhead (microsteps in the loop) at the tap
  std::uint32_t last = 0;     // ... and at the last pass: a smaller one is a wrap
};

// C1: what the scratch stage is doing, as the page knows it. On: SCR (F5), the platter is the encoder. Tape: TAPE STOP is held. Ending: the stop was asked, the engine is still running its release or backspin and holds the
// ring (the arena is given back when MasterScratch::busy() is false).
enum class DjScr : std::uint8_t { Off, On, Tape, Ending };

struct DjState {
  DjScr scr = DjScr::Off;
  bool scr_stop_sent = false;               // Ending: the ScratchMode 0 is in the ring (a refused push is retried by the next pass)
  audio::MasterScratch* scratch = nullptr;  // the engine's stage (App::attach_scratch); null: no engine, F5 and the pad say so
  std::uint8_t stop_len = kDjStopLenDefault;  // OS-DJSTOPLEN: the TAPE STOP's length, an index of kDjStopLenName (kept for the session)
  std::uint32_t scr_beat = 0;                 // the beat in samples the running scratch was started with (the stop's length is a multiple of it)
  // The encoder while the TAPE STOP pad is held: `detents` (signed) steps of the length, held to the ends. True when it moved.
  bool turn_stop_len(int detents) noexcept {
    int to = static_cast<int>(stop_len) + dj_detents(detents);
    to = to < 0 ? 0 : (to > static_cast<int>(kDjStopLens) - 1 ? static_cast<int>(kDjStopLens) - 1 : to);
    const bool changed = to != static_cast<int>(stop_len);
    stop_len = static_cast<std::uint8_t>(to);
    return changed;
  }
  // The end of SCR or TAPE STOP (RESET, leaving, F5 again, the pad's release): the engine is told next pass.
  bool scr_end() noexcept {
    if (scr != DjScr::On && scr != DjScr::Tape) {
      return false;
    }
    scr = DjScr::Ending;
    scr_stop_sent = false;
    return true;
  }
  std::uint8_t group[kDjTracks] = {};  // DjGroup of each track; kept until power off
  std::uint8_t pos = kDjCentre;
  DjCurve curve = DjCurve::Smooth;
  std::uint8_t sent[kDjTracks] = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};  // the cut last told to the engine (dj_publish); a mismatch is retried the next pass
  std::int8_t filt = 0;       // the filter, -100..+100 (A2); 0 bypassed
  std::int8_t filt_sent = 0;  // the filter last told to the engine (the engine starts bypassed); a mismatch is retried the next pass
  bool latch = false;         // LATCH (F6): the filter stays where it was left at the release
  bool filt_pad = false;      // the FILT pad is down: the encoder is the filter's (the App takes it ahead of the bindings)
  std::uint8_t kill_held = 0;     // A3: the KILL pads that are down (bit 0 low, 1 mid, 2 high)
  std::uint8_t kill_latched = 0;  // A3: the kills that stay (LATCH on and a tap)
  std::uint8_t kill_sent = 0;     // the mask last told to the engine (it starts with none); a mismatch is retried the next pass
  std::uint8_t bank = 0;          // A4: the pattern bank shown on row 1 once F1 / F2 moved it (bank_set); until then, and after leaving, the bank of the pattern that plays
  bool bank_set = false;
  std::uint8_t nudge_keys = 0;    // A4: the arrows that are down (bit 0 left, bit 1 right)
  std::uint32_t nudge_base = 0;   // the tempo (BPM x 100, without the step rate) the nudge started from; 0 none. Kept until the engine's tempo has left the nudged one, so a quick second press never nudges a nudge
  std::uint32_t nudge_sent = 0;   // the override last told to the sequencer (0: none); a mismatch is retried the next pass

  std::uint16_t fx_held = 0;      // B1: the row 2 pads that are down (bit = column 0..11)
  std::uint16_t fx_latched = 0;   // the row 2 pads LATCH keeps (ECHO, CUT, and at most one REP a group)
  std::uint8_t rep_last[2] = {0xFF, 0xFF};  // the REP column pressed last in group A / B (0xFF none): when two REP pads are down the later one decides
  bool echo_run[2] = {};          // ECHO is in force in the group and its beat has been timed (fx_sync)
  bool echo_cut[2] = {};          // ... and the beat has passed: the group's dry level is cut, the tail rings out
  std::uint32_t echo_t0[2] = {};  // the uptime (ms) at which ECHO was first seen in force

  DjTransition trans;             // B2

  DjGroup group_of(unsigned track) const noexcept { return track < kDjTracks ? static_cast<DjGroup>(group[track]) : DjGroup::Thru; }
  // Shift + row 1: THRU -> A -> B -> THRU.
  bool cycle_group(unsigned track) noexcept {
    if (track >= kDjTracks) {
      return false;
    }
    group[track] = static_cast<std::uint8_t>((group[track] + 1u) % 3u);
    return true;
  }
  bool set_position(unsigned p) noexcept {
    const std::uint8_t to = static_cast<std::uint8_t>(p > kDjTravel ? kDjTravel : p);
    const bool changed = to != pos;
    pos = to;
    return changed;
  }
  // The encoder: `detents` (signed) of 4 positions, or of 1 when `fine`; held to the ends.
  bool turn(int detents, bool fine) noexcept {
    if (scr == DjScr::On) {  // C1: with SCR on the encoder is the platter (the engine reads the counter itself)
      return false;
    }
    const int step = dj_detents(detents) * (fine ? 1 : 4);
    const int to = static_cast<int>(pos) + step;
    return set_position(static_cast<unsigned>(to < 0 ? 0 : to));
  }
  bool toggle_curve() noexcept {
    curve = curve == DjCurve::Smooth ? DjCurve::Cut : DjCurve::Smooth;
    return true;
  }
  // The filter's encoder: `detents` (signed) of 10, held to the ends.
  bool turn_filter(int detents) noexcept {
    int to = static_cast<int>(filt) + dj_detents(detents) * kDjFiltStep;
    to = to < -kDjFiltRange ? -kDjFiltRange : (to > kDjFiltRange ? kDjFiltRange : to);
    const bool changed = to != filt;
    filt = static_cast<std::int8_t>(to);
    return changed;
  }
  // F4 or the FILT pad let go: the filter springs back to 0, unless LATCH is on.
  bool release_filter() noexcept {
    if (latch || filt == 0) {
      return false;
    }
    filt = 0;
    return true;
  }
  // LATCH (F6): on keeps the filter at the release; off springs it back at once.
  void toggle_latch() noexcept {
    latch = !latch;
    if (!latch) {
      filt = 0;
      kill_latched = 0;
      fx_latched = 0;
    }
  }
  // The kills in force: the pads down and the latched ones (the engine's mask).
  std::uint8_t kill() const noexcept { return static_cast<std::uint8_t>(kill_held | kill_latched); }
  // A KILL pad went down (band 0..2): killed while it is down, or with LATCH on a tap toggles it and it stays.
  bool kill_down(unsigned band) noexcept {
    if (band >= kDjKills) {
      return false;
    }
    const auto bit = static_cast<std::uint8_t>(1u << band);
    if (latch) {
      kill_latched ^= bit;
    } else {
      kill_held |= bit;
    }
    return true;
  }
  // A KILL pad let go (band 0..2): its held kill ends; a latched one stays.
  bool kill_up(unsigned band) noexcept {
    const auto bit = static_cast<std::uint8_t>(1u << band);
    const bool had = band < kDjKills && (kill_held & bit) != 0u;
    kill_held = static_cast<std::uint8_t>(kill_held & ~bit);
    return had;
  }
  // RESET (F8): the fader centred, the filter at 0 and no kill. The curve and LATCH are settings and stay; the later slices' cuts end here too.
  bool reset() noexcept {
    const bool moved = set_position(kDjCentre);
    const bool scr_ended = scr_end();
    const bool open = filt != 0 || kill() != 0u || fx_on() != 0u || trans.kind != DjTrans::None || scr_ended;
    trans_end(false);
    filt = 0;
    kill_held = 0;
    kill_latched = 0;
    fx_held = 0;
    fx_latched = 0;
    return moved || open;
  }
  // A4: the bank on row 1 (`playing`: the pattern that plays, or the one in view when stopped), F1 / F2 move it by `delta`, held to 0..21.
  unsigned bank_now(unsigned playing) const noexcept {
    const unsigned b = bank_set ? bank : (playing > 0u ? (playing - 1u) / kDjBankSize : 0u);
    return b < kDjBanks ? b : kDjBanks - 1u;
  }
  bool move_bank(int delta, unsigned playing) noexcept {
    const int now = static_cast<int>(bank_now(playing));
    int to = now + delta;
    to = to < 0 ? 0 : (to >= static_cast<int>(kDjBanks) ? static_cast<int>(kDjBanks) - 1 : to);
    bank = static_cast<std::uint8_t>(to);
    bank_set = true;
    return to != now;
  }
  // A4: the nudge. The direction in force: left alone -1, right alone +1, both or none 0.
  int nudge_dir() const noexcept { return ((nudge_keys >> 1) & 1) - (nudge_keys & 1); }
  void nudge_down(unsigned key) noexcept { nudge_keys = static_cast<std::uint8_t>(nudge_keys | (1u << (key & 1u))); }  // key 0 left, 1 right
  bool nudge_up(unsigned key) noexcept {
    const bool had = (nudge_keys & (1u << (key & 1u))) != 0u;
    nudge_keys = static_cast<std::uint8_t>(nudge_keys & ~(1u << (key & 1u)));
    return had;
  }
  // B1: the row 2 pads in force (down or latched), as a bit mask of the columns.
  std::uint16_t fx_on() const noexcept { return static_cast<std::uint16_t>(fx_held | fx_latched); }
  bool fx_in_force(DjGroup grp, DjFx kind) const noexcept { return grp != DjGroup::Thru && ((fx_on() >> dj_fx_col(grp, kind)) & 1u) != 0u; }
  // A row 2 pad went down (column 0..11): momentary, or with LATCH on a tap toggles it and it stays (a REP latched ends the other REP latched in its group).
  bool fx_down(unsigned col) noexcept {
    if (col >= kDjFxPads) {
      return false;
    }
    const auto bit = static_cast<std::uint16_t>(1u << col);
    const DjFx kind = dj_fx_kind(col);
    if (kind <= DjFx::Rep32) {
      rep_last[col < kDjFxKinds ? 0 : 1] = static_cast<std::uint8_t>(col);
    }
    if (!latch) {
      fx_held = static_cast<std::uint16_t>(fx_held | bit);
      return true;
    }
    if ((fx_latched & bit) != 0u) {
      fx_latched = static_cast<std::uint16_t>(fx_latched & ~bit);
      return true;
    }
    if (kind <= DjFx::Rep32) {
      for (unsigned k = 0; k <= static_cast<unsigned>(DjFx::Rep32); ++k) {
        fx_latched = static_cast<std::uint16_t>(fx_latched & ~(1u << dj_fx_col(dj_fx_group(col), static_cast<DjFx>(k))));
      }
    }
    fx_latched = static_cast<std::uint16_t>(fx_latched | bit);
    return true;
  }
  // A row 2 pad let go: its held FX ends; a latched one stays.
  bool fx_up(unsigned col) noexcept {
    const auto bit = static_cast<std::uint16_t>(col < kDjFxPads ? 1u << col : 0u);
    const bool had = (fx_held & bit) != 0u;
    fx_held = static_cast<std::uint16_t>(fx_held & ~bit);
    return had;
  }
  // The REP pad that decides in a group: the last one pressed while it is in force, else the first one in force, kind 0 (1/4) to 3 (1/32); false when none.
  bool rep_pick(DjGroup grp, DjFx& kind) const noexcept {
    if (grp == DjGroup::Thru) {
      return false;
    }
    const std::uint8_t last = rep_last[grp == DjGroup::A ? 0 : 1];
    if (last < kDjFxPads && dj_fx_group(last) == grp && ((fx_on() >> last) & 1u) != 0u) {
      kind = dj_fx_kind(last);
      return true;
    }
    for (unsigned k = 0; k <= static_cast<unsigned>(DjFx::Rep32); ++k) {
      if (fx_in_force(grp, static_cast<DjFx>(k))) {
        kind = static_cast<DjFx>(k);
        return true;
      }
    }
    return false;
  }
  // Once a pass: ECHO's beat. A group whose ECHO has just come into force is timed from `now_ms`; one beat later (`beat_ms`) its dry level is cut; when ECHO goes, both are forgotten.
  void fx_sync(std::uint32_t now_ms, std::uint32_t beat_ms) noexcept {
    for (unsigned g = 0; g < 2u; ++g) {
      if (!fx_in_force(g == 0u ? DjGroup::A : DjGroup::B, DjFx::Echo)) {
        echo_run[g] = false;
        echo_cut[g] = false;
      } else if (!echo_run[g]) {
        echo_run[g] = true;
        echo_cut[g] = false;
        echo_t0[g] = now_ms;
      } else if (!echo_cut[g] && now_ms - echo_t0[g] >= beat_ms) {
        echo_cut[g] = true;
      }
    }
  }
  // What the Perform layer is told for a track (PerformBank::set_dj_layer): the stutter value, the DELAY offset and the VOLUME; all 0 when the page is not in view or the track is THRU.
  struct FxValues {
    std::int8_t stutter;
    std::int8_t delay;
    std::int8_t gain;
  };
  FxValues fx_values(unsigned track, bool active) const noexcept {
    const DjGroup g = group_of(track);
    const bool whole = active && trans.kind == DjTrans::Echo && trans.echo;  // B2: TRANS ECHO opens the send of every track, THRU included (the whole mix)
    if (!active || g == DjGroup::Thru) {
      return {0, static_cast<std::int8_t>(whole ? 100 : 0), 0};
    }
    DjFx kind = DjFx::Rep4;
    const bool rep = rep_pick(g, kind);
    const bool echo = fx_in_force(g, DjFx::Echo) || whole;
    const bool cut = fx_in_force(g, DjFx::Cut) || echo_cut[g == DjGroup::A ? 0 : 1];
    return {static_cast<std::int8_t>(rep ? dj_rep_value(kind) : 0u), static_cast<std::int8_t>(echo ? 100 : 0), static_cast<std::int8_t>(cut ? -100 : 0)};
  }
  // B2: the filter the sweep asks for (+ HP, - LP), 0 when none; it wins over the hand's `filt` while it is not 0.
  int trans_filter() const noexcept { return trans.kind == DjTrans::Hp ? static_cast<int>(trans.pct) : (trans.kind == DjTrans::Lp ? -static_cast<int>(trans.pct) : 0); }
  // The filter in force: the sweep's while it is not 0, else the hand's (what dj_publish tells the engine, and what the filter bar shows; `filt`, the value the hand returns to, is not touched by a sweep).
  int filt_now() const noexcept {
    const int sweep = trans_filter();
    return sweep != 0 ? sweep : static_cast<int>(filt);
  }
  // The transition ends (cancel, RESET, leaving, the switch): the filter goes back to 0 (`snap`: at once). A queue still held back stays owed: dj_trans_publish gives it to the sequencer.
  void trans_end(bool snap) noexcept {
    trans.kind = DjTrans::None;
    trans.echo = false;
    trans.snap = snap;
    trans.pct = 0;
  }
  // The scratch owns the arena's tail (OS-DJ C1): SCR or TAPE STOP is on, the stop was asked and the engine runs its release or backspin, or the stage is still busy. A project load, an import and a new project
  // (the File actions, and the platform's three starters: `diag load` too) are refused meanwhile, because SampleBank::clear forgets the held tail and the loader writes the samples over the ring.
  bool scratch_holds_arena() const noexcept { return scr != DjScr::Off || (scratch != nullptr && scratch->busy()); }
  // A pad of the page is down.
  bool pad_held() const noexcept { return filt_pad || kill_held != 0u || fx_held != 0u || scr == DjScr::Tape; }
  // Nothing of the page is in force.
  bool at_rest() const noexcept { return pos == kDjCentre && curve == DjCurve::Smooth && filt == 0 && !latch && !filt_pad && kill() == 0u && nudge_keys == 0u && !bank_set && fx_on() == 0u && trans.kind == DjTrans::None && !trans.owes && scr == DjScr::Off; }
  // The page was left: every effect ends, the groups stay (spec section 1).
  void leave() noexcept {
    pos = kDjCentre;
    curve = DjCurve::Smooth;
    filt = 0;
    latch = false;
    filt_pad = false;
    kill_held = 0;
    kill_latched = 0;
    fx_held = 0;
    fx_latched = 0;
    bank_set = false;
    nudge_keys = 0;
    trans_end(false);
    (void)scr_end();
  }
  std::uint8_t want(unsigned track, bool active) const noexcept { return active ? dj_level(group_of(track), pos, curve) : kDjUnity; }
};

// Tells the engine the cut of every track that changed (`active`: the page is in view; otherwise unity for all), as a ParamChange of the strip field XfGain with the 20 ms glide, and the filter
// if it changed (master DjFilter: 60 ms a detent, 120 ms to 0; not in view: 0), and the kill mask if it changed (master KillEq; the engine ramps by itself; not in view: 0). A push the ring refused is retried by the next call. Returns how many were sent. `ring` nullptr: no engine, nothing is told. Main loop (the ring's one producer).
unsigned dj_publish(DjState& dj, bool active, core::ParamRing* ring) noexcept;

// C1, once a pass: carries the scratch to its end (the stop told to the engine, a refused push retried) and gives the arena's tail back once the engine has let go of the ring; a stage that stopped by itself (the export's
// abort) is the same. `active` false (the page is out of view) ends it. Main loop; the take is taken and given back here and in dj_scratch_start only (the arena owner rule).
void dj_scratch_poll(DjState& dj, bool active, SampleBank* bank, core::ParamRing* ring) noexcept;

// B1, once a pass: the row 2 FX into the Perform layer (`bank.set_dj_layer` for every track; `active` false: all zero). `now_ms` is the main loop's uptime, `tempo_x100` the tempo ECHO's beat is counted in. True when a
// track's layer changed: the caller then runs `bank.apply()` (the commands go through the sequencer's ring; a refused one is sent again by the next apply, which the App runs every pass). Main loop.
bool dj_fx_publish(DjState& dj, bool active, std::uint32_t now_ms, unsigned tempo_x100, seq::PerformBank& bank) noexcept;

// A4, once a pass: the nudge to the sequencer. While the page is in view, the transport plays and the clock is ours (`slave` false) the override in force is `dj_nudged(base, dir)`; otherwise 0 (none). `published`
// is the tempo the sequencer says (state_.play.tempo_x100), `rate` the step rate of the performance bank. The base is taken when the nudge starts and kept until the published tempo has left the nudged values,
// so the override never compounds and its end gives back exactly the tempo that was. A push the sink refuses is retried the next pass. True when a command went. Main loop.
// B2, once a pass (before dj_publish): the transition's timer against the published play position. `loop_steps` is the length of the pattern that plays, `queued_pattern` the PATS queue the App remembers (PatternEdit::queued_pattern;
// set here when the held-back pick is told), `transport` the sequencer's sink. Moves the sweep (DjState::trans.pct), opens ECHO in the last beat, holds the queue back for the extra loops, and ends the transition at the
// switch (a loop's end with the queue in force, another pattern playing), when the pick is withdrawn or when the transport stops. A queue refused by the sink is told again by the next pass. Main loop.
// The length in steps of the loop the pattern `pattern` plays: the PAT LEN FX's if the bank has one told, else the project's (the default for a pattern it lacks). `project` or `bank` nullptr: 16.
unsigned dj_loop_steps(const model::Project* project, unsigned pattern, const seq::PerformBank* bank) noexcept;
// B2, a TRANS pad went down (HP, LP or ECHO) while playing: `play` is the published position, `queued_pattern` the PATS queue the App remembers (0: none), `next_row` the Song position to queue when there is
// neither that nor a live jump waiting. HP / LP count the loops the switch is held back for (dj_trans_loops), taking the pick off the sequencer and leaving it owed (given back by dj_trans_publish); ECHO never waits.
// False when the sink refused a command (nothing started).
// `list_ops` (E-SONG-07) is Project::order_ops() now: the order indices a transition holds (jump, hold) are the list's as it was; dj_trans_publish ends the transition when the count has moved (0 and 0: no list to watch).
bool dj_trans_start(DjState& dj, DjTrans kind, const audio::PlayPosition::Value& play, unsigned loop_steps, std::uint8_t queued_pattern, unsigned next_row, TransportSink& transport, std::uint32_t list_ops = 0) noexcept;
void dj_trans_publish(DjState& dj, const audio::PlayPosition::Value& play, unsigned loop_steps, std::uint8_t& queued_pattern, TransportSink* transport, std::uint32_t list_ops = 0) noexcept;

bool dj_tempo_publish(DjState& dj, bool active, bool playing, bool slave, unsigned published, unsigned rate, seq::CommandSink& out) noexcept;

}  // namespace o3os::app
