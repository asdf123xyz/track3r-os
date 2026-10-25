# OS-RECFLOW: count-in on row 1, REC out of LIVE, Shift+Play from the cursor

Maintainer requests of 2026-10-15 and 2026-10-16 (backlog row REC-FLOW). Level B: sequencer timing and transport keys. **New Community OS behaviour**; no OG parity is claimed. No persistent format change. One commit per ID.

## OS-COUNTIN: the count-in counts on row 1

**Before (checked 2026-10-16, OS-METRO).** The run started N bars before the start point, in the pattern's own steps, so from row 1 the count-in played **the notes of the pattern's last N bars** (every track, voices and MIDI) with the click on top, and the play line ran through those bars. A pattern shorter than N bars looped inside the count-in.

**Behaviour.** The count-in exists only from STOP (Shift+REC from stop; the Record screen's F1 from stop with ALIGN not FREE), so it always starts at row 1. The transport counts `16 x bars` steps while the play line waits on row 1:
- no step of the pattern plays (no note, FX, MIDI or tick event of a track);
- the step clock, swing, MIDI clock and the click run as before; Start is sent at the first step of the count-in;
- the published position is row 1 with `count_in` = the steps left (the view follows it, so the cursor shows row 1);
- the Step events (and click events) of the count-in carry row 1 and `kEvCountIn`; the first unflagged Step event is row 1 at the exact sample the count-in ends, so a take that starts during or before the count-in (OS-METROALIGN: Bar, Pattern, Step) begins on row 1's sample;
- a pad hit in the last half step of the count-in under REC QUANTIZE Nearest is row 1's (the play line is already on it): written with no further round-up; under Playing step it is dropped, as before;
- REC or Shift+REC during the count-in cancels it and stops the transport: back to WATCH stopped, nothing recorded or played, no undo unit (default of the orchestrator, to be confirmed by the maintainer).

**Done when:** the sequencer holds the step during the count-in and plays row 1 first; the take starts on row 1; strip, pad flash and click as before.

**Tests:** `seq_metro_test.cpp` (count-in length per bars setting, 1 and 2 bars, start point other than 0, pattern shorter than the count-in; no event of the pattern before the start sample; the first note is row 1's at the exact sample; the published step is row 1 meanwhile; the first usable Step event for a take is row 1; click beats; MIDI Start), `metro_app_test.cpp` (the view's cursor on row 1 while counting; the last-half-step hit lands on row 1). Goldens: `qa_pat_052..055` (the sim's count-in position is row 1).

## OS-LIVEEXIT: REC in LIVE goes to WATCH

**Behaviour.** In LIVE: **REC -> WATCH** (the take ends, the view keeps following the play line) and **Shift+REC -> STEP** with the cursor on the current row (the take ends). From WATCH playing, REC already gives STEP on the play line's row (unchanged). The other cells of OS-RECMODE2 section 2 are unchanged; the table there is amended (LIVE row: REC and Shift+REC swapped).

**Done when:** both transitions behave as above, the take is one closed undo unit in each, and RECMODE2's table, the QA rows and the scenarios that encoded LIVE -> STEP via REC say so.

**Tests:** `recmode_test.cpp` (both transitions, take ends, cursor on the current row, one undo unit), the scenarios that pressed REC in LIVE. Rows QA-PAT-130 and QA-PAT-131.

## OS-PLAYCUR: Shift+Play plays from the cursor's row

**Behaviour.** Shift+Play on Pattern:
- **stopped:** the pattern being edited starts at the cursor's row;
- **playing:** the play line jumps to the cursor's row at the next step boundary and goes on from there (no double trigger: the row jumped from is not played again, the row jumped to plays once);
- never a count-in (as Play); the count-in stays Shift+REC from stop only.

Shift+Play shares its key with the designed Focus Loop (`docs/ui/ui-map.md`, not built). With no block selected it is OS-PLAYCUR. With a block selected it is reserved for Focus Loop and, until that exists, falls through to OS-PLAYCUR (the block is left as it is).

**Done when:** both cases work from any Pattern mode; the cursor row is the start row in a song-less pattern play; QA rows added; ui-map and the comment in `pattern_actions.cpp` say who owns the key.

**Tests:** `play_cursor_test.cpp` (app: stopped start row in WATCH and STEP; playing jump; with a block selected; no count-in with COUNT-IN on; another pattern refused; other screens) and `seq_metro_test.cpp` (sequencer: a start from a row; the jump lands on the next step boundary, once, no step repeated or dropped, by command or by call; ignored when stopped and during a count-in), `builtin_bindings_test.cpp` (the Shift row). Mechanism: `Command::LocateRow` / `Sequencer::locate_row` (the pattern now playing, either mode), `TransportSink::play_from` / `locate_row`, action `transport.play_cursor`.

## QA ids

QA-PAT-230..239 are reserved for this batch (the PAT-UX lane uses the next free ids after QA-PAT-221).
