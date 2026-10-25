# ADR 0008 — Every OG feature stays: the original Tracker is the baseline

Status: **Accepted** (maintainer, 2026-10-10: keep the functionality of the original tracker: granular, radio, capture, etc.).

## Decision
Every user-facing feature of the original Tracker (original firmware 1.1.14 and official 1.2–1.9 releases, tracked as parity rows, PAR-xxx) is kept in the Community OS. The ledger of those rows is the maintainer's and is not in this repository: a PAR number in a spec or a comment is a label, not evidence, and a parity claim needs the evidence named next to it (a vector test, a recorded observation, a section of the official manual). It may be redesigned to fit the new screens, track model and modules, but it is never silently dropped. This includes:
- sampler, granular and wavetable engines;
- the sample recorder with line/mic/radio sources, radio scanning, and capture/resample;
- the sample editor and its effects;
- performance mode, song mode, MIDI, export (WAV/stems), USB file transfer, and every FX command.

## Rules
- Each catalog row is triaged **keep** (same behaviour), **redesign** (same capability, new UI or engine) or **drop**. A drop needs the maintainer's explicit approval, recorded in the triage file.
- An OG feature counts as delivered only when its behaviour is reproduced. Acceptance comes from the OG spec in `docs/specs/`, recorded observed behaviour of the official firmware, a fixture, or a side-by-side check in a `hw-session`.
- Already decided: this OS ships a `diag` serial console (D-OS-6); a built-in serial chat console of the official firmware is not claimed here (open research question). Arkanoid leaves the core (D-OS-6) and may come back as an SD app module (ADR 0002).

## Consequences
Before the audio engine and UI phases, a triage file the OG feature triage maps all 159 catalog rows to keep/redesign/drop, together with the OS screen or module that hosts each one.

## Amendment 1 — maintainer drops and triage (2026-10-11)
- **Dropped by the maintainer:** PAR-059, 060, 062, 107, 124 and PAR-157 (the 10th Anniversary theme). The per-row verdicts are in the OG feature triage.
- **"USB file transfer":** the release notes of 1.4.0–1.9.2 document none (PAR-022: documented, not verified). It is open as triage Q-1. The default is a card reader only.
- **"Capture/resample":** capture is the recorder. Resample is new in the OS (product-ux §5.5).
