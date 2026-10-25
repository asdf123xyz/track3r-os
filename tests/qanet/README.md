# OS-QANET tests

The test net of `docs/specs/OS-QANET.md` (ctest label `qanet`). Each part has its own binary and baseline file (a test fails on a finding that is not in its baseline). The files' headers say what each one holds.

## How strong is the net: planted bugs (F1)

`mutants/` holds 18 small patches that count (plus a few reverts of fixes, `# revert:`), each a planted bug of one class of spec section 2 (the "mutant"), with a header (ID, class, the known bug it is the twin of, the part expected to catch it). **A mutant is never committed into a product file and never applied to the tree you work in.** It is applied only in a throwaway `git worktree` under `build-mutants/`.

```
python3 -I tools/qanet_mutants.py --list                      # the mutants and their headers
python3 -I tools/qanet_mutants.py --run                       # all of them: about 6 minutes a mutant under the sanitizers on the build machine (2 hours for the set)
python3 -I tools/qanet_mutants.py --run --only M01,M07        # some of them
python3 -I tools/qanet_mutants.py --self-test                 # the dry run (ctest qanet_mutants_selftest): headers and parser, no build
```

`--run` refuses a tree with changes to tracked files (the worktree is made from HEAD: commit first). It builds the qanet binaries on the clean tree once as the control (a test that fails there voids the run), then for each patch applies it (`git apply`), builds incrementally (one changed file, ccache), runs `ctest -L qanet -LE e2e -E selftest` and reverts (`git apply -R`; a crash reverts too). Builds and tests run under `nice -n 10 taskset -c 2-19`, build `-j4`, ctest `-j2` (`--prefix ""` for none). `ci.sh` has no mode for it: on the build machine run it in the lane's worktree there (after `tools/remote_ci.sh` has sent the tree), inside one CI turn, with `--prefix "nice -n 10" --jobs 8`. The tool lists the qanet binaries it builds (`TARGETS`) and the test-to-part map (`PARTS`): a new qanet binary or part goes in both, or the control fails with `Not Run`. A patch that does not apply to HEAD is `stale`.

`--extend <regex>` re-runs the survivors that B or D1 expected with a long test (label e2e); that test is run once on the clean tree first, and when it fails there the extension is void. `--only M08,M09 --merge build-mutants/kill_table.json` re-runs a few mutants and takes the other rows from an earlier table.

Output: `build-mutants/kill_table.md` (untracked): a row per mutant (mutant, class, twin, expected part, which tests killed it with the failure kind when it is a Timeout or a crash, verdict killed / survived / stale / broken), the kill rate per part and per class, and the summary line printed last. The committed copy of the latest run is `docs/qa/qanet-kill-table.md`. It is run once per part merge, on demand: not a ctest, not part of `tools/ci.sh host`.

Adding a mutant: write the patch against HEAD (`git diff` of one product file, a few lines), put the `# mutant / class / twin / expected / effect` header above the `diff --git` line, name the file `M<nn>_<slug>.patch`, run it alone with `--only`. The header names the known bug the mutant is a twin of, and its class is that bug's class in `docs/qa/qanet-ledger.md`; the patch itself plants the same kind of bug elsewhere in the code, not that bug's own code change put back. A patch that does put back what a fix removed (OS-OPENGUARD in `file_actions.cpp`, OS-BUG-SLICEPITCH's staged descriptor) carries a `# revert:` header line: it is run and shown in the table, but not counted in the kill rate. Known bugs on dev now (`docs/qa/qanet-ledger.md`, `docs/roadmap/BACKLOG.md`): only OS-BUG-NEWLIST is open (cause unknown, left for the maintainer's hands session); OS-BUG-SLDOVER, SLICEPITCH, ADDSLICE, PREVLAT, WATCHSEL and OS-SLDTARGET are fixed on dev (OS-QAFIX, unit check pending), so a mutant that names one as its twin stands for a bug of that kind that could come back. A mutant that no part kills is a result, not a failure: it names a gap (write the part that closes it, then the mutant is killed).
