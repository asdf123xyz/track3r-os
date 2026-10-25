#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""OS-QANET F1: bug seeding, the strength of the net (docs/specs/OS-QANET.md, parts 8a and 10).

    python3 -I tools/qanet_mutants.py --run [--only M01,M02] [--jobs 2]     the full run (about 10 minutes a mutant): the kill table
    python3 -I tools/qanet_mutants.py --run --only M08 --extend <regex> --merge build-mutants/kill_table.json   a partial run: the other rows come from an earlier kill_table.json
    python3 -I tools/qanet_mutants.py --self-test [--root <repo>]            the ctest `qanet_mutants_selftest`: every patch has its header, the parser and the table work (no build, no git)
    python3 -I tools/qanet_mutants.py --list                                 the mutants and their headers

A mutant is a patch of tests/qanet/mutants/ (git diff format, one product file, a few lines): a planted bug of one class of section 2 of the spec, which stands for a known bug of the same class
(its "twin": the header names the known bug it stands for; the patch is never that bug's own code change). A patch is NEVER applied to the tree you work in, nor committed: `--run` makes (or reuses) a throwaway `git worktree` under build-mutants/ at the HEAD of this
tree, builds the qanet targets there once as the control (a test that fails on the clean tree voids the run), then for each patch: `git apply` it, build incrementally (one changed file: ccache),
run `ctest -L qanet -LE e2e` (the tests named `*selftest*` test the net's own machinery with planted faults and do not look at the product: `--ctest-exclude`), record which tests failed, and revert with
`git apply -R` (a `finally` and the SIGINT / SIGTERM handlers revert on a crash too; a tree that does not come back clean is reset). The tool refuses to run when the tree it was started in has changes to tracked files
(the worktree is made from HEAD: what is not committed would not be in it). A patch that does not apply to HEAD is reported `stale`; one that applies but does not build is `broken`.
Verdict: killed (at least one qanet test fails on the mutant and passes on the clean tree), survived (all pass). The kind of each failure is recorded (ctest's own word: Failed, Timeout, Exception: SegFault ...): a kill that is only
a Timeout or a crash is flagged in the table (the test did not assert anything: it hung or died), and a test that did not run (`Not Run`) is never a kill (a run with only those is `broken`). A mutant with a `# revert:` header is not counted in the kill rate.
`--extend <regex>` gives a survivor whose expected part is a sampled one (B, D1) a second chance with a long ctest (label e2e); that long test is run ONCE on the clean tree first, as the control of
the extension: when it fails there, the extension is void (no mutant is credited with it) and the table says so. `--merge <kill_table.json>` fills the rows of the mutants not run now from an
earlier table (headers are re-read from the patches), so a re-run of a few mutants gives the whole table; an earlier extended result is dropped unless this run controlled the extension.

The kill table is written to build-mutants/kill_table.md (untracked) and build-mutants/kill_table.json: one row per mutant (mutant | class | twin | expected part | killed by | verdict), then the kill
rate per part (which part kills how many) and per class. The summary line is the last line printed. Builds are local and run under `nice -n 10 taskset -c 2-19` at -j4 (CLAUDE.md); it is not a ctest
and not part of `ci.sh host` (spec 11: once per part merge, on demand). Stdlib only.

A patch starts with a header of `# key: value` lines (git apply ignores what comes before the first `diff --git`):
    # mutant:   M01                       the ID, the file name starts with it (M01_open_no_question.patch)
    # class:    1                         the class of section 2 of the spec (1 spec is the oracle, 2 sequences between screens, 3 two paths for one thing, 4 simulator differs from the unit)
    # twin:     OS-BUG-SLDOVER ...        the known bug this one stands for (or the kind of bug, when the class has no known one)
    # expected: A1                        the part that should catch it (A1 A2 A3 B C D1), or `none (B5)`: a gap no part covers yet, expected to survive
    # effect:   one line                  what the planted bug does
    # revert:   OS-OPENGUARD (...)        optional: the patch puts back a bug that a fix removed (a revert of a fix, not a twin elsewhere): it is run and shown, but not counted in the kill rate
"""

import argparse
import datetime
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile

HEADER_KEYS = ("mutant", "class", "twin", "expected", "effect")
CLASSES = ("1", "2", "3", "4")
# The part a failed test belongs to: the first pattern that matches. One row per binary of tests/qanet (and the ledger gate).
PARTS = (
    (re.compile(r"^qanet_rules_"), "A1"),
    (re.compile(r"^qanet_keys_"), "A2"),
    (re.compile(r"^qanet_select_"), "A3"),
    (re.compile(r"^qanet_ledger"), "A4"),
    (re.compile(r"^qanet_random_user_fatfs"), "D1"),
    (re.compile(r"^qanet_(random_user|shrink|assert)"), "B"),
    (re.compile(r"^qanet_audio_matrix"), "C"),
    (re.compile(r"^qanet_budgets"), "D3"),
)
PART_ORDER = ("A1", "A2", "A3", "A4", "B", "C", "D1", "D3", "other")
TARGETS = ("o3_qanet_random_user", "o3os_qanet_rules", "o3os_qanet_keys", "o3os_qanet_select", "o3os_qanet_audio", "o3_qanet_budgets", "o3sim", "card_images")
DEFAULT_PREFIX = "nice -n 10 taskset -c 2-19"
CTEST_EXCLUDE = "selftest"


def part_of(test):
    for pat, name in PARTS:
        if pat.search(test):
            return name
    return "other"


def expected_part(text):
    """`A1`, `B1 (and C2)` -> the parts a mutant is expected to be killed by; `none ...` -> [] (a gap)."""
    t = text.strip().lower()
    if t.startswith("none"):
        return []
    out = []
    for m in re.finditer(r"\b([A-D])([0-9]?)\b", text):
        letter, digit = m.group(1), m.group(2)
        part = letter + digit if letter in "AD" and digit else (letter if letter in "BC" else None)
        if part and part not in out:
            out.append(part)
    return out


def parse_header(text, name):
    """The `# key: value` lines before the first `diff --git`; returns (header dict, problems)."""
    head = {}
    problems = []
    seen_diff = False
    for line in text.splitlines():
        if line.startswith("diff --git"):
            seen_diff = True
            break
        m = re.match(r"#\s*([a-z]+):\s*(.*\S)\s*$", line)
        if m:
            head[m.group(1)] = m.group(2)
    if not seen_diff:
        problems.append("%s: no `diff --git` line" % name)
    for k in HEADER_KEYS:
        if k not in head:
            problems.append("%s: header has no `# %s:` line" % (name, k))
    if "mutant" in head and not name.startswith(head["mutant"] + "_"):
        problems.append("%s: the file name does not start with the header's mutant ID %s_" % (name, head["mutant"]))
    if "class" in head and head["class"] not in CLASSES:
        problems.append("%s: class %r is not one of 1 2 3 4 (spec section 2)" % (name, head["class"]))
    if "expected" in head and not head["expected"].lower().startswith("none") and not expected_part(head["expected"]):
        problems.append("%s: expected %r names no part (A1 A2 A3 B C D1) and does not start with `none`" % (name, head["expected"]))
    files = re.findall(r"^diff --git a/(\S+) b/", text, re.M)
    if len(files) != 1:
        problems.append("%s: a mutant patches exactly one file (found %d)" % (name, len(files)))
    for f in files:
        if f.startswith(("tests/", "tools/", "docs/", "sim/scenarios/")):
            problems.append("%s: patches %s: a mutant plants a bug in a product file, never in the net" % (name, f))
    return head, problems


def load_mutants(root):
    d = os.path.join(root, "tests", "qanet", "mutants")
    out = []
    problems = []
    if not os.path.isdir(d):
        return out, ["tests/qanet/mutants/ does not exist"]
    for name in sorted(os.listdir(d)):
        if not name.endswith(".patch"):
            continue
        with open(os.path.join(d, name), encoding="utf-8") as f:
            text = f.read()
        head, p = parse_header(text, name)
        problems += p
        out.append({"id": head.get("mutant", name.split("_")[0]), "file": name, "path": os.path.join(d, name), "head": head})
    ids = [m["id"] for m in out]
    for i in set(ids):
        if ids.count(i) > 1:
            problems.append("mutant ID %s is used by more than one patch" % i)
    return out, problems


def ctest_kinds(output):
    """The tests that did not pass in a `ctest` run, in order, with the kind of each: the lines `N/M Test #K: name ..... ***Failed  1.2 sec`
    give `Failed` (an assertion or a sanitizer exit), `Timeout`, `SegFault` (or the other word after `***Exception:`) and `Not Run`."""
    kinds = {}
    for line in output.splitlines():
        if "***" not in line and "Not Run" not in line:
            continue
        m = re.search(r"Test +#\d+: (\S+) +\.+", line)
        if not m or m.group(1) in kinds:
            continue
        k = re.search(r"\*\*\*(.+?)\s*(?:\d+\.\d+ sec)?\s*$", line)
        kind = k.group(1).strip() if k else "Not Run"
        if kind.startswith("Exception:"):
            kind = kind[len("Exception:"):].strip() or "Exception"
        kinds[m.group(1)] = kind
    return kinds


def failed_tests(output):
    """The names of the tests that did not pass (`ctest_kinds`)."""
    return list(ctest_kinds(output))


def kill_kinds(kinds):
    """(the tests that count as failing, the kinds that are not a plain `Failed`): `Not Run` is no failure of the product."""
    names = [n for n, k in kinds.items() if k != "Not Run"]
    return names, {n: kinds[n] for n in names if kinds[n] != "Failed"}


def merge_results(old, new, mutants, keep_ext):
    """The rows of a full table: this run's results, and for every other mutant of `mutants` its row of `old` (an earlier kill_table.json),
    with the header re-read from the patch. An old extended result is kept only when `keep_ext` (this run controlled the extension)."""
    have = {r["id"]: r for r in new}
    oldby = {r["id"]: r for r in old}
    out = []
    for m in mutants:
        if m["id"] in have:
            out.append(have[m["id"]])
        elif m["id"] in oldby:
            r = dict(oldby[m["id"]], head=m["head"], file=m["file"], merged=True)
            if not keep_ext:
                r.pop("failed_ext", None)
            out.append(r)
    return out


# ---- the table --------------------------------------------------------------------------------------------------------------------------------

def render(results, info):
    ran = [r for r in results if r["verdict"] in ("killed", "survived")]
    reverts = [r for r in ran if r["head"].get("revert")]
    run = [r for r in ran if not r["head"].get("revert")]  # the rate counts the mutants that plant a bug elsewhere, not the reverts of a fix
    killed = [r for r in run if r["verdict"] == "killed"]
    weak = [r for r in killed if weak_kill(r)]
    void = info.get("extend_void")
    longer = [r for r in run if r["verdict"] == "survived" and r.get("failed_ext") and not void]
    lines = ["# OS-QANET kill table", ""]
    lines.append("Run %s against %s (%s)." % (info.get("date", "?"), info.get("commit", "?"), info.get("what", "the clean tree plus one patch at a time")))
    lines.append("Each row is one planted bug (tests/qanet/mutants/); `killed by` lists the parts whose tests failed (the tests are in brackets, with the kind when it is not a plain failure: a Timeout or a crash). A mutant is killed when at least one qanet test fails on it and none fails on the clean tree. A row marked `revert` puts back a bug that a fix removed: it is shown, but not counted.")
    lines.append("")
    lines.append("| Mutant | Class | Twin of | Expected part | Killed by | Verdict |")
    lines.append("|---|---|---|---|---|---|")
    for r in results:
        h = r["head"]
        by = []
        for part in PART_ORDER:
            ts = [t for t in r.get("failed", []) if part_of(t) == part]
            if ts:
                by.append("%s [%s]" % (part, ", ".join(shorten(ts, r.get("kinds")))))
        verdict = r["verdict"]
        if r in weak or (r in reverts and weak_kill(r)):
            verdict += " (FLAG: only a %s)" % "/".join(sorted(set(r["kinds"].values())))
        if r in reverts:
            verdict += " (revert of %s: not counted)" % r["head"]["revert"].split(" ")[0]
        if r in longer:
            verdict = "survived (killed by the extended run: %s)" % ", ".join(shorten(r["failed_ext"], r.get("kinds_ext")))
        elif r["verdict"] == "survived" and "failed_ext" in r and not void:
            verdict = "survived (also the extended run)"
        lines.append("| %s | %s | %s | %s | %s | %s |" % (r["id"], h.get("class", "?"), cell(h.get("twin", "")), cell(h.get("expected", "")), cell("; ".join(by) if by else "-"), verdict))
    lines.append("")
    rate = (100 * len(killed) // len(run)) if run else 0
    lines.append("**Kill rate: %d of %d mutants killed (%d%%)** by the tests of the qanet label (`ctest -L qanet -LE e2e`, selftests apart); survived %d, stale %d, broken %d." % (
        len(killed), len(run), rate, len(run) - len(killed), sum(1 for r in results if r["verdict"] == "stale"), sum(1 for r in results if r["verdict"] == "broken")))
    if weak:
        lines.append("")
        lines.append("Flagged: %d of the kills are only a Timeout or a crash, not an assertion (%s): the test hung or died on the planted bug, it did not name it." % (len(weak), ", ".join(r["id"] for r in weak)))
    if reverts:
        lines.append("")
        lines.append("Not counted: %d mutant(s) put back a bug that a fix removed (%s); %d of them killed. They show that the fix has a test, not how strong the net is on bugs not yet found." % (
            len(reverts), ", ".join(r["id"] for r in reverts), sum(1 for r in reverts if r["verdict"] == "killed")))
    if void:
        lines.append("")
        lines.append("The extended run is VOID: its test (%s) fails on the clean tree (%s), so no mutant is credited with it; the kill rate is the host set's." % (info.get("extend", "?"), ", ".join(void)))
    elif any("failed_ext" in r for r in run):
        lines.append("")
        lines.append("Of the %d survivors, %d are killed by the extended run (the long run of the random user, %s): with it the kill rate is %d of %d (%d%%)." % (
            len(run) - len(killed), len(longer), info.get("extend", "a shard of the e2e label"), len(killed) + len(longer), len(run), (100 * (len(killed) + len(longer)) // len(run)) if run else 0))
        lines.append("The extension is controlled: the same test passes on the clean tree.")
    lines.append("")
    lines.append("## Per part")
    lines.append("")
    lines.append("`Kills` is how many mutants the part's tests fail on (any mutant); `Expected` how many mutants name the part as the one that should catch them, and `Hit` how many of those it did.")
    lines.append("")
    lines.append("| Part | Tests | Kills | Share of the run | Expected | Hit |")
    lines.append("|---|---|---|---|---|---|")
    for part in PART_ORDER:
        kills = sum(1 for r in run if any(part_of(t) == part for t in r.get("failed", [])))
        exp = [r for r in run if part in expected_part(r["head"].get("expected", ""))]
        hit = sum(1 for r in exp if any(part_of(t) == part for t in r.get("failed", [])))
        if part == "other" and kills == 0:
            continue
        lines.append("| %s | %s | %d | %d%% | %d | %d |" % (part, PART_TESTS.get(part, "-"), kills, (100 * kills // len(run)) if run else 0, len(exp), hit))
    only = {}
    for r in killed:
        ps = sorted({part_of(t) for t in r.get("failed", [])})
        if len(ps) == 1:
            only[ps[0]] = only.get(ps[0], 0) + 1
    if only:
        lines.append("")
        lines.append("Killed by one part alone: " + ", ".join("%s %d" % (p, only[p]) for p in PART_ORDER if p in only) + ".")
    lines.append("")
    lines.append("## Per class")
    lines.append("")
    lines.append("| Class | Mutants | Killed | Kill rate |")
    lines.append("|---|---|---|---|")
    for c in CLASSES:
        rs = [r for r in run if r["head"].get("class") == c]
        ks = [r for r in rs if r["verdict"] == "killed"]
        lines.append("| %s | %d | %d | %s |" % (c, len(rs), len(ks), ("%d%%" % (100 * len(ks) // len(rs))) if rs else "-"))
    surv = [r for r in run if r["verdict"] == "survived"]
    surv_all = [r for r in ran if r["verdict"] == "survived"]
    if surv_all:
        lines.append("")
        lines.append("## Survivors")
        lines.append("")
        for r in surv_all:
            lines.append("- %s (class %s, expected %s%s): %s" % (r["id"], r["head"].get("class", "?"), r["head"].get("expected", "?"), ", revert: not counted" if r in reverts else "", r["head"].get("effect", "")))
    lines.append("")
    ext = "; the extended run is void" if void else (("; %d more killed by the extended run (%d%%)" % (len(longer), 100 * (len(killed) + len(longer)) // len(run))) if longer and run else "")
    extra = ("; %d only by a Timeout or a crash" % len(weak) if weak else "") + ("; %d revert(s) of fixes not counted" % len(reverts) if reverts else "")
    return "\n".join(lines), "mutants: %d run, %d killed, %d survived, %d stale, %d broken; kill rate %d%%%s%s" % (
        len(run), len(killed), len(surv), sum(1 for r in results if r["verdict"] == "stale"), sum(1 for r in results if r["verdict"] == "broken"), rate, ext, extra)


PART_TESTS = {
    "A1": "qanet_rules_*",
    "A2": "qanet_keys_*",
    "A3": "qanet_select_*",
    "A4": "qanet_ledger",
    "B": "qanet_random_user_short",
    "C": "qanet_audio_matrix.*",
    "D1": "qanet_random_user_fatfs_short",
    "D3": "qanet_budgets",
}


def cell(s):
    return s.replace("|", "/").strip()


def weak_kill(r):
    """A kill whose every failing test is a Timeout or a crash (no plain `Failed`): flagged in the table."""
    kinds = r.get("kinds") or {}
    return r["verdict"] == "killed" and bool(r.get("failed")) and all(t in kinds for t in r["failed"])


def shorten(tests, kinds=None):
    out = []
    kinds = kinds or {}
    tests = sorted(tests, key=lambda t: t not in kinds)  # the flagged (a Timeout, a crash) come first, so the cell never hides one behind "+N"
    for t in tests[:3]:
        out.append(t.replace("qanet_audio_matrix.", "audio:").replace("qanet_random_user_", "ru_").replace("qanet_", "") + (" (%s)" % kinds[t] if t in kinds else ""))
    if len(tests) > 3:
        out.append("+%d" % (len(tests) - 3))
    return out


# ---- the run ----------------------------------------------------------------------------------------------------------------------------------

def sh(cmd, cwd=None, check=True, capture=True, env=None):
    p = subprocess.run(cmd, cwd=cwd, capture_output=capture, text=True, env=env)
    if check and p.returncode != 0:
        raise RuntimeError("%s failed (%d): %s" % (" ".join(cmd[:4]), p.returncode, ((p.stderr or "") + (p.stdout or "")).strip()[-600:]))
    return p


class Run:
    def __init__(self, a):
        self.root = os.path.abspath(a.root)
        self.out = os.path.abspath(a.out or os.path.join(self.root, "build-mutants"))
        self.tree = os.path.join(self.out, "tree")
        self.build = os.path.join(self.tree, "build-os-host")
        self.jobs = a.jobs
        self.prefix = a.prefix.split() if a.prefix else []
        self.exclude = a.ctest_exclude
        self.include = a.ctest_only
        self.extend = a.extend
        self.merge = a.merge
        self.extend_parts = set(a.extend_parts.split(",")) if a.extend_parts else set()
        self.targets = a.targets.split(",") if a.targets else list(TARGETS)
        self.only = set(a.only.split(",")) if a.only else None
        self.build_only = a.build_only
        self.applied = None
        self.extend_void = []

    def log(self, msg):
        print("qanet_mutants: " + msg, flush=True)

    def refuse_dirty(self):
        p = sh(["git", "-C", self.root, "status", "--porcelain", "--untracked-files=no"])
        if p.stdout.strip():
            raise SystemExit("qanet_mutants: the tree has changes to tracked files (the throwaway worktree is made from HEAD and would not have them): commit or stash first\n" + p.stdout.strip()[:400])

    def make_tree(self):
        head = sh(["git", "-C", self.root, "rev-parse", "HEAD"]).stdout.strip()
        if os.path.realpath(self.tree) == os.path.realpath(self.root):
            raise SystemExit("qanet_mutants: the throwaway tree is this tree")
        os.makedirs(self.out, exist_ok=True)
        listed = [os.path.realpath(l[len("worktree "):]) for l in sh(["git", "-C", self.root, "worktree", "list", "--porcelain"]).stdout.splitlines() if l.startswith("worktree ")]
        if os.path.realpath(self.tree) in listed:
            sh(["git", "-C", self.tree, "reset", "--hard", "-q"])
            sh(["git", "-C", self.tree, "checkout", "--detach", "-q", head])
        else:
            if os.path.exists(self.tree):
                raise SystemExit("qanet_mutants: %s exists and is not a worktree of this repository: remove it" % self.tree)
            sh(["git", "-C", self.root, "worktree", "add", "--detach", self.tree, head])
        return head

    def configure(self):
        if not os.path.isfile(os.path.join(self.build, "CMakeCache.txt")):
            self.log("configuring %s" % self.build)
            sh(self.prefix + ["cmake", "-S", self.tree, "-B", self.build, "-DO3OS_SANITIZE=ON"])

    def build_targets(self):
        p = sh(self.prefix + ["cmake", "--build", self.build, "-j%d" % self.jobs, "-t"] + self.targets, check=False)
        return p.returncode == 0, ((p.stdout or "") + (p.stderr or ""))[-800:]

    def run_ctest(self, extended=False):
        cmd = self.prefix + ["ctest", "--test-dir", self.build, "-L", "qanet", "-j2", "--no-tests=error"]
        if extended:
            cmd += ["-R", self.extend]  # the long run's shard: label e2e is wanted here
        else:
            cmd += ["-LE", "e2e"]
            if self.include:
                cmd += ["-R", self.include]
            if self.exclude:
                cmd += ["-E", self.exclude]
        p = sh(cmd, check=False)
        text = (p.stdout or "") + (p.stderr or "")
        return ctest_kinds(text), text

    def revert(self, patch):
        if self.applied is None:
            return
        p = sh(["git", "-C", self.tree, "apply", "-R", patch], check=False)
        self.applied = None
        dirty = sh(["git", "-C", self.tree, "status", "--porcelain", "--untracked-files=no"], check=False).stdout.strip()
        if p.returncode != 0 or dirty:
            self.log("the revert did not leave the tree clean: resetting it")
            sh(["git", "-C", self.tree, "reset", "--hard", "-q"], check=False)

    def one(self, m):
        res = {"id": m["id"], "file": m["file"], "head": m["head"], "failed": [], "verdict": "stale"}
        chk = sh(["git", "-C", self.tree, "apply", "--check", m["path"]], check=False)
        if chk.returncode != 0:
            res["note"] = (chk.stderr or "").strip()[:200]
            return res
        try:
            sh(["git", "-C", self.tree, "apply", m["path"]])
            self.applied = m["path"]
            ok, tail = self.build_targets()
            if not ok:
                res["verdict"] = "broken"
                res["note"] = tail[-300:]
                return res
            if self.build_only:
                res["verdict"] = "built"
                return res
            kinds, _ = self.run_ctest()
            failed, odd = kill_kinds(kinds)
            res["failed"] = failed
            if odd:
                res["kinds"] = odd
            res["verdict"] = "killed" if failed else "survived"
            if len(failed) < len(kinds):  # a test that did not run says nothing about the product: never a kill
                res["not_run"] = [n for n in kinds if n not in failed]
                if not failed:
                    res["verdict"] = "broken"
                    res["note"] = "tests did not run: " + ", ".join(res["not_run"])
            if res["verdict"] == "survived" and self.extend and not self.extend_void and self.extend_parts & set(expected_part(m["head"].get("expected", ""))):
                self.log("  survived the host set: the extended run %s" % self.extend)
                ext, ext_odd = kill_kinds(self.run_ctest(True)[0])
                res["failed_ext"] = ext
                if ext_odd:
                    res["kinds_ext"] = ext_odd
            return res
        finally:
            self.revert(m["path"])

    def go(self):
        self.refuse_dirty()
        mutants, problems = load_mutants(self.root)
        if problems:
            raise SystemExit("qanet_mutants: the patches have problems:\n  " + "\n  ".join(problems))
        every = mutants
        old = {}
        if self.merge:
            with open(self.merge, encoding="utf-8") as f:
                old = json.load(f)
        if self.only:
            unknown = self.only - {m["id"] for m in mutants}
            if unknown:
                raise SystemExit("qanet_mutants: no such mutant: " + ", ".join(sorted(unknown)))
            mutants = [m for m in mutants if m["id"] in self.only]
        head = self.make_tree()
        handlers = {}

        def stop(signum, _frame):
            raise SystemExit("qanet_mutants: interrupted (signal %d)" % signum)
        for s in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            handlers[s] = signal.signal(s, stop)
        results = []
        try:
            self.configure()
            self.log("control: the clean tree at %s" % head[:8])
            ok, tail = self.build_targets()
            if not ok:
                raise SystemExit("qanet_mutants: the clean tree does not build:\n" + tail)
            if not self.build_only:
                failed, text = self.run_ctest()
                if failed:
                    raise SystemExit("qanet_mutants: the control run fails on the clean tree, the run is void: " + ", ".join("%s (%s)" % kv for kv in failed.items()))
                if self.extend:
                    self.log("control of the extension: %s on the clean tree" % self.extend)
                    ext_failed, _ = self.run_ctest(True)
                    if ext_failed:
                        ext_failed = list(ext_failed)
                        self.extend_void = ext_failed
                        self.log("  the extension fails on the clean tree: it is void (%s)" % ", ".join(ext_failed))
            for m in mutants:
                self.log("%s %s" % (m["id"], m["head"].get("effect", "")))
                r = self.one(m)
                self.log("  -> %s%s" % (r["verdict"], (" by " + ", ".join(shorten(r["failed"], r.get("kinds")))) if r["failed"] else ""))
                results.append(r)
        finally:
            for s, h in handlers.items():
                signal.signal(s, h)
        if self.build_only:
            for r in results:
                print("%s: %s%s" % (r["id"], r["verdict"], (" " + r.get("note", "").replace("\n", " ")[-200:]) if r["verdict"] != "built" else ""))
            return 0
        info = {"date": datetime.date.today().isoformat(), "commit": head[:8], "extend": self.extend}
        if self.extend_void:
            info["extend_void"] = self.extend_void
        if old:
            results = merge_results(old.get("results", []), results, every, bool(self.extend) and not self.extend_void)
            info["merged"] = "%d row(s) from the run of %s at %s" % (sum(1 for r in results if r.get("merged")), old.get("info", {}).get("date", "?"), old.get("info", {}).get("commit", "?"))
            info["what"] = "the clean tree plus one patch at a time; the rows not run today are those of an earlier run (%s)" % info["merged"]
        table, summary = render(results, info)
        with open(os.path.join(self.out, "kill_table.md"), "w", encoding="utf-8") as f:
            f.write(table)
        with open(os.path.join(self.out, "kill_table.json"), "w", encoding="utf-8") as f:
            json.dump({"info": info, "results": results}, f, indent=1)
        self.log("table: " + os.path.join(self.out, "kill_table.md"))
        print(summary)
        return 0


# ---- the self-test ----------------------------------------------------------------------------------------------------------------------------

GOOD = """# mutant:   M99
# class:    1
# twin:     OS-BUG-X (a stand-in)
# expected: A1
# effect:   removes a thing
diff --git a/app/src/x.cpp b/app/src/x.cpp
--- a/app/src/x.cpp
+++ b/app/src/x.cpp
@@ -1 +1 @@
-a
+b
"""

CTEST_OUT2 = """4/5 Test  #4: qanet_keys_1of6 ..............***Exception: SegFault  0.40 sec
5/5 Test  #5: qanet_budgets ....................***Not Run   0.00 sec
"""

CTEST_OUT = """    Start 1: qanet_rules_1of4
1/3 Test  #1: qanet_rules_1of4 ..................   Passed    1.00 sec
2/3 Test  #2: qanet_audio_matrix.slice.beat_add ...***Failed    5.00 sec
3/3 Test  #3: qanet_random_user_short ...........***Timeout  600.00 sec
"""


def self_test(root):
    errs = []

    def expect(cond, what):
        if not cond:
            errs.append(what)
    # the parser: a good header, and every way a header is wrong
    head, p = parse_header(GOOD, "M99_x.patch")
    expect(not p and head.get("expected") == "A1", "a good patch is rejected: %s" % p)
    for what, text, name in (
            ("no header line", GOOD.replace("# twin:     OS-BUG-X (a stand-in)\n", ""), "M99_x.patch"),
            ("a wrong ID", GOOD, "M98_x.patch"),
            ("a wrong class", GOOD.replace("# class:    1", "# class:    7"), "M99_x.patch"),
            ("an expected part that is nothing", GOOD.replace("expected: A1", "expected: soon"), "M99_x.patch"),
            ("a patch of the net", GOOD.replace("app/src/x.cpp", "tests/x.cpp"), "M99_x.patch"),
            ("no diff", "# mutant: M99\n", "M99_x.patch")):
        _, p = parse_header(text, name)
        expect(p, "a patch with %s is accepted" % what)
    expect(expected_part("A1") == ["A1"] and expected_part("B1 (and C2)") == ["B", "C"] and expected_part("D1") == ["D1"] and expected_part("none (B5)") == [], "expected_part")
    # the tests that failed, and the part of each
    names = failed_tests(CTEST_OUT)
    expect(names == ["qanet_audio_matrix.slice.beat_add", "qanet_random_user_short"], "failed_tests: %s" % names)
    kinds = ctest_kinds(CTEST_OUT + CTEST_OUT2)
    expect(kinds == {"qanet_audio_matrix.slice.beat_add": "Failed", "qanet_random_user_short": "Timeout", "qanet_keys_1of6": "SegFault", "qanet_budgets": "Not Run"}, "ctest_kinds: %s" % kinds)
    kn, odd = kill_kinds(kinds)
    expect("qanet_budgets" not in kn and odd == {"qanet_random_user_short": "Timeout", "qanet_keys_1of6": "SegFault"}, "kill_kinds: %s %s" % (kn, odd))
    expect([part_of(n) for n in ("qanet_rules_2of4", "qanet_keys_selftest", "qanet_select_1of3", "qanet_random_user_fatfs_short", "qanet_random_user_short", "qanet_audio_matrix.edit.undo_add", "qanet_ledger")] ==
           ["A1", "A2", "A3", "D1", "B", "C", "A4"], "part_of")
    # the table: two mutants, one killed by two parts, one that survived
    rs = [{"id": "M99", "head": head, "failed": names, "verdict": "killed"},
          {"id": "M98", "head": dict(head, mutant="M98", expected="none (B5)", **{"class": "2"}), "failed": [], "verdict": "survived"}]
    table, summary = render(rs, {"date": "2026-01-01", "commit": "abcd1234"})
    expect("2 run, 1 killed, 1 survived" in summary and "kill rate 50%" in summary, "summary: " + summary)
    expect("| M99 | 1 |" in table and "| M98 | 2 |" in table and "## Per class" in table and "| C | qanet_audio_matrix.* | 1 |" in table, "the table is missing a row or a section")
    rs.append({"id": "M97", "head": dict(head, mutant="M97", expected="B1"), "failed": [], "failed_ext": ["qanet_random_user_1of8"], "verdict": "survived"})
    table, summary = render(rs, {"date": "2026-01-01", "commit": "abcd1234", "extend": "qanet_random_user_1of8"})
    expect("3 run, 1 killed, 2 survived" in summary and "1 more killed by the extended run (66%)" in summary, "summary with the extended run: " + summary)
    expect("killed by the extended run: ru_1of8" in table, "the table does not show the extended run")
    # a kill that is only a Timeout or a crash is flagged; a revert of a fix is shown but not counted
    rk = [{"id": "M99", "head": head, "failed": ["qanet_random_user_short"], "kinds": {"qanet_random_user_short": "Timeout"}, "verdict": "killed"},
          {"id": "M98", "head": dict(head, mutant="M98", revert="OS-X (a fix)"), "failed": ["qanet_rules_1of4"], "verdict": "killed"},
          {"id": "M97", "head": dict(head, mutant="M97", revert="OS-X (a fix)"), "failed": [], "verdict": "survived"},
          {"id": "M96", "head": dict(head, mutant="M96"), "failed": ["qanet_keys_1of6", "qanet_rules_1of4"], "kinds": {"qanet_keys_1of6": "SegFault"}, "verdict": "killed"}]
    table, summary = render(rk, {"date": "2026-01-01", "commit": "abcd1234"})
    expect("2 run, 2 killed, 0 survived" in summary and "kill rate 100%" in summary and "1 only by a Timeout" in summary and "2 revert(s)" in summary, "summary with flags and reverts: " + summary)
    expect("ru_short (Timeout)" in table and "(FLAG: only a Timeout)" in table and "revert of OS-X: not counted" in table and "keys_1of6 (SegFault)" in table, "the table does not flag a Timeout, a crash or a revert")
    expect(table.count("FLAG") == 1, "a kill with a plain failure beside a crash is flagged as weak")
    # the extension is void when its control fails; a partial run is merged into an earlier table
    table, summary = render(rs, {"date": "2026-01-01", "commit": "abcd1234", "extend": "qanet_random_user_1of8", "extend_void": ["qanet_random_user_1of8"]})
    expect("VOID" in table and "killed by the extended run" not in table and "extended run is void" in summary and "3 run, 1 killed, 2 survived" in summary, "a void extension is still credited: " + summary)
    ms = [{"id": i, "file": i + "_x.patch", "head": dict(head, mutant=i, **{"class": "3"})} for i in ("M97", "M98", "M99")]
    merged = merge_results([dict(r, failed_ext=["x"]) for r in rs], [{"id": "M98", "head": head, "failed": ["qanet_rules_1of4"], "verdict": "killed"}], ms, False)
    expect([r["id"] for r in merged] == ["M97", "M98", "M99"] and merged[1]["verdict"] == "killed" and merged[0]["head"]["class"] == "3" and merged[0].get("merged") and "failed_ext" not in merged[0] and not merged[1].get("merged"), "merge_results: %s" % merged)
    # every patch of the repository
    mutants, problems = load_mutants(root)
    errs += problems
    if not mutants:
        errs.append("tests/qanet/mutants/ holds no patch")
    classes = {m["head"].get("class") for m in mutants}
    counted = [m for m in mutants if not m["head"].get("revert")]  # (the reverts of fixes are run and shown, not counted)
    if mutants and len(counted) < 15:
        errs.append("the set holds %d counted mutants: the spec asks for 15 to 20" % len(counted))
    if len(counted) > 20:
        errs.append("the set holds %d counted mutants: the spec asks for 15 to 20" % len(counted))
    for c in CLASSES:
        if mutants and c not in classes:
            errs.append("no mutant of class %s" % c)
    for e in errs:
        print("qanet_mutants selftest: " + e)
    if not errs:
        print("qanet_mutants selftest: ok (%d patches, classes %s)" % (len(mutants), ",".join(sorted(classes))))
    return 1 if errs else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--run", action="store_true")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    ap.add_argument("--out", help="the directory of the throwaway worktree, its build and the table (default <root>/build-mutants)")
    ap.add_argument("--only", help="comma-separated mutant IDs")
    ap.add_argument("--jobs", type=int, default=4, help="build jobs (ctest runs at -j2 whatever this is)")
    ap.add_argument("--prefix", default=DEFAULT_PREFIX, help="the command that wraps builds and tests (default: %s; empty for none)" % DEFAULT_PREFIX)
    ap.add_argument("--ctest-exclude", default=CTEST_EXCLUDE, help="ctest -E regex (default %r: the self-tests of the net do not look at the product)" % CTEST_EXCLUDE)
    ap.add_argument("--extend", help="ctest -R regex of the long run (label e2e) that a mutant gets when it survived the host set and a part named by --extend-parts expects to kill it (the host test of such a part is a sample of the long one)")
    ap.add_argument("--merge", help="a kill_table.json of an earlier run: the mutants this run does not run (--only) take their rows from it")
    ap.add_argument("--extend-parts", default="B,D1", help="the parts whose host test is a sample (default B,D1)")
    ap.add_argument("--ctest-only", help="ctest -R regex: run only these tests (a quick look at one mutant; the table is then partial)")
    ap.add_argument("--build-only", action="store_true", help="apply, build and revert each patch, run no test (does each patch compile?); no table is written")
    ap.add_argument("--targets", help="comma-separated build targets (default: the qanet binaries, o3sim and the card images)")
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    if a.self_test:
        return self_test(root)
    if a.list:
        mutants, problems = load_mutants(root)
        for m in mutants:
            h = m["head"]
            print("%s  class %s  expected %s  %s" % (m["id"], h.get("class", "?"), h.get("expected", "?"), h.get("effect", "")))
        for p in problems:
            print("problem: " + p)
        return 1 if problems else 0
    if a.run:
        if not shutil.which("git") or not shutil.which("cmake") or not shutil.which("ctest"):
            print("qanet_mutants: git, cmake and ctest are needed", file=sys.stderr)
            return 2
        r = Run(a)
        return r.go()
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
