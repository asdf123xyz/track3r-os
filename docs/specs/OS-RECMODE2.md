# OS-RECMODE2: Rec in Pattern, two keys and three states

Maintainer, 2026-10-14: the rec cursor movement in the Pattern view is wrong. The model below was proposed and the maintainer accepted it. **New Community OS behaviour**; it replaces the OS-RECSWAP mapping (0.4.11). Level B: keys, chips, the count-in trigger. No persistent format change.

## 1. The model

- **REC = step edit, on/off.** You edit at a free cursor; the song keeps playing if it is playing.
- **Shift+REC = live recording, on/off.** Notes are written at the play line and the view follows it.

There are three states, each always visible as a chip in the Pattern info line, using theme tokens in all three themes:

| State | Chip | Writes notes at | View |
|---|---|---|---|
| **WATCH** | `WATCH` | nowhere | follows the play line while playing |
| **STEP** | `STEP` (amber token) | the cursor | free cursor; the arrows move it |
| **LIVE** | `● LIVE` (red token) | the play line (OS-LIVEREC placement, REC QUANTIZE and REC MICRO rules) | follows the play line |

The red REC frame, badge and tinted playhead row (the earlier "REC more visible" item) appear only in **LIVE**. STEP has the amber chip only.

> Amended by OS-RECSIGN (`OS-PATUX.md`): the REC frame and badge now show in STEP as well, whenever the pattern can be edited; only the tinted playhead row stays LIVE's.

## 2. Transitions

Amended 2026-10-16 (OS-LIVEEXIT, maintainer decision of 2026-10-15; `docs/specs/OS-RECFLOW.md`): in LIVE the two keys are swapped. REC leaves recording and keeps following (WATCH); Shift+REC leaves it to the cursor on the current row (STEP). Both end the take. Every other cell is unchanged.

| Now | REC | Shift+REC | PLAY |
|---|---|---|---|
| **WATCH**, playing | STEP; the cursor stops on the play line's row | LIVE | stop → WATCH, stopped |
| **STEP**, playing | WATCH (following again) | LIVE | stop; stay in STEP |
| **LIVE** | WATCH; recording ends, the view keeps following the play line | STEP; recording ends and the cursor stays on the current row | stop; recording ends → WATCH, stopped |
| **WATCH**, stopped | STEP at the cursor; no playback | **play + count-in + LIVE** | play → WATCH, playing |
| **STEP**, stopped | WATCH | **play + count-in + LIVE** | play; stay in STEP |

- **The count-in happens only on Shift+REC from stopped.** This replaces OS-METRO §2's Pattern line ("Play pressed in Edit"). With COUNT-IN OFF, Shift+REC from stopped starts LIVE at once. PLAY never counts in. Focus Loop (Shift+Play) never counts in (unchanged).
- **A LIVE take is one undo unit** (OS-LIVEREC, unchanged). Leaving LIVE by any route closes it.
- **When the transport stops by itself** (end of song), LIVE ends exactly as with PLAY.
- **Block selection** (Shift+arrows) works in STEP, and in WATCH while stopped. While the view follows the play line (WATCH playing, LIVE) it works too (OS-BUG-WATCHSEL, 2026-10-23; new Community OS behaviour, it was refused silently): the block is anchored at the playhead's row at the first press, the arrows set its end, the cursor stays on the playhead and the block stays while the playhead moves on; a change of pattern under the playhead drops it, and so does a plain arrow.
- **On every screen but Pattern**, REC and Shift+REC keep their current meaning. The Record screen is unchanged.

## 3. Why (for the record)

- Live recording always writes where the play line is, and step edit always at the cursor. No state records live while the view looks elsewhere, so notes never land off-screen.
- Leaving LIVE with Shift+REC drops you into STEP on the spot, which is the usual next action: fixing the take (amended by OS-LIVEEXIT; REC from LIVE is WATCH, the plain way out of recording).
- Each key keeps one meaning in every state: REC is the cursor (and WATCH when there is nothing to edit), Shift+REC is live (and, from LIVE, the step-edit exit).

## 4. Implementation notes

- `pattern.edit_toggle` (REC) and `pattern.rec_free` (Shift+REC) in `app/src/bindings.cpp` and `pattern_actions.cpp` are remapped to the table above. `rec_free` becomes the STEP/LIVE distinction. Rename if it reads better, and keep the action ids stable or alias them.
- Notes from the pads, MIDI or the keyboard:
 - In STEP they go to the cursor step, even while playing (new Community OS behaviour for the playing case). Pads advance the cursor by JUMP (documented in the official manual, §5.8). MIDI keyboard notes (`step.midi_note`) go to the cursor WITHOUT advancing (this OS's rule: in edit mode the note is written to the step and the cursor never moves; whether the official firmware does the same is Q).
  - In LIVE they go to the play line.
  - Check `app.cpp:657` ("playing in Edit, at the playhead"): in STEP while playing they must go to the cursor.
- The count-in trigger moves from "Play in Edit while stopped" to "Shift+REC while stopped" (OS-METRO code path, METROALIGN display unchanged).
- Chip texts and colours use theme tokens with no literals. Update the goldens.

## Known limits

- `view.mode 0` (Edit) while stopped gives LIVE with no running transport (it is unbound by default; tests use it). The later fix: `pattern_set_mode(Edit)` while stopped sets `rec_free = true`. Not changed here.

## 5. Acceptance

- **Host and sim:** every cell of the §2 table as a test, plus a sim snap of the three chips in each theme.
- **QA rows:** QA-PAT, one per transition.
- **HIL:**
  - Shift+REC from stopped with COUNT-IN 1: the countdown shows, notes played after the bar line are recorded at the play line, and notes during the count-in are not.
  - Shift+REC during LIVE (OS-LIVEEXIT): STEP, the cursor stays on the row, and the take undoes as one unit. REC during LIVE: WATCH, the take ends, the view keeps following.
  - PLAY during LIVE: stopped, WATCH.
- **Docs:** update the OS-METRO §2 Pattern line and the UI map Pattern row.
