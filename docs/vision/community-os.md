# Community OS — product and architecture vision

Status: vision and decision record distilled from the maintainer's design discussion (2026-10-10). Nothing here is implemented. Hardware capabilities marked **verify** are claims to confirm against datasheets and measurement before design depends on them. This document drives Phase 2 (re-architecture); Phase 1 (1.9 parity) must not pre-empt it.

## Product goal
Keep the speed and personality of the Tracker OG, and turn it into a modern tracker / sampler / synth designed around what this hardware can actually do — not "the 1.9 firmware with more options". The official manual and release notes (as mapped in `docs/ui/official-manual-map.md`: documented, not verified) and the OG file formats are the compatibility reference; the new engine and UI become our own.

Rule: no feature ships because "it fits". Every idea goes through the radar matrix (below) and must earn its CPU, RAM, flash and UX cost.

## Language and tooling decisions
- **Firmware: C++** (C++17 once the toolchain profile allows; currently gnu++14). The open-source ecosystem for this hardware (Teensy core/Audio, FT812/SGTL5000 drivers, nearly all reusable DSP: Open303, MoogLadders, pt2-clone…) is C/C++. Rust on MK66 would mean rebuilding HAL/USB/audio/linker for no musical gain.
- **Host tools: Python today, Rust allowed** for PC-side tools (project converters, sample processing, `.ptf` packaging, test harnesses, a companion editor). Isolated modules may cross a C ABI later if one clearly benefits.
- **Desktop simulator is a first-class target**: the same core/UI code compiled for PC, an 800×480 window, keyboard mapped to pads/buttons/encoder. Iterate UI and logic on PC; use hardware only for audio, timing and I/O.

## Target architecture
```
HAL            MK66 · FT812 · SDRAM · SDHC · USB · MIDI · SGTL5000 · Si4703 · pads/LEDs/encoder
CORE           clock · events · filesystem · project · undo · settings
AUDIO ENGINE   voice pool · sampler · synth engines · mixer · modulation · FX · USB multichannel
SEQUENCER      tracker patterns · song · phrases · automation · MIDI · probability/conditions
UI ENGINE      renderer · widgets · pages · theme · layout · focus · overlays
FEATURE MODULES chops · granular · 303 · wavetable · Amiga · MOD/XM/IT/MED import · …
```
Principles: model → controller → view separation (UI never owns state); engines and FX cost CPU only when active; audio has absolute priority over UI; every hardware touch goes through the HAL so the simulator and host tests can stub it.

### Signal routing matrix (central design element)
```
SOURCES: samples · synths · line L · line R · mic · radio · USB in · FX return
            │
       ROUTING MATRIX
     ┌──────┼──────────┐
   tracks  sends     recorder/resampler
     │       │            │
   mixer   FX buses    sampler (new instruments)
     ├──► USB stems  ├──► SD stems  └──► physical L/R (stereo, or 2× mono buses)
```

### Instruments are not samples
Pattern note → instrument → engine (sampler / slices / granular / wavetable / 303 / VA / FM / drum) → modulation (LFOs, envelopes, velocity, MIDI CC, aftertouch) → FX → output. Example: instrument "ACID" = 303 engine, mono, ENV1→filter, CC74→cutoff, CC71→resonance, drive + delay send.

### UI engine
- Own small renderer over FT812 display lists (no LVGL/Qt): `ui.beginFrame(); panel/text/knob/waveform/pattern/meter…; ui.endFrame();` with widgets, layout, clipping, focus, themes on top.
- Renoise-inspired philosophy, not its look: high information density, fast navigation, visible context, few modal screens; a persistent top/bottom band (transport, BPM, pattern, CPU, instrument, status); SHIFT+button expands a zone.
- Refresh policy: static UI redraws only on change; meters/scopes 20–30 Hz; playhead 30/60 Hz depending on load; the FT812 is used for big waveforms, envelopes, animated LFOs, filter curves, chop views, modulation feedback.

## Hardware opportunities (verify each before relying on it)
| Capability | Claim | Use |
|---|---|---|
| Stereo line-in | SGTL5000 delivers 2 I²S input channels; how the official firmware uses them is an open research question; I²S in/out run simultaneously — **verify** | dual-mono inputs, full-duplex recording, live input instrument, external FX loop |
| Codec DSP | SGTL5000 driver exposes 5-band GEQ, 7 parametric biquads, AVC, bass enhance, surround, ADC HPF, ADC→DAC routing — **verify** what runs in-codec | master EQ/width/bass with ~0 MCU cost |
| FM radio | Si4703 with seek, RSSI, RDS station name/text — **verify** | radio harvest, RDS-named samples, RSSI entropy |
| USB | Teensy 3.6 device port is full-speed (12 Mbit/s): 8×16-bit/44.1 kHz ≈ 706 B/ms fits one isochronous endpoint (≤1023 B/ms) — tight; the MK66 also has a high-speed USB port — **verify** whether it is wired on this board | USB stems out, stereo return, USB sampling, multiple MIDI ports |
| SDRAM code | Community work executes code from SDRAM (around `0x70100000`) — **verify** speed vs flash/SRAM; copy hot loops to SRAM | engines/importers as loadable modules from SD, freeing the 1 MiB flash |

## Feature radar (initial)
License column matters: the firmware's own license is not decided yet (open question), so GPL code is "study/reference only" until it is.

| Idea | References | What we'd take | License note | Viability |
|---|---|---|---|---|
| MIDI CC in / MIDI Learn / mod matrix | OpenMPT, Renoise (concepts) | universal CC/NRPN → any parameter, learn mode | own code | very high, ~0 CPU |
| Sample chops | Amigo (commercial, product reference), MilkyTracker, Renoise tools | equal / transient / zero-cross / manual slicing, 2–64 slices on pads, per-slice start/end/pitch/reverse/loop/vol/pan/filter; analysis offline at load | own code | very high |
| USB stems + return | Teensy USB audio | 8 tracks out, stereo in, MIDI | Teensy core (MIT-style) | very high, bandwidth-bound |
| Realtime resample / bounce / freeze | — | any track/submix/master/input → new instrument, auto-slice; frees CPU | own code | very high |
| Routing: dual-mono in, FX loop, main+cue | maintainer request history | L/R as independent channels with sends; OUT R = send, LINE IN = return, latency compensation; L = main, R = cue | own code | very high |
| ProTracker / Amiga playback mode | 8bitbubsy pt2-clone | Paula-like resampling, 8-bit quantisation, A500 RC + LED filters, hard pan | BSD-3 (verify) | high |
| TB-303 engine | Open303 (MIT core); JC-303 extensions GPLv3 | mono saw/square, env, resonant filter, accent, slide via tracker FX | use Open303 core only | high |
| Moog/ladder filters | MoogLadders (per-file licenses: Unlicense/ISC/BSD…) | selectable ladder variants, benchmark on MK66 | pick permissive files | high |
| Filter set | Q DSP, Surge (concepts) | SVF LP/HP/BP/notch, LP12/24, comb, drive | own/permissive | high |
| MED/OctaMED import | libxmp (MIT since 2025), OpenMPT MED loader (BSD) | MMD0–3 parser → offline conversion to internal model | MIT/BSD | high/medium |
| MOD/XM/IT import | libxmp, OpenMPT | offline conversion | MIT/BSD | medium |
| Amiga sampler workflow | Ami-Sampler-VST (GPLv3) | decimation, A500 filter, voice modes | reference only | medium |
| Vocoder | — | mic modulator, 8–12 bands to start, carrier = wavetable/saw/303 | own code | medium (CPU) |
| Audio → modulation | — | envelope follower (cheap), transient trigger, mono pitch detect (moderate) | own code | high |
| Radio harvest | — | scan → capture N bars → auto-chop → pads; random-station collage | own code | high, unique |
| Chip/FM engines | Furnace (GPLv2+) | ideas for 2-op FM, SID-ish, NES/GB/AY, noise drums | study only | medium |
| MPE input, MIDI processor | — | per-note expression; arp/chord/strum/scale/probability/Euclid before MIDI OUT | own code | high |
| SD-loadable modules | RETracker precedent | `/modules/*.tmod` loaded to SDRAM on demand | own ABI | high impact, needs research |
| Logical tracks | — | 12–16 tracks typed Audio / MIDI / External Audio over a voice pool | own design | high |
| Convolution reverb, 64-voice synth | — | — | — | rejected (CPU/RAM) |

## Phased plan (overall)
| Phase | Goal | Result |
|---|---|---|
| 0 Observation | record what the release notes and the official manual document about the firmware 1.1.14 → 1.9 (documented, not verified; an observation counts only with its evidence) | parity catalogue (largely done) |
| 1 Own build + parity | build our own firmware; reach the documented 1.9 behaviour (documented, not verified) | `.ptf` of our own; parity rows closed |
| 2 Clean HAL | isolate all hardware | independent drivers |
| 3 Simulator | core/UI on PC | fast iteration |
| 4 Audio engine v2 | voice pool + modular mixer | base for new engines |
| 5 UI engine v2 | widgets/layout/themes | new GUI |
| 6 Compatibility | load OG projects | ecosystem preserved |
| 7 Modern I/O | MIDI learn, USB MIDI/audio | DAW integration |
| 8 Sampler v2 | chops, streaming, Amiga | sampler leap |
| 9 Synth engines | 303, wavetable, FM, drums | groovebox/synth |
| 10 Advanced sequencing | phrases, conditions, modulation | new musical language |
| 11 Optimisation | profiling, ARM DSP, flash/RAM | squeeze the MK66 |
| 12 Polish | recovery, updater, testing | usable firmware |

Before Phase 4 an exhaustive design document is written and the maintainer signs off on the open decisions below.

## Open design decisions (maintainer)
1. Keep exactly 8 audio tracks, or logical tracks + voice pool (and how many)?
2. FX commands: keep two-digit commands? How do new functions coexist with compatibility?
3. Can each instrument switch engine? Per-voice vs per-track vs send FX?
4. How many modulators, and which parameters are automatable / MIDI-learnable?
5. Exact definition of a "slice" and its persistence.
6. Undo model (scope, memory budget in SDRAM).
7. What happens to 1.9 projects — load natively, convert once, or both?
8. MIDI Learn semantics: scope (global/project/instrument), pickup vs jump, persistence.
9. Firmware license (determines whether GPL references like JC-303 extensions, Ami-Sampler or Furnace can be ported rather than only studied).
10. Module ABI for SD-loaded engines: versioning, symbol resolution, where hot code runs.
