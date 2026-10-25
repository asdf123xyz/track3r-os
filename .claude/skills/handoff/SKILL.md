---
name: handoff
description: Close a working session cleanly so the next chat resumes cold without re-briefing - stop owned background agents, leave no orphan changes, update docs/STATUS.md and the backlog, save durable lessons. Use when the maintainer says they will stop or continue in another chat, at a milestone, or when context is running long.
---

> **Maintainer's workflow.** This skill closes a session of the maintainer's multi-session set-up. It is published as an example: `docs/agent-setup.md` says what to adapt.

# Session handoff

1. **Agents.** Stop background agents this session owns (`TaskStop`) unless the maintainer asked to let them finish. If a peer session shares the checkout (`ListAgents`), tell it what you are leaving and where.
2. **Tree.** Nothing uncommitted and unexplained: merge finished work through `fw-feature` phase 8; otherwise commit WIP on its own branch (`wip: <what> [ID]`, never on `dev` or `main`) and remove idle worktrees. Stage explicit paths only.
3. **Status.** `doc-keeper` (Haiku) with the facts - commits, hashes, sizes, test counts, decisions - updates `docs/STATUS.md` (short: done since the last handoff, decisions, "Blocked on maintainer", and **## Next** as an ordered list a fresh session can execute, 12 lines or fewer because the SessionStart hook prints it) and `docs/roadmap/BACKLOG.md` (row states, the image version under DONE, images waiting for the unit). The maintainer alone moves rows out of IDEA.
4. **Memory.** Only durable maintainer preferences or corrections become memory, updating an existing entry rather than duplicating; project facts belong in docs, not memory.
5. **Maintainer message**: 8 lines at most - what is done, what is pending, where the next session resumes, the questions waiting for them.
