# MIDI event list editor — radar card
Maintainer's words: "un editor estilo lista de eventos para cada track midi?" (from: "que en los instrumentos midi podamos grabar multiples notas y cc's, y un editor estilo lista de eventos para cada track midi?") · 2026-10-21 · Status: idea

Related cards: `midi-poly-record` and `cc-automation` (they create the data this edits). Backlog row: OS-MIDIREC (part c).

## What it is
A list view of one MIDI track: one row per event with TIME, TYPE (note, CC, pitch bend, aftertouch, program), CHANNEL and DATA (note and velocity, or controller and value). You can insert, delete, edit a field, filter by type and quantise a selection, as in a DAW list editor or an old hardware sequencer's list page.

## Prior art
| Project | URL | Licence | Language | Last activity (GitHub API, read 2026-10-21) |
|---|---|---|---|---|
| Seq66: a separate "event editor" for detailed inspection and editing of events, apart from the pattern (piano roll) editor; SMF import and export | https://github.com/ahlstromcj/seq66 | GPL-3 (`licenses/LICENSE.GPL` is the v3 text; GitHub reports no licence) | C++ | pushed 2026-10-21 |
| OpenMPT: pattern cells hold notes, plugin parameter control events and macros; double-click to edit an event cell (a tracker's list is the pattern itself) | https://github.com/OpenMPT/openmpt , https://wiki.openmpt.org/OpenMPT_Wiki:Books/Manual | BSD-3-Clause | C++ | pushed 2026-10-09 |
| MilkyTracker: tracker pattern as the event view | https://github.com/milkytracker/MilkyTracker | GPL (COPYING holds the v3 text; GitHub reports NOASSERTION) | C++ | pushed 2026-09-10 |
| LMMS: automation clip editor with points on a tick grid and a progression type | https://github.com/LMMS/lmms | GPL-2.0 (GitHub field) | C++ | pushed 2026-10-19 |
| Ardour: automation lane and event editing | https://manual.ardour.org/mixing/automation/automation-modes/ | GPL (version not verified here) | C++ | pushed 2026-10-20 |
| Synthstrom Deluge firmware: an automation view and a step-edit for each parameter (a grid of pads rather than a list) | https://github.com/SynthstromAudible/DelugeFirmware | GPL-3.0 (GitHub field) | C++ | pushed 2026-10-21 |
| Sagan Metro: a "Thin Continuous" dialog on selected events; an "Event Information" page exists (title only, content not read) | https://sagantech.com/support/Met00421.htm , https://sagantech.com/support/Met00180.htm | proprietary, concept only | n/a | n/a |

Not found: a small-MCU open-source sequencer with a documented list editor. The Standard MIDI File structure (delta time, status, data) is the natural import and export format for the list; the MIDI 1.0 message layout is at https://www.lim.di.unimi.it/IEEE/MIDI/SOT5.HTM .

## Approach
inspire; implement from scratch on the FT812 UI, no port.
- Phase 0, no format change, no lane: a READ-ONLY (or edit-through) list view of the existing step data of a track: row = step with a note, with FX CC A..F and Micro-move shown as events (TIME = step and microstep, TYPE = NOTE or CC, DATA from the FX byte). Edits go through the existing model mutators and undo, so the project file and the OG `.mt` export are unchanged. It is a view onto data that exists.
- Phase 1: the same screen over a lane (`midi-poly-record`, `cc-automation`): rows are lane events sorted by tick.
- Quantise: integer, `t' = t + (nearest grid point - t) x strength / 100` with a grid of 192 PPQN ticks (1/16 = 48 ticks, 1/32 = 24, 1/64 = 12) and a selection; one division per event, ~50 cycles *estimate*.
- Insert keeps the list sorted; edit of TIME re-sorts that row; ties keep a stable order (note-off before note-on at the same tick, as the router orders them).

## Cost on MK66 (estimate until measured)
CPU: UI only, in the main loop; nothing in the render.
- Draw: 16 visible rows (800 x 480, a row about 24 px with the header and keys, *estimate*) x 6 fields = 96 text items per frame; formatting one field ~300 cycles = 29 k cycles a frame; at 60 frames/s = 1.7 M cycles/s = 1 % of 180 MHz *estimate*, only while the screen is open and redrawn only when something changes (redraw only on change is the OS's rule for the pattern screen, `docs/specs/OS-009-pattern-editing.md`, PAR-104). The FT812 does the drawing.
- Edit: insert or delete moves up to n x 8 bytes: 1,000 events = 8 KB at ~1 cycle a byte = 8,000 cycles = 44 us. Quantise of 1,000 events: 1,000 x 50 = 50,000 cycles = 0.28 ms plus a sort (insertion sort on a nearly sorted list ~ 10 cycles an event). All main loop, so no audio cost, and a block (1.45 ms) of the render is not touched.
- Undo: one snapshot of the lane per edit (a lane of 1,000 events = 8 KB; the pattern undo area is 160 KiB, `model/include/o3os/model/undo.h` `kUndoPatternBytes`, so about 20 such snapshots; a smaller delta-based journal for single-event edits is better).
Memory: no new storage beyond the lanes (see the other cards); the view keeps a 16-row window, a selection range and a filter mask: ~64 bytes SRAM. Flash: the screen and its actions about 6 to 10 KB *estimate* (the largest part of the three ideas), plus rows in the QA catalogue. Lives in: core flash as an `app/` screen (it needs the model's lane, undo and the screen host). An SD `.tmod` app could do it only through a new module ABI minor for lane access (a published ABI is Level C) and the app has to work through the main-loop edit hooks; not for phase 1.

## Fits
UI (`app/`, `ui/`: a screen with the FT812 text and rows, F-key row as the Fill overlay uses F1..F8), model (`model/` lane mutators with undo; step view in phase 0), sequencer (read-only: the player follows the edited list through the pattern generation check as `rows.h` does for steps), formats (none in phase 0; the lane chunk in phase 1), actions (`core/` actions for insert, delete, set field, quantise, so they are undoable and scriptable). Depends on: the lane structure for phase 1; nothing for phase 0.

## UX sketch
From a MIDI track in the Pattern screen: a key opens EVENTS. Encoder scrolls (rows follow the playhead when Follow is on); Left/Right move between TIME, TYPE, CH, D1, D2, LEN and the encoder changes the field; F-keys: INS, DEL, QUANT (grid and strength, as the Fill overlay's fields), FILTER (notes / CC / bend), COPY. Pads audition the note of the row. A header shows the track, the event count and the thinning density if CC lanes are present.

## Licence
The OS is GPL-3.0-or-later. Nothing to port: a list editor is a standard idea. Do not copy code from the GPL projects above (GPL-2.0-only code is not combinable with GPL-3.0-or-later; GPL-3.0-only code would fix the combined licence). OpenMPT (BSD-3-Clause) could be ported with attribution but is a tracker-grid design, not a list. Closed products: concepts only.

## Risks / open questions
- **Project format change = Level C** for phase 1 (the lane chunk: version bump, backward-compatible loader, round-trip test; maintainer decides). Phase 0 changes no format.
- **OG `.mt` export loses the lane events** (the OG pattern format carries steps only, `docs/formats/og.md`, not re-read for this card); the bake option (lane to step FX, lossy) is described in `cc-automation`. The OG `.mt` keeps exactly its format.
- The DIN bandwidth does not affect the editor, but a list that can hold 1,000 CC events invites playing them: see the rate cap in `cc-automation`.
- Note rows: show a note as one row with a length (start and LEN) or as two rows (on and off)? One row is shorter; two rows is the classic SMF view. Undecided.
- The track's instrument is per step in this tracker, so the CHANNEL column is either per event or inherited; and the list has to say what a step event means when the instrument changes.
- Is a plain list worth it next to a piano roll and a CC curve page? The list is the precise tool (type a value, fix one bad event), the others are for gesture.
- 800 x 480 legibility of six columns at the project's smallest font: check with the gallery tools before committing to the layout.

## Bench items for the next hardware session
1. B1: frame time of the list screen with 16 rows (main loop pass time, `diag cpu`), while the heavy song plays.
2. B2: quantise and insert of 1,000 events: main-loop stall (the app stall measure of OS-H3B).
3. B3: undo snapshot size for a 1,000-event lane against the 160 KiB area.

## Priority
⭐3: the precise editing tool the lanes need to be trustworthy, and a phase 0 list over existing steps needs no format change; but it is the largest flash item (6 to 10 KB *estimate*) and its value depends on `cc-automation` and `midi-poly-record` existing first.
