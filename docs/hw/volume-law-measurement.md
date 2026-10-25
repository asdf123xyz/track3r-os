# Volume-law measurement: official firmware vs this OS (draft for the tester and the maintainer)

Status: procedure, not run. Needs the official firmware on the unit (maintainer).

## Why

Question: at the default settings, is this OS as loud as the official firmware, and do the level controls follow the same curve?

Our own evidence (no third-party source needed):
- Every real project of the fixtures (v11 to v16) stores **86** as the default of the 8 track faders, the reverb level, the delay level and the dry mix; the official 1.9.2 template of the published Polyend tracker-lib (MIT) stores 86 too. A factory default is normally the unity gain.
- This OS maps the mixer with the `Og1114` law (100 = unity, 0.4 dB a step), so its default 86 is about -5.6 dB, and the master cannot go above 0 dB. `docs/specs/OS-005-audio-engine.md` (level laws) says the mixer law is unsourced.
- Hypothesis H1: on the official firmware 86 is unity (0 dB) for the track faders. H2: the master volume has a unity point below 100 and can boost. H3: the instrument volume has 50 as unity (this OS already assumes it). These are hypotheses to test, not facts.

## What to measure (relative dB, plus one absolute number)

1. **Level at all defaults** (the number the listener hears): the same project, same sample, nothing touched.
2. **Track fader curve**: level against fader value.
3. **Master volume curve**: level against master value.
4. **Instrument volume curve**: level against instrument volume.
5. (optional) **Dry mix** 86 vs 100.

## Setup (identical on both firmwares)

Test sample: a sine of 1 kHz, -30 dBFS peak, 1 second, 44.1 kHz. A whole number of cycles, so it loops without a click. The low level keeps every combination below the limiter and the clip.

    platform/mk66/tools/o3tone.py gen sine1k_m30.wav --hz 1000 --db -30 --seconds 1 --rate 44100

Project (build it by hand on each firmware, the same way):
- Instrument 1: the sine, play mode LOOP FWD over the whole sample, instrument volume 50, panning centre (50), filter off, bit depth 16, sends 0, overdrive 0, amplitude envelope as it comes.
- Pattern 1: ONE note, C-5, instrument 1, step 1, pattern length 64 steps, tempo 120 BPM (a note held 8 s, the loop sustains it). No FX.
- Track 1 fader, master, dry mix: each cell below says what to set. Everything else at the default of the firmware. Write down the default master value shown (the official firmware may show dB, this OS shows 0..100).
- Play from the start, wait 2 s, capture 3 s (inside the held note, before the pattern wraps at 8 s). Stop the transport between cells.
- No other track plays. Metronome off. Limiter and saturation at their defaults, `clean`.

## Capture

Digital (preferred): the unit's USB AUDIO, master slot of `usb map`:

    platform/mk66/tools/o3audio.py list
    platform/mk66/tools/o3audio.py record --seconds 3 --save cell_<id>.wav

Read the RMS dBFS of the channel that carries the master. Peak is a check (a 1 kHz sine: peak = RMS + 3.01 dB).

If the official firmware does not present a USB audio device (open research question: not tried), use the analog line out into a fixed audio interface and the same recording program for BOTH firmwares. Then add a calibration step, because analog gain is not digital dBFS:
- C0. Our image, defaults, captured **twice**: USB (digital) and line out (analog). The difference is the interface's offset; apply it to the official firmware's analog numbers.

Never change the interface gain between cells. Write the interface and gain in the report.

## Matrix (one row = one capture; record RMS dBFS)

Level at defaults:

| Cell | Firmware | Fader 1 | Master | Instrument vol | RMS dBFS |
|---|---|---|---|---|---|
| D1 | official | default | default | 50 | |
| D2 | this OS | default (86) | default (86) | 50 | |

Track fader (master default, instrument 50):

| Cell | Fader 1 | official RMS | this OS RMS |
|---|---|---|---|
| F1 | 100 | | |
| F2 | 86 | | |
| F3 | 70 | | |
| F4 | 50 | | |
| F5 | 30 | | |
| F6 | 10 | | |
| F7 | 0 | | |

Master volume (fader 86, instrument 50). The official firmware: hold `Master` and turn the encoder, note the displayed value (dB or number) next to each cell:

| Cell | Master (displayed) | official RMS | this OS RMS |
|---|---|---|---|
| M1 | maximum | | |
| M2 | default | | |
| M3 | -6 dB from default | | |
| M4 | -12 dB from default | | |
| M5 | minimum | | |

Instrument volume (fader 86, master default). Watch the peak: at 100 this OS adds +24 dB, so -30 dBFS stays below clip; if either firmware clips, lower the sample level for this block and say so:

| Cell | Instrument vol | official RMS | this OS RMS |
|---|---|---|---|
| I1 | 100 | | |
| I2 | 75 | | |
| I3 | 50 | | |
| I4 | 25 | | |
| I5 | 10 | | |

Optional dry mix: D3 dry 100, D4 dry 86 (fader 86, master default).

## Reading the results

- Per block, subtract the reference cell (F2, M2, I3) from every other cell: the curve in dB against the value, on each firmware. Compare the two curves.
- Slope: dB per step between neighbours. A constant slope is a linear-in-dB law (this OS's `Og1114` is 0.4 dB a step); a changing slope is a table.
- Unity: the value where the official firmware's output equals the sample level minus the pan-centre loss. The pan law is unknown on both sides, so the safe statement is the **relative** one: "official default is X dB louder or quieter than this OS's default" (D1 - D2).
- H1 predicts: official F2 (fader 86) is about 5.6 dB above this OS's F2, and the official F1 (fader 100) is above its F2 (the official curve keeps rising past 86), where this OS's F1 is 5.6 dB above its own F2. H2 predicts: the official master at its maximum is above its default level, which this OS cannot do. H3 predicts: both firmwares agree at I3 (instrument 50) and differ only in slope elsewhere. Any other result is a finding too: report it as measured.

## Report back

For each cell: firmware, version, values set, RMS and peak dBFS, the capture file name. Then the three curves and D1 - D2. Do not claim parity from one block; name what stays unmeasured (pan law, sends, velocity). If this OS's numbers differ, do not change any law: file backlog rows (one per control) with the measured table as evidence, for the maintainer to move.

## Rules

- Read-only for firmware: no flashing in this procedure. Installing a candidate image or the official image on the unit goes only through the documented update path (`fw-gate-check` first, or the maintainer's own hands for the official firmware).
- Nothing from a Polyend source goes into the report except what we measured ourselves.
