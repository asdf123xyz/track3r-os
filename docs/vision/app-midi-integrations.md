# App MIDI integrations: what OS-APPMIDI could unlock

Status: vision, 2026-10-14. **Nothing here is built or scheduled.** OS-APPMIDI (ADR 0014 amendment C) is at the end of the queue and not started. This page lists what app authors could build once it exists, and where the design stops them. It is a catalogue for the maintainer and for future app authors, not a commitment. Items are `new Community OS behaviour`, not parity. How to write an app today: [docs/guides/app-modules.md](../guides/app-modules.md).

Anything marked [unverified] is general knowledge or an inference that was not checked against a source in this repository. Do not claim a specific product's SysEx without its public documentation.

## 1. What the slices give an app

| Slice | What an app gets | Status |
|---|---|---|
| H1 | capability word, read table, launch context, `now_ms`, `get_tempo`, `set_tempo`, a MIDI-in ring (4-byte events, SysEx dropped and counted) | PLANNED |
| H2 | audio tap and peak/RMS | PLANNED |
| H3 | typed edit table, one undo group per apply | PLANNED |
| H4 | `export_open` / `export_write` / `export_close` into `/exports/<app>/` | PLANNED |
| C1 | `midi_out` capability, `midi_send(port, bytes, n)` for DIN, USB or both, SysEx-in buffer (4 KB default, chunked) | PLANNED, OS-APPMIDI |
| C2 | read and list of the app's own `/exports/<app>/`, and a send-at-tick queue | PLANNED, later |

## 2. Limits that shape every idea

- **Main-loop jitter.** An app runs in the main loop: 16 to 33 ms of jitter. Messages sent by app code are for configuration and editing, not for playing in time.
- **Send-at-tick queue (C2).** Timed output is scheduled by the render-side MidiOut, never by app code. Until C2, no app can promise a message on a beat.
- **Sequencer first.** `midi_send` is rate-limited per frame and capped in bytes per second per app. The sequencer's timing always wins.
- **DIN speed.** MIDI DIN is 31.25 kbaud, about 3,100 bytes a second. USB is far faster. Plan bulk traffic for USB where the gear allows.
- **No project storage.** Apps write only to `/exports/<app>/`, create-only. An app's parameters are not saved with the project.
- **Trust.** `midi_out` asks a confirm on first launch. Apps are trusted community code, not a sandbox.

## 3. Summary

| Idea | Needs | Timing class | Size |
|---|---|---|---|
| Synth editor | C1 (+ H1 ring to read knob moves) | config | M |
| Patch librarian | C1, C2, H4 | config, handshakes | L |
| Setlist scenes | C1, H1 (+ H4, C2 to keep scenes) | song change, loose | M |
| Sample Dump Standard | C1, C2, H4 | handshake, slow | L |
| MIDI Tuning Standard | C1 | config | S to M |
| Identity Request | C1, H1 (`now_ms`) | request and reply | S |
| Grid-controller LEDs | C1, H1 (+ C2 for beat flashes) | frame rate | M |
| MMC / MTC | C1 (MMC); MTC not for apps | MMC loose, MTC hard | S / L |
| MIDI-to-CV helpers | C1 (+ H1) | config | S to M |
| MPE input to the matrix | H1 for display; core for live use | live, hard | L |

## 4. The ideas

### 4.1 Synth editors (CC, NRPN, SysEx)

- **For the musician:** a page on the Tracker with the knobs of a hardware synth, drawn from the app's own parameters. Turn the encoder, the synth changes. No computer, no editor software.
- **Needs:** C1. The H1 ring is a plus: read the synth's own knob moves back as CC. `now_ms` (H1) for rate limiting.
- **Timing:** configuration. One message per control change at most every few frames. Not for playing.
- **Gear:** any synth, drum machine or effects unit whose manual publishes CC, NRPN or SysEx parameters.
- **Size:** M. The logic is a table of controls and a frame builder; the table comes from the gear's public documentation.
- **Risks:** a wrong SysEx byte can write a patch slot. Without a reply path (SysEx-in) the app cannot confirm anything. Parameters are not saved with the project, so the editor shows what it last sent, not what the synth holds (unless the synth can dump it).

### 4.2 Patch librarians

- **For the musician:** ask the synth for its patches, keep them as files on the SD card, send one back later.
- **Needs:** C1 (send, SysEx-in), H4 (write `.syx` files to `/exports/<app>/`), C2 (read them back and list them).
- **Timing:** dump requests and replies are asynchronous. Transfers that need a handshake pay the jitter on every step.
- **Gear:** instruments that answer a dump request with one or more SysEx frames.
- **Size:** L.
- **Risks:** the sandbox is create-only: a new name gets a numeric suffix, nothing is overwritten or deleted from the Tracker (the user cleans up on a computer). A per-app file count cap applies. A dump larger than the 4 KB SysEx-in buffer arrives in chunks, so the app must assemble it from start, continue and end flags and write as it goes (the app has no heap). Patches are never inside the project (section 5).

### 4.3 Setlist scenes

- **For the musician:** each song in a live set sends the right program change and a few CC values to the outboard gear, so the rack is ready when the song starts.
- **Needs:** C1. H1 read table to know which pattern or song is current. The read call names are [TBD] in ADR 0014. To keep scenes between sessions: H4 plus C2 (the app writes and reads its own file). Without them the scenes live in the app's state and are lost at power off.
- **Timing:** a program change at a song boundary tolerates 33 ms. It does not tolerate being late by a bar.
- **Gear:** anything that takes program change and CC.
- **Size:** M.
- **Risks:** persistence is the real cost (parameters are not saved). A program change while the synth plays can cut a tail. Sending on the wrong channel is silent and easy to miss.

### 4.4 Sample Dump Standard (SDS) to and from vintage samplers

- **For the musician:** move samples between the Tracker and old hardware samplers over MIDI, with no computer.
- **Needs:** C1 (frames out, SysEx-in), C2 (read own files for outgoing samples), H4 (write received samples as WAV into `/exports/<app>/`). Getting a received file into a project is the import UI of OS-FILES, not the app (ADR 0014).
- **Timing:** SDS is a packet protocol with a checksum and an acknowledge per packet [unverified: read the published standard before design]. Each acknowledge costs main-loop jitter. Over DIN, 31.25 kbaud makes a few seconds of sample take minutes [unverified estimate].
- **Gear:** vintage and some current samplers that implement SDS. Support varies; many are partial or buggy [unverified].
- **Size:** L.
- **Risks:** slow and fragile. A lost acknowledge needs a retry policy. Word length and rate conversion add DSP code (host-tested, in the app). Sample ownership and licence are the user's problem.

### 4.5 MIDI Tuning Standard (MTS) retuning

- **For the musician:** play microtonal scales, or just intonation, on a synth that accepts MTS, from a scale chosen on the Tracker.
- **Needs:** C1. The scale is the app's parameters or a table in its code.
- **Timing:** send once before playing, or on a scale change. A real-time single-note change is a command, not a performance gesture.
- **Gear:** synths that implement MTS bulk or real-time retuning [unverified which do].
- **Size:** S to M. The maths (frequency to MTS value) is a pure function with a host test.
- **Risks:** many synths ignore MTS. A retune that stays on after the app closes: send a reset to equal temperament in `exit()` (not called if the app is quarantined). The Tracker's own sampler voices are not retuned by this; that is a core feature.

### 4.6 Identity Request auto-detection

- **For the musician:** plug in a synth and the editor page picks the right controls on its own.
- **Needs:** C1 (send the request, receive the reply in the SysEx-in buffer), `now_ms` from H1 for the timeout. The request and reply are the universal non-realtime SysEx messages of the MIDI specification.
- **Timing:** send, then wait up to a timeout (tens or hundreds of ms) across frames. Main-loop jitter does not matter.
- **Gear:** any device that implements Identity Reply. Some do not answer [unverified how many].
- **Size:** S. A decoder of the reply plus a table from manufacturer and family code to an editor definition.
- **Risks:** a broadcast request goes to every device on the chain. A reply may arrive on the other port. A silent device is not proof of no device. The table of known gear is the app's to maintain, from public documentation.

### 4.7 Grid-controller LED feedback (Launchpad, APC style)

- **For the musician:** a grid controller shows the Tracker's state (playing pattern, muted tracks, a step) on its pads, and its pads act as extra triggers.
- **Needs:** C1 for LED messages, H1 ring for pad presses, the read table for state. Beat-synchronous flashes need the C2 send-at-tick queue.
- **Timing:** state at frame rate (30 Hz or less) is fine. A flash exactly on the beat is not, before C2.
- **Gear:** class-compliant grid controllers; many take note or CC messages for LED colour, some need a SysEx mode switch [unverified per model].
- **Size:** M.
- **Risks:** a full refresh of 64 pads at 3 bytes each is 192 bytes, about 60 ms on DIN (the arithmetic holds for any 3-byte message). Send only the difference, or use USB. A flood competes with the sequencer's MIDI output; the OS rate limit will drop or delay messages.

### 4.8 MMC and MTC transport

- **For the musician:** start, stop and locate a tape machine, DAW or hardware recorder from the Tracker (MMC), or slave one to the Tracker's position (MTC).
- **Needs:** MMC commands are short SysEx frames: C1. MTC is a different problem.
- **Timing:** MMC start and stop tolerate main-loop jitter if the user presses a key. **MTC does not**: quarter-frame messages arrive at a steady rate of the order of 100 a second [unverified rate] and the receiver notices jitter. The send-at-tick queue (C2) may help but is not a promise. MTC generation belongs in the core, not in an app.
- **Gear:** recorders, DAWs, some mixers and lighting or show-control gear.
- **Size:** S for MMC buttons. L, and probably out of scope for apps, for MTC.
- **Risks:** two transports in conflict (the core's external transport, planned in OS-MIDI2, and an app's MMC). MMC locate with a wrong frame rate lands at the wrong place.

### 4.9 MIDI-to-CV and Eurorack helpers

- **For the musician:** configure a MIDI-to-CV interface from the Tracker: channel, note range, clock division, calibration. Possibly a helper page that shows incoming notes and CC as a monitor while patching.
- **Needs:** C1 for configuration, H1 ring for the monitor. The Tracker has no CV output; this is about outboard converters.
- **Timing:** configuration. Pitch and gate timing come from the sequencer's own MIDI output, not from app code.
- **Gear:** MIDI-to-CV converters and MIDI-controllable modules that expose settings as CC or SysEx.
- **Size:** S to M.
- **Risks:** a wrong calibration or range value is a musical, not a physical, hazard, but a bad range can send a module out of tune or clip. Calibration data is per unit and per device.

### 4.10 MPE input into the modulation matrix

- **For the musician:** an MPE controller (per-note pitch, pressure, slide) modulates sound parameters through the OS-MODMX matrix.
- **Needs:** this is a live path from the MIDI parser to the audio engine. **An app cannot do that**: it runs in the main loop with a 33 ms ring, and the edit table changes the project (`add route`, `set route depth`), not a live value. Live MPE belongs in the core parser as matrix sources. The UI map notes bend and aftertouch as planned sources under OS-MIDI2; MPE itself is [unverified] there.
- **What an app can do:** show MPE traffic as a monitor (H1 ring); send MPE zone configuration (RPN, C1); create the routes with an H3 recipe (the MOD RECIPES app, after OS-MODMX).
- **Size:** L for the core feature. S for the setup app.
- **Risks:** an app that pretends to provide live MPE modulation would feel late and uneven.

## 5. What stays out

- **Storing gear patches inside the project.** A patch is a blob in a project file. That is a change to a persistent format: Level C, with a version bump, a backward-compatible loader and a round-trip test (ADR 0014 amendment C). It is a separate decision and is not part of OS-APPMIDI. Until then, patches live as files in `/exports/<app>/`, outside the project.
- **Timed or sample-accurate MIDI from app code.** Sequencing is the core's job. The C2 queue is scheduled by the render side.
- **Live modulation sources from apps** (4.10), MIDI clock and MTC generation (4.8).
- **App access to the rest of the card.** The sandbox is `/exports/<app>/`. No `/modules`, no `Firmware/`, no project folder.
- **Scripts.** Tier 2 scripts are closed (ADR 0002 amendment 4). Every integration here is a native module.
- **Parity claims.** Official images 1.2 to 1.9 are not claimed to do any of this.
