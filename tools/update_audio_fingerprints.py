#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""update_audio_fingerprints: rewrite tests/audio_fingerprints/*.json from the journeys that opt in (OS-E2E-4).

`cmake --build <dir> --target update_audio_fingerprints` runs this, the same way `update_goldens` runs tools/update_goldens.py. A scenario opts in with a
`fingerprint <render>` line (sim/include/o3os/sim/audio_fingerprint.h has what a fingerprint is and the rules it is compared by). For each such scenario one `o3sim`
process (at most --jobs at a time, at a low priority) runs it twice (`--twice`: the fingerprint must be the same text both times, or nothing is written),
rewrites <dir>/<scenario>.json, writes what moved, and the renders as WAV files for listening. A file whose scenario no longer opts in is removed, but only after every job has succeeded
(a failed run leaves the folder as the jobs that ran left it, never with a file gone).

Output, in --out (default <sim-bin>/fingerprint/, which is emptied first; a folder named with --out is never emptied, its other files stay):
  fingerprint_diff.md   per key: new / rewritten / unchanged / removed, and for each render the frames and bands that moved, the onsets, the f0, the levels (the review of a rebaseline reads this)
  wav/<scenario>_<render>.wav   what the journeys sounded like now (never committed)

A rewritten file is accepted only by a commit that also changes a firmware source directory (app/, audio/, core/, model/, seq/, formats/, modules/, module/), the scenario or the measuring rig, whose body
(two lines at least) gives the reason (tools/check_fingerprint_guard.py, ctest fingerprint_guard).

  update_audio_fingerprints.py --o3sim <exe> --sim-src <os/sim> --sim-bin <build/sim> --dir <tests/audio_fingerprints> [--cards <build/sim/cards>] [--out <dir>] [--jobs N]
  update_audio_fingerprints.py --self-test
"""
import argparse
import concurrent.futures
import contextlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def fingerprinted_scenarios(sim_src):
    """{scenario stem: path} of the scenarios (scenarios/*.txt and scenarios/*/*.txt) that have a `fingerprint` line. A stem is the key of the file."""
    root = Path(sim_src, "scenarios")
    out = {}
    for f in sorted(root.glob("*.txt")) + sorted(root.glob("*/*.txt")):
        for line in f.read_text(encoding="utf-8").splitlines():
            words = line.split("#", 1)[0].split()
            if words and words[0].lower() == "fingerprint":
                out[f.stem] = f
                break
    return out


def run_job(label, argv, nice):
    t0 = time.monotonic()
    proc = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                          preexec_fn=(lambda: os.nice(nice)) if nice else None)
    return label, proc.returncode, proc.stdout, time.monotonic() - t0


def status_of(before, after):
    if before is None and after is None:
        return "none"
    if before is None:
        return "NEW"
    if after is None:
        return "REMOVED"
    return "unchanged" if before == after else "REWRITTEN"


def report(statuses, parts):
    """The text of fingerprint_diff.md. `statuses`: {key: status}; `parts`: {key: markdown of o3sim}."""
    lines = ["# Audio fingerprint diff", "",
             "Written by `update_audio_fingerprints`. Read this with the WAVs beside it (`wav/`) before accepting a rewritten file: the reviewer accepts a changed fingerprint, "
             "and the commit that carries it must also change a firmware source directory, the scenario or the rig and say why in a body of two lines at least (`tools/check_fingerprint_guard.py`).", "",
             "| key | file |", "|---|---|"]
    for key in sorted(statuses):
        lines.append("| %s | %s |" % (key, statuses[key]))
    lines.append("")
    for key in sorted(parts):
        lines.append(parts[key].rstrip())
        lines.append("")
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--o3sim")
    ap.add_argument("--sim-src")
    ap.add_argument("--sim-bin")
    ap.add_argument("--dir", help="the fingerprint folder, tests/audio_fingerprints/")
    ap.add_argument("--cards", default="")
    ap.add_argument("--out", default="")
    ap.add_argument("--jobs", type=int, default=int(os.environ.get("O3OS_GOLDEN_JOBS", "4")))
    ap.add_argument("--nice", type=int, default=10)
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if not (args.o3sim and args.sim_src and args.sim_bin and args.dir):
        ap.error("--o3sim, --sim-src, --sim-bin and --dir are required")
    jobs = max(1, min(args.jobs, 4))  # never more than 4: the PC is shared
    out = Path(args.out) if args.out else Path(args.sim_bin, "fingerprint")
    fp_dir = Path(args.dir)
    scenarios = fingerprinted_scenarios(args.sim_src)
    if not scenarios:
        print("update_audio_fingerprints: no scenario has a `fingerprint` line", file=sys.stderr)
        return 2
    fp_dir.mkdir(parents=True, exist_ok=True)
    if not args.out:  # the build directory's own folder is ours to clear; a folder the caller named is never emptied (its other files stay)
        shutil.rmtree(out, ignore_errors=True)
    (out / "wav").mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="fp_parts_", dir=out))

    def read(path):
        return path.read_bytes() if path.exists() else None

    before = {p.stem: read(p) for p in fp_dir.glob("*.json")}
    statuses = {}
    queue = []
    for key, f in scenarios.items():
        argv = [args.o3sim, "--scenario", str(f), "--fingerprints", str(fp_dir), "--update-fingerprints",
                "--fingerprint-diff", str(work / (key + ".md")), "--wav", str(out / "wav"), "--twice"]
        if args.cards:
            argv += ["--cards", args.cards]
        queue.append((key, argv))

    failed = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = [pool.submit(run_job, key, argv, args.nice) for key, argv in queue]
        for fut in concurrent.futures.as_completed(futures):
            key, code, output, secs = fut.result()
            print("update_audio_fingerprints: %-28s exit %d  %5.1f s" % (key, code, secs), flush=True)
            if code != 0:
                failed.append(key)
                print(output, file=sys.stderr)
    if failed:
        print("update_audio_fingerprints: FAILED for %s: the files of those keys are as they were" % ", ".join(sorted(failed)), file=sys.stderr)
        shutil.rmtree(work, ignore_errors=True)
        return 1

    for key in before:  # a file with no scenario that opts in is stale: it goes, but only now that every job has succeeded
        if key not in scenarios:
            (fp_dir / (key + ".json")).unlink()

    parts = {}
    for key in set(scenarios) | set(before):
        statuses[key] = status_of(before.get(key), read(fp_dir / (key + ".json")))
        if key in scenarios:
            parts[key] = (work / (key + ".md")).read_text(encoding="utf-8")
        else:
            parts[key] = "## %s\n\nThe scenario no longer has a `fingerprint` line: the file is removed.\n" % key
    (out / "fingerprint_diff.md").write_text(report(statuses, parts), encoding="utf-8")
    shutil.rmtree(work, ignore_errors=True)
    moved = sorted(k for k, s in statuses.items() if s not in ("unchanged", "none"))
    print("update_audio_fingerprints: %d keys, %s" % (len(statuses), ("changed: " + ", ".join(moved)) if moved else "all unchanged"))
    print("update_audio_fingerprints: %s and the WAVs in %s" % (out / "fingerprint_diff.md", out / "wav"))
    if moved:
        print("update_audio_fingerprints: commit the files with a change of a firmware source directory, the scenario or the rig, and the reason in the body (ctest fingerprint_guard)")
    return 0


def self_test():
    failures = []

    def check(cond, what):
        if not cond:
            failures.append(what)

    with tempfile.TemporaryDirectory() as tmp:
        sim = Path(tmp, "sim")
        (sim / "scenarios" / "e2e").mkdir(parents=True)
        (sim / "scenarios" / "a.txt").write_text("render x 10\nfingerprint x\n")
        (sim / "scenarios" / "e2e" / "e2e_b.txt").write_text("render x 10\n# fingerprint x\nfingerprint x poly  # a comment\n")
        (sim / "scenarios" / "e2e" / "e2e_c.txt").write_text("render x 10\nassert audio x sound\n")
        (sim / "scenarios" / "e2e" / "e2e_d.txt").write_text("Fingerprint y\n")
        found = fingerprinted_scenarios(sim)
        check(sorted(found) == ["a", "e2e_b", "e2e_d"], "fingerprinted_scenarios: %s" % sorted(found))
    check(status_of(None, None) == "none" and status_of(None, b"x") == "NEW" and status_of(b"x", None) == "REMOVED", "status NEW/REMOVED/none")
    check(status_of(b"x", b"x") == "unchanged" and status_of(b"x", b"y") == "REWRITTEN", "status unchanged/REWRITTEN")
    text = report({"k1": "REWRITTEN", "k2": "unchanged"}, {"k1": "## k1\n\nmoved\n", "k2": "## k2\n\nsame\n"})
    check(text.startswith("# Audio fingerprint diff") and "| k1 | REWRITTEN |" in text and text.index("## k1") < text.index("## k2") and text.rstrip().endswith("same"), "report layout")
    # the whole run, against a stand-in for o3sim: a failed job leaves the stale file; a good run removes it; a folder named with --out keeps its other files
    with tempfile.TemporaryDirectory() as tmp:
        sim = Path(tmp, "sim")
        (sim / "scenarios").mkdir(parents=True)
        (sim / "scenarios" / "a.txt").write_text("render x 10\nfingerprint x\n")
        (sim / "scenarios" / "b.txt").write_text("render x 10\nfingerprint x\n# FAIL\n")
        fake = Path(tmp, "fake_o3sim.py")
        fake.write_text("#!/usr/bin/env python3\nimport sys\n"
                        "a = sys.argv\nscen = a[a.index('--scenario') + 1]\nif 'FAIL' in open(scen).read():\n    print('boom')\n    sys.exit(1)\n"
                        "name = scen.rsplit('/', 1)[-1][:-4]\nopen(a[a.index('--fingerprints') + 1] + '/' + name + '.json', 'w').write('new')\n"
                        "open(a[a.index('--fingerprint-diff') + 1], 'w').write('## ' + name + '\\n\\nmoved\\n')\n")
        fake.chmod(0o755)
        fps = Path(tmp, "fp")
        fps.mkdir()
        (fps / "a.json").write_text("old")
        (fps / "stale.json").write_text("stale")
        mine = Path(tmp, "mine")
        mine.mkdir()
        (mine / "keep.txt").write_text("keep")

        def run(*extra):
            old_argv, sys.argv = sys.argv, ["x", "--o3sim", str(fake), "--sim-src", str(sim), "--sim-bin", str(Path(tmp, "bin")), "--dir", str(fps), "--nice", "0", "--jobs", "1"] + list(extra)
            sink = io.StringIO()
            try:
                with contextlib.redirect_stdout(sink), contextlib.redirect_stderr(sink):
                    return main()
            finally:
                sys.argv = old_argv

        check(run("--out", str(mine)) == 1, "a failing job fails the run")
        check((fps / "stale.json").exists(), "a failed run leaves the stale file")
        (sim / "scenarios" / "b.txt").unlink()
        check(run("--out", str(mine)) == 0, "a good run succeeds")
        check(not (fps / "stale.json").exists() and (fps / "a.json").read_text() == "new", "a good run removes the stale file and rewrites the other")
        check((mine / "keep.txt").exists() and (mine / "fingerprint_diff.md").exists(), "--out is not emptied")
        check("| stale | REMOVED |" in (mine / "fingerprint_diff.md").read_text(), "the report says REMOVED")
        (Path(tmp, "bin", "fingerprint")).mkdir(parents=True)
        (Path(tmp, "bin", "fingerprint", "old.txt")).write_text("old")
        check(run() == 0 and not Path(tmp, "bin", "fingerprint", "old.txt").exists(), "the default folder is emptied first")
    for f in failures:
        print("update_audio_fingerprints self-test FAILED: " + f, file=sys.stderr)
    if not failures:
        print("update_audio_fingerprints self-test: ok")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
