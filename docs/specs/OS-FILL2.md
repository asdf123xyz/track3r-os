# OS-FILL2: value modes and FX in the Fill dialog

Status: implemented 2026-10-18 on `feat/OS-FILL2` (Q1-Q5 as recommended; D7-D13 were taken during the build, to confirm). Backlog: APPROVED row 5. Level B, no format change: the dialog's state is session state, and a fill writes the step layout that exists. This is **new Community OS behaviour**. The fill's value types are named VALUE / RANGE / RANDOM so far (OS-009). Whether the official firmware's fill has the same value types or names is an **open research question** (Q5); the official manual is not read for it (maintainer, 2026-10-18).

## Goal

Today the F4 FILL dialog (OS-PTOOLS part 1) writes one value: the cursor's note, the edit instrument, or the cursor's volume. This item adds three things:

- **VALUE MODE**: CONST, RAMP or RANDOM.
- **FROM and TO** rows.
- **FX 1 and FX 2** as targets in WHAT, with an FX TYPE row.

The dialog stays at 8 rows.

## 0. What exists (dev c5e5c2b)

- **Rows.** Seven, from `kRows[]` in `app/src/fill_view.cpp:22-30`: SCOPE (BLOCK, TRACK, PATTERN), WHAT (NOTE, INSTR, VOLUME), TYPE (EVERY N, RANDOM, EUCLID, SCALE), N (1..32; the caption is EVENTS for EUCLID), DENSITY (0..100 %), ROOT (0..11) and SCALE (0..39, `core::kScaleNames`, where 0 is "No scale").
- **Unused rows** are drawn dim, show `-`, and ignore the encoder (`fill_row_used`, `fill_view.cpp:43-52`). No row is hidden or swapped today, except N's caption.
- **The model** is `model/include/o3os/model/pattern_fill.h` (`What` :19, `Type` :20, `Plan` :37-45) and `model/src/pattern_fill.cpp`.
  - `select()` picks the hit steps: EVERY and SCALE every Nth step, RANDOM by DENSITY, EUCLID by EVENTS.
  - `apply()` writes the value to each hit with the single-step rule `{true, true}`. Steps that are not hit are left as they are.
- **Note values today.**
  - EVERY and EUCLID write the cursor's note, moved up into the scale (`scale_up`).
  - RANDOM picks a random in-scale note from the one-octave window starting at the cursor's note, and never the same note twice in a row.
  - SCALE walks up that window and wraps.
  - A note past 127 leaves the step as it was (PAR-150).
- **Session state.** `FillView` (`plan`, `scope`, `row`, `valid`, `rng`) lives in `ViewState::fill`, keeps its settings for the session, and is not saved.
  - The RNG is a xorshift32 that is never re-seeded.
  - The preview line uses a fresh RNG.
- **Undo.** A fill is one undo unit: one `set_steps` per changed track, at most about 28.7 KB, against a Pattern ring of 160 KiB.
- **FX.** Each step has two columns `{type, value}` (`model/include/o3os/model/step.h:42-52`).
  - The FX table (`model/include/o3os/model/fx_table.h`) gives each type its min, max and default. `kPickerOrder[60]` is the picker order, where index 0 is None.
  - `edit::fx_type_set` and `edit::fx_value_set` set the type and clamp the value (`model/src/step_edit.cpp:209-257`).
  - The picker's names, `kFx`, are file-local in `app/src/screens/step_popup_overlay.cpp`.
  - `append_fx_value` (`app/include/o3os/app/fx_text.h`) formats the special types. Every other type is a plain number.
- **Note names.** The note-name text is file-local in `app/src/pattern_view.cpp:253`.

## 1. Behaviour

### 1.1 The rows

Eight rows in a fixed order. Row 6 changes its meaning with WHAT, and rows 4 and 7 change their caption. An unused row is dim, as today.

| # | Row | Values | Used when |
|---|---|---|---|
| 1 | SCOPE | BLOCK · TRACK · PATTERN | always |
| 2 | WHAT | NOTE · INSTR · VOLUME · FX 1 · FX 2 | always |
| 3 | TYPE | EVERY N · RANDOM · EUCLID · SCALE | always |
| 4 | N / EVENTS / DENSITY | N 1..32 (EVERY N, SCALE), EVENTS 1..32 (EUCLID), DENSITY 0..100 % (RANDOM) | always; the caption follows TYPE |
| 5 | MODE | CONST · RAMP · RANDOM | always, except NOTE with TYPE RANDOM or SCALE (1.3) |
| 6 | KEY / FX TYPE | **KEY** for NOTE: root and scale in one row, e.g. `C# Dorian` (Q3); **FX TYPE** for FX 1 / FX 2: every picker type except None, in picker order | NOTE, FX 1, FX 2 |
| 7 | FROM (caption VALUE in CONST) | WHAT's domain (1.4) | always |
| 8 | TO | WHAT's domain | RAMP, RANDOM, and NOTE with TYPE RANDOM or SCALE |

- **Two rows become one.** Rows 4 and 5 of today (N and DENSITY) merge into row 4, because a type only ever uses one of them. `plan.n`, `plan.events` and `plan.density` stay separate fields.
- **KEY.**
  - The encoder and Left/Right step the scale, in a ring of 40 (D9). Shift+encoder steps the root, one place in a ring of 12, so the scale's roll-over never moves the root (D5, D9).
  - With "No scale" the row shows `No scale` and the root plays no part.
- **FX TYPE.** Changing it puts FROM and TO at the new type's default (D3).
- **Size.** The dialog grows by one row pitch: 8 rows, then the preview, its count and the key row. The key row stays F1 FILL and F8 BACK.

### 1.2 The modes

Each track counts its own hits, and K is a track's hit count. FROM may be above TO.

- **CONST.** Every hit gets FROM.
- **RAMP.**
  - Hit k of K (k = 0..K−1) gets `(FROM·(K−1−k) + TO·k + (K−1)/2) / (K−1)` in integer maths, and FROM when K = 1.
  - The first hit is exactly FROM and the last is exactly TO.
  - Notes are moved up into the scale with `scale_up`. A note that cannot be placed at 127 or below leaves its step as it was (PAR-150).
- **RANDOM.**
  - Each hit gets a uniform value from `min(FROM,TO)..max(FROM,TO)`, drawn from the `FillView` RNG.
  - Notes are drawn from the in-scale notes of that range, never the same twice in a row when there are two or more (today's rule).
  - For INSTR, VOLUME and FX the draw is plain uniform (D4).

### 1.3 Notes with TYPE RANDOM or SCALE (Q1)

- These two types already choose the notes, so MODE is dim for them and FROM..TO is their **window** (the candidate notes), in place of today's fixed octave.
- RANDOM picks in the window and SCALE walks up it and wraps, as today.
- **Defaults reproduce today exactly.** With FROM = the cursor's note and TO = FROM + 11, both types give today's results, and a host test pins that down.
- **Larger windows.** The candidate array grows from 12 to 128 entries (128 B on the stack, main loop).

### 1.4 Domains and defaults

| WHAT | FROM / TO range | Text | Default at each open (Q2) |
|---|---|---|---|
| NOTE | OG note 0..127 | note name (the pattern grid's, made public) | FROM = the cursor's note, else the last played, else C-5 (today's rule); TO = FROM + 11, at most 127 |
| INSTR | 1..64, in the `instrument_walk` order | two digits, as the grid | FROM = TO = the edit instrument |
| VOLUME | 0..127 | number | FROM = the cursor's volume, else 127; TO = 0 |
| FX 1 / FX 2 | the FX type's min..max | `append_fx_value` when it formats, else the number | FROM = TO = the type's default; FX TYPE = the cursor step's type in that column, else the first picker type |

- Shift+encoder on FROM and TO uses a coarse step: 12 for notes, 10 for the others.
- **Why TO = 0 for VOLUME:** with TO = 0, picking RAMP alone already gives an audible fade-out.

### 1.5 Writing FX

- **The write.** A hit step gets the type with `fx_type_set`, then the value with `fx_value_set`. That is the fill's every-step rule, so a step whose column holds another type is replaced (Q4).
- **Single-value types** (Off 0..0, Break 1..1) take that value whatever FROM and TO say.
- **Placeholder types.** None exist today; if one appears, FX TYPE does not list it.

### 1.6 Messages

`say_fill` adds the mode and its range, for example:

- `FILLED 8 STEPS: VOLUME RAMP 127-0`
- `FILLED 4 STEPS: FX 1 RANDOM 0-100`

Notes print as names. Texts that name a key come from `app/src/key_prompt.cpp`'s tables, because `tools/check_keytext.py` fails on a literal key name anywhere else.

## 2. Decisions

One table. **Q** rows are for the maintainer: the recommended option comes first and is applied until answered. **D** rows are defaults taken in this spec, to confirm.

| # | Question or default | Options (recommended first) | Applied |
|---|---|---|---|
| Q1 | TYPE RANDOM and SCALE already choose the notes | a) keep them as they are, with MODE dim for them and FROM..TO as their window, so today's results are unchanged at the defaults · b) TYPE only picks the steps; random notes come from MODE RANDOM, and SCALE becomes a MODE value WALK (changes QA-PAT-086..095) | a |
| Q2 | FROM and TO when the dialog opens | a) taken from the current value at every open, so CONST is today's fill exactly · b) kept for the session like the other rows | a |
| Q3 | ROOT and SCALE as one KEY row, to stay at 8 rows | a) one KEY row, `C# Dorian`; the encoder steps the scale and Shift+encoder steps the root · b) two rows, and the dialog grows to 9 | a |
| Q4 | An FX fill over a step whose column holds another type | a) replaced, as the fill's every-step rule · b) only empty columns and columns of the same type are written | a |
| Q5 | The mode names. Ours so far are VALUE / RANGE / RANDOM (OS-009); the official firmware's names are an open research question | a) CONST / RAMP / RANDOM (the backlog's) · b) VALUE / RANGE / RANDOM | a |
| D1 | N and DENSITY share row 4 | the caption follows TYPE; the fields stay separate | applied |
| D2 | RAMP maths | integer, per track, endpoints exact, descending when FROM > TO | applied |
| D3 | FX TYPE change | FROM = TO = the new type's default | applied |
| D4 | No-repeat rule | notes only (today's); INSTR, VOLUME and FX draw plain uniform | applied |
| D5 | Coarse steps | 12 for notes and 10 for the other FROM/TO rows; for KEY the Shift step is the root, one place in a ring of 12 (D9) | applied |
| D6 | Shared names | the picker's `kFx` names and the grid's note-name text move to small public helpers, so the dialog reuses them | applied |
| D7 | The strip's message (1.6) | today's `FILL: <rule>` stays (QA-PAT-087, 088 and the tests read it) and a suffix says what was written: `FILL: EVERY 1 STEP, VOLUME RAMP 127-0`, `FILL: EVERY 1 STEP, FX 1 RANDOM 0-100`, `FILL: EVERY 1 STEP, NOTE RAMP C-5 TO B-5` (notes by name with TO, numbers with a dash; a note name has a dash itself). No suffix for a CONST note, instrument or volume, nor for the note window of RANDOM and SCALE. The `FILLED 8 STEPS:` form of 1.6 is not used | applied |
| D8 | A change of WHAT in the open dialog | FROM and TO (and FX TYPE) start again from what the cursor held at the open, so an edit of one WHAT is lost when the row is left; the same WHAT keeps its edit. One `Value` is kept in `FillView`, 5 bytes | applied |
| D9 | KEY (1.1) | Left/Right (the encoder) step the scale in a ring of 40 with the root unchanged; Shift + encoder steps the root in a ring of 12 (a separate step in `fill_step_coarse`, no linear 480-place value, so the scale's roll-over never moves the root). "No scale" and "Chromatic" take every note, so the row shows no root for them | applied |
| D10 | INSTR in RAMP and RANDOM | the instrument numbers 1..64 are interpolated and drawn as numbers; only the FROM and TO rows step through the instrument order (64..49, then 1..48) with `instrument_walk` | applied |
| D11 | A note window with FROM above TO (RANDOM, SCALE, NOTE with MODE RANDOM) | the same window as TO..FROM | applied |
| D12 | FX TYPE when the cursor step holds no usable type in that column | the first picker type, Off (0..0): the dialog then fills Off until a type is chosen | applied |
| D13 | `pattern.fill` before any dialog fill (the OS-CFGWIRE path) | FROM is the note in hand, as the fill always did; the dialog's session values are not read or changed | applied |

## 3. Edge cases

- **One hit:** RAMP writes FROM.
- **No hits** (DENSITY 0, or EVENTS above the length): `NOTHING TO FILL`. Today's texts are kept.
- **FROM = TO:** RAMP and RANDOM behave like CONST.
- **A note window with no in-scale note:** the steps stay as they were. That is today's PAR-150 rule.
- **FX 1 or FX 2 over an empty step:** only the FX column is written. The note, instrument and volume stay.
- **BLOCK scope** with the cursor step alone writes FROM in every mode.
- **`pattern.fill` (repeat last)** repeats with the stored plan and stored FROM/TO. Q2's reset happens only when the dialog opens.

## 4. Persistence, real time, budget

- **Persistence:** none. No `.o3pj` or `.o3st` field, and no new step layout.
- **Real time:** main loop only. The loops are bounded: at most 16 tracks × 128 steps.
- **Size [est]:** FLASH +2-3 KB; SRAM 0, because `FillView` lives in the App's SDRAM; stack +116 B for the larger window.
- **CPU per audio block:** 0.

## 5. Done when

1. `tests/pattern_fill_test.cpp` covers:
   - RAMP up and down with exact endpoints, K = 1, and the integer rounding;
   - RANDOM within the bounds, deterministic for a seeded RNG;
   - notes ramped into the scale, and a note past 127 left alone;
   - windows larger than an octave;
   - FX type and value written and clamped, including over another type;
   - **legacy equivalence**: with FROM = cursor note and TO = FROM + 11, TYPE RANDOM and SCALE give the same steps as before for the same seed.
2. `tests/fill_view_test.cpp` covers the 8 rows, the used/dim rule per WHAT, TYPE and MODE, the captions, KEY's rollover and root step, and FX TYPE's list.
3. `tests/pattern_edit_app_test.cpp`: the Fill dialog tests use row names, not row numbers, and pass. Q2's reset at open is covered.
4. Simulator:
   - the Fill block of `sim/scenarios/qa/qa_pattern.txt` is updated for the new row order;
   - new snaps: a VOLUME RAMP, an FX 1 RANDOM and a NOTE KEY row;
   - goldens are regenerated with `tools/update_goldens.py`, and `budget_test` stays under 1792.
5. QA catalogue `docs/qa/catalogue/pattern.md`: QA-PAT-086 now says eight rows, and new rows use QA-PAT-300..319. PAT-OPS takes 280..299, and 277..279 are left for fixes. `docs/ui/ui-map.md` §4 Fill row and the manual's `pattern_tools.md` are flagged for the manual loop.
6. `tools/ci.sh host` is green, the `mk66` build has no warnings, and the deltas are in the commit body.

## 6. Hardware-only checks

On the panel, the 8-row dialog and `C# Dorian` on the KEY row are legible. A VOLUME RAMP fade is heard as expected. A one-slice check, with no bench.

## 7. Files

- `model/include/o3os/model/pattern_fill.h`, `model/src/pattern_fill.cpp`
- `app/include/o3os/app/fill_view.h`, `app/src/fill_view.cpp`, `app/src/fill_actions.cpp`, `app/src/screens/fill_overlay.cpp`
- the shared names: `app/include/o3os/app/fx_text.h` (or a new `fx_names.h`), `app/src/screens/step_popup_overlay.cpp`, `app/src/pattern_view.cpp`
- tests, sim and docs as in Done when

## 8. State for implementer

Order:

1. The model first (`Plan` gains `mode`, `from`, `to` and `fx_type`; `What` gains `Fx1` and `Fx2`), with `pattern_fill_test`.
2. Then the rows and the view.
3. Then the actions and the messages.
4. Then sim and docs.

Apply the Q rows' recommended options until the maintainer answers. A new choice goes into this spec as a D row, to confirm. No format change is allowed: if one seems needed, stop and report.
