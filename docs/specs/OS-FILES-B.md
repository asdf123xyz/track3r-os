# OS-FILES part B: rename, delete by marker, recents, purge unused

BACKLOG #6. The maintainer approved OS-FILES on 2026-10-14: delete, rename, recents, purge of unused samples, import, and export to OG/.pti. Part A, import, is done. Written on 2026-10-14. Line refs come from version 0.4.20.

Parity:
- **Documented in the release notes (not verified):**
  - PAR-108: delete with a confirmation, blocked for the active project; a Recent list.
  - PAR-050: "delete all unused instruments" in one action.
- **New Community OS behaviour:** DELETE hides the folder with a marker file instead of erasing it; recents are kept in a new small file; the key homes.

**Level C** (the card's write guard changes), size M. No change to any existing file format. The new recents file is a new format, versioned from its first byte.

**Amendment, 2026-10-15 (after a power-cut test).** A FatFs rename across folders is not one directory update. A DELETE moving a folder to `Trash/` took 3 blocks, and at 2 of 4 cut points the folder existed under both names. Two directory entries then share the same clusters. If one copy is later deleted on a computer, its clusters are freed and the other copy is silently corrupted. So:
- **DELETE no longer moves anything.** It creates an empty marker file `Projects/<name>/.deleted`. That is one directory entry and no cluster, a single sector write. The File list and the recents hide a folder that holds the marker.
- **PURGE does nothing on the card.** The unreferenced sample files stay in `samples/` until a cleanup that the maintainer decides (OS-TRASHUI).
- **There is no `Trash/` folder and no cross-folder rename.**
- **RENAME was kept in the same folder in this amendment, and dropped by the next decision (§2).** Its window is smaller but not zero, if the new entry lands in another sector. That is the same risk as the save layer's own renames. The review states how the save layer's boot repair treats a pair of entries sharing clusters, and RENAME gets the same treatment. Otherwise it is a documented limit.
- **No startup sweep.** With the marker it is not needed.

## 0. Who decides

The guard today removes nothing a user made, and never a folder (`hal/save_guard.h:29-32`, `may_remove` `:245`). **This spec keeps that rule.** The new operations are a same-folder rename and an empty marker file: nothing on the card is erased, and everything can be put back from a computer. It is reversible, so the maintainer decides it.

**Real erasure is out of this item and is a maintainer decision**: erasing a marked folder or unreferenced sample files. It is listed as IDEA `OS-TRASHUI` (restore and erase from the device).

## 1. The guard additions (`hal/save_guard.h`, walked by `tests/save_guard_test.cpp`)

| New path or operation | Kind |
|---|---|
| rename `Projects/<a>` → `Projects/<b>` | RENAME: the same parent; `<b>` valid_name() and must not exist |
| `Projects/<name>/.deleted` | marker, created empty (create-new, never written to, never replaced): DELETE |
| `Settings/recent.o3rc`, `.bak` | Target and Backup (the recents, §4) |

`may_remove` does not change. No call removes a file a user made, or a folder.

## 2. RENAME (F8 on the PROJECTS page): dropped, see amendment

**Dropped by decision (b) after the sweeps** (§8): even a rename inside one folder writes two directory entries, which can sit in different sectors, and a cut between them leaves the folder under two names on one chain. F8 stays blank on the PROJECTS page. A project's title is renamed through OS-NAMES; a new folder name comes from opening the project, SAVE AS under the new name, then DELETE of the old one. The guard has no folder rename at all.

## 3. DELETE (the Delete key on the PROJECTS page; UI map rule 6)

- **The first press asks** in the strip: `DELETE <NAME>? DELETE AGAIN`.
  - **The second press goes on.** Any move, any other key or leaving the screen takes the question back. This is the same mechanism as F3 NEW (`FileView::confirm_new`, `app/file_view.h:34-39`), with its own flag.
- **It is refused for the open project** (PAR-108): `CANNOT DELETE THE OPEN PROJECT`.
- **What it does:** creates the empty marker `Projects/<name>/.deleted` (amendment above).
  - The strip says `DELETED: <NAME>`, and the name leaves the list and the recents.
  - The File list hides every folder that holds the marker. The user doc explains that the folder is still on the card: delete it on a computer to free the space, or remove `.deleted` to bring it back.
- **Failure** (card removed, card full for the directory entry, write-protected): `DELETE FAILED: <REASON>`. A cut leaves the marker either present or absent, and both states are consistent.

## 4. Recents (the top of the PROJECTS list)

- **The PROJECTS list starts with a `RECENT` group: the last 5 projects opened or saved, newest first, then the existing A–Z list.**
  - A recent row is the same folder as its A–Z row; both open it.
  - There is no new key. The group is hidden when it is empty.
  - Rows that no longer exist on the card are skipped and dropped at the next write.
- **The file is `Settings/recent.o3rc`.** It is text:
  - line 1 is `O3RC 1`;
  - then up to 5 lines, each a folder name.
- It is written through the save layer (Temp, then Target, with a Backup) after a project open, save, rename or delete. A write failure is silent: it costs only the list.
- **Loader:**
  - A missing file, a bad header or a line that is not a valid name gives an empty list, or skips the line. The project is never touched.
  - A higher version number is read as an empty list.
  - Round-trip test.

## 5. PURGE UNUSED (Shift+F7 on the File PROJECTS page, on the open project)

- **What it removes:** sample and MIDI instruments that no step of any pattern names (PAR-050).
  - Module instruments are skipped: an instance cannot be unbound (OS-INSTUI DELETE note). The strip counts them.
  - Instruments the Perform screen targets count as used.
- **The first press asks:** `PURGE <N> UNUSED INSTRUMENTS? SHIFT+F7 AGAIN`, using the same confirm mechanism. If N = 0, it says `NO UNUSED INSTRUMENTS`.
- **The second press** runs `Editor::clear_instrument` on each, in **one** Editor group. That is one undo unit, labelled `UNDO PURGE`, and F8 UNDO on any document screen brings them all back. The sample memory comes back at the next project load, as with DELETE in OS-INSTUI.
- **On the card: nothing** (amendment above). After the next SAVE, the purged instruments' files in the project's `samples/` folder are no longer referenced and simply stay there. Removing them belongs to OS-TRASHUI, a maintainer decision. The engineer reports what SAVE does today with a cleared slot's file.
- **Shift row of File:** the quarter jumps on F1–F5 as today, and `PURGE` on Shift+F7. Update the Shift label row (OS-SHIFTROW table) and `shift_labels_test`.

## 6. Export to OG and .pti: what is covered

- **One instrument as `.pti`**: OS-INSTUI S7 (Instrument Shift+F5 SAVE to `instruments/`). Nothing here.
- **WAV**: OS-WAVEXP.
- **A whole project in the OG format: not covered.** There is no OG project writer; `formats` has only the native and settings writers.
  - This becomes **OS-FILES part C** (size L, Level C): an OG project writer (the project, patterns, and `.pti` instruments) with the ADR 0004 drop report. It is verified by reading back through our OG importer in host tests. Its home is a row `OG PROJECT` on the File EXPORT page.
  - It needs its own spec when it comes up in the queue. Add it to BACKLOG as APPROVED (it is part of the maintainer's approval), after part B.

## 7. Acceptance

**`save_guard_test`:**
- every new rename is allowed;
- every near miss is refused: a rename to a different parent, an existing target, a marker anywhere but directly inside a project folder, writing to a marker;
- `may_remove` is unchanged.

**`file_projects_test`:**
- RENAME: accept, cancel, taken, empty, refused for the open project, recents follow;
- DELETE: one press asks, a move takes it back, the second press creates the marker, a marked folder leaves the list and the recents, refused for the open project;
- RECENT: order, cap at 5, missing rows skipped;
- PURGE: count, confirm, one undo unit restores, module instruments skipped, Perform targets kept.

**`recent_file_test`:** round trip, bad header, version 2, invalid lines.

**Save:** a test that SAVE after PURGE touches no file in `samples/` and the project no longer references them.

**Power cut:** the engineer's cut-at-every-block test, repeated for DELETE (marker) and RENAME, with the results recorded.

**QA rows:** QA-FIL for rename, delete, recents and purge.

**HIL `qa_files_b.json`, on the test card:**
1. Create a scratch project with SAVE AS.
2. RENAME it.
3. DELETE it; it is gone from the list.
4. Maintainer check from a computer: the folder is still in `/Projects/` and holds `.deleted`.

**Docs:**
- UI map §1, the File row (F8 RENAME, Delete, Shift+F7 PURGE, the RECENT group).
- The user doc: what `.deleted` means, how to free the space or restore from a computer.
- BACKLOG: part B goes to DONE, part C is added.

## 8. Built (from version 0.4.20; reworked twice: the 2026-10-15 marker amendment, then the RENAME drop)

Status: **implemented, host / QEMU / MK66 CI green; nothing here has run on the unit.** Level C. The first version moved a deleted folder to `Trash/`; its power-cut sweep showed a rename across folders is unsafe (3 block writes, both names at 2 of 4 cut points) and DELETE became a marker. The sweeps of the same-folder RENAME then showed the same hazard (below) and RENAME was dropped. The earlier commits stay in the history; the commits on top of them remove each part.

**Proof.** `O3OS_CI_KEEP=1 O3OS_CI_JOBS=4 nice -n 10 tools/ci.sh host`: exit 0, ctest 131/131, no warning; the unit binary runs 3,375 cases, 51 of them new (`file_projects_test` 24, `instrument_use_test` 11, `recent_file_test` 9, `save_guard_test` +4, `save_fatfs_test` +3). `ci.sh qemu`: exit 0 (`recent_file_test` also runs on the Cortex-M4F). `ci.sh mk66`: exit 0, both images, `.ramfunc_l` unchanged (27,512 B, ends 0x1fff6b78). Against version 0.4.20, same build: **flash +6,616 B** (707,232 to 713,848; 68.08 % of 1 MB), SRAM_U +0 (136,136 B), SRAM_L +0, **SDRAM** +32 B of statics and `SaveService` 7,744 to 8,136 B (+392 B off the sample arena's tail).

**The guard as it ends up (`hal/save_guard.h`).** 0.4.20's rules plus exactly two things, each tested in `save_guard_test`:
- `Settings/recent.o3rc` and its `.bak` (Target, Backup).
- One new kind of file, `Place::Marker`, `Projects/<name>/.deleted`, through `StorageWriter::create_marker` (create-new, close, no byte written; Exists when it is there). `may_create` stays Temp only, `may_remove` stays Temp and Backup only, `may_rename` is Temp to Target, Target to Backup, Backup to Target in one folder: nothing else. **A project folder is never renamed** (a test refuses every `Projects/<a>` to `Projects/<b>`, a change of case, `Trash/`, `Export/`, `instruments/`, traversal, and every file of a project to anywhere). The open-project refusal is `SaveService`'s (`OpenProject`). There is no `Trash/`, no startup sweep.

**Why RENAME is gone (measured, `save_fatfs_test` in the first versions).** FatFs makes the new directory entry set (a long name is long-name entries plus the short entry, which can straddle a sector) and then removes the old set; a cut between leaves the folder under both names on one chain, and deleting one on a computer corrupts the other. Cut-at-every-block sweeps with 31-character names and eleven directory alignments: **RENAME inside `Projects/` took 1 to 5 block writes, both names at 9 of 42 cut points** (old name only 20, new only 13); with one block failing once: Ok 3, Io 19, Damaged 9. A rename across folders: 3 blocks, both names at 2 of 4. The marker: 1 block always.

**The save layer's boot repair and a pair of entries on one chain (file:line).**
- `app/src/atomic_save.cpp:92-115` (`AtomicSave::repair`): only renames a Backup back to its Target (`moved_old_`) and removes the job's own Temp. `:198-211` (stage `Restore`, step 2 of a later save of the same file): B back to F, a stale T removed, and `writer_->remove` answers `Damaged` when T shares its data with another name.
- `platform/mk66/save/src/fat_storage_writer.cpp:223-` (`shares_chain`, called from `remove()`): looks at the folder's FILES only (`:248` skips every directory: `(info_.fattrib & AM_DIR) != 0u`) and refuses to remove a Temp or Backup whose first cluster another guarded file uses. Nothing runs at mount (`platform/mk66/storage/src/*.cpp` has no repair or cross-link check).
- So a pair of entries on one chain is detected only for files the save layer made, only when it is about to remove one, and never for a folder; a half-done folder rename could not be fixed from here (the only fix removes a directory entry, which is an erase the guard forbids, and `f_unlink` of a folder frees the clusters the other name uses). That is why RENAME could not be made safe and was dropped.

**DELETE.** `file.delete` asks (`DELETE <NAME>? DELETE AGAIN`), remembering the entry's DirIndex key and the listing's generation (`FileView::target_key`, `target_generation`); the second press goes on only over that entry of that listing (a move, another key, leaving the screen, or a rescan or a still-running scan, which moves the generation, asks again: review finding 1, tested). It creates the marker, says `DELETED: <NAME>`, and takes the name out of the RECENT list. `DirScan` takes a `ListFilter::skip_probe` (`.deleted`, `kProjectFilter` only): a folder that holds the marker is not listed, so the list and the RECENT group hide it, including one marked from a computer (a name in `recent.o3rc` of such a folder is skipped and dropped; tested). Cost: one more probe a folder (the `dir_scan_test` bounds are 4 and 160).
- **A marked folder is never loaded** (re-review finding 1): `app::folder_marked_deleted` is asked by `file_open` (the File screen, a RECENT row, a stale row of a project marked since: `THIS PROJECT WAS DELETED`) and by `start_project_load` in `platform/mk66/src/app_main.cpp` (the console's `diag load`, the File screen's sink, the boot), which answers `BadPath`. So a SAVE never writes into a hidden folder. The platform half is not host-testable; the function and the File screen half are (`file_projects_test`).
- **A FOLDER called `.deleted` is not a marker** (re-review test 3, decided): the project stays listed and opens, and DELETE on it fails with the card's words (`DELETE FAILED: NAME EXISTS`) and creates nothing. SAVE AS onto a hidden folder's name says `A PROJECT OF THAT NAME EXISTS`.
Removing `.deleted` on a computer brings the project back.

**PURGE.** The document only: `Editor::clear_instrument` on each unused sample and MIDI instrument in one `file.purge` group (`UNDO PURGE` on the strip); not one call on the card, before or after a SAVE (tested; SAVE AS copies only what the project names). The purged instruments' files stay until OS-TRASHUI. What SAVE does today with a cleared slot (checked): `samples_to_write` writes slots that have audio and an instrument; a cleared slot's `samples/<file>.wav` is left on the card.

**PURGE references (`model/instrument_use.h`), one test case each.**
1. A step's instrument byte: any pattern of the pool, any of the 16 track slots (past `active_tracks`), any of 128 steps (past a track's length); bytes 0 and above 64 name none.
2. Random Instrument (FX 6, either column): the window slot +- value (the columns add), clamped; on a step with no instrument, the closure of what the track names under the widest such window.
3. A MIDI Learn binding of the project's store that targets an instrument parameter; a "follow" binding names none.
4. The S7 instrument clipboard: the slots that read the clipboard's audio block are passed in as `held` and kept.
Not references: song order, tracks, master, the Perform slots and scenes (FX per track; there are no Perform instrument targets in this tree), mixer, MIDI, USB, note, mute, send-FX state. **There is no pattern clipboard in this tree** (Pattern Copy / Shift+Copy are unbound; the Song clipboard holds pattern numbers); when one exists its steps' instrument bytes are references of kind 1 and belong in `held`. **Instrument numbers held inside SD apps (RhythmGen and the others keep their own parameters) are outside the model**: a purge may remove an instrument such an app plays, and an undo brings it back. Module instruments are never purged and are counted.

**The marker sweeps (`save_fatfs_test`).** Long names (31 characters) beside the marker and eleven directory alignments (0 to 10 long-named files in the folder): 22 power-cut points, 1 block write in every alignment, absent 11 and present 11, never half (a listing and `exists` agree every time; the project and everything else byte for byte whole; the next call is Ok or Exists); a block failing once at every block: Io in all 11 (nothing made, the next call settles it).

**`Settings/recent.o3rc`.** Text, version 1: `O3RC 1`, then up to 5 folder names (valid_name), newest first, LF or CR LF; at most 173 bytes. `RecentList::parse` never fails: a missing, empty, binary, over-long file, a header that is not exactly `O3RC 1` (a version 2 included), gives an empty list; a bad, empty or repeated line is skipped. Round trip, bad header, version 2, invalid lines, a flip of every byte and a cut at every length: `recent_file_test` (also on QEMU). Written through the atomic save a pass after a change (open, save, delete), quietly; a failed write is silent. A save, delete or export that comes while the write runs is not refused: the write is cancelled (`yield_recent`) and done when the card is idle. A name missing from the first complete listing is skipped and dropped at the next write.

**Keys and precedence.** F8 on the PROJECTS page is blank (`file.report` stays the IMPORT page's F8; the EXPORT field context keeps F8 blank). Delete `file.delete` and Shift+F7 `file.purge` are in `kFileCtx`; the EXPORT field context binds Shift+F7 to `ui.none`, the handlers refuse the IMPORT page, and `shift_row` draws `0/4 1/4 2/4 3/4 END - PURGE -` on PROJECTS only. F7 is untouched. `builtin_bindings_test` and `shift_labels_test` resolve all of it.

**Deviations from the (amended) spec.**
1. RENAME dropped (decision b); QA rows 030, 031 and 043 are kept and marked dropped.
2. The marker is created through a new `StorageWriter::create_marker` (a pure virtual: `FatStorageWriter`, `MemCard`), not `create()` + `finish()`, so `create()` stays Temp only.
3. The info line reads `F2 rescan   Delete: delete`.
4. The RECENT group shows once the scan is complete, `RECENT` at the right edge; POS reads `R1`..`R5` there and ITEMS counts projects.
5. A folder whose name the layer does not write (over 31 characters, or a character outside letters, digits, space, `_`, `-`, `.`) cannot be deleted on the unit: `NOT A NAME THE CARD TAKES: USE A COMPUTER`.
6. `tools/check_layers.py` rule 8 lists `app/recent_list.{h,cpp}`.
7. BACKLOG.md is not touched.

**Hardware verification still needed** (`qa_files_b.json`, QA-FIL-032..044): DELETE on a real FAT32 SD (the marker hiding the folder, `.deleted` removed on a computer bringing it back, `diag load` of a marked folder refused); `recent.o3rc` across a reboot; the cost in ms of one marker create (inside one pass); a card pulled in the middle of a DELETE (the marker present or absent).
