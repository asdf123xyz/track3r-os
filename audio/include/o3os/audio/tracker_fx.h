// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The tracker FX (OS-COREFX, ADR 0004 amendment 2; docs/specs/OS-COREFX.md): what the Modern engine's sampler voices do for the effects of a MOD, S3M, XM or IT row. They are the
// source trackers' effects, so they follow those trackers' tick rules (slides on ticks 1.., fine slides on tick 0) and last the ROW they stand in, which is why they are not control-table
// FX (those hold until their column changes): the sequencer consumes them as Row FX and sends a TrkFx event for each at the start of its row, before the row's NoteOn and its Tick 0 on the same
// sample, and a TrkFx event of type 0 for a column that held one on the row before and holds none now (audio/events.h), so the effect ends with its row.
//
//   46 Arpeggio xy        tick t of the row plays the note + 0, x, y semitones for t % 3 = 0, 1, 2 (value 0: none). The pitch moves in whole semitones (exact on the sampler's table).
//   47 Volume slide xy    from tick 1: x up, else y down, one step of 1/64 a tick; held to 0..64.
//   48 Fine volume slide  the same, once, on tick 0.
//   49 / 50 Fine porta    the channel's PERIOD (Scream Tracker's units, 1/4 of a Paula period unit; C-4 = 1712) moves by the value, down (pitch up) for 49 and up for 50, once on tick 0 and for good:
//                         it holds until a new note, as it does in the trackers. The period is the note's from the table of Scream Tracker 3 and ProTracker (x 4), scaled by 8363 / the sample's
//                         recorded rate as Scream Tracker's C2SPD does it (note_period_at: none for a sample with no recorded rate or at 44,100, 8,287 or 8,363 Hz), the pitch the voice gets is
//                         the Slide offset (whole semitones plus the fraction that gives the frequency ratio period of the note / period under the voice's linear-between-semitones law),
//                         which is exact against libopenmpt to a tenth of a cent.
//   CF2 (docs/specs/OS-COREFX.md section 5):
//   51 Pan slide xy       the track's PAN, absolute 0..100 on the graph's strip (Graph::set_track_pan), counted here in 1/256 as libopenmpt does: from tick 1 y right else x left, 4/256 a tick; Fy fine left x,
//                         xF fine right y, once on tick 0 (the fine form wins when it applies: PFF is fine left). The pan holds after the row. It starts from the strip's own pan, from the Panning FX (31)'s if it is held,
//                         or from the pan the processor holds (57). A tracker's pan outlives its notes AND the step that set it: a pan the slide moved is the processor's pan, and a note's start, which puts the
//                         instrument's pan on the track, is followed by the processor's pan again (Engine::dispatch -> note_started). Only the transport start (FxReset) gives the tracks back to the
//                         instruments' pans (at their next note). NEW behaviour.
//   52 / 53 Waveform      the vibrato's (finetune LFO) or tremolo's (amp LFO) shape, the tracker's 0..7 (VoicePool::tracker_wave): set once at the row and kept until the transport starts. The LFOs run on the
//                         transport, so "no retrigger" (bit 2) has nothing to switch, and the sine is the triangle. Lasts nothing: the TrkFx of type 0 that follows it ends nothing.
//   54 Glissando          on (value != 0) or off: the Glide and Slide ramps of the track move in whole semitones (VoicePool::tracker_gliss).
//   55 Retrigger xy       every y ticks, as Scream Tracker's Q (libopenmpt's count): on every tick that carries the effect the processor restarts the sample when its count has reached y, and then counts that
//                         tick; so the first restart of a run is on tick y of its first row, the next y ticks after, across the rows, and a row without Q holds the count (Q03 on rows 1 and 3: restarts on
//                         tick 3 of row 1 and on tick 0 of row 3). A new note starts the count at 0. Each restart first changes the volume by x (0 and 8 none, 1..5 minus 1, 2, 4, 8, 16, 6 times 2/3,
//                         7 times 1/2, 9..D plus 1, 2, 4, 8, 16, E times 3/2, F times 2, held to 0..64) and then restarts the sample (VoicePool::tracker_retrigger: the voice keeps its level and its age).
//                         y = 0 never restarts. A one-shot sample that has already ended is gone: nothing to restart.
//   56 Tremor xy          the volume is gated: x + 1 ticks on, y + 1 off, the position counted on every tick of a row that carries it and kept across rows without one (libopenmpt: nTremorCount); the gate is the
//                         volume 0, the tracker volume stays, so the ticks after the effect ends are the channel's own again.
//   57 Channel pan       the track's ABSOLUTE pan, 0..100 (50 the centre), set at the row and HELD like a tracker's pan: it outlives the step, the notes (note_started puts it back after the instrument's pan) and
//                         the Panning FX's end, until another Channel pan, a pan slide that moves it, or the transport start (FxReset). What MOD 8xx / E8x and S3M Xxx / S8x import as. NEW behaviour, ADR 0004
//                         amendment 2; the Export -> OG writer drops it with the rest of 43..127.
//   CF3 (docs/specs/OS-COREFX.md section 6):
//   58 Channel volume     0..64, the track's level scale (64 full), set at the row and HELD like Channel pan: over the notes, until another one, a channel volume slide that moves it, or the transport
//                         starts (FxReset) or stops (Stopped). It scales the track's strip (Graph::set_track_gain: the dry level and the sends, pre- or post-fader), not the voice's
//                         volume, so a slide of the note's own volume and the channel volume multiply, as in the trackers; libopenmpt's M above 64 is ignored (the importer sends none).
//   59 Channel vol slide  xy, Scream Tracker's N as libopenmpt plays it: from tick 1 y down else x up, one step of 1/64 a tick; xF fine up x, Fy fine down y, once on tick 0 (the fine
//                         form wins when it applies); held to 0..64; the new channel volume holds after the row. 
//   60 Global volume      0..64, every track's level scale on top of its channel volume (the product is the strip's gain), held as 58; one value for the whole song (reset by FxReset and Stopped).
//   61 Global vol slide   xy, W as libopenmpt plays it: from tick 1 x up else y down (the order is the opposite of the channel's), 1/64 a step; xF fine up, Fy fine down, once on tick 0.
//                         A row with the effect on two tracks slides twice.
//   62 Panbrello xy       x the speed, y the depth; a zero nibble keeps the track's last (libopenmpt: the nibbles' memory). From the tick-0 of the row that carries it, each tick the pan moves by
//                         (sine64((pos + 16) >> 2) x depth + 2) / 8 of 256 and the position advances by the speed, as libopenmpt's S3M panbrello (its 64-point sine table, depth 15 nearly the whole range);
//                         the position is kept across rows and over notes (libopenmpt's S3M: measured, a note does not restart it). The offset is added to the pan in force (Graph::set_track_pan_mod: it does not change the pan the processor holds)
//                         and ends with the row that stops carrying it. The waveform S5x and the IT form of the panbrello (held after its row, IT's table) are not modelled.
//   Panning FX (31, the ModSet / ModEnd of the Panning group) is NOT held like a tracker's pan: it follows the sequencer's FX hold. Its value 0..100 is the track's absolute pan from the moment the FX is
//                         applied and again at each note while the sequencer holds it (note_started), and it ends where the sequencer's FX end: at the next note step that does not hold it, on FX Off, or when
//                         another FX replaces it (the sequencer sends the ModEnd). The ModEnd gives back the pan of the track's last instrument (note_started was told it), or the pan the processor holds (57,
//                         a pan slide). OS-PERFFX P2: the performance layer's PANNING is not the processor's pan: a ModSet whose layer is kLayerPerf carries the layer's value (`value`, -100..100) and, in a and b, the
//                         FX underneath when the track holds one: the FX is applied as above whatever the winner, and the layer's value goes to the strip (Graph::set_perf_pan), which adds it to the pan in
//                         force; the layer's end (a ModSet of a step layer, or the ModEnd) takes it off again. The performance VOLUME (the ModSet / ModEnd of the Volume group, kModGroupVolume) is the
//                         same: the strip's perf_gain (Graph::set_perf_gain). Neither is reset by FxReset or Stopped (the Perform screen owns them), and the export puts them away (Engine::offline_enter).
//                         OS-PERFFX P3: the Delay send and Reverb send groups the same way (kModGroupDelaySend, kModGroupReverbSend): the step FX 30 / 32 (a, b) is the track's send while held (Graph::set_step_send,
//                         in place of the instrument's) and the layer's value (`value`, -100..100) an offset of a full send (Graph::set_perf_send); the ModEnd takes both off, FxReset the step FX's only.
// Volume: the voice's tracker volume 0..64 is the instrument's own (SampleDesc::amiga_vol) or the note's velocity ((velocity + 1) / 2, the Amiga engine's and the importer's rule); a change
// sets the voice's gain on the scale the note began with: vol / 64 of unity for a note that named no velocity (OS-BUG-PASTE24; the instrument's own volume is its gain against unity, held at 64, so
// above level 50 the first volume set drops the note to the unity scale), or the linear velocity law, so a bare note keeps what the importer baked for it.
//
// Per tick (Tick events: 50 to 300 a second): one pass over the 16 tracks, each costing a compare when it has no effect, and, for one with an effect, a voice lookup, a few integer operations, a
// single-precision log2 only when a porta moved the period, and, when the pitch or the volume changed, one call into the voice (Slide offset, regain). Integer and single precision, no heap, no
// loop beyond the 16 tracks, no libm. Render context, in flash (O3OS_AUDIO_COLD): the hot kernels of SRAM_L are not touched by it.
//
// What it does not do: a note that Micro-move delays into the row starts after the row's tick 0, so the effects of tick 0 (fine slides) act on the voice that was sounding; the next tick
// finds the new note and starts its state over. A MIDI instrument, an SD module and the Amiga voices have no sampler voice on the track and are not touched (the Amiga voices run the
// same effects as MED commands: amiga_voices.h).

#include <o3os/audio/events.h>
#include <o3os/audio/graph.h>
#include <o3os/audio/hot.h>
#include <o3os/audio/voices.h>

#include <cstdint>

namespace o3os::audio {

namespace trkfx {

// The period, in the units of Scream Tracker 3 (a quarter of a ProTracker period unit), of the 12 notes of the octave of the sampler's unity note (S3M C-4 = 1712, ProTracker C-2 = 428).
inline constexpr std::uint16_t kPeriod12[12] = {1712, 1616, 1524, 1440, 1356, 1280, 1208, 1140, 1076, 1016, 960, 907};
inline constexpr unsigned kUnityNote = 60;  // the voice's note (the model's note byte 61, the importers' C-4 / C-2): the sample at its own rate
inline constexpr std::int32_t kPeriodMin = 16;          // the period is held to this range so that its logarithm is always defined
inline constexpr std::int32_t kPeriodMax = 1 << 22;
inline constexpr int kPanMax = 256;  // the pan processor's scale (libopenmpt's), 128 the centre; the graph's pan is x 100 / 256

// The period of note byte `note` (0..127): the table's entry shifted by the octaves from the unity note.
constexpr std::int32_t period_of_note(unsigned note) noexcept {
  const int d = static_cast<int>(note > 127u ? 127u : note) - static_cast<int>(kUnityNote);
  const int octave = d >= 0 ? d / 12 : -((11 - d) / 12);  // floor(d / 12)
  const std::int32_t p = kPeriod12[d - 12 * octave];
  return octave >= 0 ? (p >> octave) : (p << -octave);
}

constexpr std::int32_t clamp_period(std::int64_t p) noexcept { return p < kPeriodMin ? kPeriodMin : (p > kPeriodMax ? kPeriodMax : static_cast<std::int32_t>(p)); }

// OS-BUG-MODPLAY F4: Scream Tracker's period of a note is the table's (above) scaled by 8363 / C2SPD of the sample, so a slide of 16 units is a larger step on a sample of a higher C2SPD
// (21.5 cents at 11,025 Hz, 16.2 at 8,363 Hz). The period of note byte `note` for a sample recorded at `rate_hz` (VoicePool::TrackerView::rate_hz): the table's, scaled by 8363 / rate, to the nearest,
// the rate held to 1,000..96,000 Hz and the period to its range. Unchanged (the table's) for no recorded rate or 44,100 Hz (0: the OG's samples and the unity of the engine, which keep the
// documented "C-4 = 1712"), and for the Paula rates 8,287 Hz (PAL) and 8,363 Hz (NTSC), so that every MOD, whose samples are at them, keeps the slides it has (D1). Integer; a note start's one multiply and divide.
inline constexpr std::uint32_t kPeriodBaseHz = 8363;
inline constexpr std::uint32_t kPeriodRateMinHz = 1000;
inline constexpr std::uint32_t kPeriodRateMaxHz = 96000;
inline constexpr std::uint32_t kPalPaulaHz = 8287;
constexpr std::int32_t note_period_at(unsigned note, std::uint32_t rate_hz) noexcept {
  const std::int32_t p = period_of_note(note);
  if (rate_hz == 0u || rate_hz == kPalPaulaHz || rate_hz == kPeriodBaseHz) {
    return p;
  }
  const std::uint32_t r = rate_hz < kPeriodRateMinHz ? kPeriodRateMinHz : (rate_hz > kPeriodRateMaxHz ? kPeriodRateMaxHz : rate_hz);
  return clamp_period(static_cast<std::int64_t>((static_cast<std::uint32_t>(p) * kPeriodBaseHz + r / 2u) / r));  // (p <= 54,784: the product is under 2^31)
}

// The semitones above the note that tick `tick` of an arpeggio `xy` plays.
constexpr unsigned arp_semitones(std::uint8_t value, unsigned tick) noexcept {
  const unsigned phase = tick % 3u;
  return phase == 0u ? 0u : (phase == 1u ? static_cast<unsigned>(value >> 4) : static_cast<unsigned>(value & 15u));
}

// A volume slide `xy`: x up, else y down (ProTracker's Axy, Scream Tracker's D without its fine forms, which the importer sends as the Fine volume slide).
constexpr int slide_step(std::uint8_t value) noexcept { return (value >> 4) != 0u ? static_cast<int>(value >> 4) : -static_cast<int>(value & 15u); }

constexpr int clamp_volume(int v) noexcept { return v < 0 ? 0 : (v > 64 ? 64 : v); }

// The volume after a retrigger of Scream Tracker's Q with volume code x (0..15); integer divisions as libopenmpt's (64 x 2/3 = 42).
constexpr int retrig_volume(int vol, unsigned x) noexcept {
  switch (x) {
    case 1: return clamp_volume(vol - 1);
    case 2: return clamp_volume(vol - 2);
    case 3: return clamp_volume(vol - 4);
    case 4: return clamp_volume(vol - 8);
    case 5: return clamp_volume(vol - 16);
    case 6: return clamp_volume(vol * 2 / 3);
    case 7: return clamp_volume(vol / 2);
    case 9: return clamp_volume(vol + 1);
    case 10: return clamp_volume(vol + 2);
    case 11: return clamp_volume(vol + 4);
    case 12: return clamp_volume(vol + 8);
    case 13: return clamp_volume(vol + 16);
    case 14: return clamp_volume(vol * 3 / 2);
    case 15: return clamp_volume(vol * 2);
    default: return vol;
  }
}

// A pan slide `xy`'s change of the pan (1/256) on tick `tick`: the fine forms (Fy: x left; xF: y right, with y = F and x != 0 left first) once on tick 0, the plain one from tick 1 (y right else x left).
constexpr int pan_step(std::uint8_t value, unsigned tick) noexcept {
  const int x = value >> 4;
  const int y = value & 15;
  if (y == 15 && x != 0) {
    return tick == 0u ? -4 * x : 0;
  }
  if (x == 15 && y != 0) {
    return tick == 0u ? 4 * y : 0;
  }
  return tick == 0u ? 0 : (y != 0 ? 4 * y : -4 * x);
}

// CF3. A channel volume slide `xy` (Scream Tracker's N, libopenmpt's ChannelVolSlide): the step (+-, 1/64) on tick `tick`. xF is a fine slide up by x and Fy one down by y, once on tick 0 (xF wins
// when x != 0, so FF is +15); the plain one is from tick 1: y down when y != 0, else x up.
constexpr int chan_vol_step(std::uint8_t value, unsigned tick) noexcept {
  const int x = value >> 4;
  const int y = value & 15;
  if (y == 15 && x != 0) {
    return tick == 0u ? x : 0;
  }
  if (x == 15 && y != 0) {
    return tick == 0u ? -y : 0;
  }
  return tick == 0u ? 0 : (y != 0 ? -y : x);
}

// A global volume slide `xy` (W, libopenmpt's GlobalVolSlide on the 0..64 scale of S3M and XM): the same forms, but the plain one goes up first: x up when x != 0, else y down.
constexpr int global_vol_step(std::uint8_t value, unsigned tick) noexcept {
  const int x = value >> 4;
  const int y = value & 15;
  if (y == 15 && x != 0) {
    return tick == 0u ? x : 0;
  }
  if (x == 15 && y != 0) {
    return tick == 0u ? -y : 0;
  }
  return tick == 0u ? 0 : (x != 0 ? x : -y);
}

// The panbrello's sine, libopenmpt's ModSinusTable: 64 points of round(127 sin(2 pi i / 64)), from a quarter of them (17 bytes).
constexpr int panbrello_sine(unsigned i) noexcept {
  constexpr std::uint8_t kQuarter[17] = {0, 12, 25, 37, 49, 60, 71, 81, 90, 98, 106, 112, 117, 122, 125, 126, 127};
  const unsigned j = i & 31u;
  const int v = kQuarter[j <= 16u ? j : 32u - j];
  return (i & 32u) != 0u ? -v : v;
}

// The pan offset (1/256 of the range) of a panbrello at position `pos` (0..255, wrapping) and depth 0..15: libopenmpt's ((table[(pos + 16) >> 2 & 63] x depth) + 2) / 8, integer division.
constexpr int panbrello_delta(unsigned pos, unsigned depth) noexcept { return (panbrello_sine(((pos + 0x10u) >> 2) & 0x3Fu) * static_cast<int>(depth) + 2) / 8; }

// The pitch offset, in 1/65536 semitone, that puts a voice playing a note of period `note_period` at `period`: the frequency ratio note_period / period under the voice's own pitch law (linear
// between two semitones, so the fraction is not 12 log2 / 1 of the exponent: see the definition). Single precision (core::log2_fast, exp2_fast).
std::int32_t offset_q16(std::int32_t note_period, std::int32_t period) noexcept;

}  // namespace trkfx

class TrackerFx {
 public:
  static constexpr unsigned kTracks = kVoiceTracks;

  // A TrkFx event (the column's effect from now on, or none), a Tick event (every tick runs the effects in force) or an FxReset (the transport starts: every state ends). Other kinds are
  // ignored. Render context.
  O3OS_AUDIO_COLD void event(const SeqEvent& e, VoicePool& voices) noexcept;
  O3OS_AUDIO_COLD void reset(VoicePool& voices) noexcept;
  // Whether the Engine hands this event to event(): Tick, TrkFx, FxReset (every state ends), Stopped (CF3: the channel and global volumes and the panbrello end), the Panning FX's ModSet and ModEnd (held while
  // the sequencer holds it) and, OS-PERFFX P2, the ModSet and ModEnd of the Panning and Volume groups for the performance layer's PANNING and VOLUME and, P3, of the Delay send and Reverb send groups:
  // the step FX 30 / 32 and the layer's DELAY / REVERB, which go to the strip's sends (Graph::set_step_send, set_perf_send); FxReset ends the step FX's (Graph::clear_step_sends), never the layer's.
  static bool takes(const SeqEvent& e) noexcept {
    return e.kind == EventKind::Tick || e.kind == EventKind::TrkFx || e.kind == EventKind::FxReset || e.kind == EventKind::Stopped ||
           ((e.kind == EventKind::ModSet || e.kind == EventKind::ModEnd) &&
            (e.flags == kModGroupPanning || e.flags == kModGroupVolume || e.flags == kModGroupDelaySend || e.flags == kModGroupReverbSend));
  }
  // CF2: the graph whose strips' pans the pan FX move (null: they are not applied). A note's start has set the instrument's pan `instrument_pan` (0..100; a module's note: the centre) on the track:
  // note_started remembers it (the Panning FX's ModEnd gives it back) and puts the pan the processor holds over it again: the Panning FX's while it is held, else Channel pan's or a pan slide's.
  void attach(Graph* graph) noexcept { graph_ = graph; }
  O3OS_AUDIO_COLD void note_started(unsigned track, unsigned instrument_pan) noexcept;

  // For tests: the state of a track (the volume 0..64 and the period the processor holds for the note it follows).
  unsigned volume(unsigned track) const noexcept { return track < kTracks ? trk_[track].vol : 0u; }
  std::int32_t period(unsigned track) const noexcept { return track < kTracks ? trk_[track].period : 0; }
  std::int32_t note_period(unsigned track) const noexcept { return track < kTracks ? trk_[track].note_period : 0; }  // the period of the note itself (OS-BUG-MODPLAY F4: at the sample's rate)
  int pan(unsigned track) const noexcept { return track < kTracks ? trk_[track].pan : 0; }  // 0..256, valid when pan_owned
  bool pan_owned(unsigned track) const noexcept { return track < kTracks && trk_[track].pan_owned; }
  unsigned chan_volume(unsigned track) const noexcept { return track < kTracks ? trk_[track].chvol : 64u; }  // CF3: 0..64
  unsigned global_volume() const noexcept { return gvol_; }
  bool pan_fx_held(unsigned track) const noexcept { return track < kTracks && trk_[track].fx_on; }  // the Panning FX (31) is held on the track

 private:
  struct Trk {
    std::uint8_t cur_type[2] = {0, 0};   // the effects in force, one a column (0: none)
    std::uint8_t cur_value[2] = {0, 0};
    std::uint8_t note = 0;               // the note the state below follows
    std::uint8_t vol = 0;                // its tracker volume 0..64 now ...
    std::uint8_t vol_given = 0;          // ... and the one the voice was last given (equal: nothing to give)
    bool arp_on = false;                 // the pitch last given held an arpeggio step: the next tick without one takes it away
    std::uint32_t age = 0;               // the voice (VoicePool::TrackerView::age) the state belongs to; 0: none
    std::int32_t period = 0;             // the channel's period now (a Fine porta moves it) ...
    std::int32_t note_period = 0;        // ... and the note's own
    std::int32_t pitch_given = 0;        // the pitch offset the voice was last given, 1/65536 semitone
    // CF2
    std::int16_t pan = 128;              // the track's pan, 0..256 (kPanMax), when pan_owned
    bool pan_owned = false;              // the processor holds a pan (Channel pan or a pan slide ran): a note's start gives it back
    std::int8_t pan100 = -1;             // ... the 0..100 Channel pan set it from (exact on the strip), -1 once a slide has moved it
    bool fx_on = false;                  // the Panning FX (31) is held (the sequencer's ModSet, until its ModEnd) ...
    std::uint8_t fx_pan = 50;            // ... at this pan, 0..100
    std::uint8_t inst_pan = 50;          // the pan the last note's instrument gave the track (0..100): what the Panning FX's end gives back
    bool gated = false;                  // the tremor has the volume at 0 now
    std::uint8_t rt_cnt = 0;             // retrigger: the tick count of libopenmpt's (see the Retrigger paragraph)
    std::uint8_t trem_pos = 0;           // tremor: ticks into the on + off cycle
    // CF3
    std::uint8_t chvol = 64;             // the channel volume 0..64 (58, 59), held
    std::uint8_t pb_pos = 0;             // panbrello: the position (wraps at 256), kept across rows and notes
    std::uint8_t pb_speed = 0;           // ... its speed and depth (a zero nibble of the effect keeps them)
    std::uint8_t pb_depth = 0;
    bool pb_on = false;                  // ... and whether it has put an offset on the strip's pan (the row that stops carrying it takes it off)
  };

  O3OS_AUDIO_COLD void tick(unsigned index, VoicePool& voices) noexcept;
  void pan_to(unsigned track, Trk& k, std::uint32_t ramp) noexcept;
  void own_pan(unsigned track, Trk& k) noexcept;
  void panning_fx_end(unsigned track, Trk& k) noexcept;
  void gain_to(unsigned track, const Trk& k) noexcept;  // CF3: the strip's gain, channel volume x global volume
  void gains_all() noexcept;
  void end_volumes(bool now) noexcept;                  // CF3: channel and global volume back to full, the panbrello's offset off (a stop, a play); now false (a stop): the strips' gains wait, see gain_stale_

  Graph* graph_ = nullptr;
  std::uint8_t gvol_ = 64;  // CF3: the global volume 0..64
  // CF3, a stop: the states are full again but a strip whose gain was below full keeps that gain (bit per track) until the track's next note (note_started) or the transport start (FxReset), so
  // the release tails of a fade to 0 do not swell back to full level. The pads and the next song start at full. Render context only (event()/note_started()); no per-sample work.
  std::uint32_t gain_stale_ = 0;
  Trk trk_[kTracks];
};

}  // namespace o3os::audio
