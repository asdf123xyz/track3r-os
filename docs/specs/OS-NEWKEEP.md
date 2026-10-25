# OS-NEWKEEP: File > NEW asks ALL or KEEP INSTRUMENTS

BACKLOG OS-NEWKEEP, decided by the maintainer on 2026-10-16. **New Community OS behaviour**, Level B, size S; no parity with the official firmware is claimed. No format change: both answers produce a project the existing writer and loader handle.

**Root cause of "instruments 1-4 stay".** They were not the old project's: `Project::reset()` already emptied the instrument table and `begin_samples` emptied the sample memory, then `demo_then_modules(false)` (platform/mk66 `app_main.cpp`) saw a project with no sample instrument and installed the demo kit (four sample instruments with generated audio). NEW no longer installs it.

**Gesture** (File, PROJECTS page; F3 stays NEW, no new key): F3 asks in the strip, `NEW PROJECT: F3 ALL, F1 KEEP INSTRUMENTS`, or `UNSAVED CHANGES: F3 ALL, F1 KEEP INSTRUMENTS` when no save has taken the changes (the OS-OPENGUARD guard is this question). F3 again is ALL, F1 is KEEP INSTRUMENTS. Any other key, a cursor move, a page change, leaving the screen or 4 s without an answer cancels: nothing changes, and F1 is OPEN again. Refused while a load or a save runs. The question only counts while its text is the line on the strip: with another source's message in front of it (it queues behind), a second F3 asks again instead of answering ALL, so nobody answers a question nobody has read. If the project is changed after the plain question was asked, the answer asks once more with the unsaved-changes text.

**ALL**: patterns, song, names, settings and the whole instrument table (sample and module) and the module table are emptied; the sample memory is freed (`SampleBank::clear`); the transport stops and the voices settle first, as for a load.

**KEEP INSTRUMENTS**: the same, except the instrument table and the module table stay byte for byte, the sample memory and the voices' table stay (the audio plays at once), and the module instruments are bound again. The new project has pattern 1 (empty) and one song position, as ALL.

**Save after KEEP.** The new project has no folder, so no slot's audio counts as held by the old project's folder (`SampleBank::new_project` zeroes `card_version`): the first SAVE writes every sample into its own `Projects/<name>/samples/` (instrument paths are relative), and the old folder is not touched. The new project has an empty undo journal, so KEEP also forgets the bank's bookkeeping of the old one: the swap records, the sample edit history and any recorder take (their blocks would otherwise never be reclaimed, and a take could be committed into the new project); the audio of the slots stays.

**Code**: `Project::reset(keep_instruments)`, `NativeFolderLoader::begin_fresh(arena, keep)`, `ProjectSink::start_new_keeping_instruments()`, `SampleBank::new_project(keep)`, `FileView::new_choice`. Tests: `save_app_test` (`new ...`). Unit check: QA-FIL-070.

## OS-NEWKEEP2: a cancelled question leaves the strip (2026-10-16)

**New Community OS behaviour**, Level B, size S. Before, a cancel (another key, a move, a page change, leaving the screen) only cleared the flags: the question's text stayed on the strip for the rest of its 4 s with nothing left to answer. Now `file_question_sync` (`app/src/file_actions.cpp`, called once a pass from `App::run_once` after `poll_files`) takes the text of the three File questions off the strip (source "file", exact text: NEW's plain and unsaved-changes questions and OS-OPENGUARD's `UNSAVED CHANGES: F1 AGAIN TO DISCARD THEM`, also used by the IMPORT page's F1) whenever neither `new_choice` nor `confirm_new` is open. It is one place for every way of cancelling (all of them go through `FileView::ask_clear()`), the same code serves NEW and OPEN/IMPORT, and an answer needs nothing special because it has already replaced the text. `NotifyQueue::dismiss(source, text)` is the new overload. Tests: `save_app_test` (`new: a cancelled question leaves the strip`, `open: the unsaved-changes question leaves the strip`), `notify_test`. Unit check: QA-FIL-080.

Defaults the request did not settle (default, to confirm by the maintainer): OPENGUARD's question is dismissed the same way (it is the same flags and the same strip); the 4 s expiry itself is unchanged; a question queued behind another message is removed from the queue too.


## What NEW leaves alone (OS-BUGS4, from the v0.6.0 unit run)

After NEW (ALL or KEEP) the `diag state` line still showed `row=31 instrument=4 tempo=10500`. The project is reset (`Project::reset`: tempo 130.00, one empty pattern, no song); the three numbers are not the project's:
- `row`, `col`, `track`: the Pattern screen's cursor, and `instrument`: the last used instrument. They are view state, which a load does not reset either (`App::document_replaced` touches neither); the instrument table of ALL is empty, so `instrument` points at an empty slot. (default, to confirm by the maintainer: NEW puts the cursor on row 0 and the instrument in view on 1.) No code changes until decided.
- `tempo`: the state line shows the sequencer's last published tempo while it has one (`app/src/state_line.cpp`); the sequencer takes the project's at the next Play (`seq/src/sequencer.cpp`, the fresh-run `tempo_sm_.set`), so the reading is stale while stopped, not the project's tempo. Cheapest unit check: Play after NEW and read `tempo=13000`.
