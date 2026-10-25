# OS-H4: export writes for apps (ADR 0014 amendment A); SMF EXPORT

ADR 0014, slice H4. Written on 2026-10-15. Line refs come from version 0.4.21 (which has H3B / ABI 1.6).

This is **new Community OS behaviour**. The ABI and the card writes are **Level C**: the ABI is published and frozen, and the writes go to the user's card. The SMF EXPORT app is Level B. Size M. There is no persistent format change: the project, the instrument and the settings files are untouched.

## 0. Decisions (2026-10-15)

1. The folder is **`AppExport/<app>/`**. On a computer, `Export/` (the WAV export, `Export/<project>/`) and `exports/` read as the same folder, and `AppExport` cannot collide with a project name. ADR 0014 amendment A is amended to match, and C2's read access later uses the same folder.
2. SMF EXPORT v1 exports the pattern in view, or the F7 selection when there is one. Details are in §3.
3. The caps are 4 MiB per file, at most 16 files per launch, and a numeric suffix up to 999.
4. The calls never block. export_write copies into an SDRAM staging ring and returns BUSY when the ring is full. export_close only requests the commit. `export_status()` tells the app the result.
5. Extensions come from an allowlist.
6. A file left open when the app exits or faults is aborted.
7. TOOLS lists `edit_project` OR `export_files`.
8. One confirm dialog shows both lines.

## 1. ABI 1.7 (additive)

`O3M_ABI_MINOR` goes from 6 to **7** (`module/sdk/o3module.h:76`). The OS now grants `O3M_CAP_EXPORT_FILES` (16, `o3module.h:332`, today "assigned to a later slice"). Update the 1.7 line of the version history at the top of `o3module.h`, and the refusal-at-load text (`o3module.h:325`: an app asking for a bit the OS does not grant). An app that asks for bit 16 needs a file of ABI 1.7.

The io table (`o3m_io_api`, `o3module.h:540`) grows four members at its end, after `tap_levels`. Each is NULL without bit 16, and absent when `size` is below its offset (`O3M_HOST_HAS`). Add an `O3M_ABI_ASSERT` for the new offsets, and add a literal copy of the 1.6 layout to the layout test (as `module_edit_test` keeps for 1.2..1.4).

```c
#define O3M_EXPORT_OK      0
#define O3M_EXPORT_BUSY    1   /* the staging ring is full: call again on a later frame with the same bytes; nothing was taken */
#define O3M_EXPORT_RANGE   2   /* a bad name, an extension not on the list, a NULL buffer, n == 0 */
#define O3M_EXPORT_LIMIT   3   /* over a cap: the file's 4 MiB (the file is aborted), the 16 files of a launch, or no free suffix (999 taken) */
#define O3M_EXPORT_STATE   4   /* export_open while a file is open or committing; export_write / export_close with no file open */
#define O3M_EXPORT_NO_CARD 5   /* no card, a card that may not be written (write protected, exFAT), or the save layer is busy with a save it cannot share */

#define O3M_EXPORT_IDLE    0   /* nothing open, nothing committing (also after the app has read DONE or FAILED once) */
#define O3M_EXPORT_WRITING 1   /* open, or closed and still committing */
#define O3M_EXPORT_DONE    2   /* the last file is whole on the card under its final name */
#define O3M_EXPORT_FAILED  3   /* the last file is not on the card (nothing of it is left: the Temp was removed) */

int32_t  (*export_open)(const char* name, const char* ext);
int32_t  (*export_write)(const void* buf, uint32_t n);
int32_t  (*export_close)(void);
uint32_t (*export_status)(char* final_name, uint32_t cap);  /* final_name may be NULL; on DONE it gets "<stem>.<ext>" as written */
```

- **export_open(name, ext):**
  - `name` is the stem. It must pass `hal::guard::valid_name` (save_guard.h:141; the module-name character rules, at most `kNameMax` 31 bytes), with no path components. Otherwise RANGE.
  - `ext` must be on the allowlist **`mid`, `syx`, `txt`, `csv`, `json`**, lower case, with no dot. Anything else is RANGE. No `.ptf`, `.tmod`, `.o3pj` or `.wav` file can ever come from an app.
  - If `<stem>.<ext>` exists in the folder, the name becomes `<stem>_001` and so on up to `_999`, using the WAV export's `free_name` (save_service.cpp:748, 794) with 3 digits. Past `_999` the result is LIMIT. The stem is cut so that the suffix fits in 31 bytes, as the WAV export cuts its stem.
  - At most 16 opens per launch. The 17th is LIMIT. The count restarts when the app is launched again.
- **export_write(buf, n):** copies `n` bytes into the staging ring, or returns BUSY and takes nothing. The ring must not be smaller than any single n the app may pass: say so in the guide, and return RANGE for an `n` larger than the ring. A write that would take the file past **4 MiB** returns LIMIT and aborts the file: the Temp is removed, and the status becomes FAILED.
- **export_close():** requests the commit and returns at once. The status stays WRITING until the save job has drained the ring, synced, read back and renamed. The result is then DONE or FAILED.
- **export_status():** reading DONE or FAILED once returns the status to IDLE.

**`o3m_instrument_info` grows one member at its end:** `uint32_t midi_channel`. It is 1..16 for a MIDI instrument (`Instrument::midi_channel`, OS-INSTUI S8a) and 0 for any other kind. The struct is already size-versioned (`size` in and out, `o3module.h:427`), so a 1.6 app's shorter struct gets the old bytes only. It is a read-table change, with no capability change. SMF EXPORT is its client in this slice.

## 2. The host side (app, hal)

### 2.1 The guard (`hal/include/o3os/hal/save_guard.h`, Level C)

New places, and nothing else changes:

| Path | Place | Rule |
|---|---|---|
| `AppExport` | Folder | make_dir |
| `AppExport/<app>` | Folder | `<app>`: `valid_name` (the app's catalogue stem, the `.tmod` file name without extension) |
| `AppExport/<app>/_o3save.tmp` | Temp | create, write, remove (abort and failure only) |
| `AppExport/<app>/<stem>.<ext>` | Target | `<stem>` `valid_name`, `<ext>` on the allowlist; created by the NewOnly rename of the Temp; **never replaced, never removed, never renamed** |

- Add `kAppExportDir[] = "AppExport"` and the allowlist as constants next to `kExportDir` (save_guard.h:51).
- Check that `kPathCap` (96) still holds the longest path: "AppExport/" + 31 + "/" + 31 + ".json" is 79 bytes plus the NUL, so it fits. State it in the comment as line 62 does.
- Add guard tests, pure and constexpr where the existing ones are:
  - every allowlisted extension, and a refused one (`ptf`, `tmod`, `o3pj`, `wav`, `PTF`, an empty extension, `mid.ptf`);
  - `..`, `/` and `\` in the stem or the app;
  - `AppExport/<app>/<file>` as a rename source, as a remove target, and as a Backup;
  - `AppExport` paths deeper than three components.

### 2.2 The export service (new, `app`: `app_export.h/.cpp` or inside `module_ui`; the engineer chooses and says why)

- **One open file per app at a time.** Only one app runs at a time.
- **Staging ring:** **16 KiB in SDRAM_STATIC** (82,816 of 196,608 B used at 0.4.20), static, single producer (the app's call in the main loop) and single consumer (the save job, also in the main loop). No allocation.
- **Commit:** through `AtomicSave` in **NewOnly** mode (atomic_save.h, steps 1..6), with the same gates as a core save: a mounted, writable FAT card (no marker file: OS-ANYCARD), and the busy state of the save layer. The ring is the job's `SaveSource`. It **cannot restart**: a step that would need `begin()` again (a retry) fails the export instead (FAILED, Temp removed). Say so in the source's comment.
- **Bytes leave the ring while the app is still writing.** The job's slices run in the main loop's job pass, so the ring is the only RAM a 4 MiB file needs. The time the job takes is the host's, not the app's (as close_group, module_ui.cpp:1488).
- **The CRC read-back of step 5 stays.**
- **Abort:**
  - when the app exits with a file open or committing, when it is stopped for its budget or faults, or on a 4 MiB overrun, the job is cancelled at its next step and the Temp is removed (`may_remove` allows a Temp only);
  - **files already DONE stay**;
  - nothing else under `AppExport` is ever removed.
- **A card pulled or full mid-export:** FAILED and the Temp is left for the next export's heal (step 2 removes a stale Temp). The heal must be allowed in `AppExport/<app>/`.
- **The UI:**
  - while a commit runs, the app's screen keeps drawing;
  - a save requested by the user while an export commits waits its turn, or the export gets NO_CARD at open. Pick the one the save layer already supports, and test it;
  - the core's own saves are never refused because of an app.

### 2.3 Launch, confirm, TOOLS (`app/src/module_ui.cpp`)

- **Grant** (module_ui.cpp:771 for edit): `io.export_*` are set only when `caps & O3M_CAP_EXPORT_FILES` and the file's ABI is 1.7 or later.
- **First-launch confirm** (module_ui.cpp:732-735; once per boot, not stored):
  - an app with export_files is confirmed like an edit app;
  - the text names the folder: **"THIS TOOL WRITES FILES TO APPEXPORT/<APP>"** (fit the dialog's width; the exact wording is the engineer's within the dialog's rules);
  - with both bits, **one dialog shows both lines** (edits are undoable / writes files).
- **TOOLS** (module_ui.cpp:1633): Pattern F7 lists apps that declare `edit_project` **or** `export_files`. That way SMF EXPORT gets the selection as its launch context.
- **The app page lists the capability** as the other bits are listed.

### 2.4 Tests (host)

- **Fuzz** of the four calls with the others, in and out of an open file, including random names and extensions and n from 0 to beyond the ring, and exit or fault at random points. Assertions:
  - every return code is published;
  - no path outside `AppExport/<app>/` is ever created;
  - no existing file is ever opened for writing, renamed or removed;
  - after an abort, no Temp is left.
  Use the fake storage the save tests use.
- **Property tests:**
  - a file written in random chunk sizes, with BUSY retries, is byte-identical on the card;
  - two exports with the same stem give `<stem>.mid` and `<stem>_001.mid`;
  - the 17th open is LIMIT;
  - 4 MiB + 1 is LIMIT and FAILED with no file left;
  - the card gate gives NO_CARD on a card that may not be written (no card, write protected, exFAT).
- **The 1.6 layout literal**, and a 1.6 app still loads on 1.7 (its io table is shorter: no export members; its `o3m_instrument_info` is 16 bytes and gets 16).
- `instrument_info` of a MIDI instrument gives its channel, and of a sample or a module gives 0.

## 3. SMF EXPORT (`modules/apps/smfexport/`, Level B)

- **Caps:** `read_project` + `export_files` only. It declares **ABI 1.7**. It opens from Pattern F7 TOOLS, or from the app list (no selection: the whole pattern in view).
- **What it writes:** a Standard MIDI File, **type 1, PPQ 96**, one row = 24 ticks.
  - **MTrk 0:** the tempo meta (`get_tempo`), time signature 4/4, and the end of track.
  - **One MTrk per non-empty track** of the selection (or of the pattern), in track order. The track name meta is "Track N", 1-based. No new read entry, so no names.
- **Range:** the selection's rows and tracks from the launch context (`launch_ctx`: block_tracks, block_length). Otherwise rows 0..`track_length[t]-1` of each track (`pattern_info`).
- **Channel:**
  - a MIDI instrument's own channel (`instrument_info`, `kind` MIDI, the new `midi_channel`, §1);
  - otherwise (track number mod 16) + 1.
- **Notes:**
  - note-on at `row * 24 + offset`. `offset` is the Micro-move FX 2 as the sequencer plays it: `seq::fx::micromove_start(value)` is in 48ths of a step (fx.h:264), so the offset in ticks is `micromove_start(v) / 2`. Copy the formula into the app with a comment naming fx.h:264, since an app cannot include the core;
  - velocity: the step's volume column as the sequencer turns it into a velocity, else the sequencer's default; 1..127;
  - a note ends at the next note or Off on the same track, or at the end of the range. A Cut or a Fade step ends it like an Off (v1 does not tell the three apart: a Fade is not a gradual release in the file; as built, OS-H4).
- **Not in v1:**
  - Chord FX 14 is not expanded;
  - the other FX are not exported.
  The app counts each skipped FX by number and shows the counts in its report.
- **Writing:** the app builds each MTrk's length in a first pass and writes in a second; there is no seek. It feeds export_write in chunks of at most 1 KiB, retries BUSY on later frames, then closes and polls `export_status`.
- **The screen:** EXPORTING…, then DONE with the file name (`AppExport/SMFEXPORT/<project>_P<n>.mid`, or the stem the app chooses; the engineer picks and documents it), notes written, and FX skipped by number. FAILED shows the code.
- **Host test:**
  - a fixture pattern exported, then parsed back by a small SMF reader in the test: header, track count, tempo, every note's tick, channel, velocity and length;
  - a micro-move row;
  - a MIDI instrument's channel;
  - a selection.
- **Size:** report the `.tmod` size and the core flash delta, and check that it fits the module slab.

## 4. Docs

- `docs/guides/app-modules.md`: the four calls, the codes, the allowlist, the caps, BUSY handling, the ring-size rule, the folder, and the confirm.
- `module/sdk/README` (or wherever the SDK lists the ABI versions): ABI 1.7.
- QA catalogue: rows in `modules.md` for the confirm, TOOLS listing an export app, SMF EXPORT of a pattern and of a selection, and a file appearing in `AppExport/SMFEXPORT/` on the card. Use the next free QA-MOD ids on the base. Recount the README.
- An HIL file `platform/mk66/tools/hil/qa_h4.json`: export, then check the file on the card. Validate it with `python3 -I platform/mk66/tools/o3hil.py validate`.

## 5. Acceptance

- Full CI (host, qemu, mk66) green. Flash delta reported. SDRAM_STATIC delta reported (the ring, 16 KiB expected).
- **Independent review (Level C):** the guard table, the abort paths, and that no app path can create, replace, rename or remove anything outside `AppExport/<app>/` or anything already DONE.
- **Unit check by the tester on a card:** export a pattern, then open the `.mid` on the computer; a second export makes `_001`; a pulled card fails without a stray final file.
- **Not in H4:** reading `AppExport` (C2), whole-song SMF export (IDEA OS-SMFSONG), and Chord expansion.
