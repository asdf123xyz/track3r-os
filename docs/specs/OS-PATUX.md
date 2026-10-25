# OS-PATUX: Pattern editing, small batch 1

Level B. New Community OS behaviour unless a section says otherwise (no OG parity claimed). No persistent format changes. One commit per ID.

## OS-RECSIGN: the REC signs follow "can be edited"

**Goal.** The REC signs (the badge in the top strip and the three-pixel frame round the grid) show whenever the pattern can be edited, STEP included. Before, they showed only in LIVE (OS-RECMODE2), so in STEP the user could edit with no red sign.

**Behaviour.**
- One predicate, `app::pattern_editable(const ViewState&)` (`app/include/o3os/app/screens/pattern_screen.h`): the Pattern screen in Edit (STEP and LIVE alike). `pe::edit_enabled` (every edit action's `enabled`) calls it; `PatternScreen::render` sets `Status::rec` from it (with a document to write into). The signs and the actions can no longer disagree.
- The Rec playhead bar (red row, SelText) is the take's, not a sign of editing: it stays LIVE only, past the count-in (new `Status::recording`).
- Count-in (OS-METRO): the pattern is in Edit, so the signs show during the count-in too (before: not red until the start point). The strip's `RECORDING IN n..` text and the pads' flash still mark the count-in; the red playhead bar and recording start at the start point. QA-PAT-052 and 055 are reworded.
- Demo (no document): no signs (nothing can be edited).

**Done when.** Host tests: the signs follow the predicate in WATCH, STEP and LIVE and agree with `step.note` enabled/disabled (`pattern_edit_app_test.cpp`); the gallery state `rec` checks frame, badge and bar per variant (`pattern_view_test.cpp`); sim goldens regenerated; QA rows in `docs/qa/catalogue/pattern.md`.

## OS-DELREPEAT: a held Delete repeats in Pattern

**Goal.** In Pattern a held Delete clears (Shift + Delete: backspaces) step after step and speeds up like the arrows, without making any other Delete binding repeat.

**Behaviour.**
- Delete joins `kRepeatButtons` (auto-repeat is per button and global; the schedule is `kRepeatGapMs`).
- A repeat is a Tap flagged `Gesture::repeat` (set by the recogniser), carried to `ActionContext::repeat`. `dispatch()` runs a repeat only for an action with `kActionRepeatable`; the press itself always runs. A skipped repeat does nothing, says nothing and is not recorded in a macro. Before this, the flag was carried but not honoured.
- Audit (every Tap binding of Up, Down, Left, Right, any context, any modifier): all were repeating; the flag is added to the five that lacked it (`wave.even_n`, `sedit.amp`, `chooser.delta`, `chooser.defined`, `text.move`), the rest had it. `builtin_bindings_test` walks the table and fails for an arrow-bound action without the flag. Of Delete's bindings only `step.clear` and `step.pull (was step.backspace until OS-DELPULL)` repeat; `file.delete`, `song.delete_slot`, `param.reset`, `mixer.reset`, `record.reset`, `sedit.all`, `instrument.init`, `text.backspace` and the rest take the press only.
- A held Delete in PROJECTS therefore asks (`DELETE <NAME>? DELETE AGAIN`) and marks nothing; a second press marks exactly one folder.
- A held key never wraps: the cursor advance of an entry (`move_cursor_by_step`) stops at the last row on a repeat; a fresh press keeps the wrap, except `step.clear`, which never wraps (its press may start a hold). Held across the last row, or started on it, Delete stops there and never clears from row 1: the clear (`step.clear`) never wraps, a press on the last row stays there.
- Undo: one unit per repeat for now (OS-UNDOGRP will group them).
- User keymaps: an arrow (or Delete) bound by a keymap to an action without the flag no longer repeats.

**Done when.** Host tests: held Delete clears as many steps as a held arrow moves in the same time; Shift + Delete pulls once per repeat; a held Delete on a Song slot removes one slot, in PROJECTS only asks; arrows still repeat (gesture flag test, binding audit, Song walk); a flagged repeat of a non-repeatable action does nothing. QA-PAT-216..218, QA-SNG-012, QA-FIL-064.

## OS-ROWWRAP: Up on the first row goes to the last

**Today (checked).** In Pattern, `Up` and `Down` are Global's `nav.up` / `nav.down`, which call `PatternScreen::step` and then `PatternView::scroll_rows`: both clamp at row 1 and at the last row (no wrap, nothing says so). Shift + `Up` / `Down` is `select.extend` (the block): it clamps too (Shift + Up at row 1 selects the column, again every track: OS-009). There are no other Up/Down variants in Pattern (Page moves are Shift + F-keys, `cursor.jump_fraction`). Holding an arrow repeats (OS-009b). New Community OS behaviour; OG parity open.

**Behaviour.**
- A **fresh** `Up` on the first row goes to the last row of the same pattern; a fresh `Down` on the last row goes to the first (symmetry). The length is the pattern's own (`pattern_dims`: 16, 64, 100 ...), not 64.
- Never on an auto-repeat (as `instrument.turn`): `Cursor::step_press` (new, default = `step`) is what `nav.up/down` call for a press; a repeat (`ActionContext::repeat`, OS-DELREPEAT) calls `step`. Holding `Up` from row 4 stops at row 1; a press that starts on row 1 wraps once and the repeats then walk up from the last row.
- Not while the cursor follows the playhead (WATCH and LIVE while playing: the cursor is the playhead's and would be pulled back at once); STEP's free cursor wraps while playing as stopped; WATCH stopped wraps.
- The column and the track stay. A selection is dropped, as for any move.
- Shift + the arrows (block extension) clamp as before: wrapping a block over the pattern's end is not a meaningful extension.
- Only Pattern wraps; every other list keeps its behaviour (the default `step_press` is `step`).

**Tests that encoded the clamp, changed on purpose.** `actions_test` (a repeat clamps, a press wraps), `app_test` (the second Up wraps), `history_app_test` (macro replay starts off row 1).

**Done when.** Host tests: fresh Up at row 1 and fresh Down at the last row wrap for lengths 16, 32, 64, 100; held arrows stop at the edge; Shift + arrows clamp; STEP playing wraps, WATCH playing and LIVE do not, stopped wraps (`pattern_edit_app_test`, `recmode_test`). QA-PAT-219..221.

## OS-PADHOLD: pressing Instr never changes the instrument through the pads

**Goal.** A tap on `Instr` used to turn the pads into an instrument picker (pad + 1); a stray tap then changed the cell's instrument. New Community OS behaviour; what the official firmware does for the Instr column is an open research question; maintainer decision (option A).

**Behaviour.**
- Edit, cursor on the Instr column: a pad writes its note to the row exactly as in the Note column (`step.note_pad`: the current instrument on an empty step, the cursor advances by JUMP). Browse (outside edit mode): the pad only previews (the note on the last used instrument; `pad.preview` no longer special-cases the Instr column).
- The instrument changes only with the Instr hold popup (the current one lit; its pads pick) or the encoder on the Instr column (`step.instrument_delta`).
- Vol and Fx unchanged (tap-then-pad FX entry stays). `step.instrument` stays in the registry (the popup's entry).
- Code: the `kInstr` pad binding in `bindings.cpp` is `step.note_pad`; `pad_preview` (pattern_actions.cpp) loses its Instr branch.

**Tests that encoded the old behaviour, changed on purpose.** `pattern_edit_app_test` (the Instr-field pad, PAR-035), `app_test` (five tests that used the Instr pad as a probe now read the pad's remembered note, `edit.last_note`), sim scenarios `pattern_edit.txt` (snap renamed `instr_column_pad`) and `pattern_keys.txt` (goldens regenerated), QA-PAT-120 and the HIL steps that used the Instr pad (`pattern_edit_undo.json`, `qa_instrmidi.json`: notes only; the steps hold).

**Done when.** Host tests: Edit + Instr column + pad writes the note and keeps the instrument; Browse + Instr pad previews only; tap Instr then pad is a note; the hold popup and the encoder still change the instrument; Vol pad entry unchanged. QA-PAT-222..224.

**Amended (OS-AUDIT E-PAT-12, maintainer 2026-10-23: the minor rows are in for 1.0).** In Edit with the cursor on the Instr column a pad picks instrument pad + 1 again (`step_note_pad` calls `step_instrument`: one undo unit, no note of its own, the pad sounds the picked instrument); the Note, Vol and Fx columns, Browse and the hold popup are as above. Open research question: whether the official firmware also moves the cursor on.

## OS-BUG-STEPSND: a pad in STEP is heard

**Root cause.** `step_note_pad` (STEP, Edit with REC free) wrote the note and never asked for the live note: `PreviewSink::pad_down` was called by Browse (`pad_preview`), the PIANO / ISO pads (`play_note`) and LIVE (`rec_pad`) only. So in STEP nothing reached the engine, stopped or playing (the 0.5 s heard with the pattern playing was the written step, played by the sequencer on its next pass). The release path was already generic.

**Fix.** `step_note_pad` sounds the pad (`pad_down`, the cursor's track, the current instrument, the pad's note) before the write moves the cursor; a pad during the count-in sounds too (it returned before the sound). The sound is the live note's: held until the pad's release; the sequencer's next note on a track shared with it cuts it (this OS's rule; what the official firmware does is an open research question). Track choice is the engine's `pick_track` (`live_notes.h`): from the cursor's track, a track with no live note and no sounding voice, else the cursor's track; that rule is unchanged (a pad on a track that is sounding lands on the next free track, so it is heard before the next step). No Config option (maintainer, 2026-10-15). The pad sounds even when the write is refused (`PAST TRACK END`), and on a step that already has a note it sounds the current instrument (`ctx.view.instrument`, as PIANO / ISO do) while the step keeps its own. Out of this fix: the STEP pad mode (`Shift + F5`, `step_pad_down`: a pad lights or clears the step it stands for) makes no sound and stays as it was. The TB-303 and other module instruments take the same `pad_down` as in Browse; no separate module path.

**Done when.** Host tests: STEP stopped and playing, LIVE, the count-in (`metro_app_test`): one `pad_down` on the cursor's track (track 3 in the tests) and the release passed on; a refused write; the step with another instrument; Vol / Fx pads silent (`pattern_edit_app_test`, `recmode_test`). QA-PAT-225. On the unit: stopped, playing on a track with a note on every step, TB-303.

## OS-PATCOPY: Copy and paste in Pattern

**Goal.** Copy takes the block selection (or the step under the cursor); Shift + Copy writes it at the cursor. New Community OS behaviour: no parity with the OG is claimed (OS-009 P1b described a richer copy and paste; none of it was built). The keys are the ones Song and Perform use for their own copy and paste, and were free on Pattern.

**Behaviour.**
- `block.copy` (Copy): the block (`Selection::block`, the cursor step when there is none) into the pattern clipboard: whole steps, one column per track. Any mode of Pattern; nothing is changed (no undo level). The strip says `COPIED n STEP(S)`. A second copy replaces the clip; the clip is the session's (a pattern switch, an undo, a new document leave it).
- `block.paste` (Shift + Copy): the clip's top-left step goes on the cursor (the cursor's row and track, whatever the selection); whole steps overwrite what is there. Clipped at each track's own end and at the last track: what does not fit is dropped, never wrapped, no length changes. A paste whose first row is past every track's end writes nothing and says `PAST TRACK END`; an empty clipboard says `NOTHING TO PASTE`. One undo unit (the pattern is created in it when it does not exist). Edit only (STEP or LIVE), also while the pattern plays (it does not join a LIVE take: it is its own unit); the cursor and the block stay. In LIVE the cursor rides the playhead, so the paste lands on the row the playhead is on at the key press (host test). Several tracks are written one track after the other, so a render block may hear track 0 new and track 1 still old for an instant: the same limit as Insert and Pull (one undo unit, not one atomic write to the sequencer). No cut: Copy, then Delete.
- Not in this item: paste masks (notes only, instruments only, FX), whole-pattern copy, the chooser's copy; OctaMED preset keys.
- Storage: `PatternClip` (`app/pattern_clip.h`), 16 x 128 steps x 7 bytes = 14,336 bytes, a member of `App`; the device builds the App in SDRAM (`g_app_storage`), so it costs no SRAM.

**Done when.** Host tests (`pattern_edit_app_test`): block copy and paste with one undo unit and the cursor staying; one step with no block; clipping at the end of the pattern and at the last track; paste while the pattern plays in STEP; Browse copies, does not paste; the clip survives Delete. Scenario `pattern_edit` (`paste`). QA-PAT-226..228. Unit (HIL): paste while playing, several tracks, listening for the instant of old and new; a pattern of several tracks.

## OS-DELPULL: Shift + Delete is the inverse of Insert

**Goal.** Shift + Delete pulls: it deletes the step at the block's first row (the step under the cursor when nothing is selected; maintainer decision 2026-10-22, OS-QAFIX-17: with a block the action works from the block, as Insert does) and moves every selected track up from there through step 127 (the last step is cleared); the cursor and the block stay. One undo unit. New Community OS behaviour, no parity claimed: it replaces `step.backspace` (the earlier backspace: the steps before the cursor pulled up, the cursor following), which Insert could not undo.

**Behaviour.**
- The action is `step.pull` (was `step.backspace`; a user keymap that names the old id is rejected as an unknown action). Still repeatable (a held Shift + Delete pulls once per repeat, QA-PAT-217). The same refusals as Insert: a track whose end is above the acting row (the block's first row, the cursor's with no block) is left alone, and `PAST TRACK END` when none can be.
- Insert then Shift + Delete leaves the pattern byte for byte, steps past the length included. Limit: Insert pushes step 127 out of the pattern, so a note on step 127 does not come back.
- Tests that encoded the old behaviour: `pattern_edit_app_test` (the 60 insert / backspace episodes against the reference vectors now compare only the insert episodes; the backspace vectors stay in `tests/data/pattern_edit_vectors.h` unused for backspace), `builtin_bindings_test` (the Shift + Delete row, the Delete repeat audit), the `pattern_edit` scenario (`backspace` is now `pull`, goldens regenerated).

**Done when.** Host tests: the round trip over 24 random patterns and blocks with undo depth and the cursor; pull's own semantics, at row 0 and past a track's end. QA-PAT-217, QA-PAT-229.
