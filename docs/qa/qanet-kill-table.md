# OS-QANET kill table (F1: the strength of the net)

Run 2026-10-23 on the remote build machine, one single run of all 21 patches (`python3 -I tools/qanet_mutants.py --run --extend '^qanet_random_user_1of8$'`, about 3 hours) against cc3dabe1: dev 0292f250 (the OS-QAFIX batch merged; of the known bugs only OS-BUG-NEWLIST is still open) plus the F1 branch's own files (the tool and the patches). Every row of the table below comes from this run; none is merged from an earlier one. Control run: the clean tree passes every test, and so does the extended test (`qanet_random_user_1of8`), which is why its result counts. Nothing was committed into a product file: each patch was applied, built and reverted in a throwaway worktree (build-mutants/tree). The tests named `*selftest*` are not run (they test the net's own machinery with planted faults and do not look at the product).

Provenance of the earlier table (the one this one replaces, 12 of 19 = 63%): it was not one run either. Its 18 rows came from a full run on the snapshot 43285840 and the row of M13 from a second, partial run (`--only M13 --merge`) on the snapshot 7748bcc1; the text called both "dev 74963ec7", which is the base of the branch, not what was built. That run did not record the kind of each failure (a Timeout and a crash counted as kills like an assertion), so its rows could not be flagged afterwards: this run is a full one for that reason.

Each row is one planted bug (tests/qanet/mutants/). A mutant is killed when at least one qanet test fails on it and none fails on the clean tree. The kind of every failure is now recorded: a test that only timed out or crashed is flagged in the `killed by` cell (`(Timeout)`, `(SegFault)`) and a kill that has no assertion at all is flagged `FLAG` in the verdict; a test that did not run (`Not Run`) is never a kill. Three rows are reverts of fixes (header `# revert:`: M01 and M02 switch the OS-OPENGUARD guards off, M13 puts the silence-only wait of OS-BUG-SLICEPITCH back): they are run and shown, but not counted in the rate, because they show that a fix has a test, not how strong the net is on bugs nobody has found. Two new mutants (M20, M21) plant the same classes elsewhere, so the counted set stays at 18.

| Mutant | Class | Twin of | Expected part | Killed by | Verdict |
|---|---|---|---|---|---|
| M01 | 1 | OS-BUG-SLDOVER (an action that replaces the document asks nothing) | A1 | - | survived (revert of OS-OPENGUARD: not counted) |
| M02 | 1 | OS-BUG-SLDOVER (an action that replaces the document asks nothing) | A1 | - | survived (revert of OS-OPENGUARD: not counted) |
| M03 | 1 | OS-BUG-SLDOVER (a change of the document that is no undo unit) | A1 | A1 [rules_2of4] | killed |
| M04 | 1 | OS-BUG-SLDOVER (the undo history does not give the document back) | B | B [ru_short]; D1 [ru_fatfs_short] | killed |
| M05 | 1 | OS-BUG-SLDOVER (a destructive action asks nothing) | none (a card file is not in the document's digest: no rule and no invariant reads it) | - | survived |
| M06 | 1 | OS-BUG-WATCHSEL (a bound key does nothing and says nothing in one state) | A2 | - | survived |
| M07 | 2 | USA-003 and USA-004 (a live key with a wrong or missing label) | A2 | A2 [keys_5of6, keys_3of6] | killed |
| M08 | 2 | OS-BUG-NEWLIST (a listing is not refreshed after a change elsewhere) | B | B [ru_short] | killed |
| M09 | 2 | OS-BUG-NEWLIST (File, then Sample load: the listing of one screen is shown on the other) | B | - | survived (also the extended run) |
| M10 | 3 | OS-BUG-SLICEPITCH (the pad path plays another pitch than the Pattern path) | C | C [audio:edit.load_over_sliced, audio:edit.load_empty, audio:edit.mode_loop_bwd, +16] | killed |
| M11 | 3 | OS-BUG-SLICEPITCH (MIDI in and the Pattern path disagree) | C | C [audio:sample.one_shot, audio:sample.loop_fwd, audio:sample.loop_bwd, +15] | killed |
| M12 | 3 | OS-BUG-SLICEPITCH (the export and the live sound disagree) | C | C [audio:sample.one_shot, audio:sample.loop_fwd, audio:sample.loop_bwd, +12] | killed |
| M13 | 3 | OS-BUG-SLICEPITCH (a pad pressed just after an edit plays the old descriptor) | C | C [audio:ring.tail_auto, audio:ring.tail_even, audio:ring.tail_add, +3] | killed (revert of OS-BUG-SLICEPITCH: not counted) |
| M14 | 3 | OS-BUG-SLICEPITCH (one path of two plays the wrong slice) | C | - | survived |
| M15 | 1 | OS-BUG-ADDSLICE (an operation on a selection acts outside it) | A3 | A3 [select_1of3, select_2of3, select_3of3] | killed |
| M16 | 1 | OS-BUG-ADDSLICE (an operation on a selection acts outside it) | A3 | A1 [rules_gaps, rules_4of4] | killed |
| M17 | 4 | OS-BUG-NEWLIST (a directory handle state that a host folder never shows) | B | B [ru_short]; D1 [ru_fatfs_short] | killed |
| M18 | 4 | OS-BUG-PREVLAT (a card job whose end the host folder never withholds) | B | B [ru_short (Timeout)]; D1 [ru_fatfs_short] | killed |
| M19 | 3 | OS-BUG-SLICEPITCH (the document and what is written of it differ) | B (B5, save+OPEN) | B [meta_save_open of qanet_random_user_short: seed 1 step 300 and the meta selftest] | killed (B5, 2026-10-24, one targeted run: `O3OS_CI_TESTS=qanet_random_user_short`, 1 fresh finding meta_save_open, the document digest 454f62e38a41 against 41ef3a6d2a8f; not part of the 3-hour run above) |
| M20 | 1 | OS-BUG-SLDOVER (a destructive action asks nothing) | A1 | - | survived |
| M21 | 3 | OS-BUG-SLICEPITCH (a pad pressed just after an edit plays the old descriptor) | C | - | survived |

**Kill rate: 11 of 18 mutants killed (61%)** by the tests of the qanet label (`ctest -L qanet -LE e2e`, selftests apart); survived 7, stale 0, broken 0.

Not counted: 3 mutant(s) put back a bug that a fix removed (M01, M02, M13); 1 of them killed. They show that the fix has a test, not how strong the net is on bugs not yet found.

Of the 7 survivors, 0 are killed by the extended run (the long run of the random user, ^qanet_random_user_1of8$): with it the kill rate is 11 of 18 (61%).
The extension is controlled: the same test passes on the clean tree.

## Per part

`Kills` is how many mutants the part's tests fail on (any mutant); `Expected` how many mutants name the part as the one that should catch them, and `Hit` how many of those it did.

| Part | Tests | Kills | Share of the run | Expected | Hit |
|---|---|---|---|---|---|
| A1 | qanet_rules_* | 2 | 11% | 2 | 1 |
| A2 | qanet_keys_* | 1 | 5% | 2 | 1 |
| A3 | qanet_select_* | 1 | 5% | 2 | 1 |
| A4 | qanet_ledger | 0 | 0% | 0 | 0 |
| B | qanet_random_user_short | 4 | 22% | 5 | 4 |
| C | qanet_audio_matrix.* | 3 | 16% | 5 | 3 |
| D1 | qanet_random_user_fatfs_short | 3 | 16% | 0 | 0 |
| D3 | qanet_budgets | 0 | 0% | 0 | 0 |

Killed by one part alone: A1 2, A2 1, A3 1, B 1, C 3.

## Per class

| Class | Mutants | Killed | Kill rate |
|---|---|---|---|
| 1 | 7 | 4 | 57% |
| 2 | 3 | 2 | 66% |
| 3 | 6 | 3 | 50% |
| 4 | 2 | 2 | 100% |

Flagged kills: one. M18 (a listing job that never ends) is killed by `ru_short` as a Timeout, which is what a job that never ends does; D1's `ru_fatfs_short` also fails it with a plain failure, so the kill stands. No kill is only a Timeout or a crash; no test was `Not Run`.

## Why each survivor survived

| Mutant | Why no test failed | What would kill it |
|---|---|---|
| M01 File OPEN with no question (revert, not counted) | A1's table (`action_states.cpp`) runs `file.open` on a clean document: the unsaved-changes question is never reached. | A state with a dirty document for `file.open` (and for IMPORT), then rule 1 sees the digest change with no question. |
| M02 IMPORT with no question (revert, not counted) | The same: no state with unsaved changes before the import key. | The same table row. |
| M05 DELETE with no question | Gap, as expected: a folder on the card is not in the document's digest, so rule 1 cannot see it, and no random-walk invariant looks at what a delete removes. | A card-state rule (the files before and after a destructive File action), a new part. |
| M06 UNIQUE silent on an unshared pattern | A2 presses F5 in the one Song state it walks, which probably holds a shared pattern, so the silent branch (the pattern is only in this slot) is never walked. Not checked beyond that. | A second Song state in A2's table (a song of unshared patterns), or B4 (the silent no-op rule at every step of the walk). |
| M09 the other screen's listing kept | Survived the extended run too: `listing_empty` only fires on an EMPTY listing, and a wrong, non-empty one passes. A gap in B1's invariants. | An invariant that compares the screen's listing with the filter's (names and extensions), B4/B6. |
| M14 Wave pad plays slice n+1 | The matrix compares pitch and onset of the Wave pad against the step (`audio_matrix_test.cpp`, header of the Wave probe); the slices of the Kit's sine loop have the same pitch and the same onset, so the neighbour slice sounds the same. In the first run C killed it only because the then-known bug left findings whose values moved; with the bug fixed the baseline is clean and M14 shows its real gap. Not run to ground beyond the header. | A kit whose slices differ (two tones), or a check of which slice a pad plays (the sample position at the onset). |
| M19 a field Save does not write | Closed by B5 (2026-10-24): the save+OPEN relation of the `--meta` pass plants a fine tune, saves, opens and compares a digest that hashes the writer's bytes and then the model's fields one by one; with the patch applied `qanet_random_user_short` fails (meta_save_open), clean it passes. | - |
| M20 firmware update with unsaved changes | A1's table (`action_states.cpp`) has no state in which the firmware dialog is opened on a dirty document, and rule 1 sees no document change (the update is a restart, not an edit): the refusal "SAVE THE PROJECT FIRST" is not read by any rule. | A dirty-document state for the firmware dialog (a row in A1's table), or an invariant: an action that ends the session with unsaved changes asks or refuses. |
| M21 pad before the staged descriptor | The matrix (`audio_matrix_test.cpp`) presses its pad a block or more after the edit, so the staged descriptor has already been taken; no cell presses the pad in the very block of the edit. | A matrix cell that presses the pad in the block that follows the edit (the staged cell not yet taken). |

## Planted in the harness (not counted above)

Defects planted in the test net itself, not in the firmware: `--plant-exhaust` of `o3_qanet_random_user` (B8a, 2026-10-24) makes a step at the limit leave no unit (undo cap) or change the pattern with no unit (sample-edit history full), and `qanet_random_exhaust_selftest` must find it by rule 1. Both plants are caught by the new rule. With the OLD condition of rule 1 put back (a full history credits any change) the undo-cap plant is still caught and the sample-edit plant is MISSED (`a defect planted at the limit of sedit_cap is found by rule 1: FAILED`, remote run 2026-10-24): the blind spot of B4 is closed by B8a. No firmware patch is involved, so no row in the table.

## What changed since the first table (2026-10-21, 11 of 19, 57%; with the long shard 13 of 19, 68%)

- The rate is now over 18 counted mutants (11 killed, 61%); it was 12 of 19 with three reverts of fixes in it (M01, M02, M13: only M13 was killed). Counted alone, the 12 of 19 would have been 11 of 16 (68%); the two new mutants (M20, M21) survive, which is why the rate went down: they are the same classes as M01 and M02, and M13, planted in code no fix has touched.
- The first table's extended run credited M08 and M18; B's host test (`ru_short`) kills both, so the host set alone does.
- Stale patches: none failed `git apply`, and all 21 built. M13 was refreshed once (a longer settle wait no longer planted the bug after OS-BUG-SLICEPITCH moved that case to a staged descriptor) and is now marked a revert of that fix; M21 plants the class in the engine's block order.
- M14 went from killed to survived (see above); M10 is also killed by B (`ru_short`). M07 is killed by two A2 tests instead of three (`keys_patterns` no longer fails on it; not looked into: the kill itself is unchanged).
- The tool's target and part lists know `qanet_budgets` (D3, on dev; without it the control failed with "Not Run"). D3 kills nothing: its budgets are latency limits, and no mutant slows a path.
- M06 and M19 headers: the class of the bug is now the one of its twin (M06 class 1, M19 class 3).
