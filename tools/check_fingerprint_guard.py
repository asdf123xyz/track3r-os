#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""check_fingerprint_guard: an audio fingerprint changes only with a reason (OS-E2E-4, docs/specs/OS-E2E.md).

Two checks, ctest `fingerprint_guard`:

 1. Keys. Every tests/audio_fingerprints/<key>.json belongs to the scenario sim/scenarios/**/<key>.txt, which has a `fingerprint` line; every scenario that has one has its file. (No git needed.)
 2. The guard. Every commit of <base>..HEAD (merge commits left out) that touches a tests/audio_fingerprints/<key>.json must, in that same commit, touch a firmware source directory (REASON_PREFIXES below: what sounds or plays can sit in the app, the model, the formats and the module host as well as in audio/), or the scenario of that
    key (sim/scenarios/**/<key>.txt), or the rig that measures and plays it (RIG below: a change of the analysis or of the simulator's engine wiring moves every print), and its message must have a body
    of at least two lines (the reason; trailers do not count). The uncommitted changes (staged, unstaged, untracked) count as one more commit with no message: the files they touch must satisfy the same rule.

<base> is $O3OS_FP_BASE, else the first of origin/dev, dev, origin/main, main that exists. With none (a shallow clone, an export) the guard is skipped (exit 77) after the key check.

  check_fingerprint_guard.py [--root <repo>] [--base <ref>]
  check_fingerprint_guard.py --self-test
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

FP_DIR = "tests/audio_fingerprints/"
REASON_PREFIXES = ("app/", "audio/", "core/", "model/", "seq/", "formats/", "modules/", "module/")
RIG = ("sim/src/audio_fingerprint.cpp", "sim/include/o3os/sim/audio_fingerprint.h", "sim/src/audio_check.cpp", "sim/include/o3os/sim/audio_check.h",
       "sim/src/sim_audio.cpp", "sim/include/o3os/sim/sim_audio.h")
MIN_BODY_LINES = 2
TRAILER = re.compile(r"^(Co-Authored-By|Signed-off-by|Reviewed-by|Acked-by|Fixes|Refs):\s", re.IGNORECASE)


def scenario_paths(root, key):
    """sim/scenarios/<key>.txt and sim/scenarios/<folder>/<key>.txt, as repository-relative paths."""
    base = Path(root, "sim", "scenarios")
    found = [p for p in list(base.glob(key + ".txt")) + list(base.glob("*/" + key + ".txt"))]
    return [p.relative_to(root).as_posix() for p in found]


def has_fingerprint_line(path):
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        words = line.split("#", 1)[0].split()
        if words and words[0].lower() == "fingerprint":
            return True
    return False


def check_keys(root):
    """The problems of the pairing of files and scenarios (a list of lines)."""
    problems = []
    keys = sorted(p.stem for p in Path(root, FP_DIR).glob("*.json"))
    for key in keys:
        paths = scenario_paths(root, key)
        if not paths:
            problems.append("%s%s.json has no scenario sim/scenarios/**/%s.txt" % (FP_DIR, key, key))
        elif not has_fingerprint_line(Path(root, paths[0])):
            problems.append("%s%s.json: the scenario %s has no `fingerprint` line" % (FP_DIR, key, paths[0]))
    base = Path(root, "sim", "scenarios")
    for f in sorted(list(base.glob("*.txt")) + list(base.glob("*/*.txt"))):
        if has_fingerprint_line(f) and f.stem not in keys:
            problems.append("%s has `fingerprint` lines but no %s%s.json (run update_audio_fingerprints)" % (f.relative_to(root).as_posix(), FP_DIR, f.stem))
    return problems


def judge(files, message, root):
    """Problems of one change: `files` it touches, `message` (None: not a commit)."""
    changed = sorted({f[len(FP_DIR):-len(".json")] for f in files if f.startswith(FP_DIR) and f.endswith(".json") and "/" not in f[len(FP_DIR):]})
    if not changed:
        return []
    problems = []
    general = any(f.startswith(REASON_PREFIXES) or f in RIG for f in files)
    for key in changed:
        if not general and not any(p in files for p in scenario_paths(root, key) + ["sim/scenarios/%s.txt" % key]):
            problems.append("%s%s.json changes, but the same change touches none of app/, audio/, core/, model/, seq/, formats/, modules/, module/, the scenario of %s or the rig that measures it" % (FP_DIR, key, key))
    if message is not None:
        body = [ln for ln in message.splitlines()[1:] if ln.strip() and not TRAILER.match(ln.strip())]
        if len(body) < MIN_BODY_LINES:
            problems.append("the message body has %d line(s), %d needed: it must say why the sound changed" % (len(body), MIN_BODY_LINES))
    return problems


def git(root, *args):
    return subprocess.run(["git", "-C", str(root)] + list(args), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def resolve_base(root, wanted):
    for ref in ([wanted] if wanted else []) + ["origin/dev", "dev", "origin/main", "main"]:
        if git(root, "rev-parse", "--verify", "--quiet", ref + "^{commit}").returncode == 0:
            return ref
    return None


def check_commits(root, base):
    problems = []
    listing = git(root, "rev-list", "--no-merges", "--reverse", base + "..HEAD")
    if listing.returncode != 0:
        return ["git rev-list %s..HEAD failed: %s" % (base, listing.stderr.strip())]
    for sha in listing.stdout.split():
        files = git(root, "diff-tree", "--no-commit-id", "--name-only", "-r", "--root", sha).stdout.split("\n")
        message = git(root, "log", "-1", "--format=%B", sha).stdout
        for p in judge([f for f in files if f], message, root):
            problems.append("commit %s (%s): %s" % (sha[:10], message.splitlines()[0] if message.strip() else "no message", p))
    pending = set(git(root, "diff", "--name-only", "HEAD").stdout.split("\n"))
    pending |= set(git(root, "ls-files", "--others", "--exclude-standard").stdout.split("\n"))
    for p in judge([f for f in pending if f], None, root):
        problems.append("uncommitted changes: " + p)
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=str(Path(__file__).resolve().parent.parent))
    ap.add_argument("--base", default=os.environ.get("O3OS_FP_BASE", ""))
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    problems = check_keys(args.root)
    for p in problems:
        print("fingerprint_guard: " + p, file=sys.stderr)
    if problems:
        return 1
    base = resolve_base(args.root, args.base)
    if base is None:
        print("fingerprint_guard: keys ok; no base ref (origin/dev, dev, origin/main, main) in this checkout: the commits are not checked (skipped)")
        return 77
    problems = check_commits(args.root, base)
    for p in problems:
        print("fingerprint_guard: " + p, file=sys.stderr)
    if problems:
        print("fingerprint_guard: a fingerprint changes with a firmware source directory (app/, audio/, core/, model/, seq/, formats/, modules/, module/), its scenario or the rig in the same commit, and the commit body (two lines at least) says why "
              "(tools/update_audio_fingerprints.py writes fingerprint_diff.md for the reviewer)", file=sys.stderr)
        return 1
    print("fingerprint_guard: keys ok, %s..HEAD ok" % base)
    return 0


def self_test():
    failures = []

    def check(cond, what):
        if not cond:
            failures.append(what)

    def run_git(root, *a):
        subprocess.run(["git", "-C", str(root), "-c", "user.name=t", "-c", "user.email=t@example.invalid", "-c", "commit.gpgsign=false"] + list(a),
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def write(root, rel, text):
        p = Path(root, rel)
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        run_git(root, "init", "-q", "-b", "dev")
        write(root, "sim/scenarios/e2e/e2e_k.txt", "render x 10\nfingerprint x\n")
        write(root, "sim/scenarios/e2e/e2e_other.txt", "render x 10\n")
        write(root, FP_DIR + "e2e_k.json", "{}")
        write(root, "audio/a.cpp", "1")
        run_git(root, "add", "-A")
        run_git(root, "commit", "-q", "-m", "base")
        check(check_keys(root) == [], "keys of a good tree: %s" % check_keys(root))
        check(resolve_base(root, "") == "dev", "base is dev")

        def commit(files, message):
            for rel, text in files.items():
                write(root, rel, text)
            run_git(root, "add", "-A")
            run_git(root, "commit", "-q", "-m", message)

        run_git(root, "branch", "base")
        # a fingerprint with the engine and a reason: ok
        commit({FP_DIR + "e2e_k.json": "{1}", "audio/a.cpp": "2"}, "fix(audio): the filter\n\nThe low-pass corner moved 3 Hz, the sound moved with it.\nThe three e2e keys that play it follow.\n\nCo-Authored-By: X <x@example.invalid>")
        check(check_commits(root, "base") == [], "audio + fingerprint + body: %s" % check_commits(root, "base"))
        # the same without a body (a trailer is not a body)
        commit({FP_DIR + "e2e_k.json": "{2}", "audio/a.cpp": "3"}, "fix(audio): again\n\nCo-Authored-By: X <x@example.invalid>")
        got = check_commits(root, "base")
        check(len(got) == 1 and "body has 0 line" in got[0], "no body: %s" % got)
        run_git(root, "reset", "-q", "--hard", "HEAD~1")
        # a fingerprint alone, and with an unrelated file: refused
        commit({FP_DIR + "e2e_k.json": "{3}", "docs/x.md": "x"}, "chore: rebaseline\n\nBecause.\nReally.")
        got = check_commits(root, "base")
        check(len(got) == 1 and "touches none of" in got[0], "fingerprint with docs only: %s" % got)
        run_git(root, "reset", "-q", "--hard", "HEAD~1")
        # a fingerprint with its own scenario: ok; with another key's scenario: refused
        commit({FP_DIR + "e2e_k.json": "{4}", "sim/scenarios/e2e/e2e_k.txt": "render x 10\nrender y 10\nfingerprint x\n"}, "test: the scenario\n\nA second render.\nIt plays the second note.")
        check(check_commits(root, "base") == [], "own scenario: %s" % check_commits(root, "base"))
        run_git(root, "reset", "-q", "--hard", "HEAD~1")
        commit({FP_DIR + "e2e_k.json": "{5}", "sim/scenarios/e2e/e2e_other.txt": "render x 10\nrender y 10\n"}, "test: other\n\nAnother scenario.\nNot this key's.")
        check(len(check_commits(root, "base")) == 1, "another key's scenario")
        run_git(root, "reset", "-q", "--hard", "HEAD~1")
        # the rig and modules/ count
        commit({FP_DIR + "e2e_k.json": "{6}", "sim/src/audio_fingerprint.cpp": "x"}, "fix(sim): the bands\n\nThe band edges moved.\nEvery print moves with them.")
        check(check_commits(root, "base") == [], "rig")
        run_git(root, "reset", "-q", "--hard", "HEAD~1")
        commit({FP_DIR + "e2e_k.json": "{7}", "modules/tb303/x.cpp": "x"}, "fix(tb303): decay\n\nThe decay is longer.\nThe acid line follows.")
        check(check_commits(root, "base") == [], "modules")
        run_git(root, "reset", "-q", "--hard", "HEAD~1")
        # every firmware source directory counts (a sound can come from the app, the model, the formats or the module host), but one line of body does not
        for d in ("app", "core", "model", "formats", "module"):
            commit({FP_DIR + "e2e_k.json": "{d-%s}" % d, d + "/x.cpp": "x"}, "fix(%s): a sound\n\nThe %s changed what plays.\nThe key follows." % (d, d))
            check(check_commits(root, "base") == [], "%s/ counts: %s" % (d, check_commits(root, "base")))
            run_git(root, "reset", "-q", "--hard", "HEAD~1")
        commit({FP_DIR + "e2e_k.json": "{terse}", "audio/a.cpp": "terse"}, "fix(audio): terse\n\nBecause.\n\nCo-Authored-By: X <x@example.invalid>")
        got = check_commits(root, "base")
        check(len(got) == 1 and "body has 1 line" in got[0], "one line of body: %s" % got)
        run_git(root, "reset", "-q", "--hard", "HEAD~1")
        # a commit that does not touch a fingerprint is none of the guard's business
        commit({"docs/y.md": "y"}, "docs: y")
        check(check_commits(root, "base") == [], "no fingerprint")
        # the working tree is one more change, with no message
        write(root, FP_DIR + "e2e_k.json", "{8}")
        got = check_commits(root, "base")
        check(len(got) == 1 and got[0].startswith("uncommitted changes"), "pending fingerprint alone: %s" % got)
        write(root, "audio/a.cpp", "9")
        check(check_commits(root, "base") == [], "pending fingerprint with audio")
        run_git(root, "checkout", "-q", "--", ".")
        # keys: a file without a scenario, a scenario without a file, a scenario without the line
        write(root, FP_DIR + "e2e_nope.json", "{}")
        write(root, "sim/scenarios/e2e/e2e_new.txt", "render x 10\nfingerprint x\n")
        write(root, FP_DIR + "e2e_other.json", "{}")
        got = check_keys(root)
        check(len(got) == 3 and any("e2e_nope" in g and "no scenario" in g for g in got) and any("e2e_new.txt" in g for g in got) and any("e2e_other" in g and "no `fingerprint` line" in g for g in got), "keys: %s" % got)
    for f in failures:
        print("check_fingerprint_guard self-test FAILED: " + f, file=sys.stderr)
    if not failures:
        print("check_fingerprint_guard self-test: ok")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
