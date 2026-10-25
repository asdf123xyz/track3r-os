# OS-CPULAYOUT: does the code layout move the render cost between a bench image and a release image?

Status: **closed 2026-10-17 as REFUTED (section 8); the unit A/B of section 5 decided it; F0 (merged) is the only code change** · Level B (investigation); any fix that moves hot code or touches the linker script is Level C · Classification: **open research question** (nothing here is a parity claim). Evidence markers: [map] read in the linked ELF/map of the builds below, [model] computed from the map with the cache geometry of `lmem.h`, [unit] measured by the tester, [est] estimate.

## 1. Evidence

Unit, same song config (16 instruments, FILTER On, LFO off) [unit, relayed by the orchestrator]:

| Image | Over-budget blocks (render above 60 % of the block, 156.7k cycles) | cpu max |
|---|---|---|
| bench image 2, `g248905f` (bench code always in) | 64-67 | 210-212k |
| release v0.6.3, `a29c7c9` (`O3OS_BENCH` OFF, OS-BENCHOPT) | 129-135 | 216-220k |

Builds made for this note (mk66, arm-gnu 15.3.Rel1, `-DO3OS_PLATFORM=mk66`, ccache; local build dirs, not committed): `a29c7c9` with `O3OS_BENCH` OFF and ON, and `248905f`.

| Build | FLASH | SRAM_L | SRAM_U | SDRAM_STATIC | `.text` | `.ramfunc_l` |
|---|---|---|---|---|---|---|
| a29c7c9 OFF (= v0.6.3 config) | 740,148 | 49,592 | 136,760 | 113,728 | 712,800 | 27,316 |
| a29c7c9 ON | 757,968 (+17,820) | 49,624 (+32) | 136,760 (0) | 113,728 | 730,584 | 27,352 (+36) |
| 248905f | 756,620 | 49,624 | 136,760 | 113,728 | 729,236 | 27,352 |

The 17,820 B of the bench are 13,840 B of text, 3,944 B of rodata and 36 B of SRAM_L code (`load_pass`) [map]. `.bss`, `.data`, `.dma_rings`, `.noinit*` are byte-identical in size and address: **no RAM data moves** between OFF and ON.

**What the maps say** [map]:
1. `bench.cpp` is the 11th source of the `o3os` executable, after `main.cpp` and before `usb0.cpp`/`console.cpp` and the libraries; its text lands at 0x24e8 (OFF: `usb0.cpp` starts there). Every function linked after it moves **+13,840 B** (3,315 of 4,323 matched symbols; the flash audio code, the sequencer, the app, libgcc/newlib all together). 13,840 = 865 lines of 16 B, so line alignment (mod 16) is unchanged and the 256-set cache map rotates by 97 sets. The text group moves rigidly: all distances between audio, sequencer and library code are the same in OFF and ON.
2. The rodata (after all text, one `.text` output section) moves **+17,784 B** (+17,984 for part of it): the text-to-rodata distance changes by 3,944 B, i.e. -152 B modulo the 4 KiB cache period. Early text (vectors, startup, platform glue below 0x24e8) does not move.
3. In SRAM_L everything after `isr_boundary` moves +36 B (mod 4 unchanged, mod 8 flips); `.tapmem_l` +32 B. SRAM_L is not cached.
4. The flash audio code that runs per block (the render, voices, mixer, filters and kernels are already in SRAM_L, `.ramfunc_l`, 109 functions, 27 KB): SendDelay (process 2,170 B, read_taps 784), SendReverb (run 2,240, process 362, input_block 956, tank_block 1,008, decimate 872, interpolate 464), MasterLimiter::process 1,356, VoicePool::mod_block 596, exp2/log2_fast, the loop player. About **12.7 KB, 810 lines, against 512 lines in the cache (8 KiB, 2 ways, 256 sets of 16 B)**: it cannot fit whatever the layout. Addresses (set range of the 256):

| function | size | a29c7c9 OFF | a29c7c9 ON | 248905f |
|---|---|---|---|---|
| SendDelay::process | 2170 | 0x222e0 (46-181) | 0x258f0 (143-22) | 0x25808 (128-8) |
| SendReverb::run | 2240 | 0x24198 (25-165) | 0x277a8 (122-6) | 0x276c0 (108-247) |
| SendReverb::input_block | 956 | 0x234b4 (75-134) | 0x26ac4 (172-231) | 0x269dc (157-217) |
| SendReverb::tank_block | 1008 | 0x23870 (135-197) | 0x26e80 (232-38) | 0x26d98 (217-24) |
| MasterLimiter::process | 1356 | 0x24e74 (231-59) | 0x28484 (72-156) | 0x2839c (57-142) |
| VoicePool::mod_block | 596 | 0x1e0a0 (10-47) | 0x216b0 (107-144) | 0x215c8 (92-129) |
| kSvfW (flash table the SRAM_L filter reads) | 200 | 0x994fc (79-92) | 0x9c42c (66-79) | 0x9bea0 (234-246) |

5. **bench image 2 is not the same code as v0.6.3 besides the bench.** `248905f..a29c7c9` also adds the OG PCM/PTI loaders, the save service and the storage remount (app, storage; no audio source). The audio files differ only in comments and in the deleted C5 asm (`svf_mul` is the plain product with the switch off, so the filter object code is the same). In 248905f the FX group sits +13,608 B from OFF (8 mod 16), other groups +12,672 / +13,336 B: the groups no longer move together, so the distances between audio, sequencer and app code differ from both OFF and ON.
6. Static pressure [model], 19 per-block flash objects above (12,746 B): lines in sets with more than 2 contenders (cyclic access, all miss under LRU) are **684 (OFF), 712 (ON), 688 (248905f)** of 810. The model puts ON slightly *worse* than OFF, the opposite of the unit, and the whole difference is about 28 lines, about 0.3k cycles at 8-10 cycles a flash line fill [est]. The unit difference is 6-8k cycles in the worst block.

## 2. What this means

The map shows one real layout change on the hot path (a rigid 97-set rotation of all flash code and a -152 B change of the code-to-rodata distance, relative to the early text and to fixed-address streams such as the SDRAM alias, which is cached) and no change of RAM data. The static model of the per-block code does **not** reproduce the unit difference in size or sign. An effect of 6-8k cycles is about the cost of refilling the whole 12.7 KB (810 lines) once, so it would need a block in which the hot set was evicted, which the model of a steady state does not show. The over-budget count is a tail statistic around 156.7k cycles: a 2-3 % shift of the heavy blocks is enough to double it, so a modest effect is enough; the cause is still unproven.

## 3. Hypotheses, ranked

| # | Hypothesis | For | Against | Test |
|---|---|---|---|---|
| H1 | The two images are not comparable: other SHA (app/save/storage changes), the bench image carries `diag bench` that was run before the song windows, `cpu perf` ON/OFF, uptime or SD/USB activity differ | the tester compared bench image 2 with a different SHA; the protocol of `cpu-budget.md` section 5 lists the perf toggle | the counts are stable inside each image (64-67, 129-135) | same-SHA A/B, section 5 arms A and B |
| H2 | Flash code and rodata set mapping (the rotation, the -152 B), i.e. conflict misses in the 2-way cache for the 12.7 KB per-block set plus kSvfW | the only mechanism the diff creates; 12.7 KB over an 8 KiB cache is thrash-prone; every merge rotates the map, so cross-version CPU numbers carry a layout term | the static model gives about 0.3k cycles and the wrong sign | arm C (OFF plus pad, same 13,840 B text / 3,944 B rodata): if C tracks B, layout is proven |
| H3 | SRAM_L code moved +36 B (mod 8 flips) | the only other hot-path change | SRAM_L has a 32-bit code bus and zero wait; mod 4 is unchanged; expected cycles about 0 | cannot be told apart from H2 by arm C (it keeps the 36 B out); arm C' adds a 36 B SRAM_L pad if C does not reproduce B |
| H4 | Run-to-run noise | cpu max is a single-event figure | counts repeat to within 3-6 % in each image | repeat windows, ABBA order |

Top: **H2 is the maintainer's suspicion and the only mechanism in the diff, but the model does not support it by itself; H1 must be excluded first because it costs one image.**

## 4. Fix candidates (none implemented)

| # | Candidate | Cost | Gain | Risk / level |
|---|---|---|---|---|
| F0 | **Link bench.cpp last** (after the libraries, e.g. its source in a `bench` OBJECT library named last in the link line): a bench image then has the same addresses for all non-bench code as the release, so bench-versus-release A/B is valid for good | 0 B | makes bench images comparable; cures this item's confound, not the layout term | Level A, `platform/mk66/CMakeLists.txt` only |
| F1 | **Pin the per-block flash set**: `O3OS_AUDIO_FLASHHOT` (section `.text.o3os_hot.*`, and `.rodata.o3os_hot` for kSvfW/kLfoRate) on the 12.7 KB group; `mk66.ld.in` places `.text_hot` first inside `.text` after the flash-config field at ALIGN(4096), so the set map of that group is the same in every build whatever is linked before; a `check_ramfunc.py`-style test asserts the start address mod 4096 | up to 4,095 B flash padding (about 2 KB average), 0 SRAM, 0 cycles of its own | removes the layout term for good; the order inside can put the always-running part (limiter, delay/reverb run) in the first 8 KiB so it has no self-conflict | **Level C** (linker, audio path); needs the map diff gate in `fw-gate-check` |
| F2 | **Move the top items into SRAM_L**: MasterLimiter::process (1,356 B, always runs) first; then SendDelay::process + read_taps (2,954 B); then SendReverb (6.9 KB). Region: 49,592 of 51,200 B used (1,608 free), hot budget 27 KiB with 332 B left, tap data budget 1,280 B of room; the limiter alone needs the data budget's room (76 B short), the delay another 2-3 KB: a slab cut (`kModuleSlabSize` 14 -> 12 KiB; the hot parts of the three built-in modules use about 7.5 KB, two earlier cuts are the precedent) | +1.4 to +4.3 KB SRAM_L, flash unchanged (load image) | removes those lines from the cache and from the thrash; `cpu-budget.md` E1 estimates 3-8 points of the block for buses + master [est, unmeasured] | **Level C**, ADR 0013 D7 amendment (maintainer) |
| F3 | `-falign-functions=16` for the group | up to +1 KB flash | none expected (rigid shift keeps mod 16) | not recommended |

F1 fixes the cross-version noise; F2 fixes the capacity. They are independent; F0 is needed first for the measurements. Outcome (2026-10-17): F1 dropped; F2 out; F0 merged (section 8).

## 5. Unit A/B protocol

Images, all from **`a29c7c9`** (same SHA, same toolchain): **A** release (bench OFF, = v0.6.3 as shipped); **B** bench ON; **C** bench OFF plus a link-time pad of 13,840 B text and 3,944 B rodata ahead of the audio libraries (a throwaway build option, never merged; it reproduces B's addresses without the bench); **C'** only if C does not match B: C plus 36 B in `.ramfunc_l`.

Procedure per window (tester, same card, same project t-v16 with the 16-instrument FILTER On, LFO off config, console connected in every window, nothing else run before the window; no `diag bench` before the measurement on B):
1. Cold boot, load the project, 2 minutes of play unmeasured.
2. `diag cache` and record the line (PCCCR, PCCRMR must be identical across images).
3. `diag cpu reset`, `diag cpu perf off`, play 60 s, `diag cpu`: render mean, max, over-budget count, underruns. On B also `diag bench stages` and `diag bench worst` (stage split of the worst block).
4. Bound of the cache's whole contribution: `diag cache off`, repeat step 3, `diag cache on`. The difference between cache on and off is the ceiling of any layout effect.
5. Order A B B A A B B A, then C (and C' if needed) in the same ABBA pattern: at least 4 windows per image; first report the spread of repeated windows of the same image (noise), then the means.

Decision rule: layout is **proven** if B and C each differ from A by at least 1.5x in over-budget and 3k cycles in max in at least 3 of 4 windows and C is within noise of B; **refuted** if A, B and C agree within noise (then the 2x between bench image 2 and v0.6.3 is H1, i.e. SHA or protocol). Report with SHA, image name and sha256 of each image.

## 6. Maintainer questions

| # | Question | Recommendation |
|---|---|---|
| Q1 | Build and hand the tester images A, B and C (a29c7c9) for the A/B above? (costs three gate-checked images, no code merged) | **Yes** |
| Q2 | Merge F0 (bench object linked last) now, Level A? | **Yes**; independent of the result |
| Q3 | If layout is proven (section 5), approve F1 (pinned, 4 KiB-aligned hot flash section, up to 4 KiB of flash) as Level C? | **Yes, only after the A/B**; reject if C is within noise of A. Answer (2026-10-17): C is within noise of A, so F1 is dropped (section 8) |
| Q4 | F2 (limiter, then delay into SRAM_L by cutting the module slab 14 -> 12 KiB) as a separate item OS-PERF4/E1? | **Not now**: decide on `diag bench stages` buses + master (above 10 % of the block) from arm B; it is the larger lever if layout is refuted |
| Q5 | Accept that CPU figures across versions (and across bench/release builds) carry a layout term of unknown size until F1, so a result under about 1 % of a block between two SHAs is not a regression? | **Yes**, state it in `cpu-budget.md` section 5 once the A/B gives the size |

## 7. State

1. Maps of a29c7c9 OFF, ON and 248905f diffed; findings in section 1.
2. No RAM data moves; text +13,840 B rigid, rodata +17,784 B, SRAM_L code +36 B.
3. The per-block flash set is 12.7 KB over an 8 KiB cache: it thrashes in every layout.
4. Static model: ON is 28 lines worse than OFF, about 0.3k cycles, opposite to the unit.
5. 248905f differs from a29c7c9 also in app/storage code: the compared pair is confounded.
6. Top suspicion (layout) refuted by the same-SHA A/B (section 8).
7. A/B/C run on the unit (2026-10-17). The cache on/off bound is the same in all arms (about 48k cycles of max, about 13.5k of over-budget).
8. F0 merged (9f49ffb). F1 dropped; F2 out (section 8).
9. F0 (9f49ffb) changed only the link order (the bench is linked last). No format changed.
10. The section 5 check is done (section 8); no hardware check is open for this item.

## 8. Outcome (2026-10-17)

Closed as **REFUTED**: the code layout does not move the render cost in this scene. The source is the tester's local report (untracked, not in git), run with the section 5 protocol.

Scene: Track 14 = t-v16 with FILTER on x16, LFO off, pattern 9 looping; 60 s windows; cold boot, then 2 min warm-up. PCCCR and PCCRMR are identical in all images; 0 underruns.

| Arm | Build | Windows | cpu max | over-budget |
|---|---|---|---|---|
| A | 9f49ffb, release (bench OFF) | 8 | 208490 (sd 214) | 149.0 (sd 1.2) |
| B | 7e8e839, bench ON | 4 | 208371 | 147.8 |
| C | 6a4be7a with pads (throwaway) | 4 | 208900 | 146.0 |

- B-A and C-A are inside A's own spread. 0 of 4 windows meet the rule of section 5 (1.5x over-budget and +3k max). A, B and C agree within noise.
- The cache on/off bound is the same in all three arms: about 48k cycles of max and about 13.5k of over-budget.
- The gap (64-67 over-budget on bench image 2 against 129-150 on release) is not code layout: bench ON at 7e8e839 gives 147.8, the release value. It stays an open research question, recorded in the BACKLOG row; no new item.
- F1 (`.text_hot`) is dropped, so Q3 does not apply. F2 was already out. F0 (9f49ffb) stays merged.
