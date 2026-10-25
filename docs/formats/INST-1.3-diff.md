# INST 1.3: format diff for the owner (OS-INSTUI S8b + S9)

Status: for review before the merge of `feat/OS-INSTUI-S8B`. Level C (a persistent format). The format is documented in `docs/formats/o3-project.md` (section 3, "INST"); this note is the short diff.

## What is appended

INST 1.2 ended with the engine byte. **INST 1.3 appends twelve bytes after it.** Nothing before them moves; the older fields keep their offsets and meaning.

| # | Byte | Range | Default | Meaning |
|---|---|---|---|---|
| 1 | velocity | 1..127 | 127 | velocity of a pad note and of a step note that names none (MIDI instrument) |
| 2 | program | 0..128 | 0 | 0 none, else program + 1 (0..127): sent when it changes and before the first note after a project load |
| 3 | out | 0..3 | 0 | 0 the device's MIDI OUT port, 1 DIN, 2 USB, 3 both |
| 4 | pb range | 0..24 | 2 | the bend range, in semitones each way, that the Micro-tune FX of a MIDI step is sent for as pitch bend: **2 by default** (the MIDI default of most receivers: the OS does not send an RPN to set it); 0 sends no bend |
| 5 | offset | 0..100 | 0 | milliseconds the instrument's MIDI is delayed by |
| 6..11 | CC A..F | 0..131 | 0 each | 0 the Config page's number, 1..128 controller 0..127, 129 PROG (Program Change), 130 channel aftertouch, 131 polyphonic aftertouch |
| 12 | VEL> | 0..100 | 100 | a sample instrument's: how much the velocity of a note from the MIDI keyboard counts, a percentage. Continuous: played = 127 - VEL> / 100 x (127 - velocity), rounded. **100 (the default) is the velocity as it is, the law the keyboard has always had; 0 ignores it (every note plays as velocity 127 does)**; between is linear. The range is 0..100, so there is no extension above 100 (a larger value reads as the default, and the function that applies it holds it to 100). A pad has no velocity and is not touched |

New Community OS choices (no parity is claimed) that are reversible: the PB RANGE default (2, the MIDI default of most receivers: PAR-039's range is unknown), the OFFSET unit (ms, 0..100, delay only: PAR-075's unit is unknown), VEL> and its linear law (the default changes nothing), and that CC A..F are per instrument with "Config" (the project's MIDI chunk table) as the default.

## Byte layout of the chunk

`u16 version (0x0103)`, the 1.0 fields (slot, kind, channel, module index, name, path, 296-byte sampler block, blob length, blob), `u32 sample rate`, `u8 engine` (always present in 1.3, 0 = automatic), then the twelve bytes above in that order. Payload = the 1.1 payload + 13 bytes. A chunk of 1.3 is exactly that long: a byte short or long is `Malformed`.

## Write condition

Per instrument, whatever its kind: **1.3 only when at least one of the twelve bytes differs from its default**; else 1.2 when the engine is not automatic (the byte layout of OS-004d); else 1.1. So a project that never used the new fields is the file it was. Checked: the empty, small, state, persist2, maximal and settings fixtures of the build before INST 1.3 and of this branch are byte-identical (the first four have their size and CRC-32 pinned in `tests/inst_midi_format_test.cpp`).

## Readers

- **A 1.3 reader reads 1.2 and 1.1** as the defaults (the table above). A value out of a field's range in a 1.3 chunk reads as that field's default (tolerant, like the engine byte, so a later minor may add values); the model refuses such a value from the UI.
- **A 1.4 file in a 1.3 reader**: read as far as 1.3, the rest skipped, `newer_minor` set, lost on the next save (the same minor rule as every chunk).
- **An older reader** (INST 1.2 or 1.1) sees a newer minor of `INST`. Checked with the build before INST 1.3 (`o3pj_tool resave` on a 1.3 fixture): it loads, reads the instrument as far as it knows (rate, engine, channel, name, sampler block), and writes it back as 1.2 / 1.1: **the twelve fields are lost on its next save, the instrument stays intact**.

- **Learn bindings** to the twelve new parameters (ids `0x80..0x8B` of an instrument, all of them learnable) live in the project's `LRNB` chunk, which does not change. An older reader keeps such a binding in its Learn store and writes it back, but it has no parameter of that id, so a control change bound to it does nothing there (the binding is skipped when the message arrives). Nothing is lost by opening and saving the project in the older build.

## Other changes that travel with it (not format)

- `Instrument` grows 704 -> 716 bytes in memory; the model arena grows by 3,840 B (64 x 12 B of instrument and 12 parameter slots x 64 x 4 B of generation counters): 1,928,592 -> 1,932,432 B of SDRAM.
- The undo record of a whole-instrument edit grows from 1,440 to 1,464 journal bytes.
- Twelve parameter rows at ids `0x80..0x8B`; every row is a Learn target. They are the project's and the screen's: the app SDK (ABI 1.6, `O3M_SAMPLE_PARAM_COUNT` 83) does not list them and `get_param_desc` refuses their ids for a sample instrument, so the published ABI is exactly what it was (`instrument_param::kSdkCount`).
- The pitch bend a Micro-tune FX sends is sized by PB RANGE (default 2 semitones); the centre goes to every port a note bent, at a stop and before a note of the same port without the FX. A stop also drops a bend that is staged behind an OFFSET, so none can land after the centre.
- New MIDI event kinds in `audio/events.h` (pitch bend, channel and polyphonic aftertouch); `flags` and `value` of the channel events carry the route (ports, OFFSET in microseconds).

## Tests

`tests/inst_midi_fields_test.cpp` (VEL> at 0, 25, 50, 75, 100 and out of range) and `tests/pattern_edit_app_test.cpp` (the keyboard path), `tests/seq_midi_inst_test.cpp` (the default range of 2 semitones, the centre on every bent port), `tests/inst_midi_format_test.cpp` (the bytes from the description, the write condition field by field, round trip, older files, damaged values, lengths, 1.4, the maximal project, the size and CRC pins), `native_format_test.cpp` and `project_state_test.cpp` (the minor-version cases moved to 1.4), `tests/o3project_roundtrip.py` (ctest `project_python`, the `midi13` fixture, C++ against the Python reference).
