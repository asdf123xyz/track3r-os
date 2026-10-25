# OS-INSTRMIDI: the Instr column reaches the MIDI instruments

Level B, size S, **new Community OS behaviour**: this is the OS's own design. Whether the official firmware numbers, orders and labels its MIDI instruments the same way is an **open research question** (no evidence in the repository). No persistent format changes.

## The design

- There are 48 sample instrument slots (0..47). A step names an instrument as `slot` 0..63; the display number is `slot + 1`, so 1..64.
- The instrument selection clamps to 0..63 (47 + 16) and the delta to -5..5. The encoder's order is this OS's own: slots 63..48 first, then 0..47.
- Slots 48..63 (display 49..64) are the 16 MIDI channels: the instrument name list has 16 more entries, and a MIDI instrument reads `MIDI CH n` (its channel `n`) wherever the list is shown. The label text is this OS's own.

## What the OS does now (found, mostly already there)

| Part | State | Where |
|---|---|---|
| Range of the Instr column | **already 1..64** (`kInstrumentSlots`), the encoder walks this OS's order, tested against the reference vectors of `tests/data/pattern_edit_vectors.h` (the output of our own reference model, not evidence of the official firmware); a pad enters 1..48 (there are 48 pads); `step.instrument` takes 1..64 | `model/step_edit.cpp` `instrument_walk`, `instrument_set`, `instrument_delta`; `app/step_actions.cpp` |
| Encoder direction | in this OS's order **up from 1 stops at 48** and **down from 1 goes to 49** (then 50..64; 64 is the start of the order and does not wrap) | `tests/pattern_edit_rules_test.cpp` (the walk against the reference vectors) |
| Instr popup | already lists all 64 slots; a MIDI instrument reads `MIDI CH n` on its channel (short enough for the 320 px panel) instead of its stored name (`MIDI n` from an OG import, empty when made by hand) | `app/screens/step_popup_overlay.cpp` `step_popup_instrument_text` |
| Sequencer | a step on a slot that holds a **MIDI instrument** sends note-on/off on that instrument's channel (`Sequencer::send_note_on`), whatever the slot; test for 49 -> channel 1 and 64 -> channel 16 | `seq/sequencer.cpp`, `tests/seq_sequencer_test.cpp` |
| OG import | creates the 16 MIDI instruments in slots 49..64 (`MIDI n`, channel n) | `formats/og_project.cpp` |
| Save / load, native | the step byte `instr` 0..255 is stored as is (PATT chunk); the MIDI instrument is an INST entry with its channel. Round trip of a note on 49, 63 and 64 and the instrument 64: byte for byte | `tests/native_format_test.cpp` |
| OG pattern export / import | the step byte is `slot + 1` (docs/formats/og.md): bytes 48 and 63 import as 49 and 64 and export the same bytes | `tests/og_pattern_test.cpp` |

## Findings for the maintainer (no code built: out of scope or an maintainer decision)

1. **A native project has no MIDI instruments in 49..64.** Only an OG import creates them (docs/formats/og.md). A step on instrument 49 in a native project names an empty slot and is silent. The maintainer decided on 2026-10-14 that any slot can be any type (STATUS.md, "Instrument handling"); creating a MIDI instrument by hand waits for OS-INSTUI (the MIDI kind of the free-slot picker). Until then a step can name 49..64 and nothing sounds unless the slot holds a MIDI instrument.
2. The sequencer already sends MIDI for **any** MIDI-kind instrument in any slot, not only 49..64.
3. The pattern cell shows the number as a decimal pair (49..64); `M01..M16` is PAR-104 / P1e.1, not here.

## Questions (closed 2026-10-14; no code change for 0.4.14)

- **Q-IM-1** Which number does a MIDI instrument print? The OS prints `slot + 1` (49..64), the number the step shows, as the samples do.
  **Closed:** keep `slot + 1` (49..64): our choice, new OS behaviour. Whether the official firmware prints another number for its MIDI entries is an open research question.
- **Q-IM-2** Is the channel fixed by the slot? The OS stores the channel on the instrument, so the label follows the instrument's channel, not the slot.
  **Closed:** the channel follows the instrument (OS), because any slot can be any kind. An OG project import gives slot 48+n channel n (the import rule of `formats/og_project.cpp`); whether the official firmware fixes the channel by the slot is an open research question.
- **Q-IM-3** An empty slot 49..64 in a native project: the OS leaves it blank (it is not a MIDI instrument). Whether the official firmware labels such a slot is an open research question. Maintainer call if a label is wanted.
  **Closed:** it stays blank in a native project (nothing sounds there). The fix is in OS-INSTUI: the empty-slot chooser's F2 MIDI defaults to channel slot−48 on 49..64 and 1 on 1..48 (the OS-INSTUI spec).

## Tests

`pattern_edit_app_test` (encoder at 48/49/64, ends, out-of-range refused; list label), `seq_sequencer_test` (49 -> ch 1, 64 -> ch 16), `native_format_test` and `og_pattern_test` (round trips). QA rows QA-PAT-120..123 in `docs/qa/catalogue/pattern.md`; the unit scenario is `platform/mk66/tools/hil/qa_instrmidi.json`.
