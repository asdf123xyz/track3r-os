# OS-UIFIX — a visible Shift label row wherever Shift + F is bound, and a test

Status: specified 2026-10-14 (maintainer decision), nothing implemented · Level A/B (labels, one test; no behaviour change) · Source: `docs/ui/ui-map.md` §3 item 1, §5 rule 3, recommendation 1.

## Today (verified in the UI map)

Only Pattern and Song ship `soft_keys_shifted` (`pattern_screen.h:51,65`, `song_screen.h:33`); the default (`screen.h:110`) repeats the plain labels, so holding Shift shows the wrong row. Invisible Shift + F bindings: **Sample edit** Shift + F1 / F2 AMIGA 8k / 16k, Shift + F3 Q8, Shift + F6 SAVE WAV, Shift + F7 preview from the edge, Shift + F8 REDO (`bindings.cpp:450-457`); **Wave** Shift + F6 preview from the point, Shift + F8 REDO (`:423-426`); **Instrument** Shift + F8 REDO (`:546`). The Global Shift + F1..F5 quarter jumps (`:286-290`) act on the list screens without a label.

## Change

- A `soft_keys_shifted` table for Sample edit, Wave and Instrument, labels at most 8 characters, `-` where nothing is bound (Instrument: REDO only, until OS-INSTUI S1 fills its row; Wave: OS-INSTUI S4 adds MODE on Shift + F1 later).
- The default `soft_keys_shifted()` stops repeating the plain row: it shows what Shift + F1..F8 really do on that screen (the Global jumps `0/4 1/4 2/4 3/4 END` where they act, `-` elsewhere).
- **A host test** walks every screen: for each Shift + F1..F8 binding that resolves in the screen's context chain (screen, mode, Global), the screen's shifted label for that key must not be `-` or empty, and a `-` label must have no binding. It fails on a Shift binding without a label (UI map rule 3).
- No binding changes.
- **Fall-through keys (review, same item).** The resolver tries a Shift + F gesture again without Shift when nothing in the chain answers it (`core/binding.cpp` `resolve`), so a key with no Shift binding runs its plain binding. Its Shift label is then the plain label, and `-` is only a key that resolves to nothing (Perform F6..F8 LATCH, NEXT PAT, RESET; Config F6 and F7; Mixer F7 and F8; Record F6..F8; Sample load F6 and F7; Wave F7; Song F6 and F7). The test resolves every key in every chain of every screen (modes, the fields found by walking the screen's pages and cursor) and fails on a `-` over a bound or fall-through key, and on a label that is not what runs. Recorded, not changed: Global's Shift + F1..F5 shadow the plain F1..F5 (they hide Perform's scene keys and move nothing there), and Sample edit's Shift + F4 and F5 both select the end edge.

## Acceptance

- The new test, green; sim goldens of the three screens with Shift held (and any golden of a list screen whose Shift row changes); `docs/ui/ui-map.md` §1 Shift column updated.

## Superseded in part by OS-SHIFTROW (2026-10-14)

The fall-through rule above and Global's quarter jumps are gone: Shift + F belongs to the screen, Global binds Shift + F1..F8 to `ui.none`, and a Shift + F key with no binding of its own is `-` and does nothing (`docs/specs/OS-SHIFTROW.md`). The labels, the rows of each screen and the test (now also failing any fall-through) are as built here; the jumps are bound in Song, File and Sample load.
