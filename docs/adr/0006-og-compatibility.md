# ADR 0006 — OG project compatibility

Status: **Accepted** (maintainer, 2026-10-10: compatible with the OG) · Interpretation below is the maintainer's; the maintainer may correct it.

## Decision
- **Open:** OG and 1.x–1.9 projects, patterns and instruments (`.mt`, `.mtp`, `.mti`, `.pti`, PAMD, 8- and 12-track patterns) open directly in the OS, through the M1 pure format modules (file format, sanitiser, instrument, pattern, project and PAMD readers) and their fixture tests.
- **Save:** saving writes the OS's own versioned project format. The OG files are never modified in place, so the original project stays usable on the OG firmware.
- **Export:** an explicit **Export → OG project** action writes an OG/1.9-compatible copy. It warns about everything that can't be represented: more than 8/12 tracks, engine FX 128–255, modules, modulation, routing. The export has round-trip tests against the fixtures.

## Consequences
- The OS project format v1 must be a superset of what OG can express (ADR 0003, 0004), so that OG → OS → OG is lossless for OG-only content.
- The fixture corpus (maintainer's card, never committed) is the acceptance test for import and export.

## Amendment 1 — compatibility ceiling is official 1.9.2 (2026-10-11)
- **Functionality.** All 1.9.1/1.9.2 parity rows (PAR-xxx) are OS requirements via ADR 0008:
  - PAR-154: Line-In crackle fix.
  - PAR-155: OFF placement during live recording.
  - PAR-156: Focus Loop.
  - PAR-157: 10th Anniversary theme. **Dropped by the maintainer (2026-10-11).**
  - PAR-158: fixes.
  - PAR-159: Monochrome theme fixes and encoder state at power-on (1.9.2), added by the triage, 2026-10-11.

  Sources: the vendor release notes and the 1.9.2 manual.
- **Formats.**
  - *Read:* the goal is to read everything official 1.9.2 saves (the fixtures stop at 1.9.0, see the evidence gap).
  - *Write:* Export → OG writes files in the layouts of official 1.9.x (not yet checked on an official unit: OS-FILES-C C3): project v16, 12-track pattern v5, `.pti` v9, PAMD.
  - *Evidence gap:* fixtures exist only up to 1.9.0, so it is unknown whether 1.9.1/1.9.2 bumped any structure version.
- **Evidence plan.**
  - *No reverse engineering needed.* The maintainer saves a test project with official 1.9.2 on the designated test card and the fixture survey reads it. The project should have 12 tracks, two `.pti` instruments, Focus Loop used, and the theme selected.
  - *If the versions are unchanged,* the 1.9.0 readers cover it.
  - *If they changed,* readers are written from the fixtures, as was done for the project file.
- **Hardware check.** Export from the OS, then open the result on official 1.9.2, as a `hw-session` item.

## Amendment 2 — a SAVE in place writes only what changed (maintainer, 2026-10-20, OS-BUG-OGSAVE)
- **Save in place.** "The OG files are never modified in place" is read as: a SAVE of an OG project in its own folder adds or updates `project.o3pj` (the previous one kept as `.bak`) and never touches `project.mt`, the patterns or the `.pti` files.
- **Sample WAVs.** The native project names `samples/instrNN.wav`, so a SAVE may write them, but only when needed: a WAV is rewritten (the old one kept as `.bak`) only for a sample that was edited, or written when it is absent. A WAV that already holds the bank's audio (the same channels, rate and 16 bits, the same whole frames byte for byte; a trailing half frame and other chunks are ignored, as in the official firmware's odd data chunk with no pad byte) is never touched. Any doubt (it differs, a read error, a shorter file) is a rewrite with a `.bak`.
- **Cost.** The comparison runs in the save job (a read of the file, a chunk a step, only for a sample the folder is not known to hold), never at load and never on the audio path.
