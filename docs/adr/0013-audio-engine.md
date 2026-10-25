# ADR 0013 — Audio engine, sequencer and SDRAM memory map

Status: **Accepted** (maintainer-delegated, 2026-10-11). See "Acceptance" at the end.

**Inputs:** ADR 0002 (module ABI, SRAM_L slab), 0003 (12 typed tracks over a voice pool), 0004 (step FX), 0008 (every OG feature stays), 0009 rev 1 (descriptors, generation counters, ParamChange SPSC at block boundaries, SPSC both ways, no observers, jobs), 0010 am. 2 (`ui::plot` taps), 0011 (alignment declared, `diag cpu/mem`, crash record), 0012 + amendments (StemTap mono pre-pan int16, USB ISR at 1 ms, retune to 44,100 Hz gated by `hw-session`), triage §5/§6, radar cards `amiga-playback-mode`, `filter-set`, `tb303-engine`, `rt-optimize`, the hazard list H1–H16 (below).
Tags: **[fact]** read in the repo (file cited) · **[est]** arithmetic or judgement, a bench item · **[inf]** inference. Cycle model: 180 MHz; 1 cycle/sample = 0.0245 % CPU; a 64-frame block ≈ 261 k cycles (1.45 ms), a 128-frame block ≈ 522 k (2.9 ms) — the same to 0.04 % at 44,100 or 44,117.65 Hz [est from `rt-optimize`]. Maintainer rules honoured throughout: no heap after boot, no RTTI/exceptions/`double`, nothing virtual in sample loops, no PC audio engine.

## Decisions at a glance
1. Tiny I²S DMA ISR pends a **deferred render** at mid priority; **64-frame blocks**, 3-slot TX ring (2 before Amendment 6; one constant); worst case ≤ 60 % CPU at 12 tracks with a load governor.
2. **Mixed format:** Q15 + SIMD on memory- and sum-bound stages, float32 inside voices, modules, reverb and master; one conversion per block per boundary.
3. **16-voice pool:** one guaranteed voice per track + 4 shared tail voices; sampler family (8 OG modes incl. slices, wavetable, granular) and the Amiga/Paula voice are **core**; 303/FM/chip/drums are SD modules rendered **per voice** under a DWT budget.
4. **Fixed graph:** voices → track (2 insert slots) → level → StemTap → pan → MAIN/CUE/SNDA/SNDB/REC/USB; master chain `saturation → limiter → EQ → bass → space → bit depth` as a table, **marked for contract/hw check**; all taps are block copies into rings. *(Amendment 5, 2026-10-19: the EQ and bass slots now run ahead of the limiter; see "Amendment 5".)*
5. **Sequencer runs block-synchronously inside the render** on a 64-bit sample timeline, 192 PPQN grid, sample-exact event offsets; MIDI clock out through a timer-compare emitter; MIDI in timestamped in the UART/USB ISR.
6. Parameters: ParamChange ring drained after the block's voices (in the graph's render: a note in the same block as a knob edit starts on the old value, for at most one block, and that order is intended), per-parameter ramps from one smoothing table; mod matrix evaluated per voice at block rate (690 Hz); MIDI Learn via actions.
7. **SDRAM map** (§D7): one 11.19 MiB sample arena (samples + recordings + edit copies, no overlap, D-OS-10), 192 KiB slack, every region a `constexpr` in one header.
8. Host reference tests for every pure part, QEMU for SIMD/loader paths, four `hw-session` gates HW-A…D (§D8).
9. Slices OS-005a…i (§D9).

## Context (facts)
- Engine of this OS (new Community OS behaviour, this ADR's design): 128-frame render blocks at 44,100 Hz since v1.0.0 (44,117.65 Hz before; the codec's master clock, 11.2896 MHz = 256 × the rate, D-OS-8), I²S DMA halves of 64 stereo frames (1.45 ms). These are this OS's choices, not claims about the official firmware: its block size, voice count and audio clock are open research questions (unverified), as is how it schedules its own work (interrupt priorities, control-tick rates). The unit has 8 tracks up to firmware 1.9 and 12 from 1.9.0 [fact: the project and pattern files, `docs/formats/og.md`]; an instrument has 6 envelope slots (amp + pan, cutoff, wavetable pos, granular pos, finetune) [fact: the instrument file's six envelope slots].
- Sound memory of the unit: the sample bank is 11.25 MiB; PAR-134 (1.8.0): record ≈ 133 s = all of it (documented in the release notes, not verified). D-OS-10: no overlap in this OS (`product-ux.md` §7) [fact]. The official firmware's own memory map is an open research question (unverified) and is not recorded here.
- Sequencer grid: 48 microsteps per step (= 192 PPQN), step = 15/BPM s, swing as alternating step durations (swing+50)/100 and (150−swing)/100, 40–800 BPM, MIDI clock every 8th microstep (this OS's design, not a claim about the official firmware: its grid and clock output are open research questions). Whether the official firmware's external clock is quantised to a poll is an open research question (unverified).
- LINE and RADIO are exclusive input sources [fact: one source setting in the project file, `docs/formats/og.md` group 4]; that the radio's audio enters the SGTL5000 line-in is an inference (open research question, unverified); whether MIC and LINE are both wired is a hw check (`docs/hw/tracker-hardware-notes.md` §8).
- Hazards to design out: H1/H2/H4/H12 (sequencer ISR mutating voices, non-nesting lock, pattern bank races, phase reset), H3/H6/H7/H8 (MIDI/SD/UI from ISRs, spinning sends), H5 (preload handshake), H9/H10 (ad-hoc flags), H11 (PIT exhaustion), H13, H14 (stack), H16 (pre-main I²S start) [this design's own list, from the design review of this tree].

## D1 — Execution model, priorities, block size, CPU budget
| Option | Pros | Cons |
|---|---|---|
| A. Render inside the I²S DMA ISR | lowest latency; one context | a 0.9 ms render at DMA priority starves USB iso (1 ms cadence), MIDI RX and SD DMA; no preemption point |
| **B. Tiny DMA ISR pends a software IRQ; render there at mid priority** | DMA ISR < 1 µs and jitter-free timestamps; USB/MIDI/SD preempt the render; one deadline per block measured by DWT | render start jitter (bounded by the ISRs above it); needs an underrun policy |
| C. Render from the main loop | no ISR | UI SPI and SD block for ms; rejected |
| D. RTOS thread | preemptible | no RTOS, no heap; rejected |

**Decision: B.** Priority table (NVIC, lower = higher; one `hal::irq::prio` table, H11 fixed by a channel-ownership table):
| Prio | Source | Work |
|---|---|---|
| 16 | I²S TX/RX DMA half/complete | advance slot index, stamp `cycles()`, `pend_render()`; no data touched |
| 32 | SysTick 1 kHz | time base |
| 48 | UART4 RX (MIDI DIN), USB0 (64) | bytes timestamped into `midiRt` (→ render) and `midiUi` (→ main); USB per ADR 0012 (7.5 µs worst [est]) |
| 80 | SDHC DMA, SPI2 DMA (FT812), I²C | completion flags only |
| 96 | MIDI-out timer compare (one PIT) | emits due bytes to the UART FIFO (D5) |
| **128** | **render** (reserved NVIC line pended by software, as the Teensy core does; PendSV is the fallback) | sequencer tick + whole graph, ≤ 60 % of the block |
| 160 | pad/button/encoder pin ISRs | flags → main |
| main | UI, SD, jobs | never audio |

**Block size: 64 frames** (`kBlock`, one constant; 128 remains legal). Per-block fixed work (ramps, envelopes/LFOs, coefficient updates, event dispatch) ≈ 16 k cycles at 16 voices → 6 % at 64 frames vs 3 % at 128 [est]; in exchange the control rate doubles (690 Hz vs 345 Hz) and latency halves: line-in → out ≈ RX fill 1.45 + render ≤ 1.45 + TX 1.45 = **4.4 ms** vs ≈ 8.7 ms at 128 frames [est]; pad → sound ≈ 3–4.5 ms engine side [est]. TX ring `kTxSlots = 2` (deadline one block); `3` adds 1.45 ms latency and doubles the deadline (3 since Amendment 6). **Underrun policy:** the DMA replays the stale slot, counts it (`diag cpu`), never waits; 3 consecutive underruns mute and notify (ADR 0011). I²S starts only after codec mute from `hal::audio::start()` (H16).

**CPU budget, worst case at 12 tracks [est, bench HW-B]:**
| Stage | % | Stage | % |
|---|---|---|---|
| 16 core voices, heaviest (granular + SVF + drive + ramps, 88 cycles/sample) | 35 | delay (block-wise SDRAM, stereo, 2 taps) | 1.5 |
| track insert modules (DWT-capped budget) | 6 | reverb (block-wise FDN, 8–16 lines) | 3 |
| track sum/pan/sends/StemTap/meters (Q15 SIMD) | 2 | master chain (6 stages, float) | 4 |
| sequencer + control rate at 690 Hz | 6 | USB stems + return (ADR 0012) | 1.3 |
| record/resample/scope/spectrum block copies | 0.3 | **total** | **≈ 59** |
**Load governor:** rolling max per block in `diag cpu`; above 75 % the allocator frees tail voices first, modules over 150 % of their declared cycles on 2 consecutive blocks are bypassed with a notification (ADR 0002). Amiga mode replaces sampler voices (+0.8–2.5 % each, radar card [est]) under the same governor.

## D2 — Sample format
| Option | Pros | Cons |
|---|---|---|
| A. float32 everywhere | one numeric world; modules are float (ADR 0002) | memory-bound stages 2–3× slower than Q15 SIMD; 2× bus bandwidth; pool and USB are int16 anyway |
| B. Q15 everywhere | `__SMLAD` 2 MACs/cycle; int16 pool | resonant filters, 303, reverb need scaling tricks; modules convert anyway |
| C. Q31 | headroom | no dual-MAC; 32-bit lines double SDRAM traffic |
| **D. Mixed per stage** | each stage in its best format; converts once per block | two worlds to test; conversion points must be explicit (they are, below) |

**Decision: D.** Conversion points are fixed in the graph; nothing converts per sample.
| Stage | Format | Why | Cycles/sample [est] |
|---|---|---|---|
| sample fetch + interpolate | Q15 planar; per-voice **span prefetch** (LDM burst of the ≤ 64·ratio samples the block needs into a 1 KiB SRAM window, then `__SMLAD` 2-tap; 4-tap Hermite optional) | SDRAM random reads ≈ 10–15 cycles [est]; sequential bursts ≈ 2–3 | 4 + 3 |
| voice filter, drive, oscillators, envelopes, modules | float32 | feedback precision; ADR 0002 float-only | SVF 38, biquad 24, drive 12 |
| voice → track | `VCVT` + `__SSAT` once per block | — | 3 |
| track sum, pan, sends, StemTap, meters | Q15 in, Q31 accumulate (`__SMLALD`), saturate once at bus output | 12 sources; `__SSUB16`/`__SEL` for peaks | 2–3 per source |
| delay / reverb lines | int16 in SDRAM, float feedback and filter math | bandwidth vs precision | — |
| master chain | float32 | limiter, saturation, EQ precision | — |
| I²S out, USB, record, export | Q15 | hardware and UAC2 16-bit (ADR 0012) | 1 |
SIMD pays on interpolation, sums, pan, meters, interleave (`__PKHBT`), mipmaps; it does not on filters, oscillators or envelopes. Host builds get bit-exact C versions of the intrinsics so one kernel source runs on host, QEMU and target.

## D3 — Voice architecture
**Pool:** `kVoices = 16` = 12 track voices + 4 tail voices (ceiling 16 tracks after HW-B, ADR 0003). Per-voice cost [est, bench HW-B]: sampler 10–14 cycles/sample (0.3 %); slices/beat-slice = sampler + event setup; wavetable (2048 window, 2-table crossfade) ≈ 18 (0.45 %); granular (2 overlapping grains, window LUT) ≈ 30 (0.75 %); Amiga/Paula B ≈ 1.1–2.8 % (card); 303 module B ≈ 10 %, C ≈ 5.5 % (card). Per-voice chain: TPT SVF 0.93 %, drive 0.29 %, ramps + convert 0.2 % (`filter-set` card).
| Stealing option | Pros | Cons |
|---|---|---|
| A. one voice per track, hard cut | predictable | clicks; no release overlap |
| B. free pool, steal quietest globally | most polyphony | a note on one track can kill another track's note; CPU unbounded |
| **C. guaranteed voice per track + 4 shared tails, steal quietest-oldest tail** | tracker semantics kept; tails when spare; bounded CPU | tails limited to 4 (constant) |
| D. per-track polyphony N | chords per track | not a tracker; CPU |
**Decision: C**, "tail overlap" per instrument (default on, 2 ms crossfade on a hard steal — PAR-125/126 click-free behaviour [fact of behaviour, method unknown]).
| Core vs module | Pros | Cons |
|---|---|---|
| everything core | no loader on the critical path | flash (335 KiB free) fills; contradicts ADR 0002 |
| everything modules (incl. sampler) | smallest core | the device must play OG projects with no SD (ADR 0009 standalone-first); loader risk on the baseline |
| **core = OG sampler family + Amiga/Paula; modules = 303, FM, chip, drums, extra filters/FX** | baseline works standalone; Amiga is ≈ 5 KB and feeds OctaMED/MOD playback (maintainer priority) | Amiga flash cost is in the core |
**Module render contract** (ADR 0002 refined): instance **per voice** plus an instrument-shared state block; header declares `voice_bytes`, `shared_bytes`, `sdram_bytes`, `cycles_per_block`, parameters and step-FX (ADR 0004). `render(voice, const VoiceCtx&{sr, n ≤ kBlock, note, velocity, hz, flags{legato, accent, retrigger}, mod[16]}, float* out)` mono (stereo by flag); `note_on/off` carry the legato flag for 303 slide. Effects: `render(fx, const FxCtx&, float* L, float* R, n)` in place. DWT around every call; budget as in D1. Hot `render` in the SRAM_L slab (16 KiB), cold code/data in the SDRAM module region (D7).

## D4 — Graph, master chain order, taps, inputs
```
codec RX ── LINE L/R | MIC | RADIO (codec input select, exclusive [fact])      USB RETURN ring (ADR 0012)
   └── external sources: level + routing bits; the "External audio" track type (ADR 0003) ──┘
voice[16]: engine → filter → drive → amp/pan envelopes            (float, per voice)
track[12]: Σ own voices → insert 1 → insert 2 → level ─┬→ StemTap (mono int16, ADR 0012) → USB / SD stems
          (Amiga: Paula RC+LED filter is a core insert; LTI, so per-track ≡ Paula's post-sum bus) ├→ meters
                                                       pan → MAIN | CUE | SNDA | SNDB | REC | USB  (routing matrix)
SNDA → delay → return ┐  SNDB → reverb → return ┘ → MAIN (+CUE); sends pre-fader (our default), PAR-024 setting later
MAIN: Σ → headroom (110) → saturation (109) → limiter (058) → EQ (055) → bass (056) → space (057) → bit depth → OUT
CUE:  Σ → cue level → OUT MODE {stereo MAIN | MAIN L / CUE R} (product-ux §4)
taps: scope ring (master L/R, 30 Hz) · spectrum ring (512 mono frames, job rFFT, 20 Hz) · REC bus → sample arena · export (pull mode)
```
| Master order | Pros | Cons |
|---|---|---|
| A. limiter first (PAR-058 literal, 1.3.0) | sourced | PAR-109 (1.7.0) says saturation is first |
| **B. saturation → limiter → EQ → bass → space → bit depth** | consistent if 1.7.0 inserted saturation ahead of the 1.3.0 chain [inf]; the limiter still protects after drive | EQ/bass/space order unsourced; **superseded in part by Amendment 5 (2026-10-19): the EQ and bass slots now run ahead of the limiter** |
| C. user-orderable | flexible | not needed for compatibility; UI and format cost |
**Decision: B as `kMasterChain[]`**, reserved insert points with bypass until each behaviour is recorded (strategy B rows); **marked for a compatibility/hw check** (the order of the official firmware's master chain is an open research question, Q-3: a measurement on an official unit, if ever done). Every tap is a block copy into an SPSC ring or, for REC/resample, a sequential `STM` write straight into the sample arena (≈ 1 cycle/sample, no staging); `ui::plot` never sees audio buffers (ADR 0010). Offline render (PAR-054/069/139/151): the same graph in **pull mode** from a job with I²S stopped, faster than real time, tail detection on the master meter, sharp cut for selections. Monitoring during playback (PAR-048) is just a routing row.

## D5 — Sequencer
| Where it runs | Pros | Cons |
|---|---|---|
| A. timer ISR at top priority | fine phase | H1/H2/H4/H12: ISR mutates voices, phase resets, races |
| B. timer ISR posting events to the render | decoupled | two clocks to reconcile; extra jitter |
| **C. block-synchronous inside the render, sample-exact offsets** | one clock (sample counter); no races; deterministic offline render; host-testable as a pure function | MIDI clock out needs a timed emitter; tempo changes land at block granularity (1.45 ms, under one microstep: 2.6 ms at 120 BPM) |
**Decision: C.** The sequencer is a pure `step(block_start, n) → events[]` over a `uint64` sample timeline; tempo is samples-per-tick in Q32.32 (no float drift). Grid: **192 PPQN = 48 microsteps per step** (this OS's resolution for micromove, roll and note length; the official resolution is an open research question). Swing uses this OS's pair arithmetic (new Community OS behaviour, checked against the reference vectors of OS-005c, the output of our own reference model; whether the official firmware's swing is the same is an open research question); PAR-081 swing FX and PAR-082 tempo FX change the next step's duration. Events carry a sample offset 0..n−1; voice kernels take `(begin, end)` so note-on/off and FX changes split the block (SIMD unroll by 4, scalar remainder). Row FX are expanded at step time; per-tick FX (roll, arp, gate length) become sub-step events; continuous FX (LFO, glide/slide) become per-voice modulators (D6). The three control layers (sequencer, sequencer 2, performance; BASE-20) are a per-parameter source table, pure and host-tested. Playheads: song cursor, per-track pattern cursor (own lengths), performance layer; song/pattern switch at pattern end without stopping (PAR-021). Pattern rows are read from the model arena (D7) with a generation check and a 96 B row copy (H4 out); residency and preload are OS-004a's (H5/H6 out: the render never requests SD). MIDI in: bytes timestamped in the UART/USB ISR (H13 out) → `midiRt` ring → clock-follow PLL (24/48 PPQN, slews phase, never jumps), Start/Stop/Continue/SPP as transport events; PAR-119 correction reserved until it is specified. MIDI out (clock, notes of MIDI tracks, CC A–F, PC, hang bookkeeping PAR-063): due sample times → `midiOut` timed queue → PIT-compare emitter (DIN, jitter ≤ 50 µs target [est]); USB MIDI is 1 ms-quantised by the bus. H7/H8 out: nothing spins.

## D6 — Parameters and modulation
- `ParamChange` ring (256 entries) drained after the block's voices have rendered, in the graph's render (an edit applies from the next block; a note in the same block starts on the old value, for at most one block: intended); each audible parameter has a target and a per-sample linear ramp over one block (gain, pan, sends), coefficient refresh every 16 samples (cutoff, resonance), per-sample phase-increment ramp (glide); all times in one `kSmoothing` table (PAR-125/126 behaviour, method ours). Descriptor flags `smooth`, `modulatable`, `learnable` (ADR 0009).
- Audio → main: an `AudioStatus` struct (single writer, generation counter: meters with peak hold, position, load) read by the UI at 30 Hz, plus an events ring (overload, note-on LEDs, step change). No observers.
- MIDI Learn: CC from `midiUi` → action (pickup mode) → ParamChange; targets are any `learnable` descriptor incl. module parameters. Live MIDI notes/CC-as-modulation reach the render directly via `midiRt` when a track is MIDI-input.
| Modulators | Pros | Cons |
|---|---|---|
| per track/instrument, shared | cheap; free-running LFOs | tails share modulators; per-voice envelopes break |
| **per voice state, LFO phase mode {voice, track, tempo-sync}** | the instrument's 6 envelope slots map 1:1; both behaviours | 16 × state (≈ 1 KiB per voice [est]) |
**Decision:** per voice; 4 LFO + 4 envelopes + sources (velocity, key, macro, CC, bend, aftertouch, input follower) → 16 routes per instrument (product-ux §7 D4), evaluated at block rate (690 Hz) with per-sample ramps. Hook points: pitch, cutoff/resonance, amp, pan, position (start/wavetable/granular), sends, insert and module parameters flagged `modulatable`. Envelope/LFO shapes are pure functions (this OS's model, tested against double-precision references).

## D7 — SDRAM map (16,777,216 B) and SRAM placement
One header `mem_map.h`; every region `constexpr`, `alignas(32)`, `static_assert`ed to sum ≤ 16 MiB (ADR 0011 lesson 1). Rule for lines in SDRAM: **block-wise processing, line length ≥ kBlock** (read/write 64 sequential samples per line per block, one row open) — reverb and delay then cost ≈ 2–3 cycles per sample of SDRAM traffic instead of 10–15 random [est, HW-B]; anything shorter than a block (chorus, flanger, combs) lives in SRAM.
| Region | KiB | Use | Tunable |
|---|---:|---|---|
| **Sample arena** | 11,456 | samples, recordings, edit working copies, resample/freeze output — one bump arena with a compacting job; samples are handles, voices resolve the base per block; recording allocates at the top so an empty project records **133 s** (PAR-134) with **no overlap** (D-OS-10); in-place edits with the undo snapshot on SD when no copy fits | yes (vs project/module) |
| Delay lines | 768 | stereo int16, up to 4.0 s = 689 KiB (the official firmware's longest delay time and memory use are open research questions) + a mono tap line | yes |
| Reverb lines | 256 | FDN 8–16 int16 lines, block-wise | no |
| Module region (ADR 0002) | 768 | cold code/data of loaded modules, via the cached alias if HW-D validates it | yes |
| Project model arena (OS-004a) | 1,280 | song, instruments, resident patterns (a 16-track × 128-step pattern = 16 KiB; ≈ 64 resident, or OS-004a streams) | yes |
| Undo journal (ADR 0009 rev 1) | 384 | fixed-record ring | yes |
| File/job buffers | 512 | WAV import/export staging, SD preview ring 32 KiB, export ring, module load staging, directory index | no |
| Boot-generated LUTs | 192 | sine, pitch/exp, dB, granular windows, BLEP, 303 mips if a module asks | no |
| UI cache (ADR 0010) | 512 | wave mipmaps, LRU per sample; a full-length 11 MiB sample's 4 levels ≈ 490 KiB [est] | no |
| Diag log (session only; SDRAM is power-gated [fact]) | 64 | ring | no |
| **Slack** | **192** | 1.2 % | — |
**SRAM (256 KiB) [est, `diag mem` high-water]:** SRAM_L 64 KiB (code bus) = vectors 1 + hot kernels (voices, buses, filters, sequencer) ≈ 20 + module slab 16 + audio HAL/ISR 4 → ≈ 41 KiB, no data. SRAM_U 192 KiB = I²S TX/RX rings (2 × 768 B, `alignas(4)`), fixed block buffers (no dynamic pool: 16 voice floats 4 KiB + tracks/buses int16 ≈ 10 KiB), voice state 16 KiB, module hot state 16 KiB, sequencer 8 KiB, rings (midi, params, events, StemRing 10 ch × 5 blocks, USB return/packets) ≈ 16 KiB, SD sector and FT812 DL staging 24 KiB, UI/app `.bss` ≈ 40 KiB, stacks (main + the single ISR stack, painted, high-water) 24 KiB → ≈ 160 KiB, ≥ 32 KiB free (H14 out).

## D8 — Testing and acceptance
| Level | What | Evidence |
|---|---|---|
| **V1 host reference tests** (doctest, ASan/UBSan, `-Wdouble-promotion`) | sequencer arithmetic vs reference formulae (tempo, swing, microsteps, FX expansion, control layers); allocator/stealing; smoothing; mod matrix; envelope/LFO vs double-precision references; interpolators Q15 vs float; SVF vs `stmlib`/double; block-wise delay/FDN vs sample-wise reference; dB map (PAR-023), pan law (PAR-087), bit depth; Paula BLEP vs pt2-clone; SIMD sums/meters/interleave vs scalar; sample-arena compaction (handles stay valid) | correctness and relative op counts only |
| **V3 QEMU `mps2-an386`** | the same units on the real intrinsics; render loop with a timer-driven fake I²S; module loader and relocation; fault handlers | no cycle claims (no DWT fidelity) |
| **V5 `hw-session` bench** | `BENCH` lines: cycles per voice kind at ratios 0.5/1/2/4, granular, wavetable, Amiga A/B/C; SVF/biquad/drive; track sum; delay; FDN; master chain; meters; sequencer per block; SDRAM random vs sequential vs LDM burst, with SD DMA concurrent; render worst case and jitter with USB + SD + SPI active; underrun soak 30 min at 12 heavy tracks; latencies (pad → out, line-in → out by loopback, MIDI in → note, DIN clock jitter); 196/3125 MCLK (ADR 0012 item 2); codec input wiring (mic + line simultaneous?) | measured numbers in the perf notes |
**Audio acceptance:** rows with a digital definition (gain map, pan law, bit depth, step timing in samples, export length) pass a **null test** against this OS's own reference model; the gain map and the pan law are our model (maintainer answer Q4 of OS-CLEANROOM, 2026-10-18), so a null test against an export of an official unit of the same fixture (within ±1 LSB after rate correction) waits for a recording and is an open research question; DSP rows whose algorithm is unknown (saturation, limiter, delay filter, reverb, granular edges PAR-125/126) are **A/B listening** rows until the official behaviour is recorded (ADR 0008, Q-3). Gates: HW-A (skeleton: silence, ISR timing, underrun counters, retune lock), HW-B (voice/bus benches, SDRAM latency), HW-C (inputs, recorder, crackle soak PAR-154, latency), HW-D (modules: slab, alias, 303 cost).

## D9 — Slices (each host-verifiable as far as the row allows)
| Slice | Content | Verified by | Gate |
|---|---|---|---|
| **OS-005a** | engine skeleton: `mem_map.h`, priority table, I²S DMA ISR + render IRQ, 2/3-slot ring, one rate constant + retune, DWT per stage, `diag cpu/mem`, silence + test signal | QEMU fake I²S; ctest for map sums | **HW-A** |
| OS-005b | fixed graph: tracks, buses, routing matrix, master chain table with bypassed inserts, meters, `AudioStatus`, scope/spectrum/StemTap rings | V1 SIMD vs scalar; ring tests | HW-A |
| OS-005c | sequencer v2: timeline, grid, swing, FX expansion, control layers, playheads, transport, ParamChange drain + smoothing, MIDI clock in/out queues | V1 vs reference formulae (our own model) | — (timing in HW-B) |
| OS-005d | voices: pool, allocator, sampler family (8 modes, slices), span prefetch, envelopes/LFO/mod matrix, TPT SVF, drive | V1; V3 SIMD | **HW-B** |
| OS-005e | sends and master: block-wise delay, FDN reverb, dB map, pan law, bit depth, insert points | V1 vs sample-wise reference | HW-B + A/B |
| OS-005f | inputs and recorder: codec sources, USB RETURN, REC/resample into the sample arena, compaction job, monitoring (PAR-048/076) | V1 arena tests | **HW-C** |
| OS-005g | offline render/export pull mode, stems, tail detection, selection cut (PAR-054/069/139/151) | V1 null tests on fixtures | HW-C |
| OS-005h | Amiga/Paula voice + Paula insert (`dsp-port`, pt2-clone as reference) | V1 | HW-B listening |
| OS-005i | module host: per-voice contract, DWT guard, SRAM_L slab, SDRAM region, loader; 303 via `dsp-port` | V3 loader; V1 303 float vs double | **HW-D** |
Order: a → b → c ∥ d → e → f → g; h and i after d. Performance mode and MIDI tracks ride on c (control layer, MIDI-out queue); their screens are P2/P4.

## Hazards designed out
H1/H2/H4/H12 → D5 (no sequencer ISR, one timeline, row copy under generation) · H3/H6/H7/H8/H13 → D1/D5 (ISRs only timestamp and enqueue; timed emitter; render never touches SD) · H5 → D5/OS-004a · H9/H10 → D6 (`AudioStatus` + rings, `std::atomic` single-writer) · H11 → D1 priority/ownership table · H14 → D7 painted stacks + `diag mem` · H16 → D1 `hal::audio::start()` after mute.

## Open for the maintainer
1. **Block size 64** (latency) over 128 (≈ 3 % CPU cheaper): veto if the 2.9 ms granularity of 128-frame blocks matters more than latency.
2. **Sample arena 11.19 MiB** gives the 133 s record only in an empty project; a full project records what is free. Accept, or shrink the project/module regions.
3. **Master chain order** B is an inference; the official chain's order stays an open research question (Q-3: a measurement on an official unit, if ever done), or accept listening A/B.
4. **Amiga/Paula in core flash** (≈ 5 KB) rather than an SD module.
5. Tail overlap default **on** (4 shared tail voices) and the PAR-024 send default (pre-fader, our choice).
6. One PIT channel dedicated to the MIDI-out emitter (jitter ≤ 50 µs on DIN).

## Acceptance (2026-10-11)
- **Open items 1, 2, 4, 5 and 6 take the recommended defaults:** 64-frame blocks; the 11.19 MiB sample arena, with recording limited to free space; Amiga/Paula in core flash; tail overlap on; pre-fader sends; one PIT channel for the MIDI-out emitter. The maintainer may veto any of them, and each is one constant or one table.
- **Item 3, master chain order,** joins triage Q-3 (the official chain's order, an open research question). Until it is answered, the chain ships with the inferred order, every insert sits behind its table entry, and acceptance for those rows is by listening A/B.
- **Order prerequisite:** OS-005a needs the `platform/mk66` skeleton (startup, clocks, linker script generated from `mem_map.h`, NVIC priority table, SysTick and DWT) and the QEMU test layer (STATUS.md basket items 12–13) first. They become slice OS-007a, ahead of OS-005a.

## Amendment 1 — project region follows the OS-004a model (2026-10-11)
The merged project model's worst-case arena is 1,904,856 B (`model/.../limits.h`, static-asserted). D7's project region therefore grows from 1,280 KiB to **1,920 KiB**, and `mem_map.h` asserts that it is at least the model arena size. The extra 640 KiB comes from the sample arena, which drops from 11,456 KiB to **10,816 KiB**; the slack is unchanged. Recording stays limited to free arena space. Raising the 128-pattern pool towards the OG's 255 would need about 1.8 MiB more and is a later trade-off (OS-004b reports patterns 129–255 as dropped).

## Amendment 2 — what OS-005b built: SRAM split and graph cost (2026-10-11)
- **SRAM_L now holds data as well as code.**
  - D7 kept SRAM_L for code only ("no data"). OS-005b puts the cold tap memory (`.tapmem_l`, 22,272 B: the stem pool, the scope, spectrum and meter rings) into SRAM_L beside `.ramfunc_l` (14,588 B).
  - The hot block buffers stay in SRAM_U.
  - Default image after the change:
    - SRAM_L: 36,860 B used, 12,292 B free in the region; hot-code budget 9,988 B free.
    - SRAM_U: 64,360 B used, 107,672 B free after the stack.
  - **Voices and modules (005d onwards) must plan inside these figures.** The 16 KiB module slab of D7 now competes with the hot-code headroom. 005d states its SRAM_L and SRAM_U bytes before it starts, and moves anything cold to SDRAM.
- **Graph cost.**
  - D1 budgeted the graph at about 2 % of the block. OS-005b estimates about 29 k cycles (11 %) for 12 centred tracks with a send each, falling to about 22 k with only the master meters. These figures come from a disassembly cycle model, not a measurement.
  - With the D1 voice budget this exceeds the 60 % governor line. Two consequences:
    - Meters and taps are consumer-gated: a screen asks for them, and they cost nothing otherwise.
    - The **HW-A G-rows measure the real cost before 005d is specified**. If the measurement confirms the estimate, 005d either uses fewer than 16 voices at 44.1 kHz or optimises the strip kernels first. The decision is recorded then.
- **Solo and the sends (Community OS choice; the official firmware's live behaviour is an open research question, Q).** By default a live solo mutes the other tracks' sends too; `set_solo_mutes_sends(false)` leaves them open (the official manual is read as describing that for the export: documented, not verified). Either is one constant if the maintainer prefers otherwise.
- **PAR-023 level curve:** −24..+24 dB from 1.2 onwards, default 50 = 0 dB. The two anchors are sourced; the curve shape between them is inferred and is an open item for the strategy-B batch. Maintainer decision, 2026-10-16: the Og12 law (its 101-entry table) stays as it is, for compatibility (OS-FIX-OG12-COMMENT); the repo holds no capture behind its curve shape, so that shape stays inferred (Q).

## Amendment 3 — the send effects: memory, placement, cost, controls (OS-SENDFX, 2026-10-13)
**Status of the sound: new Community OS behaviour; the controls follow OS-SENDFX; whether they match the official firmware's is an open research question.** SND A is a delay (`audio/send_delay.h`), SND B a Dattorro plate reverb (`audio/send_reverb.h`), both `SendFxFn`s installed un-bypassed by the MK66 composition root (`app_main.cpp`). Design: radar card `send-fx`.
- **SDRAM (D7 table unchanged, regions now claimed).**
  - Delay region (768 KiB): the stereo ring, 176,468 frames of one 32-bit word (L low half, R high half) = **705,872 B** = 4.0 s at 44,117 Hz; 80,560 B spare. The TIME control stops at 3.5 s.
  - Reverb region (256 KiB): the plate's 13 int16 lines, each on a word boundary, **26,632 samples = 53,264 B** (the paper's 22.5 k samples = 45 KB, plus the 4,096-sample pre-delay and 6 samples of padding); 208,880 B spare. HALF rate uses about half of it.
  - `mem_map.h` mirrors both figures (`kDelayRingBytes`, `kReverbMemBytes`, static-asserted to fit their regions; `app_main.cpp` asserts they equal the effects' own constants). Both are cleared when attached (SDRAM holds whatever it held), before the audio starts.
- **SRAM.**
  - SRAM_U: the two objects, **5,336 B** in `.bss` (delay 1,384 B, reverb 3,952 B: the 64-frame scratch, the staged line contents, the half-band work arrays). Image SRAM_U 125,440 B, 71,168 B free before the 24 KiB stack.
  - SRAM_L: **not used by the effects.** Its hot budget has 388 B of room (`.ramfunc_l` 26,236 B of 26,624 B). The effects run from flash at `-O3` (their two sources, `audio/CMakeLists.txt`), through the graph's `SendFxFn` pointer (an indirect call, which the ramfunc check allows). The block-wise inner loops are short and loop-resident in the flash cache; a placement in SRAM_L is a measurement decision for the hardware session.
  - Flash: +13.0 KB over the 0.4.5 image (13,032 B: the two effects 11.9 KB at -O3, the graph, engine, voices and app 1.1 KB).
- **CPU, ESTIMATED (not measured; no SDRAM wait states in it).**
  - Method: the effects compiled for the MK66 at `-O3`, the instructions of each inner loop counted from the disassembly and priced (load 2, store 1, VADD/VMUL/VCVT/VMOV/SSAT 1, VFMA 3, VDIV/VSQRT 14), plus the staging copies at about 2 to 3 cycles a sample; ±30 %. The same figures are printed by `o3os_audio_bench`.
  - Delay, ping-pong, steady: about 85 cycles a frame = **2.1 %** of the 261,120-cycle block (3.6 % while a time cross-fade runs, 1,024 frames).
  - Reverb FULL (44 kHz): about 369 cycles a frame = **9.0 %**. Over the 5 % line, so the HALF variant was built.
  - Reverb HALF (22 kHz; the default): about 237 cycles a frame = **5.8 %**. The radar's 4.8 % for the full-rate plate left out the staging and the int16 conversions at every line; both are in this figure.
  - Together (delay + HALF reverb) **7.9 %** against D1's 1.5 % + 3 %. A DWT measurement on the unit decides; the options if it confirms the estimate are a fixed-point plate (SMLAWB: the arithmetic of a line about 40 % cheaper), fewer output taps (4 of the paper's 14 cost about 8 cycles a frame each), or a 4-line FDN "Lite" tier (radar option C, about 3 %).
- **Controls: OS-SENDFX's set, on the Mixer's MASTER FX page** (rows after CUE LEVEL, edited and sent as the levels are: a `ParamChange` of kind Master, fields `0x110..` delay and `0x120..` reverb, `graph_params.h sendfx::`). Delay: PING-PONG, SYNC, RATE (19 rates, in steps), TIME (1..3500 ms, 10 ms a detent below 500, 30 above), FEEDBACK (0..70). Reverb: SIZE (the tank's decay, RT60 about 1.2 s to 18 s), DAMPING, PREDELAY (0..93 ms), DIFFUSION (100 is 0.85). The send levels, routing bits, return levels and mutes are the existing ones.
  - **New, runtime only, NOT persisted (OS-PERSIST2):** DLY TONE (the feedback low-pass), DLY MODE (MONO or STEREO with ping-pong off), RVB QUALITY (HALF or FULL; a change clears the tail). The OG's own fields (delay time, feedback and flags, the reverb block) are read by the importer into the OGPM chunk and not applied; applying them at load is part of OS-PERSIST2. No persistent format changed.
- **Sends reach the effects.** An instrument's DELAY SEND / REVERB SEND (0..100, the instrument's own sends) go to the note's track at its start (instant) and on a live edit (smoothed over a block) through `Graph::set_voice_sends`, added to the strip's own SendA / SendB. **The routing matrix's SND A / SND B bit gates only the strip's own send**; the instrument's send is open at the default route (MAIN | USB, which has neither bit and must stay the default: MIXR is written only when it differs from it), so an OG project sounds. The master DELAY / REVERB levels and mutes, the track's mute and a solo elsewhere close both. A project load (`app::tracks_apply`) sends the project's SENDS setting (PAR-024: post-fader sends), forgets the last notes' instrument sends (`master::VoiceSendsReset`) and tells each effect to clear its memory (`master::DelayClear` / `ReverbClear`: a slice of 2,048 frames or samples a block, the return silent meanwhile; an effect that sleeps has nothing to clear). A change of reverb quality clears the same way.
- **Delay behaviour.** Integer synced times (round(rate x fs x 15 / bpm), the tempo from the event source when it changes), a 1,024-frame cross-fade when the time moves (a linear pair of gains that sum to 1, so a fade never swells the loop), one-pole low-pass and DC blocker in the loop, loop gain capped at 0.98, writes saturated and cut towards zero so a tail ends in exact silence and the effect sleeps (the cost then is a 64-sample test of the input).
- **Reverb behaviour.** Constants are the paper's, **written down from memory without the paper at hand: to be audited** (the tests hold the properties, not the numbers). Nothing of Mutable Instruments' Clouds reverb (MIT) is used, so `THIRD_PARTY.md` has no entry for it.
- **Open items.** (1) The Routing page's SND A / SND B bits gate only the strip's own send, not the instrument's. (2) Step FX 30 and 32 (delay / reverb send) are not applied by the engine. (3) A project load does not push the Master FX values (they are runtime only). (4) MIDI clock follow may move the tempo every block: each change of at least one frame in the synced time starts a cross-fade (cost, and a slight phasing); a hysteresis is a follow-up. (5) A/B listening of both effects, the paper audit, the DWT figures and the SDRAM latency are hardware-session rows.

## Amendment 4 — the loop player's routing (OS-LOOPER L1, 2026-10-16)
**New Community OS behaviour.** A take made with LOOP = N BARS (`docs/specs/OS-LOOPER.md`) is played back by `audio/loop_player.h`.
- **Routing.** The loop is added into the TX block, the interleaved pair the codec plays, **after every tap has read it**, in `Graph::render()` right after the metronome's click (same place, same reason). It is therefore in no REC take, no RESAMPLE take (`RecTap::Source::Master` reads MAIN before this), no stem, no scope, no spectrum, no meter, no app tap, and no export (`Engine::offline_enter` parks the player: it hears nothing and moves nothing while an export has the engine, and it is halted when the engine is given back): a RESAMPLE overdub cannot record the loop into itself (the no-loopback rule of ADR 0012 amendment 6, which keeps the USB return out of the captures the same way). It reaches both channels of the line out and the headphones (MAIN on the left jack and CUE on the right one in the split OUT MODE; the loop is mono), saturating, at LOOP VOL (the mixer level law) times the master level (the smoother's gain of the block). It bypasses the master chain and the limiter, as the click does; the master level scales it.
- **Clock.** Sample counter restarted by the sequencer's Step events (the 16 N-th after the take began), not a free-running modulo: see the header of `loop_player.h`. Tempo changes keep the loop's length in samples.
- **Placement and cost.** State: a few words at the end of the `Graph` (SRAM_U), outside the taps' memory. Code: `LoopPlayer::on_step / on_stopped` and `Graph::mix_extras` (the click and the loop, one call behind one test: `metro_.busy() || loop_.needs_service()`, the latter one byte) are `O3OS_AUDIO_COLD` (flash; per Step event and per block while a click sounds or a loop exists), so SRAM_L grows by the call sites and one test per block, nothing per sample. The loop reads the take where it lies in the arena (SDRAM), sequentially. CPU [est] 0.3 to 0.4 % with a loop playing, to be measured on the unit.
- **Contexts.** The main loop announces (`capture`), releases, sets the volume and the mute (atomics, one request word the render takes with an exchange) and reads `phase()`; everything else is the render's. The buffer is the recorder's take: nothing writes it while it plays, `release()` makes the render fade out over 32 frames and let go at the end of the block (a project load, NEW or a kit releases it first through `SampleBank::set_clear_hook`), and the recorder arms no take until `phase()` is Idle.

**Overdub (OS-LOOPER L2c, 2026-10-17): mix, then write.** The loop player is the first writer of the take's memory. In the same `serve()` that adds the loop into the TX block, the frames it has just played are rewritten in place: `loop[pos] = sat(loop[pos] x FEEDBACK + input[pos])` (Q16 feedback, `loop_ov::loop_rmw`), so a new layer is heard from the next pass, never twice in the pass it is written in, and lands exactly on the layer under it (the write head is the play head; no latency constant). The input is the REC tap's own: the REC bus planes (MAIN for a RESAMPLE) after `run_taps`, read in place (left or right plane) or mixed to mono in the graph's scratch block; the loop itself is added after the taps and is therefore never in the layer it is written to. Before the first write to a 64-frame block of a session the block is copied to the undo buffer behind the take (`SampleBank::take_end`'s tail) and its bit set; UNDO LAYER copies the marked blocks back a few per render block, bounded by copies, bitmap words and cycles, skipping a block that has used more than 70 % of its cycles but never more than eight in a row. The session writes nothing in a block that releases the loop, when the render is muted or parked, and nothing after `phase()` says Idle (the sentinel test). The main loop and the render talk through one command word with a sequence number and a published acknowledgement (`overdub_on / quiesce / undo`, `acked(seq)`), a published flags byte (`overdubbing`, `restoring`, `layer`), and the bitmap, which the main loop clears only while neither a session nor a restore runs. Cost: [unit] to be measured (`diag bench looper`, the gate of docs/specs/OS-LOOPER.md 3c.7); size at L2c: FLASH +4,868 B, SRAM_L +0 B, SRAM_U +512 B, SDRAM_STATIC 0.

## Amendment 5 — the EQ and bass slots run ahead of the limiter (OS-DJ A3c, 2026-10-19)
**New Community OS behaviour** (orchestrator decision, 2026-10-19; the order of the official chain stays an open research question, Q-3). `kMasterChain[]` is now headroom, saturation, **EQ, bass, limiter**, space, bit depth: the EQ and bass slots hold the DJ page's kill EQ and DJ filter (`docs/specs/OS-DJ.md`), whose all-pass split and near-open ringing raise the peaks of a limited mix; behind the limiter the converter clipped them, ahead of it the limiter's ceiling (-0.3 dBFS, `|out| <= kCeiling` for any finite input) holds. Both slots are bypassed at rest, bit for bit, with no cost; nothing else in the chain moves. The MAIN meters (read where the limiter takes the bus) now count the DJ stages' peaks as the overload the limiter caught.

## Amendment 6 — D1: the TX ring is 3 slots (OS-CPU10 slice 3, 2026-10-23)
**New Community OS behaviour** (maintainer decision, 2026-10-23, taking the "3 as a one-constant fallback" of D1). `hal::audio::kTxSlots` is 3: the block rendered for boundary `s` plays at `s + 2`, so the render's deadline is two blocks instead of one and a single late render (a USB or display ISR burst, one heavy block) no longer underruns; the cost is +1.45 ms of output latency (line-in to out about 5.8 ms, pad to sound +1.45 ms; the MIDI timeline's `kAudioLeadCycles` follows `kTxSlots` and moves by the same 261,120 cycles) and the physical TX ring becomes 4 slots (`kTxPhysSlots`, the eDMA modulo needs a power of two): 1 KiB, +512 B of SRAM_U, still inside the 2 KiB `audio_io_rings` row of D7 (with the 1 KiB RX ring), ATTR[SMOD] 10 and the ring 1 KiB aligned like the RX ring. Unchanged: the 60 % governor budget is still measured against one block; an underrun still replays the stale slot, three in a row still mute (a render that stalls loses `kTxSlots - 1` consecutive boundaries, 2 of the 3 that mute); the render still serves the newest boundary. Verification: host (`audio_slot_ring_test`, `audio_pipeline_test` in terms of `kTxSlots`), qemu (`audio_fake_test`), and the unit: the Heavy project's underrun counter in `diag cpu` (hardware check still open).

## Amendment 7 — the main-loop guard: the render sheds when the main loop starves (OS-MAINGUARD, 2026-10-23)
**New Community OS behaviour** (Level C, a 1.0 blocker; hardware check pending). The render line preempts thread mode and serves the newest unrendered boundary, so a render of about a whole block (Projects/Heavy: unmuted mean about 266k cycles of 261,120, spikes to 430k) keeps its pend latched and the main loop (console, UI, Stop, the SD jobs, the heartbeat LED) never runs; with `kTxSlots` 3 (amendment 6) nothing underruns any more, so the underrun mute that used to shed load by accident no longer latches. The guard puts a floor under the main loop, and only there: `Pipeline::main_pass()` is called once per main-loop pass (a beat, one word, single writer: the main loop); a render over the governor budget (60 %) while the beat has not moved is stale, and after `kStarveBlocks` (8) stale renders in a row the render sheds, block after block, until the beat moves (a whole pass has run), then 8 more renders. Any render under 60 % ends the count, so a light load never sheds, however long an SD job takes. A shed block is a fade out from the last frame published (the first of a run) or zeros, the first real block after a run ramps in (64 samples): no step. It publishes through the normal slot protocol (`on_time` first; a shed whose slot already plays is not published and the ISR has counted the underrun, `shed_late`). Unchanged: slot ownership, the underrun and mute policy, the 60 % governor, the render callback's contract. What a shed hides: the callback does not run, so the sequencer, the sample clock, the stems tap and the recorder skip the block: a hole of one block, 64 frames (1.45 ms), per shed block, in a recording; the song holds for that time. Counters (`diag cpu`, line `guard`): shed blocks, runs, late, the longest run since boot; the render is their only writer (`cpu reset` hands it a request count and it zeroes them at its next block; the longest run stays since boot, 8 bits, stops at 255). Off unless `Config::starve_guard` (the MK66 main arms it just before its loop; `diag bench` waits beat). Verification: host (`audio_pipeline_test`, `audio_status_test`), qemu (`audio_fake_test`: a render of a whole block with a thread loop; guard off about 0 passes, on: the loop passes, 11 % (a 0.5-block pass) to 28 % (2.5 blocks) of the blocks shed, no underrun, no mute; a light render sheds 0), and the unit: Heavy on a guard image (console answers, Stop works, the heartbeat LED blinks, shed counters > 0; a 50 % project sheds 0), hardware check open.

Bytes of amendment 7 (remote mk66, against the amendment 6 figures): FLASH 900,132 -> 901,548 B (+1,416; bench 928,236 -> 929,636), `.ramfunc_l` 26,496 -> 26,920 B (+424, of the 27,648 B hot-code limit: 728 B free; bench 26,532 -> 26,920), SRAM_U 139,240 B unchanged (the guard state is 18 bytes packed by hand into the Pipeline's padding and a 3-byte gap, no static of its own: the 24 B margin of the 32 KiB floor is untouched). `copy_block` is always-inline (three call sites made GCC outline it into flash, which `check_ramfunc` refuses).

Known limits of amendment 7 (to be measured on the unit, not hidden). (1) A shed whose slot has already started to play (`late`) is not published, and the next shed block of the run is zeros, not a fade from the last real frame: a rare step to silence can happen on top of an underrun the ISR has already counted. (2) The trigger is the 60 % governor budget, not a measure of starvation: at a steady 65 to 80 % render load a long main pass (an SD slice, a screen rebuild) that spans more than 8 renders can shed a few blocks although nothing underruns; `diag cpu` (`shed`, `runs`, `longest`) on a 65 to 80 % project during a screen rebuild decides whether the threshold or `kStarveBlocks` moves. Guard against growth: `platform/mk66/src/audio.cpp` asserts `sizeof(Pipeline) == 856` (the SRAM_U margin is 24 B).
