# CC automation lanes on MIDI tracks — radar card
Maintainer's words: "que los tracks puedan grabar los cc's o los midis estilo automatizacion? si hay muchos reventaria la cpu? muchos mensajes?" · 2026-10-21 · Status: idea

Related cards: `midi-poly-record` (notes in the same lane structure), `midi-event-list` (the editor). Backlog row: OS-MIDIREC (part a).

## What it is
A MIDI track records the controllers it receives (MIDI IN, DIN or USB) and the ones the Tracker's own controls send, as automation lanes (CC, pitch bend, aftertouch) with time finer than a step. They play back with the pattern, smoothly (interpolated between thinned points), never saturating the MIDI OUT wire.

## Answer to the CPU question (the numbers are *estimate* until the bench items below are run)
Short version: the CPU is not the limit. The limits are the DIN wire (about 1,040 messages a second) and bursts that land in one audio block.

| Quantity | Arithmetic | Result |
|---|---|---|
| Cycles per sample | 180,000,000 / 44,117 | 4,080 |
| Cycles per 64-sample block (1.45 ms) | 4,080 x 64 | 261,120 |
| Render cost of one lane event (find, build event, route, stage) | ~300 cycles *estimate* | 0.11 % of a block |
| DIN ISR cost per byte | ~120 cycles *estimate* | 360 per 3-byte message |
| One DIN message, render + ISR | 300 + 360 | ~660 cycles *estimate* |
| DIN wire, 3-byte message | 10 bits / 31,250 baud = 320 us a byte; 3 x 320 | 960 us, so 1,041 messages/s |
| DIN wire with running status (2 bytes) | 2 x 320 | 640 us, so 1,562 messages/s |
| CPU at DIN saturation | 1,041 x 660 = 0.69 M cycles/s / 180 M | **0.38 %** *estimate* |
| CPU at 100 messages/s | 100 x 660 = 66 k cycles/s | 0.04 % *estimate* |
| Events/s at which CPU reaches 1 % | 1.8 M cycles/s / 660 | ~2,700 events/s *estimate*; only USB can carry that |
| USB MIDI cap in this OS (one 64-byte packet = 16 events per 1 ms frame, `midi/usb_out.h`) | 16,000 events/s x ~410 cycles = 6.6 M cycles/s | 3.6 % *estimate* |
| For scale: one TB-303 voice, measured | `docs/architecture/cpu-budget.md` | 21.6 % |

So: up to about 1,000 events/s the CPU cost is under 0.4 % (about 1/55 of one TB-303 voice). The DIN wire saturates at about 1,040 messages/s (1,560 with running status) before the CPU matters. What can hurt is a burst. The render already averages about 95 % on the heavy song (`cpu-budget.md`), so the headroom is about 5 % x 261,120 = 13,000 cycles a block, about 43 events of 300 cycles inside ONE block. A loop restart that re-sends the value of 16 tracks x 8 lanes = 128 controllers in one block costs 128 x 300 = 38,400 cycles (15 % of a block) and puts 128 x 960 us = 123 ms of bytes on the DIN wire (the staging queue of 256 items would take 256 x 0.96 ms = 246 ms to drain). So the design needs: a cap on lane events per block (for example 16, which costs 16 x 300 = 4,800 cycles = 1.8 % of a block, *estimate*), a last-sent table so a restart sends only values that differ, and notes sent before controllers.

### Wire arithmetic for dense automation (120 BPM, 192 PPQN, `seq/sequencer.h` `kPpqn`)
- One tick = 1 / (192 x 2 beats/s) = 2.6 ms, so one value per tick per lane = 384 messages/s per lane. Two lanes at that rate take 768 x 0.96 ms = 0.74 s of wire per second with 3-byte messages (74 %), or 768 x 0.64 ms = 0.49 s (49 %) with running status; three lanes take 1,152 x 0.96 ms = 1.1 s: over 100 % before any note or clock.
- Clock at 24 PPQN, 120 BPM: 48 bytes/s x 320 us = 15.4 ms/s = 1.5 % of the wire.
- A 7-bit controller that is only sent when its value changes can send at most 127 messages for a full sweep, whatever the speed. The danger is jitter (the value toggling by one) and 14-bit data (pitch bend: 16,384 values), so thinning is by value change AND by time.
- Example plan: 8 lanes, one value per 4 ticks at most (10.4 ms at 120 BPM): 8 x 96 = 768 messages/s x 0.64 ms (running status, same channel) = 49 % of the wire; x 0.96 ms = 74 %. Too dense for notes: default total cap about 300 CC messages/s on DIN (300 x 0.96 ms = 29 % of the wire, *estimate*, to tune on the unit); USB MIDI has much more room.
- A 6-note chord on DIN is 18 bytes = 5.8 ms (13 bytes = 4.2 ms with running status) for the last note, with or without this feature; USB has no such smear.

## Prior art
| Project | URL | Licence | Language | Last activity (GitHub API, read 2026-10-21) |
|---|---|---|---|---|
| Synthstrom Deluge firmware (community branch): parameters hold a node list with an interpolate flag; learned CC controls can be recorded as automation; takeover modes (jump, pickup, scale, relative) | https://github.com/SynthstromAudible/DelugeFirmware , https://delugecommunity.com/features/community_features/ , https://github.com/SynthstromAudible/DelugeFirmware/blob/community/src/deluge/modulation/automation/auto_param.h | GPL-3.0 (GitHub field; the LICENSE text was not read; "only" or "or later" not determined) | C++ (Renesas RZ/A1L target, not an MCU like ours) | pushed 2026-10-21 |
| Seq66: pattern editor with CC, plus a separate event editor, SMF import and export | https://github.com/ahlstromcj/seq66 | GPL-3 (`licenses/LICENSE.GPL` is the v3 text; GitHub reports no licence) | C++ | pushed 2026-10-21 |
| LMMS: automation clips with Discrete, Linear and CubicHermite progression, points in a sorted map keyed by tick | https://github.com/LMMS/lmms (header `include/AutomationClip.h`) | GPL-2.0 (GitHub field) | C++ | pushed 2026-10-19 |
| Ardour: automation lanes; Linear and Discrete modes; a `thin` call on an automation list (the example script says an area-below-curve threshold) | https://manual.ardour.org/mixing/automation/automation-modes/ , https://git.ardour.org/ardour/livetrax/raw/branch/master/share/scripts/s_thin_automation.lua | GPL (version not verified here; GitHub reports NOASSERTION) | C++ | pushed 2026-10-20 |
| OpenMPT: MIDI macros (Zxx) and plugin parameter control events in the pattern grid; BSD-3-Clause | https://github.com/OpenMPT/openmpt , https://wiki.openmpt.org/OpenMPT_Wiki:Books/Manual | BSD-3-Clause | C++ | pushed 2026-10-09 |
| Sagan Metro "Thin Continuous": two criteria, a value threshold (drop changes smaller than N) and a time threshold (drop data inside N ticks), to avoid MIDI buffer overflow in playback and to save memory | https://sagantech.com/support/Met00421.htm | proprietary, concept only | n/a | n/a |
| Cubase "Thin Out Data": reduces dense controller curves to ease the load on external devices | https://steinberg.help/cubase_pro/v13/en/cubase_nuendo/topics/midi_processing/midi_processing_thinning_out_controller_data_t.html | proprietary, concept only | n/a | n/a |
| Renoise: record a control into a graphical envelope or into effect columns; envelope values are applied per tick (a forum report notes the stepped result at few ticks per row) | https://tutorials.renoise.com/wiki/Graphical_Automation , https://forum.renoise.com/t/recording-a-slider-knob/71339 | proprietary, concept only | n/a | n/a |
| Dirtywave M8: a MIDI Out instrument with up to 10 CC numbers a track (concept only; from reviews) | https://www.soundonsound.com/reviews/dirtywave-m8-model02 | proprietary, concept only | n/a | n/a |
| MIDI 1.0 wire timing (31.25 kbaud, 10 bits a byte, running status) | https://www.lim.di.unimi.it/IEEE/MIDI/SOT5.HTM | specification | n/a | n/a |

Not found in this research: a published small-MCU sequencer whose CC thinning is documented; the Deluge forum has a user remark that automating many CCs "could overload the MIDI OUT buffer" (an opinion, not a measurement): https://www.elektronauts.com/t/synthstrom-audible-deluge-inc-open-source-development/29613?page=147 .

## Approach
inspire, written from scratch (the algorithms are a few lines each; no port needed).
- Record thinning, streaming, integer only: (1) a deadband on the 7-bit value (drop a change of 1 that returns to the old value within a tick: hysteresis); (2) a minimum interval per lane (1 tick by default); (3) at the end of the take one pass that drops a middle point when the line through its neighbours predicts it within 1 (the "co-linear" idea of Ardour's filtering; Douglas-Peucker is the textbook alternative, offline, with a bounded stack). All O(n); about 30 cycles a point *estimate*.
- Playback: points are stored sparse; between two points the lane interpolates linearly (one division per segment, then a Bresenham-style step emits a message each time the integer value changes, at most |v1 - v0| <= 127 messages), or holds (discrete) for switches such as sustain.
- Playback rate cap: a global budget of CC messages per second per port, a per-lane minimum interval, a per-block cap, a last-sent table (per port, channel and controller) so equal values are never sent, notes and clock first (`midi/out.h` already orders by due time; a priority class for controllers is new).
- Phase 0 without a format change: record the value into the existing step FX columns (CC A..E, slot 37; `seq/fx.h` `kFxInfo`) at step resolution, 2 per step. It is OG-exportable but coarse (8 per second at 120 BPM per column). Worth saying to the maintainer as the cheap first step.

## Cost on MK66 (estimate until measured)
CPU (see the table above):
- Playback: ~300 cycles a lane event in the render, +120 per byte in the DIN ISR: 0.38 % at wire saturation.
- Record, render side: parse about 20 cycles a byte (existing `CcParser`) + a ring push ~20 = ~80 cycles a message. Main loop side: thin and append ~150 cycles a message (1,000 messages/s = 0.15 M cycles/s = 0.08 %). An insert in the middle of a lane (overdub) moves up to n x 4 bytes: 1,000 points x 4 B = 4 KB at ~1 cycle a byte = 4,000 cycles = 22 us, main loop only.
- The existing path is too small for recording: the CC ring holds 32 words and the main loop takes 16 a pass of about 16 ms (`midi/cc_in.h`, `app/midi_input.h`): at most about 1,000 messages/s, and **no time stamp** (a CC is stamped only by the main loop's pass: 16 ms = 6 ticks at 120 BPM). Recording needs the render to stamp the position (a block is 1.45 ms = 0.56 tick) and a bigger ring (for example 256 words of 8 bytes = 2 KB in SDRAM) drained in bigger passes.
Memory (*estimate*):
- A CC point 4 bytes (tick u16, value u8, curve and flags u8); a 14-bit or note event 8 bytes. A smooth 8-second sweep after thinning: about 10 to 20 points *estimate* (to be measured from real takes), 80 bytes. 16 tracks x 4 lanes x 200 points x 4 B = 51 KB for a very busy pattern; a shared pool of 128 KiB to 256 KiB in SDRAM inside the project arena is enough.
- Raw record buffer before thinning: 8 bytes x 1,000/s x 60 s = 480 KB worst case in SDRAM (not needed: thin while recording).
- SRAM: per track a lane cursor (u16) and a last-sent byte per lane: about 16 x 4 x 3 = 192 bytes. Flash: record stamp + thinning + lane player + rate cap about 4 to 6 KB *estimate*. Lives in: core flash (the render and the project load are core). SD data pack: none needed. An SD plugin cannot play lanes (the render path); a later `.tmod` app could edit them only through a new ABI minor (published ABI: Level C).

## Fits
Layers: MIDI I/O (`midi/cc_in.h` stamp, `midi/out.h` priority class and rate cap), sequencer (`seq/`: a per-track lane cursor next to the step reader of `rows.h`; lanes drive MIDI output only, never audio voices, so no voice CPU is added), model (a lane arena next to `Pattern`; `Pattern` is 16 x 128 x 7 bytes = 14 KB of steps and has no room for it), `app/record.h` (the record sink gets a CC entry beside `rec.note_on`; the take stays one undo unit, as OS-LIVEREC does: with 1,000 messages/s the undo journal must NOT take one entry per message, because a take that has grown to half the journal is split: store the take as a lane snapshot; the pattern undo area is 160 KiB, `model/include/o3os/model/undo.h`, so a 4 KB lane snapshot fits about 40 times), formats (`docs/formats/o3-project.md`: an additive optional chunk, provisional name `MEVT`). Depends on: OS-LIVEREC, MIDI Learn (OS-013a), `MidiOut`.

## UX sketch
Rec + Play on a MIDI track: turn the encoder or a MIDI controller; the value shows in the track strip; Stop ends the take (one Undo removes it). A lane page per track (CC number, channel, a small curve on the FT812), with SMOOTH (interpolate or hold) and a DENSITY control (the rate cap). A pattern with no lanes costs nothing.

## Licence
The OS is GPL-3.0-or-later. Nothing needs porting. Ideas and published algorithms (deadband, time decimation, linear interpolation, Douglas-Peucker) are free to implement; do not copy source from the GPL projects above (Deluge: GPL-3.0, "only" or "or later" not determined, so mixing code could fix the combined licence to 3.0-only; LMMS GPL-2.0 is not compatible with GPL-3.0-or-later code if it is "only"). OpenMPT (BSD-3-Clause) could be ported with attribution, but there is nothing needed from it. Closed products: concepts only.

## Risks / open questions
- **Project format change = Level C** (version bump, backward-compatible loader, round-trip test; maintainer decides). An additive chunk is carried as an unknown chunk by an older build only up to 16 chunks and 16,384 bytes in all (shared with `OGPN`, up to 12,822 bytes: `docs/formats/o3-project.md` "Unknown chunks"), so an older OS may drop the lanes and, after a save, lose them.
- **The OG `.mt` export loses the lanes.** Options: drop with a notice, or bake to CC FX at step resolution (lossy). Maintainer decision.
- **DIN bandwidth**: see the table; needs a rate cap, last-sent table, notes first, a bounded chase at loop restart.
- Time grid: lanes use ticks of 192 PPQN on the nominal grid; swing, track length, backward and ping-pong modes and song order change the mapping (what a lane does when its track plays backwards is undecided).
- Which channel and port: in this tracker the instrument is chosen per step, so a lane needs its own channel and port (or follows the last instrument of the track).
- Record sources: the Tracker's own controls need a binding "this knob sends CC A..F of the track's instrument" (`seq/midi_route.h`, `midi_cc[]`); is it wanted? MIDI thru while recording: undecided.
- 14-bit pairs (CC 0..31 with 32..63) and RPN/NRPN: later, as a lane type.
- Chase at a jump (Stop, locate): resend only changed values, limit per block.

## Bench items for the next hardware session
1. B1: render cycles per lane event with 1, 8 and 16 events in a block (`diag cpu` / bench image), with the heavy song running.
2. B2: DIN saturation: CC at 1,041 messages/s plus notes and clock; note lateness and `staging_overflow`.
3. B3: CC in at 1,000 and 1,500 messages/s for 10 s: `dropped()` counts of the current ring (shows the need for the bigger ring).
4. B4: USB MIDI out at 16 events a frame: cost (expected 3.6 %) and late events.
5. B5: SDRAM read cost of a sequential lane scan while the sampler runs.
6. B6: capture real knob and pad-pressure takes (amidi) and measure the thinned point count.

## Priority
⭐4: the most useful of the three (movement over time on external synths is what a tracker lacks), cost is negligible in CPU, and a phase 0 exists that needs no format change; held back from 5 by the Level C format decision and the DIN rate cap work.
