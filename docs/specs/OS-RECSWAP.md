# OS-RECSWAP — Rec = free edit, Shift + Rec = edit following the playhead

Status: specified 2026-10-14 (maintainer decision), implemented 2026-10-14 (in 0.4.10) · Level B (keys and view state; no format, no audio) · **New Community OS behaviour**: it reverses the OS-LIVEREC choice of 2026-10-13 (what the official firmware's view does during a Rec while playing is an open research question).

## Today (verified 2026-10-14)

- Pattern, Rec tap = `pattern.edit_toggle`, Browse ↔ Edit (`bindings.cpp:304`); entering Edit while playing = the cursor follows the playhead.
- Shift + Rec (Global) = `pattern.rec_free` (`bindings.cpp:264`, `pattern_actions.cpp:45-56`): Edit with `view.edit.rec_free = true` (the cursor and the view stay where the user puts them); in Edit + free it goes back to following. Chip `FREE` vs `EDIT` (`pattern_screen.cpp:110`).
- Shift + arrows are refused while the cursor follows the playhead, with "SELECT: SHIFT+REC FOR FREE MODE".

## Change (maintainer, 2026-10-14)

- **Rec tap**: Browse ↔ Edit, and Edit is entered **free** (`rec_free = true`). Rec in Edit (free or following) goes back to Browse.
- **Shift + Rec**: from Browse, Edit **following** the playhead (`rec_free = false`); in Edit it toggles free ↔ following.
- The chip names the state (UI map rule 7): one chip for free edit and one for following (for example `EDIT` and `FOLLOW`; the engineer picks the labels within the chip's width and says so).
- **Selecting while following stays refused, silently** (maintainer, 2026-10-14: no, and no notification; a maintainer override of UI map rule 7 "a refused key says why"). Shift + arrows simply do nothing while the cursor follows; the chip already shows the state. Browse while playing also refuses silently. So a Focus Loop range is chosen while stopped or in free Edit (note for the Focus Loop item).
- Unchanged: where a recorded note lands (OS-LIVEREC: the playhead's step on the cursor's track, REC QUANTIZE rules), Play, the block itself.

## Acceptance

- Action tests: Rec and Shift + Rec from Browse, from Edit free and from Edit following, playing and stopped; Shift + arrows while following change nothing and post no notification.
- Update the `pattern_screen.h` and `bindings.cpp:220` comments, the help text, the generated key reference, sim goldens and the scenarios that press Rec or Shift + Rec.
- `docs/ui/ui-map.md`: Pattern row (Rec / Shift + Rec) and global keys.
