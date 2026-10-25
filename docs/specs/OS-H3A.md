# OS-H3A: apps edit the project (typed edit table, undo groups), F7 TOOLS, RHYTHM GEN

ADR 0014 slice H3, first half. Written on 2026-10-14. The ADR gives the outline:
- L21: the edit table;
- L30: undo groups and limits;
- L32 and L47: launch context and F7 TOOLS;
- L54: tests;
- L63: "RHYTHM GEN first";
- amendment D (L106-128): H1 as built.

Line refs are from `0.4.18`. **New Community OS behaviour.**

**Level C for the ABI** (it is published and frozen once released), Level B for the rest. Size M/L. No persistent format change.

**H3b** (later): `set_instrument_param`, the route entries (after OS-MODMX), CHORDS, HUMANIZE, MUTATE/MORPH and MOD RECIPES.

## 1. ABI 1.5 (additive)

`module/sdk/o3module.h` is at `O3M_ABI_MINOR 4` (L69). H3a raises it to **5**. Every new entry is appended after the existing ones, and each table's `size` tells an app which entries exist (`O3M_HOST_HAS`, L450). The capability bits stay as frozen in amendment D: `read_project` 2, `edit_project` 8.

- An app built for 1.5 on an OS older than 1.5 is refused at load with `NEEDS NEWER OS`.
- The engineer checks the existing minor-version rule and adds a test.

### 1.1 The step POD

`o3m_step`, 8 bytes:

```c
typedef struct o3m_step {
  uint8_t note, instr, vol;
  uint8_t fx1_type, fx1_value, fx2_type, fx2_value;
  uint8_t reserved; /* 0 */
} o3m_step;
```

- The values use the model's encodings (`model/step.h:21-52`): note 0 empty, 1..128, 253 fade, 254 cut, 255 off; vol 255 unset.
- `instr` uses the step byte as the Pattern shows it, 1..64, with 0 = none.
- The header names each value as a constant (`O3M_NOTE_OFF` …).
- A write takes a **field mask**: `O3M_STEP_NOTE 1`, `INSTR 2`, `VOL 4`, `FX1 8`, `FX2 16`. Only the masked fields change, so a generator that writes notes keeps the existing FX.

### 1.2 Read table (needs `read_project`), appended to `o3m_read_api` (L345-353)

- `int32_t pattern_info(uint16_t pattern, o3m_pattern_info* out)`. The struct is `{size, length /* rows, 1..128 */, tracks /* active, ≤ 16 */}`.
- `int32_t get_steps(uint16_t pattern, uint8_t track, uint8_t first_row, o3m_step* out, uint8_t count)`. It copies out; the app never sees the model.

### 1.3 Edit table (needs `edit_project`), appended to `o3m_edit_api` (L363-368, after `set_tempo`)

- `int32_t edit_begin(void)`
- `int32_t edit_end(void)`
- `int32_t set_step(uint16_t pattern, uint8_t track, uint8_t row, const o3m_step* s, uint8_t fields)`
- `int32_t set_steps(uint16_t pattern, uint8_t track, uint8_t first_row, const o3m_step* s, uint8_t count, uint8_t fields)`. Count ≤ length − first_row, the Editor's own rule (`editor.cpp:528`).

New error codes sit beside the existing `NO_PROJECT` and `RANGE`: `NO_GROUP`, `GROUP_OPEN`, `LIMIT`, `RATE`.
- **Bad arguments** (a pattern that does not exist, a track ≥ active tracks, a row or count out of range, a NULL pointer, an invalid value, an unknown field bit) return `RANGE`, and that call writes nothing.

### 1.4 Undo groups (ADR L30)

- **Opening and closing:**
  - `edit_begin` opens one Editor group: `begin_group(module.app_edit, <app slot>)`, `model/editor.h:66`.
  - Only one group is open at a time. A second `edit_begin` returns `GROUP_OPEN`.
  - A write outside a group returns `NO_GROUP`. There are no implicit units: one apply is one group.
- **A group never spans app frames.** If a group is still open when the app's frame returns, the host closes it, keeps its changes, and counts and logs it once. Undo and redo are Busy inside an open group (`editor.h:49`), so the user's UNDO is never blocked.
- **Limits, per group:**
  - at most 4096 calls;
  - at most 2048 steps written (16 KB of 8-byte steps, the ADR's journal cap).
  The write that crosses either limit, and every later write in that group, returns `LIMIT` and writes nothing. At `edit_end` (or at frame end) **the whole group is rolled back** and `edit_end` returns `LIMIT`: the project is exactly as before. If the Editor has no abort, add `Editor::abort_group()`: it closes the open unit, undoes it and leaves no redo entry.
- **Rate:** at most one group per app frame. A second `edit_begin` in the same frame returns `RATE`.
- **Undo units:**
  - A group that changes nothing leaves no undo unit.
  - The unit is named after the app, so UNDO's notification reads `UNDO <APP NAME>`.
- **Edits while the song plays are allowed**, exactly as a hand edit on the main loop.
- **`set_tempo` joins an open group**, as amendment D.6 planned for H3: inside a group it is applied at once, as part of that undo unit. Outside a group it stays queued and becomes its own unit `module.app_tempo`, as today.

### 1.5 Launch context

`o3m_launch_ctx` (L333-341) gets one appended field, `uint8_t block_tracks`; its `size` tells whether it is there.

From TOOLS, the context carries:
- `pattern`: the Pattern's pattern;
- `track`: the block's first track;
- `block_tracks`: how many tracks the block covers;
- `block_start` and `block_length`: the block's rows;
- `instrument`: the current instrument;
- `flags`: `O3M_LAUNCH_FROM_TOOLS`.

With no block (`Selection::active` false, `app/pattern_edit.h:22`), `block_length` is 0: the app decides what to do from the cursor (`track`, `block_start` = the cursor row).

`launch_app(entry, project, ctx)` already takes the context (`module_ui.cpp:661`); TOOLS is its second caller.

## 2. F7 TOOLS

- **Built here, or by OS-WAVEXP slice A, whichever lands first.** The contract is in `docs/specs/OS-WAVEXP.md` §3.2: Pattern F7, a list overlay, Up/Down, F1 run, F8 BACK, refused while the view follows the play line, `EXPORT` first.
- **Below EXPORT and a divider come the installed apps that declare `edit_project`.** They come from the module scan the Modules screen already makes, sorted by name.
  - If the list has no app, it shows one dim line: `NO TOOL APPS ON THE CARD`.
- **Running an app:** F1 launches it with the context above. The first-launch confirm applies once per boot (amendment D.7).
- **Exit:** F7+F8 returns to Pattern with the cursor and block unchanged (one level of return, UI map rule 8). The edits are on screen at once.

## 3. RHYTHM GEN (app, `modules/apps/rhythmgen/`, ABI 1.5, caps `read_project` + `edit_project`)

ADR L11: "polyrhythm, probability, variations".

### 3.1 Lanes

- **One lane per track of the block, up to 4.** Tracks beyond the fourth stay untouched, and the header says `LANES 4 OF n`. With no block there is one lane, the cursor track, covering 16 rows from the cursor row, or to the pattern end if that is sooner.
- **Per lane:**
  - `HITS` 0..LENGTH;
  - `LENGTH` 1..64, default the block length capped at 64; the cycle repeats over the block;
  - `ROTATE` 0..LENGTH−1;
  - `NOTE`: by default the first note already in the lane's block, else C-4;
  - `PROB` 0..100 %, default 100.
- Different LENGTHs on different lanes give the polyrhythm.
- **Shared:**
  - `INSTR`, default the context instrument;
  - `VOL`, unset or 0..127, default unset;
  - `MODE`: REPLACE writes the hits and empties the note on every other row of the lane's block; ADD writes only the hits and keeps existing notes;
  - `SEED` 0..9999.

### 3.2 Rule

- **Hit:** row `i` of the cycle is a hit when `(i × HITS) mod LENGTH < HITS`. This is the Euclid rule of the Fill dialog (`docs/specs/OS-PTOOLS.md:25`, `model/pattern_fill.h`), so both give the same pattern. The cycle is then rotated by ROTATE.
- **Probability:** each hit is kept with probability PROB. The draw is xorshift32 seeded from `SEED` and the lane, so the same settings always give the same steps.
- **Fields written:**
  - on a hit: NOTE, INSTR, and VOL if set;
  - on a non-hit row in REPLACE: NOTE = empty;
  - FX are never touched.

### 3.3 Keys and pages

- The pages come from the descriptor table, at most 8 rows each (UI map rule 2): pages 1–4 are lanes 1–4 (a lane with no track is dimmed), page 5 holds the shared values.
- F1 `APPLY` writes one group (one `set_steps` per lane), so it is one undo unit.
- F2 `VARY` does `SEED + 1`, then APPLY: a fresh variation and a fresh undo unit at each press.
- F7+F8 exits.
- There is no preview: you hear the result back in Pattern, and UNDO reverts it.
- It must fit the module slab (14 KiB). Report its size.

## 4. Tests and acceptance

- **Host fuzz** of every new entry (ADR L54): random patterns, tracks, rows, counts, masks and NULLs. It never crashes, never writes out of range, and returns `RANGE` or `NO_GROUP` as specified.
- **Undo property test**, extending `undo_test.cpp:239`:
  - any run of groups followed by the same number of UNDOs restores the project byte for byte, and REDO re-applies it;
  - a group rolled back for `LIMIT` leaves the project identical and no redo entry;
  - an empty group leaves no unit;
  - a group left open at frame end is closed.
- **Gating:** without `edit_project`, the edit table is NULL (C1 rule, amendment D); without `read_project`, the read additions are NULL. A 1.2 fixture and the 1.3 and 1.4 apps (Tap Calc, MIDI Monitor, Loudness) still load.
- **TOOLS:**
  - the list shows the `edit_project` apps under EXPORT;
  - the context carries the block (`block_tracks` included);
  - exit returns to Pattern with the block kept.
- **RHYTHM GEN**, through the fake host (`modules/tests/o3m_test_support.h`):
  - 3/8 equals the Fill dialog's Euclid;
  - ROTATE;
  - PROB 0 and PROB 100;
  - the same SEED gives the same steps;
  - ADD keeps existing notes;
  - FX are untouched;
  - one APPLY is one undo unit;
  - four lanes with different LENGTHs.
- **QA rows:** QA-PAT (TOOLS lists apps, launch with a block), QA-MOD (RHYTHM GEN apply, VARY, undo).
- **HIL** `qa_h3a.json`:
  1. Block of 2 tracks × 16 rows, then TOOLS, then RHYTHM GEN, then APPLY.
  2. Exit; the pattern shows the hits.
  3. UNDO restores.
- **Docs:**
  - `docs/guides/app-modules.md` §3.3 and §3.6: replace the `[TBD]` names and PLANNED marks with the real ones. §4: point to RHYTHM GEN as the worked example.
  - ADR 0014: one line in H3 saying H3a is done and what H3b keeps.

## 5. Built (2026-10-15, from version 0.4.19)

Everything in sections 1 to 4 is built and host-tested; nothing has run on a unit. The design record is ADR 0014 **amendment G** (read it for the reasons); the guide's section 3.3 and 3.6 and the SDK README are the user-facing text.

**What was built**
- **ABI 1.5** (`module/sdk/o3module.h`, minor 5): `o3m_step`, `pattern_info` / `get_steps`, `edit_begin` / `edit_end` / `set_step` / `set_steps`, `block_tracks`, the codes `NO_GROUP` 3, `GROUP_OPEN` 4, `LIMIT` 5, `RATE` 6, the limits 4,096 calls / 2,048 steps. Appended after the old members; the header ends with the byte offsets of the 32-bit target as compile-time assertions. `o3m_gen.py` and `o3m_pack.py` write and accept 1.5 (`"abi": "1.5"`). An app of 1.5 on an OS of 1.4 is refused at load, and the launch says `NEEDS NEWER OS`.
- **The group** (`ModuleUi`, `module.app_group`): checked at each call, buffered, applied together at `edit_end` as one undo unit named after the app, rolled back by never being applied; closed and applied by the OS when the call returns with it open and when the app is stopped; `set_tempo` joins it; UNDO says `UNDO <APP NAME>`.
- **F7 TOOLS** (`tools_list.h`, `tools_actions.cpp`, `screens/tools_overlay.cpp`, binding and label on Pattern): the tool apps below the fixed part (empty here; OS-WAVEXP puts EXPORT PATTERN in it), launch with the block, the way back to Pattern.
- **RHYTHM GEN** (`modules/apps/rhythmgen/`): 4,596 bytes of file (the slab is 14 KiB), 136 bytes of state, no hot code.

**Deviations from this spec** (all recorded in amendment G)
1. **No `Editor::abort_group()` and no journal change.** The group is buffered in the module system and applied at `edit_end` by an ordinary undoable action (the Editor is private to `dispatch()`); the rollback of a dropped group is that nothing was ever written. A read inside a group sees the project before it; `edit_end` OK means applied.
2. **`pattern_info` also returns `track_length[16]`.** Rows are valid inside the track's own length (the editor's rule, D7), not the pattern's.
3. **A bad argument (RANGE) does not poison the group**; only LIMIT drops it (section 1.3 says the call writes nothing and the group goes on).
4. **Groups are made from `input()` only**; `edit_begin` from `enter`, `exit`, `set_param` or `view_draw` answers `RATE` (so does a second group in one call).
5. **RHYTHM GEN has no OS parameters**: its pages are drawn by the app (the app page lists at most eight parameter rows and has no paging). Values are not remembered between launches. The default NOTE is the step byte 49, which the Pattern names C-4 (the model's own default 60 shows as C-5).
6. **The time `edit_end` takes is credited back to the app's 4 ms** (`ViewHost::credit_host_us`), which the spec did not foresee.
7. **`NEEDS NEWER OS`** is the launch text for the loader's existing ABI-minor refusal (the loader and its quarantine are unchanged).
8. **HIL file**: `qa_tools.json` (the task allowed it), not `qa_h3a.json`.
9. **Pattern's F7 label reads `TOOLS`**, which changed 192 goldens of the simulator (every Pattern shot shows the key row; 162 in 11 scenarios, 30 in the gallery) and added two (`qa_pat_167`, `qa_pat_167_shift`); regenerated and looked at.
10. **Memory**: the module system's host block (SDRAM, start of the module region) grew from 32 to 64 KiB for the group buffer (about 26 KB); the modules' arena is 32 KiB smaller.

**Sizes** (`ci.sh mk66`, the same compiler, against the previous build): flash image 680,096 to 684,852 bytes (**+4,756**), `.text` 651,928 to 656,684, `.data` 624 (0), SRAM `.bss` 132,032 (0), `.noinit_sdram` 82,624 (0), `.ramfunc_l` 27,512 (0: the 136 B left are untouched; nothing hot was added). SDRAM: the module system's block 32 to 64 KiB inside the fixed 768 KiB module region.

**Tests**: `tests/module_edit_test.cpp` (29 cases: the layouts, the gates, the codes, the group, the limits, the abandoned and stopped groups, the undo property over 24 random seeds, a 600-frame fuzz of every entry, TOOLS, Rhythm Gen through the whole stack) and `modules/apps/rhythmgen/tests/rhythmgen_test.cpp` (18 cases, with the Fill dialog's Euclid as oracle and a layout check of every page). `ci.sh host` with `O3OS_CI_SCOPE=changed` and `ci.sh mk66` both exit 0.

**Left for the unit** (`qa_tools.json`, QA-PAT-160..167, QA-MOD-070..079): F7 on the real key row, the list and the first-launch question, a block of 2 tracks x 16 rows as the app's context, one APPLY and one UNDO on a real project, the time of an APPLY of four lanes (`diag cpu`; the main loop stalls for it, the app's budget does not count it), the legibility of the app's page, and the sound of the generated rhythms.

