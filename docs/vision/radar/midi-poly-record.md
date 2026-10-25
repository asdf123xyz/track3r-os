# MIDI poly record (chords and overlapping notes, finer than a step) — radar card
Maintainer's words: "que en los instrumentos midi podamos grabar multiples notas y cc's" · 2026-10-21 · Status: idea

Related cards: `cc-automation` (same lane structure, and the CPU and DIN arithmetic), `midi-event-list` (the editor). Backlog row: OS-MIDIREC (part b).

## What it is
A MIDI instrument track records several notes at once (a played chord, a legato overlap) with their real start and length, in one track, instead of one note per step and one track per chord tone. A chord no longer eats 4 to 6 of the 16 tracks.

## What exists today (facts from this repository)
- A step holds one note: `Step` is 7 bytes (note, instrument, volume, two FX); a pattern is 16 tracks x 128 steps (`model/include/o3os/model/step.h`, `pattern.h`).
- Chords today: FX 14 Chord plays up to a 30-entry mask of notes from a root on a MIDI instrument (`seq/fx.h`, `docs/specs/OS-PFX.md`); the CHORDS app writes roots and FX 14 or spreads the tones over tracks (`docs/specs/OS-H3B.md`).
- Live recording (OS-LIVEREC, `docs/architecture/os-tree.md`): device setting REC POLY, Off by default, "Spill" sends the notes held together to the next tracks, one a voice; REC MICRO writes Micro-move (FX 2) with the position inside the step (`docs/specs/OS-RECMICRO.md`). Micro-move gives a start on the 48-microstep grid (1/48 of a step, `kMicrostepsPerStep`); the end of a note is an OFF step.
- Notes in from MIDI are one message a ring slot, without a time stamp, applied by the main loop about 16 ms later (`midi/cc_in.h` NOTES IN; the position comes from the playhead read by the main loop): about 6 ticks of 192 PPQN at 120 BPM.
So most of the musical need (chord, finer than a step) is already reachable across tracks; what is new is track economy, note length independent of OFF steps, and overlap on one channel.

## Prior art
| Project | URL | Licence | Language | Last activity (GitHub API, read 2026-10-21) |
|---|---|---|---|---|
| Seq66 / seq24 lineage: patterns of MIDI events with note length, event editor | https://github.com/ahlstromcj/seq66 | GPL-3 (`licenses/LICENSE.GPL` v3 text; GitHub reports none) | C++ | pushed 2026-10-21 |
| Synthstrom Deluge firmware (community): note rows in MIDI clips, MPE and poly aftertouch handling, "poly expression to mono" | https://github.com/SynthstromAudible/DelugeFirmware , https://delugecommunity.com/features/community_features/ | GPL-3.0 (GitHub field; "only" or "or later" not determined) | C++ | pushed 2026-10-21 |
| LMMS: piano roll with notes as (tick, length, key, velocity) | https://github.com/LMMS/lmms | GPL-2.0 (GitHub field) | C++ | pushed 2026-10-19 |
| OpenMPT: MIDI macros and one note a channel; notes are step-bound as in our model | https://github.com/OpenMPT/openmpt | BSD-3-Clause | C++ | pushed 2026-10-09 |
| MilkyTracker: a tracker with one note per channel per row, i.e. our model's lineage | https://github.com/milkytracker/MilkyTracker | GPL (COPYING holds the v3 text; GitHub reports NOASSERTION) | C++ | pushed 2026-09-10 |
| MIOS32 / MIDIbox SEQ V4: step sequencer with chord and multi-note tracks (that the SEQ V4 application sits in this source tree, and its chord features, are from memory and not verified here) | https://github.com/midibox/mios32 | not an OSI licence: non-commercial terms by the author's forum statements (not verified in a LICENSE file); study only | C | pushed 2026-03-13 |
| Zynthian zynseq (pattern sequencer, MIDI) | https://github.com/zynthian/zynthian-ui | GPL-3.0 | Python (UI), the sequencer library is C++ (not verified here; the `zynseq` repository URL returned 404) | pushed 2026-10-19 |
| Dirtywave M8, Renoise, Elektron | concept only (a row is a time slot; chords by extra columns or tracks) | proprietary | n/a | n/a |

## Approach
inspire / adapt the data model; no code to port. A lane note is a record `(start tick, length, note, velocity, channel/port)`; the sequencer turns it into a note-on at start and a note-off at start + length through the existing `MidiOut`, so DIN/USB timing, running status, the hang book and panic stay the router's own (`midi/out.h`, `midi/hang.h`). Lanes drive MIDI output only: no audio voice is ever started by a lane note, so the 12 to 22 % a voice costs (`docs/architecture/cpu-budget.md`) cannot appear.
Staging of the work:
- Phase 0, no format change: raise the precision of what exists (the render stamps the incoming note with the playhead tick instead of the main loop's read; Spill keeps its rules; the same record sink). The OG `.mt` export keeps working.
- Phase 1: the event lane of `cc-automation` carries notes too (8-byte events), recorded as note-on and note-off pairs, held in a 128-entry table of start ticks (512 bytes with a velocity byte *estimate*).

## Cost on MK66 (estimate until measured)
CPU, with the figures of `cc-automation.md` (render ~300 cycles a lane event, DIN ISR ~120 cycles a byte *estimate*):
- A 6-note chord: 12 events (6 on, 6 off) x 300 = 3,600 cycles in the render once per chord, 1.4 % of one block (261,120 cycles) *estimate*; then 6 x 3 + 6 x 3 = 36 bytes x 120 = 4,320 ISR cycles over the following 11.5 ms. Nothing sustained.
- 16 tracks of 4-note chords every beat at 120 BPM: 16 x 4 x 2 = 128 events per 0.5 s = 256 events/s x 660 = 0.17 M cycles/s = 0.09 % *estimate*.
- Burst: all 16 tracks of 4 notes on one tick = 64 note-ons x 300 = 19,200 cycles = 7.4 % of a block, 64 x 0.96 ms = 61 ms of DIN wire (the last note 61 ms late: a smear) *estimate*; hence the per-block cap and notes-by-due-time ordering. USB sends 16 events a 1 ms frame: 4 ms.
- Record, render side: the note ring word needs a stamp (32 -> 64 bits); main loop per note ~150 cycles; negligible.
Memory (*estimate*): 8 bytes a note (tick u16, length u16, note, velocity, channel and port). A 4-bar pattern (8 s at 120 BPM) of 4-note chords every beat = 64 notes = 512 bytes a track. 16 tracks x 256 notes x 8 B = 32 KB for a dense pattern. Shared pool of 128 to 256 KiB SDRAM with the CC points. SRAM: the held-note table 512 bytes plus one cursor a track (about 16 x 2 B). Flash: lane notes in the player about 1.5 to 2.5 KB on top of `cc-automation`. Lives in: core flash (render path and project file). Not an SD plugin.

## Fits
Sequencer (`seq/`, per-track lane cursor with the per-track length of `Pattern::track_length`), model (lane arena, undo as a lane snapshot per take, never an entry per event), `app/record.h` and the note ring in `midi/cc_in.h`, formats (additive chunk, provisional `MEVT`), UI (the lane is shown in the event list card and, later, a piano-roll-like row view). Depends on: OS-LIVEREC, OS-005g (notes in), OS-INSTUI S8b (MIDI route), the lane structure of `cc-automation`.

## UX sketch
Rec on a MIDI instrument track: hold a chord on the pads or the keyboard; it is stored as one event group at the played time (device setting for QUANTIZE: off / 1/16 / 1/32; REC MICRO as today). The Pattern grid shows the track cell as "CHORD n" or "LANE" for a step that starts a lane note; Enter opens the event list for that track (`midi-event-list`). A track is either step-based (as today) or has a lane (a per-track mode), so nothing changes for existing projects.

## Licence
The OS is GPL-3.0-or-later. Nothing to port; the data model (a note as start, length, pitch, velocity) is common knowledge. Do not copy code from the GPL projects above (mixing GPL-2.0-only or GPL-3.0-only code would fix the combined licence); OpenMPT is BSD-3-Clause and could be ported with attribution but offers nothing needed. MIOS32 and the closed products: study or concept only.

## Risks / open questions
- **Project format change = Level C** (an additive chunk `MEVT`: version bump, backward-compatible loader, round-trip test; maintainer decides). An older build holds unknown chunks only up to 16 chunks and 16,384 bytes in all (shared with `OGPN`), so it may drop the lanes and, once it saves, lose them (`docs/formats/o3-project.md` "Unknown chunks").
- **OG `.mt` export loses the lanes**; alternative: bake the lane to steps (chord to FX 14 or Spill tracks; lossy). Maintainer decision.
- Overlap of the same note on one channel: the receiver has one voice per note and the hang book is per channel and note (`seq/midi_link.h` known limits); a re-strike must send an off first, a rule to decide.
- DIN: a chord smears by 3 bytes x 320 us = 0.96 ms a note (13 bytes = 4.2 ms for 6 notes with running status); USB does not.
- Note length across the loop end and track length: clip, wrap or ring over? Swing and the track play modes (backward, ping-pong) apply to lanes or not?
- Which instrument and channel a lane note uses, since the instrument is chosen per step in this tracker.
- Is phase 0 enough? REC POLY Spill plus Micro-move already record chords with sub-step starts; the new value is track economy and note length. The maintainer may prefer to stop at phase 0.
- Mute, solo and Perform on a lane track: reuse the track's (the same sequencer paths).

## Bench items for the next hardware session
1. B1: render cycles for a 64-note burst in one block (heavy song running).
2. B2: DIN lateness of the last note of a 6-note chord with and without running status (measure with an external timestamping tool).
3. B3: SDRAM scan of a lane while the sampler runs (cache contention).
4. B4: record accuracy: play a rhythm from a MIDI keyboard, compare stored ticks with the intended ones (block stamp vs main loop read).

## Priority
⭐3: real track-economy gain for MIDI users and a clean base for lanes, but most of the musical result (chord, sub-step start) is already reachable with REC POLY Spill, REC MICRO and FX 14, and a lane costs a Level C format decision; do it with `cc-automation`, not before.
