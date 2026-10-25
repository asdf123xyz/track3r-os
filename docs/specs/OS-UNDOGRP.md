# OS-UNDOGRP: merge encoder clicks into one undo unit and keep one undo history per area

BACKLOG OS-UNDOGRP (queued 2026-10-16, level B, size M). **Status: implemented on branch feat/OS-UNDOGRP, host PASS; unit checks open (see Hardware-only checks).** Defaults picked in the build: Sample edit is in SOUND (its sample.commit moves the instrument's points); a screen with no area (File, Config...) answers F8 with `NOTHING TO UNDO HERE`; the unused project-level ops (default length, active tracks, track, master, track name) go to SONG when Auto; the `UndoHistory` carves 5/12, 3/12, 4/12 of a region (160 / 96 / 128 KiB at 384 KiB); the Editor adds `UndoArea::None` (no journal) and `set_final_params` for D10. Written 2026-10-16 from the current tree. **New Community OS behaviour; no persistent format changes.** The journal is RAM only and not persisted (`undo.h`). No project or settings chunk changes, and no change to `.ptf`, the SD card or the module ABI.

## Goal
1. Consecutive encoder clicks on one parameter row become one undo unit. Today a 48-click cutoff sweep needs about 50 F8 presses, because each click is a 40 B `set_param` unit.
2. F8 undoes only the edits of its own area. Today one 384 KiB ring serves Pattern, Song and Params, and going back far undid a song-slot delete.

## Decisions
| # | Decision | Status |
|---|---|---|
| D1 | Three areas: **PATTERN** (Pattern screen), **SONG** (Song screen), **SOUND** (Instrument/Params, Wave/Chop). The Mixer has no history (D10). | orchestrator default |
| D2 | The area is chosen per dispatch, in this order: (1) an action forced to AUTO (`midi.cc`, `learn.*`, `module.app_group`); (2) the screen the action ran on (a table in `actions.cpp`); (3) AUTO for screens with no history (File, Modules, ModuleApp, Record, SampleLoad, Perform, Config), where the first journalled record decides: steps and pattern records go to PATTERN; order, tempo, swing and project title go to SONG; instrument, param and learn records go to SOUND. A unit is never split, and the macro and the live take stay one unit. | orchestrator default |
| D3 | Cross-screen edits land where they were made. An edit from a screen in no area follows its data (D2 rule 3). Pattern-screen edits (FILL, TOOLS, chooser) go to PATTERN. Song copy, paste, add slot and pattern NAME go to SONG. Track NAME on Mixer is not journalled (D10). A project-title change from File goes to SONG. | orchestrator default |
| D4 | Structure fence: a unit that creates or deletes a pattern, or fills or clears an instrument slot, is *structural*. Written, undone or redone (review fold), it ends the history of every other ring that names the object (a pattern: PATTERN and SONG; an instrument slot: SOUND), with the notice `PATTERN HISTORY CLEARED` (`SONG` / `SOUND` likewise): writing a creation or a fill drops those rings' redo units, a deletion or a clear all their units, an undo or a redo all of them. A unit that fails ends every ring. The Mixer's edits are final per action (`mixer.*`, a track's name, a MIDI CC on a track or master parameter), not per screen. Nothing else couples the areas: replay is by value, and steps stay valid for any pattern length. | maintainer (Q2 a, 2026-10-16) |
| D5 | Project-wide operations clear ALL areas, as today: NEW, load, import, save-time reset (`reset_history`, `Stale`). Device settings (Config) are not the project's and have no history, as today. | orchestrator default |
| D6 | One 384 KiB region (ADR 0013 D7), three separate rings: PATTERN 160 KiB, SONG 96 KiB, SOUND 128 KiB. A full ring evicts only its own oldest units. The split is sized by each area's largest units (a whole-pattern delete or pattern copy of about 14 KiB in PATTERN and SONG, a 64-instrument PURGE of about 94 KiB in SOUND), not by Mixer traffic (40 B units), so taking the Mixer out does not change it. | orchestrator default (rechecked after Q1 c) |
| D7 | A click group is keyed by the `ParamId`, set by the action (param.edit, param.coarse, midi.cc). Fine and coarse clicks on one row merge. Only parameter rows merge. | maintainer (Q3 a, 2026-10-16) |
| D8 | A group ends on: a click on another ParamId; any other undoable dispatch; undo or redo; a screen change; 1500 ms without a click (our choice, the same 1.5 s as our tap-tempo rule; a named constant); any clear of the history. Navigation alone (cursor, page) does not end it. | maintainer (Q4 a, 2026-10-16) |
| D9 | Redo is per area (Shift+F8 as today). A new edit in an area discards that area's redo units only. | orchestrator default |
| D10 | The Mixer has no undo: its edits (mixer.edit, mixer.coarse, track NAME) are final and write no journal record in any area; Mixer F8 stays SOLO. A Mixer-owned parameter (track volume, pan, sends, master) changed by `midi.cc` from another screen is not journalled either, so one parameter never has history in one place and none in another (orchestrator default). | maintainer (Q1 c, 2026-10-16) |
| D11 | The undo policy (maintainer, 2026-10-22). Undo covers only pattern edits, parameter changes (an instrument and the like) and sample edits. A destructive action outside those asks a confirmation question instead of having undo (an existing question mechanism: the strip question with its key marked, no new modal type). Other changes need neither: a load into a free slot, a new instrument in an empty slot, the Mixer's levels, mutes and solos (D10), the settings. On the Mixer's STEMS page Delete (the cursor's source) and Shift+Delete (the whole map) ask first. The OS-QANET rule 1 (`tests/qanet/rules.h`) checks exactly this: a change to pattern data, an instrument or its parameters, or sample audio leaves an undo unit or is asked first (a load over a used slot included); a destructive change outside them asks (STEMS Delete and Shift+Delete); the rest is exempt. | maintainer (2026-10-22) |

## Behaviour
- F8 undoes the newest unit of the current screen's area. A screen with no area (File, Config...) has no F8 undo, as now. On an empty area the strip says `NOTHING TO UNDO HERE`.
- The first click of a sweep makes a unit. Each later click of the same row folds into it: the `set_param` record keeps its first old value and takes the latest new value. One F8 returns to the value before the sweep, and Shift+F8 returns to the last value.
- The Pattern info line `undo n` shows the PATTERN area's depth.
- An undone area does not touch the others. A song-slot delete stays undoable only from Song, however many Pattern or Params edits come after it.

## Design
- Data (`undo.h`, `editor.h`):
  - `enum class UndoArea : u8 { Auto, Pattern, Song, Sound }`.
  - `UndoHistory { UndoJournal ring[3]; bool begin(void* region, size_t bytes); }` carves the one region 160/96/128 KiB (each a multiple of 4).
  - `Editor(Project&, UndoHistory&)`. The old `Editor(Project&, UndoJournal&)` stays and points all areas at one ring, so the existing tests run unchanged.
  - `undo(area)`, `redo(area)`, `can_undo(area)`, `journal(area)`. `begin_group(action, arg, area)` resolves AUTO at the first record; the unit header is already lazy.
- Merge (journal level):
  - `merge_hint(key)`, called by the handler before it writes. At the first record of a unit, `reserve()` reopens the previous unit if `key` equals the ring's `last_key_` and its redo count is 0. Reopening moves `cur_` back to that unit's start and restores `unit_start_`, `unit_records_` and `open_bytes_` from the unit header.
  - A `set_param` record with the same id as the unit's last record is patched in place (the new value). Any other record appends.
  - If the patch makes new equal old, the record is removed. An unnamed empty unit is aborted.
  - `commit` sets `last_key_ = key`. Undo, redo, `clear` and `begin` zero it. `Editor::merge_break()` zeroes it for the screen change and the timeout. The app compares `ctx.now` with the last click's tick (`core::tick_elapsed`).
- Wiring:
  - `edit_undo` and `edit_redo` take the area from the screen table.
  - `dispatch` checks `redo_depth()` of the area it wrote to (and SOUND, if D4 dropped its redo) to call `drop_undone_swaps`.
  - `rec_actions.cpp` take code uses `journal(Pattern)`.
  - The `Failed` result clears one ring, not all three.
- SDRAM: 0 bytes extra. Region 384 KiB = 160 + 96 + 128 KiB. `mem_map.h` `kUndoSize` is unchanged, and `kUndoRegionBytes` becomes the sum of the three. Capacity in units: PATTERN 3,413 step edits or 11 whole-pattern deletes (14,412 B); SONG 3,072 song edits or 6 full pattern copies; SOUND 2,457 param edits or 89 instrument edits (1,464 B). A 64-instrument PURGE (about 94 KiB) still fits SOUND.
- SRAM about +420 B: three journal objects at about 130 B each, the hint and merge fields, and the editor's area pointers.
- Real-time safety:
  - Main loop only, as today. No allocation. Fixed rings, in-place patching, no new buffers.
  - Undo applied values still go to audio as `ParamChange` through the queue.
  - Nothing in the audio ISR reads a journal.

## Edge cases
- A sweep A, B, A (rows alternate) makes three units.
- A sweep that returns to its starting value leaves no unit.
- A merged unit evicted by a full ring is dropped whole (`dropped_units`). A merged unit is never larger than 40 B plus the module blob's first-write record.
- First edit of a module param: that click holds `set_instrument` plus `set_param`. Later clicks patch the `set_param` only.
- An undo, or any other undoable edit, between two clicks stops the merge (`last_key_` is zero).
- `midi.cc` sweeps merge by the param id the CC resolved to, whichever screen is open. A CC that resolves to a Mixer-owned parameter writes no record (D10).
- A Mixer edit between two Params clicks does not end the click group: it is not an undoable dispatch.
- Deferred (APPLY) and Device params are not journalled and not merged.
- Live take (OS-LIVEREC): PATTERN-area group, untouched. `take_is_long` uses the PATTERN ring's capacity.
- The macro is one unit in the area of the screen it was played from.
- App edit groups: one unit each, AUTO. A steps group is PATTERN; a params-only group is SOUND; tempo only is SONG. The notice `UNDO <app>` is unchanged.
- "An instrument delete that clears steps": `instrument_delete` does NOT touch steps (the comment in `instrument_actions.cpp` says so), so it is a pure SOUND unit. The real cross-area case is pattern creation (Song paste, add slot, Pattern chooser): D4.
- The File PURGE strip says `F8 UNDO`, but File has no F8. Reword it to name Params (a text change, same slice).

## Out of scope
- Merging Song slot turns and Pattern note, instrument or volume field turns (Q3); held Delete repeats (OS-DELREPEAT may reuse `merge_hint`).
- Any Mixer undo (maintainer, Q1 c).
- The sample's own 3-step history (`sedit.undo`).
- A per-screen "undo all"; branching history.

## Done when (host tests, `undo_test`, a new `undo_area_test`, `dispatch` tests)
1. 48 `param.edit` clicks on one row make `depth()==1`; one F8 restores the pre-sweep value and Shift+F8 the last. Applied param count is 1.
2. A, B, A rows make 3 units. A sweep back to the start leaves depth unchanged.
3. Each of these ends a group: another undoable action, undo, a screen change, `ctx.now` more than 1500 ms later. A navigation action does not.
4. `midi.cc` merges by ParamId. `param.reset` does not merge. `mixer.edit`, `mixer.coarse`, a Mixer track NAME and a `midi.cc` on a Mixer-owned parameter write no journal record: every area's `depth()` is unchanged.
5. Song edits plus Pattern edits plus Params edits: F8 on each screen undoes only that area, and each area's `depth()` is independent. The old song-slot-delete scenario no longer undoes from Pattern or Params.
6. D4: undo of a Song-made `create_pattern` after Pattern edits clears PATTERN only, and the strip says so. A non-structural cross-area edit clears nothing.
7. Each area fills and evicts alone; an oversize unit clears only its ring and `lost_units` counts it.
8. NEW, load and import clear all three and `can_undo` is false in every area (`Stale` once).
9. The property test (random edit, undo, redo against full snapshots) passes with three areas and with merging.
10. `mk66_undo_region_test`: the three rings sum to `mem::kUndoSize`, are aligned, and `mem_map_test` still passes. Existing golden and scenario suites pass; update goldens only for the new strip text.
11. Docs: BACKLOG row to DONE; UI map F8/Shift+F8 text and the Mixer note "no undo"; editor.h comment; ADR 0009 line 51 ("one ring per area"); QA-PAT/SNG/PRM undo rows and a QA-MIX row saying Mixer edits are not undoable.

## Hardware-only checks
- Encoder feel: whether 1500 ms splits a slow cutoff sweep; the cutoff after F8 sounds as before the sweep.
- Undo of a 14 KiB Song pattern copy from a wrapped SDRAM ring (latency, no audio glitch).
- The three rings sit at their SDRAM addresses and survive a long session (the diag log shows `dropped_units` per area).

## Byte budget
Flash about +2.0 KB (cap 3.0 KB: area table, reopen and patch, per-area walk). SRAM about +420 B. SDRAM 0 (the region is split, not grown). Report image, `.data` and `.bss` against the previous build (`ci.sh mk66`).

## Maintainer answers (2026-10-16)
| Question | Answer |
|---|---|
| Q1 Mixer edits | (c) The Mixer has no undo; its edits are final; F8 stays SOLO (D10). |
| Q2 Undo of a pattern creation after Pattern edits to it | (a) Clears the PATTERN history with a notice (D4). |
| Q3 Merge scope | (a) Parameter rows only: Params and MIDI CC (D7). |
| Q4 Click-group timeout | (a) 1500 ms (D8). |

## Files
- `model/include/o3os/model/undo.h`, `model/src/undo.cpp`
- `model/include/o3os/model/editor.h`
- `app/src/actions.cpp`, `app/src/edit_actions.cpp`
