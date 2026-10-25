# OS-RECMICRO — recording microtiming and velocity (PAR-037, PAR-064)

Status: implemented 2026-10-14 (host and simulator verified; the hardware checks of section 6 are pending) · Level B · New Community OS behaviour; no parity content: every rule below is this OS's own.

## 0. What exists

- `app/rec_place.h` (OS-LIVEREC): where a live note goes (cursor track, playhead step rounded by REC QUANTIZE Nearest / Playing). The official manual describes one recording-options setting (notes only, micro-timing, velocity, or both; §5.7, not verified, see `docs/ui/official-manual-map.md`); it was not built as one setting.
- Our scaling (new Community OS behaviour): the playhead's microstep 0..47 maps onto the Micro-move's 0..100 as `ustep * 100 / 47` (section 2); the velocity of a keyboard note goes to the volume column as it is (section 3). What the official firmware writes is an open research question (Q).
- Release notes: 1.2.2 lists a fix to how recorded microtiming is quantised (PAR-037), 1.3.2 a fix to real-time microtiming recording that wrote one fixed value every time (PAR-064). **They give no rule**, and no measurement of the later behaviour exists. Nothing below is claimed as the official behaviour.

## 1. Settings (the o3-settings.md row rule: appended, default 0, no version bump)

| id | name | values | default |
|---|---|---|---|
| 20 | REC MICRO | Off, On | Off |
| 21 | REC VELOCITY | Off, External | Off |

They split the one four-valued recording-options setting the official manual describes (§5.7, not verified; OS-009 field 8 was never built, so no existing row is reused). Config PATTERN page, seven rows: REC POLY, REC QUANTIZE, REC MICRO, REC VELOCITY, METRO, METRO VOL, COUNT-IN (the page may have eight).

## 2. REC MICRO (rule)

1. The offset of a note is the playhead's microstep (0..47, `PlayPosition::ustep`) when the note is written to the step the playhead is on: REC QUANTIZE Playing step always; Nearest step in the first half of the step; the last step of a song (it has no next step).
2. A note that Nearest step rounds **up** to the next step is early for it. Micro-move (0..100, `fx_table.h`) is a delay only, so the offset is clamped to 0: no FX, the note is on the grid.
3. The value is `offset * 100 / 47` (0..100), our scaling of the 0..47 microstep onto the effect's range (the official firmware's rule is an open research question, Q, see section 0). A value of 0 writes **no** Micro-move (never an unconditional m02, never m02 00).
4. It goes to **FX 2** (the second FX column; FX 1 is never touched). FX 2 is written only when it is empty or already a Micro-move (an earlier note's, which the new note replaces: a zero offset then clears it). Any other FX in FX 2 is the user's and stays: the note then has no Micro-move.
5. The OFF written by a release is not micro-timed (whether the official firmware times it is an open research question).

## 3. REC VELOCITY (rule)

With External, the velocity (1..127) of a note played on the MIDI keyboard is written to the step's volume column (0..127, the same scale: no mapping). The pads have no velocity of their own (what the official firmware writes for a pad note is an open research question): a pad note leaves the volume column as it is. Off: the volume column stays as it was, as a note entered by hand leaves it.

## 4. Where

`rec_place.h` (`goes_on`, `micro_offset`, `micro_value`, `micro_slot_free`, `write_note`), `rec_actions.cpp` (`rec_note`), `app.cpp` (`poll_midi_notes` hands the velocity over as `PatternEdit::last_velocity`), `params.cpp` rows 20, 21, `config_screen.cpp`. Main loop only: nothing runs in the audio context, no flash-hot code, no SRAM_L.

## 5. Tests and QA

`rec_test.cpp` (the value for every microstep against the source's `map`, the Playing / Nearest truth table, the slot rule, the velocity mapping, the actions), `settings_format_test.cpp` (the literals of 13..20 settings read with both Off, the 214-byte default file, round trip, an older reader skips both), `config_screen_test.cpp`. QA rows QA-CFG-026..030 (`config.md`), QA-PAT-059..061 (`pattern.md`).

## 6. Open

- An exact 1.2.2 / 1.3.2 rule, if the behaviour is ever observed: the placement of an early note (whether the official firmware writes it on the playing step with a delay instead of snapping it is an open research question) and what m02 is written when.
- A negative offset needs a different FX or a wider range; the clamp is the reversible choice.
- Micro-timing the OFF; recording the pads' velocity (the pad board has none today).
- By ear / on the unit: a keyboard played against the metronome, the recorded pattern played back (the Micro-move is the sequencer's own FX, tested there).
