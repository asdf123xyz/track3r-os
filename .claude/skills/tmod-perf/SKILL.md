---
name: tmod-perf
description: CPU rules and checks for writing or optimising an SD module (.tmod) instrument or effect on the MK66, from the whole-engine CPU study. Use with sd-module whenever a new tmod is generated, its render() is written or reviewed, or a module's CPU cost is too high. Covers the CPU budget, render structure, register pressure, control rate, placement, the static disassembly check and the measurement on the unit.
---

# tmod performance

CPU is the binding constraint (maintainer, 2026-10-14). A song with one TB-303 already runs at about 70 %. Every module renders inside the same 64-frame block: about 261k cycles at 180 MHz, shared by the whole device. The process steps (spec, generate, oracle tests, pack) are in `sd-module` and `module/sdk/README.md`. This skill holds the CPU rules. Background and numbers: `docs/architecture/cpu-budget.md`. General doctrine: `rt-optimize`.

## 1. Budget before code

- In the spec, state the target cost per voice for a sustained note, in points (percent of 261k cycles). The modules row of the heavy-song budget is 22 % in total: TB-303 12 % (after its optimisation), FM 8 %, chip 2 %. Unit measurements in 0.4.9 for comparison: tb303 21.6 %, fm 11.7 %, chip 2.8 %.
- A new instrument aims at 5 points or less per voice; going above needs the maintainer's agreement. An effect gives its cost per instance.
- Write `[est]` on every number not measured on the unit.

## 2. Render structure

- **Two-level loop.** The outer loop runs once every kCtl samples (4 to 16) and does all control work: coefficients, envelope segment changes, idle tests, countdowns. The inner loop runs those kCtl samples and has no branches beyond the loop. Pattern to avoid: a `ctl` countdown, a `quiet` test and an `if` inside every sample (as in the TB-303's render loop, `modules/tb303/tb303_dsp.h`).
- **Register pressure is the first thing to check.** The M4F has 32 single-precision registers. Copy every piece of state used per sample into locals before the loop and write it back after it, as the TB-303 does for its ladder and envelope state. Never read `v.<struct>` inside the sample loop. If more than about 24 floats are live in the inner loop, split it into passes that go through a scratch buffer (oscillator and filter into scratch, then the decimator), so each pass keeps its state in registers. The TB-303's hot code is 32 % VLDR/VSTR: that is its cost, not the arithmetic.
- **No division in the sample loop.** Precompute reciprocals (`inv_fs`, `1/q`) in `derive()` or at control rate. A VDIV takes about 14 cycles.
- **Host maths at control rate only.** `exp2`, `log2`, `sin` and `cos` in `o3m_host` are an indirect call into cold flash code. A few calls per kCtl at most, never per sample. Per-sample curves use your own short inline polynomials.
- **Envelopes by segment.** Compute the segment's per-sample increment at control rate (as chip's control split does); the inner loop only adds.
- **Silence costs nothing.** Detect a settled voice or operator at control rate and skip its work (write zeros or return early). FM: skip operators at zero level; pick the algorithm's render at control rate, not per sample.
- **Oversampling is expensive.** Prove it is needed. When it is, decimate with a polyphase half-band IIR (two first-order allpass branches) rather than cascaded biquads.
- **No per-sample anti-denormal tricks on the target.** The FPv4-SP handles subnormals in hardware at full speed [est, from the study]; the TB-303's `kTiny` adds 6 operations a sample for nothing on the unit. If the host oracle needs it, guard it to host builds.
- **Accept any `n` from 1 to O3M_BLOCK_MAX (128).** Size scratch buffers for 128. Maintainer, 2026-10-14: if needed, intensive modules such as the TB-303 move to 128-frame renders, so never assume 64.

## 3. Parameters

- `set_param()` runs in the render context. A parameter flagged `O3M_PARAM_MOD` can be called every block by the modulation matrix: its `set_param` must update only what that parameter changes. No full `derive()`, which costs four exp2/log2 calls plus divisions in cold code (`derive()` in `modules/tb303/tb303_dsp.h`). If a parameter cannot be made cheap, do not flag it MOD.
- Smooth parameter changes with per-block ramps; the inner loop reads only locals.

## 4. Placement and code size

- `O3M_HOT` is `noinline` and puts code in `.text.o3m_hot` (`module/sdk/o3module.h`). Mark only `render()` and helpers called once per block. Per-sample helpers are `inline` (use `__attribute__((always_inline))` if needed) so they fold into the loop. A per-sample helper that is neither inlined nor hot ends up in cold SDRAM code: 10 to 30 times slower.
- All loaded modules share one 14 KiB SRAM_L slab for their hot parts. About 8 KiB of it is used by the built-ins [est], and a module that does not fit runs from SDRAM. Keep the hot part within about 2.5 KB (the TB-303's is 2,320 B); `o3m_pack` prints the hot size.
- State is the instance's `state_bytes`: it goes to a shared 4 KiB SRAM_U arena first and to SDRAM when the arena is full. Keep it compact. The hot code reads no large tables: they live in SDRAM.
- The packer builds `-O2 -ffp-contract=off` (`CFLAGS` in `tools/o3m_pack.py`), so there is no fused multiply-add today. Do not hand-tune for FMA.

## 5. Checks before review

1. **Oracle test.** Host test against the reference, bit-exact or with a stated SNR, before and after every optimisation.
2. **Static disassembly** of the target ELF (the module's ELF in the MK66 build directory, for example under `build-os-ci-mk66/modules/`):
   ```sh
   OBJDUMP=<toolchain dir>/bin/arm-none-eabi-objdump   # the directory is documented in docs/toolchain.md
   $OBJDUMP -d -j .o3m_hot <elf> | awk '/^ +[0-9a-f]+:/{n++} /vldr|vstr|vldm|vstm|vpush|vpop/{m++} /vdiv/{d++} /blx/{b++} END{printf "instr %d  fp ld/st %d (%.0f%%)  vdiv %d  blx %d\n",n,m,(n?100*m/n:0),d,b}'
   ```
   - Targets: FP loads and stores under 15 %; no VDIV and no `blx` inside the sample loop. Report the line in the review.
   - Built-ins as of 2026-10-14: tb303 616 instructions, 32 % FP ld/st, 7 VDIV, 1 blx; fm 518, 16 %, 0, 0; chip 696, 17 %, 5, 1.
   - The section name follows the packer: if `-j .o3m_hot` prints nothing, look up the name with `objdump -h`.
3. **On the unit**, through `hw-session` and the tester, using the protocol in `docs/architecture/cpu-budget.md` §5:
   - same build id, `diag cpu` with the module off and then on;
   - `diag mod` for placement (slab or SDRAM);
   - `diag bench` where the module has a bench kernel (it needs an image built with `-DO3OS_BENCH=ON`; a release image has no bench).
   - Report min, mean and max. Host numbers never count as cycles.
4. **The commit body** carries: the hot size, the disassembly line, the unit cycles (or "HW pending") and the cost against the module's budget row.

## Reviewer rejects

A per-sample read of `v.<struct>`, a per-sample host maths call or division, a per-sample helper left out of line, a MOD parameter whose `set_param` runs `derive()`, a hot part over about 2.5 KB without a reason, an optimisation without an oracle result, or a cycle claim without a unit measurement.
