---
name: decide
description: Settle an architecture or product decision for the Community OS (e.g. tracks vs voice pool, FX command format, module ABI, project format, undo model, licence) with a small parallel design panel and record it as an ADR for the maintainer to accept. Use for any cross-cutting decision that several work items depend on.
---

# Decision to ADR

Decisions are the bottleneck of a rewrite: make them explicit, cheap to review, and batch them for the maintainer.

## Steps
1. **Context packet** (orchestrator, 300 words at most): the question, constraints (MK66 budgets, real-time rules, compatibility with the original firmware's files, the maintainer's direction such as SD modules), links to the relevant docs and radar cards. No file dumps.
2. **Panel.** Small decisions: one `Plan` agent. Cross-cutting ones: three `Plan` agents in parallel, one lens each, 400 words back at most:
   - *Real-time and resources* - CPU, ISR and memory on the MK66, failure modes.
   - *Musician and compatibility* - workflow on pads, encoder and screen; import of original-firmware projects; what users lose.
   - *Extensibility and verification* - modules from the SD card, simulator, testability, maintenance.
   Each returns 2-4 options, a recommendation, costs, risks, and "what would change my mind".
3. **ADR** `docs/adr/NNNN-<slug>.md` (next free number), status *Proposed*:
   Context, Options (table: option, cost, risk, reversibility), Panel views (one line each), Recommendation, Consequences (what it unlocks or blocks), Validation (what to measure, which `hw-session` bench), Links.
4. **Maintainer.** Batch open ADRs into one round of questions (4 per call at most, recommendation first, ADR path in the description). Record the answer: status *Accepted* / *Rejected* / *Superseded by NNNN*, date, and the maintainer's words. `doc-keeper` adds it to `docs/STATUS.md`. If the maintainer is away, leave it *Proposed* and list it under "Blocked on maintainer".
5. Accepted ADRs are binding for specs; changing one needs a new ADR that supersedes it, or a dated amendment inside the ADR.
