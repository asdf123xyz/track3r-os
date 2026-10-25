# ADR 0004 — Step format: note, instrument, volume, 2 FX (the OG file layout) + engine-defined FX

Status: **Accepted** (maintainer, 2026-10-10) · Source: `docs/design/product-ux.md` §2, §8 Q2.

- **Decision.** Each step holds note, instrument, volume and two FX columns of (type byte, value byte).
 - FX types 0–42 keep the OG type numbers and value bytes (PAR-003); how each effect sounds is this OS's own model, and whether it matches the official firmware is an open research question.
  - Types 128–255 are declared by the instrument's engine through the module ABI (ADR 0002). For example, the 303 module declares Accent and Slide. The UI shows engine-declared names and ranges from the module's parameter table.
  - Types 43–127 are reserved for future core FX.
- **Consequences.**
  - OG/1.9 patterns import losslessly.
  - A step whose FX type belongs to an engine that is not loaded is preserved and shown as unknown; it is never dropped.
  - Unknown types are untrusted input (PAR-003 rules).

## Amendment 1 — core engines and new core FX (2026-10-11, maintainer-delegated)
- **Core engines declare engine FX too.** The engine-FX window 128–255 belongs to the instrument's engine, whether it is an SD module (ADR 0002) or a core engine compiled into flash (ADR 0013: sampler family, Amiga/Paula). Core engines use the same parameter-table mechanism. The Amiga/MED engine declares 128–191 as `128 + MED command`, holding MED commands verbatim (`docs/formats/med.md` §4).
- **New core FX** from the reserved 43–127 range:
  - 43 Speed (ticks per row);
  - 44 Tempo fine;
  - 45 Order jump.
  
  Export → OG drops them and lists them in the drop report.

## Amendment 2 — the tracker FX (OS-COREFX, 2026-10-14, maintainer-delegated)
- **New core FX** from the reserved range, contiguous after amendment 1 (43 Speed, 44 Tempo fine, 45 Order jump). Slice CF1 of `docs/specs/OS-COREFX.md` assigns five (46..50); CF2 six more (51..56, below) and Channel pan (57, CF2 fold); CF3 five more (58..62, below).

  | id | FX | value | source trackers |
  |---|---|---|---|
  | 46 | Arpeggio | `xy`: the note, then +x and +y semitones on the next two ticks, repeating | MOD `0xy`, S3M `Jxy`, XM / IT `0xy` |
  | 47 | Volume slide | `xy`: from tick 1 of the row, x up else y down, in steps of 1/64 | MOD `Axy`, S3M `Dxy` |
  | 48 | Fine volume slide | `xy`: once on tick 0, x up else y down, in steps of 1/64 | MOD `EAx` `EBx`, S3M `DFy` `DxF` |
  | 49 | Fine porta up | quarter Paula period units, once on tick 0 | MOD `E1x` (4 x), S3M `FFx` (4 x), `FEx` (x) |
  | 50 | Fine porta down | the same, down | MOD `E2x` (4 x), S3M `EFx` (4 x), `EEx` (x) |
  | 51 | Pan slide | `xy`: from tick 1 y right else x left, 4/256 of the range a tick; `xF` fine left x and `Fy` fine right y, once on tick 0 (libopenmpt's PanningSlide) | S3M / IT `Pxy`, XM `Pxy` |
  | 52 | Vibrato waveform | `0..7`: 0 sine, 1 ramp down, 2 square, 3 random, +4 no retrigger (the OS has the first four as triangle, reverse saw, square, random LFO shapes; "no retrigger" is not modelled) | MOD `E4x`, S3M `S3x`, XM `E4x` |
  | 53 | Tremolo waveform | the same for the tremolo | MOD `E7x`, S3M `S4x`, XM `E7x` |
  | 54 | Glissando | `0` off, else on: the Glide and Slide ramps of the track move in whole semitones | MOD `E3x`, S3M `S1x` |
  | 55 | Retrigger | `xy`: restart the sample every y ticks (Scream Tracker's count), after changing the volume by the code x | S3M `Qxy`, XM `Rxy` |
  | 56 | Tremor | `xy`: x + 1 ticks on, y + 1 off | S3M `Ixy`, XM `Txy` |
  | 57 | Channel pan | `0..100`: the track's absolute pan (50 the centre), held until another Channel pan, a pan slide that moves it, or the transport start (not row-scoped; in this OS's model the Panning FX 31 ends with its step) | MOD `8xx` `E8x`, S3M `Xxx` `S8x` |
  | 58 | Channel volume | `0..64`: the track's level scale (64 full), held like 57 until another Channel volume, a channel volume slide that moves it, or the transport start or stop; multiplies the note's own volume (a strip gain, not the voice's volume) | S3M / IT `Mxx` |
  | 59 | Channel volume slide | `xy`: from tick 1 y down else x up, 1/64 a tick; `xF` fine up x and `Fy` fine down y, once on tick 0 (libopenmpt's ChannelVolSlide; note that the plain form goes down first) | S3M / IT `Nxy` |
  | 60 | Global volume | `0..64`: every track's level scale on top of its channel volume, held like 58 (one value for the whole song) | S3M `Vxx`, XM `Gxx` (IT `Vxx` is 0..128: no importer yet, see the spec) |
  | 61 | Global volume slide | `xy`: from tick 1 x up else y down (the opposite order to 59), 1/64 a tick; `xF` / `Fy` fine, once on tick 0 | S3M `Wxy`, XM `Hxy` |
  | 62 | Panbrello | `xy`: x the speed, y the depth (a zero nibble keeps the track's last): the pan swings by libopenmpt's S3M panbrello (64-point sine, `(sine x depth + 2) / 8` of 256 a tick, the position advancing by the speed, kept over rows and notes) while the rows carry it, added to the pan in force | S3M / IT `Yxy` |

- **Row-scoped.** A tracker effect lasts the row it stands in (a MOD or S3M effect does); the sequencer consumes it at the start of the row (kind Row) and sends the voices a `TrkFx` event, so a row without one carries none over and the importer writes no terminator. This differs from this OS's Voice FX and from the Amiga engine window, which hold until their column changes.
- **No persistent layout change.** The ids were reserved and inert; a pattern file carries the two FX bytes of a step verbatim as before and an older OS keeps them inert. **Export to OG (maintainer decision 2026-10-14, OS-COREFX):** amendment 1 is accepted and the exporter does it: `step_to_og` writes every FX type 43..127 as `(0, 0)` and counts it in `ExportDrops::core_fx` (one per FX column), as it does for the engine window 128..255 (`engine_fx`); `any_lost()` is true for it. Types 0..42 are written byte for byte. A native `.o3pj` keeps all the bytes. An OG file whose FX bytes lie in 43..127 therefore no longer comes back identical through OG -> model -> OG (the types the OG itself defines still do).
- **CF2 (2026-10-14).** Ids 51..56 are Row FX like 46..50 (one `TrkFx` event each). 52, 53 and 54 set a state of the track that the following rows keep (the TrkFx that ends them ends nothing); 51, 55 and 56 run on the Tick events of the rows that carry them, and their counters (the retrigger's, the tremor's position) are kept by rows that do not. The **Panning FX (31)**, which the OS did not act on before CF2, now acts on the Modern engine's tracks in this OS's own model (whether the official firmware's Panning FX behaves the same is an open research question): its value 0..100 is the track's ABSOLUTE pan on the graph's strip (it replaces the instrument's offset: a MOD / S3M import hard-pans its channels with the strip), applied at once and at each note while it is held, and ended by the sequencer's ModEnd (the next note step without it, FX Off 01, another FX in its column), which gives the instrument's pan back. **57 Channel pan** (CF2 fold) is what a tracker's pan is: `0..100` (50 the centre), the track's absolute pan, set at its row and held until another Channel pan, a pan slide that moves it or the transport start, over notes (MOD `8xx` `E8x`, S3M `Xxx` `S8x`; one `TrkFx` event, Row kind, the processor holds the pan). The Amiga engine takes the retrigger as MED `1F y` (the volume code has no MED counterpart) and ignores the other FX of 51..56; 57 moves the strip, on any engine. The OG exporter drops 51..57 with the rest of 43..127 (counted in `ExportDrops::core_fx`, no conversion to 31: the lifetimes differ).
- **CF3 (2026-10-14).** Ids 58..62 are Row FX like the rest (one `TrkFx` event each). 58 and 60 are set once and held (the TrkFx that ends the row ends nothing, as for 57) and reset when the transport starts and, new in CF3, when it stops, so that a song that ends at global volume 0 does not leave the pads silent; 59, 61 and 62 run on the Tick events of the rows that carry them. The volumes are a gain on the track's strip (`Graph::set_track_gain`, channel volume x global volume, linear, in the strip's per-block level smoother), the panbrello an offset on the strip's pan law (`Graph::set_track_pan_mod`). Values: 58 and 60 are `0..64` in the table (a value above 64 is held to 64); an importer ignores an S3M `Mxx` / `Vxx` above 64 as libopenmpt does.
- **Tick semantics** follow the source trackers (slides on ticks 1.., fine slides on tick 0); the project-level slide-on-tick-0 switch stays an open maintainer decision (`docs/formats/mod.md` section 4), not part of this amendment.
