// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the fixed graph (graph.h) takes as parameters (OS-005b), and the small enumerations it works with. Every value reaches the render as
// a ParamChange (core/param.h) through the engine's ring and is applied by the graph's drain, in the graph's pass after the block's voices; nothing else changes the graph while it renders.
//
// The ids are the model's where the model has them (the kind Track, slot 0..15: Volume, Pan, Mute, Solo, Bus; the kind Master, slot 0:
// Volume, Dry, Reverb, Delay, the three mutes, Limiter), with the model's own units and ranges (a volume or pan 0..100, a switch 0 or 1) and
// the meaning of this OS's mixer (smoothing.h). They are re-exported here under audio:: as this library's own constants, so that no header
// of the engine includes the model and no platform code reaches `model::track_param` through it (the layers: audio does not depend on model);
// tests/audio_model_ids_test.cpp holds every one of them equal to the model's, at compile time, so they cannot drift. The rest are ENGINE
// fields: ids of the same two kinds with a field number from 0x100 up, which the model's tables do not have (find_param refuses them), so no
// on-disk format changes and a generated edit page does not list them. Giving them descriptors and storage (the routing matrix is a project's,
// the cue level and OUT MODE a device's or a project's) is a model change with a version bump and a loader, a later slice's job; until then
// the app pushes them at project load.
//
//   Track slot t (0..15) and the INPUT STRIP (slot 16: the external audio bus, codec RX; ADR 0013 D4, the monitoring card)
//     Volume   0..100  the mixer level law (the Og1114 table: smoothing.h; the strip's fader); the input strip starts at 0 (silent: see graph.h, "monitoring")
//     Pan      0..100  the pan law (tracks only; the input keeps its two channels apart)
//     Mute, Solo       0 or 1 (a muted track is silent everywhere, its sends too; a solo mutes the other tracks, sends included by default
//                      (graph.h, "Solo"); not the input)
//     Bus              accepted and ignored (the USB stem slot of a track is the StemTap's profile, taps.h)
//     Route    0..63   the routing matrix row: kMain | kCue | kSndA | kSndB | kRec | kUsb (route:: below); the input has no USB bit
//     SendA, SendB  0..100  the mixer level law of the send (pre-fader unless the project says post); the master DELAY / REVERB level scales it; the routing bit SNDA / SNDB
//              gates THIS send (the strip's own), not the instrument's (Graph::set_voice_sends: the per-instrument sends, which are added to it and need no routing bit)
//     Source   0..3    a test source until the voices exist (OS-005d): SourceKind
//   Master (slot 0)
//     Volume   0..100  the master level after the chain; Dry: the direct mix (every strip's fader is scaled by it, as in the
//              mixer's model); Delay / Reverb: the level of SNDA / SNDB (they scale the sends); the three Mute switches silence them
//     Limiter  0 or 1  the project's LIMITER switch, told to the master limiter (OS-LIMIT, master_limiter.h): ON puts the three controls below and the project's LIMITER MODE in
//                      force, OFF leaves the base limiter alone (the ceiling is always on)
//     LimiterThreshold, LimiterAttack, LimiterRelease  the three limiter controls (limiter:: below for the units); engine fields, no stored field yet
//     CueLevel 0..100  the level of the CUE bus        OutMode  0 or 1  OutMode
//     MetroMode 0..2, MetroLevel 0..100  OS-METRO: the metronome (metro.h): off, on the CUE side of the line out, on MAIN; its volume, a linear amplitude. Added to the TX block after every tap, so it is in no capture, stem or meter
//     UsbReturnLevel 0..100  OS-006e: the level of the USB return (the host's stream) to the outputs, the mixer level law, 100 = unity (the default), times the master level; the DRY level and DRY MUTE do not touch it; it is in no bus and no capture
//     RetRouteA, RetRouteB  0..3  where the SNDA / SNDB effect's return goes: bit 0 MAIN, bit 1 CUE
//     (0x104 was LevelLaw, a switch between the 1.1.14 table and the 1.2+ instrument law for every level of the graph: removed by OS-007i, and the id is
//     not to be reused. The mixer's levels always go through the 1.1.14 table; the 1.2+ law is the INSTRUMENT volume's, applied by the voices: smoothing.h.)
//
// ParamChange::ramp_samples: 0 smooths over one block, 1 is instant, n >= 2 is ceil(n / 64) blocks (smoothing.h).

#include <o3os/core/param.h>

#include <cstdint>

namespace o3os::audio {

namespace route {
inline constexpr std::uint8_t kMain = 1;
inline constexpr std::uint8_t kCue = 2;
inline constexpr std::uint8_t kSndA = 4;
inline constexpr std::uint8_t kSndB = 8;
inline constexpr std::uint8_t kRec = 16;
inline constexpr std::uint8_t kUsb = 32;
inline constexpr std::uint8_t kAll = 63;
}  // namespace route

enum class SourceKind : std::uint8_t {
  None = 0,   // silence: the strip is skipped
  Sine = 1,   // the strip's test sine (graph.h: test_source_hz), faded in
  Noise = 2,  // white noise at the same level, faded in
  Feed = 3,   // the caller writes the strip's input block before every render (Graph::track_input): what the voices will do
};

// What the input strip is made of (OS-011c, the recorder's source switch; Graph::set_input_feed). Since OS-006e the USB return reaches the outputs by a route of its own (graph.h, "USB return"), so
// Sum and CodecOnly both make the strip the codec's RX alone (the line in, microphone, radio): the host's stream is not in it, whatever the monitor does. ReturnOnly makes the strip the
// host's stream alone (the recorder's USB source: it is recorded through the strip and heard only with the monitor), and while it is so the return's own route is shut, so that the
// monitor does not double it.
enum class InputFeed : std::uint8_t { Sum = 0, CodecOnly = 1, ReturnOnly = 2 };

enum class OutMode : std::uint8_t {
  Stereo = 0,     // the line out is MAIN left and right; CUE has no output
  MainLCueR = 1,  // the line out's left is MAIN (the mono sum), its right is CUE (the mono sum): the split-cue arrangement of product-ux section 4
};

namespace graph_param {
namespace strip {  // ParamKind::Track fields: the model's (0..4), then the engine's
inline constexpr unsigned Volume = 0;
inline constexpr unsigned Pan = 1;
inline constexpr unsigned Mute = 2;
inline constexpr unsigned Solo = 3;
inline constexpr unsigned Bus = 4;
inline constexpr unsigned Route = 0x100;
inline constexpr unsigned SendA = 0x101;
inline constexpr unsigned SendB = 0x102;
inline constexpr unsigned Source = 0x103;
// OS-DJ A1 (new Community OS behaviour, docs/specs/OS-DJ.md): the DJ crossfader's cut on a track, 0..255 (255 = unity, no cut; the DJ page's groups A / B at the crossfader's position), runtime only, never stored: the strip's
// tracker gain is multiplied by it (recompute_strip: the dry level and the sends, as the Perform layer's VOLUME), and the export puts it away and brings it back (Graph::perf_save). Tracks only.
inline constexpr unsigned XfGain = 0x110;
}  // namespace strip
namespace master {  // ParamKind::Master fields: the model's (0..7), then the engine's
inline constexpr unsigned Volume = 0;
inline constexpr unsigned Dry = 1;
inline constexpr unsigned Reverb = 2;
inline constexpr unsigned Delay = 3;
inline constexpr unsigned DryMute = 4;
inline constexpr unsigned ReverbMute = 5;
inline constexpr unsigned DelayMute = 6;
inline constexpr unsigned Limiter = 7;
inline constexpr unsigned CueLevel = 0x100;
inline constexpr unsigned OutMode = 0x101;
inline constexpr unsigned RetRouteA = 0x102;
inline constexpr unsigned RetRouteB = 0x103;
// OS-006e: the USB return's own level to the outputs (Graph: "USB return"). 0x150 follows the limiter's 0x140.. and the send effects' 0x110..0x12E.
inline constexpr unsigned UsbReturnLevel = 0x150;
// OS-METRO: the metronome's two controls (metro.h), told by the device settings METRO (0 Off, 1 Cue, 2 Main) and METRO VOL (0..100). Cue plays on the CUE side of the line out, and on MAIN while OUT MODE is Stereo (no CUE output).
inline constexpr unsigned MetroMode = 0x160;
inline constexpr unsigned MetroLevel = 0x161;
// OS-DJ A2 (new Community OS behaviour, docs/specs/OS-DJ.md): the master DJ filter, -100..+100 per cent (left low-pass, right high-pass, 0 bypassed), runtime only, never stored; the ParamChange's ramp is its glide
// (at least one block). It runs in the `bass` slot of the master chain (master_dj_filter.h) and the export puts it away (Graph::perf_save).
inline constexpr unsigned DjFilter = 0x170;
// OS-DJ A3: the master kill EQ, the bands killed as a mask (1 low, 2 mid, 4 high; a whole number 0..7), runtime only, never stored; the stage ramps every gain by itself (5 ms), the ParamChange's ramp is not used.
// It runs in the `eq` slot of the master chain (master_kill_eq.h) and the export puts it away (Graph::perf_save).
inline constexpr unsigned KillEq = 0x171;
// OS-DJ C1: the master scratch stage (docs/specs/OS-DJ.md 2.6), the `headroom` slot: 0 ends (a release, or a backspin after a fast turn), 1 the platter (SCR), 2 TAPE STOP with the ParamChange's ramp as the beat in samples;
// anything else, or 1 / 2 with no ring staged (MasterScratch::stage), is refused. Runtime only, never stored; the export ends it (Graph::perf_clear).
inline constexpr unsigned ScratchMode = 0x172;
// OS-LIMIT: the master limiter's own controls, ParamKind::Master fields 0x140.. (limiter:: below has the ranges); told to the limiter installed in the graph (Graph::install_master_limiter).
inline constexpr unsigned LimiterThreshold = 0x140;
inline constexpr unsigned LimiterAttack = 0x141;
inline constexpr unsigned LimiterRelease = 0x142;
// OS-SENDFX: the parameters of the send effects (SNDA = delay, SNDB = reverb), ParamKind::Master fields 0x110.. and 0x120.. (sendfx:: below has the ranges).
inline constexpr unsigned DelayTime = 0x110;
inline constexpr unsigned DelayFeedback = 0x111;
inline constexpr unsigned DelaySync = 0x112;
inline constexpr unsigned DelayRate = 0x113;
inline constexpr unsigned DelayPingPong = 0x114;
inline constexpr unsigned DelayTone = 0x115;
inline constexpr unsigned DelayMode = 0x116;
inline constexpr unsigned ReverbSize = 0x120;
inline constexpr unsigned ReverbDamp = 0x121;
inline constexpr unsigned ReverbPredelay = 0x122;
inline constexpr unsigned ReverbDiffusion = 0x123;
inline constexpr unsigned ReverbQuality = 0x124;
inline constexpr unsigned DelayClear = 0x11E;   // 1: forget what the delay holds (a project load): cleared a slice a block while its return is silent (sendfx::kClearField)
inline constexpr unsigned ReverbClear = 0x12E;  // the same for the reverb
// OS-SENDFX: 1 forgets every track's instrument sends (Graph::set_voice_sends): they are the last note's and a project load must not leave them to the next one. Not an effect field.
inline constexpr unsigned VoiceSendsReset = 0x130;
}  // namespace master
}  // namespace graph_param

// The send effects' parameters (OS-SENDFX; radar card `send-fx`). The CONTROL SET is the Mixer's MASTER FX page's (DELAY: ping-pong, sync, sync
// rate, time, feedback; REVERB: size, damping, pre-delay, diffusion); the sound is Community OS's own. A value travels as a ParamChange of kind Master (slot 0), field = kFieldBase[effect] + the
// effect-local number below, in the units below (a whole number in a float where the unit is whole). The graph hands it to the effect installed in the slot (Graph::install_send_fx's
// parameter function), once per change, from the graph's drain (after the block's voices); an effect that has none refuses it. THE CONTROLS ARE THE PROJECT'S (OS-PERSIST2: the SNFX chunk, in the order of app/send_fx_rows.h; the OG
// project's own delay and reverb fields are read by the OG importer into the OGPM chunk and still not mapped into it).
namespace sendfx {
inline constexpr unsigned kFieldBase[2] = {0x110u, 0x120u};  // SNDA, SNDB
inline constexpr unsigned kFieldSpan = 0x10u;                // effect-local numbers 0..15
inline constexpr unsigned kTempoField = 0xFu;                // the engine's own: the tempo in force, BPM x 100 (Graph::set_send_fx_tempo); the delay uses it, the reverb refuses it
inline constexpr unsigned kClearField = 0xEu;                // 1: clear the effect's memory (the master:: DelayClear / ReverbClear above), incrementally, its return silent meanwhile
namespace delay {                                            // effect-local field numbers (the master:: names above are kFieldBase[0] + these)
inline constexpr unsigned Time = 0;      // ms 1..3500, default 500 (the page steps it by 10 below 500 and 30 above); read when SYNC is off
inline constexpr unsigned Feedback = 1;  // % 0..100, default 50 (the page stops at 70, kDelayFeedbackOgMax; the DSP takes up to 100 and never lets the loop reach unity)
inline constexpr unsigned Sync = 2;      // 0 or 1, default 0 (bit 6 of the project's delay byte)
inline constexpr unsigned Rate = 3;      // 0..18, default 0: the index of kDelayRates (bits 0..5 of the project's delay byte)
inline constexpr unsigned PingPong = 4;  // 0 or 1, default 0 (bit 7 of the project's delay byte)
inline constexpr unsigned Tone = 5;      // NEW, 0..100, default 100: 100 leaves the repeats alone, lower is darker (a one-pole low-pass in the feedback loop)
inline constexpr unsigned Mode = 6;      // NEW, 0 MONO or 1 STEREO, default 0: with PING-PONG off, MONO repeats are centred and STEREO puts the right channel's at 3/4 of the time
}  // namespace delay
namespace reverb {
inline constexpr unsigned Size = 0;       // 0..100, default 50: the decay of the tank, the RT60
inline constexpr unsigned Damp = 1;       // 0..100, default 50: how fast the tail loses its highs
inline constexpr unsigned Predelay = 2;   // 0..100, default 50: 0 .. ~93 ms before the reverb hears the send
inline constexpr unsigned Diffusion = 3;  // 0..100, default 80: maps to a coefficient 0..0.85 (80 is 0.68)
inline constexpr unsigned Quality = 4;    // NEW, 0 HALF (the plate runs at 22 kHz: about a third less CPU, a band-limited tail) or 1 FULL (44 kHz), default 0; a change clears the tail
}  // namespace reverb
inline constexpr float kDelayTimeMin = 1.0f;
inline constexpr float kDelayTimeMax = 3500.0f;
inline constexpr float kDelayTimeDefault = 500.0f;
inline constexpr float kDelayFeedbackDefault = 50.0f;
inline constexpr float kDelayFeedbackOgMax = 70.0f;  // where the page stops
inline constexpr unsigned kDelayRateCount = 19;
// The sync rates (this OS's list of 19: 8, 6, 4, 3, 2, 3/2, 1, 3/4, 1/2, 3/8, 1/3, 1/4, 3/16, 1/6, 1/8, 1/12, 1/16, 1/24, 1/32) are lengths in SEQUENCER STEPS (a 16th note at the
// usual 4 steps a beat: "4" is a quarter note, "1" a step), stored here as multiples of 1/96 of a step, so that every one of them is a whole number.
inline constexpr std::uint16_t kDelayRates96[kDelayRateCount] = {768, 576, 384, 288, 192, 144, 96, 72, 48, 36, 32, 24, 18, 16, 12, 8, 6, 4, 3};
inline constexpr float kReverbDefault = 50.0f;
inline constexpr float kReverbDiffusionDefault = 80.0f;
}  // namespace sendfx

// The project's settings the graph takes (ParamKind::Project, slot 0; ids as the model's, tests/audio_model_ids_test.cpp holds them equal): SendsMode 0 pre-fader (the default), 1 post-fader.
namespace graph_param::project {
inline constexpr unsigned LimiterMode = 1;  // OS-LIMIT: 0 Subtle (the slower, gentler preset), 1 Extreme (four times faster, tighter; master_limiter.h), told to the master limiter
inline constexpr unsigned SendsMode = 2;
}  // namespace graph_param::project

// The master limiter's controls (OS-LIMIT; audio/master_limiter.h). THE CONTROL SET is the Mixer's MASTER FX page's limiter rows (attack, release and threshold) and the config item
// LIMITER MODE: threshold 0..100 % of full scale (a threshold of 0 would mute, so 1 % is the least), attack and release 1..1000
// (the 10 % to 90 % time of the stage's response in MILLISECONDS, the "Subtle" reading of its value). THE SOUND IS NEW COMMUNITY OS BEHAVIOUR,
// not measured against the official firmware (PAR-025's presets are unpublished; PAR-058 has the threshold in dB, -inf..0, from 1.3.0: an open question): the values below are ours and documented
// in master_limiter.h. The local field numbers are what MasterLimiter::set_param takes; the graph maps ParamChanges to them.
namespace limiter {
inline constexpr unsigned Switch = 0;     // 0 or 1: master LIMITER (graph_param::master::Limiter)
inline constexpr unsigned Mode = 1;       // 0 Subtle or 1 Extreme (graph_param::project::LimiterMode)
inline constexpr unsigned Threshold = 2;  // %, kThresholdMin..kThresholdMax
inline constexpr unsigned Attack = 3;     // ms, kTimeMin..kTimeMax
inline constexpr unsigned Release = 4;    // ms, kTimeMin..kTimeMax
inline constexpr float kThresholdMin = 1.0f;
inline constexpr float kThresholdMax = 100.0f;
inline constexpr float kThresholdDefault = 92.0f;  // our default
inline constexpr float kTimeMin = 1.0f;
inline constexpr float kTimeMax = 1000.0f;
inline constexpr float kAttackDefault = 10.0f;     // NEW (a 100 ms attack would be far slower than the fast stage that catches what it lets through)
inline constexpr float kReleaseDefault = 100.0f;   // NEW
}  // namespace limiter

// The slot of the input strip in a ParamKind::Track id (the tracks are 0..15).
inline constexpr unsigned kInputStrip = 16;

constexpr core::ParamId strip_param(unsigned slot, unsigned field) noexcept {
  return core::param_id(core::ParamKind::Track, slot, field);
}
constexpr core::ParamId master_param(unsigned field) noexcept { return core::param_id(core::ParamKind::Master, 0, field); }

}  // namespace o3os::audio
