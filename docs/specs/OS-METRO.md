# OS-METRO — the metronome and the count-in (minimal)

Status: specified 2026-10-14 (maintainer decisions), implemented 2026-10-14 (host, simulator and QEMU verified; the hardware checks of section 5 are pending) · Level C for the engine part (a new source in the audio render), B for the rest · The official manual (section 3.5) describes a metronome with a pre-roll, a time signature and a volume, and a release note adds its settings to the Sample Recorder (PAR-112): documented, not verified (`docs/ui/official-manual-map.md`). This item is **new Community OS behaviour** throughout: the name, the beat / bar sound, the countdown text and the rest, listed below; no parity is claimed.

## 0. What exists (verified 2026-10-14)

- No metronome. `device_setting::MetronomeVolume` (id 2) is a dead, dimmed row on Config MIDI (`config_screen.h:14`).
- The grid is fixed: 16 steps a bar, 4 a beat (`seq/sequencer.h:115`, `app/recorder.h:86`); there is no time signature setting, and none is added.
- Buses MAIN and CUE (`graph_params.h:23,49`). **CUE has no output in OUT MODE Stereo, the default** (`graph_params.h:71`); it is heard only in MainLCueR (line out left = MAIN, right = CUE).
- Config PATTERN page: REC POLY, REC QUANTIZE (`params.cpp:175-176`, `config_screen.h:29`).

## 1. Decisions

- **Maintainer (2026-10-14)**: minimal metronome; rows on **Config › PATTERN**: METRO `OFF / CUE / MAIN` (default **CUE**), METRO VOL, COUNT-IN `OFF / 1 BAR / 2 BARS` (default OFF). COUNT-IN lives in Config only (no field on Record). No Pattern key for the metronome. No time signature rows.
- **Orchestrator defaults (reversible)**:
  - **D1 CUE falls back to MAIN** while OUT MODE is Stereo (CUE would be silent); the METRO row shows `CUE (→ MAIN)` then.
  - **D2 The click never enters a recording or a stream**: it is mixed into the codec line out only (MAIN or CUE side), after the RESAMPLE / REC taps, the USB stream and the stems, and it is not in a WAV export.
  - **D3 METRO ≠ OFF clicks whenever the transport runs** (our choice; when the official firmware clicks is an open research question), accent on the bar's first beat, a plain tick on the other three.
  - **D4 The click is synthesised** (a short decaying sine, a higher pitch for the accent; about 15 ms): no sound data of the official firmware is carried over.

## 2. The count-in (COUNT-IN ≠ OFF)

It applies only when the transport is **stopped** and a recording is armed:
- **Pattern**: Shift+REC pressed with the transport stopped (OS-RECMODE2: it starts the transport and LIVE recording together; it replaces "Play pressed in Edit"). Play never counts in, in any mode. With COUNT-IN OFF, Shift+REC from stopped starts LIVE at once.
- **Record**: F1 / Rec arms a take whose ALIGN is not FREE, or a looper take (OS-LOOPER).

What happens (OS-COUNTIN, 2026-10-16, replaces the first design's musical pre-roll): the transport counts N bars **on the start point** (always row 1: the count-in exists only from stop) and the recording is armed at the start point's bar line. During those N bars:
- **The play line waits on row 1** and **nothing of the pattern plays** (the first design started the run N bars before the start point and played the pattern's last N bars, notes included, as a musical pre-roll; replaced). Row 1 is the first step played when the count-in ends.
- **The click plays** if METRO ≠ OFF.
- **Always visual**: the notification strip counts the beats left (`4.. 3.. 2.. 1..`, our label) and the pad LEDs flash each beat, brighter on the bar.
- Notes played or audio heard during the count-in are not recorded (the nearest-step rule of REC QUANTIZE still applies at the start boundary).

With the transport **running** there is no extra count-in: ALIGN's wait for the bar is the count-in, shown the same visual way when at least one beat is left (built: OS-METROALIGN, §6). **Focus Loop (Shift + Play) never counts in.**

## 3. Engine

- The sequencer emits a metronome event on each beat of its timeline (step index multiple of 4; accent on multiples of 16), sample-accurate like any event; the click voice renders it. Bounded, no allocation; state outside the SRAM hot slab.
- Settings: device settings appended to `device_setting` (MetroMode, CountIn; METRO VOL keeps id 2 and moves to the PATTERN group, a page change only); a settings file without them reads the defaults (the SETS keyed-list rule); round-trip test.
- Cost: CPU negligible [est]; flash about 1–2 KB [est]; record the deltas. Budget row 0.2 % and the protocol: `docs/architecture/cpu-budget.md`; not merged until measured on the unit.

## 4. UI

- Config PATTERN: REC POLY, REC QUANTIZE, METRO, METRO VOL, COUNT-IN (5 rows; 6 with REC MICRO). Config MIDI loses the dead METRO VOL row (room for OS-MIDI2).
- A `METRO` chip in the Pattern info line while METRO ≠ OFF (UI map rule 7).

## 5. Acceptance

- Host tests: beat / accent event positions over a pattern and across tempo changes; count-in start offset (N bars before the start, pattern wrap), arming at the bar, nothing recorded during the count-in; D1 fallback; settings round trip and old-file defaults.
- Sim / qemu: the Config PATTERN golden; a scenario that counts in from stop.
- **HW**: the click level and pitch by ear on MAIN and on the split CUE; no click in a RESAMPLE take, in the USB stream or in an export.

## 6. Implementation notes

Where: `audio/{include/o3os/audio/metro.h,src/metro.cpp}` (the click), `graph.cpp` (mix after the taps), `events.h` (`EventKind::Metro`, `kEvCountIn`), `seq/src/sequencer.cpp` (beats, count-in), `app/` (`metro_sync.h`, Play and Record actions, strip and pads, Config rows, the METRO chip), `model/src/params.cpp` (rows 18 and 19), `docs/formats/o3-settings.md`. No persistent format changed except the two appended device rows.

Choices the spec left open (documented, each one reversible):
- **The sound** (D4): 1 kHz plain beat, 1.5 kHz accent, 662 frames (15 ms), a two-pole resonator with no table; the decay ends 96 dB down, below the least bit. **METRO VOL** is a linear amplitude: 100 is 0.7 of full scale on the accent (-3 dBFS), the default 50 is -9 dBFS, the plain beat is 0.8 of the accent. By-ear numbers: constants of `metro.h` / `metro.cpp`.
- **Routing** (D1, D2): the click is added to the codec's TX block after every tap, so it is in no meter, scope, stem, RESAMPLE take or USB stream. OUT MODE Stereo: both channels whatever METRO says; MAIN L / CUE R: Main on the left jack, Cue on the right. A muted render drops it. **The click leads the audible mix by 63 samples (about 1.4 ms)**: it is added after the master chain, whose always-on look-ahead limiter delays the music by `kLatency` = 63 samples (`master_limiter.h`) while the sequencer stamps the Metro event and the Step event of a beat on the same sample; the click is early, never late, by that fixed amount at any tempo (left as it is: delaying it would take a 63-sample line in the render for 1.4 ms).
- **Events**: the sequencer emits `Metro` on the first microstep of every 4th step (the same sample as the `Step` event), only while told the metronome is on (`seq_param::Metro`, a ParamChange the app sends with the mode and level); a bar is 16 steps from the pattern's start, and the count-in counts its own bar, so its first beat is accented whatever the pattern's length. **MIDI clock out**: a counted run announces itself with Start at the first step of the count-in, never SPP + Continue (only a follower that honours a Song Position Pointer would take SPP + Continue for a start; every follower honours Start): the count-in is the follower's bars 1..N and the start point its bar N + 1. A plain start from a step other than 0 keeps SPP + Continue.
- **Count-in**: Pattern mode only. OS-COUNTIN: the run waits `16 x bars` steps on `from_step` (the tracks play nothing, the step does not advance, the clock, swing, MIDI clock and click run); the Step events of the count-in carry the start step and `kEvCountIn`, the published position carries the start step and the steps left, the engine does not offer a counted step to a take that waits for one, so a take armed before the count-in begins on the step after it. Shift+REC from stopped (OS-RECMODE2) counts in; Play, in any mode, never does, and neither does Shift+Play; the Song screen, Browse, a running transport, an external Start and a resume never do. The Record screen's F1 from stop counts in the pattern being edited when ALIGN is not FREE (the aligned modes fell back to FREE with the transport stopped). Notes during the count-in sound and are neither recorded nor entered, except a hit in the last half step of it under REC QUANTIZE Nearest (the default), which is the start step's (the play line waits on it, so the hit is not rounded again: `rec_note`); under Playing step it is the count-in's and is dropped. A Stop during the count-in ends the countdown at once (the strip's text is dismissed) and gives a take that waits for the start point up (`Recorder::poll`: the published transport falls; the same for an aligned take armed on a running transport), so a later Play starts nothing by itself. With the outputs still closed (the boot fallback of the Play key) the count-in is dropped. Not built: see the list at the end of this section.
- **Visual**: the strip shows `RECORDING IN 4.. 3.. 2.. 1..` (our label), the digits so far, one beat each, rewritten in place and gone with the last beat; the pads flash for 120 ms on each beat, Back on a beat and Front on the first of a bar; the REC badge and frame turn red at the start point. The count-in is published a microstep early (the cursor of the sequencer is already the start point's during the last microstep of the count-in, as the published step is for every step).
- **UI**: the PATTERN page rows are REC POLY, REC QUANTIZE, METRO, METRO VOL, COUNT-IN (the spec's order; the table order puts METRO VOL first); the METRO row reads `Cue (-> Main)` while OUT MODE is Stereo; a `METRO` chip beside the mode chip whenever METRO is not Off, which is the default, so every Pattern screen shows it by default.
- **Not built** (a follow-up, not a regression): the looper take (§2: OS-LOOPER is not in this tree).
- **ALIGN wait on a running transport** (OS-METROALIGN, built): a take armed on the Record screen while the transport runs, with START ON STEP, BAR or PATTERN, is shown the way the count-in from stop is: the same strip text, the same pad flash (`App::poll_count_in`, one path; the click follows METRO as for any run: no extra click). The steps left are `wait_steps_left(align, published step, the pattern's length)` (`recorder.h`: Bar the next multiple of 16 or the pattern's end, Pattern step 0, Step one), the beat is `wait_beat` (ceil of steps / 4, as the count-in's). It starts only when at least one beat (4 steps) is left at the moment the take is armed, and a countdown that has begun runs to the take's start (so arming in the last beat shows nothing, by design). The recorder says what it waits for (`Recorder::wait_align()`, set by `arm()`); no new shared state: the published position word is read as before. The pass in which a count-in ends is skipped (the recorder still says "waiting" for one pass, and a bar-long countdown would flash). The take and the engine are unchanged: the start Step event, the sample of its first frame and the give-up on a stop are as before (`recorder_test.cpp` "the take starts at the bar's own sample" holds on both sides). Display only: a Break or Jump FX inside the pattern moves the boundary, and the countdown follows the position it reads. Focus Loop is not a take and never counts. No format change; main loop only, no SRAM_L code. Simulator: `playhead <step> wait <bar|pattern|step>`. Rows QA-PAT-080..085, QA-REC-010.
