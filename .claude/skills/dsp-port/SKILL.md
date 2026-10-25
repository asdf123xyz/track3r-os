---
name: dsp-port
description: Bring an open-source DSP algorithm (filter, oscillator, TB-303 voice, Moog ladder, Amiga/Paula resampling, effect) into the OS or a module correctly - licence gate, float-only block-based port, host oracle tests against the original code, cost estimate and bench entry. Use together with fw-feature for any DSP port or reimplementation.
---

# DSP port

## Gate 1 - licence (before reading code closely)
Read the LICENSE file and the file headers; record the repo URL and commit hash in the spec.
- MIT/BSD/ISC/zlib/Unlicense/Apache-2.0: may port; attribution in `THIRD_PARTY.md` and the file header.
- GPL/LGPL: only if compatible with this project's licence (GPL-3.0-or-later for the OS; the module SDK under `module/sdk/` is MIT, so a module that links the SDK must not carry GPL-only code unless the module itself is distributed under a compatible licence); otherwise clean-room from papers or descriptions (a different agent writes the spec, the implementer never sees the incompatible code).
- Proprietary or unknown: inspiration only.

## Gate 2 - oracle
Build the original implementation on the host (in the scratchpad, or under `tests/` if the licence allows vendoring) and render reference outputs for fixed stimuli: impulse, sine sweep, saw at several pitches, seeded noise, parameter ramps. The port is tested against these: max abs error 1e-4 or less, or SNR of 80 dB or more against the double-precision reference, unless the spec documents an intended deviation (for example a cheaper tanh approximation with its measured error).

## Port rules
- `float` only: `f`-suffixed constants, `sinf`/`tanhf` or table/polynomial approximations; no `double`, no libm double calls.
- Block processing, state in a struct, all memory allocated at init (SDRAM for big tables and delay lines, SRAM for hot state), no allocation, SD I/O or blocking in the render path. Accept any block length from 1 up to the SDK maximum.
- Coefficients per block (with smoothing) unless per-sample modulation is the point.
- Use CMSIS-DSP or M4 SIMD intrinsics only where a measured hot spot justifies them.
- It lives where the radar card says: core flash, or an SD module (`.tmod`, skills `sd-module` and `tmod-perf`).

## Cost
Estimate cycles per sample from the op count (show the arithmetic) and add the kernel to the bench list for the next `hw-session`. The radar card and the spec say *estimate* until measured on the unit.

## Tests
Host doctest under ASan/UBSan: oracle comparison, parameter extremes (no NaN or Inf, bounded output), reset and state behaviour, block-size independence. Then the normal `fw-feature` verify and review.
