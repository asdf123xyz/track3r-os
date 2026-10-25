# OS-SONGSHARE: shared-pattern marks, cut, move, next use and range export on Song

Status: spec, 2026-10-18. Backlog: APPROVED row 2, right after SONG-UX (`docs/specs/SONG-UX.md`), which it builds on: the PAT/track column, the rectangle and TOOLS.

- **Level B** for slices S1-S4.
- **Level C** for S5, the range export, because it touches the sequencer's offline render.
- No format change.
- Everything here is **new Community OS behaviour**.

## 0. What exists (dev 20c5223, with SONG-UX specified, not yet built)

- **Shift+Delete** has no Song binding. It falls back to the plain Delete, which deletes the slot (`core/src/binding.cpp:55-80`). Note and Instr have no Song or Global binding. Vol and Fx are in the Learn chord.
- **Copy's Tap fires on press** today. A key named as some binding's `modifier` in the chain becomes a held modifier (`core/include/o3os/core/gesture.h:24-46`, `core/src/binding.cpp:~85-110`). Its own Tap then waits for the release, and is consumed if another button comes while it is held. PAT-NAV's Note works this way (`app/src/bindings.cpp:272, 403`). Up/Down auto-repeat keeps the first press's modifier, and only `kActionRepeatable` actions take the repeats.
- **Order list.** `insert_order`, `set_order` and `remove_order` each record one undo piece, and one dispatched Song action is one unit (`model/src/editor.cpp:468-500`, `app/src/actions.cpp:25`). There is no move or swap. `song.delete_slot` deletes the range and leaves the clipboard alone. An empty song is refused with `SONG NEEDS A SLOT`.
- **NAME column.** On the Order page only: 12 characters, then about 16 px before LEN (Amiga theme; `app/src/screens/song_screen.cpp:55-79, 316-318`). Nothing counts the slots that hold a pattern. SONG-UX's question needs that count too.
- **WAV export.** Song, pattern, selection and stems exist (OS-WAVEXP). `OfflineRequest` has no song-position range; a song render runs from position 0 to the end (`audio/include/o3os/audio/offline.h`, `seq/src/offline.cpp:303`). Files go to `Export/<project>/` and are never overwritten (`save_service.cpp:767-830`).

## 1. Behaviour, by slice

**S1. The shared mark (Level B).**
- **The count.** A use-count table of 256 entries is rebuilt when `song_generation` changes. It is shared with SONG-UX's question: whichever item lands first builds it.
- **The mark.** A slot whose pattern is held by two or more slots gets a one-character mark in the gap after NAME, in the Label face (Q4).
- **The info line** for the cursor's slot reads, for example, `PATTERN 03 · IN 4 SLOTS`.
- **The other slots.** The slots that hold the cursor's pattern draw their PAT tile in a second ink: `Fx`, which the LEN column (Order page) and the BARS column (Arrangement page) already wear: the ink is not new to the screen, and no other PAT tile has it (D6).

**S2. CUT, on Shift+Delete (Level B).**
- **On PAT:** copies the range (no range: the cursor's slot) into the slot clipboard, then deletes it. It is one undo unit. The clipboard itself is not undone, as with Copy.
- **In the map:** a track cut. It fills the song clip at once, as SONG-UX Copy does, then clears the rectangle by SONG-UX's Delete rules, including the shared-pattern question.
- **All the slots:** cutting the whole song is refused with `SONG NEEDS A SLOT`, and the clipboard keeps what it held.

**S3. MOVE, Copy held + Up/Down (Level B, Q1).**
- **What moves:** the range (in the map: the rectangle's slots; no range: the cursor's slot) moves one position. The cursor and the range follow, and the column is kept.
- **Holding the arrow** keeps moving, because the action is `kActionRepeatable`. Each step is one undo unit (D2).
- **At the top or the bottom** the move does nothing.
- **How it is built:** an `insert_order` of the slot at its new place, then a `remove_order` of the old one, in one unit: the list the render reads without a check is never one slot short (a pattern that ends in between would end or loop the song early, `Sequencer::pattern_end`). A full list (255) cannot take the insert: the slots rotate with `set_order` writes, which keep the length. It is allowed while the song plays (D3).
- **The cost of the gesture.** Copy becomes a held modifier on Song. Copy and Shift+Copy then fire on release, and Copy followed by another button is consumed. Shift then Copy still pastes. Pattern and Perform are unchanged, because the modifier is scoped to the context.

**S4. NEXT USE, on Note / Shift+Note (Level B, Q2).**
- **Note** moves the cursor to the next slot that holds the cursor's pattern; **Shift+Note** goes to the previous one.
- **Both wrap.** The strip says, for example, `SLOT 12 (2 OF 4)`.
- **A pattern held by one slot only** says `ONLY IN THIS SLOT`.
- **A plain jump:** the range collapses, as any cursor move does.

**S5. EXPORT of slots (Level C, Q3).** A fourth OP in SONG-UX's F6 TOOLS: **EXPORT WAV**.
- **What it renders:** the rectangle's slots `s0..s1`, with only the rectangle's tracks (`track_mask`). On PAT, or with no range, it renders the cursor's slot with every track.
- **The engine side.**
  - `OfflineRequest` gains `order_first` and `order_last`.
  - The offline render starts at `play_song(order_first, 0)`.
  - The sequencer gets an order stop at the end of `order_last`, a Config field like `pattern_stop`, read only by the offline path.
  - Progress counts from `order_first`.
- **The tail** is kept, as for the song (D4).
- **The file** is `Export/<project>/<project>_S<a>-<b>_0001.wav`: never overwritten, the name cut to 31 characters.
- **The rest** is today's: the export stops the transport and shows the progress overlay (F8 CANCEL).
- **Its own gates:** a risk review, a host test in `seq_offline_test`, and `tools/ci.sh qemu` before the merge.

## 2. Decisions

One table. **Q** rows are for the maintainer: the recommended option comes first and is applied until answered. **D** rows are defaults, to confirm.

| # | Question or default | Options (recommended first) | Applied |
|---|---|---|---|
| Q1 | The move gesture | a) Copy held + Up/Down (the approved row); on Song, Copy and Shift+Copy then fire on release · b) Note held + Up/Down, and Copy stays on press | a |
| Q2 | The next-use key | a) Note next, Shift+Note previous, both wrap · b) Shift+Right / Shift+Left on the PAT column (in the map they extend the tracks) | a |
| Q3 | The range export, which is Level C because of the sequencer's offline stop | a) a separate slice S5, last, started from TOOLS as EXPORT WAV over the rectangle · b) out of this item | a |
| Q4 | The shared mark | a) one mark character in the gap after NAME, with the count in the info line · b) a `×N` count, with NAME cut to 10 characters | a |
| D1 | CUT in the map | fills the song clip at the press; the clear follows SONG-UX's rules and question | applied |
| D2 | Undo of a held move | one unit per step, no grouping (OS-UNDOGRP Q3a groups parameter rows only) | applied |
| D3 | Moving while the song plays | allowed, as insert and delete are today; the play position stays a position (the reviewer checks `sequencer.cpp:538-562, 810-825`) | applied |
| D4 | The export's tail | kept, as for the song (PAR-151 cuts only selections sharply) | applied |
| D5 | The use-count table | 256 entries, rebuilt on `song_generation`, shared with SONG-UX | applied |
| D6 | Same-pattern PAT tiles | drawn in the `Fx` ink, the ink of the same row's LEN (Order page) or BARS (Arrangement page) number, and of the shared mark of the other slots | applied |
| D7 | The order stop and a jump (S5) | the stop is checked where the next order position is decided (`Sequencer::pattern_end`), so an Order jump FX that lands past the last slot ends the render with it; a jump back inside the range loops, as it does in the song, until the measuring pass reaches the length cap (`TOO LONG FOR A WAV (100 MIN)`, `ExportSource::kMaxFrames`) and F8 cancels it sooner; no `SongEnd` event for the stop (nothing consumes it offline) | applied |
| D8 | Which tracks the export takes (S5) | a slot or track range selected in the map: the rectangle's tracks as `track_mask` (a muted or soloed track in them sounds, as in the Pattern selection); on PAT or with nothing selected in the map: every track, `kAllTracks`, the project's own mixer, as the whole song (`kAllTracks` is 0, no mask, so a rectangle over all 16 tracks is the mask `0xFFFF`, a choice: a muted track in it sounds; a slot range with no track range in the map renders the cursor's track only) | applied |
| D9 | The file name (S5) | the slot numbers as the Song screen counts them (slot 0 is `S0`), not padded: `Untitled_S0-1_0001.wav`, `Untitled_S12-255_0001.wav`; the project part is cut as for `_P<n>_` | applied |
| D10 | The dialog (S5) | EXPORT WAV has the OP row alone, a dim line says what F1 does, the footer says `ALL TRACKS` when every track is rendered; the progress overlay reads `EXPORTING THE SLOTS` | applied |

## 3. Done when

1. **Host tests** in `tests/song_edit_test.cpp`:
   - the use count and its cache;
   - CUT on PAT (clipboard, list, one unit, the empty-song refusal) and in the map;
   - MOVE up and down (order, cursor, range, edges, repeats, one unit a step);
   - NEXT USE (wrap, the only-slot text).
2. **Gesture tests** in `tests/gesture_test.cpp`: Copy as a held modifier on Song (release Tap, consumed by Up, Shift+Copy still pastes).
3. **Binding tests:** `tests/builtin_bindings_test.cpp` lists the new Song rows and the Shift list.
4. **S5 only:** `seq_offline_test` covers `order_first..order_last`, the stop and the progress. `tests/file_export_test.cpp` covers the file name.
5. **Simulator:** `sim/scenarios/qa/qa_song.txt` snaps the mark, the info line and a moved range. Goldens are regenerated, and `budget_test` stays under 1792.
6. **QA and docs:** QA rows QA-SNG-050..059 in `docs/qa/catalogue/song.md`, and the ui-map Song row. The manual's Song chapter is OS-MANUAL2 C3's.
7. **Build:**
   - `tools/ci.sh host` is green, the `mk66` build has no warnings, and the deltas are in the commit bodies.
   - S5 also runs `tools/ci.sh qemu`.

## 4. Hardware-only checks

- **Moving while playing:** a held Copy+Down moves a slot while the song plays, by ear.
- **The export:** a slot-range WAV, cut at the right pattern end, with the tail.

## 5. Budget [est]

- **FLASH:** S1-S4 about +2 KB; S5 about +1.5 KB.
- **SRAM:** 0. The use-count table is 256 B in the App's SDRAM.
- **CPU per audio block:** 0. S5's order stop is a compare at the pattern boundary, offline only.

## 6. State for implementer

- Start after SONG-UX merges: S1-S4 need its column model.
- Order: S1, S2, S3, S4, then S5 on its own, with its risk review.
- QA ids QA-SNG-050..059.
- Apply the Q rows' recommended options until answered, and record any new choice as a D row.
