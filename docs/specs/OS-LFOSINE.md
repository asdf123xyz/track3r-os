# OS-LFOSINE: LFO shape Sine

Status: spec, before implementation · Level C (a persistent format and the audio path) · Classification: **new Community OS behaviour** (maintainer, 2026-10-23; the official firmware has five LFO shapes and no parity is claimed).

Numbering note: the brief says "shape 6". The model counts from 0 (Rev Saw 0, Saw 1, Triangle 2, Square 3, Random 4), the choice row is a dense list, so the sixth shape is **value 5**. This spec uses 5 (a value 6 with a hole at 5 would need a label for 5 and buys nothing). Say so if 6 was meant.

## 1. Where the shape is stored

- One byte, `Lfo::shape` (`model/include/o3os/model/instrument.h`), six LFOs per sample instrument (`SamplerParams::lfo[6]`: amp, pan, cutoff, WT position, grain position, finetune).
- **.o3pj**: the `INST` chunk, `formats/src/native_codec.h` `encode_sampler` / `decode_sampler`, 6 x (shape u8, speed u8, amount f32) at payload offset 164 of the sampler block (`docs/formats/o3-project.md`). There is no other native instrument file: an instrument is an INST chunk (the project, and what the instrument browser copies). Settings (`.o3st`) hold no LFO shape.
- **Live path**: the same value, as the float of `inst_param::lfo(slot, LfoShape)` (ring entry), held to range by `apply_instrument_param`.
- **OG**: byte 0 of each 8-byte LFO record of the .pti (offset 212), `formats/src/og_instrument.cpp`.

## 2. Today's loader, and an older OS with a newer file

- Today the native decoder stores the byte **as it is** (no range check; `validate_instrument` does not look at it). The engine holds a shape above 4 to Triangle (`sanitize_mod`, `apply_instrument_param`, the draw in `params_screen.cpp`). So a stored 5 (or 6..255) plays and draws as Triangle today and is kept in the file on the next save.
- The OG reader also copies it verbatim.
- An OS that does not know INST 1.4 reads a 1.4 chunk as a **newer minor** of a known major (`docs/formats/o3-project.md`, "Newer minor": same layout, nothing appended, so it reads all of it): the shape 5 is kept, plays as Triangle, and is written back under its own (older) version on the next save. The older OS cannot play or label it (the byte stays in its model and is saved back, under the older version). Not verified on an older image; the row text of such an OS is its own UI.

## 3. The version bump

- **INST 1.4 (`kInstVersionLfoSine = 0x0104`) adds no field and no byte** (the rule of PERF/SCEN 1.1, decision D5 of OS-PERFFX): the layout is 1.3's, whole (engine byte and the twelve MIDI bytes always present), and the version only says that a shape of 5 is Sine.
- **Writer**: 1.4 only for an instrument one of whose six LFOs has shape 5 (`model::inst_uses_sine`), else 1.3, 1.2 or 1.1 exactly as before: a project that never uses Sine is byte for byte the file it was. Size formula: `v >= kInstVersionMidi` already selects the 1.3 bytes.
- **Reader**: knows minor 4. A chunk of minor < 4 holding a shape of 5 or more reads it as **2 (Triangle)**, which is what it played; a chunk of any minor holding a shape above 5 reads 2 (a later shape plays as Triangle, as the engine does today). Only a 1.4+ chunk keeps 5.
- **OG import**: a shape above 4 reads as 2 (OG has five; this OS never holds a stray 5 from an OG file that would now play as Sine).
- Tests: round trip (shape 5 on every slot, 1.4 chunk, byte equality of the second save); old-file load (a hand-built 1.3 chunk with shape 5 gives 2, a 1.3 file without Sine is unchanged and still written as 1.3); a 1.4 chunk with shape 9 gives 2; a file without Sine carries no 1.4.

## 4. OG export

- `.pti` (and every .pti of an OG project export) writes shape **2, Triangle**, for each LFO of shape 5 (`InstrumentWriter::build`). The rest of the instrument is untouched.
- Counted, not silent (ADR 0006): `ExportDrops::lfo_sine` counts instruments with at least one such LFO (`any_lost()` includes it); `og_loss_items` adds the kind **brief "SINE LFO", full "SINE LFO SHAPES WRITTEN AS TRIANGLE"** (`kOgLossMax` 14 to 15); it appears in the notice line of the export dialog and the file screen's loss list like the others.

## 5. Evaluation

- Control rate only: the generator evaluates at the cutoff's every 16th sample and at the other slots' grid points (`flfo_level`, `kFilterCtl = 16`); nothing per sample. No libm, no `double`, no table.
- Phase `u` (0..1) is the transport's, as for the other shapes. Level (before the amount) is `s(u) = (1 - cos 2 pi u) / 2`: 0 at u = 0, 1 at u = 0.5, the Triangle's phase and amplitude convention, so the output `2 x level - A` is `-A cos(2 pi u)` and the tremolo level runs 0..A. The default amount, sync speeds, amount range, live edit and destinations are those of every shape.
- `s(u) = 0.5 + 0.5 sin(2 pi v)` with `v = u - 0.25`, folded (`v > 0.25: v = 0.5 - v`) to `[-0.25, 0.25]`, and an odd quintic `sin(2 pi v) ~ v (c1 + c3 v^2 + c5 v^4)` with `c3 = -41.0695`, `c5 = 73.1833` and `c1 = 4 - c3/16 - c5/256` (6.28097), the peak exact. Minimax fit on the quarter wave: **maximum error 8.1e-5 of the sine (4e-5 of the level, -88 dB)**, held by a host test against `std::sin` (limit 1e-4 of the sine, in float). Result clamped like the others: `clampf(s x A, -1, 1)` (a negative A mirrors; the UI range is 0..1).
- The existing five shapes: no code path of theirs changes (a new `case 5` before `default:`), `tests/audio_lfo_equiv_test.cpp` hashes unchanged. The tracker waves FX (vibrato / tremolo `tracker_wave`, wave 0 "sine" mapped to Triangle) is **not changed** here: follow-up if the maintainer wants it to use the new shape.
- UI: label `Sine` in the SHAPE choice (6 entries), the curve under the rows shows a 16-step window (it drew two periods before; see OS-LFO-GRAPH below).

## 6. Cost

- Over Triangle: one fold, one compare and about 6 multiplies/adds, about 15 cycles per evaluation (estimate, not measured). Evaluations: 4 per 64-frame block per generator, at most 5 generators per voice and 12 voices: 240 x 15 = 3,600 cycles of 261,000 per block (1.4 %) with **all** slots of all voices on Sine; a typical case (1 slot, 4 voices) 240 cycles (0.09 %). No RAM (ModGen stays 44 bytes), flash about 100 bytes. **Unit check left**: the CPU measurement of a Sine-filter-LFO scene and a listen (no zipper at the 16-sample control rate, the peak and trough).

## 7. OS-LFO-GRAPH

- The MOD page's LFO picture is a fixed 16-step window, not two periods: it shows cycles = 16 / period (the tempo-sync rate in steps), so the SPEED row moves the picture. 128 steps is an eighth of a cycle, 16 steps one, 1/2 step 32.
- Past 32 cycles (a period under half a step) a cycle is under 10 px: the picture is a filled band over the shape's full range 0..A. New Community OS behaviour.
