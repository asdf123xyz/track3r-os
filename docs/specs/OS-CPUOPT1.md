# OS-CPUOPT1: CPU optimisation of the live LFOs (OS-LFOLIVE) and the loop player (OS-LOOPER L1)

Status: **phase 2, implemented on `feat/OS-CPUOPT1` (2026-10-16), waiting for the unit bench (section 8); phase 1 (analysis and plan) is sections 1..7.** The risk review and the unit baseline come next, then the implementation · Level C (audio render path, SRAM_L budget) · **No behaviour change**: every item is bit-identical to what the unit plays today, proved by an old-versus-new equality test, or it is not done · Follows `docs/specs/OS-LFOLIVE.md`, `docs/specs/OS-LOOPER.md`, `docs/architecture/cpu-budget.md`, skill `rt-optimize`.

Classification: **new Community OS behaviour** is unchanged by this item (no parity claim is touched). Evidence markers: [unit] measured on the unit (tester, v0.6.0), [dis] read in the disassembly of `build-os-ci-mk66/platform/mk66/o3os.elf` (dev 604d721, arm-gnu 15.3), [est] estimated from the instruction count and the M4F timing rules, to be confirmed by the bench of section 5. Cycles are M4F cycles at 180 MHz; one block is 64 frames = 261,120 cycles, 1 % = 2,611 cycles.

## 1. Targets and the unit numbers we start from

| Target | Unit v0.6.0 [unit] | Goal |
|---|---|---|
| LOOPER L1, a loop playing | +1.70 / +0.86 / +2.02 % of the block (noise ±1.2 pp), about 2.2k..5.3k cycles | below 0.5 % (1.3k cycles), design target 0.3 % |
| LFOLIVE, worst block | max 136k -> 154..158k cycles, of which the filter alone is +12..17k; the LFO's own share is about 1..3 pp (2.6k..7.8k cycles) | below 1 % (2.6k cycles) in the worst block, with the filter ON in both arms |

The tester is taking a paired baseline (10+ alternating windows). Its numbers replace the [unit] column and fix the pass thresholds of section 5; nothing below depends on them except the order of the work.

## 2. Findings: where the cycles go

### 2.1 Loop player (`audio/src/loop_player.cpp`, `audio/include/o3os/audio/loop_player.h`)

The playing path is `Graph::mix_extras` (`graph.cpp:573`, flash, once a block) -> `LoopPlayer::service` (`loop_player.cpp:121`) -> `serve` (`:131`) -> `run` (`:66`) -> the lambda `body` (`:85`). Everything is flash code at **-Os** (`audio/CMakeLists.txt:39-48` asks -O3 only for the send delay, the reverb and the limiter).

1. **The inner loop is not a register loop.** `body` is a lambda that captures `pos`, `g`, `o` by reference and GCC at -Os did not inline it [dis]: `LoopPlayer::run(...)::{lambda(unsigned long, long)#1}::operator()` is a separate function at `+0x00..0xf2`, and its frame loop (`+0x60..0xa2`, 23 instructions plus the 3-instruction head `+0x20..0x28`) **loads and stores `pos`, `g` and `o` through the captured references every frame**: 7 `ldr` and 4 `str` of stack words, one `ldrsh` of the take (SDRAM), two read-modify-write `ldrsh`/`ssat`/`strh` for L and R, and two taken branches. About 26 instructions, roughly 45..50 cycles a frame [est] = **2.9k..3.2k cycles a block = 1.1..1.2 %**, which matches the unit's mean of 1.5 % and is the whole of the overrun. The estimate in `OS-LOOPER.md` (12..16 cycles a frame, 0.3..0.4 %) assumed a register loop.
2. **Halfword loads from the cached SDRAM alias.** The take lies in the sample arena, which the core reads through the ICode alias 0x0800_0000 (`sdram_view.h`: LMEM code cache, 16-byte lines, 8 KiB shared with flash code, **about 10 cycles a load on a hit**, 28 uncached). The loop does one `ldrsh` a frame: 64 x 10 = 640 cycles of the 3.0k are the loads, and a block touches 8 new lines (8 x about 30 = 240 cycles of line fills) [est].
3. **Two saturating read-modify-writes a frame** where one packed `qadd16` does both channels (the TX block is interleaved L,R; the same value goes to both). The gain is at most Q16 unity (`LevelCurve` Og1114 100 = `kUnity`, master gain <= `kUnity`, `mul_q16` clamps), so `v = (p * g) >> 16` is inside int16 and `qadd16(out, v|v<<16)` equals `ssat16(o + v)` per lane exactly.
4. **Per-block overhead** `service()`: three atomic read-modify-write operations (`fetch_and`, `exchange`, `fetch_or`/`fetch_and`) at about 10..15 cycles each with LDREX/STREX, even when no request waits [dis `service`], about 40..60 cycles. 0.02 %. Armed but stopped, a Capturing or non-running Ready player costs this and `run`'s early return, about 100 cycles: below the noise.
5. **Record path (`RecTap::push`, `taps.cpp:151`)**: hot, one `kernel::mono_mix` (SIMD) of 64 frames into the arena: not a hot spot (about 300 cycles with the SDRAM write-through writes) [est]. Left alone.

### 2.2 Live LFOs (`audio/src/voices.cpp`, `audio/include/o3os/audio/voices.h`)

Six modulators (amp = tremolo, pan, finetune, cutoff, WT position, grain position) share the generator `ModGen` and `flfo_level` (`voices.cpp:833`, SRAM_L, 0x1b0 bytes, **about 45 instructions, 75..90 cycles an evaluation** [dis]: the 64-bit `umlal` tick maths, a `udiv` (2..12), the float conversion and clamp with `vcmpe`/`vmrs`). Per voice and block:

| Where | LFO work | Evaluations | Est. cycles per voice-block |
|---|---|---|---|
| `mod_block` (`:1755`, flash, called from `render` `:654` once a block per voice with a modulator) | pan 1, finetune 1 (+ `repitch`: `pitch_inc` `:1416`, 64-bit scaling, 0x344 bytes, about 250..300), position 1 (+ `move_position` `:1740`), tremolo 4 grid targets (`:1787-1789`, each `flfo_level` called separately with its own prologue and 64-bit tick maths) | up to 7 | all six about 1.1k; tremolo 4 x 85 = 340, finetune about 400, pan about 130, position about 160 |
| `trem_span` (`:889`, SRAM_L, 0xc4 bytes) | the multiplier per sample: `ldrsh`, `mul`, `asr`, `strh`, `cur += step`, loop test, about 7 cycles a sample, in chunks of 16 | 0 | 64 x 7 + 4 chunk set-ups = about 500 |
| `filter_mod_span` (`:865`, SRAM_L, 0x13a) | cutoff LFO every 16th sample: `flfo_eval` (a call + wrapper, about 90), clamp, `svf_set_cutoff` (`svf.h:68`, about 35), then **`filter_span` called per chunk** (`:704`, 0x14a bytes: reloads the five Svf floats, a type switch, stores them back: about 40 cycles of overhead a chunk) | 4 | 4 x (90 + 15 + 35 + 40 + loop) = about 600; the same voice without the LFO is one `filter_span` call |
| `render` (`:654`) | the three tests per voice | 0 | about 15 |

So **a voice with all six LFOs costs about 2.3k cycles a block (0.9 %)**, and one with the tremolo and cutoff about 1.4k. The tester's 1..3 pp is 3..9 such voices. The costs that do not need to exist:

- **The cutoff LFO splits the filter into four 16-sample calls** and re-reads and re-writes the filter state each time (about 120 cycles a voice-block of pure overhead).
- **The tremolo's four `flfo_level` calls** each redo the shared work (`tick_q_ + samples_on * tick_rate_q_`, clamp, `tick_steps_` base) and the prologue/epilogue (about 25 of the 85).
- **`repitch` every block** for a finetune LFO even when the whole-cent value it quantises to (`mod * 100` cut toward zero, `:1771-1773`) did not change: `pitch_inc` is a pure function of (`pnote`, `fine`, `q8` of the slide ramp), so an unchanged triple gives an unchanged `inc`. At LFO speeds slower than a few Hz most blocks repeat.
- **`trem_span` runs 7 cycles a sample** where the same arithmetic is two samples per iteration in `smlawb`/`smlawt` (`(out * m2) >> 16` with `m2 = (cur >> 8) << 1` equals `(out * (cur >> 8)) >> 15` bit for bit), about 4..4.5 cycles a sample.
- **The pan modulator already skips the strip when its value did not change** (`Graph::set_voice_pan`, `graph.cpp:277`): nothing to gain there.

### 2.3 The filter under the LFO (shared code, listed because it dominates the worst block)

`svf_run` (`svf.h:107`, inlined into `filter_span`) is **28 instructions a sample** [dis, `filter_span` +0x30..+0x8a], of which ten are `vmla`/`vmls`/`vnmls`. On the M4F a non-fused multiply-accumulate is **3 cycles** and the chain `lp -> hp -> bp` is serial, so a sample is about 45 cycles [est] = 2.9k cycles per filtered voice and block, which matches the tester's "+12..17k for the filter alone" (4..6 voices). Issuing the product as `vmul` (1 cycle, off the critical path) and the sum as `vadd` (1 cycle) gives the **identical float result** (a non-fused multiply-add is a multiply rounded, then an add rounded) with a dependent chain of about 12..14 cycles a sample. It is the largest single lever of the whole list, but it is the filter, not the LFO; it is in the plan as an item of its own (C5) with its own switch so the maintainer can take it or leave it.

## 3. Plan, ranked by saving per risk

Savings are per block, [est] until the bench (section 5) confirms them. "Exact" = bit-identical output, proved by an old-versus-new test with random data (section 4). No item changes a format, an id, a parameter range, or the control rate (the 16-sample cutoff grid and the once-a-block evaluations are this OS's control rate and stay).

| # | Item | Saves | Risk | Placement / bytes |
|---|---|---|---|---|
| **L1** | Rewrite `LoopPlayer::run`: no lambda, locals in registers, constant-gain pass and ramp pass as two loops, the take read as **words** (two frames; a one-sample head when `pos` is odd), the add into the TX block as one **`qadd16`** of the duplicated value (new `NativeOps::qadd16`, portable model for the host), `loop_player.cpp` compiled -O3 (flash, like the send delay) | 3.0k -> about 0.6..0.8k: **-2.2k..2.4k (about 0.9 pp)**, the loop at about 0.25..0.3 % | low: exact; gain bounded by construction (`static_assert` on `kUnity`, a clamp in `service`) | flash only (+about 150 B); SRAM_L 0 |
| **L2** | Read the take through one **LDM burst** per 8 words into a small SRAM buffer (a 128-byte stack buffer) when the bench shows the cached alias beats 10 cycles a word | -0.2k..0.3k [est] | low | flash; only if the bench (K3) shows it |
| **L3** | `service()` takes the request atomics only when `go_` says a request waits (relaxed load first), sets `kGoActive` only when it changes | -40..60 cycles | low: same protocol (the main loop sets `req_` before `go_`; a request landing after the check is taken next block) | flash |
| **C1** | Tremolo/pan/finetune/position: one `lfo_levels` helper evaluates the four grid targets with the tick base computed once (`flfo_level` split into the shared part and the shape) | -100..120 per tremolo voice | low: exact | flash (`mod_block` is cold); `flfo_level` stays hot and unchanged for the cutoff |
| **C2** | `trem_span`: two samples an iteration with `smlawb`/`smlawt` and a word load/store (one-sample head and tail for an odd position) | -150..200 per tremolo voice | low: exact | SRAM_L: +0..40 B (the loop is a little larger than now) |
| **C3** | `filter_mod_span`: the four chunks run inside one function with the Svf state in registers (coefficient swap between chunks), `flfo_eval` inlined | -150..200 per cutoff-LFO voice | medium: touches the shared filter loop (the test of section 4 covers all three types, resonance, silence, NaN-free) | SRAM_L +50..100 B (see budget) |
| **C4** | Finetune: remember the whole-cent value (and the slide state) the voice's `inc` was made for; skip `repitch` when equal | -250..300 per finetune-LFO voice-block that repeats (slow LFO) | low: exact; key = (`fine`, `q8`, `pnote`, `slot`), cleared at note start and any live edit of tune/rate | flash; Voice +4 B each (12 voices) in SRAM_U |
| **C5** | `svf_run`: product as an opaque `vmul` then `vadd` (an empty `asm` with a `"+t"` constraint stops GCC fusing back into `vmla`); same operation order, same rounding | **-1.2k..1.6k per filtered voice-block** (45 -> about 20 cycles a sample) | medium: shared by every filtered voice, not only LFO; exact by construction, proved by the test; the claim "VMLA 3 cycles, VMUL/VADD 1" is the TRM's and must be seen in `diag bench` before the change merges | SRAM_L +0..120 B (explicit ops; may shrink if the three types share a loop) |
| **C6** | `move_position` for WT/grain: skip the pointer maths when the quantised window did not move | -60 per position voice | low: exact | flash |

Expected result [est]: LOOPER about 0.25..0.35 % (L1 alone is enough; L2/L3 are polish); the LFO's own share of a six-LFO voice from about 2.3k to about 1.5k cycles (C1..C4, C6), i.e. the tester's 1..3 pp becomes 0.6..1.8 pp, **below 1 % in the worst block for up to about 6 such voices**; with C5 the worst block loses 1.2k..1.6k per filtered voice on top (the filter's own +12..17k becomes about +6..8k), which is what actually puts the worst block under the line when the filter is on. Without C5 the 1 % goal in the worst block is met only if the tester's LFO share is at the low end of 1..3 pp. Order of work: L1 (largest, lowest risk, resolves the looper), C5 behind its bench check, C3, C2, C1, C4, then the rest.

Not planned, with the reason: a coarser control rate for the cutoff or a cheaper shape (audible, and the 16-sample grid is this OS's control rate); skipping voices whose LFO amount is 0 (rare, would add a test to every evaluation); moving `flfo_level` evaluations to a per-block table shared by voices (voices differ by `l_shift` and period; the saving is the 10 cycles of the 64-bit maths, folded into C1 instead); SIMD for the SVF (it is a feedback loop, one voice has nothing to vectorise; two voices at once would, but the pool renders voice by voice); a lock-free restructure of the looper clock (not a cost).

## 4. Tests and equivalence (host, no hardware)

- **Old versus new, bit for bit**, with the old implementation kept as a reference copy in the test file (the shape `tests/audio_filter_mod_test.cpp` already uses): `tests/audio_loop_player_equiv_test.cpp` (L1/L2: random take lengths, positions odd and even, restarts mid-block, stop/fade, ramps in and out, hold frames, gains 0..unity, TX blocks with saturating content, 10^5 blocks), `tests/audio_trem_equiv_test.cpp` (C2: random multipliers, odd starts and tails), the existing `audio_svf_test`/`audio_filter_mod_test` plus a new random old-versus-new in `tests/audio_svf_equiv_test.cpp` for the three types, all resonances, every cutoff index, long runs (C3, C5), `audio_lfolive_test` and `audio_mod_test` unchanged and green, and a new `repitch` skip test (C4: the voice's `inc` after N blocks with and without the cache is identical across random LFO speeds, shapes, slides and live edits).
- **Host NativeOps `qadd16`** model checked against a scalar lane-by-lane reference over all 2^16 lane pairs at the edges.
- **Existing oracles stay unchanged**: `audio_loop_test` (the clock against the real sequencer), `audio_wtlfo_test`, `audio_lfolive_test`, `audio_bounds_test`, the QEMU `audio` suites. A golden or a hash that changes is a failure of this item, not something to regenerate.
- `-Wdouble-promotion` clean; alignment declared (`alignas(4)` and a `static_assert` on the stack buffer of L2; the word loads of L1 prove the pointer parity at run time and use `memcpy` for the head).

## 5. Unit measurement plan (the tester runs it after the go; Level C)

Same build id and commit for A and B except the item under test; bench first (cheap, deterministic), then the paired song windows.

1. **New `diag bench` kernels** (`platform/mk66/src/bench.cpp`, next to `k_mono_mix`, main loop only, minimum over N blocks as the existing ones): `k_loop_run` (one block of `LoopPlayer::run` on a 64 KiB take in the arena, constant gain, and a 32-frame ramp), `k_trem_span`, `k_svf_run` (low/high/band, 64 samples, resonance 3), `k_flfo_level`, `k_mod_block` (a voice with all six LFOs). Each prints min/mean/max cycles for old (the image of dev) and new. Pass (as built, section 8): `loop_new` <= 1.0k cycles and below half of `loop_ref`; `svf_low` <= 0.8 x `svf_ref_low` (the review's 34 against 45 cycles a sample; confirms the VMLA premise; below that C5 is dropped) with `BENCH cpuopt_eq_svf OK` and `cpuopt_eq_loop OK`. (`k_trem_span`, `k_flfo_level` and `k_mod_block` were not built: C2 was dropped and the evaluation is private to the pool; the paired song windows carry the LFO items.)
2. **K3**: the same `k_loop_run` with word loads, with an LDM-8 burst, and with halfword loads (kept as `k_loop_run_h`), to decide L2.
3. **Paired song windows** (the tester's protocol, 10+ alternating windows, `diag cpu reset`, `diag cpu perf off`, 60 s each, `diag cpu` render mean/max and `diag bench worst`): LOOPER arms (loop playing / armed stopped / no loop) on the reference project t-v16; LFOLIVE arms (filter ON in both; LFO off / amp+pan+finetune+cutoff LFO / all six on the WT and grain instruments). Report the mean and worst-block differences with the noise of the paired baseline; goals of section 1.
4. Ear: `qa_lfolive.json` and the loop steps of `qa_record.json` unchanged (the output is bit-identical, so this only confirms nothing was lost; no click at the loop restart, no new tick at a stop).
5. Report the FLASH / SRAM_L / SRAM_U / SDRAM_STATIC deltas from `build-os-ci-mk66/ci-build.log` for every commit.

## 6. Placement and budget (`rt-optimize`, hot budget)

`.ramfunc_l` has **312 B** free of the 27 KiB hot budget after OS-MODPITCH (it was 24 B before `ModuleVoices::start_note` moved to flash). Items L1..L3, C1, C4, C6 are flash code (cold callers, called once a block or a few times a block): SRAM_L unchanged. C2/C3/C5 change code that already lives in SRAM_L (`trem_span`, `filter_mod_span`, `filter_span`): net budget +50..250 B, to stay inside **+200 B** in total (the hard limit of this item; the rest of the 312 B stays free). If C5's explicit operations or C3's fused chunks do not fit, the three filter-type loops of `filter_span` are merged into one loop with the output sum chosen once per call (the sum differs only in the last line), which frees about 200 B. Nothing in the hot path calls into flash that did not already (`tools/check_ramfunc.py` unchanged). No `double`, no allocation, no SD access, loops bounded by the block.

## 7. Risks and decisions for the review

- The M4F timing claims (VMLA 3 cycles versus VMUL+VADD, the cache hit cost per word load, LDM burst behaviour) are the only places where an estimate carries a design decision; **each is checked by a bench kernel before the item merges** and an item that does not show its saving is dropped, not kept for its tidiness.
- C5 is outside the letter of "LFO live path" (it is the filter loop). It is the biggest lever and exact; the orchestrator decides whether it is part of this ID or its own.
- C4's cache key must include everything `pitch_inc` reads (`pnote`, `s.tune`, `s.rate_q28`, `s.wt_size`, `fine`, `q8`); a live edit of tune, rate or the sample (`apply_param`) clears it. The review should check that list against `pitch_inc`.
- L1 relies on gain <= unity; a future "LOOP VOL above 100" or a master boost must revisit it (a `static_assert` and a comment at the clamp).
- No format, ABI, id or QA-visible behaviour changes. QA rows are not expected; if the implementation adds a bench kernel with a console line, its row goes to `docs/qa/catalogue/instrument.md` (QA-INS-150..159) and `record.md` (QA-REC-150..159), HIL steps in `qa_lfolive.json` and the looper's record file.
- Out of scope, untouched: OS-BUG-PASTE24 (the volume law in `voices.cpp`, `sample_bank.cpp:477`, `app_main.cpp:1212-1220`, the Amiga engine). Items C2/C3/C5/C4 edit other functions of `voices.cpp` and `svf.h` than the volume code; the engineer rebases on lane B's change if it lands first.

## 8. Implementation and the unit baselines (2026-10-16)

### 8.1 Baselines from the unit (v0.6.0, tester, paired alternating windows; they replace the earlier +0.78 % and the +1.70 / +0.86 / +2.02 %)

| Baseline | Arms | Result |
|---|---|---|
| A, LFOLIVE | scene t-v16 song F2; FILTER On on all 16 instruments in both arms; LFO Saw / 4 steps / 100 % on Pan, Cutoff, Finetune of all 16, WT Position on instruments 4 and 6; no volume LFO; 14 windows of 4096 blocks per arm | window mean ON 101,577 (sd 17,294) against OFF 81,669 (sd 15,046): **+19,908 cycles = +7.62 %** of the block (paired sd 0.94 pp); `cpu` max +26..27k (+10 pp); over-budget blocks 2 and 2 against 0 and 0; 0 underruns |
| B, LOOPER | 4-bar loop, dense pattern 9, 10 windows per arm | **+3,905 (+1.50 %)** and **+2,958 (+1.13 %)** while the transport runs; loop present and stopped **+520 (+0.20 %)** |

A is **1.24k cycles per voice-block for pan + cutoff + finetune** (16 voices), which agrees with the section-2.2 model (cutoff about 610 + pan about 250 + finetune about 400 = 1.26k). B agrees with the L1 finding (45..50 cycles a frame, 3.0k a block). One correction to 2.1: a cached-alias SDRAM load costs no more than an SRAM one on the unit (`diag bench mem`: the loop overhead of 10 is the whole of the 10.0 figure), so the loop's cost is code, not the take's reads; the line fills (28 cycles a 16-byte line) remain.

### 8.2 What was built, what was dropped, and why (one commit each)

| Item | Result | SRAM_L (hot, bytes, dev 604d721 = 49,608) |
|---|---|---|
| bench | `diag bench cpuopt`: old reference copies and production, cycles, and `BENCH cpuopt_eq_loop` / `cpuopt_eq_svf` (OK or FAIL: the old loop against the new one on the unit, bit for bit) | 0 (flash) |
| L1 | `loop_mix::take` (`audio/loop_mix.h`, flash, -O3): registers, two frames a word, one `qadd16`, gain clamped to unity in `LoopPlayer::serve`; hold and silence paths unchanged | 0 (flash) |
| C1 | `flfo_levels(n, stride)`: the tremolo's four grid levels in one call, tick base and shape read once | +104 |
| C2 | **built and dropped** (reverted): the tremolo loop at two / four samples an iteration with SMULWB / SMULWT. The disassembly showed 12.5 cycles a sample inlined (the compiler spills the end pointer and the step to the stack) and about 9.5 as a function of its own, against about 10 for the scalar loop it replaces, for +56..+296 B of SRAM_L; the tremolo is not in the measured scene. Not worth the bytes | 0 after the revert |
| C3 | `filter_run`: the plain and the modulated filter in one function (three type loops once, the 16-sample chunks inside, the filter state in registers between chunks); `Svf::idx` + `svf_set_cutoff` keeps `f` while the cutoff stays in the same table index (f depends on the index alone) | about +24 |
| C4 | `repitch` keeps `inc` while (slide offset, cent) are the last ones; cleared by the note's start and by a Tune / FineTune edit. The first key (note, tune, rate, window size too) overflowed the `voice_state` row of SRAM_U (16 voices, 28 KiB, full) and was cut to the two values that change in a block | 0 (flash); Voice +8 B (SRAM_U, inside the row) |
| C5 | **dropped and removed** (the `svf_mul` opaque-product asm and its switch are gone from `svf.h`): the unit measured 0.916x of the plain loop against a 0.8x gate (section 8.6) | 0 |
| L2, L3, C6 | dropped (review: L2 only if bench K3 shows a gain, L3 negligible, C6 breaks `apply_position`'s overwrite of `v.data` / `v.gnew`) | |
| total | | **+192 of the +200 limit** (312 B were free) |

Tests: `audio_loop_mix_test` (old loop against the new one, 6,000 random runs, odd and even takes, ramps, full scale; `qadd16` model against the instruction), `audio_lfo_equiv_test` (six destinations x five shapes x speeds x amounts x running / stopped x five block splits, Slide FX and live Tune / FineTune edits, every filter type; six FNV hashes of everything the outside sees, **recorded on dev 604d721 before any change** and still equal after C1, C3 and C4), `audio_svf_cache_test` (a cutoff walk against a fresh set, NaN and out-of-range included). (C5's equality was to be proved on the unit by `BENCH cpuopt_eq_svf`; C5 is dropped, section 8.6; the bench line stays and now compares the plain loop with its reference.)

### 8.3 Re-estimate for the baseline-A scene (16 voices x pan + cutoff + finetune, plus WT on two) and what it costs after

Items that touch this scene: C3 (cutoff chunks and coefficient cache) and C4 (finetune); C1 and C2 are tremolo only and the scene has no volume LFO (C1 is kept for the tremolo; it costs +104 B and saves nothing here). Per voice-block [est]:

| Part | Now | After | Saved |
|---|---|---|---|
| cutoff LFO (4 evaluations, 4 coefficient sets, 4 filter chunks) | about 610 | about 330 (chunks in registers -150..170; coefficient kept in the same index, about 2 of 3 evaluations at a 100 % saw over 4 steps: -80..110) | about 280 |
| finetune LFO (1 evaluation + `repitch`) | about 400 | about 285 (a whole cent changes in about 4 of 10 blocks at this rate: -115) | about 115 |
| pan LFO | about 250 | about 250 | 0 |
| total | about 1,260 | about 865 | **about 400 (-32 %)** |

For the scene: **+19.9k (+7.6 %) becomes about +13.9k (+5.3 %)**, 16 voices x 3 LFOs. The filter itself is in both arms and is where the larger saving is: C5 takes the SVF from about 45 to about 28..34 cycles a sample [est], 0.7..1.1k a filtered voice-block, **11..18k for the 16 voices (4.3..6.9 pp of the block) in both arms**, so the scene's absolute render (101.6k ON) falls to about 84..90k. What does not move: the three evaluations of the pan / finetune / position generators (about 85 cycles each, about 40 % of them loads of generator and tick fields) and the cutoff's four; a possible next item (C7, not built) is a per-block tick base shared by all generators of all voices (about -25 cycles an evaluation, -150 a three-LFO voice-block, medium risk: block-splitting events and `tick_q_` advance).

### 8.4 A realistic target, for the maintainer

The "LFOLIVE < 1 % in the worst block" target is per feature and cannot hold at 16 voices x 3 LFOs (it is 0.3 pp a voice-block for the triple after C3 / C4, about 0.5 pp now). Proposed targets, per LFO voice, measured as baseline A:
- **Cost of a voice with pan + cutoff + finetune (filter on in both arms): at most 0.9k cycles a voice-block (0.35 pp), from 1.24k now (0.48 pp).** This scene then lands at about +5.3 % (16 voices) instead of +7.6 %.
- A single LFO on one voice at most about 0.2 pp; the "below 1 % in the worst block" statement applies to up to three voices with the three LFOs (0.35 pp each).
- Absolute: the A scene's ON arm (all 16 voices filtered and modulated) at most **90k cycles mean** (from 101.6k) with C5, and the `cpu` max below 150k (from 171k).
- LOOPER (baseline B): at most 0.5 % while the transport runs (from +1.1..1.5 %), expected 0.25..0.35 % after L1; stopped unchanged (+0.2 %).
If C5's bench fails its gate the ON arm stays near 100k and only the LFO's own share (about -5.5k) is gained.

### 8.5 Size deltas of the fold (mk66, against origin/dev aafdcb8 built the same way: FLASH 751,308, SRAM_L 49,608, SRAM_U 136,760, SDRAM_STATIC 113,728 B)

| Region | Branch | Delta | Where it comes from |
|---|---|---|---|
| FLASH | 757,008 | **+5,700 B** | `diag bench cpuopt` about 3.9 KB (`cpuopt::run` 3,252 B, `ref_loop_block` 242 B, `ref_svf` x3 388 B: the old reference copies ship in the image on purpose, for the A/B and the equality lines on the unit; they can be dropped once the unit has agreed) and about 1.8 KB for the rest (`loop_player.cpp` whole-file -O3 and `loop_mix::take`, `filter_run`, `flfo_levels`, `repitch`) |
| SRAM_L | 49,800 | **+192 B** (limit +200) | C1 +104, C3 about +24, C5 about +64 |
| SRAM_U | 136,760 | 0 in the map: `g_voices` sits in the fixed `voice_state` row | the row's use grows by about 192 B: `Svf::idx` +4 and `Voice::rp_*` +8 for each of the 16 voices (the row's static_assert passes) |
| SDRAM_STATIC | 113,728 | 0 | |

### 8.6 The first unit run of the bench image (79260ba, three runs at 64 and 256 blocks) and what followed

- **L1 confirmed on the unit:** `cpuopt_eq_loop OK`; `loop_ref` 3,173 min -> `loop_new` 893 (0.28x); with a 32-frame ramp 3,166 -> 1,006.
- **C5 dropped.** `svf_low` 2,598 min against `svf_ref_low` 2,837 = **0.916x** (0.90x average), short of the 0.8x gate; `svf_high` 2,697, `svf_band` 2,829. The premise (VMLA on the chain costs most of the loop) was wrong: the chain is not the limit. The switch and the asm were **deleted** afterwards (a switch at 0 compares plain code with plain code); `svf.h` is dev's again, byte for byte.
- **`cpuopt_eq_svf FAIL` was the reference, not the loop.** `platform/mk66/src/bench.cpp` was compiled without `-ffp-contract=off` (the audio library has it), so GCC turned the reference copy's plain products into **VFMA** (one rounding), and the bench compared the new unfused loop with a fused filter the voices never ran. C3 is not in that path (the bench uses neither `filter_run` nor the coefficient cache). Evidence: `tests/audio_svf_split_test.cpp` on the Cortex-M4F under QEMU (which emulates the VFP operation by operation): with the reference left fused it reports 73 sample and 1,356 state mismatches over 180 runs; with the reference built `-ffp-contract=off` (as bench.cpp now is, and every QEMU test target since the fold of round 2) it reported **0 and 0, with the split loop switched on and off** (that test, `audio_svf_split_test.cpp`, was removed with C5). The off-unit test therefore sees this class of error. C3's bits are covered by `audio_lfo_equiv_test` (hashes recorded on dev, host x86) and by construction (`filter_run` runs the same `svf_run` over the same chunks; the coefficient cache skips only an identical computation).

### 8.7 Why C3 and C4 delivered about 3 % and not 30 % (bench image 79260ba, baseline A: ON 100,985 against OFF 81,724 = +19,260 cycles, +7.38 %; v0.6.0 +19,908)

The saving was 592 cycles in 16 voice-blocks = **37 cycles a voice-block against the 400 estimated.** Honest reading, in order of weight; none of it is measured yet and the next step is measurement, not more code.

1. **The estimate counted arithmetic; the cost is probably not arithmetic.** C3 removes three filter-state reloads and about 80 cycles of coefficient work; C4 skips a `repitch` in about 4 of 10 blocks at this LFO rate (a whole cent changes about 0.6 times a block for a saw over 4 steps at 120 BPM). Both are tens of cycles, and the measured 37 is the same order. They are correct and cheap, and they are small.
2. **C1 was a net cost in this scene, and is reverted.** It made every single evaluation go through the n-level loop (a 64-bit stride product, a float on the stack, a longer function): about 10-20 cycles on each of the 6 evaluations a voice-block of this scene (cutoff 4, pan, finetune), i.e. 60-120 cycles a voice-block that cancel most of C3 and C4; the scene has no tremolo, which was the only user that gained. `a10819c` restores `flfo_level` (SRAM_L -104 B).
3. **Where the 1.2k probably is** (hypotheses, each testable): **(a) instruction-cache misses on the flash-resident per-voice path.** `mod_block` (596 B), `pitch_inc` (836 B), `repitch`, `move_position` run -Os from flash once per voice per block, between the voices' renders, which stream the samples through the same 8 KiB code cache (the ICode alias, 16-byte lines, about 25-30 cycles a line): a cold `mod_block` is about 37 lines = 900-1,000 cycles if it is evicted every time. That is the right size for the whole 1.2k and explains why arithmetic edits do nothing. **(b) the pan LFO's strip work in the mixer, not in the voice:** every block a moving pan sets the strip dirty and `Graph::recompute_strip` recomputes the whole strip (6 `curve_.q16` table interpolations, about 9 `mul_q16`, 9 smoother retargets, the pan law), about 400-500 cycles, to change two pan gains; the fader and the sends have not moved. That cost is in the "strips" stage and appears under no LFO function. **(c) the cutoff path** is in SRAM_L and is the smallest part (4 evaluations, 4 coefficient sets: about 250 now).
4. **What would cut it** (to be built only after the attribution below): **G1**, a pan-only strip update: when only `pan_offset` moved and the strip's smoothers are idle (a `retarget` to the target a smoother already stands at is a no-op, `smoothing.h:73-82`), skip the fader and send recomputation, bit-identical, estimated -300..-400 a pan-LFO voice-block (-5..-6k in scene A, 2 pp). **M1**, one pass of `mod_block` for all voices before any voice renders (the code stays warm across voices instead of being evicted by each voice's sample reads), estimated -300..-600 a voice if (a) holds; bit-identical except that the shared random-shape generator would be drawn in a different voice order (a random LFO is random, but it is a change; to be decided), or exact if the pass skips voices with a random shape. **M2**, a code layout that keeps `mod_block`/`pitch_inc` resident (SRAM_L has 312 B + 104 B back; they are 1.4 KB: not without moving something out). **M3**, a cheaper `pitch_inc` for the finetune case (a fine-tune step is one table lookup and one 64-bit scale; -100 a miss-free call).
5. **Attribution first (one tester session, no code):** baseline A with three more arms, each with only one LFO type on the 16 voices (pan only, cutoff only, finetune only), plus an in-situ cycle counter around `mod_block` and `recompute_strip` (a debug counter in flash, printed by `diag cpu`). If the split is as hypothesised, G1 and M1 are worth about 40-50 % of the LFO cost; if not, the LFO path is near its floor.
6. **Floor, if hypotheses (a) and (b) are right and G1 + M1 are built:** about 0.5-0.7k cycles a voice-block (0.2-0.27 pp) for pan + cutoff + finetune, scene A about +9..11k (+3.5..4.2 %). Without them the path is at about 1.2k and the arithmetic edits cannot move it.

**Looper residual** (S1 dense +1,955 = +0.75 % against +0.33 % in S3 and 893 cycles on the bench): about 1,000 cycles more than the isolated kernel in the dense scene, which fits cache pressure from the take's 8 lines a block and the loop's code lines evicting voice data. Option **L4** (Level C: its own change): fetch the next block of the take by eDMA into an SRAM_U scratch (no CPU wait, no cache lines), expected under 0.3 %; not started.

### 8.8 Round-2 review fold (no change to the firmware)

C5 deleted entirely (the asm, the switch, `tests/audio_svf_split_test.cpp` and its CMake lines). `cmake/O3osQemu.cmake` adds `-ffp-contract=off` to every QEMU test target (production is built so; a test that compares float loops now sees the unfused code the unit runs). The mk66 image is byte-identical to the one built at 248905f.
