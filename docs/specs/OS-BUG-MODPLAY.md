# OS-BUG-MODENGINE and OS-BUG-MODPLAY: the engine at load, and S3M period slides

Status: spec, 2026-10-18. The findings come from E2E-9, "Found, not changed" F1-F4 (`docs/specs/OS-E2E.md` on `feat/OS-E2E-9`). Every decision here was **decided by the orchestrator under the maintainer's delegation, 2026-10-18** ("simple, pragmatic, choices that save time"). There is no question table.

| ID | What | Decision | Level |
|---|---|---|---|
| OS-BUG-MODENGINE (F1) | a load binds every sample on the Modern engine, whatever its instrument says | fix, at **every load** | B |
| OS-BUG-MODPLAY F4 | S3M period slides are sized at C-4 = 1712 whatever the sample's C2SPD | fix | C (audio path, `tools/ci.sh qemu`) |
| OS-BUG-MODPLAY F2 | MOD slides also run on tick 0 | PARKED, a known difference of imports | — |
| OS-BUG-MODPLAY F3 | a volume on a row with no note does not change the sounding note | PARKED, a known difference of imports | — |

## 1. OS-BUG-MODENGINE: the saved engine counts at load

**Today.** `OgSampleLoader::bind` (`app/src/og_samples.cpp:505-537`) is the one bind path for native `.o3pj` loads, OG loads and MOD/S3M/MED imports.

- It builds the voice descriptor with `make_sample_desc` (`og_samples.cpp:140`), which leaves `SampleDesc::engine` at `kEngineModern` (`audio/include/o3os/audio/voices.h:483`).
- It never calls `SampleBank::engine_for` (`app/src/sample_bank.cpp:448-459`).
- So every instrument plays Modern after a load, until something requests a rebind: the PLAY ENGINE row, a parameter edit, an undo, and so on. The same project therefore sounds different before and after an unrelated edit.
- An imported MOD or MED, which the importer writes as AUTO, plays on the sampler until each instrument is stepped. QA-FIL-005 and QA-FIL-006 claim the Amiga engine without that step.

**Fix.** One line in `bind`, before `set_sample`:

```cpp
if (bank_ != nullptr) desc.engine = bank_->engine_for(slot, *in);
```

- Tests may pass a null `bank_`, and then the descriptor stays Modern as today.
- `sim/src/sim_audio.cpp:280` (the no-card test bank) is not a load, and it is not changed.

**What sounds different after the fix** (the expected change; the CHANGELOG says so, §4):

- **(a)** An instrument whose PLAY ENGINE is set explicitly to AMIGA, 12-BIT or 8-BIT now loads with that engine. It gets the engine the user chose; before the fix it got Modern until an edit.
- **(b)** An AUTO instrument whose sample is at exactly 8,287 or 8,363 Hz, in play mode 0 or 1, now loads on Amiga, as the AUTO rule says (`engine_for`).
- **Nothing else changes.** An instrument set to MODERN stays Modern. So does an AUTO instrument at any other rate.
- **S3M.** An S3M imported for the Modern engine stays Modern, because the importer writes MODERN explicitly (`formats/mod/src/map.cpp:417-419`; QA-FIL-006).
- **Users' projects cannot be counted:** the criterion is (a) or (b).

**Persistence.** None changes. The INST engine byte (`docs/formats/o3-project.md:144`) is read as today; it now takes effect at load.

## 2. OS-BUG-MODPLAY F4: S3M period slides at the sample's C2SPD

**Today.**

- The Modern tracker-FX processor bases each note's period on the fixed table: `k.note_period = trkfx::period_of_note(view.note)`, with C-4 = 1712 (`audio/src/tracker_fx.cpp:222`, `tracker_fx.h:76-83`).
- The sample's rate never enters. So a fine portamento `FF4` moves 16 units on 1712, about 16.2 cents.
- Scream Tracker and libopenmpt move it on the period that the sample's C2SPD gives: at C2SPD 11025, 21.5 cents.
- The same base serves every period effect (the coarse and fine slides, tone portamento), so they all share the error away from 8,363 Hz.

**Fix.**

- `TrackerView` (`voices.h`, filled by `tracker_view`, `voices.cpp:1604`) gains the voice's sample rate in Hz, from `samples_[slot].rate_q28`.
- `tracker_fx.cpp:222` scales the base: `note_period = period_of_note(note) * 8363 / rate`, in integer maths, rounded.
- **Exemption (D1):** when the rate is 8,287 or 8,363 Hz, the scale is 1. A MOD at the PAL rate 8,287 Hz therefore keeps today's base and today's slides exactly.
- **Rates out of range:** clamp the rate to 1,000..96,000 Hz, so the period stays in range.

**Where it is safe.** `corefx_openmpt` compares at C2SPD 8363, where the fix changes nothing. `tracker_view` and the processor must not add bytes to `.ramfunc_l`: check the map and `tools/check_ramfunc.py`.

## 3. PARKED: F2 and F3, known differences of imported modules

Both stay as they are. The E2E-9 journeys keep them pinned (`tests/e2e_openmpt_ref.py`: `ours_tick0` and the `known` cases), and the importer's report keeps naming F2 ("slide timing", `kDropSlides`).

- **F2. MOD slides on tick 0.**
  - **The difference.** A MOD imported for the Amiga engine runs `1xx`, `2xx`, `3xx` and `Axy` on tick 0 as OctaMED does. That is one tick of slide more per row than ProTracker and libopenmpt:
    - `104` is +16.3 cents on its first row and +34 cents on its second;
    - `305` is about +21 cents a row;
    - `A03` is 3/64 of the level a row (0.047).
  - **The switch exists**, `AmigaVoices::set_slide_tick0`, but nothing sets it.
  - **Why parked.** Setting it per song needs a persisted flag, which is a format change. Rescaling the slides at import cannot be exact: the factor is (speed−1)/speed in integer period units, Fxx changes the speed during the song, and 3xx stops at its target.
- **F3. A volume on a row with no note.**
  - **The difference.** MOD `Cxx` alone, the S3M volume column alone, and MED `0C` alone change the sounding note in libopenmpt (to 0.5, 0.25 and 0.5 of the level in the three songs). Here they change nothing: 1.00 against 0.50 is 6 dB, and 1.00 against 0.25 is 12 dB.
  - **Why.** The sequencer reads a step's volume only where a note starts (`seq/src/sequencer.cpp:1384, 1828`), and no voice reads the Volume group.
  - **Why parked.** Acting on it would change how native projects play, because the Vol column is the Community OS's own. A per-import switch would need a persisted flag.

## 4. Done when

1. **MODENGINE host tests** (extend `tests/og_samples_test.cpp` or `tests/play_after_load_test.cpp`):
   - **The round trip:** a project saved with instruments on AMIGA, 12-BIT, MODERN and AUTO (one AUTO sample at 8,363 Hz and one at 44,100 Hz), then reloaded, has each slot's `SampleDesc::engine` equal to `engine_for` with no edit.
   - **An imported MOD:** its instruments are on Amiga at once.
   - **An S3M imported for Modern:** it stays Modern.
2. **F4 host test** (`tests/audio_tracker_fx_test.cpp`):
   - `FF4` at C2SPD 11025 moves 21.5 ± 0.5 cents;
   - at 8,363 Hz and at 8,287 Hz the movement is unchanged from today;
   - a coarse slide and a tone portamento at C2SPD 11025 scale the same way.
3. **The E2E-9 pins are updated:**
   - `e2e_ref_mod` no longer needs the PLAY ENGINE steps, and its pinned 259 Hz take becomes the arpeggio;
   - the F4 case in `tests/e2e_openmpt_ref.py` moves from `known` to agreeing;
   - QA-FIL-005 and QA-FIL-006 hold without a step.

   Whichever lands second (this fix or the E2E-9 merge) updates them.
4. **`docs/CHANGELOG.md`, Unreleased:**
   - one entry for MODENGINE that names cases (a) and (b) as the expected change in sound;
   - one entry for F4 (S3M slides at C2SPD ≠ 8,363 Hz now match Scream Tracker);
   - one "known differences of imported modules" entry for F2 and F3, with the numbers above.
5. **Gates:**
   - `tools/ci.sh host` is green;
   - F4 also needs `tools/ci.sh qemu` and a `.ramfunc_l` delta ≤ 0 in its commit body;
   - the `mk66` build has no warnings.

## 5. Hardware-only checks

- **MODENGINE:** an imported MOD plays its arpeggio on the unit right after the import, and again after Save and reopen. This is QA-FIL-005 by ear.
- **F4:** an S3M at C2SPD 11025 with `FFx`, heard against a reference.

## 6. State for implementer

- **Batch:** one branch, `fix/OS-BUG-MODPLAY`, with two commits: MODENGINE (Level B), then F4 (Level C). One `firmware-reviewer` for MODENGINE; a risk review plus one reviewer for F4.
- **MODENGINE first:** it is one line plus the round-trip test.
- **F4:** add the rate to `TrackerView` and scale the base, with D1's exemption.
- **D1, confirmed by the orchestrator under the maintainer's delegation, 2026-10-18:** the Paula-rate exemption keeps every MOD's slides as today. A finetuned MOD sample whose rate is not a Paula rate is scaled by 8363/rate, which is at most about 1 % off ProTracker's base.
- **Do not touch** `set_slide_tick0` or the Volume group: F2 and F3 are PARKED.
- **Size [est]:** FLASH under 200 B in all; SRAM 0; CPU per audio block 0 (one multiply-divide per note start in the processor).

## 7. Built (branch `fix/OS-BUG-MODPLAY`, 2026-10-18)

- **MODENGINE (commit 1).** The one line, in `OgSampleLoader::bind`; tests in `tests/save_app_test.cpp` (the round trip of AMIGA, 12-BIT, 8-BIT, MODERN and AUTO at 8,363, 8,287 and 44,100 Hz with no edit; an imported MOD on Amiga after the import and after Save and reload) and `tests/module_import_test.cpp` (MOD and OctaMED on Amiga at once; an S3M at 8,363 Hz stays Modern when imported for Modern and is Amiga when imported for Amiga). Both fail without the line. The E2E-9 pins: the F1 take is gone, the three Amiga journeys take no PLAY ENGINE step, QA-FIL-005 and QA-FIL-006 lost the caveat.
- **F4 (commit 2).** `VoicePool::TrackerView::rate_hz` (from `rate_q28`, `rate_hz_of_q28`), `trkfx::note_period_at`, used where the processor starts a note (`TrackerFx::tick`). `tests/audio_tracker_fx_test.cpp`: the arithmetic, FF4 at 11,025 Hz moves 21.46 cents (libopenmpt 21.5), at 8,363 and 8,287 Hz the 16.26 cents of before. The S3M journey's case "fine portamento up FF4 (C2SPD 11025)" moved from `known` to strict (184 comparisons, 0 disagree).
- **Two points where the build differs from the text above, both for the reviewers.**
  1. **Only the fine portamento is a period effect in the Modern processor.** The S3M `E` and `F` slides and `G` tone portamento are imported as MED slide and Glide commands (approximated, `docs/formats/mod.md`) and never touch `Trk::period`, so "a coarse slide and a tone portamento scale the same way" (section 4) has nothing to test: they do not take the base. The test covers the fine portamento (FX 49 and 50) and its sum with the arpeggio.
  2. **A sample with no recorded rate, or at 44,100 Hz (`rate_q28` 0), keeps the table.** Section 2 does not say what the rate is for those, and the choice is not neutral: scaling them by 8363 / 44100 would make FX 49 and 50 five times larger on every native and OG sample, which changes the sound of saved native projects and contradicts the documented "C-4 = 1712" of the FX. So `rate_hz` 0 means "none", exempt like the Paula rates. The cost: an S3M whose C2SPD is exactly 44,100 Hz is not fixed (no importer test file has one). To scale those too, drop `rate_hz == 0u` from `note_period_at` and change the expectations of the "keeps the table" test lines; the decision is the maintainer's.

## 8. Review fold (2026-10-18)

- **MAJOR, the Modern import left AUTO.** A MOD imported with `EngineFx::Off` (F7 "FX MODRN") and an OctaMED song imported with `engine_fx` off kept `engine` AUTO; with the load now binding `engine_for`, their Paula rate (8,287 / 8,363 Hz) sent them to Amiga, which runs none of the core FX written for the Modern engine. Fixed: `mod::Importer` writes MODERN whenever `!report_.engine_fx` (it was S3M only, `formats/mod/src/import.cpp`), `med::Importer` writes MODERN for a sample instrument when `!opt_.engine_fx` (`formats/med/src/med_import.cpp`). Tests: `modimp_import_test` (a MOD with `Off` is MODERN), `module_import_test` (a MOD and an OctaMED song imported for Modern are MODERN in the project and on the Modern engine in the voices; for Amiga both stay AUTO and are Amiga); both fail without the fix. The CHANGELOG no longer says "only two cases change": (c) is the Modern import, and a MOD or OctaMED song imported for Modern with an earlier build and saved as AUTO now loads on Amiga (named there, with the remedy).
- **Minors.** The F4 accuracy is one number everywhere: 1.74 cents between the players on every tick of the FF4 case in `e2e_ref_openmpt_s3m` (libopenmpt 0.7.3; tolerance 1.5, 3 on a note's first tick), 21.46 against 21.5 for the step itself; parity is claimed at 11,025 Hz only. The three rates that keep or hold the table (C2SPD 44,100, 8,287, above 96,000 Hz) are listed as known differences beside F2 and F3. `save_app_test` has the AUTO row at 8,363 Hz in a play mode above 1 (ping-pong: Modern), and a forward loop at 8,287 Hz (Amiga).
