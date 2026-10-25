# OS-LFOLIVE and OS-BUG-WTLFO: the LFOs run while stopped, and their fields are live

Status: implemented on `feat/OS-LFOLIVE`, host-tested, awaiting the RT review and the unit CPU measurement of section 5 · Level C (audio path, the descriptor protocol) · Two IDs, two commits, one spec · Follows OS-INSTUI S6 (the MOD page) · First slice of the live-parameter path of OS-MODMX (`docs/specs/OS-MODMX.md`)

Classification: OS-BUG-WTLFO (a) **new Community OS behaviour** that closes a gap of this OS (what the official firmware does while its transport is stopped is Q; no parity is claimed). OS-LFOLIVE: **new Community OS behaviour** (a voice has a changed LFO field within a block). Layer 1 of OS-MODMX M1 (the fixed modulators, oracle-tested) is unchanged: with no edit and a running transport every output is bit for bit what it was.

## 1. OS-BUG-WTLFO

Measured on the unit (v0.5.0, a Wavetable instrument, WT SIZE 2048, 8 windows, Modern engine; backlog row): (a) an LFO on WT Position does not move with the transport stopped; running, it sweeps 0 -> 7 cleanly (12 steps at 127 BPM: 1.4 s). The tester added: granular Grain Position, LFO and transport stopped, is frozen at position 0, the same defect. (b) in several takes the stem plays something that is none of the 8 windows, 30 dB down.

### (a) The LFOs run while the transport is stopped

All six modulators of an instrument share one generator whose phase is the transport's tick count (`voices.h` FILTER MODULATION): the tremolo (amp LFO), the pan, the finetune, the cutoff, WT position and GRAIN position. With the transport stopped the count was held at the end of its step, so a pad or a MIDI key played then sounded a modulation frozen at one phase (for a saw that is the start of its sweep: window 0, position 0). All six froze; one fix covers all six.

Evidence and class. This OS held these LFOs while the transport was stopped (`docs/specs/OS-005-audio-engine.md`, OS-007m, and `voices.h`). There is no unit capture and no recovered phase or rate, so the new behaviour is **new Community OS behaviour** that closes a gap of this OS; it is not claimed as parity. **Open research question (Q)**: whether the official firmware's LFOs run while its transport is stopped, and if so at what rate and from which phase.

Rule (`voices.h` LFOS WHILE STOPPED):
- From the `Stopped` event until the next `Step` event the count goes on at the tempo in force, carrying into the next step, instead of being held at the step's last tick. The sequencer reports the project's tempo while stopped, so the LFO is at the project tempo.
- A stop takes the count on from where it was (no jump).
- The transport START (`FxReset`) restarts the count at 0 as before; a voice that sounds when the transport had been stopped KEEPS ITS PHASE (its shift is made its phase at that moment; a random level already drawn is held). A restart while the transport ran, and everything while it runs (the count held between Step events), is unchanged.
- The count is a u32 of ticks (576 a step): an idle stopped transport wraps it after about 10 days at 120 BPM (one phase jump of an LFO that sounds then). Same width as before; noted, not changed.

Cost: nothing per voice. One compare in `render()` and one in `flfo_level` (`running_`), and, stopped only, one 64-bit subtract per block. The start restart walks 12 voices x 5 generators once.

### (b) The sound that is none of the 8 windows

**Root cause: not found on the host. Do not close (b) from this commit.** What was done:
- A seeded property test over all play modes (`tests/audio_bounds_test.cpp`: one shot, the three loops, SLICE, BEAT SLICE, WAVETABLE, GRANULAR; random sizes, parameters, ADSR and LFO modulators on every slot, live window / position / loop edits, retriggers, a clock that stops and restarts) with a guard-pattern oracle (the window sits in a buffer whose surround is filled with a pattern; a run with two patterns must hash alike; under ASan the exact-size buffer catches the same read). 30,000 scenarios ran once at development (about 15 s under ASan + UBSan) with zero reads outside a window, in the code before and after this change; the committed run is 240. The oracle is sensitive: a one-window overshoot of the clamp in `move_position` is caught at once.
- Reading the path (`move_position`, `apply_position`, `set_sample`, `make_sample_desc`): the window index is held to `0..windows-1`, `wt_pos` and `data` move together, `loop_end` is the window; no pointer can leave the stack.
- So the sound is not a window pointer outside the stack in the voices. Candidates the host cannot exclude, to check with the unit capture: a voice that plays on a descriptor that is stale until the handover (before OS-LFOLIVE a SOURCE switch to LFO reached the engine only at the quiet handover, which waits for a stopped transport and no held note: a take made in between plays the old modulator); an instrument bound with no PCM at that moment (a silent descriptor; the 30 dB down figure may then be another source on the stem). OS-LFOLIVE removes the first (SOURCE and the LFO fields are live now). The tester's next capture should be read with `diag inst` (`win W/N`, the descriptor's window) and `diag voices` at the moment of a bad take.

## 2. OS-LFOLIVE: the LFO fields are live

Today (v0.5.0) the LFO's shape, speed, amount and the loop byte of SOURCE are read only when the descriptor is rebuilt at the quiet handover (stopped, no held note); OFF / ENV toggles were already live (`EnvEnable`). Wanted: an edit is heard on sounding voices.

### Design (the first slice of the OS-MODMX live-parameter path)

- **Ids.** `audio::inst_param::lfo(slot, field)`, equal to the model's `instrument_param::mod(slot, field)` (0x60..; `tests/audio_model_ids_test.cpp` holds it): SOURCE (0 Off, 1 Envelope, 2 LFO), SHAPE, SPEED, AMOUNT for the six slots (0 tremolo, 1 pan, 2 cutoff, 3 WT position, 4 GRAIN position, 5 finetune).
- **Cheap per-field update, no `derive()`.** `apply_instrument_param` writes the descriptor field (the one function `make_sample_desc` is held equal to; a random equality test covers every slot and source). `VoicePool::lfo_live` then does, per voice that plays the slot: SHAPE and AMOUNT nothing (every evaluation reads them from the descriptor: the cutoff within 16 samples, the others at the next block); SPEED the voice's period and its reciprocal again (one division; the phase is the transport's, so the LFO is where the new period puts it, no continuity across a rate change); SOURCE starts or ends the modulator as an ENABLE edit does (`refilter`, `remod`). The cost is once per ParamChange, bounded by 12 voices, never per block.
- **No work when nothing changed.** No edit, no ParamChange, nothing runs; an edit to the value it has costs one descriptor write and the voice loop, once.
- **Single-writer protocol (2 lines).** The render context is the only writer of a descriptor while a voice sounds: it applies the ParamChange ring in the graph's pass (at most 64 a block, after the block's voices; nothing the render waits on, no allocation, no lock). The main loop writes a descriptor only at the sample bank's handover, when no voice sounds and the ring is empty; the rebuild from the project equals the live edits, so a dropped ring entry is healed there.
- **App side.** `send_param_change` sends the same id for a MOD row (plus the rebind as before). A SOURCE edit also sends what the new generator reads (the descriptor has one amount for the slot's ADSR and LFO): the LFO's shape, speed and amount, or, for Off and Envelope, the ADSR's amount (the amp slot keeps its own). `make_sample_desc` now stores the LFO speed whatever the source is (the amp slot did), so a live SOURCE edit finds it.
- **Next slices (OS-MODMX).** Route tables (MX2) use the same ring and the same writer rule; the per-route value is a field write plus a bounded voice loop, not a `derive()` (cpu-budget M5); module parameters (M7) go to the instance with `set_param` only when the value moved.

### Not done here

The amp ADSR flipping live (existing behaviour: the voice's envelope pointer is taken at the note); Amiga voices and SD modules (they read the descriptor at the note).

Phase (RT review fold): a SPEED edit keeps the phase u = (x mod old period) / old period of every sounding LFO (`lfo_live` remakes `l_shift` for the new period, once per edit and voice, so no zipper at an encoder step; a random level already drawn is held). The first Step after a stop with no start before it (Continue, a resume) gives the sounding voices the phase they have (`resume_phase`, via `on_step`), as the transport start does (`clock_restart_impl`). In FxReset `restart_clock` now runs before `clear_filter_fxs`, so an LFO that the FX end starts afresh (`mod_start`) is not given the stopped count a second time. Tests: `audio_lfolive_test` (SPEED continuity), `audio_wtlfo_test` (Step after a stop, tempo change while stopped).

Side effect of the FxReset reorder: with the transport already running (a Play while playing), an LFO that the FX end starts afresh (`mod_start`) now gets shift 0, because `restart_clock` has set `cursor_step_` to 0 first; before it got `(cursor % 36) x 576` from the old run's cursor. The count restarts at 0 in the same event, so the new value is the one a note at step 0 of the new run has. New Community OS behaviour, not a parity claim; a unit check is not needed (the phase of a fresh LFO at a restart is arbitrary to the ear).

Latent, not fixed here: `lfo_set_impl` (`voices.cpp`, the Filter LFO FX changing the rate of an LFO that already runs) sets `l_period` and `l_inv` without making `l_shift` again, so a rate change by the FX keeps the phase jump that a live SPEED edit no longer has (`lfo_live`). The same helper would fix it; left out because it changes sound already on the unit (FX 35) and is not the live-edit path.

Tests added for the fold: `audio_lfolive_test` (SPEED up and down, `(x % op) x np / op` truncating, new period in force on pan and cutoff, random level held), `audio_wtlfo_test` (random level held across a Step after a stop).

## 3. Done when

| Item | Evidence |
|---|---|
| All six LFO destinations move with the transport stopped, at the tempo in force; WT window and GRAIN position included | `tests/audio_wtlfo_test.cpp` (fails with the held clock: checked by mutation) |
| The phase is kept when the transport starts; running behaviour unchanged | `audio_wtlfo_test.cpp`; the existing oracles `audio_filter_mod_test`, `audio_mod_test`, `audio_wavetable_test` unchanged |
| No sample read leaves a window, any play mode, seeded random parameters, modulation, live edits | `tests/audio_bounds_test.cpp` (unit_audio group; 240 scenarios, well under a second) |
| A live edit of each LFO field (shape, speed, amount, source) equals the note on with the new value from the next block, bit for bit; no change, no difference; ranges | `tests/audio_lfolive_test.cpp` |
| The live descriptor equals the rebuild (every slot, every source) and the page's edits are ParamChanges | `tests/inst_live_app_test.cpp` (random equality, MOD page) |
| (b) root cause | **open**: see 1(b) |

## 4. CPU

Host bench (`o3os_audio_bench`, `voice_lfo_table`, Release flags, 12 voices, relative; the host is not the M4F), before this change and after, three runs each on a loaded machine (about 15 other jobs):

| case | before ns / block | after ns / block |
|---|---|---|
| 12 voices, no modulator | 29.7 k | 29.7 k |
| 12 voices x 5 LFOs, transport running | 34.5 k | 34.6 .. 36.4 k (noise: +0 .. +5 %) |
| the same, stopped | 33.4 k (held) | 34.3 .. 36.2 k (the clock runs on) |
| running, a SPEED edit every block | (not available) | 34.3 .. 37.8 k |

The modulators themselves cost about 4.8 k ns a block for 60 generators on the host (80 ns each), which was the case before. The change adds a branch per evaluation and, stopped, a subtract per block: not measurable. M4F estimate [est]: an LFO evaluation about 120 cycles (OS-007m); the added compare 2 cycles; a SPEED edit about 12 voices x 40 + 80 (division) = 0.6 k cycles once (0.2 % of a block, once). Budget row: none added (the generators were budgeted in the OS-MODMX row, section 5 of that spec).

Placement (`rt-optimize`): the hot path gains only the two compares (`render`, `flfo_level`, both already `O3OS_AUDIO_HOT`); `clock_restart_impl`, `apply_lfo_field` and `lfo_live` are flash code (`.text`), reached through a pointer (`clock_restart_`) or from the flash `apply_param`; the SRAM_L slab is not touched. `VoicePool` grows by a pointer and a bool.

## 5. Unit measurement (the tester runs it; `diag cpu`)

Build id the same, the feature off then on (`docs/architecture/cpu-budget.md` section 5). Scene: the reference project t-v16 plus the TB-303 line, 60 s, with the sampler instruments in use given LFO sources:

1. Off: every MOD destination SOURCE Off or Envelope as in the project. `diag cpu reset`, `diag cpu perf off`, play 60 s, `diag cpu` (render max, mean, underruns), `diag bench worst`.
2. On: on each sampler instrument in use set SOURCE LFO (Saw, 4 steps, 100 %) on the amp, panning, finetune, and, with the filter on, the cutoff; on a wavetable instrument WT position and on a granular one GRAIN position (the six). Same commands. The difference is the six generators of at most 12 voices: expected under 1 % [est].
3. Stopped transport: hold 8 pads on those instruments for 8 s: `diag cpu` max (the clock runs on).
4. Live edit: during step 2 turn LFO SPEED / SHAPE on the MOD page continuously for 10 s: no underrun; the sound follows the encoder within a block, with no click that the same edit made with the pad up does not have.
5. Ear: `qa_lfolive.json` (`platform/mk66/tools/hil/`): the six destinations with the transport stopped; Play with a pad held (no jump); the four fields live.

Report the render max / mean before and after, the underruns, and the FLASH / SRAM_U / SRAM_L deltas from the map.

## 6. QA rows

`docs/qa/catalogue/instrument.md` QA-INS-087 .. 096; HIL steps `platform/mk66/tools/hil/qa_lfolive.json`.
