# OS-MODMX — Modulation matrix per instrument

Status: designed 2026-10-14; M1–M7 accepted (maintainer, 2026-10-14, together with the review folded into M3–M5 and section 6); nothing implemented · Follows OS-INSTUI S6 (the MOD page; the matrix is its second page) · Classification: **new Community OS behaviour** (our own design; the official manual does not describe a modulation matrix, `docs/ui/official-manual-map.md`) · Amends ADR 0013 D6 on two points (M1, M2): the amendment is written with MX1.

## 0. What exists (verified 2026-10-14)

- **ADR 0013 D6 (accepted)**: modulation per voice; 4 LFO + 4 envelopes + sources (velocity, key, macro, CC, bend, aftertouch, input follower) → 16 routes per instrument, evaluated per voice at block rate (64-frame blocks, 690 Hz) with per-sample ramps; hook points pitch, cutoff/resonance, amp, pan, position (start / wavetable / granular), sends, insert and module parameters flagged `modulatable`; the six fixed modulators of an OG instrument file map in. Same in `docs/design/product-ux.md` §7 D4 and line 210 ("Mod matrix · Instrument").
- **Not implemented.** The voices run six fixed modulators (the layout of the OG instrument file), one per destination, each an ADSR or an LFO (`og_samples.cpp:250-272`, `voices.h:142`). The descriptor flag `kParamModulatable` is set on the tables (`params.cpp`) and mapped from `O3M_PARAM_MOD` for modules (`module_ui.cpp:168`); nothing reads it.
- **MIDI Learn** exists (`LRNB`, modes pickup / jump / relative, action → `ParamChange`): it *moves the stored value*.
- **Parameters reach the render** through the `ParamChange` ring drained in the graph's pass (after the block's voices), with ramps (gain, pan, sends per sample; cutoff / resonance coefficients every 16 samples).
- **Modules**: one instance per instrument slot; `set_param(id, value)` runs in the audio context and the OS does not clamp (`o3module.h:332`).
- **Voices**: 12 (`voices.h:214`). **Budgets** (STATUS.md): SRAM_L hot slab about 1–2 KB of room, SRAM_U about 33 KB over its floor: the matrix's runtime state goes outside the hot slab unless a measurement says otherwise.
- **Format**: older readers keep a chunk they do not know as an unknown chunk (`docs/formats/o3-project.md:78`).

## 1. Decisions proposed to the maintainer

- **M1 Two layers.** Layer 1 = the six fixed modulators of the OG instrument file (OS-INSTUI S6: per destination OFF / ENV / LFO), meaning unchanged, oracle-tested, OG import exact. Layer 2 = the matrix, added on top. *Amends ADR 0013 D6* (the fixed modulators of an OG file map in): this OS's per-destination semantics (amount scaling per destination, kill level at a retrigger, the cutoff envelope only with the filter on) stay bit-exact without the matrix having to emulate them.
- **M2 Matrix generators: 2 LFO + 2 ENV per instrument** (*amends* the ADR's 4 + 4); raised after a CPU / SRAM measurement if wanted. Stateless sources come on top (section 2).
- **M3 16 routes per instrument** (as the ADR). A route is SOURCE → DEST, AMOUNT −100..+100 %, optional VIA (a second source that scales the amount: the mod wheel sets the vibrato depth). A new route's AMOUNT starts at +50 % (audible at once).
- **M4 Place**: Instrument screen, **F4 pressed again** cycles MOD → MATRIX → GEN (the "press again flips the page" convention, product-ux line 44). The F4 label carries the page (`MOD 1/3`, `MOD 2/3`, `MOD 3/3`) and the header names it; three pages is the ceiling (the cycle only goes forward: Shift+F4 is DELETE).
- **M5 A CC source is picked with the Learn gesture** (FX1 + FX2 on the SOURCE cell, then move the controller). Learn on a Params row keeps meaning "move the value"; on a matrix row it means "modulate". A CC can also be dialled with the encoder (number and channel) without a controller at hand; on the matrix the footer says `LEARN = MODULATE`.
- **M6 VEL> (OS-INSTUI S9) becomes a route** (VELOCITY → VOLUME) when this item lands. Until then VEL> is a stored field: INST 1.3 carries it as its twelfth byte (continuous: 100, the default, is the keyboard's previous v/127 law, 0 ignores velocity; `docs/formats/INST-1.3-diff.md`). When the matrix lands, the loader converts the VEL> byte of a 1.3 instrument into a VELOCITY → VOLUME route with the equivalent amount, under a new INST version with a round-trip test, as the format rule asks (a changed meaning of a field is a version bump with a loader for the old one); the 1.3 byte is then read as the older form and no longer written.
- **M7 Modules**: destinations are their parameters flagged `O3M_PARAM_MOD`, per **instance** (one per slot; the OS keeps no per-voice state for a module); the OS clamps to the descriptor range before `set_param`; per-voice sources follow the instance's last note. No ABI change.

## 2. Sources (v1)

| Source | Value | Scope | Notes |
|---|---|---|---|
| VELOCITY | 0..1 | voice, at note on | pads have no velocity sensing (product-ux line 42): a pad note is 1.0 |
| NOTE (key track) | −1..+1 | voice | 0 at middle C (the device's MIDDLE C setting), ±1 at ±5 octaves |
| RANDOM | 0..1 | voice | drawn at note on |
| MOD LFO 1 / 2 | −1..+1 | voice | shapes sine, triangle, saw up, saw down, square, S&H, smooth random; rate free (Hz) or synced (the 29 rates of the OS-INSTUI LFO page); retrigger at note on or free running; start phase |
| MOD ENV 1 / 2 | 0..1 | voice | attack, decay, sustain, release, loop; this OS's existing envelope generator as the host oracle |
| CC n (channel c) | 0..1 | MIDI channel | at most 8 distinct CCs referenced in the project (a table of 8 slots filled by the MIDI input path) |
| PITCH BEND | −1..+1 | MIDI channel | |
| AFTERTOUCH | 0..1 | MIDI channel | channel pressure |

Later, not v1: poly aftertouch, the input follower (line in), a step-FX value (a tracker-native source: an FX sets a per-track value that routes read), Perform macros, the layer-1 modulators as sources.

## 3. Destinations (v1)

- Every descriptor flagged `kParamModulatable` that has a hook in the voice, plus module parameters flagged `O3M_PARAM_MOD`. Hooks: PITCH (±24 semitones at 100 %), FINE (±100 cents), VOLUME, PAN, CUTOFF, RESONANCE, WT POS, GRAIN POS, START (read at note on), DELAY SEND, REVERB SEND, OVERDRIVE. A test fails if a flagged descriptor has no hook.
- **Scale rule**: 100 % = the whole range of the descriptor on its own curve (the normalised 0..1 position the page's bar shows), so one rule fits every parameter, module ones included; PITCH and FINE are in semitones and cents.
- **Sum**: value = clamp(base + layer 1 + Σ routes) on the normalised curve, then the existing ramps and coefficient refresh (no zipper noise).

## 4. Engine (real-time, Level C)

- Once per block per sounding voice (at most 12): advance the voice's generators by one block; for each route of its instrument (at most 16): `src × amount (× via)` into the voice's accumulator for the destination's hook. Bounded loops (12 × 16), no allocation, no SD, no blocking.
- **Route tables**: the engine's own compact copy for the 64 instruments (about 64 × 16 × 4 B = 4 KB, SRAM_U or SDRAM after measurement), updated from the main loop with a single-writer protocol (route messages through the `ParamChange` ring, bounded per block, or a per-instrument double buffer with a generation; the engineer picks and documents it).
- **Per-voice state** (2 LFO, 2 ENV, random, accumulators): about 12 × 96 B ≈ 1.2 KB, outside the hot slab unless measured otherwise.
- **CC / bend / aftertouch**: the 8-slot CC table and per-channel bend / pressure, written by the MIDI input path, read at block start.
- **Modules**: once per block per instance, `set_param(id, clamp(base + offset))`, only when the value moved.
- **CPU estimate** [est]: 12 × 16 × ~25 cycles + 12 × 4 × ~40 for the generators ≈ 7 k cycles of the 261 k a block has ≈ 2.7 %. Measured with `diag cpu` before and after (the `rt-optimize` guidelines: measure first); the OS-GOV governor is unaffected. Budget row 3 % and the protocol: `docs/architecture/cpu-budget.md`; not merged until measured on the unit. A routed module parameter must not cost a full `derive()` per block (cpu-budget M5).
- Host oracle tests: route math, VIA, clamping, the scale rule, LFO shapes and synced rates, ENV against this OS's existing envelope generator, the CC table.

## 5. Model, undo, format (Level C: persistent format)

- **Model**: a `ModMatrix` side table in the Project, one entry per instrument slot: 16 routes of 8 B (source u8, source arg u8 (CC number, generator index), channel u8 (CC: 0 any, 1..16), dest u16 (instrument field id), amount i8, via u8, via arg u8) plus the generators (about 48 B): about 176 B a slot, 11 KB in all, with the project in SDRAM. **The `Instrument` struct does not change** (its static_assert'd layout and the arena size stay).
- **Generator parameters are descriptors** (a new instrument field range, like `kModuleFirst`): the generated page, Learn and undo work without new code. Every route edit goes through the Editor (journalled, undoable).
- **Chunk `MODM` 1.0**, after `SNFX`: written only for slots with at least one route or a generator not at its default, so a project without routes is the file it was. Older builds keep it as an unknown chunk. Read strictly like the OS-004d chunks, except that a route with a source or destination this version does not know (a later version's) is dropped and counted, not `Malformed` (the INST engine byte's tolerance). Round-trip and old-file tests; `docs/formats/o3-project.md` updated; the maintainer sees the format diff before the merge.
- OS-INSTUI interplay: INIT clears the routes and resets the generators; DELETE drops them; COPY / PASTE carry them; a `.pti` cannot hold them (SAVE says "MATRIX NOT IN .PTI").

## 6. UI (review, accepted by the maintainer 2026-10-14)

- **MATRIX page** (F4 a second time): the routes in use plus one `+ ADD` row, not 16 rows of dashes; the header says `ROUTES 3/16`. Columns SOURCE · DEST · AMOUNT · VIA in fixed character columns, as on Pattern.
- **Cell editing**: Up / Down a row, Left / Right a column, the encoder edits the cell; Shift + encoder: AMOUNT ±10, SOURCE / DEST jump a group. Delete on VIA clears VIA, on AMOUNT resets it to +50 %, elsewhere clears the row. Copy / Shift + Copy copy and paste a route row like a step (the engineer checks both are free in the Instrument context). F8 UNDO, Shift + F8 REDO.
- **Long lists**: while the cursor is on SOURCE or DEST, the right-hand panel becomes the scrolling list with the current entry highlighted (no overlay to dismiss). DEST entries carry a group prefix (`VOI PITCH`, `FLT CUTOFF`, `SND DELAY`, `SMP WT POS`; a module's parameters by name). SOURCE: FX1 + FX2 then a controller move fills `CC 74 CH 1` (M5), or dial it.
- **GEN page** (F4 a third time): the generators' rows; Left / Right picks LFO 1, LFO 2, ENV 1, ENV 2 (as the MOD page picks a destination).
- **MOD THIS = Insert** on any Params row (Insert is unbound in the Instrument context: `bindings.cpp:534-546`): creates a route with that destination and lands on its SOURCE cell.
- **Feedback**: a modulated row on any page shows a `~` and a thin second bar with the live value; in the matrix, a one-character live meter in the SOURCE cell. The right panel: on SOURCE, the source's shape with a moving dot; on DEST or AMOUNT, the destination's bar with the base value, the **reach bracket** (base ± amount) and the live value; on VIA, the via level. It is labelled `LAST VOICE C-4` (the engine exports a few floats for the voice played last, single writer), so it is clear why it stops when nothing plays.
- **Footer**: one line naming the keys acting on the cell.
- **Avoid**: modal dialogs, hiding rows mid-list, refusals where replace-with-undo works, toasts for non-events, long lists without a group jump, a gesture that changes meaning without a visible label, page cycles longer than three.

Mockups (review; 800 × 480, about 80 columns of text):

```
| INS 07 KICK · SAMPLE · ONE SHOT · MODERN       MOD 2/3 MATRIX   ROUTES 3/16  |
+--------------------------------------------------------+---------------------+
| #  SOURCE          DEST          AMOUNT         VIA     | LFO 1   2.3 Hz  TRI |
| 1  LFO 1       ~|  FLT CUTOFF   [    |====  ] +45%  --  |   /\      /\        |
| 2  VELOCITY     |  VOL VOLUME   [    |======] +80%  --  |  /  \    /  \  o    |
| 3 >CC 1 CH 1   ~|  FLT CUTOFF   [  ==|      ] -30%  AT  |      \  /    \ /    |
| 4  + ADD                                               |       \/      \/    |
|                                                        | DEST FLT CUTOFF     |
|                                                        | BASE 62  REACH 32-92|
|                                                        | [   ==|====   ] ~71 |
|                                                        | LAST VOICE  C-4     |
+--------------------------------------------------------+---------------------+
| SOURCE: ENC PICKS · SHIFT+ENC GROUP · FX1+FX2 LEARN=MODULATE · DELETE CLEARS |
| SAMPLE | VOICE |FILTER |MOD 2/3| SENDS |  <06  |  08>  | UNDO  |
```

The F1 GRAIN page (OS-INSTUI S3) with a modulated row:

```
| INS 07 PADVOX · SAMPLE · GRANULAR · MODERN               GRAIN               |
+--------------------------------------------------------+---------------------+
|  PLAY MODE    GRANULAR                                 |  ...||||||||||...   |
| >GRAIN POS    38 %     [=======|          ] ~          |  ..||||[####]|||..  |
|  GRAIN LEN    120 ms   [====|             ]            |  grain window       |
|  GRAIN SHAPE  GAUSS    [  /\  ]                        |  POS LIVE ~ 44 %    |
|  GRAIN LOOP   FWD                                      |  2 ROUTES -> POS    |
|  PLAY ENGINE  MODERN                                   |  LAST VOICE  C-4    |
+--------------------------------------------------------+---------------------+
| 0..100 % · DEFAULT 0 · CC 74 · INSERT: MOD THIS · DELETE: DEFAULT            |
| GRAIN  | VOICE |FILTER |MOD 1/3| SENDS |  <06  |  08>  | UNDO  |
```

## 7. Slices

| Slice | What | Level |
|---|---|---|
| **MX1** | Model side table, `MODM` chunk, editor actions, undo, round trip | C |
| **MX2** | Engine: per-voice routes, stateless sources (velocity, note, random, CC, bend, aftertouch), built-in hooks, `diag` dump, CPU measured | C |
| **MX3** | MATRIX page, Learn on SOURCE, MOD THIS, live markers | B |
| **MX4** | Generators LFO 1–2 / ENV 1–2 (engine + GEN page) | C |
| **MX5** | Module destinations | B/C |

Order: after OS-INSTUI S6. HW checks: `diag cpu` with 12 voices × 16 routes, zipper-free sweeps by ear, CC latency from an external controller.
