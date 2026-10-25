# OS-002 — Extension architecture (ADR 0009 rev 1)

Status: 002a merged 2026-10-11; 002b specified (after the OS-003b merge); 002d implemented (see its last section) · Source of truth: `docs/adr/0009-extension-architecture.md` (Mechanisms + Revision 1). This spec only fixes scope and acceptance per slice.

## 002a — Actions, gestures, bindings
**Scope:**
- **Actions** (`app/actions/` or `core` where layer-pure). `ActionId` (stable, `constexpr` hashed name or enum), `ActionArg` (int32), flags (`undoable`, `repeatable`, `needs_focus`), and a static registry of `{id, name, handler, flags, enabled(context)}`. Handlers live in feature files (`pattern_actions.cpp`, `transport_actions.cpp`, `nav_actions.cpp`) and operate on the model plus a small `ViewState`.
- **Gesture layer.** A stateful recogniser from timestamped `InputEvent`s to `Gesture{kind: tap|hold|double_tap|hold_encoder|chord, input, modifier}`. All timings live in one table; it is deterministic, and the time source is the event timestamps, never a wall clock.
- **Binding resolver.** Three layers (user > preset > built-in) and a context chain (overlay → mode → screen → global). Overlays may capture. Built-in bindings are a `constexpr` table, conflict-checked at compile time where possible and by a test otherwise.
  - User and preset layers are loaded from data into a fixed pool. The loader only parses a text keymap buffer; it does no SD I/O.
  - `InputKind` values are reserved for PcKey, Hid and MidiNote/MidiCc; they are not implemented.
- **Rewire the skeleton.** `App` no longer switches on buttons. Every input goes HAL event → gesture → binding → action. Existing behaviour stays: arrows, encoder and F-key labels, Fx toggling FX1/FX2, pad → instrument. FX1+FX2 chord → `learn.toggle`, which shows a "LEARN" status for now (ADR/maintainer decision).
- **Scenario.** Add the command `action <name> [arg]`. Existing scenarios keep passing with **unchanged goldens**, which proves behaviour is preserved.

**Done when:**
- Clean host build with ASan/UBSan, ctest green, 0 warnings.
- MK66 build has 0 warnings, and sizes are reported per library.
- Unit tests cover: gesture timing edges (tap vs hold threshold, double-tap window, hold+encoder), resolver precedence (layers × context chain, overlay capture), conflict detection, and action dispatch with args.
- The layer checker passes.
- `app.cpp` has no `switch` on button codes.
- Reviewer approves.

## 002b — Screens, one overlay, notifications
Builds on the OS-003b merge (themes, faces, tiles, components). New screens use only the OS-003b toolkit and theme tokens.

**Scope:**
- **Screen interface and registry** (`app/screens/`).
  - `Screen` has `enter`, `leave`, `render(const Model&, const ViewState&, ui::Renderer&, ui::Rect covered)` and `context()`, plus a stable `ScreenId`.
  - There is no `on_action`. Actions stay in feature files (ADR 0009 rev 1).
  - Screens are static instances. The registry is a `constexpr` table of `{id, name, Screen*}`, and a test checks that ids and names are unique.
  - Adding a screen means one class, one registry row and its default bindings, and this is documented in `app/README` or the style guide.
- **Navigation.**
  - `ViewState` holds one base `ScreenId` and an optional overlay.
  - The action `nav.screen <id>` switches the base screen and calls `leave` then `enter`. Pressing the current screen's button again cycles its pages. Bindings come from the screen buttons in `docs/design/product-ux.md` §3.
  - The pattern view becomes the first registered screen, with unchanged goldens apart from intentional ones that are listed.
- **Per-view modes and navigation keys** (maintainer, 2026-10-11: OctaMED-style jumps; "complex navigation and key combinations depending on a mode in the view, for all views").
  - Each screen declares its modes as the "mode" level of the context chain. Pattern has Edit, Select and Browse (Follow was retired in OS-004d). Bindings may differ per mode.
  - Navigation is made of plain actions with an `ActionArg`, so any gesture can call them (tap, hold, double tap, chord, hold+encoder, Shift):
    - `cursor.jump_fraction n` jumps to n/4 of the pattern or list, n = 0..3, and `cursor.jump_end` to the end;
    - `cursor.page ±1`;
    - `cursor.track ±1`;
    - `cursor.home` and `cursor.end`.
  - **Device bindings:** Shift+F1..F4 = 0/4, 1/4, 2/4, 3/4; Shift+F5 = end. Check them against the existing Shift bindings and record any conflict test.
  - **OctaMED/ProTracker PC-keyboard layer:** F6..F10 are reserved for the same actions in the keymap format, implemented with the OctaMED keyboard layer later.
  - Song and Navigator use the same jump actions.
  - Document in the style guide how a view adds modes and bindings: one enum, one table row each.
- **Navigator overlay** (maintainer, 2026-10-11: "a navigation window between views").
  - Holding Shift+Pattern (or the bound gesture) opens a grid of every registered screen: name, icon and the screen button that reaches it. The grid is generated from the registry, so new screens appear automatically.
  - Arrows or the encoder choose; releasing the gesture, or F1, opens the chosen screen; Back cancels.
  - Screens not implemented yet are shown disabled.
  - Uses the overlay slot and the covered rect like Learn.
- **Second screen: Song.**
  - Shows the order list from static demo data: position, pattern number, pattern name and length, the play position marker, and the cursor.
  - Uses the Data face and tiles, and the Sel and Accent tokens like the pattern grid.
  - Encoder and arrows move the cursor; editing the order is out of scope.
- **One overlay slot with a covered rectangle.**
  - An overlay declares its `ui::Rect`. The base screen gets it as `covered` and skips any row or component that lies fully inside it. Partly covered items are drawn and then painted over by the overlay.
  - The overlay context comes first in the binding context chain, and it may capture so that unbound inputs do not fall through. This uses the resolver from 002a.
- **Learn overlay.**
  - The FX1+FX2 chord (maintainer) is rebound from the 002a status text to opening the Learn overlay.
  - The overlay shows a title, "move a control or send a MIDI CC", the target placeholder "—" (parameters arrive in 002d) and the exit hint.
  - It captures input. The chord again or Back closes it.
- **Notifications** (`app/notify` or core where layer-pure).
  - A bounded queue of 4 entries `{severity info|warn|error, text FixedString<48>, source, ttl_ms}`. On overflow the oldest info entry is dropped first; an error is never dropped for an info entry.
  - Time comes from `Clock::uptime_ms` and is deterministic in the simulator. The TTL counts only while the entry is shown.
  - When the overlay slot is free, the head entry is drawn as the notification overlay: one Label/Data line with a severity tile, in a strip above the soft keys.
  - A modal overlay such as Learn takes precedence, and notifications wait in the queue.
  - The F-key placeholders (002a) post an info notification instead of writing to the status line.
- **Simulator.**
  - Add scenario `sim/scenarios/screens.txt`: Pattern → Song → Pattern, quarter jumps in Pattern and Song, the Navigator choosing Song, open and close Learn, a notification shown and then expired after its TTL (snap before and after), and a queue overflow.
  - Gallery and budget states:
    - Song typical and worst;
    - Pattern + Learn;
    - Pattern + Navigator;
    - Pattern + notification;
    - Song + notification;
    - each in all three themes.

**Done when:**
- The host build with ASan/UBSan passes ctest with 0 warnings. The MK66 build has 0 warnings, with sizes per library and the delta against the OS-003b merge.
- Unit tests cover:
  - registry uniqueness;
  - navigation calling `enter` and `leave` in order;
  - overlay capture versus fall-through;
  - covered-rect skipping, by counting words with and without the overlay;
  - queue overflow policy and TTL expiry with a fake clock;
  - the chord opening and closing Learn.
- Every gallery state is at most 1792 words in every theme, including base + overlay. Display-list overflow still aborts the frame.
- The maintainer views the gallery for all three themes.
- The layer checker passes: screens stay in `app`, and there are no colour literals.
- The reviewer approves.

**Out of scope:** song editing and the real song model (OS-004), parameter targets and storing bindings in Learn (002d), MIDI input, page cycles beyond Song and Pattern.

## 002c — Undo journal and cooperative jobs
Builds on the OS-002b merge. Model: OS-004a (`model`, mutation API with generations).
- **Undo journal** (`app/undo` or `model` if layer-pure):
  - A fixed-record ring in a region of the model arena; ADR 0013 D7 budgets 384 KiB.
  - It is written only by `undoable` actions as model-level patches: old and new values per mutation. A multi-step edit is grouped as one undo unit.
  - Undo and redo restore exactly, through the same mutation choke point, so generations bump and screens redraw.
  - When the ring fills, the oldest units are dropped, and the depth is reported.
  - Macro recording replays `(ActionId, arg)` from the same log; record and play are actions only, with no UI yet.
  - Undo of pattern edits and song edits: the release notes mention 10 song-undo levels (PAR-068) and the official manual up to twenty pattern edits (`docs/ui/official-manual-map.md`); neither is verified. Ours is bounded by bytes, not levels, and must offer at least 10 song-edit levels.
  - Bind F8 UNDO in Pattern and Song; Shift+F8 is redo.
- **Job runner** (`core` or `app`):
  - `Job::step(budget_us) → Running | Done | Failed`, with cancel and progress (0..1000).
  - A pool of 4 static slots, run from the main loop with a per-frame budget. Time comes from `hal::Clock`, which is deterministic in the simulator.
  - A progress notification or overlay is shown through the 002b notification path.
- **First real job: WAV importer** (`formats/wav`):
  - Reads PCM 8/16/24-bit and float32, mono or stereo, any rate, from a `ByteSource`.
  - Converts to int16 into a caller-provided sample buffer (the model's sample arena slot), with dither when going 24 → 16, and keeps the original rate as metadata. Resampling is out of scope; the engine plays at the native rate.
  - Parses `smpl` loop points and `cue` markers as slices.
  - Hostile input is treated as in `docs/formats/og.md` (hostile-input section).
  - Resumable through the 004a reader pattern. It is registered in the format registry if OS-004b has merged; otherwise register it when both are in.
  - Builds the `ui::plot` peak cache cooperatively as a second job, if OS-003d has merged.
- **Done when:**
  - Tests cover:
    - undo/redo exactness under random action sequences, as a property test against a model snapshot;
    - ring overflow;
    - grouping;
    - job budget adherence and cancel;
    - WAV variants and hostile WAVs;
    - a scenario that imports a synthetic WAV through the job runner with a progress notification.
  - There is a QEMU suite for the pure parts.
  - Host and QEMU ctest pass with 0 warnings, MK66 sizes are reported, and the reviewer approves.

## 002d — Parameter descriptors, ParamChange, generated edit page
Builds on 002c (or in parallel if files do not overlap).
- **Descriptors:**
  - Each automatable value has a `ParamId` (stable 32-bit: owner kind, index, field) and a `constexpr` descriptor: range, default, unit, curve (lin/log/exp/table), display formatter (%, seconds, enum names for the rows that exist; Hz, dB and note names when a row needs them), step, coarse step, and modulatable or not.
  - Instrument parameters, track mixer parameters and device settings are covered. Settings are parameters with `scope ∈ {device, project}` (ADR 0009 rev 1).
- **Generations:** a counter per parameter, which screens poll on redraw. There are no observers.
- **ParamChange:** an SPSC ring towards audio, `{ParamId, value, ramp}`, drained in the graph's pass, after the block's voices (by a **fake audio consumer** in the 002d tests, because the engine is OS-005). It also feeds the MIDI Learn target model: Learn binds a CC to the parameter under the cursor and stores the binding in the device or project scope, with pickup mode (maintainer answers).
- **Generated edit page:**
  - One screen, "Instrument PARAMS", generated from descriptors: a field list with name, value, unit and a bar, using `ui` components and `ui::plot` mini-curves for envelope and filter parameters.
  - Navigation through the 002b cursor interface; the encoder edits with coarse and fine steps (Shift).
  - Every edit goes through actions, so it is undoable through 002c.
  - It must be within budget in all themes, with gallery states.
- **Settings persistence:** versioned chunks in the native format (ADR 0007), with default-fill for unknown or missing data. The device scope lives in a settings file (format spec in `docs/formats/`).
- **Done when:**
  - Tests cover descriptor tables (ranges, defaults, display formatting round trips), generation semantics, the ParamChange ring under load, the Learn binding store, settings round trip with default-fill, and edit-page goldens.
  - The usual gates pass.

### 002d as delivered (2026-10-11)
Byte-level specification of the parameters, the settings, the Learn rules and both file formats: `docs/formats/o3-settings.md`; the code tour is `docs/architecture/os-tree.md`, "Parameters". Decisions taken inside the slice that a reviewer should check:
- **One `ParamChange`.** `core::ParamChange {ParamId, value, ramp_samples}` and `core::ParamRing` replace OS-005a's placeholder `audio::ParamEvent` (the engine drains the same type, the same 256 / 64 bounds). `ramp_samples` 0 = the engine's own smoothing (descriptor flag `smooth`).
- **Descriptors live in two layers**: `core/param.h` is the type and the pure maths (curves, steps, display text, parsing), `model/params.cpp` the tables with their storage offsets, so `audio` and `ui` never depend on the model. A parameter is read and written through its table row (`param_load` / `param_store`), by `Project::set_param` and through the Editor's journal (op `set_param`, 40 bytes), so undo is one choke point.
- **Settings are `model::Settings` blocks** (values, generations, Learn store) of scope Project (inside the `Project`, persisted as the native `SETS` / `LRNB` chunks) or Device (the `O3ST` file); same two chunks, one codec. Reading is default-filling by design; a project that uses neither chunk is byte-identical to before.
- **Two Learn stores by the maintainer's split** (device: Track, Master, Device parameters; project: Instrument, Project), pickup by default, jump and relative per binding, a `follow` flag (the selected instrument) as data without UI. `midi.cc` is the one action MIDI input will enqueue; it binds while the Learn overlay is up and moves what is bound otherwise.
- **`kActionUndoableIfOpen`**: an action that is undoable when a project is open but still works on device settings without one.
- **`Cursor::count` takes the `ViewState`**: the pages of the generated page have different lengths. A pad selects the instrument on the PARAMS page, as on Pattern.
- **Goldens**: 7 existing ones changed on purpose, because INSTRUMENT is no longer a disabled Navigator cell (`gallery/overlay_pattern_navigator_*`, `screens/navigator_*`) and the "not available" scenario now presses SAMPLE EDIT; every other golden, the Learn overlay without a target included, is unchanged.
- **Sizes**: `kModelArenaBytes` 1,905,112 -> 1,918,360 (+13,248: 3,224 generation counters, the epoch, a 348-byte settings block); MK66 image text +22,224 B, bss +5,320 B (the shared 4.5 KB `CurveScratch` of the plots).
- **Review fixes (same day)**: the instrument defaults, the descriptors' and `init_instrument`'s, are this OS's default instrument parameters (volume 50, cutoff 1.0, envelope amount and sustain 1.0, release 1000 ms, volume envelope on with a 10 ms attack, the others off at 3 s; the 9 `params` gallery pictures and 10 scenario pictures that show a default were regenerated, no persisted format changed: the default project is still 186 bytes); SNAP TO ZERO left the project settings (an unsourced default must not sit behind "omit when default"); Learn state is forgotten on any generation change and keyed by parameter (an edit made elsewhere is never "crossed" by a knob), and by `reset_history()`; `midi.cc` is not recorded in macros; the chunk caps are 64 and a newer minor above them is read as far as known; `ParamField` is a handle to the table's descriptor, one decode serves `find_param` and `param_desc`, the unused Hz / dB / note / ms displays were dropped, and `Settings` has room for 16 fields (arena +40 B).
Open items: provisional defaults (NUMBERING, MIDDLE C), SNAP TO ZERO (not a project setting until sourced), the relative CC encoding, the clock sync range (PAR-119), undo granularity of knob sweeps (one unit a message), whole-object undo gives the engine no values (`param_resyncs`), Learn range scaling, a UI for follow bindings, the LFO block and the envelope `loop` flag, the rest of the default instrument parameters (LFO amount, loop points); the MK66 image has no document yet, so its page shows the defaults.
