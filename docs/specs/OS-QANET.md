# OS-QANET: a test net that catches classes of bugs

Level B per slice (tests, simulator, tools; docs). Evidence label: **new Community OS behaviour** (test tooling, no firmware behaviour). Any parity claim an audit makes is an **open research question**. Maintainer approval: 2026-10-21 (backlog row OS-QANET; rows 5 to 11 come before `rc/1.0.0`).

## 1. Goal

The v0.9.0b test session found seven bugs by hand. Our 877 catalogue rows and the goldens did not see any of them. Each test checks one case that someone thought of. This spec adds checks that cover a whole class, so the next bug of that class fails a build before a person finds it.

## 2. The four bug classes

| Class | Why the tests miss it | Evidence (rows of `docs/roadmap/BACKLOG.md`) |
|---|---|---|
| 1. Our spec is the oracle | A test written from the code agrees with the code. Nobody asked "should this ask first?" or "is this the right place?". | OS-BUG-SLDOVER (a WAV load replaces a slot, a module slot too, with no question and no undo); OS-BUG-ADDSLICE (code, spec and test agree on the window middle; the person wanted the selection) |
| 2. Sequences between screens | Tests run one screen from a fresh state. The bug lives in the state the first screen leaves behind. | OS-BUG-NEWLIST (File NEW, then Sample load shows an empty card); OS-BUG-SLICEPITCH (after ADD, EVEN or AUTO the pads play the old descriptor) |
| 3. Two paths for one thing, nobody compares them | Pad and Pattern reach one `VoicePool::note_on`, but each is tested alone. | OS-BUG-SLICEPITCH (pads three octaves down, Pattern right); OS-SLDTARGET (a pad picks the target, but nothing says so) |
| 4. The simulator differs from the unit | The simulator reads a host folder (`platform/host/src/dir_storage.cpp`, `fake_storage.cpp`), reads instantly, and its fixtures are tiny. The unit has FatFs, a slow card and a full card. | OS-BUG-PREVLAT (preview reads up to 4 s of audio at about 500 KB/s by estimate; no latency target exists); OS-BUG-NEWLIST (a scan or directory handle state that a host folder never shows) |

**Gaps found by the design review (2026-10-21).** A review of parts A to E (a Fable agent; the maintainer accepted the orchestrator's recommendation, section 12, Q5) found these gaps; each now has a slice:

| Gap | Example | Covered by |
|---|---|---|
| Rules run only on fresh states | WATCHSEL only shows while playing in WATCH; SLDOVER over a slot that the walk filled earlier | B4 |
| Nothing saves and reloads | Slice points edited after a chop are not written; the project reopens with the old sound | B5 |
| No oracle-free properties (class 1 with no spec) | A transpose that clips at the edge and does not come back | B5 |
| The walk misses screen sequences | Two seeds of 300 steps never reached File NEW, then Sample load | B6 |
| No held keys or chords | Play held while moving to another file (the second half of PREVLAT); tap and hold boundaries | B7 |
| No exhaustion | All slots used, sample RAM full, voices saturated, undo at its cap, card full | B8 |
| Sound paths compared only on a fresh chop | SLICEPITCH after an undo, a load over a sliced slot, a play-mode change | C4 |
| The net's strength is unknown | Passing the seven known bugs is one sample, not a measure | F1 |

## 3. What exists and is reused

- Scenario grammar: `sim/include/o3os/sim/scenario.h` (`press`, `tap`, `pad`, `encoder`, `action`, `document`, `card`, `card pull|insert`, `audio on`, `render`, `assert state|model|file|audio`, `mark model`). The `Simulator` class (`sim/include/o3os/sim/simulator.h`) drives the same app in-process.
- Sound measurements: `sim/include/o3os/sim/audio_check.h` (`pitch=`, `onset=`, `null=`, `envelope=`). Render and fingerprint rules: `sim/README.md`.
- State: `app/src/state_line.cpp` (the unit's `diag state` line; the same text in `assert state`). The document digest: `mark model` and `assert model digest=@label`.
- Actions: `action_registry()` and `ActionDef { id, name, handler, flags, enabled }` in `app/include/o3os/app/actions.h`; flags in `core/include/o3os/core/action.h` (`kActionUndoable`, `kActionUndoableIfOpen`, `kActionNeedsFocus`). 272 `make_action` lines in `app/src/actions.cpp`. Bindings: `app/src/bindings.cpp`; `build-map/keys.md` (from `python3 -I tools/code_map.py`) lists every binding with its handler.
- Undo: `model/include/o3os/model/undo.h` (`depth()`, `newest_undo()`). Questions: `app/include/o3os/app/key_prompt.h` (one table per question and dialog).
- Labels: `tests/shift_labels_test.cpp` (every Shift + F has a label, OS-UIFIX); `docs/qa/usability.md` (the findings USA-003 and USA-004 were live keys with a wrong label).
- Catalogue gate: `tools/qa_cover.py`, `docs/qa/catalogue/README.md`. Gallery and legibility: `o3sim --gallery`, `o3sim --legibility` (`sim/README.md`).
- Card fixture: generated at build time, never committed (`sim/CMakeLists.txt` target `e2e_card_fixture`, `tools/e2e_ref_cards.py`, `o3sim --cards`).
- HIL: `platform/mk66/tools/o3hil.py` (JSON scenarios in `platform/mk66/tools/hil/`, steps `key`, `pad`, `state`, `shot`, `within`, `poll`; audio capture `o3audio.py`). Example: `sample_load_audition.json` waits a fixed 1.2 s today.
- FatFs source: `third_party/fatfs`; the unit's storage is under `platform/mk66/storage/`; the interface is `hal/include/o3os/hal/storage.h`.
- Earlier design: `docs/specs/OS-E2E.md` D8 (E2E-10 random user, E2E-12 card and power chaos). They are folded into parts B and D. A libFuzzer lane stays out (section 11).

New code lives in `tests/qanet/`, `sim/`, `platform/host/`, `tools/` and `docs/`. ctest label `qanet`. `tools/ci.sh e2e` runs it; a short fixed-seed run joins `host` (Q4).

## 4. Part A: rules over every bound action

Walk `action_registry()` and the bindings table. For each action that a live key reaches, run it in the simulator on the demo document (`document sound`, `document tb303`, and a `card`), on a state where it has something to act on.

| Slice | Size | Build | Done-when |
|---|---|---|---|
| A1 | S | Harness `tests/qanet/rules_test.cpp`: enumerate actions and bindings, set up a state with data per action (a table `tests/qanet/action_states.cpp`: action name to setup steps), run it, take the digest before and after, and read the undo depth and the strip. Rule 1 (amended 2026-10-22 with the maintainer's undo policy, `docs/specs/OS-UNDOGRP.md` D11): a change to the pattern data (and the song), to an instrument or its parameters, or to the audio of a sample must leave an undo unit or have asked a question first (`key_prompt.h`), a load over a used sample slot included; a destructive change outside those three (on the Mixer's STEMS page Delete and Shift+Delete) must ask first; every other change is exempt (the Mixer's levels, mutes and solos, the settings, a load into a free slot, a new instrument in an empty slot: what a step only adds to an empty slot is not a change of anything that was there, and such a step may clear the history). An action that lowers the undo depth is allowed only for NEW, OPEN, undo and redo, and for a step whose changes are all exempt. The rules live in a library, `tests/qanet/rules.{h,cpp}`: a check takes the state before and after one action and returns findings, so B4 runs the same rules at every random step. The full walk runs under the label `qanet`; the default ctest keeps a short sample of it. | The test fails on dev with a line per breach (action, key, why). It finds OS-BUG-SLDOVER, without a rule that names it (a WAV load changes the digest, asks nothing, and clears the undo history). An action with no setup entry fails the test (a new action must be added to the table). The report states the count of findings that are not SLDOVER. |
| A2 | S | Rule 2: every live key has a label. Per screen, page, state and Shift layer, press each F key from the bindings table; a key that does something has a label in the key row, and a key row label has a binding; and a bound key that, in some state, changes nothing (no document, view, transport or sound change) and says nothing is a finding (silent no-op). Extends `tests/shift_labels_test.cpp` from Shift only to every state of `sim/include/o3os/sim/screens.h`, each state once with its bound keys (not every state against every key: the deeper states come from B4, which runs the silent no-op rule at every step). The rule goes into the rules library. | Test fails on a planted unlabelled binding (a self-test in the same file). On dev it passes or lists its findings; the count of new findings is reported (USA-003 and USA-004 are the known earlier cases and are fixed, so they must not appear). Finds OS-BUG-WATCHSEL (Shift + the arrows silent in WATCH while playing), here or through B4. |
| A3 | M | Rule 3: an operation on a selection acts on the selection. A table `tests/qanet/selection_ops.cpp` lists actions that take a selection (block copy, cut, paste, transpose, delete, ADD SLICE, slice delete, and so on; found from `build-map/keys.md` and the specs), with the span each may change. The test selects a span, runs the action, and compares the digest outside the span (it must not change) and the result inside it against the spec. The table is written from the spec as decided, not from the code. | Test fails on a planted out-of-selection edit (self-test). On dev it finds OS-BUG-ADDSLICE (the new slice is not between the selected start and end; the oracle is the 2026-10-21 decision: the middle of the selected slice). New findings counted. |
| A4 | S | Ledger and process rule (section 9): `docs/qa/qanet-ledger.md` and ctest `qanet_ledger`. | The ledger has the six rows; the test fails when a row has no regression test name that resolves or no catcher entry. |

## 5. Part B: a random user on a large, realistic card

| Slice | Size | Build | Done-when |
|---|---|---|---|
| B1 | M | Generator and invariants. `tests/qanet/random_user.cpp` drives `Simulator` with a seeded PRNG: weighted `tap`, `press`, `pad`, `encoder`, `card pull|insert`, and file jobs, over all screens (the weights favour screen changes and File and Sample load). Card: a large generated fixture (about 2000 files, 40 folders, long names, WAV, AIFF, 8SVX, `.pti`, projects, junk), made by `tools/qanet_card.py` at build time, never committed. After every step it checks, from `App::model()` and the state line: the card listing is not empty while the card holds files; no `NO SD CARD` with a card in; no job older than its bound (a table of bounds per job kind); no directory open at rest (`kMaxOpenDirs`); no display-list breach or crash (the simulator's healthy-frame check). Undo invariant: after N undoable steps and N undos the digest equals the digest at the start. The baseline `tests/qanet/baseline_random.txt` keys each line on the invariant, the screen and the action (and a known bug ID when there is one); `seed@step` is only the repro text after the key, so the baseline does not change when the weights change. Each run prints the count of distinct (screen from, action, screen to) tuples it covered. | `ctest -L qanet -R random_user` runs 200 seeds of 300 steps each in under 5 minutes at `-j2`. Same seed gives the same run (run twice, same final digest). A planted fault (a self-test hook that leaves a directory open) is found. It tries OS-BUG-NEWLIST: the generator must reach "NEW, then Sample load" without being told, and the report says whether the listing invariant fired (on a host folder it may pass; D1 repeats it on FatFs). Count of new findings reported. |
| B2 | S | Shrinking. A failing run is reduced by deleting steps (halving, then one at a time) while the same invariant still fails. The shortest run is written as a scenario file in the `scenario.h` grammar (`sim/scenarios/qanet/found_<seed>.txt`) that `o3sim` replays, and as a one-line repro command. | A planted fault found at step 240 shrinks to under 15 steps; the written scenario fails under `o3sim` with the same message (a test does both). |
| B3 | S | Card chaos (E2E-12 card side): card pulled during a scan, during a save, during a preview and while a directory is open; insert of a card with other content (a second fixture); a full card (write refused). | Invariants hold after each event, or findings are listed. A torn project file never loads as valid (`assert file`, and OPEN refuses it). New findings counted. |
| B4 | S | Rules at every step. After each random step, the walk runs the rules library of part A (rule 1, undo or ask; rule 2, silent no-op; rule 3, selection span, as each lands) on the state before and after the step. The short run may check every k-th step; the long run checks every step. | A planted rule-1 breach that only shows after a slot is filled mid-walk is found. On dev it reports whether WATCHSEL and SLDOVER fire mid-walk. New findings counted. Run time (2026-10-23): the long shards judge every input (k = 1) but run seeds 1..72 (9 a shard, 40 s a seed, 5 to 7.5 min a shard); k > 1 saves nothing, because the walk is the same for every k (the 100 ms and 60 ms waits around each input are walk inputs: measured on 3 seeds: 6 s a seed with `--rules-every 0`, 34 s with k = 4, 37 s with k = 1), and 200 seeds took 16-18 min. |
| B5 | S | Metamorphic relations (they need no spec). Step kinds that pair an action with its inverse and check that the digest comes back: save, then OPEN of the same project; transpose +n, then -n; octave up, then down; copy, paste, then undo; a slice edit, then undo. Save, then OPEN runs from every screen the walk reaches, with the document as the walk left it. | A planted field that `save_project` (`app/include/o3os/app/save_service.h`) does not write is found. New findings counted. |
| B6 | M | A walk guided by coverage. The walk records (screen from, action, screen to, document dirty, card state) tuples and raises the weight of steps that lead to tuples not yet seen; it prints the tuple coverage of the `screens.h` states against the bindings. | In the default run, every seed reaches File NEW, then Sample load (the NEWLIST sequence) without a weight that names it. Coverage per seed-minute is printed. New findings counted. |
| B7 | S | Hold-and-do steps. A step kind holds a key or pad, does 1 to 4 steps, then releases it; with a timing oracle per step: the first observable change comes within the D3 budget. Needs D2 and D3. | It tries the Play-held half of OS-BUG-PREVLAT (moving to another file while Play is held plays the new file at once) and reports the result. A planted lost key-up is found. New findings counted. |
| B8 | S | Exhaustion fixtures. Some seeds start from states at the limit: all sample slots used, a folder of 2000 files, the voice pool saturated by held pads, undo at its cap, a full card. | Each fixture is reached by at least one seed of the default run. New findings counted. |

## 6. Part C: a differential audio matrix

Same note, several paths, one answer. Uses `audio on`, `render`, and `assert audio ... pitch= onset= null=` (`audio_check.h`) with `sim/include/o3os/sim/sim_audio.h`.

| Slice | Size | Build | Done-when |
|---|---|---|---|
| C1 | M | The matrix `tests/qanet/audio_matrix_test.cpp` (or generated scenarios under `sim/scenarios/qanet/`): rows are instrument kinds (plain sample, loop, slice or beat slice, wavetable, grain, MIDI out where audible, TB-303 module) x play modes; columns are the path: pad, Pattern step, with the same note and velocity. Check: pitch within 5 cents and onset within 2 ms of the Pattern render (the reference is the spec: root note 60 plays at the sample's rate). | The test runs in under 3 minutes. It finds OS-BUG-SLICEPITCH: pad and Pattern pitch differ on a freshly chopped instrument (pads about 3 octaves down). A planted detune in the pad path fails it. New findings counted. |
| C2 | M | More paths: preview (F5 in Sample load), MIDI note in (`midicc` is CC only: add a `midinote` step or inject through the same event path, as in `docs/specs/OS-E2E.md` E2E-6, noting it) and EXPORT (`wav <name> <path>` of the exported file). Pairwise equal pitch and onset against the Pattern render, export included. No `null=` comparison of the export until the limiter is deterministic: it is fragile (design review, 2026-10-21). | Matrix has five path columns; each cell passes or is listed as a finding. Preview onset is reported in ms (feeds D3). New findings counted. |
| C3 | S | Right after an edit: each cell is run again straight after an edit that rebinds the descriptor (ADD, EVEN, AUTO, slice delete, change of play mode, load, undo) with no settle wait, and again after waiting. The two must agree. | The immediate row catches the descriptor-rebind lag (`SampleBank::poll`, `kSettleMs`) as SLICEPITCH lead 1 does, or states why not. New findings counted. |
| C4 | S | The matrix inside the random walk. After any step of B that rebinds a descriptor (ADD, EVEN, AUTO, slice delete, play mode, load, undo), the walk plays the same note from a pad and from a Pattern step and compares pitch and onset with the C1 tolerances. The check is a function in `tests/qanet/` that B calls; it does not copy the matrix. Needs C1 and B1. | A planted detune after an undo is found. It reports SLICEPITCH variants (after undo, after a load over a sliced slot, after a play-mode change). New findings counted. |

## 7. Part D: the simulator nearer the unit

| Slice | Size | Build | Done-when |
|---|---|---|---|
| D1 | M | A host `Storage` over FatFs: `platform/host/` gets a `FatFsStorage` (an implementation of `hal/storage.h` on `third_party/fatfs` with a disk driver over a FAT32 image file) and `tools/qanet_card.py` writes the same large fixture into an image (sparse, about 64 MB; the generator can use FatFs itself through a small host tool, so no `mkfs` or root is needed). `card <image>` in the scenario grammar and `o3sim --cards` accept an image. | The existing card scenarios (`sim/scenarios/e2e/`) pass on the image with the same asserts. Part B runs on it (`ctest -R random_user_fatfs`). It tries OS-BUG-NEWLIST again, on FatFs with long names. New findings counted. |
| D2 | M | A slow-card model: a wrapper over any `Storage` with bytes per ms and latency per call (defaults from the unit's measured read rate; a value is `PROVISIONAL` until D4 measures it, and D2 ships with the provisional values). Each call charges its time to the simulated clock in the pass where it happens, as the unit's synchronous call would stretch the pass. Options `--card-rate`, `--card-latency`, and a scenario line `card slow <bytes-per-ms> <latency-us>`. | A unit test reads 1 MB through the model and the simulated clock advances by the modelled time (within 1 ms). With the model off, nothing changes: all goldens and fingerprints are unchanged (`ctest` full). |
| D3 | S | Latency budgets as tests, on the slow card, from the key press to the observable result: preview first sound (F5 to the first sample above -50 dB, `onset=`), screen switch (press to the frame that shows the new screen), listing (entering File and Sample load until the listing shows entries). Proposed limits: 150 ms, 100 ms, 500 ms (open research question until measured; Q1). | Three tests, with the limits in one table (`tests/qanet/budgets.cpp`). On dev, preview first sound fails the 150 ms budget: it finds OS-BUG-PREVLAT. The other two are reported with their measured value. New findings counted. |
| D4 | S | The same budgets on the unit: `platform/mk66/tools/hil/qanet_latency.json` (key, then `state` and `o3audio.py` capture with a timestamp; `within` is the limit). One run gives the numbers that set D2's defaults. | `o3hil.py validate` accepts it; a tester run writes the measured values into `docs/qa/qanet-ledger.md` (or `build-hil/`, with the table in the report). Hardware verification is the tester's. D4 gates nothing: no slice waits for a tester run. |

## 8. Part E: the OG-user audit

| Slice | Size | Build | Done-when |
|---|---|---|---|
| E1 | S | Brief for an auditor agent (a fresh one, model chosen by the orchestrator). It gets only the screen list (`docs/ui/ui-map.md` §1 names, no behaviour text) and the key names. It must not read `docs/specs/`, `docs/ui/`, the catalogue or the manual, and never the official manual (maintainer rule 2026-10-18). For each screen it writes what an owner of the official firmware expects: what each key and pad does, what a load, an overwrite or a delete should warn about, what a target or selection should be. Output `docs/qa/og-user-audit/<screen>.md`, each line `expectation | confidence`, each labelled **open research question**. | The files exist for the screens of Q3 and each line has a label. A check script (`tools/`) fails when a file cites `docs/specs` or the manual. |
| E2 | S | A second agent (or the same, after) compares the expectations with `docs/qa/catalogue/` and the code, and writes one table: expectation, what the OS does, difference, severity. | The table `docs/qa/og-user-audit/differences.md` flags OS-SLDTARGET (no way to see or change the target) and the missing overwrite warning of OS-BUG-SLDOVER, without being told. Every other difference is a row for the maintainer; none is fixed or called parity. |

## 8a. Part F: the net's strength

| Slice | Size | Build | Done-when |
|---|---|---|---|
| F1 | M | Bug seeding. `tests/qanet/mutants/`: a committed set of 15 to 20 small patches to `app/` (and `audio/` or `seq/` where a class needs it), each a planted bug of one class of section 2: a swapped slice midpoint, a removed question, a missing listing refresh after NEW, an octave offset on the pad path, a lost undo unit, and so on. Each known bug has a twin of its class elsewhere in the code (not the bug itself, which is still on dev). `tools/qanet_mutants.py` applies each patch in a throwaway build on the remote machine, runs the label `qanet`, and writes the kill table: mutant, class, the parts that caught it. A patch that no longer applies is reported as stale. | The table prints the kill rate per part and per class. The first table goes to the maintainer with the slice report. From then on, a new rule must raise the kill rate or kill a mutant that nothing else kills. |

## 9. Process rule (from now on)

Every bug a person finds gets: (1) a regression test that fails before the fix; (2) a line in `docs/qa/qanet-ledger.md`: ID, class (section 2), the test, and the answer to "which rule or generator should have caught it". If none would have, the same branch adds one (a rule row, a generator weight, a matrix cell, an invariant, an audit line) or records why not. The `firmware-reviewer` checks the line. A4 builds the ledger and its gate.

## 10. Validation, order, lanes

**Validation.** A part is accepted when it finds its known bugs on dev as it is, without being told where they are. The seven rows stay unfixed on dev until then (maintainer, 2026-10-21). Known positives: A finds OS-BUG-SLDOVER, OS-BUG-ADDSLICE and OS-BUG-WATCHSEL; B tries OS-BUG-NEWLIST; C finds OS-BUG-SLICEPITCH; D finds OS-BUG-PREVLAT and tries NEWLIST on FatFs; E flags OS-SLDTARGET and the missing overwrite warning; B4 reports WATCHSEL and SLDOVER mid-walk; B6 reaches the NEWLIST sequence in every seed; B7 tries the Play-held half of PREVLAT; C4 reports SLICEPITCH variants; F1 gives the first kill rate. Every new finding goes to the maintainer as a triage table (ID, class, steps, severity, proposed row). No fix happens inside OS-QANET: fixes are the backlog rows.

**Measuring the net.** Four numbers, printed by the runs and kept in the ledger: the mutant kill rate per part (F1, the headline number); the tuple coverage per seed-minute (B6); the ledger rate, the share of bugs found by a person whose ledger line says a rule would have caught them (target: rising); the steps to the first finding on a planted mutant (the generator's efficiency).

**Order.** E and A first (cheap, they find the most per hour), then B, C, D, F. First slice of each: E1, A1, B1, C1, D1. D2 to D4 follow D1; B3 follows D1; B4 follows A1; B7 follows D3; C4 follows C1; F1 follows A1, B1 and C1.

**Lanes (amended 2026-10-21).** Lane A: A1 to A4, then F1. Lane B: B1, B2, B4, B5, B6, B8, C4, then B3 and B7 once D1 and D3 are in (every B slice and C4 touch `random_user.cpp`, so they stay in one lane). Lane bugs: E1 and E2 (done), C1 to C3, then D1 to D4 (D4 with the tester). Files do not overlap (each part has its own `tests/qanet/` files; the rules library is part A's, and B only calls it). D1 and B1 share `tools/qanet_card.py`: B1 writes it, D1 reuses it.

## 11. Costs, risks, out of scope

- **Flash and CPU:** 0 B and 0 CPU in the image. Tests, simulator, tools, docs only. No slice adds firmware. If a slice needs a state key on the unit (`app/src/state_line.cpp`), it records the flash delta and asks first.
- **Run time:** B (5 minutes), C (3 minutes), D (FatFs, slow card) must stay under the label `qanet`; none runs in `tools/ci.sh host` except the short B run and the short sample of A1's walk (Q4). F1 runs only on the remote machine, once per part merge, never in `host`.
- **Later, after `rc/1.0.0`** (maintainer, 2026-10-21): E3, each expectation of part E that the maintainer accepts becomes an executable scenario tagged `og` under `sim/scenarios/qanet/`, and E2's table is diffed again at each release; D5, the same deterministic scenarios on the simulator and on QEMU (`tools/ci.sh qemu`), comparing the state line and the digest.
- **Risk: false positives.** A rule that fails on a correct design wastes time. Each rule has a self-test, a short allow-list with a reason per entry (as `sim/src/legibility.cpp`), and the oracle is the spec.
- **Risk: the simulator still differs.** Even with FatFs and a slow card, it has no real SD timing, no USB and no real interrupts. D4 on the unit is the check; a pass in the simulator is not hardware verification.
- **Risk: flaky randomness.** Seeds are printed and replayable; no wall clock in the generator.
- **Risk: scope creep.** The net finds; it does not fix.
- **Out of scope:** fixing any bug found; a libFuzzer lane; MIDI journey, modules in the sim, format generators (OS-E2E later rows); power-loss chaos beyond card events; reading Polyend's manual; changes to the formats.

## 12. Maintainer decisions (2026-10-21)

| # | Question | Answer |
|---|---|---|
| Q1 | Latency budgets (D3, D4) | The proposed budgets stand: preview first sound under 150 ms, screen switch under 100 ms, listing under 500 ms, tuned once D4 measures them. The maintainer adds two expectations, the known positives of D3 and E: the preview streams the sample (it starts at once, it does not wait for the whole clip), and while Play is held on Sample load, moving to another file plays the new file at once. |
| Q2 | What does a new finding do to `rc/1.0.0`? | Findings block it broadly ("many": one unit session already found the seven known bugs). `rc/1.0.0` waits for OS-QANET and the triage of its findings; a finding blocks unless the maintainer marks it "after 1.0" in a triage table. |
| Q3 | Part E: which screens? | Every screen, in one pass (E1 covers all of them). |
| Q4 | Where does part B run? | On the remote build machine: the long seeded run is the main mode there; the short fixed-seed ctest stays, and runs there too through `tools/remote_ci.sh`. |
| Q5 | The design review of the net (a Fable agent, 2026-10-21) | Accepted as the orchestrator recommended: B4 to B8, C4 and part F (F1) are added; E3 and D5 wait until after `rc/1.0.0`; four simplifications (B's baseline key, no `null=` export check in C2, D4 gates nothing, A2 one pass per state). |

## 13. State for implementer: first slice

Two first slices start in parallel (lane A takes A1, lane bugs takes E1). This is A1.

- **Read first:** `app/include/o3os/app/actions.h` (`ActionDef`, `action_registry`), `core/include/o3os/core/action.h` (flags), `app/src/bindings.cpp` (or `build-map/keys.md`), `tests/builtin_bindings_test.cpp` and `tests/actions_test.cpp` (how tests walk the registry), `tests/shift_labels_test.cpp`, `sim/include/o3os/sim/simulator.h`, `sim/include/o3os/sim/demo_project.h`, `model/include/o3os/model/undo.h`, `app/include/o3os/app/key_prompt.h`.
- **Build:** `tests/qanet/rules_test.cpp` and `tests/qanet/action_states.cpp`, added in `tests/CMakeLists.txt` under ctest name `qanet_rules` with label `qanet`. For each registry action that a binding reaches: set up its state from the table (document with data, a card with a sample file, a selected slot), run it through `Simulator::action` or the bound key, compare the document digest and undo depth before and after, and read the strip for a question. Rule 1 as in the table of A1. Print one line per breach: action, key, before and after, why.
- **Also:** a self-test that plants a breach (a test-only action that edits with no unit and no question) and expects the finding; an allow-list file with a reason per entry, empty at the start.
- **Do not:** edit `app/`, fix SLDOVER, or tell the test where SLDOVER is.
- **Done-when:** `ctest -R qanet_rules` builds warning-free with `-Wall -Wextra`; it fails on dev with a finding for the WAV-load action (document changed, no question, undo depth cleared); the self-test passes; the final report says how many findings are not SLDOVER and lists them as a triage table; the test costs 0 B in the image (the host build only).
