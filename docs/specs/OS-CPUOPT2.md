# OS-CPUOPT2: the pan LFO's strip cost: G1 (dropped), G2 (the strips stage), M1 (deferred)

Status: **spec, decided 2026-10-16 (the five questions answered by the spec session for the maintainer: G1 and G1-0 dropped, G2 specified in section 11, M1 deferred); no code; PARKED at the spec on 2026-10-17 (see the State section)** · Level C (audio render path, SRAM_L budget) · **No behaviour change**: every slice is bit-identical to what the unit plays today, proved by a differential test, or it is not built · Follows `docs/specs/OS-CPUOPT1.md` (sections 8.6 and 8.7: proposals G1 and M1), `docs/specs/OS-LFOLIVE.md`, `docs/architecture/cpu-budget.md`, skill `rt-optimize`.

Sections 2 to 7 are the G1 design as it was specified; they stay for the record (G1 was dropped on the measurement of section 1, not on a flaw) and are not to be built. The live part of this spec is section 11 (G2) with section 10 (slices) and the state for the implementer at the end.

Classification: **new Community OS behaviour** is unchanged by this item (no parity claim is touched). Evidence markers: [unit] measured on the unit (tester, bench image 2 `g248905f`; report and raw attribution files held by the tester), [src] read in the source at dev 61f7270, [est] estimate, to be replaced by the measurement of section 7. One block = 64 frames = 261,120 cycles, 1 % = 2,611 cycles; "over budget" is a render above the governor's share of the block (`pipeline.cpp:111`, 60 %, 156.7k).

## 1. What the unit says about the pan LFO (it lowers the expected gain of G1)

Verified config [unit]: 16 instruments, FILTER On, LFO SPEED detent 10, scene t-v16 song from order 0, 7 windows of `bench stages 4096`. All-off baseline 97,128 cycles a render block.

| Arm | Delta render_total | Over-budget blocks | `cpu` max |
|---|---|---|---|
| Pan only | +3,516 (+1.35 %) | 64 -> 179 | 221,687 (84.9 %) |
| Cutoff only / Finetune only | +1,802 / +1,727 | 95 / 87 | |
| A full | +6,984 (+2.67 %), additive | 760 | 229,989 (88.1 %) |

The stage counters in the same files (pan arm minus baseline, mean of the 7 windows) say **where the pan cost is**:

| Stage | Pan - off | Share | What runs there |
|---|---|---|---|
| `drain` | **+655** (max 25.7k -> 29.5k) | 19 % | `Graph::drain` -> `recompute_strip` of each dirty strip: **the only part G1 touches** |
| `strips` | +1,724 | 49 % | the mixer's per-track pass: a moving pan ramps the dest gains and takes the panned lane kernel (`gain_pan2` / `gain_pan`) instead of the centred one (`gain_mac2` / `gain_mac`), `graph.cpp:726-745` |
| `render` | +1,181 (74 a voice-block) | 34 % | `mod_block`'s pan part: one `flfo_eval` (75..90 cycles) and the integer pan: at the arithmetic floor |

These are not noise: per window (same song position in both arms) the drain delta is 1,081 / 1,098 / 1,062 / 879 / 165 / 152 / 146 (the first four windows have all 16 voices playing), strips 2,973 .. 387, render 2,000 .. 398. The ratios between stages are stable; the level follows the song.

Consequences: (1) **the ceiling of G1 is the drain delta: +655 cycles on average (0.25 % of the block), about 1.0k in the dense windows (0.4 %)**, not the 5-6k (2 pp) that OS-CPUOPT1 section 8.7 estimated; (2) the render stage shows no cold-code surcharge for the pan modulator (74 cycles a voice-block is one LFO evaluation), which weakens M1's premise (section 8); (3) the largest pan cost, the strips stage, is a kernel-selection cost that G1 does not touch (follow-up G2, section 11).

**Decision (Q1, 2026-10-16): G1 and G1-0 are dropped.** The drain delta of the pan arm, **+655 cycles mean (0.25 % of the block; about 1.0k in the dense windows), is recorded as the floor of the pan arm's drain stage**: it is the cost of recomputing a strip once per integer pan step, and a change that could win at most 0.15 % of the block does not justify a Level C change of the drain. The cpu-budget row records it with this spec as its evidence.

## 2. G1 design: what is recomputed now, what is skipped (dropped, Q1; for the record)

**Now [src].** The pan modulator runs in `VoicePool::mod_block` (`voices.cpp:1772`): `pan = s.pan + int(mod * 100)` held to 0..100, stored in `pan_val_[track]`, `pan_dirty_`. After the block's voices `Engine::apply_pan_mods` (`engine.cpp:69`) calls `Graph::set_voice_pan(track, value, 0)` (`graph.cpp:273`): when the offset differs from the one in force it stores `pan_offset`, sets `ramp_[track]` and the strip's bit in `dirty_`. At the start of the next block `Graph::drain` (`graph.cpp:222`, SRAM_L) calls `recompute_strip(i, ramp_[i])` (`graph.cpp:525`, flash) for every dirty strip. That function recomputes **everything** of the strip, though only the pan moved:

- the fader `level`: 2 `LevelCurve::q16` (volume, dry), 2 `mul_q16`, one `retarget`;
- the two sends: 4 `q16` (send level, delay/reverb level), 4..6 `mul_q16`, 2 `retarget`s, with the mute, solo and route tests;
- the pan law `pan_gains_q16(pan + pan_offset + pan_mod)` and the 6 dest retargets (3 buses x L/R, each gated by its route bit).

[est] 400..500 cycles a call including the instruction-cache lines of a flash function.

**G1.** A strip whose *only* pending change is the pan skips everything but the last item: `recompute_pan(slot, ramp)` = the pan law and the 6 dest retargets (the same inline code, same order) and `settled = false`. [est] 100..150 cycles.

Mechanism: `dirty_` keeps its meaning, so every ramp rule that reads it (`((dirty_ >> t) & 1u) != 0u ? ramp_[t] ...`) is untouched. A second mask `full_` says "this strip needs the full recompute". Every writer of `dirty_` other than the three pan setters goes through one helper `mark_full(mask)` that sets both; the pan setters set `dirty_` only. Writers [src]: pan setters `set_voice_pan` (`:282`), `set_track_pan` (`:295`), `set_track_pan_mod` (`:321`); full writers `set_sends_post_fader` (`:138`), `reset_voice_sends` (`:147`), `set_voice_sends` (`:162`), `set_track_gain` (`:308`), `set_strip_mute` (`:338`), `apply_strip` Solo (`:365`) and its tail (`:412`: every other strip field, the model's Pan edit included, stays on the full path), `apply_master` (`:501`). `drain` clears both masks; for each dirty strip it runs the full recompute when its `full_` bit is set, when it is the input strip, or when the **idle guard** fails (section 3); else `recompute_pan`. `configure` (`:102`, zeroes both), `set_solo_mutes_sends` and `init` call `recompute_strip` directly and are unchanged.

Placement and bytes [est]: `recompute_pan` in flash (+about 150 B), the `drain` branch in SRAM_L (+16..32 B of the 312 B free, limit +200 B), `full_` +4 B in the `Graph` object. No ParamId, event, format or ABI change.

## 3. The bit-exactness argument

`recompute_strip` is: for X in {`level`, `send_gain[0]`, `send_gain[1]`}, `X.retarget(T_X(I), ramp)` where `T_X` is a function of the strip's non-pan inputs I (mute, solo, the global any-solo, `solo_mutes_sends_`, `m_dry_`, `m_dry_mute_`, `trk_gain`, `volume`, `sends_post_`, `route`, `send_a/b`, `inst_send_a/b`, `m_delay_`, `m_reverb_` and their mutes, the contents of `curve_`); for each bus d, `dest_l/r[d].retarget(open_d ? pan : 0, ramp)`; and `settled = false`.

`Smoother::retarget(t, r)` (`smoothing.h:73`) is the identity on a smoother that is **idle** (`blocks_ == 0 && cur_ == target_`) when `t == target_`: it stores `target_ = t` and takes the `target == cur_` branch (`cur_ = t`, `blocks_ = 0`). So the full recompute equals `recompute_pan` when, for each of the three X: **(C1)** `T_X(I)` equals the `target_` the last full recompute left, and **(C2)** X is idle.

- **C2** is checked in the drain (the idle guard, `!ramping() && current() == target()` on three smoothers, about 15 cycles). A strip with a fader or send ramp still running (a long ramp from a preset, automation, a multi-block mute) fails the guard and takes the full path, which is what retargets that ramp today (a `ramp = 0` retarget cuts a running ramp to one block: an existing behaviour that G1 keeps exactly).
- **C1** holds iff every write to a member of I marks the strip full. This is the invariant the item adds and the whole risk: today a missed mark on a pan-modulated strip would be healed by the next pan change; after G1 it would not. Enforced by (a) the single helper `mark_full`, (b) the verify mode, (c) the differential test (section 4).
- `settled = false` is set in both paths; the dest retargets are the same inline code; the ramp is `ramp_[i]` in both; the pending-ramp rules read `dirty_`, unchanged; strips are drained in the same ascending order.
- The input strip has no pan law (`PanGains{kUnity, kUnity}`) and always takes the full path.

## 4. Tests (host, no hardware)

1. **Slice G1-0, on unmodified dev:** `tests/audio_graph_panfast_test.cpp` (label `audio`) drives a `Graph` (the rig of `audio_graph_test.cpp`) with a fixed-seed random sequence and hashes (FNV, as `audio_lfo_equiv_test`) the TX block of every block and the strip gains. The ops cover **every writer of I and of the pan**: `set_voice_pan`, `set_track_pan`, `set_track_pan_mod` with ramp 0, 1, 128 and 4096; `ParamChange` of every strip field (volume, pan, mute, solo, route, sends, track gain) and master field (dry, dry mute, delay, reverb and their mutes, volume); `set_voice_sends`, `reset_voice_sends`, `set_sends_post_fader`, `set_solo_mutes_sends`, `set_strip_mute`, `configure`; a note start (ramp 1) and the autopan update in the same block; 13 strips, 60,000 blocks, a fed signal. The hashes are **recorded on dev 61f7270 before any change** and must stay equal after G1.
2. **Verify mode** (`O3OS_AUDIO_STRIP_VERIFY`, defined for the test targets and the bench image, never for release): when `recompute_pan` is taken, `drain` copies the strip, runs `recompute_strip` on the copy and compares every smoother (`cur`, `target`, `blocks`) and `settled`; `strip_verify_fail()` must be 0. Mutants (a macro skips one `mark_full`; another drops the idle guard) must make it non-zero: the test asserts that.
3. **Idle-guard cases by name:** the pan LFO while a 4096-sample fader ramp runs; the block after a mute, a solo of another track, a route change, a sends-mode flip; a note start (ramp 1) followed by the autopan update; the input strip.

Scoped run: `ctest -R audio_graph_panfast`, `-L audio`.

## 5. Expected saving and the over-budget effect

| | Now [unit] | After G1 [est] |
|---|---|---|
| drain delta, pan arm, 16 voices | +655 mean, +3.8k max (a Step boundary moves many pans in one block: 3.8k is about 8 full recomputes) | +180..250 mean (the pan law and 6 retargets stay), max about +1.3k |
| **render_total delta, pan arm** | +3,516 | **-400..-475 mean (0.15..0.18 %), about -0.7k in the dense windows (0.27 %), worst block -2.5k** |
| over-budget blocks (pan arm 179, baseline 64) | 179 | about 120..150: the count follows the spikes G1 cuts, but this is a weak estimate |

G1 recovers 11-14 % of the pan arm's cost and leaves A full at about +6.5k. It does not bring the pan arm near the baseline; that needs G2. The measurement of slice G1-0 (sections 6, 7) replaces these estimates before G1 is built.

## 6. Bench support (behind `-DO3OS_BENCH`, its own slice G1-0)

`diag bench stages` already prints `stage_drain` and `stage_strips`. G1-0 adds, compiled only with `O3OS_BENCH=ON` (a definition `O3OS_AUDIO_STRIP_STATS` on the audio library; the release image is byte-identical, checked by comparing the release `.elf` before and after):

- in `Graph::drain`: counters of full and pan-only recomputes and their summed cycles (`DWT_CYCCNT` pairs around the call), printed after the stages as `BENCH strip_full n=.. cyc=.. | strip_pan n=.. cyc=..` and reset by `diag cpu reset`. They give **the recomputes per block and the cost of one in the real scene**, which fixes the G1 estimate before any behaviour change;
- from G1 on, a bench-only switch `diag bench pan_fast 0|1` (default 1) forcing the old path, so that **arms alternate inside one image and one session**, and the verify mode's line `BENCH strip_eq OK|FAIL n=..` on the unit.

No isolated kernel: `recompute_strip` needs a populated `Graph` (private state), and the in-situ counters are better evidence.

## 7. Unit measurement protocol (tester, after the go)

Same verified config as bench image 2: scene t-v16, 16 instruments FILTER On, LFO SPEED detent 10 (set by `cpuopt_d.py apply`, every toggle checked by screenshot), song from order 0, `bench stages 4096`. **Windows are compared at the same index**: the song is deterministic from order 0 and the pan cost varies 1,000 .. 150 between windows, so a plain mean over mixed positions is not comparable; use the paired per-window delta.

1. **G1-0 image:** Pan only and all-off arms, 14 windows each; read `strip_full n/cyc` (the go/no-go of Q1).
2. **G1 image (bench image 3), `pan_fast 0|1` alternating in one session:** Pan only, 14 windows per arm, plus A full, plus the all-off drift check (within 30 cycles). Report per window index: delta of `stage_drain`, `render_total`, max of `stage_drain`, `cpu` max, over-budget count (counters reset per arm), underruns (0), `BENCH strip_eq OK`.
3. **Cross-check against bench image 2:** the pan-only delta of the same windows (+3,516 mean) must fall by the predicted amount; a result outside -250..-700 is reported, not smoothed over.
4. Pass: `strip_eq OK`, 0 underruns, the pan arm's drain delta down by at least 40 % in the paired windows, no change in the other stages of A full. A listening check (a pan-LFO'd pad at a high amount) goes into the HIL list; the output is bit-identical by construction.

## 8. M1 (a later slice): the warm `mod_block` pass, exact variant

Proposal (OS-CPUOPT1 8.7): at the first span of a block evaluate `mod_block` for all voices before any voice renders, so the modulator code stays hot instead of being evicted by each voice's sample reads.

**Can it be exact? Only with a guard.**
- Everything `mod_block` reads is fixed at the first span: voice state, `tick_q_` (advanced after the loop, `voices.cpp:680`), the descriptor. `render_voice` of voice i changes nothing that voice j > i reads, and note starts are events dispatched before `render`. The per-track outputs (`pan_val_`, `pan_dirty_`) are written in ascending voice order in both schemes. So for deterministic shapes the pass equals the loop.
- The one shared state is the xorshift `rng_` (`voices.cpp:837`), drawn by the **random** shape in `mod_block` (pan, finetune, position, tremolo) **and in the filter path** (the cutoff LFO inside voice i's `filter_run`). Today the draws interleave: v0 mod_block, v0 cutoff, v1 mod_block, ...; with a pass they become all mod_blocks, then the cutoffs: a different voice gets a different random level. That is a behaviour change.
- **Exact variant:** the pass runs only while no playing voice has a random-shape modulator (a pool counter kept at note start and descriptor change); otherwise the interleaved loop runs as today. The draw sequence is then identical in both cases by construction. Equivalence test: the `audio_lfo_equiv_test` hashes (recorded on dev) extended with mixed random / non-random scenes of 2..16 voices. Voices with `mneed` (a note starting mid-block) keep their evaluation at their own span.

**Premise check.** The render deltas per voice-block are 74 (pan), 101 (finetune), 169 (cutoff) cycles: at or below the section-2.2 arithmetic of OS-CPUOPT1, so no cold-code surcharge shows. M1 pays only if a cold `mod_block` costs 300+ cycles. **Recommendation: do not build M1 now**; build it only if an in-situ counter around `mod_block` (the section-6 mechanism, one more slice) reads over 150 cycles a call. Expected gain then 100..300 cycles a voice-block, and 0 in scenes that use the random shape.

**Decision (Q3, 2026-10-16): M1 is deferred, with no counter slice.** The render stage's per-voice-block deltas are at the arithmetic of one LFO evaluation; nothing in the attribution asks for the pass. M1 is reopened only by a new measurement that shows the modulator path above its arithmetic (a future attribution, not a slice of this item).

## 9. Risks and follow-ups

1. **C1, a missed `mark_full`** (section 3): a silently stale gain on a pan-modulated strip. Mitigation: one helper (no raw `dirty_ |=` or `dirty_ =` outside it, the pan setters and `drain`; a grep in the review), verify mode with mutants, the 60k-block differential test, a review of the writer list against the members of I.
2. **Ramp bookkeeping** (`ramp_[]`, the note-start rule `ramp_ == 1`): unchanged because `dirty_` keeps its meaning; the test has the note-start case.
3. **Small gain for a Level C change** (0.15..0.27 % for about 60 lines in the audio drain): hence the go/no-go on the G1-0 counters (Q1).
4. **SRAM_L**: `drain` is hot; the branch adds 16..32 B, inside the +200 B rule.
5. **Verify mode and counters must not ship**: the release `.elf` is compared before and after G1-0.
6. **G2 (section 11, decided Q2):** the strips stage is 49 % of the pan cost. A moving pan leaves the centred lane for the panned one and ramps its dest gains; both are inherent to the smooth pan law, so G2 does not change what is computed, only how many instructions the panned shapes take. Section 11 sizes it and sets its go/no-go.
7. Hardware verification: the section-7 protocol was G1's; G2's is section 11.6. Every slice is bit-identical, but a host test is not a unit measurement.

## 10. Slices

Decided 2026-10-16 (the question table below): G1-0 and G1 are dropped, M1 is deferred without a counter slice, G2 is the item's work. Slices run in this order; each merges on its own when green.

| Slice | Content | Gate |
|---|---|---|
| ~~G1-0~~ | ~~the differential test and hashes recorded on dev; the `O3OS_AUDIO_STRIP_STATS` counters in the bench image~~ | dropped (Q1): the drain delta +655 is the pan arm's floor |
| ~~G1~~ | ~~`full_`, `mark_full`, `recompute_pan`, idle guard, verify mode~~ | dropped (Q1) |
| ~~M1~~ | ~~the `mod_block` counter, then the guarded pass~~ | deferred (Q3), no counter slice; reopened only by a new attribution |
| **G2-0** | bench only, 0 B in the release image: the paired strip-kernel arms in `diag bench kernels` (section 11.4), the per-block shape histogram `BENCH strip_shapes` behind `O3OS_AUDIO_STRIP_STATS`, the equality lines `strip_eq_*`, and the static disassembly count of the four `strip_pairs` instantiations written into section 11.3 | host green; release `.elf` byte-identical before and after; bench image 3a to the tester; the section-11.6 model check decides G2-1 |
| **G2-1** | the kernels: (a) the ramping lane unfused and the per-gain steady shape `gain_pan_r1`; (b) the register-shaped steady stereo pair, only if the G2-0 disassembly shows spills; (c) the four-lane form, only on the section-11.7 D4 evidence. First commit: `tests/audio_graph_strips_test.cpp` with its hashes recorded on the unmodified tree | risk review; kernel tests on EmuOps and on QEMU (NativeOps); hashes unchanged; `ci.sh qemu` before merge; mk66 size deltas within section 11.5; bench image 3b, the paired protocol of 11.6 |
| **G2-2** | docs: the cpu-budget rows (the pan arm's drain floor, the strips result or its floor), this spec's measured numbers, the HIL step for the pan-LFO listening check | doc-keeper, after the tester's report |

Docs per slice: the cpu-budget row with the measured result, no QA catalogue row (no UI change), a HIL step for the pan-LFO listening check (QA catalogue `modules.md`, next to QA-MOD-110..113).

## Maintainer questions (answered 2026-10-16 by the spec session, standing in for the maintainer who delegated these decisions)

| # | Question | Recommendation | Decision |
|---|---|---|---|
| Q1 | G1 can win at most the drain share of the pan cost (+655 cycles, 0.25 %; expected -400..-475, 0.15 %). Build it? | Yes, gated: slice G1-0 first (counters, no behaviour change); build G1 only if the unit shows `strip_full` costing at least 300 cycles a block in the pan arm. Below that, drop G1 and record the pan arm's drain as at its floor. | **Drop G1 and G1-0.** The drain delta +655 is recorded as the pan arm's floor (section 1). |
| Q2 | Open G2 (the strips stage, 49 % of the pan cost, 1.7k) as a research item? | Yes: a spec and a `k_gain_pan` / `gain_mac2` bench first; it is where the pan cost is. | **Yes.** G2 gets its spec (section 11); the paired `gain_pan2` / `gain_mac2` bench kernels come first (G2-0). Go/no-go: the pan arm's strips delta down to at most half, **+1.7k to at most +0.9k cycles** (mean of the paired windows). |
| Q3 | M1, exact variant (guard: no random-shape modulator playing)? | Defer: the data show no cold-code surcharge. Revisit if the `mod_block` counter reads over 150 cycles a call. | **Defer M1, with no counter slice** (section 8). |
| Q4 | Accept a bench-only switch (`pan_fast 0|1`) and a verify mode in the bench image (about 1 KB of flash there, none in release)? | Yes: paired arms in one session and an equality line on the unit. | **Yes, for G2's paired bench arms** (bench-only, 0 B of release flash; the release `.elf` is compared). The G1 switch and verify mode go with G1. |
| Q5 | A model Pan edit (`apply_strip` Pan field) stays on the full path. Also pan-only? | No: rare, no CPU value, one more writer to review. | **No** (moot with G1 dropped). |

## 11. G2: the strips stage of a panned, moving strip

Classification: **new Community OS behaviour unchanged**; every G2 slice is bit-identical (the integer sums of the kernels commute, `graph.h:29-32`; the kernel references are the specification). Evidence: [unit] bench image 2 `g248905f`, `attrE_*.txt`; [src] read at dev 0c82737 (this branch's merge base); [est] estimate, replaced by the G2-0 measurement; [disasm] to be read from the mk66 build in G2-0 (no MK66 ELF exists in this checkout: nothing below is a disassembly claim yet).

### 11.1 Where the strips cost of the pan arm goes [unit, src]

The strips stage (`Graph::run_tracks`, `graph.cpp:647`, SRAM_L) is the per-track pass: copy the voice's block, the sends, then the fader and the pan law in one kernel over the 64 samples, two tracks that open MAIN at a time (`flush_main`, `graph.cpp:747`). Off arm, dense windows (16 voices): `stage_strips` 13,290 mean, i.e. about 830 cycles a strip-block, of which the strip kernel is about half [est]; the rest is `copy_block`, the sends' `mac`, the meter and the smoothers. Pan arm, same windows: 16,263, **+2,973 (2.9 cycles a track-sample)**; the mean over the 7 windows is **+1,724** (the Q2 figure), the per-window delta 2,973 .. 387 following the voice count.

What a moving pan changes in that pass, and nothing else [src]:

1. **Centre to panned (every block while the pan is not exactly 50).** A centred strip (`dest_l[0].same_as(dest_r[0])`, `graph.cpp:730`) adds into the one centre accumulator through `gain_mac2` / `gain_mac` (one multiply-accumulate a sample, one accumulator loaded and stored a pair of samples); a panned strip goes through `gain_pan2` / `gain_pan`: two multiply-accumulates and two accumulators. Per pair of samples and two lanes, counted on `kernels_impl.h:148-215` (not on the disassembly): the centre steady form is about 25 instructions (2 LDR of the blocks, 2 LDR + 2 STR of the accumulators, per lane 2 SMLAW + 2 SSAT + PKHBT + STR for the level, 2 SMLAW for the pan, about 3 for the loop), **6.25 an instruction a track-sample**; the stereo steady form about 33 (4 more LDR/STR, 4 more SMLAW), **8.25**. The surcharge is **2.0 instructions a track-sample, about 2.0 cycles [est], 2,048 a block at 16 voices**: this is the arithmetic floor of the panned shape, and it does not move without changing what is summed.
2. **Steady to ramping (only in the block after an integer pan step).** `mod_block` (`voices.cpp:1772`) writes the pan as `int(s.pan + mod * 100)`: the strip is retargeted only when that integer changes (`set_voice_pan`, `graph.cpp:273`: `pan_offset != offset`), with ramp 0 = one block (`kSmoothBlocks`). In that block the two dest gains ramp (`next_block` gives `start != end`), the lane fails `steady()` for every gain, and `strip_simd` (`kernels_impl.h:216`) takes the `Steady = false` instantiation for **the whole call**: the level (not moving) and the pan law's unity side (the pan law, `smoothing.h`: one side is always exactly unity, so one side's ramp is {65536, 65536} unless the pan crosses 50 inside the block) go through `Gen<false>` as well (`g += step; v = g >> shift`, 2 instructions a gain a sample), and so does the **partner lane** of the two-track form, which is steady. [est] +6 instructions a track-sample for the ramping lane, +6 for the dragged partner: about 770 cycles for one ramping pair. The attribution's drain delta (+655 mean, about 1.5 recomputes a block at 400..500 cycles each, 2.4 in the dense windows) says one or two strips ramp in most blocks.
3. **Register pressure, specific to the stereo form [est, to be read in G2-0].** The stereo steady two-lane loop keeps alive: 2 block pointers, 2 accumulator pointers, the index, 6 Q20 gains (two lanes x level, left, right), 4 accumulator values, the loaded word and the post-fader word, one or two temporaries: 18-19 values against the 13 allocatable registers of the Cortex-M4F at `-Os`; the centre form keeps 13-14. A spill (a LDR and a STR a pair) is likely in the stereo form and absent or rare in the centre one, and it would be a cost of the panned shape alone. The measured surcharge, 2.9 cycles a track-sample against the 2.0 arithmetic floor, leaves about 0.9 (900 cycles a block) for items 2 and 3 together; G2-0 separates them.

So the strips cost of the pan arm is three things: the two-accumulator arithmetic (at its floor, item 1), the ramping shape's generators and the partner it drags (item 2), and whatever the compiler spills in the stereo loop (item 3). **Only items 2 and 3 are G2's levers**, and the −50 % target (+1,724 to at most +862) is reachable only if together they are at least half of the delta: that is the fact G2-0 establishes before any kernel is touched. If the bench shows the surcharge at the arithmetic floor and the ramping share small, G2 is a **no-go**, recorded as "the pan strips cost is at its floor" with the per-shape numbers in the cpu-budget. The spec says this plainly so that a no-go is a result, not a failure.

Not a lever under this spec: fusing the level and the pan multiplications (one product instead of a rounded Q15 intermediate), or packing two tracks' samples for SMLAD (cpu-budget row E5): both change the output by up to one Q19 unit, which is a behaviour change (decision D1 below). Nor is G1's drain work (dropped).

### 11.2 Mechanism

**G2-1a. A ramping lane is never fused with a steady one, and takes a per-gain steady shape.** `Pending` (`graph.h:369`) gets a `steady` flag computed where the lane is made (`graph.cpp:727-730`: `lv`, `rl`, `rr` are known; `steady = lv.start == lv.end && rl.start == rl.end && rr.start == rr.end`, three compares). `flush_main` fuses two lanes only when both are steady and of the same `centre` kind, as today's rule plus the steadiness; otherwise the lanes go one by one. A single ramping stereo lane whose level is steady and whose pan law has a steady unity side (the pan LFO's shape in the block after a step) goes through a new one-lane kernel, `kernel::gain_pan_r1(lane, acc_unity, acc_ramp, n)`: the level through `Gen<true>`, the unity side as the constant Q20 unity into `acc_unity`, the moving side through `Gen<false>` into `acc_ramp`; the call site passes the accumulators in the order the lane needs (left unity: `acc_main_[0]` first; right unity: `acc_main_[1]` first), so there is one kernel for both sides. A lane that fits no shape (the pan crossing 50 in the block, a moving fader, a ramping centre) goes through today's `gain_pan` / `gain_mac` as it does now. [est] The ramping lane from +6 to about +2 instructions a track-sample; the partner from +6 to 0 (it is now a plain steady single or pairs with the next steady lane). Bit-identical: the integer sums commute (the order of lanes into an accumulator does not matter, `kernels.h:98-101`), `gain_pan_r1` computes exactly `gain_ref` then `mac_ref` twice with the lane's own ramps (the unity side's `ramp_gain20(65536, 65536, i)` is the constant 2^20), and `strip_simd`'s own tails and alignment fallback are reused.

**G2-1b. The steady stereo pair, register-shaped (conditional on the disassembly).** If G2-0 shows at least two spilled values a pair of samples in `strip_pairs<NativeOps, 2, true, true>`, the two-lane steady stereo instantiation is reshaped to fit the registers, in C++ (no inline asm: the C5 lesson of OS-CPUOPT1 8.6), with the disassembly re-read after each change: the accumulators of MAIN are one array (`acc_main_[2][kBlock]`, `graph.h:455`), so the kernel may take one base and address the right side at `+kBlock` (a `static_assert` on the layout in graph.h and a dedicated entry `gain_pan2_main` that documents the contract, with its own test); the two scratch blocks are likewise one array (`scratch_[2][kBlock]`, `graph.h:452`, and `p[0].block` is always `scratch_[0]`, `p[1].block` always `scratch_[1]`: `graph.cpp:688,730-734`), so one base serves both lanes; the pan law gives each steady panned lane one unity side, so when both lanes have their unity on the same side that gain is one shared register (the mixed-side pair keeps the general form). Each of these removes one or two live values; the reshaped loop must show no spill and at most 30 instructions a pair in the disassembly, or the step is dropped and the measured count recorded. [est] saving = the spills found, 0 to 1 cycle a track-sample.

**G2-1c. A four-lane steady stereo form (parked, decision D4).** Halves the accumulator traffic a track-sample (from 1 to 0.5 instruction) but needs 12 live gains, two more scratch blocks (+256 B of SRAM_U) and a four-deep `Pending`; not built unless G2-1a+b miss the target by less than a quarter and the bench attributes the rest to the accumulator loads and stores.

### 11.3 The bit-exactness argument

`gain_pan_r1` on lane L equals `gain_pan_ref(L)`: the level stage is the same `level_pair` with `Gen<true>` (the level ramp is steady by the shape test), the unity side's product is `smlawb(2^20, y, acc)` = `acc + 16 * y` = `prod_q19(y, ramp_gain20(65536, 65536, i))` for every i, the moving side is `Gen<false>` as in every ramping kernel, and the tails and the alignment fallback are `strip_simd`'s. The pairing change in `flush_main` only changes which lanes share one pass over the accumulators; the sums into each accumulator are the same integers in a different order, modulo 2^32 (`add_wrap`), and `x` (the post-fader block) is written once by its own lane whatever the pairing. G2-1b changes the addressing, not the arithmetic. No smoother, ramp, `dirty_` or drain code is touched: the G1 invariant work is not needed.

### 11.4 Bench support (G2-0, behind `-DO3OS_BENCH`, 0 B in the release image)

1. **Paired kernel arms in `diag bench kernels`** (`platform/mk66/src/bench.cpp:481`, `run_kernels`): each arm is the production kernel on a fixed lane shape, 64 samples, the minimum of N blocks as every `BENCH` line (`bench_report.h:183`: `BENCH <name> <min> <count> avg <avg> max <max>`). Shapes: `k_gain_mac2_c` (A: two centred steady lanes, the off arm's shape), `k_gain_pan2_s` (B: two panned steady lanes, unity on one side each), `k_gain_pan_s` (C: one panned steady lane), `k_gain_pan_r` (D: one lane, level steady, one side ramping, through today's `gain_pan`: the ramping lane's cost now), `k_gain_pan2_r` (E: one ramping lane and one steady partner through today's `gain_pan2`: the dragged pair), and today's `k_gain_pan2` renamed `k_gain_pan2_rr` (F: everything ramping). After G2-1: `k_gain_pan_r1` (D') and, if built, `k_gain_pan2_main` (B'). The existing `k_gain_pan` / `k_gain_mac` lines stay. Each arm is followed by an equality line `BENCH strip_eq_<shape> OK|FAIL` that runs the `_ref` on the same random input and compares the accumulators and the block, the pattern of `cpuopt_eq_*` (`bench.cpp:527-528`, `:649`).
2. **The shape histogram**, compiled only with `O3OS_AUDIO_STRIP_STATS` (a definition on the audio library set from `O3OS_BENCH`, never in release): five counters in `Graph`, incremented in `run_tracks` / `flush_main` per lane: centre-steady, panned-steady, ramping, dragged-steady (a steady lane fused with a ramping one), single (unpaired); `bench stages` prints `BENCH strip_shapes c=.. p=.. r=.. d=.. s=.. blocks=..` after the stages, `diag cpu reset` zeroes them. It gives the per-block mix of shapes in the real scene: what G1-0's `strip_full` counter would have given, from the strips' side.
3. **The static disassembly count**: `arm-none-eabi-objdump -d` of the mk66 bench build, the loop of each `strip_pairs` instantiation (`gain_mac2`, `gain_pan2`, `gain_mac`, `gain_pan`; after G2-1 `gain_pan_r1` and `gain_pan2_main`): instructions a pair of samples and the number of stack loads and stores inside the loop, written into this section as [disasm].

Release `.elf` byte-identical before and after G2-0 (the G1-0 rule, kept).

### 11.5 Flash, SRAM_L, SRAM_U [est]

| | Release flash | SRAM_L (`.ramfunc_l`, 312 B free, limit +200 B) | SRAM_U | Bench image |
|---|---|---|---|---|
| G2-0 | 0 (byte-identical) | 0 | 0 | flash +1.0..1.5 KB, SRAM_U +24 B (counters), SRAM_L +40..80 B (the counting in `run_tracks`/`flush_main`; it must still fit) |
| G2-1a | +80..160 B (the kernel's load image) | `gain_pan_r1` +120..160 B; the `steady` test and the pairing rule +16..32 B; `strip_pairs<2, true, false>` becomes unreachable if the all-ramping pair goes through singles, -120..160 B: **net -30..+120 B** | 0 (`Pending` +1 B x 2, padded, inside `Graph`'s stack frame) | as release plus the arms |
| G2-1b | 0 ± 50 B | the reshaped instantiation replaces the current one: -40..+40 B | 0 | |
| G2-1c | +150 B | +150..200 B (over the limit with 1a unless something leaves) | +256 B (two scratch blocks) | parked |

The flash figures follow SRAM_L one to one (the hot code's load image). Every slice reports FLASH, SRAM_L, SRAM_U, SDRAM_STATIC from `ci.sh mk66` and `check_ramfunc` must pass.

### 11.6 Unit measurement and the go/no-go (tester, after the orchestrator's go)

Same verified config as bench image 2 (section 7, first paragraph): scene t-v16, 16 instruments FILTER On, LFO SPEED detent 10, song from order 0, paired windows compared at the same index, `bench stages 4096`, 14 windows an arm, the all-off drift check within 30 cycles, 0 underruns.

1. **G2-0 (bench image 3a):** `diag bench kernels 256`: the six arms' minima and `strip_eq_* OK`. Then the pan arm and the off arm with `strip_shapes` read per window. **Model check:** with `n_p`, `n_r`, `n_d` the per-block counts of the histogram's `p`, `r`, `d` lanes, `n_p x (B - A) / 2 + n_r x (D - C) + n_d x (E - B)` (per-block shape counts times the per-lane kernel differences; A..E are the 11.4 arms) must land within 20 % of the measured per-window `stage_strips` delta; outside that the model is wrong and G2 stops for re-attribution. **Go for G2-1** if the ramping share `n_r x (D - C) + n_d x (E - B)` plus the spill share (`(B - A) / 2` per lane above the 2.0-cycle floor, times `n_p`) is at least half of the delta; otherwise **no-go**, recorded as the floor.
2. **G2-1 (bench image 3b):** the same windows. **Pass:** the mean of the paired per-window `stage_strips` deltas of the pan arm at most **+0.9k cycles** (from +1,724; the dense windows reported too, from +2,973), the off arm's `stage_strips` not worse than bench image 3a's by more than 100 cycles (the centre form must not pay for the panned one), `render_total` of the pan arm down by at least 0.8k, every `strip_eq_* OK`, 0 underruns, the host hashes unchanged. A result between -0.5k and -0.8k is reported as a partial and the maintainer decides whether it merges; below -0.5k G2-1 does not merge and the measured per-shape numbers go to the cpu-budget as the floor.
3. Listening check in the HIL list: a pan-LFO'd pad at a high amount and a pan slide across the centre, no click, no level step (bit-identical by construction; the check is for the pairing rule's effect on nothing else).

### 11.7 Decisions on the open questions (recommendation first; decided by the spec session for the maintainer, 2026-10-16)

| # | Question | Recommendation | Decision |
|---|---|---|---|
| D1 | May G2 change the output bits (fused level x pan, SMLAD packing, cpu-budget E5) if the error is one Q19 unit? | No: this item's premise is bit-identical; an LSB change of the mixer is a product decision and E5 stays its own IDEA row. | **Bit-identical only.** E5 is not part of G2. |
| D2 | Change the pairing rule (a ramping lane never fused with a steady one)? | Yes: bit-identical by commutation; a dragged steady partner costs about 6 instructions a track-sample, the lost accumulator sharing 1. | **Yes** (G2-1a). |
| D3 | Allow a kernel entry that assumes the MAIN accumulators and the two scratch blocks are contiguous? | Only if the G2-0 disassembly shows at least two spills a pair in the steady stereo loop; then with `static_assert`s on the layout, a dedicated entry and its own test. | **Conditional** as stated (G2-1b). |
| D4 | Build the four-lane form? | Not in this item unless 1a+1b miss the target by less than a quarter and the bench attributes the rest to accumulator traffic; it costs +256 B SRAM_U and more SRAM_L than the limit allows with 1a. | **Parked inside the item**; reopened only on that evidence, with a size decision then. |
| D5 | Where the shape histogram lives? | Bench-only define on the audio library, printed by `bench stages`, zeroed by `cpu reset`; release `.elf` identical. | **Yes.** |
| D6 | The go/no-go figure? | The mean of the paired per-window `stage_strips` deltas over 14 windows (the Q2 figure is that mean); the dense windows reported, not the gate. | **The paired mean**, +0.9k. |
| D7 | Does G2 touch the drain (G1's `recompute_strip`) at all? | No: G1 is dropped; a strips-only change keeps the Level C surface to the kernels and `flush_main`. | **No.** |

### 11.8 Risks

1. **A no-go is the likely outcome if the surcharge is at the arithmetic floor** (11.1 item 1): accepted, and G2-0 costs 0 B of release flash and one tester session. The result is recorded either way.
2. **Pairing regression in another scene:** two steady panned lanes still fuse; a ramping lane now runs alone (its partner pairs with the next lane or runs single). A scene with many ramping strips at once (a Step with pan slides on 8 tracks) loses the fusion on those pairs for that block: [est] +1 instruction a track-sample on the dragged lanes it no longer has, against -6 on each; net gain in every mix. The host bench (`tests/bench/audio_bench.cpp`) gets the ramping pair row to show it.
3. **SRAM_L:** a new instantiation against 312 B free; the limit +200 B holds only if the all-ramping two-lane instantiation leaves (11.5). The engineer reports the map delta before the review.
4. **Compiler-dependent shaping (G2-1b):** a GCC update can bring the spills back. Mitigation: the bench arm B on the unit per image, the [disasm] count in this spec, no asm.
5. **Layout contract (D3):** `static_assert`s on `offsetof` and the array shapes, and the kernel test places its buffers exactly as the graph does.
6. Hardware verification: 11.6. The host and QEMU tests prove the bits, the unit proves the cycles.

## State for the implementer (G2; the G1 state is superseded)

**Parked (2026-10-17).** G2 is parked at the spec (decided by Fable for the maintainer, 2026-10-16; spec e69f87d merged in dev bb69f70). Do not start G2-0 from the items below. Reason: the spec's own floor of the pan arm's strips delta is about 1.19k cycles, above the +0.9k go/no-go target. Section 11.1, item 1 gives the arithmetic floor as 2.0 cycles a track-sample (2,048 a block at 16 voices, dense windows). Against a mean delta of +1,724, at most about 0.5k (0.2 % of the block) is left to gain. ODD-11 returns 25-35k. Revisit only if a pan-LFO scene is still over budget after ODD-11 and OS-CPULAYOUT.

The six conditions for resuming G2-0 (from the risk review):

1. The likely outcome is "at the floor". G2-1 stays behind the G2-0 gate.
2. Prototype the `gain_pan_r1` arm D' in the G2-0 bench, and gate on the average as well as the minimum.
3. Dispatch `gain_pan_r1` only on start == end == kUnity, with an explicit ramp argument or a swapped lane.
4. `strip_pairs<2,true,false>` stays reachable; the SRAM_L headroom is re-measured at 82a21d7 before G2-1.
5. Gate on the paired (pan - off) delta, re-baselined on a bench 3a image built from the same base.
6. Run `audio_graph_strips_test` under QEMU in G2-0; `diag cpu reset` is bench-only.

Items 1 to 10 below are the G2 state as written before the park.

1. Branch `feat/OS-CPUOPT2` (merged with dev 0c82737); Level C; stage explicit paths only; one commit per slice, `perf(audio): ... [OS-CPUOPT2]`.
2. Order: **G2-0 first** (bench arms, `strip_shapes`, `strip_eq_*`, the [disasm] counts written into section 11.3/11.4), release `.elf` compared, bench image 3a to the tester; **stop** for the model check of 11.6 step 1. G2-1 only after the go, in the order 1a, then 1b if the disassembly says so; 1c is parked (D4).
3. Files: `audio/include/o3os/audio/kernels.h` and `kernels_impl.h` (`Gen` and `steady` `:55-76`, `LaneGen` `:148-163`, `lane_pair` `:166`, `strip_pairs` `:185-215`, `strip_simd` `:216`), `audio/src/kernels.cpp` (the production entries `:134-149`), `audio/src/graph.cpp` (`run_tracks` `:647`, the `Pending` creation `:727-730`, `flush_main` `:747`), `audio/include/o3os/audio/graph.h` (`Pending` `:369`, the arrays `:452-456`), `platform/mk66/src/bench.cpp` (`run_kernels` `:481`, the `cpuopt_eq` pattern `:560`), `audio/CMakeLists.txt` (the bench-only definition), `tests/audio_kernels_test.cpp` (`:255-335`: the shape cases), `tests/audio_graph_strips_test.cpp` (new, label `audio`, hashes recorded on the unmodified tree in G2-1's first commit), `tests/bench/audio_bench.cpp` (the ramping-pair row).
4. The shapes to cover in the kernel tests, on `EmuOps` (host) and `NativeOps` (QEMU: `ci.sh qemu` runs `qemu_audio_kernels`): level steady with one unity side and one ramping side, both orders of the accumulators; both sides ramping (a crossing); level ramping with the pan steady; a centre lane next to a panned one; odd `n`; a misaligned block; and, if D3 is taken, the contiguous-layout entry with buffers placed exactly as `Graph` places them.
5. The graph-level differential test: the rig of `tests/audio_graph_test.cpp`, 13 strips, a fed signal, a fixed-seed sequence of `set_voice_pan` (ramp 0, 1, 128, 4096), `set_track_pan`, `set_track_pan_mod`, volume, mute, solo, route, two strips crossing 50 in one block, alternating left/right pans (the mixed-side pair), a strip at exactly 50 beside a panned one, a ramping strip beside a steady one; FNV hashes of the TX block and the strip gains per block, 60,000 blocks; equal before and after G2-1.
6. Bit-identical means bit-identical: a test that compares with a tolerance is a wrong test here. No change to smoothers, ramps, `dirty_`, `drain`, `recompute_strip` (D7).
7. Every kernel entry stays `O3OS_AUDIO_HOT`; nothing hot calls flash (`check_ramfunc`); no allocation, no `double`, no libm, bounded loops; the lanes stay written out, not looped (`kernels_impl.h:144-147`: the -Os lesson).
8. Report per slice: FLASH, SRAM_L (limit +200 B against 312 B free), SRAM_U, SDRAM_STATIC from `ci.sh mk66`; the [disasm] instruction and spill counts per instantiation; the release `.elf` comparison for G2-0.
9. Bench-only code only under `O3OS_BENCH` / `O3OS_AUDIO_STRIP_STATS`; the shape counters are plain `std::uint32_t` incremented in the render context, read by the main loop (one writer: the usual bench-counter protocol).
10. Docs after the unit run (G2-2, doc-keeper): the cpu-budget rows (the drain floor +655; the strips result or floor), the measured numbers in 11.1 and 11.6 of this spec, the HIL listening step. M1 and G1 are not reopened by this item.
