# OS-004 — Project model v1, native format, OG import/export

Status: specified 2026-10-11 · Decisions in force: ADR 0003 (12 typed tracks, 16 slots), ADR 0004 (step format, FX ranges), ADR 0006 + amendment 1 (OG compatibility up to 1.9.2), ADR 0007 (native IFF-style format + WAV), ADR 0009 rev 1 (model–view separation, formats as Strategy + registry, resumable), ADR 0011 (declared alignment).

## Goal
A `model` library that holds a whole project with no UI or hardware knowledge, a native chunked format that round-trips it bit-exactly, and an OG reader/writer, so the OS can open any OG project from 1.0 to 1.9.x and export one that official 1.9.x opens.

## Layering
New libraries:
- `model`, depending on `core`;
- `formats`, depending on `core` and `model`, with `formats/native` and `formats/og`.

`app` may depend on both. `ui`, `eve` and `hal` must not depend on either. Extend `check_layers.py` and its tests.

## 004a — Model and native format
**Model** (`o3os::model`):
- **Project:** name, tempo (BPM ×100 fixed point), swing, pattern length default, song, pattern pool, instrument pool, track slot table (16 slots, 12 active by default; type Audio | MIDI | ExternalAudio, name, mute/solo, volume, pan, output bus), master settings, and a module reference table (name, ABI major.minor) per ADR 0002.
- **Song:** an order list of up to 255 positions, each holding a pattern index.
- **Pattern:** id, name, and length 1..128 steps. Each of the 16 track slots holds `Step{note, instr, vol, fx[2]{type, value}}`, 7 bytes, packed and declared `alignas`.
  - Note values cover OG notes plus off, cut and fade (the OG note encoding).
  - FX types follow ADR 0004: 0–42 OG, 43–127 reserved, 128–255 engine. Unknown types are kept verbatim.
- **Instrument:** name, kind (sample | MIDI | module engine), sample reference (path + loop/slice metadata), and the OG sampler parameters needed for lossless OG import (volume, panning, tune, fine, envelopes, filter, granular, wavetable, slices…). `docs/formats/og.md` lists them.
  - Module instruments add a module reference and a parameter blob, which is opaque to the core and kept verbatim.
- **Storage:** no heap. The model lives in a `ModelArena` that the platform provides (SDRAM on MK66, a static buffer in host tests), sized by `constexpr` limits in one header. State the byte cost of the worst case. The pattern pool may be smaller than 255 if the arena requires it: document the trade-off and keep the limit in one place.
- **Access:** reads are free. Writes go through a narrow mutation API (`set_step`, `set_order`…) that bumps a generation counter. OS-002c will route undo through this choke point, so do not build undo now.

**Native format** (`formats/native`, spec in `docs/formats/o3-project.md`):
- An IFF-style container: `FORM` + type `O3PJ`, then chunks of (4-char id, u32 big-endian length, payload, pad to even).
  - Chunks: `PHDR` project header, `TRAK` track table, `SONG`, `PATT` (one per used pattern), `INST` (one per used instrument), `MODR` module refs, `CRC ` (CRC-32 of everything before it).
  - Each chunk payload starts with a u16 chunk version.
  - Unknown chunks are kept as opaque blobs and re-emitted on save, within a bounded pool; if the pool overflows, the user is told.
- The reader and writer work over a `ByteSource` / `ByteSink` interface with bounded buffers and do no file I/O. They are resumable (`step()` returns progress, done or error) so OS-002c jobs can drive them. Errors are typed: truncated, bad CRC, newer major version, limit exceeded. Nothing crashes on hostile input.
- Samples stay as WAV files next to the project. The project stores relative paths.
- **Python reference** (stdlib only, `tools/o3project.py`): read and write, plus a ctest that round-trips C++ → Python → C++.

**Done when (004a):**
- Unit tests cover:
  - model limits and mutation generations;
  - a byte-exact native round trip of a maximal project;
  - unknown chunks preserved;
  - every error path, including truncated input at every offset of a small file;
  - CRC.
- There is a fuzz-style loop: deterministic PRNG mutations of a valid file, run under ASan/UBSan, with no crash or out-of-bounds access.
- The Python round-trip test passes.
- MK66 sizes are reported per library, and the arena worst case is stated.

## 004b — OG import and Export → OG
Base: 004a merged.
- **Strategy and registry** (ADR 0009 rev 1):
  - a `FormatReader` / `FormatWriter` interface (virtual at format granularity);
  - a registry keyed by format id, extension and sniffed magic;
  - native and OG registered first. MOD/MED/XM import and XM/IT/MIDI export come later and plug into the same registry.
- **OG reader** (`formats/og`). It reads project.mt v6 (1.1), v11–v14 (1.3–1.6) and v16 (1.9); patterns with 8 tracks and 12 tracks (v5); `.mti`; `.pti` v1–v9; PAMD. It maps them losslessly into the model. Unlike a reader that drops the 1.3–1.9 additions, this reader keeps everything the model can represent and lists the rest.
 - **Layouts and offsets:** use `docs/formats/og.md` (the OG byte maps), `docs/formats/o3-project.md` and the PAMD description there. The `og_*` format modules and their host tests are the compatibility layer and the source of test vectors.
  - **Newer versions:** a version newer than the highest known one (1.9.2 evidence gap, ADR 0006 amendment 1) loads read-only with an "unverified version" result that the UI turns into a notification. It is never misread silently: stop at the first structure whose size or version is unknown.
- **Volume column and per-track length (2026-10-11).**
  - OG steps have no volume byte; volume is FX 18. On import, the two OG FX columns are kept verbatim in `fx[0..1]` and the OS volume column stays empty, so OG → model → OG is lossless and FX 18 still shows where the author put it.
  - On export, an OS volume value goes into a free FX slot as FX 18. If both slots are taken, the drop report lists it.
  - Per-track lengths (the track `length` byte) map one-to-one onto the model's per-track lengths (004a).
- **Export → OG** writes project v16, 12-track pattern v5, `.pti` v9 and PAMD, the layouts of official 1.9.x (not yet checked on an official unit). It returns a report of what was dropped, for example tracks 13–16, engine FX types and module instruments.
- **Fixtures:**
  - The maintainer's files under `fixtures/` (main checkout) may be read by tests when present and are skipped when absent. Never copy, commit or print their contents beyond hashes and counts.
  - Committed tests use synthetic files generated in the test itself.

**Done when (004b):**
- OG → model → OG is byte-identical for synthetic v16 / v5 / v9 files. Any byte that is legitimately normalised is listed and justified.
- Every OG version listed above imports in a test.
- Hostile-input tests mirror the format-hardening rules of `docs/formats/og.md`: truncated input, oversize lengths, bad versions, path traversal in names.
- The fixture survey (when fixtures are present) imports every project, reports the counts, and has zero errors.
- The usual gates apply: host ctest green with 0 warnings, MK66 sizes, layer check, reviewer approval.

## Out of scope
Audio rendering (OS-005), the UI for open and save (file browser, item 5 of the basket), SD/FatFs I/O (`platform/mk66`), MOD/MED/XM/IT/MIDI (later slices on the same registry).

## 004c — OG 1.9.2 project v17: reader and Export target (layout: open research question, unverified, no file; docs/formats/og.md section 2)
- **Reader.** Move `project.mt` v17 (2,324 B) out of "unverified" into a new class, "observed only" (no file; open research question, unverified), and apply the layout of `docs/formats/og.md` section 3:
  - the name is read at 0x810;
  - the second string is read at 0x831;
  - the CRC is at 0x910;
  - the byte at 0x80C is carried losslessly: a non-zero value is kept for OG→OS→OG, in the OGPM group scheme with a version-gated range. It is not given a model field.

  The common part stays shared with v16. v8–v10, v15 and v18+ keep their current classes.
- **Export target.** The Export → OG target is a choice. The default is "OG 1.9.0" (v16, 2,320 B, as today). "OG 1.9.2" writes v17, with byte 0x80C taken from the carried value or 0. Pattern 5, instrument 9 and PAMD 1 are the same for both targets.
  - The UI choice is a device-scope setting parameter (OS-002d) shown in the Export dialog when that exists. Until then it is an API parameter.
- **Docs.** Update `docs/formats/og.md` (the v17 layout, the Export target and the "strictness is OS policy" sentence). Update `tools/o3project.py` (the Python reference) to match.
- **Tests:**
  - synthetic v17 round trips (C++ ↔ Python);
  - v16 to v17 conversion checks;
  - a carried non-zero 0x80C byte;
  - the name at 0x810;
  - CRC handling as the current policy defines it;
  - the existing fixture survey is unchanged (counts only, fixtures never printed or committed).
- **Done when:** host, QEMU and MK66 ctest pass with 0 warnings; `project_python` agrees; the reviewer approves with a persistent-format angle.
