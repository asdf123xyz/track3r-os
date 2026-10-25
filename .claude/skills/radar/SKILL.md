---
name: radar
description: Turn the maintainer's raw ideas (words like "303", "OctaMED", "sample chops", "MIDI learn", "Amiga filter") into researched feature-radar cards for the Community OS — open-source prior art, licence, CPU/RAM/flash cost on the MK66, where it lives (core flash, SD plugin, SD data pack, script), UX value and priority. Use whenever the maintainer throws ideas, products or projects at you.
---

# Feature radar

The radar is the maintainer's idea funnel. Ideas become cards, cards feed `decide` (ADRs) and the phase plan; nothing gets built straight from a word.

**Files.** Matrix: `docs/vision/radar.md` (create it on first use by importing the rows of `docs/vision/community-os.md` §Feature radar, then link back; never edit community-os.md for this). New ideas that pass triage also become rows in `docs/roadmap/BACKLOG.md` as IDEA; only the maintainer moves them on. One card per idea: `docs/vision/radar/<slug>.md`.

## Steps
1. **Normalise.** Split the maintainer's message into ideas, give each a slug, dedupe against `radar.md`. Existing card → update it only if the maintainer added something new.
2. **Fan out.** One `radar-researcher` (Sonnet) per 2–3 related ideas, all in one message, background. Pass: the slugs, the maintainer's raw words, the card template below, and "report back ≤150 words".
3. **Matrix.** Add or refresh one row per card in `radar.md`:
   `| Idea | Inspiration | OSS (licence) | CPU | RAM | Flash | Lives in | UX value | Priority | Card |`
4. **Tell the maintainer** in 10 lines or fewer: new rows, the 1–3 most promising, anything that needs a maintainer decision (licence, scope). Do not start implementation; promising items go to `decide` or the phase plan.

## Card template
```
# <Idea> — radar card
Maintainer's words: "<raw>" · Date · Status: idea | candidate | planned | rejected
## What it is            (1–3 lines, musician's view)
## Prior art             (project · URL · licence · language · last activity — facts only)
## Approach              port | adapt | inspire | too heavy — and why
## Cost on MK66 (estimate until measured)
  CPU: cycles/sample or /block → % of 180 MHz at 44,117 Hz per voice/instance
  SRAM / SDRAM / flash bytes; where it lives: core flash | SD native plugin | SD data pack | SD script
## Fits                  which OS layer (audio engine, sequencer, UI, I/O, importer) and dependencies
## UX sketch             2–4 lines: how it is played on pads/encoder/screen
## Licence               compatible? (permissive: yes with attribution; GPL: only if the project licence allows (GPL-3.0-or-later for the OS, MIT for the module SDK); proprietary: inspiration only)
## Risks / open questions
## Priority              ⭐1–5 with one-line reason
```

## Rules
- Research only: never copy third-party code into the repo at this stage; cite URLs; mark every number as *estimate* or *measured (hw-session date)*.
- CPU estimates show their arithmetic. Anything that must be measured becomes a bench item for the next `hw-session`.
- Favour SD plugins / data packs over core flash for anything not needed at boot (maintainer direction: extend the tracker from the SD card).
- Out of scope by maintainer decision: NES engine, timestretch. Do not re-propose them.
