# PAT-OPS: duplicate, expand and shrink a pattern; copy and paste a whole pattern

Status: spec, 2026-10-18. Backlog: APPROVED row 4. Level B, no format change. Two IDs on one branch, one commit each:

- **PAT-OPS-A** (M): DUPLICATE, EXPAND and SHRINK as fixed entries of Pattern's F7 TOOLS.
- **PAT-OPS-B** (S-M): copy and paste a whole pattern with F1 held.

Everything here is **new Community OS behaviour**. OS-009 (`docs/specs/OS-009-pattern-editing.md:151`) records the three operations as documented in the release notes and manual of the official firmware, not verified on a unit; whether the official firmware does exactly this stays an **open research question**. OS-009:150 claims OG parity for a whole-pattern paste that keeps the name; that claim has no evidence here, so it is an **open research question**.

## 0. What exists (dev 377ec76)

- **F7 TOOLS** (`app/include/o3os/app/tools_list.h`):
  - Fixed entries sit above the module apps. Today they are EXPORT PATTERN (`kToolsFixed` 1) and EXPORT SELECTION, which shows only with a block (`kToolsBlock` 1).
  - `tools_item()` hard-codes the labels for at most two fixed entries (`:47-52`), and `tools_run` dispatches by `ToolItem::Kind` (`app/src/tools_actions.cpp:55-88`).
  - The list scrolls (10 rows visible).
  - TOOLS refuses to open while the view follows the play line (`tools_open`, `:18`).
  - Tests count `kToolsFixed` (`tests/file_export_test.cpp`, `tests/module_edit_test.cpp:1439-1501`).
- **The pattern model** (`model/include/o3os/model/pattern.h:16-21`):
  - Per-track lengths of 1..128, and no pattern-level length. Steps past a track's length are kept.
  - The Editor has `set_steps` (all or nothing), `set_track_length`, `set_pattern_length` (all 16 tracks), `set_pattern_name` and `create_pattern`. Each records one undo piece, and only when the value changes.
  - `model/pattern_ops.h` (OS-009 D8) was never built.
- **OS-009 semantics** (`:151`, PAR-014):
  - **duplicate** doubles the length and repeats the steps;
  - **expand** puts step i at 2i, with an empty step after it;
  - **shrink** keeps the even steps;
  - an odd length L shrinks to ceil(L/2), and `shrink(expand(p)) == p`;
  - a result over 128 steps is refused with a message, never clipped.
- **Held F1** opens the PatternChooser after 400 ms (OS-PATSWITCH). It closes on release.
  - Its context `kChooserCtx` is modal and captures, and it is shared with the LENGTH and JUMP choosers (`app/src/bindings.cpp:740-753`, `app/src/app.cpp:652-654`).
  - Copy and Shift+Copy are not bound there, so today they do nothing while F1 is held.
  - The chosen pattern is `ViewState.edit.pattern`. While playing, a pick also switches the playing pattern.
- **The pattern clipboard** (OS-PATCOPY, `app/include/o3os/app/pattern_clip.h`): 16 × 128 steps (14,336 B, in the App's SDRAM), with no lengths and no name. `block_paste` needs Edit on and clips at each track's length.
- **Song's `copy_pattern` and `first_free_pattern`** (`app/src/song_actions.cpp:36-70`) are file-local. They copy into a new number only, so they cannot paste over an existing pattern.
- **Undo.**
  - The Pattern ring is 160 KiB.
  - A full 128-step `set_steps` record is 1,796 B of payload, so a 16-track rewrite is up to about 28.7 KB (16 × 1,796 B: the old and the new values of each track).
  - Creating a pattern is structural (OS-UNDOGRP D4: it ends the Song ring's redo, and undoing it clears the Song history, with a notice).

## 1. PAT-OPS-A: DUPLICATE, EXPAND, SHRINK

- **The entries.** Three fixed entries, after EXPORT PATTERN and before EXPORT SELECTION:
  - `DUPLICATE x2`
  - `EXPAND x2`
  - `SHRINK /2`

  The labels are D1. `tools_item()` takes a small table of fixed entries in place of its two-entry branch, and `kToolsFixed` goes from 1 to 4.
- **The pure functions.** They go in a new `model/include/o3os/model/pattern_ops.h` (OS-009 D8): over one track's `Step[128]` and its length, they return the new steps and length and change nothing.
  - **DUPLICATE**: new length 2L; steps L..2L−1 copy steps 0..L−1.
  - **EXPAND**: new length 2L; step i goes to 2i, and the odd steps are empty.
  - **SHRINK**: new length ceil(L/2); step i takes old step 2i.
  - **Shared rules**:
    - steps from the new length up to the old one are cleared (SHRINK);
    - steps past both lengths are left as they are;
    - a track whose result equals what it held is not written.
- **Scope (Q1).** The whole pattern, every track that exists (`active_tracks`), each by its own length. With a block selected, only the block's tracks, over their whole length.
- **Refusal.** If any track in scope would pass 128 steps, nothing is written and the strip says `TOO LONG: 128 STEPS AT MOST`.
- **The action.** It closes the list, writes through the Editor (`set_steps`, `set_track_length`) as one undo unit, and says `DUPLICATED: 64 STEPS` (or EXPANDED or SHRUNK; the length shown is the longest track's).
  - The cursor stays on its step, clamped to the new length, and the block is cleared.
  - A missing pattern says `PATTERN IS EMPTY` and does nothing.
- **Edit mode.** The entries need Edit on, as Fill and paste do. With Edit off they say the existing Edit-off text.
- **FX timing.** Tick-based FX (retrigger, arpeggio, tremor, slides) keep their values. The notes move, but the ticks inside a step do not scale. This is written in the QA row; the data is not changed.

## 2. PAT-OPS-B: the whole-pattern clipboard on F1 held

- **Copy while F1 is held** takes the chooser's pattern (`edit.pattern`) whole: its 16 × 128 steps, its 16 track lengths and its name. The strip says `PATTERN 05 COPIED`.
- **Shift+Copy while F1 is held** writes the clip over the chooser's pattern: steps, lengths and name, as one undo unit.
  - A missing target is created first. That creation is structural, so the D4 fence applies.
  - The strip says `PATTERN 07 REPLACED BY 05`.
  - It needs Edit on, as `block_paste` does.
- **Where the bindings go.** In `kChooserCtx`. The handlers check `view.overlay == PatternChooser && edit.chooser_held`, so the LENGTH and JUMP choosers are not affected.
- **The hint.** The chooser's strip hint adds `COPY: COPY   SHIFT+COPY: PASTE`. It names no F key, so `check_keytext.py` is not involved.
- **One clipboard (Q2).** A whole-pattern copy fills the existing `PatternClip` with all 16 tracks × 128 rows, plus two new fields: `lengths[16]` and `name`, and a `whole` flag.
  - Copy in the grid makes a block clip, and clears `whole`.
  - Shift+Copy in the grid pastes a whole clip as a block (steps only, at the cursor, clipped), as today.
  - The extra cost is about 48 B.
- **Pasting over the playing pattern (Q4).** This is allowed: one row may play half old and half new (OS-PATUX). A track's new length applies from its next row (a shorter track wraps by itself, and a track that is past its new end goes to row 0: `read_row`, covered by `seq_sequencer_test`), but the pattern's own length is read only when a pattern starts (`pattern_len_`, `Sequencer::pattern_end`): the pass in progress ends at the old length, and the new one applies from the next pass. After a longer paste (or a DUPLICATE or EXPAND run while it plays) the pass is cut at the old length; after a shorter one the tracks wrap early and the pass keeps its length.
- **No question (Q3).** A paste over a pattern that has steps asks nothing; one F8 undoes it.

## 3. Decisions

One table. **Q** rows are for the maintainer: the recommended option comes first and is applied until answered. **D** rows are defaults, to confirm.

| # | Question or default | Options (recommended first) | Applied |
|---|---|---|---|
| Q1 | Scope of DUPLICATE, EXPAND and SHRINK | a) the whole pattern; with a block, only the block's tracks · b) always the whole pattern, every track | a |
| Q2 | The whole-pattern clipboard | a) one clipboard shared with the grid's Copy (+48 B) · b) a second clipboard (+14.4 KB SDRAM), so a block clip and a pattern clip live apart | a |
| Q3 | Paste over a pattern that has steps | a) no question; one F8 undoes it · b) ask first | a |
| Q4 | Paste over the playing pattern while playing | a) allowed, as block paste (one row may mix) · b) refused while playing | a |
| Q5 | The name on a whole paste | a) carried, as the backlog row says (OS-009:150 kept it; whether the official firmware does the same is an open research question) · b) the target keeps its own name | a |
| D1 | Labels | `DUPLICATE x2`, `EXPAND x2`, `SHRINK /2`, between EXPORT PATTERN and EXPORT SELECTION | applied |
| D2 | Steps beyond the lengths | SHRINK clears new..old length; steps past both lengths are untouched | applied |
| D3 | After an op | the cursor is clamped, the block is cleared, and the view stays on the pattern | applied |
| D4 | Edit off | the ops and the paste are refused with the existing Edit-off text; the copy works | applied |
| D5 | Pure operations | `model/pattern_ops.h`, host-tested, as OS-009 D8 | applied |
| D6 | Outside Edit | the TOOLS entries and the paste of the held F1 say `TURN EDIT ON` (no generic Edit-off text existed: the Fill dialog's `FILL: TURN EDIT ON` names itself); nothing is written | applied (as built) |
| D7 | Actions | `pattern.op` (arg `model::ops::Op`: 0 DUPLICATE, 1 EXPAND, 2 SHRINK; undoable, enabled in Edit) is run by `tools_run` as a nested dispatch after it closed the list and checked Edit; `pattern.clip_copy` (not undoable: it writes no project data) and `pattern.clip_paste` (undoable) are the two `kChooserCtx` bindings and do nothing outside the PATTERN chooser held by F1 | applied (as built) |
| D8 | What is written | a track goes through `set_steps` over the span that changed only, its length through `set_track_length`; an operation that changes nothing leaves no unit and says `NOTHING TO CHANGE` (SHRINK of one-step tracks, a paste of a pattern over itself) | applied (as built) |
| D9 | The clip | it also keeps the number it was copied from (1 B, for `REPLACED BY 05`): 49 B more, not 48. A Copy of a pattern the project lacks says `NOTHING TO COPY`; the paste of a clip that is a block of the grid says `NO PATTERN COPIED` | applied (as built) |
| D10 | News of the clip | posted with `upsert` under the source `clip`: the strip runs only after the chooser closes, so a Copy and a paste in one hold of F1 show one line, the later | applied (as built) |
| D11 | The chooser's hint | `pads, encoder: switch now` (`+-1` stopped) replaces `pads 1-48   encoder, Up, Down: ...` to make room for `COPY: COPY   SHIFT+COPY: PASTE`; where the value (`EMPTY`, `LEAVES THE SONG`) still leaves no room, the pick is dropped and the clip and the close stay. Up, Down and the pads 1-48 still pick; the hint no longer names them (the manual's chooser text does) | applied (as built) |

## 4. Done when

1. **`tests/pattern_ops_test.cpp` (new):**
   - duplicate doubles and repeats;
   - expand spreads the steps, and shrink keeps the even ones;
   - `shrink(expand(p)) == p`;
   - odd lengths;
   - 65..128 steps are refused for duplicate and expand;
   - steps past the length;
   - the per-track lengths.
2. **`tests/pattern_edit_app_test.cpp`:**
   - each op is one undo unit;
   - the block scope;
   - the refusal text, and nothing written;
   - Edit off;
   - the whole-pattern copy and paste with F1 held (steps, lengths, name);
   - a paste to a missing pattern, with the D4 fence notice on undo;
   - the LENGTH and JUMP choosers ignore Copy;
   - a grid paste of a whole clip;
   - a paste while playing.
3. **Registry tests:**
   - `tests/actions_test.cpp:266` lists the new undoable actions;
   - `tests/builtin_bindings_test.cpp` lists the `kChooserCtx` rows;
   - the `kToolsFixed` users (`file_export_test`, `module_edit_test`) pass with 4.
4. **Simulator:** `sim/scenarios/qa/qa_pattern.txt` snaps the TOOLS list with the three entries and a pattern after EXPAND. Goldens are regenerated, and `budget_test` stays under 1792.
5. **QA and docs:**
   - QA rows QA-PAT-280..299 in `docs/qa/catalogue/pattern.md`;
   - the ui-map Pattern row (TOOLS, the F1-held keys);
   - the manual's `pattern_tools.md` is flagged for the manual loop.
6. **Build:** `tools/ci.sh host` is green, the `mk66` build has no warnings, and the deltas are in each commit body.

## 5. Hardware-only checks

- An EXPAND of a 32-step drum pattern, heard at half speed.
- A whole-pattern paste onto the playing pattern while playing: at most one mixed row, and no hung note on a sampler track. The pass in progress ends at the old pattern length; the new length applies from the next pass.
- Paste an empty MIDI track over a sounding note (a MIDI track that holds a long note, the paste of a pattern whose same track is empty, or a SHRINK that drops the row with the OFF): watch `amidi -d` and note whether the note stays on until Stop (see "Latent").

## 6. Budget [est]

- **FLASH:** A about +2 KB; B about +1 KB.
- **SRAM:** 0. The clip fields add about 48 B in SDRAM.
- **Undo:** at most about 28.7 KB a unit (16 tracks × 1,796 B, old and new values), against the 160 KiB Pattern ring.
- **CPU per audio block:** 0.

## 7. State for implementer

- **Order:**
  1. `pattern_ops.h` with its tests;
  2. the TOOLS entries and actions (A);
  3. then the clip fields and the `kChooserCtx` bindings (B).
- **QA ids:** QA-PAT-280..299. OS-FILL2 uses 300..319.
- **Moving the helpers:** `copy_pattern` and `first_free_pattern` stay where they are. B writes over an existing number, which `copy_pattern` cannot do; if both need a shared helper, move it into `app/src/pattern_edit_util.h`.
- **Decisions:** apply the Q rows' recommended options until answered, and record a new choice as a D row.

## 8. Latent

- **A hung MIDI note after a paste or a SHRINK** (found by the review, not fixed here; no code changed). A whole-pattern paste or a SHRINK that leaves a MIDI track empty after a sounding note, or skips the OFF row that would end it, leaves the note on until Stop. A block paste has the same property today. A switch of the playing pattern does end such a note (`switch_release`, OS-PATSWITCH); an edit under the running pattern does not. A sampler note is released by its voice and is not affected. To be seen on the unit (section 5); a fix (an OFF sent when the row that owed it is gone) or a documented limit follows from what it shows.
