---
name: sd-module
description: Create, review or document an SD-loadable Community OS module (.tmod) such as the TB-303 engine, an instrument, an effect, an importer or an app. Use when the maintainer asks for a module that loads from the SD card, or for the module ABI, loader or packaging. Reads module/sdk/README.md first and reports which parts are design-only.
---

# SD module

The full procedure is in [module/sdk/README.md](../../../module/sdk/README.md) (the guide is [docs/guides/module-sdk.md](../../../docs/guides/module-sdk.md); app modules: [docs/guides/app-modules.md](../../../docs/guides/app-modules.md)). Read it before acting. This skill is the gate and checklist.

## Gate (do this first)

1. Read `module/sdk/README.md`, its section "Still design-only or open", and [ADR 0002](../../../docs/adr/0002-sd-plugins.md) with its amendments.
2. Check the ABI version in `module/sdk/o3module.h` and what the README says about versioning. If the change needs an ABI change (new entry point, new header field, new capability), it is not a module task: it is Level C and needs a maintainer decision and an ADR amendment.
3. Say in the report which parts are design-only (for example effects have no slot in the current OS, per the README's capability table) and do not build on them as if they ran.
4. Do not claim that a module loads or plays on a Tracker. Host tests prove the DSP, not the device.

## Steps

1. **Licence gate** with the `dsp-port` skill before porting any DSP code.
2. **Source** in `modules/<name>/` (scaffold with `tools/o3m_gen.py` and the templates in `modules/templates/`). Include a README with header fields, parameters, memory requests, credits and licence.
3. **Rules:** float-only; no libc, RTTI, exceptions or global constructors; in `render` and `note_on`, no allocation, SD I/O, blocking or unbounded loops (CLAUDE.md, engineering standards). Performance rules: skill `tmod-perf`.
4. **Host tests:** build as a host shared object against the same header; oracle tests against the reference; register them in ctest with an area label.
5. **Blob build:** `tools/o3m_pack.py` (run by the CMake `modules/` rules with the cross compiler from `docs/toolchain.md`) produces the `.tmod`; read the hot size it prints.
6. **Hardware:** only through `hw-session`. Measure DWT cycles per memory region and the module's cost with `diag cpu` and `diag mod` before quoting any figure.

## Risk level

- A new DSP module that does not touch the loader or the ABI: Level B (`fw-feature`).
- Loader, header, ABI, card format, containment, or how projects store module references: Level C, full `fw-feature` process with maintainer decisions.

## Never

- Flash an image, or use SWD or a debug probe.
- Write to storage that is not an SD card, or format or delete files on a card without the maintainer's ask (CLAUDE.md, SD cards and firmware delivery).
- Change how projects store module references without a version bump, a backward-compatible loader and a round-trip test.
- Copy code, data or reverse-engineered material from the official firmware into a module.

## Report

State: what was built, which tests ran (command and result), what is design-only, what the hardware bench still needs. Keep it short.
