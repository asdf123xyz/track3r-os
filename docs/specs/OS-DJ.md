# OS-DJ: a DJ page in Perform for transitions inside one project

Status: spec, 2026-10-19; revised the same day after the maintainer's UX answers. Backlog: **IDEA** row OS-DJ. Nobody codes it until the maintainer approves the row. **Level C**: the audio path (strip gain, master chain) and, in phase C, a peripheral read from the render.

## Purpose

The use case is a live set made of the maintainer's own tracks, **all in one project**: for example, patterns 1-20 are the first track and 21-40 the second. The page:

- moves from one pattern to the next, between parts or between tracks, with DJ-style transitions (filter sweeps, kills, echo-out, note repeats, tape stop, scratch);
- crossfades between two groups of tracks of the playing pattern (for example drums on A and melodies on B).

### Out of scope

The maintainer's answer of 2026-10-19 rules these out:

- Two different patterns playing at once ("decks"), with their source-track remap.
- A cue pre-listen.
- A transition between two projects. Loading a project stops the audio; a gapless project change would be a separate item.

### Process, format and evidence

- **Process:** one risk review and one independent review per slice. `tools/ci.sh qemu` runs before each merge. Each slice measures its CPU on the unit before it merges (`docs/architecture/cpu-budget.md` §5).
- **Format:** none. All DJ state is runtime only (Q2). PERF and SCEN are not touched (§0.6).
- **Evidence:** everything here is **new Community OS behaviour**. Nothing claims parity. The features are generic DJ-mixer functions. Nothing comes from third-party Tracker firmware mods: one of them is based on decompiling the official firmware, and neither has a licence.

## 0. What exists (dev 04ca029; the fact sheet of 2026-10-19)

### 0.1 Perform screen

- **One page.** `PerformScreen` (`app/include/o3os/app/screens/perform_screen.h`) does not override `pages()`, so `Screen::pages()` returns 1 (`screens/screen.h:93`).
- **Next page.** `Screen::repress()` (`screen.h:96-106`) already turns "the screen's own button again" into "next page, wrapping" once `pages() >= 2`.
- **Leaving the screen.** `App::poll_perform` (`app/src/app.cpp:1983-2012`) calls `bank.set_rate(0)`, then `bank.turn_off(sink)` unless the new screen is Mixer or Song, and `bank.enter()` on the way in.
- **Pad LEDs.** They are monochrome:
  - `hal::Leds` takes a level of 0..255;
  - `PadRole {Off, Back, Front}` (`app/include/o3os/app/pad_palette.h:22`);
  - a blink is the app flipping roles every `kBlinkMs` = 250 ms (`pad_lights.h:35-38`).
- **The Shift TRACKS layer.** `PadLayer` (`app/include/o3os/app/pad_layer.h`). Its rows are AFFECTED, MUTE, MUTE MODE and SOLO, and its LEDs come from `tracks_roles()` (`pad_lights.h:68`).

### 0.2 Sequencer

- **Queuing a pattern.** `queue_pattern(id)` switches at the pattern end (`seq/include/o3os/seq/sequencer.h:260`). `queue_jump(order_pos)` jumps to a Song position at the pattern end (`:266`). NEXT PAT uses it (`app/src/perform_actions.cpp:36`). The queued state is published as `view.play.queued`.
- **Tempo override.** `set_perf_tempo(bpm_x100)` / `PerfTempo` wins over the Tempo FX and the project tempo (`sequencer.cpp:127, 355`). Nothing sends it yet, and it is ignored while following an external clock (MIDI Slave, `seq/src/midi_link.cpp:237-336`).
- **Stutter (perf id 17).** A sequencer step repeater: `set_perf_stutter`, values 0..15 (`sequencer.cpp:440, 2050-2057`). It re-sends the track's last note and does not repeat audio.

### 0.3 Audio graph

- **The strip gain hook.** `Graph::recompute_strip` (`audio/src/graph.cpp:613-649`) multiplies `trk_gain` by the cut-only `(1 + perf_gain/100)²` and retargets the smoothed `s.level`. The export saves and restores the perf layer (`perf_clear` / `perf_restore`, `graph.cpp:363-390`).
- **The perf sends.** OS-PERFFX P3 adds the perf DELAY and REVERB layers to the strips' sends. The sends are pre-fader.
- **Master chain.** `kMasterChain` (`audio/include/o3os/audio/graph.h`): Headroom, Saturation, Eq, Bass, Limiter, Space, BitDepth (the Eq and Bass slots were behind the limiter until A3c).
  - The limiter has a processor, and the DJ page's two stages once built: the kill EQ in `Eq`, the DJ filter in `Bass` (bypassed at rest).
  - `install_master_stage(id, MasterFn, state)` / `set_master_bypass` (`graph.h:176-177`). A stage works on float L/R planes in place; any active stage makes the bus float (`chain_on_`).

### 0.4 Encoder

- **Counter.** FTM2 in quadrature mode, 16-bit, free-running (`platform/mk66/input/include/o3os/platform/mk66/input/soc_facts.h:25-46`). 96 counts a turn, 24 detents (`input_map.h:82-84`).
- **Reader.** It is read only by `Device::encoder_count()` (`device.cpp:62`), in the main loop at about 1 kHz. `CNT` is written only at init.

### 0.5 SDRAM

- **The map** has no free region (`platform/mk66/include/o3os/platform/mk66/mem_map.h:87-146`).
- **How the looper gets room.** It holds the sample arena's free tail as a "take" (`SampleBank::take_begin`, `app/src/sample_bank.cpp:264`).

### 0.6 What stays

PERF and SCEN (`docs/formats/o3-project.md:187-250`) are not touched.

## 1. The rule for every slice

- **Hot path.** Net zero bytes in `.ramfunc_l` (`tools/check_ramfunc.py` green).
  - The new processors are master stages, `O3OS_AUDIO_COLD`, active only while non-neutral.
  - Gains change per event through `recompute_strip`, never per sample.
- **Exports are clean.** The offline render saves, clears and restores every DJ field and bypasses the DJ stages. An export with the page in any state is byte-identical to one with the page at rest (the QA-PRF-030 pattern).
- **Runtime only.** No DJ field is saved. At boot every track is THRU, the crossfader is centred, and the filter and the kills are off.
- **DJ only on the DJ page.** Leaving the page, for page 1 or any other screen, ends every DJ effect: the crossfader glides back to the centre (20 ms), and the filter, kills, cuts, repeats, scratch and a running sweep end. A queued pattern stays queued, as with NEXT PAT. Only the group assignment is kept until power off, because it is setup and makes no sound by itself. No DJ state is ever heard or shown outside the page (maintainer, 2026-10-19).
- **One owner of the arena tail.** The scratch ring and the looper's take never both hold it. The second one asks says `NO ROOM: LOOPER IS RECORDING` (or `SCRATCH IS ON`).

## 2. The page (UX)

### 2.1 Where, and what stays when leaving

- **Where.** Perform gets a second page, **DJ**: press Performance again. Page 1 is unchanged.
- **Leaving the page** (for page 1 or any screen) ends every DJ effect; only the groups are kept (Q4, §1).

### 2.2 The screen

The crossfader is the main control, so it is the big bar at the bottom.

```
┌ PERFORM · DJ ──────────────────────────────── 124.0 BPM · BAR 3.2 ┐
│  NOW  PAT 04  ███████░░░░░ 2/4        NEXT  PAT 05  (TRANS HP 8%) │
│  BANK 1  [01][02][03][04][05][06][07][08][09][10][11][12]         │
│                                                                    │
│  GROUP A  tracks 1 2 3 4         GROUP B  tracks 5 6 7 8           │
│  KILL  LOW  MID  HI              FILTER  LP ◀━━━━━●━━━━━▶ HP       │
│                                                                    │
│  A ━━━━━━━━━━━━━━━━━━━━━━━━━━●━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ B      │
│ encoder: crossfader · hold F4 + encoder: filter, springs back      │
├────────────────────────────────────────────────────────────────────┤
│ BANK − │ BANK + │ CURVE │ FILTER │  SCR  │ LATCH │   –   │ RESET │
```

### 2.3 Keys

| Key | Action | Phase |
|---|---|---|
| Encoder | Crossfader: one turn is the whole travel (24 detents), with a 20 ms glide. Shift + encoder is fine (1/96 of the travel a count). Master held + encoder stays the master volume. | A |
| F4 held + encoder | DJ filter, bipolar: left LP, right HP, 0 bypassed. At the release it springs back to 0, unless LATCH is on. | A |
| F1 / F2 | The pattern bank shown on pad row 1, by 12 | A |
| F3 CURVE | SMOOTH (equal power) or CUT (full level until the last step: a scratch fader) | A |
| F5 SCR | Scratch on/off, a toggle: "the hand on the record". The encoder becomes the platter. | C |
| F6 LATCH / F8 RESET | As on page 1. RESET also centres the crossfader, clears the filter, kills and cuts, cancels a transition and ends scratch. | A |
| ← / → held | Tempo nudge −4 % / +4 % while held (`set_perf_tempo`); off in MIDI Slave, and the strip says so | A |
| Shift + ← / → | Tempo ±0.1 BPM, kept (a project tempo edit, one undo unit) | A |

### 2.4 Pads (4 × 12)

```
        1      2      3      4      5      6   │  7      8      9     10     11     12
row 1   ───────────────── patterns of the bank (12): tap = NEXT ─────────────────────────
row 2   REP¼   REP⅛  REP1/16 REP1/32 ECHO   CUT │  CUT   ECHO  REP1/32 REP1/16 REP⅛  REP¼
        └──────────── group A ───────────────┘  └──────────── group B ───────────────┘
row 3   KILL L KILL M KILL H  FILT  TRANS HP TRANS LP │ TRANS ECHO TAPE STOP  –     –     –     –
row 4   A ◀──────────────── crossfader, 12 positions: tap = jump with a glide ────────────▶ B
```

- **Row 1, the patterns** (A).
  - A tap queues the pattern at the end of the one playing (`queue_pattern`). A tap on the queued pad cancels it.
  - LEDs: a pattern that exists is Back, the playing one is Front, the queued one blinks.
- **Row 2, held FX per group** (B). The left half acts on group A and the right half on group B; the halves are mirrored, so each hand has its own.
  - **REP** is the existing note repeat (stutter), a held FX. It repeats each track's last note, not audio, and the manual says so.
  - **ECHO**, held: the group's delay send opens whole; one beat later the group is cut, so the delay tail rings out. At the release the group is back. If the master DELAY level is 0, the strip says `DELAY LEVEL IS 0`.
  - **CUT**, held: the group is silent while the pad is down. Shift + CUT latches it.
- **Row 3, mix and transitions.**
  - **KILL L/M/H** (A): master kill EQ. Held = momentary; Shift + pad latches it.
  - **FILT** (A): held, it gives the encoder to the filter, like F4.
  - **TRANS HP / TRANS LP** (B): an automatic transition, §2.5.
  - **TRANS ECHO** (B): the whole mix echoes out in the last beat before the queued pattern starts.
  - **TAPE STOP** (C): held, the whole mix slows to a stop over one beat; at the release it is live again.
- **Row 4, the crossfader** (A). Pad 1 is all A, pads 2-5 are 10/20/30/40 %, pads 6 and 7 are both the centre, pads 8-11 are 60/70/80/90 % and pad 12 is all B (OS-DJCENTRE). A tap jumps there with a 20 ms glide; that is a cut, which the encoder cannot do.
- **Shift held (setup).** Row 1 cycles the track of each column THRU → A → B. THRU tracks are never crossfaded or cut. The screen shows the groups by name, because the monochrome LEDs (Off, Back, Front) do not say which is which. Rows 2-4 stay MUTE, MUTE MODE and SOLO. Tracks 13-16 are not reachable, as on Pattern (Q6).

### 2.5 Transitions (phase B)

Choose the next pattern on row 1, or leave none and Song order's next slot is taken (`queue_jump`). Then tap **TRANS HP** (or **TRANS LP**):

1. The filter sweeps from 0 towards HP (or LP) over the time left until the switch. The screen shows the progress (`TRANS HP 8 %`).
2. If less than one bar is left, the switch waits for one more loop of the playing pattern, so a sweep always lasts at least one bar.
3. At the switch the filter snaps back to 0 and the new pattern enters clean: the "drop".
4. Tapping the TRANS pad again cancels the sweep; the filter springs back and the queue stays.

### 2.6 Scratch (phase C)

- **What is scratched.** A master stage keeps the last 4 s of the mix in a ring (Q7).
- **Slip.** With SCR on, the output is a play head over that ring. The sequencer keeps running underneath. With SCR off it is live again at the running position, so the set never loses the bar.
- **The platter.** The render reads the FTM2 counter once a block (Q8). The velocity is the count difference, smoothed by a one-pole over about 3 blocks; the position is integrated and read with linear interpolation. One turn is 1.8 s of audio (a 33⅓ record). A still encoder is a held record: silence. (OS-SCRATCH amends this: the velocity is measured from the time between counts, in blocks, with a 25 ms inertia, and the read is 4-point Hermite; see `OS-SCRATCH.md`. The C1 text above is the record of the first build.)
- **Cuts while scratching.** With SCR on the left hand is free: row 2 CUT and the row 4 crossfader pads make transformer and chirp cuts.
- **Backspin.** Turning fast in the last 30 ms before SCR off keeps spinning back with a decay of about 0.5 s, then the mix is live.
- **TAPE STOP** uses the same play head.
- **Honest limits** (the manual says them):
  - the 24 detents are felt;
  - there is no inertia beyond the smoothing;
  - a MIDI jog from a DJ controller can feed the same engine later (the "Official MIDI CC map" row).

## 3. Slices

| Slice | What | Cost [est] |
|---|---|---|
| A1 | The DJ page (`pages()` = 2), the groups (Shift row 1), the crossfader (a new cut-only strip field `xf_gain` in `recompute_strip`, the encoder and row 4), CURVE, RESET, the end of every effect on leaving the page, export clean | FLASH ~2.5 KB; CPU 0 (per event) |
| A2 | The master DJ filter: a stereo float SVF (LP/HP morph, 0 bypassed) in the `Bass` slot of `kMasterChain`; F4 and FILT spring-back; LATCH | ~0.5 % while non-zero |
| A3 | The master kill EQ: a 3-band split (two Linkwitz-Riley crossovers at about 180 Hz and 2.8 kHz, D2 amended), each band's gain 1 or 0 with a 5 ms ramp, in the `Eq` slot | ~3.7 % while a kill is on (4.5 % in the 5 ms ramps; first built at ~1-1.5 % with first-order crossovers) |
| A4 | Pattern row (queue, cancel, banks, LEDs), tempo nudge and fine tempo | 0 |
| B1 | Row 2 per group: REP (stutter on the group's tracks), ECHO (perf DELAY layer plus the perf gain), CUT | 0 DSP |
| B2 | TRANS HP, LP and ECHO (§2.5): an app-side timer driving the A2 filter and the B1 echo against the play position | 0 DSP beyond A2 |
| C1 | Scratch: the encoder read from the render (a platform function pointer), the ring as a master stage, slip, backspin, TAPE STOP | ~0.3 % write while armed, ~1 % while scratching |

- **Order:** A1, A2, A3, A4, B1, B2, C1. Each phase is usable on its own.
- **B1 depends on** OS-PERFFX P3 (the perf DELAY layer).

## 4. Decisions

One table.

- **Q1-Q6 and Q10** are the maintainer's answers of 2026-10-19 (one project; transitions, not two tracks at once) or follow from them.
- **Q7-Q9 and D1-D3** are the spec session's defaults under the maintainer's delegation.

The maintainer can change any row when approving the IDEA row.

| # | Question | Options (recommended first) | Applied |
|---|---|---|---|
| Q1 | Where does it live? | a) Perform page 2 · b) its own screen (no free screen button) · c) replace Perform | a |
| Q2 | Is the DJ state saved? | a) no, runtime only: no format change · b) a new DJ chunk (a version bump, a loader and a round trip) | a |
| Q3 | What is the crossfader between? | a) two groups of tracks of the playing pattern · b) two patterns at once (decks) | a (maintainer: transitions in one project, not two tracks at once) |
| Q4 | Does DJ state survive leaving the page? | a) no: every DJ effect ends on leaving the page; only the groups are kept · b) the crossfader and the latched effects stay, with a `DJ` chip on every screen | a (maintainer, 2026-10-19: "solo DJ en modo DJ") |
| Q5 | Where do the row 1 patterns come from? | a) pattern numbers in banks of 12 · b) the Song's slots | a |
| Q6 | Tracks 13-16 | a) not reachable from the pads, as on Pattern · b) a second assignment page | a |
| Q7 | The scratch ring | a) the arena's free tail while SCR is armed, as the looper's take: up to 4 s, 16-bit stereo, ~705 KB, shorter if less is free · b) a fixed region carved from the map · c) mono from the slack (~2 s) | a |
| Q8 | How does the scratch read the encoder? | a) the render reads FTM2 `CNT` once a block (read-only, through a platform function pointer; scripted on the host and QEMU) · b) the main loop's events through a queue | a |
| Q9 | How does SCR work? | a) a toggle on F5, so a hand is free for the cuts · b) a held pad | a |
| Q10 | Decks, cue pre-listen, a gapless project change | a) out of scope · b) in | a |
| D1 | Crossfader curves | SMOOTH: `cos/sin` equal power; CUT: unity until the last step. A table of 97 floats in flash. | applied |
| D2 | Kill EQ crossovers | 200 Hz and 2.5 kHz, first-order complementary (the bands sum flat when no kill is on) | superseded by the next row |
| D2 amended (maintainer 2026-10-19, kill depth A) | Kill EQ crossovers and depth | Linkwitz-Riley 4th order (two cascaded Butterworth sections, 24 dB an octave), a second-order all-pass on LOW so that the bands sum to an all-pass; -40 dB or deeper at the centre of a killed band; up to 3-4.5 % CPU while a kill is on, 0 when off. Corners 180 Hz and 2.8 kHz (the build's choice: at 200 Hz and 2.5 kHz the MID kill reaches only -38 dB at 700 Hz) | applied (A3b) |
| D4 (orchestrator, 2026-10-19) | Where the DJ stages sit in the master chain | a) before the limiter, so the limiter catches the peaks they add · b) behind it, with a headroom gain of about 3.5 dB while either is in · c) accept the clipping | a (A3c): the DJ stages run before the limiter (orchestrator, 2026-10-19) |
| D3 | Nudge | ±4 % while held; Shift ±0.1 BPM kept; both clamped to the tempo range | applied |

## 5. Real time and hazards

- **No boosts.** The crossfader and CUT only cut, like perf VOLUME Q3 (b): the limiter does not catch a strip's peaks.
- **Master stages.**
  - The kill EQ and the DJ filter run before the limiter (D4, A3c): the limiter's ceiling is the last word on the peaks they add.
  - Each runs only while non-neutral. When all are neutral, the bypass is set again and the bus goes back to Q19.
  - Every on/off has a 5 ms ramp: no clicks.
- **The render reads one register.** The read is atomic (16-bit) and wrap-safe (signed 16-bit difference). Nothing else in the render touches FTM2.
- **The scratch ring.**
  - The write is a bounded copy of 64 frames a block.
  - The read clamps to the written span, and the play head never passes the write head.
  - The arena take is taken and given back only from the app thread.
- **ECHO** never raises the delay feedback; it only opens the send.
- **TRANS** is app-side timing against the published play position: no new sequencer command. Its error is one UI pass, about 1 ms.
- **MIDI Slave.** Nudge and fine tempo are refused, with a strip text. TRANS still works, since the switch is the sequencer's own queue.

## 6. Done when (per slice)

- **Host tests:**
  - the curve table;
  - `xf_gain` in `recompute_strip`, including THRU;
  - the filter and kill stages against a reference: flat sum at neutral; in a killed band −40 dB or deeper at the band's centre (D2 amended; the −60 dB first written is not reachable at the corners of a split of this order and was dropped by the maintainer's kill-depth decision);
  - the nudge maths and the Slave refusal;
  - the TRANS timer: sweep length, the one-bar minimum, cancel;
  - the scratch integrator, with a scripted count: still is silence, one turn is 1.8 s, a reversal plays backwards;
  - the export byte-compare with the page in a non-rest state.
- **Simulator:** one scenario per slice under `sim/scenarios/qa/qa_perform_dj.txt` and goldens of the page.
- **QA catalogue:** rows QA-PRF-100..149 (reserved for OS-DJ) in `docs/qa/catalogue/perform.md`.
- **Unit:** CPU per slice (the cpu-budget §5 method), 0 underruns, and a by-ear check of the curves, the filter, the kills, the transitions and the scratch feel.
- **Docs:** the UI map row for Perform and, after the last slice, the manual's Perform chapter (C1 of OS-MANUAL2 already waits for OS-PERFFX).

## 7. Budget [est]

- **FLASH:** about +2 KB a slice, +12-14 KB for all.
- **SRAM_L:** 0.
- **SRAM_U:** about 40 B (`xf_gain` and the group on the strips, the page state), plus about 64 B for the stage states.
- **SDRAM:** 0 until C1, then the arena tail only while SCR is armed.
- **CPU:**
  - 0 at rest;
  - filter +0.5 %, kills +3.7 % (4.5 % in the 5 ms ramps; D2 amended, counted from the compiled loop), scratch armed +0.3 %, scratching +1 %;
  - worst case about +5 % (filter, a kill held and scratching) against the 17 % headroom of the heavy-song budget.

## 8. Open research questions

- R1: is the render-side FTM2 read stable on the unit? (C1, the first hardware check.)
- R2: is the feel of 24 detents acceptable for scratch with the smoothing? (C1, by hand.)

## 9. State for implementer

- **Order:** A1 first. Brief from §1, §2 and the slice row; do not read this whole spec into the agent's brief.
- **Before code:** a failing host test for `xf_gain` in `recompute_strip` and for the export byte-compare.
- **Stop and report** if a slice needs an SRAM_L byte, a per-sample loop outside a master stage, a format change, or a change to PERF/SCEN.
- **Files likely in scope for A1:**
  - `app/include/o3os/app/screens/perform_screen.h` and `app/src/app.cpp` (`poll_perform`);
  - `pad_layer.h` and `pad_lights.h`;
  - `audio/src/graph.cpp` (`recompute_strip`, `perf_clear` / `perf_restore`);
  - `app/src/bindings.cpp`.

### A1 as built (2026-10-19, branch feat/OS-DJ)

- **Where:** `app/include/o3os/app/dj.h` (the state, the curves, the pad map; `dj_publish`), `app/src/dj_actions.cpp` (pads, encoder, F3, F8, groups), `ViewState::dj` and `dj_page()`, `App::poll_dj`, the field context `perform.dj`, the page's render and LEDs in `perform_screen.cpp` / `pad_lights.cpp`; the engine side is the strip field `graph_param::strip::XfGain` (0x110, 0..255, 255 = no cut) in `Graph::recompute_strip`, `PerfSave::xf`. Tests `tests/dj_test.cpp`, `tests/audio_perform_test.cpp` (the last three cases), scenario `qa/qa_perform_dj`, rows QA-PRF-100..108.
- **Decisions of the build:** the cut multiplies the tracker gain, so it cuts the dry level and the sends together (a group faded out also leaves the delay and reverb sends); `xf_gain` is the last padding byte of `Strip` (no SRAM_U); the cos table is built at compile time (97 bytes of flash); a refused ring push is retried by the next pass (`DjState::sent`); the slots' keys, pads and encoder are inert on page 2 (`on_perform` refuses it) and F1, F2, F4..F7 read `-` until their slice.
- **Deviations from section 2:**
  - Shift + encoder is one position a detent, not one count: the app is given detents (`kEncoderCountsPerDetent`), and the counts would need a platform change.
  - On leaving the page the curve goes back to SMOOTH too (section 1: only the groups are kept); RESET leaves the curve.
  - The centre with SMOOTH is -3 dB on both groups (D1 equal power), heard as soon as a track is in a group.
- **Left for the next slices:** the kills (A3; the filter, LATCH and F4 came with A2), the pattern row, banks and tempo keys (A4: F1, F2 and the arrows are free on the page), row 2 and row 3 pads (B1, B2, C1).

### A2 as built (2026-10-19, branch feat/OS-DJ)

**Engine part**

- **Where:** `audio/include/o3os/audio/master_dj_filter.h` + `audio/src/master_dj_filter.cpp` (the filter, built -O3 from flash like the limiter), `Graph::dj_filter_` (installed in the `Bass` slot by `init_defaults`, bypassed), `graph_param::master::DjFilter` (0x170), `PerfSave::dj_filter`. Tests `tests/audio_dj_filter_test.cpp` (5 cases), `tests/audio_perform_test.cpp` (the last two cases).
- **Contract for the app side:** a ParamChange `master_param(DjFilter)`, value -100..+100 per cent (negative low-pass, positive high-pass, 0 bypassed), `ramp_samples` is the glide (at least one block of 64). The engine switches the stage on at the first non-zero target and off by itself after the glide reaches 0. Out of range or NaN is refused (counted as rejected).
- **Decisions:** one SVF (trapezoidal), k 2 (critically damped, Q 0.5; changed from 1.3 by the phase-A review fold below) so that a sine is never lifted and the low-pass has no overshoot of its own; cutoff law linear in log2 of the warped cutoff, low-pass 21 kHz to 60 Hz, high-pass 10 Hz to 8 kHz (the first ~10 % of the travel is inaudible by construction, so the two halves meet at 0 with both open); the state starts from the first sample, so no step on start; the export puts the filter away with `jump(0)` and gives it back at once.

**Page part**

- **Where:** `DjState::filt` / `filt_sent` / `latch` / `filt_pad` and `turn_filter`, `release_filter`, `toggle_latch`, `at_rest` (`app/include/o3os/app/dj.h`); the actions `dj.filter`, `dj.pad_up`, `dj.key_up` and the F6 branch of `dj.key` (`app/src/dj_actions.cpp`), `dj_publish` sends the filter after the cuts; the bindings (`perform.dj`: F4 `hold_encoder`, F6 tap) in `bindings.cpp`; the three raw-event hooks in `App::run_once` (`app.cpp`); the filter row, the F4 / F6 labels and marks, and the FILT pad's LED in `perform_screen.cpp`. Tests: 7 new cases in `tests/dj_test.cpp`; scenario steps `qa_prf_109..114` in `qa/qa_perform_dj`; rows QA-PRF-109..114; the manual's "Use the DJ page".
- **Decisions of the build:**
  - 10 per detent (ten detents from OFF to either end), glide 60 ms a detent (2646 samples) and 120 ms (5292) to 0, which is also what RESET, unlatching and leaving use.
  - The spring-back is a raw event: F4's release (`dj.key_up`, only while the filter is away from 0, so F4 on every other screen costs nothing) and the FILT pad's release (`dj.pad_up`, only while the pad is held), as `perform.key_up` does, because the gesture layer makes a hold-encoder button's release silent after a turn.
  - The FILT pad is taken ahead of the bindings (`App::run_once`, beside the held STEP pad): a pad has no HoldEncoder.
  - Both held: F4's release does not spring the filter back while the FILT pad is down; the pad's release does.
  - RESET clears the filter and keeps LATCH (a setting, like the curve); leaving clears LATCH too; unlatching springs back at once.
  - The F4 tap without a turn does nothing.
  - The engine is told 0 whenever the page is not in view (`dj_publish(active=false)`), like the cuts.
  - LATCH here is the page's own; the slots page's LATCH (`bank.latch`) is untouched.
- **Deviations from section 2:** none. The filter row sits in the middle of the page (under the gains, over the crossfader) and not beside GROUP B, with the value before the trough (`FILTER LP 50  LATCH`), because the page has no KILL row yet.
- **Hardware:** the by-ear check of the sweep, the spring-back and the latch; the CPU of the stage (the engine slice's measure).

### A3 as built (2026-10-19, branch feat/OS-DJ)

**Engine part**

- **Where:** `audio/include/o3os/audio/master_kill_eq.h` + `audio/src/master_kill_eq.cpp` (-O3 from flash like the A2 filter), `Graph::kill_eq_` (installed in the `Eq` slot by `init_defaults`, bypassed), `graph_param::master::KillEq` (0x171, a whole number 0..7: 1 low, 2 mid, 4 high; anything else refused), `PerfSave::kill_eq`. Tests `tests/audio_kill_eq_test.cpp` (7 cases), `tests/audio_perform_test.cpp` (the last two cases).
- **Maths (A3 as first built; replaced by A3b below):** D2 as written: a trapezoidal one-pole low-pass at 200 Hz takes LOW; what is left goes through a second at 2.5 kHz (MID), the rest is HIGH; the bands are made by subtraction, so they sum to the input exactly. Out = A x + B low + C mid (A = gh, B = gl - gh, C = gm - gh), so with every gain at 1 it is x bit for bit. Each gain moves 64/221 a block (5 ms of travel, the ramp per sample linear inside the block); the stage bypasses itself once no kill is on and the gains are back at 1; it starts from the input's first sample. The ParamChange ramp is not used (the stage owns the 5 ms). The export puts it away with `reset(0)` and gives it back as `reset(mask)` (gains at 1, the kill ramped in: no step).
- **Deviations from section 6 and 4:**
  - **Kill depth (A3 as first built; closed by A3b below).** First-order and complementary is 6 dB an octave: LOW killed is -3 dB at 200 Hz, -12 at 50 Hz, -20 at 20 Hz; MID killed -22 dB at 700 Hz; HIGH killed -17 dB at 20 kHz. Section 6's "-60 dB or less in a killed band" cannot hold with D2 (the complement of any minimum-phase low-pass falls at 6 dB an octave); the tests state these depths instead. A steeper split (LR4 plus an all-pass) is about three times the CPU and not a sum-to-input split. By-ear check on the unit decides whether A3b is wanted.
  - **Level (A3 as first built; closed by A3b below).** Killing HIGH lifts 394 Hz by +0.43 dB (low + mid are phase-shifted against each other); the other kills never lift. The stage is after the limiter, so a full-scale mix with energy at 300-500 Hz can clip at the converter by that much. Remedy in the review fold below (a constant 0.95 while HIGH is killed).
- **CPU [est] (A3 as first built):** about 30 flops a frame, 45-60 cycles, 1.1-1.5 % at 44.1 kHz while a kill is on, 0 otherwise. State 44 bytes in the graph (no SRAM_U beyond the object), the code in flash. Not measured (no mk66 build run, as briefed). A3b: see below.

**Page part**

- **Where:** `DjState::kill_held` / `kill_latched` / `kill_sent`, `kill()`, `kill_down`, `kill_up` (`app/include/o3os/app/dj.h`); `dj_pad` / `dj_pad_up` and `dj_publish` (`app/src/dj_actions.cpp`; the pad release hook in `App::run_once` also fires while a kill pad is held); the KILL row (pitch row 8) and the three pads' LEDs (`perform_screen.cpp`). Tests: 3 new cases in `tests/dj_test.cpp`; steps `qa_prf_115..117` in `qa/qa_perform_dj`; rows QA-PRF-115..117; a line in the manual's "Use the DJ page".
- **Decisions of the build:** the pads are row 3, pads 1-3 (24..26); held = momentary; the spec's "Shift + pad latches" is **LATCH (F6)** here, because a held Shift turns the pads into the TRACKS layer (rows 2-4 are MUTE / MODE / SOLO), so Shift + a row 3 pad is SOLO. With LATCH on a tap toggles the band; unlatching, RESET and leaving end every kill; RESET keeps LATCH. The engine is told 0 while the page is not in view. No new key (the key row has none for the kills).
- **Left:** the unit arm (CPU of the stage with a kill on; by ear: the three kills, the click-free ramps, HIGH's bump on a loud mix); everything of A4 onwards.

### A4 as built (2026-10-19, branch feat/OS-DJ) - phase A is complete

- **Where:** `app/include/o3os/app/dj.h` (the bank, the nudge keys and base, `dj_nudged` / `dj_nominal` / `dj_published`, `dj_tempo_publish`), `app/src/dj_actions.cpp` (row 1 in `dj_pad`, F1 / F2 in `dj_key`, `dj_nudge`, `dj_nudge_up`, `dj_tempo`, `dj_tempo_publish`), `pattern_queue_tap` (`pad_mode_actions.cpp`: the PATS tap, now shared) and `tempo_fine_step` (`song_actions.cpp`: the Song screen's tempo step, now shared), `ViewState::dj_now()` / `dj_bank()`, `dj_pattern_roles` (`pad_lights.cpp`), the NOW / NEXT / BANK line, the pattern numbers and the strip's `BPM` and `NUDGE` chip in `perform_screen.cpp`, the bindings of F1, F2 and the arrows in `perform.dj`, the raw release hook for the arrows and the redraws in `app.cpp`. Tests: 8 new cases in `tests/dj_test.cpp`; steps `qa_prf_118..122` in `qa/qa_perform_dj`; rows QA-PRF-118..122; two steps in the manual's "Use the DJ page".
- **No second mechanism:** a row 1 tap is the PATS tap (`TransportSink::queue_pattern`, cancel by a second tap, stopped it selects the pattern in view); the nudge is the sequencer's `PerfTempo` command (`set_perf_tempo`), told through the Perform board's command sink (no platform change, no new `TransportSink` method); the fine tempo is `Editor::set_tempo_x100` plus the `seq::tempo_param()` ParamChange, as the Song screen does it.
- **Decisions of the build:**
  - The nudge never adds to the song's tempo: the override is cleared at the release, so the tempo after it is the one before, bit for bit. The base is the published tempo with the step rate taken out (the override is nominal), captured when the nudge starts and kept until the published tempo has left the two nudged values, so a quick second press or a pass that has not yet seen the return never nudges a nudge. Both arrows down cancel. While stopped the arrow says `NUDGE NEEDS PLAY` and does nothing; stopping with the arrow down, leaving the page and the release all send 0.
  - A Tempo FX in the song is overridden while the nudge is held (the performance tempo wins, section 0.2) and is back at the release.
  - Slave is `CLOCK MODE` = Slave; the nudge and the fine tempo say `MIDI SLAVE: THE CLOCK SETS THE TEMPO` and change nothing.
  - Shift + the arrow repeats while held (each repeat is one undo unit, as any repeating edit).
  - Banks of 12, 22 of them, held at both ends (no wrap). The bank is the one of the pattern that plays (stopped: the one in view) until F1 / F2 move it, and goes back to that on leaving; the queue stays queued when the page is left (section 1).
  - The page is re-laid: NOW / NEXT / BANK on pane row 2, the numbers on row 3, the group lines moved from row 3 to 4.
- **Deviations from section 2:** no progress bar on the NOW line and no `TRANS` text on NEXT (B2); the tempo is in the strip, not in the header; the bar count `BAR 3.2` is not shown.
- **Phase A is complete:** A1 to A4 are in; B1 (row 2 per group) is next and depends on OS-PERFFX P3.
- **Hardware:** by ear and by BPM readout, the nudge and its return on a running loop (the tempo before and after must read the same), the queue landing at the end of the pattern, the bank keys; the CPU of A4 is a main-loop pass only (no per-block code).

### Phase-A review fold (2026-10-19, branch feat/OS-DJ)

- **DJ filter overshoot.** The review's "+0.1 dB at most" held for sines only. Measured on the real filter with a ceiling-level (-0.3 dBFS) step and squares: k = 1.3 overshoots an edge by 7 % mid-travel and by 10 to 19 % when wide open. Changed to k = 2 (kK): the low-pass has no overshoot from knob -40 to -100 (cutoff under about 8 kHz), proven by `audio_dj_filter_test` (a step both ways and squares at 30 Hz to 8 kHz; the high-pass step too). **Not fixed, open:** from knob -1 to -30 the cutoff is near the Nyquist frequency and the bilinear filter rings by itself, 4 to 31 % over an edge at the ceiling (k = 1.3 was the same, worse), so a ceiling-level kick or square there can still saturate in `float_to_q15`. **Closed by A3c (below):** the stage now runs before the limiter, which catches it. A high-pass passes an edge at its height: a bipolar full-scale square at low frequency through a high cutoff goes over, as with any high-pass. Options for the maintainer: hold the open end of the law at about 8 kHz (a step when it leaves 0), or a headroom gain on the stage. The price of k = 2: at +-1 the response is down 0.5 dB at 16 kHz and up to 1.2 dB at 30 Hz.
- **HIGH kill.** A constant gain of 0.95 (`kHighKillGain`, -0.45 dB) is applied to the sum while HIGH is killed, ramped with the kill; the 394 Hz bump goes from +0.43 to -0.02 dB (`audio_kill_eq_test`, every kill at or under 0 dB). Removed again by A3b (the LR4 split has no lift).
- **Clamp.** `DjState::turn` and `turn_filter` clamp the detents to +-65536 (`dj_detents`) before multiplying, as Song's tempo step does; test with INT_MAX and INT_MIN.
- **Nudge return.** `dj_test` drives the real sequencer through its command ring: the tempo is exactly the one before at the release (reported, nominal, and the step length), the project's tempo untouched.
- **Goldens:** none changed.

### A3b as built (2026-10-19, branch feat/OS-DJ): the steeper kill (D2 amended, kill depth A)

- **Where:** `audio/include/o3os/audio/master_kill_eq.h` + `audio/src/master_kill_eq.cpp` (same files, same `Graph` slot, same `KillEq` ParamChange and `PerfSave`; nothing else changed in the engine). Tests: `tests/audio_kill_eq_test.cpp` (8 cases, rewritten); `audio_perform_test.cpp` unchanged and green.
- **Maths:** each crossover is Linkwitz-Riley 4th order: two second-order Butterworth sections (trapezoidal state-variable form) one after the other for the low-pass, and the high-pass is the second-order all-pass of the corner minus the low-pass (`x - 2 k bp1 - lp4`, exact, because LP4 + HP4 is that all-pass). LOW = LP4(180 Hz) then a second-order all-pass at 2.8 kHz; REST = HP4(180 Hz); MID = LP4(2.8 kHz) of REST; HIGH = REST - 2 k bp - MID. Five sections a channel (three with the band-pass out, two low-pass only). The bands sum to an all-pass: flat within 0.001 dB analytically (6e-15 in double), within 0.02 dB on the real stage, impulse energy 1.
- **Corners 180 Hz and 2.8 kHz, not 200 Hz and 2.5 kHz.** The MID kill's depth at the middle of the band is |LOW + HIGH| there, the two neighbours' leaks, (corner / f)^4 each, in phase: -38.1 dB at 700 Hz with 200 Hz and 2.5 kHz, short of the -40 dB asked. 180 Hz and 2.8 kHz give -41.9 dB. The page, the QA rows and the manual say "about 180 Hz and 2.8 kHz". Reversible: two constants (`kLowHz`, `kHighHz`, and the warped `g` in the .cpp).
- **Depths (analytic, the real stage agrees within 0.05 dB above -60 dB):** LOW killed -44.6 dB at 50 Hz, -62 at 30 Hz, -76 at 20 Hz; MID killed -41.9 dB at 700 Hz (the worst point of the band; -35 at 500 Hz and 1 kHz); HIGH killed -50.5 dB at 10 kHz, -28.6 at 6 kHz, -82 at 16 kHz. At a corner the killed band leaves -6 dB. No kill lifts anything: every response is at or under 0 dB, so the 0.95 constant on the HIGH kill (`kHighKillGain`) is removed; what a HIGH kill takes off is its own skirt only (0.13 dB at 1 kHz, 0.00 below 400 Hz).
- **Entry and exit, no click:** a wet weight crossfades the input and the band sum, ramped 5 ms like the gains: out = (1 - wet) input + wet (bands with their gains). The filters start from the DC state of the first sample, so the stage starts at the input (to one ramp step) and the crossfade hides their settling; a release ends at the input and the stage bypasses itself, bit for bit the input again. Cheaper than running the filters at rest (0 CPU at rest is kept) and it passes the step test. The click bound (`audio_kill_eq_test`): on a full-scale sine at 100 Hz, 1 kHz and 8 kHz, at four phases and for each of the seven masks, on and off, the largest sample step is at most 1.02 x the sine's own plus 2/221 of full scale (0.009); measured at most 0.004 over the sine's own (0.0181 against 0.0142 at 100 Hz, 0.1445 against 0.1423 at 1 kHz, none over at 8 kHz). An instant kill would step by the killed band's amplitude, up to 1. The export's `reset(0)` / `reset(mask)` crossfade back in the same way, with no step.
- **CPU [est], counted from the compiled MK66 loop:** 69 instructions, about 75 cycles a channel and sample, 150 a frame: 3.7 % of the core at 44.1 kHz while a kill is held (a steady-state loop with no dry term); 91 cycles a channel in the 5 ms ramps (4.5 %); 0 at rest. Inside the 3-4.5 % the maintainer allowed. To be measured on the unit.
- **FLASH / RAM:** FLASH +1,176 B (830,008 to 831,184 B; the kill's code 900 to 2,044 B). State per stage 36 to 104 B (+68 B), the engine's SRAM_U object +72 B (`Graph` 14,360 to 14,432 B); the linker's region table is unchanged at the byte (SRAM_L 49,888, SRAM_U 138,120, SDRAM_STATIC 146,592). The SRAM_L hot budget is untouched (the code runs from flash).
- **Peaks (closed by A3c, option (c): the stage now runs before the limiter):** an all-pass turns the phases, so the peak of a limited mix grows while the stage is in, and the stage sits after the limiter. Analytic, with the 180 Hz and 2.8 kHz sum: a synthetic mix with flat tops at the limiter's ceiling (-0.3 dBFS) gains +2 dB on average and up to +4 dB; a ceiling-level square gains 3.3 to 6.6 dB (400 Hz is the worst); a saw 0.8 to 4.5 dB; a kick-like burst changes by +0.1 to -3.2 dB. What passes full scale is saturated by `float_to_q15`, so a hot master can clip while a kill is held. Sines and the band levels are exact; the overshoot is a property of any all-pass-summed split (the DJ filter's open-end overshoot, section 9 above, is a separate item and untouched). Options: (a) accept it; (b) a headroom gain of 3 to 4 dB on the stage while it is in (0 CPU: fold it into the mix coefficients; every kill then drops the level by that much); (c) move the stage before the limiter (the limiter then catches the peaks; the kill's -6 dB corners would then be limited too). As first written this was left to an ear check at the unit; the orchestrator took (c) on 2026-10-19 (A3c below).
- **Hardware:** the CPU of a held kill and of the ramps (the cpu-budget §5 method); by ear the three kills on a full mix (the depth at the band centres), the entry and exit (no click), and a loud flat-topped master for clipping while a kill is held.

### A3c as built (2026-10-19, branch feat/OS-DJ): the DJ stages run before the limiter (D4)

- **Why.** Two proven peak problems on a hot master, both behind the limiter, where the converter clips anything over full scale: the DJ filter near its open end overshoots a ceiling edge by 4 to 31 % (knob -1 to -30, section 9 above), and the kill EQ's all-pass sum raises the peaks of a limited mix (+2 dB on average, up to +4 dB; a ceiling square up to +6.6 dB; A3b above).
- **Move, not the fallback.** The chain is a table whose slot index is the position, so the move is the table: `MasterStage` and `kMasterChain` are now Headroom, Saturation, Eq, Bass, Limiter, Space, BitDepth (`audio/include/o3os/audio/graph.h`). `run_chain` walks the table, so nothing else changed in the engine: the stages' bypass at rest (bit for bit, 0 CPU), their 5 ms ramps, `Graph::kill_engage` / `dj_engage` and the export's `reset(0)` / `reset(mask)` are untouched. The relative order of the two stages is unchanged: the kill EQ (Eq slot) runs first, then the filter (Bass slot). No headroom gain is used, so a kill or the filter lowers no level. The limiter's `|out| <= kCeiling` for any finite input is what holds the ceiling.
- **Side effects.** The MAIN meters are read where the limiter takes the bus (`main_pre_`), so they now count the DJ stages' peaks as the overload the limiter caught (the master meters stay under the ceiling). The limiter's 63-sample delay now also sits behind the stages: the stages' output reaches the converter 63 samples after the block they ran in, as everything else does. The limiter's gain reduction shows while a kill or a low-pass lifts a peak above its threshold; a quiet mix is untouched (gain exactly 1).
- **Tests.** `tests/audio_master_limiter_graph_test.cpp` (suite `audio_limiter`, also under QEMU): on a hot master (four tracks of one full-scale waveform: squares at 100 Hz, 400 Hz and 1 kHz, a saw, two squares summed), with every kill alone and all together, the low-pass from knob -1 to -30, the high-pass at 5 and 50 and a kill with the filter, the output never exceeds the ceiling's code (31,658). Run against the old order (limiter first) the same test fails: 57 of the 60 combinations reach the converter's rail (32,767 or 32,768). The same file proves that with both stages at rest the graph is bit for bit the graph with the two slots empty, and that both put themselves in bypass again after a release. `tests/audio_graph_test.cpp`: the table order and the call order (saturation, EQ, bass, limiter).
- **Goldens.** None changed: the stages are bypassed at rest, and the full host `ci.sh` (206 of 206 ctest, simulator goldens and manual images included) is green on the untouched goldens.
- **FLASH / RAM.** 0 B: the table is the same size and the compare in `run_chain` encodes the same (FLASH 831,184 B, SRAM_L 49,888 B, SRAM_U 138,120 B, SDRAM_STATIC 146,592 B, equal to A3b). CPU: no new code on the render path; the limiter, the filter and the kill cost what they did.
- **Docs.** ADR 0013 amendment 5; OS-005 and os-tree name the new order.
- **Hardware.** On a loud master with a kill held and the filter open: the master meter's clip count stays 0 and no converter clip is heard; the CPU of the stages is as measured for A2 and A3b (the limiter's cost does not change).

### B1 as built (2026-10-19, branch feat/OS-DJ): row 2 per group, REP, ECHO and CUT

- **Where:** `app/include/o3os/app/dj.h` (the pad map `dj_fx_group` / `dj_fx_kind` / `dj_fx_col`, `dj_rep_value`, `dj_beat_ms`; `DjState::fx_held` / `fx_latched` / `rep_last` / `echo_run` / `echo_cut` / `echo_t0`, `fx_down`, `fx_up`, `rep_pick`, `fx_sync`, `fx_values`, `pad_held`), `app/src/dj_actions.cpp` (`dj_fx_publish`, the row 2 branches of `dj_pad` and `dj_pad_up`), `App::poll_dj` (`app.cpp`), the line under the gains and the pads' LEDs (`perform_screen.cpp`), and the one engine-side hook `PerformBank::set_dj_layer` (`seq/include/o3os/seq/perform.h`, `seq/src/perform.cpp`). Tests: 7 new cases in `tests/dj_test.cpp`; steps `qa_prf_123..127` in `qa/qa_perform_dj`; rows QA-PRF-123..128; a step in the manual's "Use the DJ page".
- **No new DSP, no new engine feature.** The three FX are the Perform layer's own: REP is `PerfStutter` (the sequencer's performance stutter, which retriggers the last note of the track: notes, not audio), ECHO is the DELAY send offset +100 (`PerfControl`, group DelaySend, `Graph::set_perf_send`) and CUT is VOLUME -100 (`PerfControl`, group Volume, `Graph::set_perf_gain`). They go out as `seq::Command` through the Perform board's sink, as the slots' commands do. The cut of the crossfader (`xf_gain`) is a different strip field and is untouched: the two multiply.
- **The layer over the slots (the one decision that touches `seq/`).** The slots page and the DJ page share the sequencer's control table, so a bare command from the DJ page would end a slot's FX behind the bank's back (the bank only resends what it thinks changed). `PerformBank::desired()` therefore has a second source: a per-track DJ layer (3 bytes a track, 36 B in the board, in SDRAM) that wins over the slots while a value is not 0, whether or not the track is AFFECTED; at 0 the slots' value is told again by the next `apply()`, with no help from the slots' pads. The layer is not in `state()`, the scenes or any file; `reset()` clears it. The DJ page sets it every pass from `DjState` (`dj_fx_publish`: THRU tracks and a page not in view are all 0) and runs `apply()` itself when a track's layer changed; a refused push is sent again by the App's apply of the next pass.
- **Pads (row 2 = pads 12..23, 0-based).** Left half group A: REP 1/4, 1/8, 1/16, 1/32, ECHO, CUT; right half group B, mirrored. Held = momentary. REP values are note values on the 16th-note step: 1/4 = value 5 (192 microsteps), 1/8 = 7 (96), 1/16 = 8 (48), 1/32 = 9 (24) of `stutter_microsteps`. ECHO: the send opens at once; one beat later (`dj_beat_ms`: 6,000,000 / tempo x100 ms of the published tempo, timed from the first pass that sees ECHO in force, so the error is one UI pass) the group's VOLUME goes to -100, which also stops the group feeding the delay: the repeats already in the line ring out. The release gives both back. A release inside the first beat never cuts. CUT: VOLUME -100 while down. At ECHO's press with the master DELAY level at 0 the strip says `DELAY LEVEL IS 0` (the send still opens; a muted delay is not told).
- **LATCH, RESET, leaving.** As the kills: with LATCH (F6) on a tap toggles the pad and it stays; a REP latched replaces the other latched REP of its group (ECHO and CUT only toggle); unlatching, RESET (F8, which keeps LATCH) and leaving the page end every pad, held or latched. Two REP pads down in one group: the later press decides, the earlier is back when the later is let go (`rep_pick`). The pad's release is the same raw event as the kills' (`App::run_once`, `DjState::pad_held`), so a pad let go after the page changed does nothing twice.
- **Runtime only, exports clean.** Nothing is stored (not in `PerfState`, the SCEN chunk, the project or settings). An export already puts the strips' perf gain and sends away (`Graph::perf_save`, tests `audio_perform_test` P2 and P3, the same fields) and plays from a sequencer of its own, which holds no stutter; the transport has to be stopped for it. So no engine test changed; the byte-compare stays the P2 / P3 one (QA-PRF-128).
- **Deviations from section 2.4:** (1) **Shift + CUT latches** is **LATCH (F6)**, as for the kills: a held Shift turns the pads into the TRACKS layer. (2) The `A  REP 1/16  ECHO  CUT` line sits on pane row 6 and appears only while a pad is in force, so no existing golden moved. (3) ECHO's "one beat" is the tempo's nominal beat from the first pass that sees the pad, not the sequencer's beat grid (it is a timer, as B2's TRANS will be a timer against the play position).
- **Cost:** 0 DSP. FLASH: see the commit message. The layer's lookup adds three compares to `desired()`, which runs on the main loop's Perform pass only.
- **Hardware:** by ear on the unit, the four REP rates on a group, the ECHO beat and the tail, the CUT with the tail of the previous notes, the slot's DELAY or VOLUME coming back after a pad effect, LATCH and leaving the page with a pad held; the CPU is the sequencer's stutter (nothing new in the render).

### B2 as built (2026-10-19, branch feat/OS-DJ): TRANS HP, LP and ECHO

- **Where:** `app/include/o3os/app/dj.h` (`DjTrans`, `DjTransition` in `DjState::trans`, `dj_loop_len`, `dj_loop_pos`, `dj_trans_loops`, `dj_trans_start`, `dj_trans_publish`, `dj_loop_steps`), `app/src/dj_actions.cpp` (the three pads, `dj_pad` arg 28..30; the timer; `dj_publish` takes the sweep for the filter), `App::poll_dj` (`app.cpp`, the timer before `dj_publish`), the NEXT line and the pads' LEDs (`perform_screen.cpp`), `next_song_row` (the NEXT PATTERN code, shared: `perform_actions.cpp`), `PerformBank::told_length` (one inline getter). Tests: 10 new cases in `tests/dj_test.cpp` (the timer runs over a real `Sequencer`); step `qa_prf_129` in `qa/qa_perform_dj`; rows QA-PRF-129..132; step 12 in the manual's "Use the DJ page".
- **No new DSP, no new command.** The sweep is `master_param(DjFilter)` (A2), TRANS ECHO is the DELAY send of B1's layer on every track, and the switch is the sequencer's own queue (`queue_pattern`, or `queue_jump` to the Song order's next slot, as NEXT PAT). 0 DSP; the timer is a main-loop pass.
- **Timing is the play position, not a clock.** The playhead is `step * 48 + ustep` in the loop of the playing pattern (length: the PAT LEN FX if told, else the project's); progress is elapsed over total in those microsteps, so a tempo nudge or a Tempo FX stretches the sweep with the music. A wrap of the position (or another pattern playing) is the switch: the filter snaps back (a one-block ramp), the line goes. A sweep updates the engine when its per cent changes, each with a 20 ms glide.
- **The one-bar minimum is whole loops, from the tap.** The switch is the end of the first loop that ends at least 16 steps after the tap; `dj_trans_loops` counts the loops to hold back (0 with a bar or more left, one with 8 steps left of 32, two with 2 steps left of 8). The sweep starts at the tap and spans all of it (rest + loops). The queue is held back by taking the pick off the sequencer (`queue_pattern(0)`, or `queue_jump(0xFFFF)`) and telling it again at the last loop (`DjTransition::owes`); a pick or a jump the sink refuses is told again next pass; cancelling, RESET and leaving give it back at once. In Song mode the song would move on by itself, so the held-back loops are a live jump to the slot that plays (`hold`), told again at each wrap.
- **Choices of the build:**
  - The queue is row 1's pick; with none (and no live jump waiting) the Song order's next slot is queued at the tap (`next_song_row`: the strip says `SONG IS EMPTY` or `PATTERN IS NOT IN THE SONG` and nothing starts). Stopped: `TRANS NEEDS PLAY`; during a count-in `TRANS NEEDS THE COUNT-IN OVER` (the pre-roll's position repeats the first step, each step would look like a wrap). A cancel or RESET gives back the pick and the live jump both (the jump wins at the end, as before the tap) and, in Song mode, takes the hold jump away; a send the sequencer's ring refuses half-way leaves the pick owed and the pad does nothing.
  - Any TRANS pad while one runs cancels it (the filter springs back in 120 ms, the queue stays); a different TRANS pad does not replace the running one. A second pick on row 1 during a transition replaces the first; withdrawing the pick (row 1's second tap) ends the transition.
  - TRANS ECHO opens in the last beat (192 microsteps) and tapped inside it opens at once; it never waits for a bar. It uses only the send (+100, B1's layer) on all 12 tracks of the page, THRU included (tracks 13-16 are not on the page, Q6): the dry level is not cut, since the new pattern enters clean at the switch; the repeats already in the delay ring out. `DELAY LEVEL IS 0` as for ECHO.
  - The sweep wins over the hand's `filt` while it is not 0 (the first per cent); F4 and the FILT pad still move `filt`, which shows when the sweep ends. LATCH keeps a hand-set filter, not a sweep.
  - HP sweeps 0 to +100, LP 0 to -100 of the A2 knob (a law that is linear in log frequency).
  - The error is one UI pass at the start and at the switch; the filter may sit closed for that pass on the new pattern's first step (section 5).
- **Runtime only, exports clean.** `DjTransition` lives in `ViewState`; nothing is saved, and an export puts the strips' sends and the filter away already (A2, B1). Leaving the page, RESET and a stop end a transition; a held-back pick is given back so that "a queued pattern stays queued" holds.
- **Deviations from section 2.5:** a pattern shorter than a bar waits as many loops as the bar needs (the spec says one more); TRANS ECHO has no bar minimum (the spec gives none).
- **Hardware:** by ear, the sweep landing on the downbeat of the new pattern, the snap (no lingering filter), the echo's last beat, the extra loop when tapped late, cancelling; the sweep's per-cent steps on the filter (20 ms glides) for zipper noise.


### C1 as built (2026-10-20, branch feat/OS-DJ): scratch, slip, backspin and TAPE STOP - the last DJ slice

- **Where:** `audio/include/o3os/audio/master_scratch.h` + `audio/src/master_scratch.cpp` (the stage, -O3 from flash), `Graph::scratch_stage_` in the `Headroom` slot (installed by `init_defaults`, bypassed), `graph_param::master::ScratchMode` (0x172: 0 end, 1 SCR, 2 TAPE STOP with the ParamChange's ramp as the beat in samples), `Graph::scratch()`, `perf_clear` aborts it; app side `DjScr` / `DjState::scr` / `scr_end` (`dj.h`), `dj_scratch_start`, `dj_scratch_poll`, F5 and the TAPE STOP pad (`dj_actions.cpp`), `App::attach_scratch`, `Recorder::arena_busy`; platform `input::device::encoder_count()` (a read of FTM2 `CNT`) given to the stage in `app_main.cpp`. Tests `tests/audio_scratch_test.cpp` (8 cases, also under QEMU in the `audio_limiter` suite), `tests/dj_test.cpp` (3 cases `dj scratch (C1)`), `tests/audio_perform_test.cpp` (`scratch export`); step `qa_prf_133`; rows QA-PRF-133..138; step 13 of the manual's "Use the DJ page".
- **Where the stage sits (the spec is silent).** First in the chain (the empty `Headroom` slot), so the ring holds the mix before the kill EQ, the DJ filter and the limiter: a kill or the filter while scratching acts on the scratched sound as on a deck, and the limiter (D4, A3c) catches whatever the head makes. The chain table, the MAIN meters and the other stages are untouched.
- **Maths.** The head is a LAG behind the write head. The rate (frames per frame, 1 real time) ramps linearly inside a block; lag += 1 - rate per sample; lag is clamped to [0, written - 2] and to the ring (the head never passes the write head, never reads an unwritten frame); the Platter reads with the 4-point Hermite (OS-SCRATCH; C1 and the TAPE STOP read linearly). Platter: the hand is the signed 16-bit difference of the FTM2 counter, one count is 827.2 frames (96 counts = 1.8 s), clamped to 16 counts a block, its velocity is measured from the time between counts and followed by a one-pole of 25 ms (OS-SCRATCH, 0.0564 a block; C1 had 0.28 a block, about 3 blocks), and a rate under 0.001 with a target of 0 is 0. The output is the read times a gate min(8 |rate|, 1), so a still or reversing head is exactly silent without a DC step. The entry starts at rate 1 (the live mix) and runs down to the hand through the 25 ms one-pole (exact silence after about 170 ms from rate 1); the release is a 256-sample crossfade to the live mix (5.8 ms), then the stage bypasses itself. Backspin: a platter rate at or below -4 (and the hand's own target at or below -4, OS-SCRATCH: the inertia keeps the rate up for some blocks after a hand that stopped) within the last 20 blocks (29 ms) at SCR off keeps that rate and brakes it to 0 in a straight line over 0.5 s (22,059 samples), then the release. TAPE STOP: rate 1 to 0 in a straight line over the beat (tempo at the press), the head held silent, half a beat behind at the end; the release is the same crossfade.
- **Arena (Q7a).** `dj_scratch_start` asks `Recorder::arena_busy()` (a take armed, recording or kept, a loop playing), the bank's own `take_held()` and a live take, then `SampleBank::take_begin`; the ring is `min(room / 2, 176,400)` frames of interleaved 16-bit stereo (705,600 B), at least 4,410. Refusals: `NO ROOM: LOOPER IS RECORDING`, `NO ROOM FOR THE SCRATCH`, `SCRATCH BUSY: TRY AGAIN` (the ring refused the push); the pad with SCR on says `SCRATCH IS ON`. The tail is held whole (the arena is full for every other taker) and given back (`take_end(0)`) by the main loop only after `busy()` is false: `stage()` and the render's `done_` are an epoch pair, so a ParamChange still in the ring can never see a ring that was given back.
- **Q8a.** The stage calls the platform pointer once a block (in `set_mode` too, to start from the counter now); the host and the tests script it. The mk66 pointer is one register read with no guard and no state.
- **Q9a.** F5 toggles. TAPE STOP is a held pad (row 3, pad 8); with SCR on it refuses. RESET, leaving the page and the export end it (the export aborts the stage at once; the page finds `busy()` false and gives the tail back).
- **Deviations / choices.** (1) The stage is in the `Headroom` slot, not a new table row (no change to the table, the tests of its order or `main_pre_`). (2) The simulator has no encoder counter and, in the QA scenario, no card: F5 says `NO ENGINE FOR THE SCRATCH`; the platter is proven by the stage tests with a scripted counter. (3) The encoder is inert for the crossfader while SCR is on. (4) The REC screen's own NoRoom (a take armed while SCR holds the tail) stays the generic one: it needs the page left first, which ends the scratch. (5) The 4 s are a ceiling; a small arena gives a shorter ring and the head stops at the oldest frame kept.
- **C1 review fold (2026-10-20).** (1) The block is written whole before it is read, so the lag stops `kBlock + 2` frames short of a full ring (`cap - 66`; the 64 oldest frames are already the block's own): 0ad289b's `cap - 2` let a resume read up to 63 live frames ahead and 2 stale ones a block (a buzz near 689 Hz). (2) The ring is the arena's tail, so `SampleBank::clear()` (project load, import, new project) must not run under it: the File actions OPEN, IMPORT and NEW refuse with `END THE SCRATCH FIRST` while `dj.scr != Off` or `scratch.busy()` (the backspin's 0.5 s included); `dj_scratch_start` refuses with `NO ROOM: SCRATCH IS ENDING` while busy; `Recorder::arm` answers Busy and `Recorder::bank_clear_busy` (the bank's clear check, one pair of hooks) is true while the stage is busy, so the two owners of the tail are covered; the console `diag load` path is covered by the same test (`DjState::scratch_holds_arena`, asked by the platform's three starters `start_project_load`, `start_new_project` and `start_module_import`: `load: END THE SCRATCH FIRST`). (3) The ring is stored at half scale (+-2.0 kept whole; the MAIN mix is float to the limiter and a hot sum was clipped at +-1.0): 15 bits of resolution, about -90 dB. (4) The lag is an integer number of frames plus a float fraction (`MasterScratch::advance_lag`): a float of 176k frames resolves 1/64 and swallowed the increments of a rate within 0.8 % of 1.
- **Cost [est].** Armed (still): the 64-frame copy with the float-to-Q15 conversion, about 12 cycles a sample, 0.3 % at 44.1 kHz. Scratching: plus the interpolated read, about 25 cycles a frame, 1 %. Bypassed: 0. Not measured (no unit run).
- **Hardware:** R1 (is the render-side FTM2 read stable), R2 (the feel of 24 detents with the smoothing), CPU armed and scratching (cpu-budget section 5), the by-ear check of the entry, the release, the backspin and the TAPE STOP, a loop recording first (refusal) and the tail after.
