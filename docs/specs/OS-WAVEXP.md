# OS-WAVEXP: WAV export of the song, a pattern, stems and a selection

BACKLOG #10, approved by the maintainer on 2026-10-14. Written on 2026-10-14. Line refs: version 0.4.18 (bare `:n` = `app/src/bindings.cpp`).

Parity:
- **Documented in the official manual** (§8.8; documented behaviour, not verified parity): the export types song, song stems, pattern, pattern stems, and the render of a selection (§4.3).
- **Open research question (Q)**: whether the official firmware writes 16-bit stereo at 44,100 Hz, and its folder and name patterns (the manual map, `docs/ui/official-manual-map.md`, rows "Export folders and names", lists ours as different).
- **Documented in the release notes** (not verified):
  - PAR-054/069: stems aligned to the start;
  - PAR-139: long tails end;
  - PAR-151: a selection cuts sharply, while song and pattern keep their tails.
- **New Community OS behaviour**: the home of the keys, the progress overlay, auto-numbered selection names (no keyboard), and the stem set (12 tracks and two buses).

No persistent format change.

## 1. Two slices

| Slice | What | Level |
|---|---|---|
| **A** | Review the engine handover (the live render parks: `audio/engine.h:107-131`); File EXPORT page for SONG and PATTERN; progress overlay and cancel; Pattern F7 TOOLS with EXPORT | **C** (the real-time render path), size M |
| **B** | `OfflineRequest` grows a track mask, dry sends, bus-only and a row range; SONG STEMS, PATT STEMS, and the selection export from TOOLS | **C**, size M |

A is useful alone, and B adds only rows and an argument.

## 2. What the engine already does (branch, kept)

- **What it renders:** the live path offline (`seq/offline.h:8-11`): voices, the mixer as it stands, the send effects, the limiter, and the project tempo, swing and FX.
  - Not included: line in, USB return, live notes, MIDI out (PAR-063 holds by construction) and the external clock.
- **Output:** a 44,100 Hz stereo 16-bit WAV.
  - The rate goes from 44,117.647 Hz to 44,100 Hz through the cubic `RateConverter` (`app/export_source.h:38-45`). It is the identity under `O3OS_RATE_44100`, which is ON by default since v1.0.0 (OS-RATE): with the default build the export is not resampled; the converter only runs for the OFF build (44,117.647 Hz).
  - It takes two passes (measure, then write).
  - Tail limits: `kHoldBlocks` 3500, `kMaxTailBlocks` 20700, and at most 100 minutes (`:62-65`).
- **Saving:** `SaveService::save_export / cancel_export / export_name` (`save_service.h:179-185`).
  - Files are `Export/<Project>/<Project>_Song0001.wav` and `<Project>_P<n>_0001.wav`, never replaced (`NewOnly`, `save_guard.h:41`), with an `Untitled` fallback.
  - Memory: 32 KB of SDRAM staging plus about 5 KB.

**Fixed, no options.** 44,100 Hz, 16-bit and stereo are this OS's choices (whether they are the official firmware's is Q, see Parity). A 24-bit or a mono option would be an IDEA, not this item.

## 3. Homes

### 3.1 File: an EXPORT page (slice A, rows added in B)

- **F7 `EXPORT` on the PROJECTS page opens the EXPORT page**, and on the EXPORT page F7 reads `PROJECTS` and goes back. F7 is the free key there, so no cycle is needed (UI map rule 1). F6 stays the IMPORT ↔ PROJECTS toggle (OS-FILES part A).
- OS-FILES part B places RENAME on F8 of the PROJECTS page and DELETE on the Delete key (rule 6: Delete clears the thing under the cursor).
- **Rows** (Up/Down choose; the encoder changes the pattern number on the pattern rows, existing patterns only; Shift+Up/Down page, as OS-NAV3 says):
  | Row | Slice | Renders |
  |---|---|---|
  | `SONG` | A | the arrangement, with its tail |
  | `PATTERN nn` | A | pattern `nn` once, with its tail; `nn` starts at the Pattern screen's pattern |
  | `SONG STEMS` | B | see §4 |
  | `PATT STEMS nn` | B | see §4 |
  The right-hand card shows the file or folder that `export_name()` will create.
- **Keys on the page:**
  - F1 `EXPORT` starts the chosen row.
  - F6 `IMPORT` goes to the import page.
  - F7 `PROJECTS` goes back.
  - F4 SAVE and F5 SAVE AS stay as they are on the other pages.
  - F2, F3 and F8 are blank.
- The Shift row keeps the quarter jumps (OS-SHIFTROW).

### 3.2 Pattern: F7 TOOLS (maintainer, 2026-10-14: "F7 = TOOLS, a list of block operations with EXPORT first")

- **F7 opens the TOOLS list**, a list overlay with the same grammar as the module picker. It sits on `:327`, the reserved F7.
  - Up/Down choose and F1 runs the entry; F8 is BACK.
  - The list cannot open while the view follows the play line (LIVE, or WATCH while playing). It is refused silently, like block selection (OS-RECMODE2 §2).
- **Entries:**
  1. `EXPORT PATTERN`, always first: it renders the current pattern.
  2. `EXPORT SELECTION` (slice B), shown only while a block is active: it renders the selection (§4.3). Two entries, so exporting the whole pattern never needs the block cleared.
  3. The tool apps (ADR 0014 H3a), listed below a divider.
- **Whichever of OS-WAVEXP A and H3a lands first builds this overlay.** The contract above stays the same either way.

## 4. Stems and the selection (slice B)

### 4.1 The request

`OfflineRequest` (`audio/offline.h`) grows these fields, all defaulting to today's behaviour:
- `track_mask` (16 bits);
- `dry_sends` (the masked-in tracks feed no send);
- `buses` (which send returns sound: delay, reverb);
- `row_first` / `row_last` (pattern only);
- `trim_tail` (end at the first silence after the last row, with no hold).

The mute path is the graph's own. What the official firmware's export solo does is an open research question; this OS's solo is graph.h "Solo".

### 4.2 Stems

- One pass per stem. This is slow but writes only one file at a time; the overlay shows `STEM n/m`.
- The stems are:
  - `T01`..`T12` (to `T16` if more tracks are active): one track, post-fader, with its sends off and every return silent. A track with no note in the range is skipped.
  - `DELAY` and `REVERB`: that return only, fed by every unmuted track. A bus whose send level is 0 on every track is skipped.
  - `MIX`: the normal render.
- **Every stem starts at the arrangement start** (PAR-069) and is cut to the length of the `MIX` pass, so a DAW lines them up.
- **The master chain (limiter) stays in for every stem.** A stem loud enough to drive the limiter alone is limited alone, so the stems sum to the mix only while the limiter is idle. This keeps the engine change small; note it in the user doc.
- **Folder:** `Export/<Project>/<Project>_Song_S0001/` or `<Project>_P<n>_S0001/`. Files inside are `<Project>_T01.wav` … `<Project>_MIX.wav`.

### 4.3 Selection

TOOLS → `EXPORT SELECTION` (listed only while a Pattern block is active):
- **What sounds:** the block's tracks only, from its first row to its last.
- **Cut:** a sharp start and `trim_tail` (PAR-151).
- **File:** `Export/<Project>/Selection/<Project>_SEL0001.wav` (this OS's export folder; the name is auto-numbered, because no keyboard name is needed when nothing is ever overwritten).

## 5. Progress, cancel and errors (slice A)

- **Starting an export stops the transport** (the live engine parks for the render). The transport stays stopped when the export ends.
- **The export is modal.** It renders the live model, so nothing may change during it.
  - A progress overlay (one, shared by both homes) shows the name, a bar from `progress()` (0..1000, `audio/offline.h`), `STEM n/m` in slice B, and F8 `CANCEL`.
  - Screen buttons, pads, transport and every other key do nothing under it (`overlay.h:17-18`). Play shows `EXPORTING` in the strip.
- **Cancel:** `cancel_export`. The file being written is not left on the card; the engineer confirms that `NewOnly` with the atomic save leaves nothing.
  - In slice B, stems already finished stay. The strip says `CANCELLED: 3 OF 9 STEMS SAVED`.
- **End:**
  - The strip says `SAVED EXPORT/<PROJECT>/<NAME>.WAV`, or the folder for stems.
  - The overlay closes and live audio resumes with no click; the review checks the handover.
- **Failure:** the strip says `EXPORT FAILED: <failure()>`. Possible reasons: a held note, the recorder armed, an empty project, the card full, no card.
- **Gate:** the same card gate as SAVE (a mounted, writable FAT card; no marker file since OS-ANYCARD).

## 6. Cost

- Flash and SDRAM: the engineer reports both after the rebase.
- SRAM: none expected.
- Live CPU: 0 (the live render is parked during an export).
- Speed: report the time a 1-minute pattern takes on the unit, and the time of its stems.

## 7. Acceptance

**Slice A:**
- The engine branch's tests pass after the rebase: `export_wav_test`, `seq_offline_test`, `audio_offline_test`, `audio_offline_thread_test` (TSan) and `save_guard_test`.
- New `file_export_test`:
  - F7 EXPORT / PROJECTS and F6 IMPORT on the EXPORT page, with their labels;
  - the rows and the encoder on PATTERN;
  - F1 starts;
  - the overlay captures every key but F8;
  - cancel leaves no file;
  - the strip texts.
- New `pattern_tools_test`:
  - F7 opens TOOLS;
  - it is refused while following;
  - EXPORT PATTERN renders the pattern;
  - starting an export stops the transport.
- Gallery: `file_export`, `export_progress`, `pattern_tools`.
- QA rows: QA-FIL (EXPORT page, cancel, failure), QA-PAT (TOOLS).
- HIL `qa_wavexp.json`: export a short pattern on the test card. The strip shows SAVED and the play line resumes afterwards.
- Maintainer check: open the WAV on the computer; it plays at the right pitch and speed. Then export the same pattern again and get `_0002`.

**Slice B:**
- Host tests for each mask:
  - a soloed dry track equals the live render of that track with its sends off;
  - bus-only stems;
  - stem lengths are equal;
  - silent tracks are skipped;
  - the selection is cut at the first and last row;
  - the selection tail is trimmed.
- QA rows: QA-FIL (stems), QA-PAT (EXPORT SELECTION).
- HIL: pattern stems of the test song. The folder holds `T..`, buses and `MIX`, all the same length.

**Docs:**
- UI map §1, File and Pattern rows.
- user guide export section: the stems and the limiter.
- BACKLOG #10 DONE after B.

## 8. Determinism: an export of an unchanged project is byte-identical (new Community OS behaviour)

Two exports of the same project, with nothing changed between them, are the same file, byte for byte, whatever the engine did before (a live Play, an earlier export, any time at rest). QA-PRF-128 and every "X never reaches an export" check rely on it: the method is two exports and `cmp`. The rule is held at the start of every pass of the render (`seq::OfflineRender::start_pass`, through `Engine::offline_quiet`) and costs nothing per block; the live render is untouched.

- **Sources found (int.15, the OG project t-v16, two song exports differed at -28.8 dB rms) and fixed:**
  - The send reverb's modulation: `SendReverb::begin_clear` (the `ReverbClear` the pass pushes) returned early for a plate that was asleep, so the two LFOs that modulate the tank's allpasses (`lfo_phase_`, `lfo_m_`) woke from wherever the last tail left them. It now configures the plate and puts the LFOs at their beginning whether the plate sleeps or not (the state is the one after `attach()`).
  - The random LFO shape (filter, pan, finetune, amp, step FX): the voice pool's one xorshift32 (`VoicePool::rng_`) carried on from the last draw. `VoicePool::seed_random()` puts it back to its seed at the start of each pass (`Engine::offline_quiet`, only while the export has the engine). The live render still draws from wherever the generator is.
  - The Amiga engine (OS-SMALLFIX12): `AmigaVoices::release_track` only fades a voice, and the voices' Paula channels and filters, the MED track state (`tracks_`), the song's LED switch (`song_led_`, 0F F8 / F9) and the tick count were never reset between passes, so an export after a live Play that had switched the LED filter on (or a song that had left a slide held) was coloured by it. `AmigaVoices::reset_pass()` puts all of it back to what the constructor made at the start of each pass (`Engine::offline_quiet`, only while the export has the engine); the model, the LED setting and the slide flag are the app's and stay. The live render is not touched.
- **Already deterministic, held by the test:** the sequencer (`Sequencer::Config::seed`, applied at every transport start; the export's sequencer is built afresh per pass), the delay's ring and state, the limiter (flushed), the voices (cut), the tracker FX processor (reset), the strips (the Perform layer is put away).
- **Not in the rule:**
  - A module instrument keeps its instance's own memory (the TB-303's oscillator phase, say) because the module ABI has no reset call (`seq/offline.h`, A PASS). An export of a project that plays a module instrument can differ in that voice; closing it needs an ABI amendment (not done here). So the `cmp` check holds for a project without module instruments.
  - **Latent (recorded, not fixed):** `Engine::offline_leave` (`audio/src/engine.cpp`) only fades the voices, so after an export the live Amiga render keeps the export's last state (`song_led_`, `tracks_`) and the live state from before the export is lost (`offline_leave` clears `offline_entered_` before `offline_quiet()`, so `reset_pass()` is not called there). It does not affect export determinism: every pass of an export starts with `reset_pass()`.
  - **Latent (recorded, not fixed):** `OfflineRender::push_clears` ignores the result of `ring.push` (`seq/src/offline.cpp`), so if the ParamChange ring were full the `DelayClear`, `ReverbClear` or `VoiceSendsReset` of that pass would be dropped silently and the pass would start with a tail. The ring has 256 slots and the engine drains it every block, so it is not reachable today; `push_mixer` shows the way to check (all or nothing) if it ever matters.
- Test: `tests/seq_export_determinism_test.cpp` (three exports in a row through the unit's export path; the random LFO shapes and Chance / Random note FX; an export after a live Play; a Pattern request, whole and as a selection; a project on the Amiga engine, also after a live Play that left the LED filter on). Every file is also checked to have sound in it (peak and non-zero samples), so a silent render cannot pass. Before the fixes, the Amiga live-Play case and the random / LFO cases failed; the Pattern and energy cases guard the path. The Amiga live-Play case asserts `AmigaVoices::song_led()` before the second export, so it cannot pass with the LED filter never engaged. Unit check still open: export the OG project t-v16 twice and `cmp` the files.

## Built

### Slice A (from version 0.4.19)

**Done:**
- The engine work was rebased onto version 0.4.19. One conflict, in `audio/engine.h` (the members next to COREFX's `tfx_`: both kept). Everything else merged cleanly and was then reviewed against today's engine (commit `fix(audio): the offline engine against today's engine`):
  - **Real-time path untouched.** The park check does not live in `render_block` any more: `Engine::trampoline` (the BlockCallback's function) is in flash and looks at `want_`; `.ramfunc_l` is byte for byte the baseline's (27,512 B). The live notes' queue is not read by the export because the main loop owns `live_` (null) for the block, the render being parked; `play_block()` is the baseline's.
  - The apps' tap (H2a), the scope and the spectrum are put away for the export; the tracker FX processor (COREFX) is reset on entry, between passes and on leave (a cancel leaves no channel volume or pan on the strips); `MasterLimiter::flush` (controls kept) so no residue of an export is in the first live block and every pass starts alike; ParamChanges the graph does not own go to the export's sequencer and to the live one (parked, it would never see them).
  - The S2b live queue and the INSTUI voices need nothing: the live queue is not consumed (and on the unit `LivePreview::push` refuses a note while the engine is the export's), and the export's bare `Sequencer` has no MIDI link (MIDI instruments are silent in a file, PAR-063).
- **UI.** File F7 = EXPORT on the PROJECTS page, the EXPORT page (field context `file.export`: F1 EXPORT, F6 IMPORT, F7 PROJECTS, F2 / F3 / F8 blank, F4 / F5 as on the other pages), rows SONG and PATTERN nn (Up / Down; the encoder over the patterns that exist; nn starts at the Pattern screen's pattern), the file name F1 will make. The progress overlay (`ExportProgress`, context `exportprogress`): the file, `PASS 1 OF 2` / `2 OF 2`, a bar, F8 CANCEL; it captures every other key; the strip's badge says EXPORTING. The end: `SAVED EXPORT/<PROJECT>/<NAME>.WAV`, `EXPORT CANCELLED`, `EXPORT FAILED: <why>`. The export is armed, the transport is stopped, and it starts when the published position says it has.
- **Pattern F7 TOOLS / EXPORT PATTERN: the action `export.pattern` is built and tested (it exports the Pattern screen's pattern from any screen); OS-H3A adds it as the first TOOLS entry when both are merged, nothing here builds TOOLS.**
  - **EXPORT PATTERN is the first TOOLS entry (on 0.4.20 with both slices merged):** `kToolsFixed` is 1 (`tools_list.h`: `Kind::ExportPattern`, the label, the divider above the first app); `tools_run` closes the list and runs `export_pattern`, so the progress overlay and the refusals (no card, a save running) are File EXPORT's. The fixed entry needs no module system: F7 opens the list with only it (a dim `NO MODULE SYSTEM` / `NO TOOL APPS ON THE CARD` line under a rule); `NO PROJECT` still refuses it. The cursor opens on EXPORT PATTERN, so an app is one Down away. QA-PAT-180; golden `qa_pat_180`.
- Memory: the export's staging ring (32 KiB), its event buffer, its own sequencer and the renderer sit in the same SDRAM block as the save layer (`ExportHost`, about 44.2 KB: a PERMANENT static carve of the sample arena's tail, `kArenaSamples` is that much smaller for the life of the image, export or not); nothing in SRAM but the engine's 48 B and the sequencer's 8 B of members.

**Deviations from the spec:**
- The bar is the export source's own account over both passes (`ExportSource::progress_permille`), not `OfflineRenderer::progress()` alone; the overlay says which pass.
- `SAVED EXPORT/...` is cut from the left to the strip's 48 characters when the project's name is long.
- The card of the file that F1 makes is under the rows, not at their right (the rows are two).
- While the transport is being stopped the overlay says `STOPPING THE TRANSPORT`; F8 there cancels before anything is opened.
- Numbers chosen to leave room for the other branches: `Context::FileExport` 44, `Context::ExportProgress` 45, `OverlayId::ExportProgress` 20.
- While an export runs the MIDI link is not stepped (no external clock is followed, nothing goes to MIDI out), new live pad / MIDI notes are dropped, (a release is not dropped, but it finds no note: the notes held when the export began were stopped at its start, see OS-EXPNOTE below) and the SD modules' CPU governor is off. When the engine is given back the MIDI link forgets what the interrupts stamped meanwhile and starts its clock loop over (`EventSource::resume`), so no stale Start / Continue / SPP is applied.
- A cancelled or failed export leaves an EMPTY `Export/<project>` folder when it made it: the card writer never removes a folder (`hal/save_guard.h`, a hard rule of the guard), so removing it needs a decision of the maintainer; the next export into it reuses it. An `_o3save.tmp` a crash left in the folder is removed by the next save into it (AtomicSave's Probe stage).

**Not done / for the unit:** the time a 1-minute pattern takes (the slice is 8 ms of the main loop's pass, two passes), a real card write, listening to the file on a PC, the card pulled in the middle, the MIDI clock-in follower after an export. Slice B (stems, selection) is not started.

### Slice B (from version 0.4.20)

**Done:**
- **The request** (`audio/offline.h`): `OfflineRequest` grows `track_mask`, `dry_sends`, `buses`, `bus_only`, `row_first` / `row_last` and `trim_tail`, all defaulting to the slice A render (`masked()` is false, the engine is not asked for anything).
- **The mixer is told, not changed.** `seq/offline.cpp` `push_mixer`: a masked request pushes the 16 tracks' MUTE and SOLO and the master's DRY, DELAY and REVERB mutes as ParamChanges with the settle (the graph, its render path and `.ramfunc_l` are untouched), and `close()` pushes the project's own values back. A track mask unmutes the masked-in tracks and mutes the others whatever the project says, clears every solo (a stem is the track alone), and forces DRY MUTE off; `dry_sends` shuts the delay and the reverb (this OS's model of an export solo, graph.h "Solo": the soloed track's sends off, and here every return silent; what the official firmware's export solo does is an open research question); `bus_only` is DRY MUTE and a clear bit of `buses` shuts that effect. A ring that cannot take the 35 changes fails the pass (`THE MIXER IS BUSY`).
- **Rows** (selection): `Sequencer::Config::pattern_stop` (the transport stops at the start of that step: `row_last + 1`, to the sample, through the same `end_run` as the end of a pattern) and `play_pattern(pattern, row_first)`. `OfflineRender::open` refuses `NO SUCH ROWS`; a last row past the pattern is the pattern's end.
- **Stem pass model.** `app/export_stems.*` plans the set from the project (pure): `T01`..`T16` for each audio track with a note in the range (a MIDI track or instrument, a track past the active ones and a note off alone do not count; a muted track keeps its stem but does not feed the buses), `DELAY` / `REVERB` when the bus can sound (not muted, master level above 0, an instrument played by a heard track sends to it; a module instrument is assumed to), `MIX` last. A set is ONE SAVE JOB PER FILE (`SaveService` `StemRun`, like the project's plan: it keeps the card it began on, `launch` pins it). The first file's job measures the plain MIX (`ExportSource::set_stem`: measure request, write request), every other file is told that length and only writes (`known_frames`), so a set of M files is M + 1 passes, every file the same length to the frame (PAR-069), and the bar of every job is its share of the whole (`unit_base` of `units`; it never falls, also between two files). The engine is handed over for each file (enter / leave per job): the governor, the MIX tap, the MIDI resync and the limiter flush of slice A run for every file (tests: 8 cycles of enter / leave; the governor test now runs 6 passes).
- **Cancel and failure.** F8 stops the set: the file in progress is not left (the atomic save's own cleanup), the files already finished stay, no file starts after the cancel (also when the cancel arrives as a file ends), the strip says `CANCELLED: n OF m STEMS SAVED`. A failure (card full, a render that fails) says `EXPORT FAILED: <why> (n OF m)` and also ends the set there. A set that ends well says `SAVED n STEMS EXPORT/<PROJECT>/<set>_*.WAV`.
- **EXPORT SELECTION** (`tools_list.h`, `tools_actions.cpp`, `export_actions.cpp` `export_selection`): the second fixed entry of Pattern F7 TOOLS, listed only while `edit.selection.active` (`tools_fixed(block)`; `tools_count` / `tools_item` take `block`, default false, so slice A's callers are unchanged). It exports the block's tracks (its columns, as `track_mask`) from its first row to its last (cut to the pattern), `trim_tail` on: the file ends at the first silent block after the last row (no hold). `Export/<Project>/<Project>_SEL0001.wav`.
- **UI.** EXPORT page rows SONG STEMS and PATT STEMS nn; the overlay says `FILE n OF m   writing T03` (the plain overlay is unchanged); `export.selection` action.
- Memory: `StemRun` (about 540 B) in `SaveService` (SDRAM, with the save layer's carve), 11 B more in `ExportView`; nothing in `.ramfunc_l`.

**Same sound.** `seq_offline_test`: a track stem is, bit for bit, the live render of that track soloed with the delay and reverb shut, for four tracks; the stems of a pattern (four tracks, DELAY, REVERB) sum to the MIX with a worst error of 0 LSB while the limiter is idle (2 LSB is the asserted tolerance, for the six stems rounding on their own; a mix the limiter works on is not the sum of stems limited one by one); a selection is the live Play from its first row, bit for bit, up to the block its last row ends in.

**Deviations from the spec:**
- **No sub-folders.** The spec (sections 3.2 and 4.2) puts a set in `Export/<Project>/<Project>_Song_S0001/` and a selection in `Export/<Project>/Selection/`. The save guard (`hal/save_guard.h`) takes only `Export/<name>/<stem>.wav` (three components) and the brief was not to widen it, so the files are flat in `Export/<Project>/`: `<Project>_Song_S0001_T01.wav` (or `_P<n>_S0001_`), `_DELAY`, `_REVERB`, `_MIX`, and `<Project>_SEL0001.wav`. The project part is cut to 13 characters for a set (the guard's 31 for the longest name, `..._P255_S0001_REVERB`). The set number is the first none of the 19 possible names of a set uses. If the maintainer wants the folders, the guard needs a fourth component (Export/<name>/<set>/<stem>.wav, make_dir for the set's folder) and its tests: a decision of the maintainer (a hard-rule file); the code here changes in `stem_names` only.
- The progress of a set counts passes (M + 1), not bytes.
- A muted track keeps its T stem (a stem is the track alone) and a solo is cleared for a stem; the selection ignores the project's mutes and solos on the tracks it names, and keeps the delay and reverb of those tracks.
- Between two files the live engine runs for a moment (the transport is stopped, no live note): the mixer is the project's then, not a stem's.
- **Known limit, the gap between files.** Because the engine is given back between two files, a MIDI-clock Start that makes the live sequencer play makes the next file's open() refuse (`STOP THE TRANSPORT FIRST`; a pad or MIDI note held in that moment is stopped, OS-EXPNOTE) and the set ends as `EXPORT FAILED: <why> (n OF m)`, the finished files staying. Keeping the set going would mean keeping the engine parked across the files (the mixer masked meanwhile, and every end of a set releasing it): a change of its own, not a one-liner.
- **Known limit, every stem is cut to the plain MIX's length.** The length is the MIX's (measured once, first); a stem is cut hard there. A tail that rings longer than the MIX's (the MIX ends at its first long silence, or at the 30 s cap) is cut, and a muted or excluded track's tail, which the MIX does not carry, is cut at the MIX's end too. Nothing is appended after it.

**Review fold.** The mixer restore is all or nothing: `push_mixer` checks the ParamChange ring for room for all 35 changes first. When `close()` finds none, the restore stays owed (`OfflineRender::restore_owed`) and `service()` (OfflineRenderer, called by `SaveService::poll` every pass) pushes it when there is room, so it cannot be lost. `midi.cc` is ignored while an export runs (`ViewState::xport.running`, also between two files): a learned CC on a mute, solo or DRY MUTE must not change the project and the ring under a render.

**Not done / for the unit:** the time of a pattern's stems (a set is M + 1 passes of the main loop's 8 ms slices), a real card (a full card in the middle of a set, a pulled card), listening to the files on a PC and summing them in a DAW, the live playback and the mixer after a set, the MIDI clock-in follower after many files. The BACKLOG #10 row and a user guide section on stems and the limiter are not written here.
- **OS-EXPNOTE (new Community OS behaviour, maintainer decision 2026-10-16; Level B).** A held live note no longer refuses the export (it used to say `A NOTE IS HELD`, and a MIDI note-on that never got its note-off, seen on the unit from the computer on channel 10, blocked every export). When the export takes the engine (`Engine::offline_enter`, the render parked, main loop) the voices are cut as before (`offline_quiet`), the table of held live notes forgets its notes (`LiveNotes::end_all`), the live source ends what it holds outside the table (`EventSource::stop_live`: the MIDI link releases the pads that play a MIDI instrument and sends their note-offs at once, since the link is not stepped during the export), and the queue is emptied of what was pushed before the export (a note-on the render never took does not start after it; it is counted and, if nothing else is named, named); the oldest held note and the count are kept in the `LiveBox` and become the strip line `STOPPED PAD C-5`, `STOPPED MIDI CH10 C-4`, with `+n` for the others (at most 25 characters: it fits the 48 of the strip), kept while the export runs (`OfflineRenderer::stopped`, `App::poll_export`) and posted right after the result line when it ends (`App::finish_export`: the progress overlay is modal, so the strip cannot show it earlier). Note names are the tracker's (note 0 = C-0, so MIDI 60 is C-5; the tenth octave as the note tile: `C10`, sharps `c10`). A late note-off, or a pad release, for a stopped note finds no note and does nothing; the releases queued during the export are played when the engine is given back and find no note, and a note pressed during the export is dropped by the platform (as before), so nothing is stuck. **Known limit:** in a set of stems the strip tells what the FIRST file stopped; notes held in the gap between two files are stopped too but not reported again. The render path change is one: `LiveNotes::begin` also stores the event's note and MIDI channel (two byte stores per live note-on; the table grows 50 to 74 bytes, SDRAM). `PreviewSink::midi_note` gained the channel.
