# ADR 0009 — Extension architecture: actions, bindings, screens, formats, parameters

Status: **Accepted** (maintainer requirement 2026-10-10: input/output formats, editing screens, controls, functionality and key assignments must be easy to add; the design is the maintainer's). Next work item: OS-002.

## Principle — standalone first, SD extends (maintainer, 2026-10-10)
The device works fully without an SD card. Every screen, layout, theme, font, keymap and core engine has a compiled-in default in flash. Special files on the SD only **add or override**: modules (ADR 0002), extra themes and layouts, keymaps and presets. A missing, corrupt or incompatible SD file, or a crashing module, falls back to the built-in default with a notification. It never leaves the device unusable. The same validation (format version, display-list budget, binding conflicts) applies to built-in data and to SD data.

## Constraints
No heap after boot, no RTTI or exceptions, nothing virtual inside sample loops (ADR 0005, rt-optimize). The mechanisms are static `constexpr` tables, plus data loaded from the SD into fixed pools. Virtual interfaces are allowed at screen, format and module granularity.

## Mechanisms
1. **Actions (Command).** Every user function is a named action (`ActionId` + handler + an "enabled in context" predicate), listed in one registry. Inputs, menus, MIDI and scripts all invoke actions; no input handler calls feature code directly.
2. **Bindings (data-driven keymap).** A `(context, input chord) → ActionId` table, where the input chord is a button, a combo (e.g. FX1+FX2 = `learn.toggle`), a pad, the encoder, a PC key, a HID event, or a MIDI note/CC.
   - The defaults are compiled in. User and preset keymaps (e.g. OctaMED) are SD data packs (ADR 0002 tier 0), validated on load.
   - Conflicts are reported, never silently overridden.
3. **Screens (State, with a screen stack).** Each screen is a `Screen` with `enter / leave / on_action / render(model) / context()`.
   - A registry holds the screens. Navigation pushes and pops them; overlays such as Learn and popups stack on top.
   - Adding a screen means one class, one registry entry and its default bindings.
4. **Formats (Strategy + registry).** `Importer` / `Exporter` interfaces are selected by extension or magic bytes and work on byte streams through the storage HAL.
   - Built in: OG (M1 modules, ADR 0006), native (ADR 0007), WAV.
   - XM, IT, MED and MOD are importer/exporter modules from the SD (ADR 0002, kind `importer`/`exporter`).
   - Every format is pure and fixture-tested.
5. **Parameters (descriptors + change notification).** Every automatable value has a `ParamId` and a descriptor: range, unit, curve, display, and modulatable or not.
   - MIDI Learn, the mod matrix, automation, presets and generated edit pages all work on descriptors.
   - Engine modules publish descriptors through the ABI, so a new engine gets its edit page without new UI code.
   - Changes go out through a bounded subscription list (Observer) or through SPSC messages towards the audio side.
6. **Model–view separation.** The project model (ADR 0003/0004/0007) knows nothing about UI or hardware. Screens render from the model and change it only through actions, which also gives undo a single choke point.

## Consequences
- OS-002 replaces the walking skeleton's hard-coded input `switch` with these mechanisms. The skeleton's pattern view becomes the first registered screen, with a second trivial screen and an overlay proving navigation. Formats and parameters get their interfaces and registries, plus one real implementation each.
- Verification:
  - unit tests for binding resolution, conflict detection and screen navigation;
  - simulator scenarios driven through bindings;
  - layer rules: `app` screens may not include `platform`/`sim`, and formats live below `app`.

## Revision 1 — design review (2026-10-10), adopted
**Changes to the mechanisms above:**
- **Actions** carry a 32-bit `ActionArg` (pad index, encoder delta…) and flags (`undoable`, `repeatable`, `needs_focus`). Actions live in feature files that operate on the model plus a small `ViewState`; screens render and expose focus. They do **not** switch on actions (`Screen::on_action` is removed).
- **Gesture layer** between HAL events and bindings: tap / hold / double-tap / hold+encoder / Shift, with all timings in one table.
- **Bindings:** layered resolution, user > preset > built-in, walking a **context chain** (overlay → mode → screen → global); overlays may capture. A conflict means two entries in the same layer and context.
- **Screens:** one base screen plus at most one overlay. The overlay declares its covered rectangle and the base skips drawing there (2048-word display-list budget).
- **Parameters:** a per-parameter generation counter that screens poll on redraw; audio receives `ParamChange` at block boundaries. No observer callbacks.
- **Formats:** resumable `begin(src, arena) / step / report()` over a fixed SDRAM arena; `report()` lists what was dropped (ADR 0007).
- **PC key / HID / host keyboard:** `InputKind` values are reserved now; implementation comes later (OctaMED keymap first in the simulator).

**Added mechanisms:**
- **Main↔audio bus:** typed SPSC rings in both directions.
  - Towards audio: ParamChange, NoteOn/Off, transport.
  - Back from audio: meters, position, overload.
  - MIDI input *enqueues* actions; it never runs them from the ISR.
- **Undo journal:** one fixed-record ring per area (PATTERN, SONG, SOUND; OS-UNDOGRP) carved out of one SDRAM region, written only by `undoable` actions as model-level patches; F8 undoes the area of its screen, and encoder clicks on one parameter row fold into one unit. The Mixer has no history. Macros replay `(ActionId, arg)` from the same log.
- **Cooperative jobs:** `Job::step(budget_us) → Running | Done | Failed`, with cancel and progress, a pool of 2–4 jobs, run from the main loop. Used for import/export, analysis, autosave and module loading.
- **Notifications:** a bounded queue `{severity, text, source, ttl}` drawn as the overlay.
- **Settings are parameters** with `scope ∈ {device, project}`, persisted as versioned chunks (ADR 0007), with default-fill for old or unknown data.
- **Record/replay:** the timestamped input queue can be dumped to SD; the simulator replays it.
- **Module page hints:** group / order / F-tab, plus a non-real-time "curve points" callback for graphs. Modules never draw.
- **Build trimming:** per-feature `constexpr` registration lists, enabled by CMake options.
- **Low priority:** module-declared needs (cycles, bytes), used for display and quarantine only; a `StrId` string table so themes and strings can become SD packs.

**Open, for OS-004 (project model):** where recorded automation lives. Proposal: per-pattern automation lanes kept outside the step data.

## OS-002 slices (in this order)
- **002a:** action table + `ActionArg`/flags; gesture layer; layered resolver with context chain. Replaces the skeleton `switch`. Scenario command `action <name> [arg]`.
- **002b:** base screen + one overlay with covered rectangle; a second screen; the notification overlay.
- **002c:** undo journal; job runner, with the WAV importer as the first resumable job.
- **002d:** parameter descriptors + generation counters; the `ParamChange` SPSC to a fake audio consumer; one generated edit page. Settings persistence may slip to OS-003.

## Amendment 1 — macro recording (2026-10-11, OS-002c review)
Macros record `(ActionId, arg)` in their own bounded buffer (64 steps), not in the undo journal. Navigation and other non-undoable actions never reach the journal, yet a macro must replay them. The journal stays the single choke point for model edits; replaying a macro goes through `dispatch`, so its edits are journaled like any others.
