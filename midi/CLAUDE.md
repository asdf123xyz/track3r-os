# midi/ — MIDI timing core (pure state machines)

- Owns messages, the timed queue, the out router, the DIN byte-time emitter, USB-MIDI 1 ms quantiser, MIDI in with stamped bytes, the clock-follow DLL and the CC parser for MIDI Learn. Pure logic: the MK66 drivers (`platform/mk66`) only call `poll()` / `frame()` and write the byte. Must not: touch hardware, allocate, block, use `double`, or include `platform`, `app`, `model`. May include `core`, `usb`, `hal`, and `audio`'s event struct.
- Entry points:
  - `include/o3os/midi/out.h`: router from `audio::SeqEvent` MIDI kinds to bytes with a due time (clock, notes, CC A-F, PC, SPP)
  - `include/o3os/midi/din.h`: DIN emitter state machine and wire model (31,250 baud; a started byte cannot be interrupted, so clock bytes wait for it)
  - `include/o3os/midi/usb_out.h`: USB packets quantised to the 1 ms frame, never early
  - `include/o3os/midi/in.h`, `clock_dll.h`: ISR-stamped bytes into a ring, drained per block; clock follow that slews, never jumps
  - `include/o3os/midi/cc_in.h`: controller input to the main loop for Learn; `config.h`: the user's MIDI settings as the engine acts on them
- ISR boundary: an ISR stamps and pushes into a single-producer ring (`core/ring.h`); the render drains. Keep that protocol (one producer per ring) when adding a port.
- Add a message kind: a new `SeqEvent` kind at the end of its group (additive, `audio/events.h`), its encoding in `out.h`, a bound check in the DIN/USB tests (lateness bounds).
- Tests: `tests/midi_*_test.cpp`, `seq_midi_link_test.cpp`; they run in the seq group. `ctest --test-dir build-os-host -R unit_seq --output-on-failure`. `midi/` is unmapped in `tools/ci.sh`'s scope map (`O3OS_CI_SCOPE=changed` runs everything), so scope with `ctest -R` instead.
- Persistent: the device settings file holds CLOCK IN, CLOCK SYNC, CLOCK MODE, TRANSPORT IN, TRANSPORT OUT, MIDI OUT, OUT CHANNEL (`docs/formats/o3-settings.md`): a change there needs a version bump, a backward-compatible loader and a round-trip test.
- Docs: `docs/specs/OS-005-audio-engine.md` (005c, 005c2), ADR 0013 D5. QA rows in `docs/qa/catalogue/global.md` and `config.md` (format in `app/CLAUDE.md`). Hardware steps: `platform/mk66/tools/hil/{midi_notes_ready,cfg_clock_*,pfx_midi}.json`, `midi_clock_tool.py`.
