# OS-PTOOLS part 1: the Pattern Fill dialog

Level B (pattern-edit logic + one overlay; no RT path, no file format change). Sources: `docs/ui/ui-map.md` (Fill dialog row, section 4 item 3, section 5), PAR-053, PAR-073, PAR-093, PAR-150 (release-note items: documented, not verified). Part 2, **F7 TOOLS** (a list of block operations, EXPORT first), is NOT in this slice: F7 stays as it is.

## 1. UI home (budget rules, ui-map section 5)

- Pattern **F4 FILL** (was free) opens the **Fill overlay** in Edit (outside Edit it says `FILL: TURN EDIT ON`). Overlay = a modal dialog of 7 descriptor rows (limit 8), the F-key row drawn by the overlay: **F1 FILL**, **F8 BACK**. Up/Down choose a row, Left/Right or the encoder change its value. No overlay opens another.
- **`pattern.fill`** (no key since OS-QJUMP: Shift + Fx was its key and is the 3/4 jump now) **repeats the last fill** with the dialog's settings (a random fill draws again). Before any fill it keeps the OS-CFGWIRE behaviour: the current note on every Nth step, N = the edit step.
- Rows are `core::ParamDesc` descriptors (name, range, default, step), so the clamp, the step and the text are the generic ones. They are session state, not project data (OS-009 D5); not Learn targets.

| Row | Values | Used by |
|---|---|---|
| SCOPE | BLOCK / TRACK / PATTERN | all. BLOCK = the selection (the cursor step with none); TRACK = the cursor's track; PATTERN = every track. Each track is cut at its own length (D7) |
| WHAT | NOTE / INSTR / VOLUME | all. The value is the current one: the cursor's note (else the last played, else C-5), the edit instrument, the cursor's volume (else 127) |
| TYPE | EVERY N / RANDOM / EUCLID / SCALE | selects the steps (and, for NOTE, the notes) |
| N / EVENTS | 1..32 | EVERY N and SCALE: N; EUCLID: EVENTS |
| DENSITY | 0..100 % | RANDOM only |
| ROOT, SCALE | C..B, the 40 scales | WHAT = NOTE only |

## 2. Rules

Our rules: EVERY n selects `offset % n == 0` counted from the first step of the range, n 1..32; the written step follows `note_set` (an empty step takes the last instrument, one with an instrument keeps it); a fill never writes outside 0..127 (PAR-150).

**New Community OS behaviour** (no historical parity is claimed for any of these; the 1.3.0 rounding rule is UNKNOWN):
- **EUCLID**: step i of N is a hit when `(i * E) mod N < E`, E clamped to N (E = 0: none). This is the Bresenham-even distribution, a rotation of Bjorklund's necklace, with the first step always a hit: E = 3, N = 8 gives `x..x..x.`; E = N fills every step; the hit count is exactly E. Not "round to nearest".
- **RANDOM**: each step is chosen with probability DENSITY % (default 30, our choice; the official firmware's rate is an open research question, Q-P1-3 of OS-009). A note fill writes a random in-scale note from the octave of the current note, never the same note twice in a row when the window has two or more.
- **SCALE**: every Nth step takes the next in-scale note of one octave from the current note, ascending, repeating.
- EVERY and EUCLID write the current note moved up to the next in-scale note; no in-scale note up to 127: the step is left as it was (PAR-150).
- No duplicates (PAR-073): each step is written once, and no random or scale note repeats its neighbour.
- **Scale fixes** (PAR-073, standard definitions, the masks of `core/scales.h`): Melo Min = melodic minor 0 2 3 5 7 9 11 (was Dorian, a copy of row 4); Blues Maj = 0 2 3 4 7 9; Todi = 0 1 3 6 7 8 11; Gipsy = Gypsy minor 0 2 3 6 7 8 11 (was a copy of DoubHarm; the same notes as Hung Min). This changes the pad board for those rows too; the names are unchanged, so no stored index changes meaning.

## 3. Undo and tests

A fill is ONE undo unit (`pattern.fill` and `fill.apply` are undoable actions; the steps go through `set_steps`). Tests: `tests/pattern_fill_test.cpp` (Euclid table incl. 0, N, E > N, every-N, density bounds, the four scales, no duplicates, scope) and `pattern_edit_app_test.cpp` (the dialog, pattern.fill, undo restores). QA rows `QA-PAT-086..095` (snaps `qa_pat_086..093`) in `docs/qa/catalogue/pattern.md`, snaps in `qa/qa_pattern.txt`, HIL step in `qa_pattern.json`.

## 4. Open

Q1 Euclid rounding: whether the official 1.3.0 build rounds to nearest or as this rule does is an open research question. Q2 FX fills and from/to ranges (VALUE / RANGE / RANDOM value types) are not in this slice. Q3 BLOCK with no selection acts on the cursor step.
