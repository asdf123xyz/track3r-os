# Usability review from the simulator (OS-FINALQA §1 item 5)

Status: 2026-10-20, first pass from the simulator gallery (`o3sim --gallery`, 147 screen × state shots per theme, 800×480). Reviewed against `docs/ui/ui-map.md` §5 and the five checks of OS-FINALQA §1 item 5: budget rules, Shift+F labels, header names screen and page, three presses or fewer to a common action, F-key meaning always visible, no hidden mode, readability in amiga, black and white.

The pictures live outside git (the gallery regenerates them). File names below are gallery file names in `amiga`, `black` or `white`, with the theme suffix where it matters.

Result: **0 blockers, 4 confusing, 4 cosmetic.** No usability blocker is open. The four confusing items are small label or header fixes; each is marked for the implementer. Confirmation of the code side is listed per row.

## Findings

| ID | Screen | Picture (gallery file) | Finding | Severity | Proposed fix | Status |
|---|---|---|---|---|---|---|
| USA-001 | Instrument (Params) | `params_env_amiga.png`, `params_worst_amiga.png` | The header reads only `PARAMS`; the page is not named and no F1–F5 key shows as the active page. Config does name its page (`PAGE INFO`), so the two screens differ. | confusing | Append the page name to the header (`PARAMS · VOICE`), as Config does. Label change only. | **not a bug: the page is named at the header's right edge (`SOURCE 1/5`, `VOICE 2/5`, `params_screen.cpp` header block: `param_group_name` plus its number); the strip carries the instrument and its mode on a sample or MIDI instrument by design (OS-INSTUI S1), and a module or free slot shows `PAGE` in the strip** |
| USA-002 | Instrument (Params) | `params_env_amiga.png` | The F1 label reads `SAMPLE`; `ui-map.md` §1 names the instrument pages SOURCE · VOICE · FILTER · MOD · SENDS. The page body shows a `SOURCE` card (`params_screen.cpp:533`). Confirm which label F1 carries on each instrument page and align the label or the map. | confusing | Align the F1 label with the page it opens (`SOURCE`), or correct `ui-map.md` §1. Confirm in `params_screen.cpp` first. | **not a bug: F1 is labelled by the play mode, `SAMPLE` / `SLICE` / `WT` / `GRAIN` (`source_label`, `params_screen.cpp:652`), and opens the SOURCE page whose card says `SOURCE`; `ui-map.md` §1 already says so in its key column** |
| USA-003 | File (Export, OG folder counting) | `file_export_og_counting_amiga.png` | While the OG export counts files, the body says `F8 stops it`, but the F8 key has no label and is bound to nothing in this state (`bindings.cpp:591`). F4 SAVE, F5 SAVE AS, F6 IMPORT and F7 PROJECTS keep their labels, while the body says `nothing else works until it ends` (the F4/F5 bindings are the screen's, `bindings.cpp:587`). The progress overlay (`export_og_progress_amiga.png`) is correct: F1–F7 blank, `F8 CANCEL`. | confusing | In the counting state, show `F8 CANCEL` (if F8 is meant to stop it) and blank F4–F7; or drop the F8 sentence. Make the counting state match the progress overlay. | **fixed 15fe30d: the notes read `Once started, ...` and `While it runs, F8 stops it`, so the page no longer promises an F8 or a lock that exist only while the export runs (F8 stays blank on the page, F4-F7 keep their labels)** |
| USA-004 | Config (INFO page) | `config_info_amiga.png` | The read-only INFO page shows `F7 APPLY`. APPLY only commits a pending USB row (`param_actions.cpp:590`), so on INFO it labels a key that does nothing. | confusing | Blank F7 on the INFO page (keep APPLY on the pages that have a pending value). Verify that INFO has no pending row first. | **fixed 15fe30d: INFO has no row and no pending value, so F7 is blank there (and on SD, the same case)** |
| USA-005 | Pattern | `pattern_typical_amiga.png`, `pattern_shift_layer_amiga.png` | The header reads `PAT 01/1 BPM …` and never names the screen as `PATTERN`; the other screens name themselves (`SONG`, `MIXER`, `FILE`, `RECORD`). The OS-FINALQA header rule asks for the screen name. | cosmetic | Use `PATTERN` in the header, or record `PAT` as the accepted short name in `ui-map.md` §1. | **not a bug: `PAT` is the Pattern screen's accepted short name (the number field `PAT 01/1`, `pattern_view.cpp` `draw_strip`); recorded in `ui-map.md` §1** |
| USA-006 | Record | `record_none_amiga.png` | The hint in the header runs to the right edge (`…with MONITOR on` ends at about x=792 of 800). It touches the margin and would clip on a longer hint. | cosmetic | Shorten the hint, e.g. `input heard only with MONITOR on`. | **fixed 15fe30d: `input heard only with MONITOR on`** |
| USA-007 | Sample edit (Shift) | `sampleedit_shift_amiga.png` | The info text uses lower-case key notation (`shift + F1: 8287 HZ`), while the keycaps and the QA catalogue write `Shift + F1`. | cosmetic | Write `Shift+F1` in the hint text, as the keycaps do. | **fixed 15fe30d: `Shift+F1: 8287 HZ` and the two after it** |
| USA-008 | Pattern (Shift layer) | `pattern_shift_layer_amiga.png` | Under Shift the track header shows MUTE / SOLO boxes and small `M`/`S` markers next to the track names. The markers crowd the name (`01 KICK`, `02 SNARE`). | cosmetic | Keep the boxes; move the M/S markers to the header's right edge with a space from the name. | **not a bug: the M / S marks sit in their own slots at the column's right edge and the name is clipped to the room they leave (`draw_headers`: `room` subtracts `slots * mark_w`), so they cannot crowd a name** |

## Checked, no finding

- **Display-list budget (ui-map §5 rendering limits):** the gallery's budget log has an `ok` verdict on every line for every screen, state and theme. This is the render budget only; the F-page and press counts of §5 were not re-counted in this pass.
- **Hidden modes:** the states checked in the pictures (STEP, LIVE, WATCH, EDIT, the overlays) each show a chip or info text, and the Learn and Song Tools overlays own the key row (blank keys, labelled answers). Not every screen's modes were checked.
- **Contrast:** the grey info text (`ChromeDim`) measured 5.3–5.5 : 1 on amiga and white; the white-theme mixer and perform text measured 4.6–5.8 : 1. Each passes WCAG AA (4.5 : 1) for normal text. No contrast blocker.
- **Clipping and overlap:** no overlap or clipped text found in the thumbnails of the three themes; the overlays fit their frames.

## Not covered by this pass

- **Shift+F labels:** the gallery shows the Shift row only for Pattern (`pattern_shift_layer_*`) and Sample edit (`sampleedit_shift_*`). The other screens' Shift rows are checked by the OS-UIFIX test (`soft_keys_shifted`), not by this review. A follow-up gallery pass should add a Shift shot per screen.
- **Text size at arm's length:** this review judges size from the 800×480 pictures, not from the panel. The size question needs the unit.

## Unit shots still to review

The tester's unit shots come later (OS-FINALQA §3, step 3). When they arrive, check:

- USA-001 to USA-004 on the panel (labels and the F-key row as the unit draws them, not the simulator);
- the Shift row on every screen (not just Pattern and Sample edit);
- text size and contrast at arm's length, in the three themes, on the unit's screen (the key-cap bar is known to render darker on the unit; see OS-SIMKEY in `OS-FINALQA.md` §0);
- the Learn overlay with a target and with bindings (F1–F4 labels are state-dependent).

Small fixes (USA-001 to USA-008, all label, header or alignment work) go to the implementer as one batch. None of them changes a key's meaning, so none needs a maintainer decision. Any change to the F-key layout would be a maintainer item.
