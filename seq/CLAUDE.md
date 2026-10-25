# seq/ — sequencer v2, control layers, Perform, MIDI sync glue

- Owns the sequencer as a pure function `step(block_start, n) -> SeqEvent[]` over a `uint64` sample timeline, called once per block from the render: deterministic, no ISR, no second clock, renderable offline. Must not: request SD, allocate, hold a pointer into the model across a call (`rows.h` copies a step and checks the pattern generation), or use `double`. May include `core`, `usb`, `hal`, `audio`, `model`, `midi`.
- Entry points:
  - `include/o3os/seq/sequencer.h`: `step()`, tempo/swing, transport, follow
  - `include/o3os/seq/fx.h`: `kFxInfo`, the one table of which step FX is executable, with ranges and groups
  - `include/o3os/seq/control.h`: who controls a voice parameter (FX col 1 over col 2 over Perform)
  - `include/o3os/seq/rows.h`: reading a step from the model arena in the render context
  - `include/o3os/seq/engine_link.h`: `audio::EventSource` over a `Sequencer`; `midi_link.h`: MIDI sync end to end; `offline.h`: export render (a song's slot range, OS-SONGSHARE S5: `OfflineRequest::order_first` / `order_last`, `Config::order_stop`; tests `tests/seq_offline_test.cpp`, `tests/seq_sequencer_test.cpp`); `perform.h`: the 12 Perform slots
- Add a step FX: a row in `kFxInfo` (`fx.h`), its behaviour in the sequencer/control, a test. FX ids and ranges are part of the pattern file (ADR 0004 and its amendments): that is a persistent-format change (version bump, loader, round-trip test; maintainer decides incompatible ones). FX bytes in a pattern are untrusted: carried verbatim, sanitised where used.
- Behaviour claimed as OG parity needs evidence from the official firmware; `tests/data/seq_oracle_vectors.h` (`tests/seq_oracle_test.cpp`) is the output of our own reference model (maintainer, 2026-10-18) and pins this OS's behaviour only; determinism in `tests/seq_determinism_test.cpp`.
- Tests: `tests/{seq_*,metro_*,play_*}_test.cpp` (`midi_*` too). Scoped: `ctest --test-dir build-os-host -R unit_seq --output-on-failure` (or `-L seq`; also `seq_bench_smoke`); `O3OS_CI_SCOPE=changed tools/ci.sh host` maps `seq/` to labels `seq app`. `qemu_seq*` run under `tools/ci.sh qemu`.
- Cost: bench `tests/bench/seq_bench.cpp`; measure on the unit (`docs/architecture/cpu-budget.md`).
- Docs: `docs/specs/OS-005-audio-engine.md` (005c), ADR 0013 D5, ADR 0004, `docs/specs/{OS-COREFX,OS-METRO,OS-PFX}.md`. QA rows in `docs/qa/catalogue/{pattern,song,perform}.md` (format in `app/CLAUDE.md`); hardware steps in `platform/mk66/tools/hil/` (`play_*.json`, `song_*.json`, `perform_*.json`).
