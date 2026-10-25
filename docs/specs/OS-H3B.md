# OS-H3B: instrument parameters in the edit table; CHORDS, HUMANIZE, MUTATE, MORPH

ADR 0014, slice H3, second half. Written on 2026-10-15. Line refs come from version 0.4.21 (which has H3B / ABI 1.6).

ADR L11 sets out the apps:
- CHORDS: "a progression into a track";
- HUMANIZE: "microtiming and volume jitter on a block";
- MUTATE / MORPH: "instrument parameters";
- MOD RECIPES: "modulation-matrix routes".

This is **new Community OS behaviour**. The ABI part is Level C (published and frozen); the apps are Level B. Size L. No persistent format change.

## 0. Scope rule: no ABI entry without a client in the same slice

- **Route entries move to H3c, with MOD RECIPES, after OS-MODMX.** There is no route in the model yet (no route type in `model` at version 0.4.21), so an entry now would freeze an interface nothing can test.
- **H3b adds only the instrument-parameter entries that MUTATE and MORPH use.**
- **CHORDS and HUMANIZE need nothing new.** They use the H3a step entries and **declare ABI 1.5**, so they also run on a 1.5 OS. An app declares the lowest ABI it needs; say so in `docs/guides/app-modules.md`.

## 1. ABI 1.6 (additive)

`O3M_ABI_MINOR` goes from 5 to **6** (`o3module.h:73`). New members are appended after the H3a ones, and the table `size` says which exist. There are no new capability bits.

### 1.1 Read table (needs `read_project`)

- `int32_t instrument_info(uint8_t instrument, o3m_instrument_info* out)`
  - The struct is `{size, kind /* EMPTY, SAMPLE, MIDI, MODULE */, engine /* sample play engine or module id hash */, param_count}`.
  - `instrument` uses the step numbering, 1..64.
- `int32_t get_param_desc(uint8_t instrument, uint16_t index, o3m_param_desc* out)`
  - The struct is `{size, id, min, max, def, flags, group, name[12]}`. `index` runs over 0..param_count−1.
  - `flags` carries STEPPED, ENUM and **APP_WRITABLE**.
  - `group` is VOICE, FILTER, ENV, SENDS or MOD for a sample instrument, or the page number for a module (S10 pages).
  - It comes from `core::ParamDesc` (`model/params.h:215`).
- `int32_t get_instrument_param(uint8_t instrument, uint16_t id, float* out)`

### 1.2 Edit table (needs `edit_project`)

`int32_t set_instrument_param(uint8_t instrument, uint16_t id, float value)`
- It runs through `Editor::set_param` (`editor.h:138`) inside the open group, the same path as the UI, so the engine hears it as it hears a hand edit.
- It refuses with `RANGE`:
  - an instrument that is empty or MIDI (MIDI fields are S8b's; they come later if an app needs them);
  - an unknown id;
  - a param without APP_WRITABLE;
  - a value outside min..max or NaN.
  Nothing is written for that call. STEPPED and ENUM values are rounded.
- **APP_WRITABLE** is set on continuous and stepped sound parameters. It is clear on structural ones: the sample, play mode, slice table, engine, module binding, and anything that reloads memory. The engineer marks the descriptor table and lists the clear ones in the review.

### 1.3 Limits

The H3a group rules apply unchanged: one group, one frame, 4096 calls, 2048 steps, and whole-group rollback (`docs/specs/OS-H3A.md` §1.4).
- A `set_instrument_param` counts as one call.
- The rollback restores each param's float bits exactly.

## 2. The apps (`modules/apps/<name>/`; each fits the module slab, with its size reported)

All four:
- appear in Pattern F7 TOOLS under EXPORT (H3a);
- take their target from the launch context;
- use F1 `APPLY`, one group per press, so each apply is one undo unit named after the app;
- exit with F7+F8;
- draw from seeded xorshift32 where randomness is used, so the same settings give the same result.

### 2.1 HUMANIZE (ABI 1.5; caps read_project + edit_project)

- **Target:** the block, or with no block the cursor track for 16 rows (as RHYTHM GEN). It acts only on steps with a note.
- **Values:**
  - `TIME` 0..100: the largest delay, as FX 2 Micro-move (`model/fx_table.h:43`, values 0..100). It delays only, as the FX does.
  - `VOL` 0..64: a ± jitter on the volume of a step that already carries one. An unset volume stays unset (review fold: the sequencer and the voices give such a step the instrument's own volume, and a written velocity would replace it, about doubling a volume-50 instrument). Repeated APPLYs jitter what they find, so the volume random-walks.
  - `SEED`.
- **FX slot:**
  - A step that already has Micro-move gets that slot's value replaced.
  - Otherwise the delay goes in an empty slot, FX2 first.
  - A step with both slots taken keeps its timing. The app shows `N STEPS: NO FREE FX SLOT`.
- **Keys:** F1 APPLY; F2 VARY (SEED + 1, then APPLY).
- **TIME 0 and VOL 0 change nothing**, so no undo unit is made.

### 2.2 CHORDS (ABI 1.5)

- **Target:** the block's first track and rows, or with no block the cursor track from the cursor row to the pattern end.
- **Values:**
  - `KEY` C..B;
  - `SCALE` MAJOR / MINOR;
  - `PROG`, one of 8 presets: I–V–vi–IV, I–vi–IV–V, ii–V–I, I–IV–V–IV, vi–IV–I–V, i–VI–III–VII, i–iv–v–i, the 12-bar blues;
  - `RATE`, rows per chord, default: the target rows divided by the number of chords;
  - `OCTAVE` 2..6;
  - `SEVENTHS` off / on;
  - `INSTR`, default: the context instrument;
  - `MODE` CHORD FX / SPREAD.
- **CHORD FX:** at each chord change row it writes the root note, the instrument, and FX 14 Chord. The value comes from the 30-entry table (`seq/fx.h` kChordMask, `docs/specs/OS-PFX.md`) and is the entry for the degree's quality (major, minor, diminished; with SEVENTHS, maj7 / min7 / dom7 / m7b5 where the table has them, else the triad).
  - It writes into FX2, or into FX1 if FX2 is taken on that row.
  - It empties the note on the other rows of the target.
  - The app keeps a copy of the chord-index map. A host test checks it against the OS table.
- **SPREAD:** one chord tone per track over the block's first 3 or 4 tracks (root, third, fifth, seventh), same rows, no FX.
- **Default mode:** CHORD FX on a MIDI instrument, SPREAD otherwise. **Unless** the engineer finds that the sequencer plays FX 14 on sample and module instruments too (`seq/fx.h`), in which case the default is CHORD FX everywhere. Record the finding.

### 2.3 MUTATE (ABI 1.6)

- **Target:** the context instrument. It must be a sample or module instrument; otherwise the app says `SAMPLE OR MODULE INSTRUMENTS ONLY`.
- **Values:**
  - `AMOUNT` 0..100 %;
  - `GROUP`: ALL or one group or page, from `get_param_desc`;
  - `SEED`.
- **APPLY:** for every APP_WRITABLE param in the group, new = old + (r − 0.5) × AMOUNT × (max − min), clamped, with r uniform in 0..1. STEPPED and ENUM params move by the rounded delta.
- **Keys:** F1 APPLY; F2 VARY (SEED + 1, then APPLY). AMOUNT 0 changes nothing.

### 2.4 MORPH (ABI 1.6)

- **Target:** the context instrument, A.
  - On launch the app copies A's writable params into its own state, as A0, which is far under its 4 KiB arena.
- **Values:**
  - `SOURCE` B: only instruments of the same kind and the same engine or module are listed. If there is none, the app says `NO MATCHING INSTRUMENT`.
  - `POS` 0..100 %.
- **APPLY:** A = A0 + (B − A0) × POS. A STEPPED or ENUM param switches to B's value at POS ≥ 50.
- **Keys:** F1 APPLY; F3 `RESET` (APPLY at POS 0).
- **No live sweep.** Every apply is an undo unit, and sweeping would flood the history.

## 3. Acceptance

**ABI:**
- Host fuzz of the four new entries: random instruments, ids and indices, NaN, NULL.
- Gating: without the capability bit the entries are NULL (amendment D C1).
- The 1.2 to 1.5 fixtures and apps still load. A 1.6 app on a 1.5 OS is refused with `NEEDS NEWER OS`.

**Undo property test, extended:**
- A param apply, then UNDO, restores every float bit-exact.
- A rollback restores them too.
- A structural param is refused.

**Apps**, through the fake host (`modules/tests/o3m_test_support.h`):
- HUMANIZE:
  - it respects the TIME and VOL bounds;
  - it replaces an existing Micro-move, or uses the free slot;
  - a step with both slots taken is counted;
  - the same SEED gives the same result;
  - 0/0 leaves no unit.
- CHORDS:
  - I–V–vi–IV in C gives C, G, A minor and F, with the right FX 14 values;
  - SEVENTHS;
  - the RATE split;
  - SPREAD voices;
  - the chord map matches the OS table.
- MUTATE:
  - bounds and groups;
  - AMOUNT 0 changes nothing;
  - the same SEED gives the same result;
  - non-writable params are untouched.
- MORPH:
  - POS 0 = A0 and POS 100 = B (continuous);
  - stepped params switch at 50;
  - the SOURCE filter.

**QA rows:**
- QA-PAT: the TOOLS list holds the four apps.
- QA-MOD: one row per app (apply, vary or reset, undo).

**HIL `qa_h3b.json`:**
1. CHORDS on a block, then the Pattern shot shows roots and FX 14.
2. HUMANIZE, then the shot shows Micro-move values.
3. MUTATE on a sample instrument; a Params page shot shows changed values; UNDO restores them.

**Docs:**
- `docs/guides/app-modules.md`: the 1.6 entries, the APP_WRITABLE rule and the lowest-ABI rule.
- ADR 0014 H3 line: H3b done; H3c (routes, MOD RECIPES) waits for OS-MODMX.
- BACKLOG: H3c row after OS-MODMX.

## 4. Built (2026-10-15, from version 0.4.21)

Everything in sections 0 to 3 is built and host-tested; nothing has run on a unit. The design record is ADR 0014 **amendment H** (read it for the reasons); the guide's sections 3.2, 3.3, 3.6 and 4 and the SDK README are the user-facing text.

**What was built**
- **ABI 1.6** (`module/sdk/o3module.h`, minor 6): `instrument_info`, `get_param_desc`, `get_instrument_param` (read table), `set_instrument_param` (edit table, joins the H3a group), `o3m_instrument_info` (16 B), `o3m_param_desc` (40 B), `O3M_INSTRUMENT_*`, `O3M_PGROUP_*`, `O3M_PD_STEPPED | ENUM | APP_WRITABLE`, `O3M_TABLE_HAS`. Appended after the 1.5 members; the offsets are asserted at compile time for the 32-bit target and tested on the host for 1.2 .. 1.6. `o3m_gen.py` and `o3m_pack.py` write and accept `"abi": "1.6"`.
- **Host** (`ModuleUi`, `tools_actions.cpp`, `model/params.cpp`): the four entries; the parameter write is an op in the group's buffer (one call, one of the 2,048 slots; no memory added), written at `edit_end` through `write_param`. `model::instrument_field_app_writable`: **61 of a sample's 83 rows** are writable, every parameter of a module instrument; the 22 clear ones are listed in amendment H.3.
- **Apps** (`modules/apps/{chords,humanize,mutate,morph}/`, packed like RHYTHM GEN, shared `apps/common/o3m_edit.h`): CHORDS and HUMANIZE ABI 1.5, MUTATE and MORPH ABI 1.6; .tmod 4,916 / 3,148 / 3,256 / 2,956 bytes, state 140 / 108 / 1,060 / 1,448 bytes.
- **FX 14 on sample and module instruments** (the question of section 2.2): the sequencer **plays the root alone**. `seq/src/sequencer.cpp` `pfx_note` builds the chord's other notes only `if (is_midi_slot(slot))` and counts `fx_ignored` otherwise; `seq/sequencer.h` says the same. So the default MODE is **CHORD FX on a MIDI instrument and SPREAD otherwise**.

**Deviations from this spec** (all recorded in amendment H)
1. **`o3m_param_desc.name` is 16 bytes, not 12** (a 12-character name needs 13 with its NUL; the descriptor is 40 bytes).
2. **APP_WRITABLE is a predicate in the model** (`instrument_field_app_writable`), not a flag on each row of the descriptor table; the descriptor an app sees carries it.
3. **A parameter write also takes one of the 2,048 buffer slots** of the group (it shares the step pool so that no memory is added), and a group takes at most 64 accepted parameter writes (`O3M_EDIT_MAX_PARAM_WRITES`, review fold: the rings behind the engine and a module take no more). Still one call.
4. **CHORDS asks `instrument_info` when the table has it** (it declares 1.5): without it the default is SPREAD. The spec's default (CHORD FX on MIDI) needs the instrument's kind, which only 1.6 can say.
5. **RHYTHM GEN's init checks the member it calls** (`O3M_TABLE_HAS`), not `sizeof` of the whole table, which would have refused an OS of 1.5 once built with the 1.6 header.
6. `instrument_info.engine` of a module is the FNV-1a hash of its stem.
7. **The progression of CHORDS plays once** (no loop); with no block SPREAD writes the one tone of the cursor's track.
8. **QA**: QA-MOD-080..089 (ten rows, the spec asked for one per app) and QA-PAT-210..213; the README counts recomputed by row count (366 functions, Modules 55, Pattern 130). No simulator golden changed (no tool app in the simulator's card), so `update_goldens` was not run.
9. **HIL**: the steps are in `qa_tools.json` (the H3a file), not a new `qa_h3b.json`; the H3a text about RHYTHM GEN being the first app below the rule now says it sorts after the four new apps.

**Sizes** (`ci.sh mk66`, the same compiler, against the previous build): flash image 715,972 to 717,636 bytes (**+1,664**, all `.text`: 687,804 to 689,468), `.data` 624 (0), SRAM `.bss` 132,552 (0), `.noinit_sdram` 82,816 (0), `.ramfunc_l` 27,512 (0: the 136 B left are untouched; nothing hot was added). SDRAM: unchanged (the 64 KiB host block of H3a holds the parameter ops in the step pool).

**Tests**: `tests/module_edit_test.cpp` (21 new cases: offsets 1.2..1.6, the 1.5 tables on a 1.6 OS, gating, the four entries, refusals, groups mixing steps / tempo / parameters, limits, the 2,048-write worst case, a fuzz, the undo property over 16 seeds, TOOLS and the four apps through the whole stack), `modules/apps/{chords,humanize,mutate,morph}/tests/` (15, 15, 11 and 11 cases, against the OS's chord table, an independent scale reckoning and a double-precision reckoning of the rules; the HUMANIZE and MUTATE results for a seed are pinned). `ci.sh host` (139 tests), `ci.sh qemu` (55) and `ci.sh mk66` (17, and the tone image) all exit 0.

**Left for the unit** (`qa_tools.json`, QA-MOD-080..089, QA-PAT-210..213): the list with five apps and the first-launch question for each; CHORDS on a MIDI instrument (the whole chord on the external synth) and on a sample instrument (SPREAD); HUMANIZE by ear; MUTATE and MORPH on a real sample and on the TB-303 (the Params page before, after and after UNDO; the running voice following); the legibility of the pages at 800 x 330; the main-loop stall of one APPLY of 61 parameters (`diag cpu`).
