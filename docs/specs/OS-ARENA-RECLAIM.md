# OS-ARENA-RECLAIM: replacing a slot's sample gives the memory back (OS-AUDIT-34, E-LOAD-04, SLD-ARENABAR)

Status: design note, Level C (sample memory read by the audio voices). New Community OS behaviour; no file format changes.
Code: `app/include/o3os/app/sample_bank.h` (RECLAIM), `app/src/sample_bank.cpp` (`commit_load`, `reclaim`), `app/src/og_samples.cpp` (`bind`).

## Why the unit did not reclaim

Observed on fqa.10: a 374-frame WAV loaded over instrument 1 (152,757 frames, from the project) moved USED 7595 -> 7596 KB.
OS-FU1 (RECLAIM) needs all of these, checked in `commit_load()` and `reclaim()`:

1. `Slot::own != nullptr`: the old audio is the block `commit_load()` gave the slot. Project-loaded audio comes in through `OgSampleLoader::bind()` -> `adopt()`, which sets `own = nullptr`. **This is the condition that failed.**
2. The new block starts exactly where the old one ends (`own + own_len == pcm`). In a project the slot's block lies below the audio of the slots after it, so it fails here as well.
3. The new block is the last of the arena, no holder of either block, new sample <= `kReclaimMax`.

So today only "load the same slot twice in a row" is reclaimed; the first replace of any project sample, and an alternation of two slots, leave the old block dead until the next project load.

## Options

| Option | Gives back | Cost / risk |
|---|---|---|
| A. Slot capacity (chosen) | A replace never grows the arena when the new audio fits the slot's own block; repeated replaces stay free | small; same quiet-window guarantee as OS-FU1 |
| B. Hole list + allocation into holes | The freed block for any later load | medium+; the WAV importer must be told a bound for the length before it reads; `free` becomes fragmented (what the bars show is a UI decision); loader extents; many allocator interactions |
| C. Compaction of the tail over the hole | Everything | large: every pointer into the moved tail must be fixed (slots pcm / bound / own / table, descriptors, staged cell, clipboard, swaps, edit history, jobs, takes, SampleBankMem itself); the move of megabytes must be chunked across passes with the voices silent. Estimate 3 to 5 days plus hardware soak. Not done here. |

## Design (option A)

A slot's own block has a **capacity** (`own_len`, even). Project-loaded audio now counts: the loader tells the bank the extent of the block it placed (`adopt(..., block)`: the even PCM length, not the slice table that follows it, which the first rebind replaces anyway).

`commit_load()` loads the WAV into the free tail as ever, and when the old audio is the slot's own block, unreferenced by any other holder, it records it as `stale` (start and capacity) and marks the slot. At the next quiet handover (the existing `poll()` window: transport stopped, no live note, voices silent for `kSettleMs`, no job running; the descriptor has already been rebound to the new audio, so no voice reads the old block), `reclaim()` does one of two things:

- **Adjacent** (the old block ends where the new one starts): as OS-FU1, move down, the capacity becomes the new length.
- **Inside the capacity** (new): the new block is the last of the arena and `new <= capacity`: move it to the start of the old block (`memmove`, no overlap), the arena shrinks back to where the new block began (the whole tail copy is given back), the capacity stays. The next replace of the slot is free again, however many times.

Both stay at most `kReclaimMax` samples (256 KiB, about 10 ms) per pass: bounded work, no chunking needed. A longer sample, or a new one bigger than the capacity, keeps today's behaviour.

What the slack costs: a block that is bigger than its audio keeps its room for that slot (it counts as used, as before); it is not returned to other slots. Returning it needs option B or C.

## When it is safe (the rules, all re-checked in `reclaim()`)

- No voice reads the old or the new block: the handover window (stopped, no live note, pool silent for `kSettleMs`, `jobs_idle`), after `rebind_now()` has written the new descriptor; the slot is set silent during the copy and `set_sample()` cancels a staged descriptor.
- No other holder: `referenced()` over the old block and the new one (other slots' `pcm`/`bound`, clipboard, swap records done and undone) and `block_in_history()` (sample edit undo and redo). A `.pti` load (`commit_load_undoable`) pushes a swap that names both blocks, so it is never reclaimed; a paste that shares the audio, a copy on the clipboard and an undo entry block it the same way.
- The slot still reads its own block (`pcm == own`): after a sample edit or a paste `own` is not the slot's audio and nothing moves.
- Instrument delete does not tell the bank and is undoable (the audio comes back with it), so a delete frees nothing; a later load into the same slot reuses the room through the capacity. Loading into another slot does not.
- Project load (`clear()`) and File > NEW (`new_project`) are unchanged.

## Not covered (follow-ups)

- Option B/C above (give the slack back to other slots).
- A beat-slice instrument's slice table is allocated after the new block at the handover, so the new block is no longer last and the move is refused (as in OS-FU1). Reusing the slot's table across a replace would fix it.
- Audio after a sample edit has `own != pcm`.

## Tests

`tests/sample_bank_test.cpp`: two or three loads before the handover (the slot's original block, the lowest stale one, keeps the larger capacity and receives the newest audio; the first load's block stays a hole until the next project load); the kReclaimMax boundary (131,072 frames moves, 131,074 does not); a beat slice slot refuses; adopt() ignores an odd, too small or oversized block; project-loaded block replaced twice (arena unchanged, audio intact, version moves); a held reference (swap from an undoable load, clipboard, another slot, edit history, a sounding voice, a running job) blocks the move; new longer than the capacity falls back; the arena invariants (`used`, slot blocks inside it, no overlap) after every step.
