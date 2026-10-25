# OS-MODPITCH: a pitch-slide path for module instruments (ABI 1.8, Level C, additive)

BACKLOG MOD-ABI (OS-MODPITCH + OS-INSTUI S10), approved 2026-10-16, Level C (published ABI), size M. Written 2026-10-16 from `dev`. **New Community OS behaviour.** No persistent project or settings format change; the `.tmod` file header is unchanged and the descriptor grows only for modules that declare minor 8.

Base: `dev`. `O3M_ABI_MINOR` is 7 there (ABI 1.7 came with OS-H4 and is merged). MOD-ABI takes **1.8**, one bump shared with OS-INSTUI S10. Whichever of the two commits lands first bumps the constant and writes the 1.8 entry in the `o3module.h` history. The second only appends to it. If another slice takes 1.8 first, MOD-ABI takes the next free minor. Code refers to the constant, so only the docs are renumbered.

## Goal and findings
Glide (19) and Slide up / down (41 / 42) reach sampler voices only. `VoicePool::fx_event` keeps one pitch ramp a voice, in 1/65536 semitone, walked once a block. `Engine::dispatch` never forwards `ModSet` / `ModEnd` to `ModuleVoices`. Module instances have no pitch input at all.

**Arp 21 may already work on modules.** `Sequencer::pfx_note` and `arp_next` send ordinary retriggering NoteOn events on any instrument. They need a Chord FX in the other column, otherwise `fx_ignored` counts it and nothing happens. The OS-FXMODULE stems may have had no Chord FX. Step 1 of the work is therefore a host test of the event stream (Arp + Chord on a module slot). If it passes, Arp needs no pitch path and QA-MOD-099 is corrected. Arp stays retriggered notes either way (no legato arp).

Other pitch FX today:
- **Tune 40** shifts the note in the sequencer, so it already works on modules.
- **Chord** is the root alone (one voice per instance).
- **Micro-tune 34, Finetune LFO 33, tracker FX 46, 49, 50** are sampler or Amiga only.

The new path adds these later with no ABI change, because the host sums them into one offset.

## Decisions
| Topic | Decision |
|---|---|
| Who ramps | **Host.** It owns tempo, glide time and the previous-note memory. The module only follows a number. |
| Call | New descriptor member `void (*pitch)(void* state, float semitones, float per_frame)`, render context. From the next frame the offset is `semitones` and moves by `per_frame` each frame until the next `pitch()` or `note_on()`. The offset is relative to the note given to `note_on`. A module may ignore `per_frame` (staircase of one block). |
| Guarantees | Finite, \|semitones\| ≤ 128, \|per_frame\| ≤ 128. A `note_on` resets the offset to 0, then the host calls `pitch()` at once if the new note starts on a ramp (Glide starts at prev − note). The module still clamps its frequency. |
| Ramp law | Same as the sampler: linear in semitones, Q16 integer, Slide wins over Glide, lands on the target at a block end (as the sampler does, 1 block of quantisation). A shared pure helper, pinned against `VoicePool` by a test. |
| Glide | The FX only (modules have no GLIDE field). Time = `glide_fx_ms(value)`, 0 = none. A repeat of the same note does not glide. The memory is the last module note on the track. |
| Slide | Ramp 0 to ±value/16 semitones in one step at the tempo in force (576 ticks). It holds until the note or the FX ends. A step with no note slides the sounding note from where it is. ModEnd and FxReset snap to 0 with one call. |
| Legato (maintainer Q5 b, 2026-10-16) | A Glide or Slide note on a module is legato, as a TB-303 slide: the envelope is not restarted, only the pitch moves. The host does not release the held note first; it sends `note_on(note, O3M_NOTE_LEGATO \| O3M_NOTE_RAMP)` and then `pitch()`. With no note sounding on the instance (first note, or after a release) the note starts normally, flags `O3M_NOTE_RAMP` only, and the ramp still runs. A Glide or Slide note on the same note as the sounding one is a tie: no retrigger and no movement (orchestrator default). Sampler voices keep retriggering as today. |
| Host ramp vs module slew | The host ramp is the only slide on a pattern note, so the pitch never slides twice. `O3M_NOTE_RAMP` tells the module that the host drives this note's pitch: the module keeps its envelope when `O3M_NOTE_LEGATO` is also set, jumps its base pitch to the new note at once, applies no portamento of its own, and follows `pitch()` exactly until the next `note_on` without `O3M_NOTE_RAMP`. A module's own portamento (tb303 `Slide` and `Legato Slide`) applies only to live legato keys (`O3M_NOTE_LEGATO` without `O3M_NOTE_RAMP`). A flag is used instead of relying on the call order, so a module decides at `note_on` time (orchestrator default). On a pattern note the slide time comes from the Glide FX value or the Slide FX speed, not from the module's `Slide` parameter. |
| Legacy | `pitch` NULL (any module of minor ≤ 7, or a 1.8 module that leaves it NULL): the host starts no ramp and makes no call. The FX are ignored as today. |
| Descriptor size gate | `struct_size` must equal the size of the declared minor's descriptor (64 up to 1.7, 80 from 1.8), else `BadDescriptor`. Members past offset 64 are read and validated only when the declared minor ≥ 8. `o3m_gen.py` and the templates write the size through a new `O3M_MODULE_SIZE(minor)` macro instead of `sizeof`. |

## ABI changes (one minor, 1.8)
Offsets are for the 32-bit target. All additions are appended to `o3m_module` (today 64 bytes, `view_count` at 60).

| Offset | Size | Member | Item | Meaning |
|---|---|---|---|---|
| 64 | 4 | `pitch` (function pointer) | MODPITCH | NULL = no pitch input |
| 68 | 4 | `pages` (`const o3m_page*`) | S10 | NULL = pages "1-8"... as today |
| 72 | 4 | `page_count` | S10 | 0..4, zero exactly when `pages` is NULL |
| 76 | 4 | `labels` (`const o3m_labels*`, `param_count` entries parallel to `params`) | S10 | NULL = no labels |
| 80 | | `sizeof(o3m_module)` | | was 64 |

New `note_on()` flag, from 1.8: `O3M_NOTE_RAMP 2u`, sent only to modules that export `pitch` (so never to a module of minor 7 or lower). The comment of `O3M_NOTE_LEGATO` ("a pattern's note never carries it") gains "until 1.8: a Glide or Slide note on a module with `pitch` carries it together with `O3M_NOTE_RAMP`".

New structs: `o3m_page {const char* name; uint32_t first_param;}` is 8 bytes. `o3m_labels {const char* const* names; uint32_t count;}` is 8 bytes.

Limits (published, see Q3):
- At most 4 pages, matching F1..F4, and at most 8 parameters a page.
- A page name is at most 7 characters (`O3M_PAGE_NAME_MAX` 8 with the NUL).
- Labels only for `O3M_PARAM_INT` parameters.
- `count` equals max − min + 1 and lies in 2..16.
- A label is at most 11 characters (`O3M_LABEL_NAME_MAX` 12).

Loader validation (`image.cpp` `read_desc`, `ModuleSet::add_native`), bounded by 32 parameters × 16 labels:
- `pitch` must be code inside the image, or NULL.
- `pages[0].first_param` is 0, the entries ascend strictly, and each page holds 1..8 parameters. Together the pages cover `param_count` exactly, and `param_count` 0 means no pages.
- Every pointer resolves into the image and every string passes `string_ok`.
- A label entry is non-zero only for an INT parameter with the right count.

The file header is unchanged. Tool changes:
- `o3m_gen.py` and `o3m_pack.py` write minor 8 only when the manifest asks for `pitch`, `pages` or `labels`, and use the size macro. Plain modules keep their old minor, so they still load on an old OS.
- The packer's `DESC` size follows the declared minor.
- The `module.json` manifest gains `"pitch": true`, `"pages": [{"name", "first"}]` and a per-parameter `"labels": [...]`.

The host reads pages and labels in the main loop only, straight from the loaded image. `ParamsScreen::module_page_count`, the F-key labels (today `kModulePages[]`) and the enum value text switch to the tables when they are present. App tables (`o3m_read_api` and the rest) are untouched.

## Host behaviour
- `Engine::dispatch` forwards the Slide and Glide groups of `ModSet` / `ModEnd` and `FxReset` to a new `ModuleVoices::fx_event`. This is one call in the `else` branch that already calls `voices_->fx_event`, so about 16 B of SRAM_L.
- `play_block` also passes the tempo to `ModuleVoices` (next to the existing `voices_->set_tempo_x100`).
- Everything else is flash code (`O3OS_AUDIO_COLD`): FX state, ramp start, ramp advance.
- Per track: the FX on/off bits, the slide distance, the glide value and the last note (about 70 B).
- Per instance: the offset, target, samples left and a mode byte (16 B).
- A module note on a track takes the instance over as today, and the ramp state is reset with it.

## Built-in modules
| Module | Change | Cost |
|---|---|---|
| chip | `pitch` stores the offset and slope. The offset is added to the semitone sum already computed at control rate before `exp2_f`. | no extra `exp2`, about 40 B hot |
| tb303 | `pitch` sets the frequency to `note_frequency × 2^(offset/12)` with one `exp2_f`, straight, bypassing its slew (`slide_d`) while `O3M_NOTE_RAMP` is in force. `note_on` with `O3M_NOTE_LEGATO \| O3M_NOTE_RAMP` keeps the envelope and jumps the base pitch; its own `Slide` / `Legato Slide` stay for live legato keys only. | about 60 B hot, one `exp2_f` a call |
| fm | `pitch` recomputes `base_inc` with one `exp2_f` and refreshes the four operator increments. A per-sample slope is optional; the first cut is a staircase. | about 100 B hot, check for zipper by ear |

Each built-in declares minor 8, `pitch` and (S10) page names and labels.

## Real-time safety and CPU
- No allocation, no heap, no `double`: Q16 integers, a float slope, one float division a block per ramping instance. Q16 to float is exact (< 2^24). Bounded by `kMaxModuleInstances` (8).
- Idle cost: one byte test per instance segment, about 3 cycles.
- Ramp cost: at most one call per ramping instance per block, plus one per `note_on` or FX event. Host side about 50 cycles. Module side about 300 cycles for tb303 and fm, near 0 for chip [est].
- Worst case: 8 instances ramping in one block is about 2.4k cycles, about 0.9 % of the 261k-cycle block [est]. A typical case is under 0.2 %.
- `pitch()` runs inside the `enter()` / `leave()` fault hooks and inside the governor's window.
- Budget: `.ramfunc_l` +about 50 B against the 864 B left (hard cap 120 B). **Corrected when built**: the 864 B was stale, dev had 24 B of the 27 KiB hot budget left; `ModuleVoices::start_note` moved to flash and `.ramfunc_l` fell by 288 B (`docs/architecture/cpu-budget.md`). Flash +about 1.5 KB for the host. SRAM_U +about 90 B for `ModuleVoices`; check its allowance in `app_main.cpp` (`g_module_voices`). Slab: each module's hot part grows by the bytes above (about 8 KiB used of 14 KiB). No SDRAM change.

## Risks (Level C)
- A published ABI is frozen. Semantics must be right the first time: the unit, the relative-to-note rule, the "until the next call" rule and the ±128 range.
- Rebuilding a module against the new header with a hand-written `sizeof(o3m_module)` and an old minor is refused. This is deliberate and documented in the SDK README and `module-sdk.md`. Existing binaries are unaffected.
- A third-party module that ignores `O3M_NOTE_RAMP` and keeps its own slew would slide twice. The SDK rule, the README and the tb303 oracle test pin the expected behaviour.
- A staircase may zipper on a fast octave glide on FM. Check it by ear.
- Two features share one bump but land as separate commits. Only the first bumps.
- OS-MODMX's planned `set_mod` (cpu-budget M5) needs the next minor after this one, not this one.
- Notes on a track that alternates sampler and module instruments do not glide across (separate memories).

## Done when
- `O3M_ABI_ASSERT` lines for the 1.8 offsets and sizes in `o3module.h`. The layout test in `tests/module_image_test.cpp` ("the SDK's layout is what the format says") extended, with a literal copy of the 64-byte 1.7 descriptor.
- A real 1.7 `.tmod` (64-byte descriptor, minor 7) still loads and plays. With Glide / Slide steps its audio is bit-identical to the same song without them.
- Image / loader refusal tests: size 64 with minor 8, size 80 with minor 7, `pitch` not code, unsorted or oversized pages, first page not 0, a label count that does not match, a label on a non-INT parameter, a string outside the image, pages without a count.
- `tests/module_voices_test.cpp` with a recording module:
  - Slide 16 reaches +1 semitone after one step at 120 BPM, within a block.
  - Glide 60 to 67 with value 10 starts at −7.0 and lands on 0 after 200 ms.
  - Slide beats Glide; ModEnd, FxReset and a tempo change behave as described.
  - At most one call per instance per block, and none when nothing is held.
  - Never NaN or out of range.
  - The ramp matches the `VoicePool` ramp for the same events.
- Engine test: a sequencer Slide / Glide / Arp + Chord song reaches the module (event stream). A Glide or Slide note on a sounding instance arrives as `note_on(LEGATO | RAMP)` with no `note_off` before it; on a silent instance as `note_on(RAMP)`; a same-note Glide sends no `note_on`; a plain note after it still releases the held note first. A module of minor 7 never sees `O3M_NOTE_RAMP` or a pattern `O3M_NOTE_LEGATO`.
- tb303 oracle: with `LEGATO | RAMP` the envelope does not restart and the frequency equals the host ramp at every block (no slew lag); a live legato key without `RAMP` still slides with its `Slide` parameter.
- Oracle tests for chip, fm and tb303: `pitch(0, 0)` is bit-identical to not calling it, and `pitch(12, 0)` sounds an octave up.
- `o3m_gen.py` and `o3m_pack.py` self-tests for minor 8, the size macro and the manifest keys.
- S10: goldens of the tb303 page with named pages and a labelled INT parameter. A legacy module still shows "1-8".
- Sim: no scenario for the pitch path (modules do not run audio in the simulator). An existing `qemu_module_*` run may add a Slide.

## Hardware-only checks (HIL steps in `qa_modules.json`)
- `diag mod` / `diag cpu` min / mean / max per instance on the unit: tb303, fm and chip with a held Slide, against the same without. Accept under 1 % of the block.
- By ear: Slide up and down and Glide on tb303, fm and chip, legato (no envelope restart, a single slide); an octave glide on fm (zipper); Arp with Chord on tb303 (does it already work); page names legible on the key tile.

## Docs to update
- ADR 0002: amendment for 1.8 (shape, gating, limits). The `o3module.h` history entry.
- `docs/guides/module-sdk.md`, `module/sdk/README.md` (new members, size macro, the "use `pitch` or ignore it" rule), `docs/specs/OS-INSTUI.md` §S10 (built note), `docs/manual/module-instrument-fx.md`.
- QA-MOD-099 (from OS-FXMODULE; update it once that branch has merged): the limit is removed, or Arp corrected. New rows per FX for tb303, fm, chip and legacy.
- `docs/architecture/cpu-budget.md`, BACKLOG rows MOD-ABI / OS-FXMODULE, `module/CLAUDE.md` if the macro rule needs a line.

## Maintainer answers (2026-10-16)
| Question | Answer |
|---|---|
| Q1 Shape of the published pitch call | (a) Host ramps, `pitch(state, semitones, per_frame)`, offset relative to the note. |
| Q2 Where the new members live | (a) Appended to `o3m_module` (64 to 80 bytes), size gated per minor. |
| Q3 Limits written into the ABI | (a) 4 pages x 8 parameters, names <= 7 characters, labels on INT only, 2..16 values, <= 11 characters. |
| Q4 If Arp already reaches modules | (a) Correct QA-MOD-099 and the manual, no Arp work. |
| Q5 Glide / Slide note on a module: legato? | (b) Legato: the envelope is not restarted, only the pitch moves (TB-303 slide). Replaces the earlier default "no legato". |

Orchestrator defaults, not asked:
- Strict descriptor size per minor.
- Glide by FX only.
- A same-note Glide or Slide is a tie (no retrigger, no movement).
- `O3M_NOTE_RAMP` as a flag rather than a call-order rule.
- Micro-tune 34 and tracker FX 46 / 49 / 50 later, on the same offset.
- `pitch()` kept cold (SDRAM) unless the unit measurement says otherwise.
- Names are 7 characters.

## Files
- `module/sdk/o3module.h`
- `module/src/image.cpp`, `module/include/o3os/module/image.h`, `module/src/loader.cpp`
- `audio/src/module_voices.cpp`, `audio/include/o3os/audio/module_voices.h`, `audio/src/engine.cpp`
- `app/src/screens/params_screen.cpp` (S10 pages and labels)
- `tools/o3m_gen.py`, `tools/o3m_pack.py`, `modules/{chip,fm,tb303}`

## Built defaults, to confirm by the maintainer (MOD-ABI, 2026-10-16)
Where the spec was silent the engineer took these (each: default, to confirm by the maintainer):
- The first Glide note of a track (nothing to glide from) is an ordinary note (flags 0, retriggers).
- A Slide FX with value 0 is no slide: the note is an ordinary note; on a sounding note a Slide of 0 returns it to the note's pitch.
- Page names and labels of the built-in modules are in capitals (the OS shows them as given); the header's PAGE field counts the module's pages, `P1`..`Pn` (maintainer, 2026-10-16), the key row and the pane heading carry the name.
- The TB-303 voice has one new member, `note_hz` (`tb303_dsp.h`), so the bytes of its state moved: the QEMU golden of `module_builtin_run_test.cpp` (output and voice bytes) changed 0x22675067 -> 0x8ad564f3, and the output alone is pinned as its own golden (0xf910ea28). Evidence that the sound is unchanged, on the target: the ceec397 tb303 module (ABI 1.2) run through the same test gives 0x22675067 and 0xf910ea28 exactly (a native old-versus-new run of the scripts also agrees, a weaker scope: other host services). MOD-ABI was merged without a QEMU run; `ci.sh qemu` is part of the gate for a module-DSP change.
- Chip `state_bytes` 192 -> 208 (the offset and its slope are in the voice).
- The built-in modules stay version 1.0.0 (now ABI 1.8): an older card copy of the same version never replaces them.
- The tempo for a Slide is passed by `Engine::play_block` (`ModuleVoices::set_tempo_x100`) and read when the ramp starts.
- The TB-303 declares no labelled INT (Waveform is a float blend, Legato Slide a bool): the S10 golden uses a native "Named Synth".
- `kRampLimitQ16` in `audio/pitch_ramp.h` is declared (the ABI's +-128 semitone limit) but not used yet: `ModuleVoices::pitch_call` clamps the numbers it passes.
- Known hazards of the built behaviour (recorded, not fixed; by ear or on the unit): (1) a Glide or Slide **tie** on the sounding note sends no `note_on`, so a new velocity or accent on that row is not applied; (2) a Slide `ModSet` and a ramping note in the same step make two `pitch()` calls in one block, the last wins (`tests/module_pitch_test.cpp`); (3) the chip's ramping `note_on` calls `control()` once to put the new base pitch in at once, so its vibrato, arpeggio and sweep advance one control period (a few samples) at that note; (4) a ramping note that starts after the last stretch of a block (`ahead` 0) passes the offset with no slope and the next block walks on (finite numbers, tested).
- **Latent (ADR 0014, not touched here)**: `ModuleUi` fills the app SDK's `param_desc.group` as `index / 8` (`app/src/module_ui.cpp`, the module branch of the app SDK's `get_param_desc`); with named pages that no longer matches where a page starts (Chip: 0 / 8 / 12 / 17). An app that reads `group` sees the old eight-a-page grouping until the SDK reports the page table.
- A labelled INT may start above 0 (FM Algorithm 1..8): `core::param_desc_valid` takes an Enum whose names count from `min` (`list_count == max - min + 1`; `enum_name`, `param_step`, `param_parse` already used `min`), pinned by `tests/param_test.cpp`, `tests/module_ui_test.cpp` (every built-in parameter is described) and `tests/module_image_test.cpp`.
