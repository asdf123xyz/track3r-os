# ADR 0007 — Native project format: open chunked format + WAV; XM/IT/MIDI for interchange

Status: **Accepted** (maintainer, 2026-10-10: the proposal was accepted). The maintainer had asked for "something standard", e.g. Renoise.

## Decision (proposed)
- **Native save.** A project folder:
  - samples as standard **WAV**;
  - project, song, pattern, instrument and module-reference data in an **IFF-style chunked binary** (`FORM` / typed chunks with lengths, the Amiga standard container). Each chunk is versioned; unknown chunks are preserved on resave; CRC per file.
  - The specification is published in the repo (`docs/formats/`), with a stdlib-Python reference reader/writer and round-trip tests.
- **Interchange.** Export to **XM / IT**, the de facto open tracker standards read by OpenMPT, MilkyTracker, libxmp and Renoise, and to **Standard MIDI File**. Import of MED/MOD/XM/IT per the `tracker-module-import` card. A later desktop tool may convert to Renoise XRNS.
- **OG.** Open natively; Export → OG (ADR 0006).

## Why not Renoise XRNS as the native format
- It is a closed application's format, specified only by per-version XSD schemas.
- It needs ZIP + XML + FLAC on a 256 KiB-SRAM MCU at every save.
- It cannot hold our engines/modules, routing or modulation without private extensions that Renoise would drop.
- Renoise already imports XM/IT/MOD/MIDI.

## Consequences
- The format must be a superset of OG content (ADR 0006) and hold 16 track slots (ADR 0003), the step layout (ADR 0004), and module name + ABI version per instrument (ADR 0002).
- Exports are lossy by nature and must list what was dropped.
