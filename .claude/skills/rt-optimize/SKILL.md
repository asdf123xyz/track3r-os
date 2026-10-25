---
name: rt-optimize
description: Performance doctrine for the Tracker hardware (Cortex-M4F DSP optimisation, memory placement, DMA/zero-copy, SGTL5000 DAP and FT812 EVE offload) — how to measure, what to try in which order, and the rules each technique must follow. Use when writing or reviewing audio engine, sequencer, mixer, filter, resampler, synth, UI-render or driver code, when a budget is tight, or when a radar/ADR needs a cost estimate.
---

# Real-time optimisation (MK66 Cortex-M4F + SGTL5000 + FT812)

The device is not one 180 MHz CPU: it is the MK66 plus DMA engines, the SGTL5000 audio DSP (DAP) and the FT812 graphics coprocessor, around 16 MiB of SDRAM. Spend MK66 cycles only on what nothing else can do.

## 0. Order of work (never skip a step)
1. **Measure first.** Cycle counts with `DWT->CYCCNT` (enable `CoreDebug->DEMCR |= TRCENA`, `DWT->CTRL |= CYCCNTENA`), the OS's own `diag cpu` and `diag bench` console commands (`docs/architecture/cpu-budget.md` §5), ISR duration and worst case, not averages. Budget: one audio block is a fixed number of cycles shared by *everything* (a 64-frame block at 44,117 Hz is about 261k cycles at 180 MHz; a 128-frame block about 522k); keep audio at 60 % of the worst case or less, and see `docs/architecture/cpu-budget.md` for the heavy-song budget.
2. **Algorithm before micro-optimisation.** A better filter topology, resampler (e.g. polyphase/Hermite vs naive), voice-stealing policy or envelope update rate beats any intrinsic.
3. **Then data/memory, then CPU tricks, then offload.** Keep each step's before/after numbers in the commit body and, for hardware-measured kernels, in `docs/perf/` (via `hw-session`).
`diag bench` and `diag cache check` exist only in an image built with `-DO3OS_BENCH=ON` (OS-BENCHOPT); a release image answers "not in this image".
Host builds can prove correctness and relative op counts only; cycle claims need the device (bench lines `BENCH <kernel> <cycles/block> <blocks>`).

## 1. CPU (Cortex-M4F)
- **FPU single precision only** (`fpv4-sp-d16`): `float`, `f`-suffixed literals, `sinf/expf/tanhf` or cheaper approximations. `double` is software-emulated — banned in hot paths (`-Wdouble-promotion` on new code). Prefer multiply by reciprocal over divide (VDIV/VSQRT take ~14 cycles).
- **DSP/SIMD instructions** via CMSIS intrinsics (`__SMLAD`, `__SMLALD`, `__QADD16`, `__SSAT`, `__PKHBT`, `__SADD16`…): two 16-bit MACs per cycle, saturating arithmetic for free. Ideal for Q15 mixing, gain, panning, sample interpolation.
- **Fixed point (Q15/Q31) vs float** per kernel: Q15 + SIMD where bandwidth dominates (mixing, sample playback, int16 audio blocks); float where precision/feedback dominates (resonant filters, 303/ladder, envelopes, reverb). Convert once per block at boundaries, never per sample in between.
- **CMSIS-DSP** (Apache-2.0) for biquads (`arm_biquad_cascade_df2T_f32` / `_q31`), FIR, FFT, vector ops, before hand-writing them. Build CMSIS-DSP from source with the project's flags (check `THIRD_PARTY.md` for what is already vendored).
- **Block processing**: per-sample work is pure arithmetic; parameter changes, coefficient updates and branching happen per block (with smoothing/ramps).
- **LUTs**: sine, exp/pitch (note→phase increment), dB→gain, envelope curves, tanh. Small hot tables in SRAM; large ones in SDRAM; generated at boot when cheap to compute (flash is finite).
- **Hot loops**: `__restrict`, unroll by 4 to match SIMD, no virtual calls or function pointers inside the sample loop, branch-free selection (`__SSAT`, min/max), counted loops.
- **Inlining + LTO**: keep ISR and audio code in the hot (`-O3`) set of the CMake build; verify that LTO does not merge away ISR vectors or section placement (`platform/mk66/tools/check_ramfunc.py` checks the hot path in SRAM_L).

## 2. Memory
- **Static allocation / pools**: voices, events, blocks, plugin instances reserved at init; no `malloc/new` after boot in any real-time context.
- **Placement**: SRAM_L (0x1FFF0000–0x1FFFFFFF, code bus) gives zero-wait fetch for hot code placed in `.ramfunc_l` (copied from flash at reset; see `platform/mk66/README.md`) — check in the map that it actually lands below 0x20000000; SRAM_U for hot data and DMA buffers (`.dma_rings`); flash runs behind wait states and a small prefetch cache; SDRAM (0x70000000) for samples, big tables, delay lines, plugin images. Accesses must not span the 0x20000000 SRAM_L/SRAM_U boundary.
- **Alignment is declared, never placed** (hardware finding 2026-10-10: a 2-byte-aligned audio block pool landed at 0x1FFF0606 under `-fdata-sections` and LDM/LDRD faulted with UNALIGNED). Every buffer touched by word/multi-word loads gets `alignas` + `static_assert`.
- **Zero-copy / rings**: pass block pointers, single-producer single-consumer ring buffers between ISR/audio/UI/SD with documented single-writer ownership; no intermediate copies of audio blocks.
- **Layout**: SoA for per-voice state iterated in the sample loop; align SIMD data to 4/8 bytes; keep the per-block working set small.

## 3. Peripheral offload (DMA)
I2S audio (already DMA), SPI to the FT812, SDHC (ADMA) for sample streaming, USB. CPU starts transfers and handles completion; it never copies byte-by-byte. SD I/O never happens in an ISR or audio `update()` — stream through a ring filled by the main loop/DMA.

## 4. Coprocessor offload
- **FT812 (EVE)**: the MK66 builds command/display lists; the FT812 rasterises. Assets (fonts, bitmaps, icons, themes) live on SD and are loaded into RAM_G (1 MiB) at boot, compressed with `CMD_INFLATE`/`CMD_LOADIMAGE`, not in MCU flash. Cache static panels as DL snippets in RAM_G (`CMD_MEMCPY` + `CMD_APPEND`); rebuild only dirty regions/state. Rate tiers: static UI on change, meters/scopes 20–30 Hz, playhead 30–60 Hz, all below audio priority and dropped first under load. SPI transfers by DMA.
- **SGTL5000 DAP**: 7-band PEQ / 5-band GEQ or tone control, AVC (compressor/limiter-like), bass enhancement, surround — on the final stereo path, for free (the codec driver can configure it; unused today). Trade-off to decide by ADR before use: DAP processing is only in the analog/headphone output path unless routed back, so it would be missing from WAV export, USB audio and resampling, and it is one stereo bus (master only), not per track.

## Rules for reviewers
Reject optimisations without before and after numbers; reject `double`, allocation or SD I/O in real-time paths; reject unsafe ISR sharing introduced for speed; prefer the simplest version that meets the budget with margin.
