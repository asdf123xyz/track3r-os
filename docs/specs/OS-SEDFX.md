# OS-SEDFX: Sample edit, NORMALISE and DC REMOVE

Level B, S. Maintainer scope (2026-10-16): the sample editor gets two offline processes only, NORMALISE and DC REMOVE. Printing the live DSP (filter, bitcrush, delay, plate) is out, as the rest of PAR-017 / 019 / 020 / 114. **New Community OS behaviour** (the official manual describes a list of sample-editor effects, §8.10, not verified, BASE-16; the Community OS keeps its own screen, no parity claimed for either process).

## 1. What exists and what this adds

- NORMALISE already exists (OS-011a): Sample edit F2, `EditOp::Normalise` in `app/include/o3os/app/sample_dsp.h`. This item **specifies** it and changes nothing in it.
- DC REMOVE is new: `EditOp::DcRemove`, key **Shift + F4** (label `DC REM`; the slot was blank, so the Shift row of OS-SHIFTROW has no new rule: the key row had a free Shift key, F6 stays SAVE WAV).

Both run the screen's existing edit protocol (`screens/sample_edit_screen.h`, THE EDIT PROTOCOL): a `core::Job` on the main loop (`SampleEditJob`), a scan pass then a write pass, a slice of the pass at a time (`kFramesPerUs`, PROVISIONAL), into a spare buffer of the bank's edit pool, then the internal action `sample.commit` swaps it in as one undo unit and one step of the sample's own history (F8 UNDO / Shift + F8 REDO, three steps). The audio thread never sees a half-written buffer; no heap, no SD, no `double`.

## 2. Behaviour

- **Range:** the selection `[a, b)` when there is one, else the whole sample. Frames outside the range are copied as they are (a selection therefore can leave a step at its edges: fade it if needed).
- **Mono:** the bank holds mono 16-bit PCM (the importer mixes a stereo file to mono, OS-002c), so there is no channel to link; both processes work on that one signal.
- **NORMALISE:** gain so that the range's peak reaches **32767 (0 dBFS)**; gain Q16, held to 255x, rounded to nearest, never below unity, so a sample already at full scale, or silence, stays and the strip says `NOTHING TO NORMALISE` (no undo step). Target 0 dBFS: *default, to confirm by the maintainer* (no documented OG target is claimed).
- **DC REMOVE:** subtracts the **mean of the range**, rounded to nearest (half away from zero), computed over all of it before anything is written (a one-pole high-pass is not used: it would change the audio, not only the offset, and would need warm-up handling). **No clipping:** the offset is held to `[hi - 32767, lo + 32768]`, where `lo` / `hi` are the range's extremes; so every output frame stays in int16 and, in the rare case where the sample touches full scale on the side the offset would push towards, the removal is partial (down to nothing). An offset of 0 because the mean is 0 says `NO DC OFFSET TO REMOVE`; an offset of 0 only because full-scale frames held it back says `NO ROOM: SAMPLE AT FULL SCALE`; both make no step. A partial removal commits and says `DC PARTLY REMOVED: FULL SCALE LIMIT`.
- Progress is the strip's `EDIT NORMALISE` / `EDIT DC REMOVE` percentage over the two passes; another edit or import running says `ANOTHER JOB IS RUNNING`; no room in the pool says `NO ROOM FOR THE EDIT`.

## 3. Keys

| Key | Action |
|---|---|
| F2 | NORMALISE (`sedit.run 1`) |
| Shift + F4 | DC REMOVE (`sedit.run 7`, `EditOp::DcRemove`) |

## 4. Done when

1. Normalise: the range's peak is exactly 32767 afterwards; silence and a full-scale sample stay and make no step (host: `sample_dsp_test`, `sample_edit_screen_test`).
2. DC remove: a constant offset goes to 0; a tone plus an offset has |mean| < 1 afterwards; a selection leaves the frames around it byte for byte; silence is unchanged (host).
3. DC remove never clips (host: full-scale frames hold the offset back).
4. The result in chunks of any size equals the one-shot (host); the job's progress reaches 100 %.
5. One undo unit: undo restores the original bytes exactly (host, `memcmp`).
6. The key row with Shift reads `AMG 8K AMG 16K Q8 DC REM - SAVE WAV PRV EDGE REDO` (sim golden `qa_sample_edit/qa_sed_001`).
7. Unit (hardware): `platform/mk66/tools/hil/sample_edit_dc.json`: Shift + F4 on instrument 1 completes within seconds with the strip DONE, the sum changes, F8 brings back the first sum; the UI stays responsive meanwhile.

## 5. Out of scope

Stereo samples (the bank is mono), a high-pass option, a target level other than 0 dBFS, and every live-DSP printing process.
