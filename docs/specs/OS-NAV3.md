# OS-NAV3: page moves in every list, and the end of basket 6b

BACKLOG #11, "OS-NAV (rest)": per-view modes and navigation keys (maintainer's basket 6b, STATUS.md item 6b). Written on 2026-10-14 from a survey of `app/src/bindings.cpp` on `0.4.18` (bare `:n` below). **New Community OS behaviour**. Level B, size S: bindings, one action argument, docs. No format change, no new screen, nothing visible changes but the cursor.

## 1. What basket 6b asked for, and where each part is now

| Part | State |
|---|---|
| Quarter jumps | Done in OS-SHIFTROW: Song `:412-416`, File `:441-445`, Sample load `:458-462`; Global Shift+F = `ui.none` `:316-323` |
| Navigator | Done in OS-NAV2 (sticky overlay, F8 GO, F7 CLOSE) |
| Per-mode bindings | Done: Pattern WATCH / STEP / LIVE (OS-RECMODE2), Perform EDIT, Instrument pages and empty-slot chooser (OS-INSTUI), File IMPORT page (OS-FILES) |
| Page moves in lists | **Missing.** No list has page up/down. `cursor.page` (`actions.cpp:59`) exists but is not bound; `kPage` is declared and unused (`:63`) |
| Track moves in Pattern | **Missing.** `cursor.track` (`actions.cpp:64`) is not bound (`kTrack`, `:64`). Moved to OS-ZOOM, see §4 |

This slice binds the page moves and closes 6b.

## 2. The rule

**On a list, Shift+Up / Shift+Down = one page up / down.**
- A page is the number of rows the list shows.
- The move clamps at the first and last entry; it never wraps.
- A list shorter than one page goes to its first or last entry.

The rule applies only to contexts where Shift+Up/Down has no binding of its own today, either falling through to the plain arrow (`:268-270`) or doing nothing. Where Shift+arrows already mean something, nothing changes: Pattern and Song select a block, Perform swaps, Chop splits evenly, Sample edit sets the amplitude, and the module app uses its row keys.

## 3. The bindings

| Context | Lines (0.4.18) | Shift+Up / Shift+Down |
|---|---|---|
| File, PROJECTS and IMPORT pages | `:430-445` | `cursor.page −1 / +1` |
| Sample load | `:447-462` | same |
| Modules | `:710-712` | same |
| Module picker | `:723-727` | same |
| Learn list | `:676-686` | same |
| Instrument list (StepPopup) | `:746-756` | same; a page is the visible entries of the list |

- Lists that need nothing:
  - The Instrument, Config and Record rows fit one page.
  - The PlayMode list has eight entries.
  - Song order already has the quarter jumps.
  - Choosers and Fill move values, not rows.
- If `cursor.page` does not move by the visible row count, the engineer gives it that argument, or adds one small action per list. Either way, it is documented.
- Shift labels: no Shift+F row changes. The strip does not need a hint, but the generated key reference lists the new gesture.

## 4. Left out, and where it goes

- **Track moves in Pattern** (next/previous track in one press): every Shift+arrow in Pattern is already block selection, and the free combos belong to the key-collision list. This goes to **OS-ZOOM**, where the 12-track view needs it most. Its UI review picks the gesture. Add one line to OS-ZOOM's BACKLOG note.
- **The encoder press**: it is not a gesture on this hardware (`core/include/o3os/core/gesture.h:41`). Nothing to bind.
- **The 15 collisions of `ui-map.md` §3**: these belong to OS-FINALQA.

## 5. Acceptance

**Host tests**, for every context in §3:
- Shift+Down from the first entry lands on the first entry of the next page.
- Shift+Down near the end stops on the last entry, and Shift+Up stops on the first.
- A list shorter than a page goes to its ends.
- An empty list does nothing.

**A pinning test**: Shift+Up/Down in Pattern, Song, Perform, Chop, Sample edit and the module app still resolve to their current actions.

**`shift_labels_test`** still passes.

**QA rows:**
- QA-FIL and QA-SLD: page move on a long list. The card's `/MOD` folder has enough files.
- QA-MOD: page move in Modules and in the module picker.
- QA-INS: page move in the instrument list (64 entries).
- QA-GLB: the rule itself.

**HIL** `qa_nav3.json`: in File IMPORT, open `/MOD`, then Shift+Down twice and Shift+Up once. Take a shot that matches a sim snap. No new goldens otherwise; nothing drawn changes.

**Docs:**
- `ui-map.md` §5: add the rule as a new item 9.
- `ui-map.md` §1: fix the rows this survey found stale:
  - Modules, File and Config still show the Global jumps (now `ui.none`, or the screen's own jumps).
  - Wave and Sample edit list Instr-hold as planned; it is bound (`:483`, `:524`).
  - The File row's F6–F8 should be updated to the IMPORT page.
- BACKLOG: row #11 is DONE when this merges.

## 6. Built (on version 0.4.18)

- **Action design.** `cursor.page` already multiplied its argument (pages, + down) by `Cursor::page()`, the visible rows of whatever owns the input, so it needed no new argument: the bindings pass -1 / +1 and each list reports its own page. Two of the reported pages were wrong against §2 and were corrected: Modules 8 to 16 (`kFrameRows`, the rows of the pane; the catalogue holds 16 apps, so a page is the whole list) and the engine picker 8 to 10 (`kModulePickerRows`, the rows it draws). File and Sample load 16 and Learn 5 were right. The step popup is not a `Cursor` (it keeps its own `popup.move` / `popup.delta`), so it gets one small action, `popup.page` (arg = pages, + down; `step_popup_actions.cpp`), whose page is `step_popup_list_rows(theme, kind)`, the same number its renderer uses: 13 for the instrument list, 10 for the FX list (the Note panel has none). The FX list pages too, since the popup is one list overlay.
- **Bindings** (`bindings.cpp`): Shift+Up / Down in the File, Sample load, Modules, module picker and Learn contexts (`cursor.page -1 / 1`) and in the StepPopup context (`popup.page -1 / 1`). The Shift+arrow list at the head of the file names them.
- **Sample-only popups.** On Wave and Sample edit the instrument list dims the non-sample entries; a page whose target is dimmed ends on the nearest sample instrument on the way back, and stays put with none (`instrument_page_after`).
- **Not changed.** Pattern, Song, Perform (Shift+Left/Right swap; Shift+Up/Down fall through to the plain `perform.fx`), Chop, Sample edit and the module app keep their Shift+arrows, pinned in `builtin_bindings_test.cpp`. Track moves stay with OS-ZOOM (BACKLOG row 10).
- **Tests.** `nav3_page_test.cpp` (File PROJECTS and IMPORT over 40 entries, short and empty lists, Sample load, Learn with 12 bindings, the instrument and FX lists); `module_ui_test.cpp` (Modules and the picker); `builtin_bindings_test.cpp` (the Shift table and the pinning case). `shift_labels_test` unchanged and passing.
- **Docs.** ui-map §5 rule 10 (rule 9 is OS-SHIFTROW's on this base), §1 rows for File, Sample load, Modules, Wave and Sample edit (Instr hold is bound, Modules has no Global jumps) and the overlay line; QA-FIL-013, QA-SLD-016, QA-GLB-014, QA-MOD-060 / 061, QA-INS-060; `qa_nav3.json`; one new golden, `qa_file/qa_fil_013` (the IMPORT page of the simulator's empty card after Shift+Down; the unit compares its key row against it).
- **Latent, not fixed.** The MODULES list is not windowed: it draws every app, so a catalogue larger than 16 would run off the pane (`kModuleCatalogMax` is 16, so it cannot today).
