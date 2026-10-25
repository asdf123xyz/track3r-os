# SONG-UX: OS-SONGMAP (the track map as an editable grid) and OS-SONGPADS (the pads on Song)

Status: spec, 2026-10-18. Backlog: APPROVED row 1, `SONG-UX (OS-SONGMAP, OS-SONGPADS)`. Level B: no format change; only the steps and order list of an existing project change, through the model's existing API. One branch, one commit per ID, one `firmware-reviewer` pass. Everything here is **new Community OS behaviour**, except where a line says otherwise. The OG parity of today's Song keys is an **open research question** (Q1).

## Goal

Make song editing quicker without leaving Song. Two things:

1. **OS-SONGMAP.** The track map beside the order list becomes a grid that the cursor can enter. You select slots × tracks, then copy, paste, clear or transform those tracks across the patterns of those slots. F5 UNIQUE gives a slot its own copy of its pattern. An edit that would also change slots outside the selection asks first.
2. **OS-SONGPADS.** The pads do something on Song. Shift+F7 switches between TRACKS (mute and solo, the layer Pattern and Perform already have) and SLOTS (48 pads = 48 slots; a tap queues that slot while the song plays).

## Maintainer decisions already taken (2026-10-18)

- Go with the format-compatible part. Per-slot properties (OS-SLOTPROPS) and the F1 slot loop (OS-SONGLOOP) are DROPPED.
- Pads: both modes, switched with Shift+F7 (Q1a of the backlog triage).
- F5/F6 are freed for UNIQUE and TOOLS only if they are not the official firmware's own Song keys (Q2 yes, on that condition). How to settle that condition is Q1 below. Polyend's manual is not read for it (maintainer, 2026-10-18).
- An edit that changes a pattern also held by slots outside the selection asks every time (Q3c).

## 0. What exists today (dev cd2bed3)

- **Cursor.** One axis: `pos_`, `top_` (a 16-row window) and a range `anchor_` (`app/include/o3os/app/song_view.h:32-37, 62-64`). The cursor block sits on the PAT field (`app/src/screens/song_screen.cpp:124-131`). Song has no columns.
- **Left/Right.** No Song binding: Global's `nav.left/right` reach `SongScreen::step`, which only calls `leave_edit()` (it collapses the range and drops the tempo focus, `song_view.h:43-48`). Shift+Left/Right are unbound and fall back to the plain keys (`core/binding.h:22-24`).
- **Track map.** `SongTrackMap` (`app/include/o3os/app/song_map.h`) caches 2 bits per track per pattern (empty, other data, notes), keyed by `pattern_generation`. Cells = the project's `active_tracks()`, at most 16. A cell is 10 × 16 px at a 14 px pitch. Order page x ≈ 300-524 and Arrangement page x ≈ 570-794: both pages draw the map when it fits (`song_screen.cpp:71-75, 136-149`). The backlog row's "the Arrangement page draws no map" was stale.
- **Slot clipboard.** Pattern numbers only, 255 slots (`song_view.h:22`, `song_actions.cpp:198-243`). DUP PAT (`song.add_slot 1`) copies a pattern to `first_free_pattern()` with `copy_pattern()` (`song_actions.cpp:36-70`); a full pool says `NO FREE PATTERN`.
- **Pattern clipboard (OS-PATCOPY).** `PatternClip`, 16 tracks × 128 steps, 14,336 B in the App (SDRAM). Paste clips at each target track's length, writes changed tracks with `set_steps`, creates a missing pattern, and is allowed while playing (`step_actions.cpp:375-441`).
- **Undo (OS-UNDOGRP).** One byte ring per area: Pattern 160 KiB, Song 96 KiB, Sound 128 KiB, all in SDRAM. Every record of a Song-screen unit goes to the Song ring, step records included (`actions.cpp:358, 414`). A unit may hold records for many patterns. A full 128-step `set_steps` record is about 1,804 B (new + old steps). A unit that does not fit an empty ring **poisons** it: the whole Song history is reset and nothing tells the user (`model/src/undo.cpp:142-150`; `lost_units()` is read only by tests). A Song-made `create_pattern` keeps OS-UNDOGRP's fence (D4: its undo clears the Pattern history, with a strip notice). A non-structural cross-area step edit clears nothing (OS-UNDOGRP Done-when 6).
- **Questions (OS-KEYROWDLG).** A question is a strip text answered by the next key press. It is not modal: any other key acts and drops the question. The answer keys are drawn inverted on the screen's own row with `draw_question_keys` (`app/include/o3os/app/key_prompt.h:7, 87`). Song has no question today. Its row is drawn with `ui::soft_keys` (`song_screen.cpp:261`).
- **Fill dialog.** `FillOverlay` (`OverlayId::Fill`, `Context::FillDialog`) is a dialog of up to 8 descriptor rows with F1 FILL and F8 BACK, applied as one undo unit (`fill_view.h`, `fill_actions.cpp:171-183`). Free ids: OverlayId 13-19, Context 39-43.
- **Step helpers.** `edit::note_delta`, `instrument_set` and `clear(step, elements)` (`model/include/o3os/model/step_edit.h`), as Pattern's block edits use them (`step_actions.cpp:108-120`). Nothing works across patterns today.
- **Pads.** `PadsTracks` (`bindings.cpp:880-899`): a 4 × 12 grid. Row 1 `pads.track`, row 2 `track.mute` (AFFECTED on Perform), row 3 `track.mute_mode`, row 4 `track.solo`. The arg is `pad % 12`, so tracks 13-16 cannot be reached from the pads (on Pattern too). The layer is in force on Pattern and Perform only (`app.cpp:426-435`); `track_arg_ok` and `pads_track` refuse other screens (`step_actions.cpp:32-35`, `pattern_actions.cpp:175-186`). Pad lights go through `App::pad_lights` and the per-screen hook `Screen::pad_roles` (`screens/screen.h:119`); roles are Off, Back, Front, plus `blink_on` (250 ms). The PATS mode queues a pattern while playing and selects it when stopped (`pad_mode_actions.cpp:126-148`).
- **Live jump.** Rec on Song runs `song.queue_jump`: it queues the cursor's slot for the end of the playing pattern; Rec on the queued slot cancels (`song_actions.cpp:285-302`). The sequencer applies it at the pattern boundary (`seq/src/sequencer.cpp:1183-1194`).
- **Edits while playing.** The main loop is the single writer of the model. The render side copies each row with a generation check (`seq/include/o3os/seq/rows.h`). A multi-track write is not atomic, so one row may play half old and half new (OS-PATUX). The order list is read without a check.
- **Display list.** The budget is 1792 words. Song's recorded worst case is 571-616 words (before the map), so the room is ample.

## 1. OS-SONGMAP behaviour

### 1.1 The track cursor

- The cursor gets a column: **PAT** (today's) or a **track** 1..N, where N is the number of map cells drawn.
- **Right** on PAT enters the map at track 1. Right in the map moves one track, and stops at the last one. **Left** moves back, and Left on track 1 returns to PAT. There is no wrap.
- **Up/Down** keep the column. A plain move collapses the selection, as today.
- The map only exists while it is drawn: an empty song, no project, or a page where it does not fit leaves the cursor on PAT, and Right does nothing.
- **Drawing.**
  - The cell under the cursor gets the cursor's colour, a box one gap wider than the cell.
  - The PAT block is drawn as unselected, and the `TRACKS` header caption is bright (today PAT's is).
  - The info line names the track, e.g. `TRACK 03`.
  - Every active track has a cell while the cursor is in the map: a track that holds nothing in the slot's pattern is a faint tick (outside the map an empty track draws nothing, as in OS-SONG1). Otherwise a project of 12 tracks whose last four are empty drew a map 8 cells wide, with the cursor at the right of it over blank space (OS-BUG-SONGMAPCUR). The map is never scrolled: it is drawn only when all its cells (at most 16) fit, so the cursor's cell is always on screen.
- **The encoder** keeps changing the slot's pattern in either column (backlog row). F7 TEMPO is unchanged.

### 1.2 The selection

- **Shift+Up/Down** extend the slots (`song.select`, today's).
- **Shift+Left/Right** extend the tracks, in the map only. They get a Song binding, so they no longer fall back to plain Left/Right.
- The selection is a rectangle: slots `s0..s1` × tracks `t0..t1`. The selected cells are drawn with the selection background. One rectangle per row of the window: at most 16 rects.
- Shift+Left/Right on PAT do nothing.

### 1.3 Copy, paste, clear

- **On PAT** Copy, Shift+Copy and Delete are today's slot actions, unchanged.
- **Copy in the map** takes the rectangle (no selection: the cursor's cell) into the **song clip**. The clip holds, for each slot row and each track, that track's steps from its pattern, up to the track's length. A slot naming a missing pattern copies as empty cells. The strip says `COPIED 3 SLOTS × 2 TRACKS`.
- **Shift+Copy in the map** pastes the clip:
  - **With no selection,** the clip's top-left lands on the cursor's cell.
  - **With a selection,** the clip is tiled to fill the rectangle (Q3): rows and columns repeat, clipped to the rectangle. This way one cell pasted over 8 slots fills 8 slots in one press.
  - Steps are written from step 0 of each target track and clipped at its length (Q2). A clipped paste says `CLIPPED AT TRACK END`.
  - A missing target pattern is created (as OS-PATCOPY). The strip says `PASTED: 4 PATTERNS`.
- **Delete in the map** clears every element of the rectangle's tracks in those patterns. The track lengths stay. A missing pattern is skipped. A held Delete does it once (QA-SNG-012's rule).
- **Distinct patterns.** Each edit acts once per (pattern, track), never once per slot.
  - If pattern 02 sits in two selected slots, a clear or a transpose changes it once.
  - If two clip rows would write different steps to the same pattern, the top-most row wins and the later rows are skipped for that pattern (D5).

### 1.4 F6 TOOLS

F6 opens a dialog over the rectangle (no selection: the cursor's cell), in the Fill dialog's shape. It has at most 8 rows; rows swap by OP.

| Row | Values | Default |
|---|---|---|
| OP | TRANSPOSE · INSTRUMENT · CLEAR | TRANSPOSE |
| AMOUNT (TRANSPOSE) | −24..+24 semitones; Shift+encoder ±12 | 0 |
| TO (INSTRUMENT) | 1..the instrument count; every step with a note gets it | the instrument of the cursor's first note, else 1 |
| WHAT (CLEAR) | NOTES · VOLUME · FX 1 · FX 2 · ALL | ALL |

- A footer gives the scope, e.g. `SLOTS 03-10 · TRACKS 2-3 · 5 PATTERNS`.
- **F1 APPLY** closes the dialog and applies the op (with the question of 1.6 first). **F8 BACK** leaves.
- TRANSPOSE and INSTRUMENT use the same `edit::` helpers and range rules as Pattern's block edits, so a note at the edge behaves exactly as there.
- AMOUNT 0 says `NOTHING TO DO`. TOOLS on PAT acts on the cursor slot's tracks, all of them.
- The dialog's settings live for the session, as the Fill dialog's do.

### 1.5 F5 UNIQUE

- **No selection.** If the cursor slot's pattern is also held by other slots, UNIQUE copies it to the first free number and points this slot at the copy. The copy uses DUP PAT's `copy_pattern`, so it keeps the name, lengths and steps. The strip says `SLOT 05: PATTERN 07 (A COPY OF 03)`. If no other slot holds the pattern, it says `PATTERN 03 IS ONLY IN THIS SLOT`.
- **With a selection.** UNIQUE makes one copy for each pattern held both inside and outside the selection, and points every selected slot that holds that pattern at its copy. Selected slots that shared a pattern keep sharing the copy. This is the same rule as the question's UNIQUE answer (1.6).
- **Pool limits.** If the pool cannot take every copy needed (`kMaxPatterns` 128, ids 1..255), nothing is written and the strip says `NO FREE PATTERN`.
- **Undo.** One unit. A Song-made `create_pattern` keeps OS-UNDOGRP D4: undoing it clears the Pattern history, with the strip notice.

### 1.6 The question (Q3c of the triage)

- **When it asks.** Before a paste, clear or tool writes anything, the action finds the patterns it would change that are also held by a slot outside the rectangle. If there are none, it acts at once.
- **What it asks.**
  - One pattern: `PATTERN 03 IS ALSO IN 2 OTHER SLOTS: F1 ALL, F2 UNIQUE`.
  - Several patterns: `3 PATTERNS ARE ALSO IN OTHER SLOTS: F1 ALL, F2 UNIQUE`.
  - The texts are cut to the strip's width if needed.
  - The texts and the TOOLS key row are built from the key tables of `app/src/key_prompt.cpp`, because `tools/check_keytext.py` fails on a string literal that names an F key anywhere else.
- **The answers.** It is an OS-KEYROWDLG question: a new `Ask` value and a Song case in `prompt_in_force`. Song's row is drawn through `draw_question_keys`, which inverts F1 and F2 for the answers.
  - **F1 ALL** applies the edit to the shared patterns, so every slot holding them changes.
  - **F2 UNIQUE** first makes the copies of 1.5 (selection rule), then applies the edit to the copies only.
  - Any other key, an encoder turn or a cursor move drops the question and the pending edit, and that key acts as usual. The pending edit (kind, rectangle, tool settings) is kept in the Song view until then.
- **Every time.** There is no "don't ask again".
- **Undo.** Copies plus edit are one unit: one F8 takes both back.

### 1.7 Undo size limit (Q4)

- **The check.** Before writing, the action adds up the bytes of the records it will write: the changed span of each track as `set_steps` records it, the new and old steps, the copies of UNIQUE, and the unit's header and trailer.
- **Over the limit,** nothing is written and the strip says `TOO BIG TO UNDO: SELECT LESS`.
- **The limit** is half the Song ring, 48 KiB: about 26 full 128-step tracks, or about 54 tracks of 64 steps.
- **Why.** A unit larger than the ring would silently reset the whole Song history (section 0).
- **What it does not change.** A Song-screen step edit undoes from Song's F8 only. A later Pattern edit of the same steps follows OS-UNDOGRP's cross-area rule: it clears nothing. A test pins this down (Done when).

### 1.8 While the song plays

- **Allowed:** copy, paste, clear, tools and UNIQUE all work while playing, as OS-PATCOPY does.
- **One mixed row:** a multi-track write may sound half old and half new for one row (the documented OS-PATUX hazard).
- **UNIQUE on the playing slot:** this retargets the slot to an identical copy, so the sound does not change. The edit then lands on the copy. The reviewer checks how the sequencer reads `order()` mid-pattern (`sequencer.cpp:538-562, 810-825`).

### 1.9 The song clip

- A third clipboard, beside the slot clip and OS-PATCOPY's. It is a member of the App, so it lives in SDRAM and costs no SRAM.
- **Capacity:** 32 track cells of 128 steps, 28,672 B plus the cells' lengths and the clip's shape (D6). A larger rectangle says `TOO BIG TO COPY` and keeps the old clip.
- It lives for the session. A pattern switch, an undo or a new document do not clear it, as `PatternClip`.

## 2. OS-SONGPADS behaviour

- **Switching.**
  - Shift+F7 shows **PADS** on Song's Shift row and switches the mode: TRACKS → SLOTS → TRACKS. The strip says `PADS: TRACKS` or `PADS: SLOTS`.
  - The mode starts as TRACKS at boot. It is not saved, so no settings field is touched (Q5).
  - An overlay (the keyboard, TOOLS) takes the pads, as everywhere.
- **TRACKS.** The PadsTracks layer, in force on Song too.
  - Row 1 moves the map's track cursor to that track. With no map, it is a no-op.
  - Row 2 is MUTE, row 3 MUTE MODE, row 4 SOLO.
  - Mute and solo are the project's own state, shared with the Mixer and Pattern.
  - `pads_tracks_in_force`, `track_arg_ok` and `pads_track` admit Song. Lights come from `tracks_roles`, unchanged.
  - Tracks 13-16 stay unreachable from the pads, as on Pattern (`pad % 12`). This is recorded, not changed.
- **SLOTS.** The 48 pads show the page of 48 slots that holds the cursor: slots `48·k .. 48·k+47`, in PATS mode's pad order. The page moves when the cursor leaves it.
  - **Tap while playing:** queues that slot (`song.queue_jump` semantics with the pad's slot). A tap on the queued slot cancels. The cursor stays where it is (Q6).
  - **Tap when stopped:** moves the cursor there and collapses the selection.
  - **Pads past the song's end:** unlit, and a tap does nothing.
  - **Lights,** through `SongScreen::pad_roles`: a slot that exists is Back, the playing slot is Front, the queued slot blinks between Front and Back, and when stopped the cursor's slot is Front.
  - **A lone Shift hold** shows the TRACKS layer for as long as it is held, as on Pattern and Perform (D9).

## 3. Keys after SONG-UX

| Key on Song | Today | After |
|---|---|---|
| Left / Right | collapse the range | move the cursor between PAT and the tracks (1.1) |
| Shift + Left / Right | as Left / Right | extend the tracks of the rectangle, in the map (1.2) |
| Copy / Shift+Copy / Delete | slots | slots on PAT; tracks in the map (1.3) |
| F5 | PAT– | **UNIQUE** (Q1) |
| F6 | PAT+ | **TOOLS** (Q1) |
| Shift + F7 | nothing | **PADS** (2) |
| F1 / F2, while a question is up | PLAY PAT / PLAY SNG | **ALL** / **UNIQUE** (1.6) |
| pads | nothing | TRACKS or SLOTS (2) |

PAT–/PAT+ stay on the encoder. F1-F4, F7, F8, Insert, Shift+Insert, Rec and the Shift row's jumps and NAME do not change.

## 4. Decisions

One table. **Q** rows are for the maintainer: the recommended option comes first, and it is the default the implementer applies until answered. **D** rows are defaults taken in this spec, to confirm.

| # | Question or default | Options (recommended first) | Applied |
|---|---|---|---|
| Q1 | F5/F6 are freed only if the official firmware's Song F5/F6 are not PAT–/PAT+, and nobody knows yet (open research question; the manual is not read). How to settle it? | a) build F5 UNIQUE / F6 TOOLS now; the keys are reversible, so if a later check on a unit with the official firmware shows they match, they move · b) build everything else, and keep F5/F6 as today until the maintainer checks a unit (hands-only) · c) the maintainer states the official keys from memory now | a |
| Q2 | What a pasted track carries | a) steps only, clipped at the target track's length (as OS-PATCOPY) · b) steps and the source track's length | a |
| Q3 | Paste over a selected rectangle | a) tile the clip to fill the rectangle · b) always the clip's own size at the rectangle's top-left | a |
| Q4 | An edit too big for the Song undo ring | a) refuse over 48 KiB (half the ring) with `TOO BIG TO UNDO: SELECT LESS` · b) allow it with a question "cannot be undone, clears Song undo" · c) grow the Song ring (an OS-UNDOGRP change, out of this item) | a |
| Q5 | The pads' mode at boot | a) TRACKS, not saved · b) SLOTS, not saved | a |
| Q6 | SLOTS tap while playing | a) queue only, the cursor stays (as PATS mode) · b) queue and move the cursor | a |
| D1 | The column model | Left/Right walk PAT → track 1..N with no wrap; Up/Down keep the column; plain moves collapse the selection | applied |
| D2 | The encoder in the map | still the slot's pattern (as the backlog row says) | applied |
| D3 | The question's keys and texts | F1 ALL, F2 UNIQUE; any other key cancels and acts; texts of 1.6 | applied |
| D4 | UNIQUE with a selection | one copy per pattern shared with slots outside; selected slots that shared it keep sharing the copy | applied |
| D5 | Two clip rows aimed at the same pattern | the top-most row wins; the rest are skipped for that pattern | applied |
| D6 | Song clip capacity | 32 cells of 128 steps (28,672 B, SDRAM) | applied |
| D7 | TOOLS rows and ranges | the table of 1.4; settings kept for the session | applied |
| D8 | SLOTS window | pages of 48 slots holding the cursor, in PATS mode's pad order | applied |
| D9 | A lone Shift hold in SLOTS | shows the TRACKS layer while held, as Pattern and Perform | applied |
| D10 | Missing patterns | a paste creates them; clear and TOOLS skip them; a copy reads them as empty | applied |
| D11 | The question's wording | the strip holds 48 characters and the keys take 19 of them (`: F1 ALL, F2 UNIQUE`), so the heads of 1.6 are shortened: `PATTERN 03 ALSO IN 2 SLOTS` and `3 PATTERNS ALSO IN 9 SLOTS`, and `127 PATS ALSO IN 128 SLOTS` above 99 patterns (`PATTERNS` with two three-digit counts is 49 characters with the keys; a cut tail would read `F2 U`) | applied by the engineer, to confirm |
| D12 | What F2 UNIQUE copies | the patterns the question named (the ones the edit would change that a slot outside also holds), not every pattern the range shares with the outside: a pattern the edit leaves alone is not copied (the 1.5 rule is the F5 key's) | confirmed by the maintainer 2026-10-18 |
| D13 | The pads' row 1 lights on Song | Front on the map's track cursor, none lit while the cursor is on PAT (the layer's lights read the Song cursor there, not Pattern's) | applied by the engineer, to confirm |
| D14 | Delete | its own action `song.clear` (in the map the cells, on PAT DEL SLOT as before); F4 stays DEL SLOT in the map | confirmed by the maintainer 2026-10-18 |
| D15 | A copy's number | UNIQUE takes the first free number that no pattern has and no slot names (a slot that names a pattern that does not exist yet would start to play the copy); DUP PAT's `first_free_pattern` does not look at the order list: recorded, not changed here | applied |
| D16 | A missing source in the clip | an empty cell of the project's default length (what `copy_pattern` does for a missing source) | applied |
| D17 | TO (INSTRUMENT) | 1..64, the instrument slots of a step (`model::edit::kInstrumentSlots`) | confirmed by the maintainer 2026-10-18 |
| D18 | Song to Pattern | the Pattern button on Song opens the pattern of the slot under the Song cursor (`nav_screen`); other ways to Pattern leave the view as it was; a followed playhead (WATCH) still wins | new Community OS behaviour (OS-QAFIX-E-SONG-06; an owner of the official firmware expects it, E-SONG-06) |
| D18 | A mute on Song | a Song-screen pad mute or solo is an undo unit of the SONG area (the screen's area, as on Pattern), so it ends the Song redo history and F8 on Song undoes it first | confirmed by the maintainer 2026-10-18 |
| D19 | The notices | the results say `PASTED: n PATTERNS` (`, CLIPPED AT TRACK END`, or ` + n COPIES`, or both: ` + n COPIES, CLIPPED`), `CLEARED: n PATTERNS`, `TRANSPOSED: n PATTERNS`, `INSTRUMENT SET: n PATTERNS`, `UNIQUE: n COPIES`, `NOTHING TO CHANGE`; a result rewrites the Song notice that is up (it does not queue behind it); the copy's `×` is `X` (the Label face has no multiplication sign) | applied |

## 5. Edge cases

- **Empty song or no project:** Right does nothing. Map actions say `SONG IS EMPTY` (or the existing no-project text).
- **Selection past the map:** the track range is clamped to the cells drawn. A project with more tracks than fit leaves the map off, and the cursor stays on PAT.
- **Paste at the bottom:** a clip that runs past the last slot is clipped. A paste never adds slots.
- **Questions in a row:** a question is up, and Shift+Copy is pressed again. The old question is dropped and a new one is asked (the key acts).
- **Undo of a big unit:** it must be quick and silent (OS-UNDOGRP's unit check on the unit).
- **NEW, OPEN, IMPORT:** they clear the question and the pending edit. The song clip survives.
- **Name and lengths:** a paste or tool never changes a pattern's name or lengths. UNIQUE keeps them (`copy_pattern`).

## 6. Persistence, real time, budget

- **Persistence:** none. No `.o3pj` or `.o3st` field. The pad mode and the TOOLS settings live in the view and are not saved. OG files are untouched.
- **Real time:**
  - Main loop only. No new per-block audio work, and no sequencer change.
  - A 48 KiB edit is a bounded loop of at most about 26 to 54 track writes, so it is fast enough for the main loop. The engineer measures it in the host test and records the figure.
- **Size [est]:**
  - FLASH: OS-SONGMAP about +4-6 KB, OS-SONGPADS about +1 KB.
  - SRAM 0. SDRAM about +29 KB (the song clip).
  - Display list: the cursor box and up to 16 selection rects, about +60 words, far under 1792.
  - The engineer records the measured deltas in the commit.
- **CPU per audio block:** 0.

## 7. Done when

1. Host tests (extend `tests/song_edit_test.cpp`, or a new `tests/song_map_edit_test.cpp`), each named after its behaviour:
   - the column walk and the clamping;
   - the rectangle and its collapse;
   - copy then paste with no selection, and tiled over a selection;
   - the clip at a track's length (`CLIPPED AT TRACK END`);
   - a paste creating a missing pattern;
   - Delete in the map, once per held key;
   - TRANSPOSE, INSTRUMENT and CLEAR each acting once per distinct pattern;
   - UNIQUE with no selection, with a selection, on an unshared pattern, and with a full pool (nothing written);
   - the question: nothing is written before the answer, F1 ALL, F2 UNIQUE, and another key drops it;
   - one F8 undoes paste, copies and tools as one unit;
   - an edit over 48 KiB writes nothing;
   - a Song-made step edit followed by a Pattern edit of the same steps: Song's F8 behaves as OS-UNDOGRP's cross-area rule says;
   - edits while the transport runs.
2. Pads tests (`tests/song_pads_test.cpp`; `tests/pads_app_test.cpp` is the SDK Pads app's test, and the TRACKS layer's own are in `tests/pad_modes_test.cpp`):
   - Shift+F7 cycles the mode, and the strip text follows;
   - TRACKS on Song mutes, solos and moves the track cursor;
   - SLOTS: a tap when stopped moves the cursor, a tap while playing queues, a tap on the queued slot cancels;
   - the pad roles (exists, playing, queued blink, cursor) and paging;
   - a lone Shift hold shows TRACKS.
3. `key_prompt_test` covers the new overlay's owner row (TOOLS: F1 APPLY, F8 BACK) and the Song question's answer keys. `shift_labels_test` shows Shift+F7 `PADS`.
4. Simulator:
   - `sim/scenarios/qa/qa_song.txt` snaps the map cursor, a rectangle, the TOOLS dialog, the question's key row and the SLOTS lights;
   - `sim/src/screens_app.cpp` gains the Song rectangle state and the TOOLS overlay state;
   - `budget_test` keeps every state under 1792 words;
   - goldens are regenerated with `tools/update_goldens.py`.
5. Documentation:
   - QA rows in `docs/qa/catalogue/song.md`: OS-SONGMAP takes QA-SNG-020..039 and OS-SONGPADS takes QA-SNG-040..049 (the next free id is 014; 014-019 are left for fixes);
   - the Song row of `docs/ui/ui-map.md` (section 1) and its section 3 notes;
   - the manual's Song chapter is left to OS-MANUAL2 C3 (the manual loop flags it).
6. Build and size:
   - `tools/ci.sh host` is green and the `mk66` build is warning-free;
   - FLASH, SRAM and SDRAM deltas are in each commit body.

## 8. Hardware-only checks

- **SLOTS lights while the song plays:** the playing slot, the queued slot's blink, and a queue by pad heard at the pattern end.
- **TRACKS on Song:** mute and solo by ear, with the Mixer showing the same state.
- **A big paste while playing:** a tiled paste over 30+ slots × 1 track at about 48 KiB. No hang, no audio dropout. At most one mixed row. The undo is quick and silent.
- **Readability:** the track cursor on a 10 × 16 px cell, with 16 tracks, on the panel.

## 9. Risks (for the review)

- **Silent undo loss.** The size check (1.7) must count exactly what `set_steps` records. An undercount poisons the Song ring, and that loss is silent.
- **Wrong patterns.** The distinct-pattern rule (1.3, D5) and the question's pattern set (1.6) must be computed from the same list, or an edit may touch a pattern the question did not name.
- **UNIQUE with too many patterns.** UNIQUE across many patterns runs near the pool and the undo limit together, so both are checked before any write.
- **A stale pending edit.** The question is non-modal, and the pending edit must not survive a cursor move, an undo, a NEW, or a screen change.
- **The pad layer on Song.** Admitting Song in `pads_tracks_in_force` and `track_arg_ok` must not reach other screens.

## 10. Out of scope

- OS-SONGSHARE (shared-pattern marks, CUT, moving slots, next use, range export): the next item.
- "Song clip preview" (IDEA).
- Reaching tracks 13-16 from the pads.
- Growing the undo rings.
- Per-slot properties and the slot loop (DROPPED).
- The manual chapter (OS-MANUAL2 C3).

## 11. Files (from the fact sheet, 2026-10-18)

- **OS-SONGMAP:**
  - `app/include/o3os/app/song_view.h`, `app/src/song_view.cpp` (column, track anchor, pending edit);
  - `app/include/o3os/app/song_map.h`;
  - `app/src/song_actions.cpp`, `app/src/action_handlers.h`, `app/src/actions.cpp`, `app/src/bindings.cpp`;
  - `app/include/o3os/app/screens/song_screen.h`, `app/src/screens/song_screen.cpp`;
  - new `app/include/o3os/app/song_clip.h`;
  - new `app/include/o3os/app/song_tools_view.h`, `app/src/song_tools_actions.cpp`, `app/src/screens/song_tools_overlay.cpp`;
  - `app/include/o3os/app/screens/ids.h`, `app/include/o3os/app/bindings.h` (`kContextNames`), `app/include/o3os/app/screens/overlay.h`, `app/src/screens/overlays.cpp`, `app/include/o3os/app/view_state.h`, `app/src/state_line.cpp`;
  - `app/include/o3os/app/key_prompt.h`, `app/src/key_prompt.cpp`;
  - `app/src/pattern_edit_util.h`.
- **OS-SONGPADS:**
  - `app/include/o3os/app/pad_layer.h`, `app/include/o3os/app/pad_lights.h`, `app/src/pad_lights.cpp`;
  - `app/src/app.cpp`, `app/include/o3os/app/app.h`;
  - `app/src/bindings.cpp`, `app/src/song_actions.cpp`;
  - `app/src/step_actions.cpp` (`track_arg_ok`), `app/src/pattern_actions.cpp` (`pads_track`);
  - `app/src/screens/song_screen.cpp` (`pad_roles`, `kSongShiftKeys`).
- **Tests, sim and docs:** the files named in Done when.

## 12. State for implementer

Order: OS-SONGPADS first (S, self-contained, and it gives the map's track cursor a pad entry), then OS-SONGMAP in this order: 1.1 → 1.2 → 1.3 → 1.7 → 1.5 → 1.6 → 1.4. Each step comes with its tests.

QA ids: QA-SNG-020..049. Apply the Q rows' recommended options until the maintainer answers, and do not invent anything outside the table: put a new choice in the spec as a D row, to confirm. No format change is allowed. If the work seems to need one, stop and report.

## 13. Implementation record (2026-10-18)

- **OS-SONGPADS** (`feat/SONG-UX`, first commit): `PadLayer::song_slots` / `song_tracks`, `App::pads_tracks_in_force` admits Song, `track_arg_ok` and `pe::track_enabled` admit Song, `song.pads` (Shift + F7) and `song.pad` (the pads' wildcard row of the Song context, reached in SLOTS only), `SongScreen::pad_roles`, `SongView::pad_base`, `diag state` says `pads=SLOTS` / `pads=TRACKS` on Song; tests `tests/song_pads_test.cpp`.
- **OS-SONGMAP**: `song_edit.h` / `song_edit.cpp` (the rectangle, the planner and the executor over one walk, copy of the cells, UNIQUE), `song_clip.h`, `song_tools_view.h` / `.cpp`, `song_map_actions.cpp`, `screens/song_tools_overlay.cpp`, `SongView` (the column, the track anchor, `SongAsk`), `Context::SongAsk` (the screen's field level while the question is up: F1 / F2) and `Context::SongTools`, `OverlayId::SongTools`, `Ask::Shared`; tests `tests/song_map_edit_test.cpp`, `tests/key_prompt_test.cpp`.
- The question is non-modal and withdrawn by `song_question_taken_back` (App::run_once, before the input is resolved) and `song_question_sync` (the strip's text, the screen, an overlay, `Project::song_generation`).
- Reversing Q1 (F5 / F6 back to PAT - / PAT +): the two `bind` lines of Song in `bindings.cpp` and `kSongSoftKeys`; `song.pattern_delta` is still registered, `song.unique` and `song.tools` would take other keys.
