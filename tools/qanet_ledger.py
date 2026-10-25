#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The OS-QANET ledger gate (docs/specs/OS-QANET.md section 9, slice A4): every bug a person found has a row in docs/qa/qanet-ledger.md.

    python3 -I tools/qanet_ledger.py --check --ctest-dir <build dir>   the ctest `qanet_ledger`: exit 1 with one line per finding
    python3 -I tools/qanet_ledger.py --stats                           the ledger rate and the states (also printed by --check)
    python3 -I tools/qanet_ledger.py --self-test                       the ctest `qanet_ledger_selftest`: plants each fault and expects the finding

The table sits between <!-- ledger --> and <!-- /ledger --> and has five cells: `| ID | Class | Regression test | Catcher | State |`.

  ID          the backlog ID of the bug, unique
  Class       starts with 1 to 4, the classes of OS-QANET section 2 (`1`, `3 (and 2)`)
  Regression  backticked references: a ctest name (a glob such as `qanet_rules_*of4` is fine and must match at least one ctest of the build) and a
              baseline file, `tests/qanet/baseline_x.txt` (it must have a line, not a comment, whose last column starts with the ID); for a bug that
              an audit flagged and no test holds yet, a document under docs/ that names the ID. Prose outside the backticks is not read.
  Catcher     the rule, generator, matrix cell, invariant or audit line that caught it (or should have). `none, because <reason>` when none did.
              Anything else that begins with `none` is not an entry.
  State       one word and a note:
                found     a test of the net caught it: the Regression cell names a ctest and a baseline line that exist
                flagged   an audit of part E flagged it and no test holds it yet: the Regression cell names the audit document that holds the ID
                tried     the net tried and did not reproduce it: the note names the slice (B1, D1 ...)
                pending   no part has tried yet: the note names the slice that will (D3 ...)
                fixed     the fix landed (its own branch): the Regression cell names the ctest that failed before the fix; the baseline line is gone

Stdlib only. `--ctest-dir` runs `ctest -N` there to list the tests (nothing runs).
"""

import argparse
import fnmatch
import os
import re
import subprocess
import sys
import tempfile

LEDGER = "docs/qa/qanet-ledger.md"
OPEN, CLOSE = "<!-- ledger -->", "<!-- /ledger -->"
STATES = ("found", "flagged", "tried", "pending", "fixed")
SLICE = re.compile(r"\b[A-F][0-9]+\b")


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def parse(text):
    """-> (rows, findings). A row is a dict with the five cells and its line number."""
    rows, findings = [], []
    lines = text.split("\n")
    try:
        a = next(i for i, l in enumerate(lines) if l.strip() == OPEN)
        b = next(i for i, l in enumerate(lines) if l.strip() == CLOSE)
    except StopIteration:
        return rows, ["%s: the table is not between %s and %s" % (LEDGER, OPEN, CLOSE)]
    seen = set()
    for n in range(a + 1, b):
        line = lines[n].strip()
        if not line.startswith("|") or re.match(r"^\|[\s:|-]+\|?$", line):
            continue
        cells = [c.strip() for c in line.strip("|").split("|")]
        if cells[0] == "ID":
            continue
        if len(cells) != 5:
            findings.append("%s:%d: %d cells, a row has 5 (| ID | Class | Regression test | Catcher | State |); a `|` inside a cell breaks the row" % (LEDGER, n + 1, len(cells)))
            continue
        row = dict(zip(("id", "cls", "reg", "catcher", "state"), cells), line=n + 1)
        if row["id"] in seen:
            findings.append("%s:%d: %s is in the ledger twice" % (LEDGER, n + 1, row["id"]))
        seen.add(row["id"])
        rows.append(row)
    if not rows:
        findings.append("%s: the ledger has no row" % LEDGER)
    return rows, findings


def ticks(cell):
    return re.findall(r"`([^`]+)`", cell)


def baseline_has(path, bug):
    """A non-comment line of the baseline whose last column is the ID (a note may follow it)."""
    for line in read(path).split("\n"):
        if line.startswith("#") or not line.strip():
            continue
        last = line.rsplit(" | ", 1)[-1].strip()
        if last == bug or last.startswith(bug + " "):
            return True
    return False


def check_row(root, row, names):
    out = []
    where = "%s:%d: %s" % (LEDGER, row["line"], row["id"])
    if not re.match(r"^[1-4]\b", row["cls"]):
        out.append("%s: the class starts with 1 to 4 (OS-QANET section 2), not %r" % (where, row["cls"]))
    cat = row["catcher"]
    if not cat or cat in ("-", "?"):
        out.append("%s: no catcher entry: name the rule, generator, matrix cell, invariant or audit line that caught it, or write `none, because <reason>`" % where)
    elif cat.lower().startswith("none") and not re.match(r"^none, because \S+(\s+\S+){1,}", cat, re.I):
        out.append("%s: `none` needs its reason: `none, because <reason>`" % where)
    word = row["state"].split()[0].strip("*_`") if row["state"] else ""
    note = row["state"][len(row["state"].split()[0]):] if row["state"] else ""
    if word not in STATES:
        out.append("%s: the state is one of %s, not %r" % (where, ", ".join(STATES), row["state"]))
        return out
    refs = ticks(row["reg"])
    docs = [r for r in refs if r.startswith("docs/")]
    bases = [r for r in refs if r.endswith(".txt") and "/" in r and not r.startswith("docs/")]
    cts = [r for r in refs if r not in docs and r not in bases]
    if word == "found":
        if not cts:
            out.append("%s: state found and no ctest named in the regression cell" % where)
        for c in cts:
            if names is not None and not any(fnmatch.fnmatchcase(n, c) for n in names):
                out.append("%s: the ctest `%s` does not exist (no test of the build matches it)" % (where, c))
        if not bases:
            out.append("%s: state found and no baseline file named in the regression cell" % where)
        for b in bases:
            p = os.path.join(root, b)
            if not os.path.isfile(p):
                out.append("%s: the baseline `%s` does not exist" % (where, b))
            elif not baseline_has(p, row["id"]):
                out.append("%s: `%s` has no line whose last column is %s (the fix leaves the baseline: the row's state moves on with it)" % (where, b, row["id"]))
    elif word == "fixed":
        if not cts:
            out.append("%s: state fixed and no ctest named in the regression cell (the test that failed before the fix)" % where)
        for c in cts:
            if names is not None and not any(fnmatch.fnmatchcase(n, c) for n in names):
                out.append("%s: the ctest `%s` does not exist (no test of the build matches it)" % (where, c))
    elif word == "flagged":
        if not docs:
            out.append("%s: state flagged and no audit document named in the regression cell" % where)
        for d in docs:
            p = os.path.join(root, d)
            if not os.path.isfile(p):
                out.append("%s: the document `%s` does not exist" % (where, d))
            elif row["id"] not in read(p):
                out.append("%s: `%s` does not name %s" % (where, d, row["id"]))
    elif not SLICE.search(note):
        out.append("%s: state %s names no slice (B1, D3 ...): %s" % (where, word, "the slice that tried" if word == "tried" else "the slice that will try"))
    return out


def check(root, names):
    path = os.path.join(root, LEDGER)
    if not os.path.isfile(path):
        return ["%s does not exist" % LEDGER], []
    rows, out = parse(read(path))
    for row in rows:
        out += check_row(root, row, names)
    return out, rows


def stats(rows):
    caught = [r for r in rows if not r["catcher"].lower().startswith("none")]
    by = {s: sum(1 for r in rows if r["state"].split()[:1] == [s]) for s in STATES}
    pct = 100 * len(caught) // len(rows) if rows else 0
    return "ledger: %d rows, %d with a catcher (ledger rate %d%%); found %d, flagged %d, tried %d, pending %d, fixed %d" % (
        len(rows), len(caught), pct, by["found"], by["flagged"], by["tried"], by["pending"], by["fixed"])


def ctest_names(build, ctest="ctest"):
    try:
        p = subprocess.run([ctest, "--test-dir", build, "-N"], capture_output=True, text=True)
    except OSError as e:
        return None, "cannot run %s (%s): pass the CMake one with --ctest" % (ctest, e.strerror or e)
    if p.returncode != 0:
        return None, "ctest -N in %s failed: %s" % (build, p.stderr.strip() or p.stdout.strip()[-200:])
    names = re.findall(r"Test +#\d+: (\S+)", p.stdout)
    return (names, None) if names else (None, "ctest -N in %s lists no test" % build)


# ---- the self-test ------------------------------------------------------------------------------------------------------------------------------

GOOD = "| OS-BUG-X | 1 | `qx_*of2`, `tests/qanet/baseline_x.txt` | rule 1 (A1) | found |"
FLAG = "| OS-Y | 3 | `docs/qa/audit.md` | audit line E-1 | flagged |"
TRIED = "| OS-Z | 2 (and 4) | - | none, because it does not reproduce on a host folder | tried (B1, D1) |"
PEND = "| OS-W | 4 | - | none, because no latency budget exists yet | pending (D3) |"


def fixture(root, rows, baseline="R1 | a | b | why | OS-BUG-X\n", audit="E-1 OS-Y\n"):
    os.makedirs(os.path.join(root, "docs/qa"))
    os.makedirs(os.path.join(root, "tests/qanet"))
    with open(os.path.join(root, LEDGER), "w") as f:
        f.write("# ledger\n\n%s\n| ID | Class | Regression test | Catcher | State |\n|---|---|---|---|---|\n%s\n%s\n" % (OPEN, "\n".join(rows), CLOSE))
    with open(os.path.join(root, "tests/qanet/baseline_x.txt"), "w") as f:
        f.write("# comment OS-BUG-X\n" + baseline)
    with open(os.path.join(root, "docs/qa/audit.md"), "w") as f:
        f.write(audit)


def self_test():
    ok = True
    names = ["qx_1of2", "qx_2of2", "other"]

    def expect(name, rows, want, **kw):
        nonlocal ok
        with tempfile.TemporaryDirectory() as d:
            fixture(d, rows, **kw)
            got, _ = check(d, names)
        hit = [g for g in got if want in g] if want else got
        good = bool(hit) if want else not got
        print("%-40s %s%s" % (name, "ok" if good else "FAIL", "" if good else "  (wanted %r, got %r)" % (want, got)))
        ok = ok and good

    expect("the four kinds of row pass", [GOOD, FLAG, TRIED, PEND], None)
    expect("a row with no catcher", [GOOD.replace("rule 1 (A1)", "")], "no catcher entry")
    expect("a bare none is not an entry", [GOOD.replace("rule 1 (A1)", "none")], "needs its reason")
    expect("none with no reason", [GOOD.replace("rule 1 (A1)", "none, because")], "needs its reason")
    expect("a ctest that does not exist", [GOOD.replace("qx_*of2", "qy_*of2")], "does not exist")
    expect("found with no ctest", [GOOD.replace("`qx_*of2`, ", "")], "no ctest named")
    expect("found with no baseline", [GOOD.replace(", `tests/qanet/baseline_x.txt`", "")], "no baseline file named")
    expect("a baseline that is missing", [GOOD.replace("baseline_x", "baseline_gone")], "baseline_gone.txt` does not exist")
    expect("a baseline with no line of the ID", [GOOD], "has no line whose last column is OS-BUG-X", baseline="R1 | a | b | why | new\n")
    expect("the ID only in a comment is no line", [GOOD], "has no line whose last column", baseline="# R1 | a | b | why | OS-BUG-X\n")
    expect("an ID that only starts the other", [GOOD], "has no line whose last column", baseline="R1 | a | b | why | OS-BUG-XL\n")
    expect("a note after the ID is fine", [GOOD], None, baseline="R1 | a | b | why | OS-BUG-X (the AUTO apply)\n")
    expect("a fixed row needs its ctest", [GOOD.replace("found", "fixed").replace(", `tests/qanet/baseline_x.txt`", "")], None, baseline="R1 | a | b | why | new\n")
    expect("a fixed row with no ctest", [GOOD.replace("found", "fixed").replace("`qx_*of2`, ", "").replace(", `tests/qanet/baseline_x.txt`", "")], "state fixed and no ctest")
    expect("pending with no slice", [PEND.replace(" (D3)", "")], "names no slice")
    expect("tried with no slice", [TRIED.replace(" (B1, D1)", "")], "names no slice")
    expect("flagged and the audit lost the ID", [FLAG], "does not name OS-Y", audit="nothing\n")
    expect("flagged with no document", [FLAG.replace("`docs/qa/audit.md`", "-")], "no audit document")
    expect("an unknown state", [GOOD.replace("found", "mended")], "the state is one of")
    expect("a class outside 1 to 4", [GOOD.replace("| 1 |", "| 5 |")], "the class starts with 1 to 4")
    expect("an ID twice", [GOOD, GOOD], "in the ledger twice")
    expect("a row with six cells", [GOOD.replace("| found |", "| found | extra |")], "6 cells")
    # the table markers and an empty ledger
    with tempfile.TemporaryDirectory() as d:
        fixture(d, [GOOD])
        with open(os.path.join(d, LEDGER), "w") as f:
            f.write("| ID | Class | Regression test | Catcher | State |\n" + GOOD + "\n")
        got, _ = check(d, names)
        good = any("is not between" in g for g in got)
        print("%-40s %s" % ("a table with no markers", "ok" if good else "FAIL  %r" % got))
        ok = ok and good
    with tempfile.TemporaryDirectory() as d:
        got, _ = check(d, names)
        good = any("does not exist" in g for g in got)
        print("%-40s %s" % ("a missing ledger", "ok" if good else "FAIL  %r" % got))
        ok = ok and good
    # the rate
    rows = parse("%s\n%s\n%s\n%s\n%s\n%s\n" % (OPEN, GOOD, FLAG, TRIED, PEND, CLOSE))[0]
    s = stats(rows)
    good = s == "ledger: 4 rows, 2 with a catcher (ledger rate 50%); found 1, flagged 1, tried 1, pending 1, fixed 0"
    print("%-40s %s" % ("the ledger rate", "ok" if good else "FAIL  %r" % s))
    ok = ok and good
    print("qanet_ledger self-test: %s" % ("all good" if ok else "FAILED"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--stats", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    ap.add_argument("--ctest-dir", help="the build directory whose `ctest -N` names the tests that exist")
    ap.add_argument("--ctest", default="ctest", help="the ctest executable (the ctest target passes ${CMAKE_CTEST_COMMAND})")
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    root = os.path.abspath(a.root)
    names = None
    if a.check:
        if not a.ctest_dir:
            print("qanet_ledger: --check needs --ctest-dir", file=sys.stderr)
            return 2
        names, err = ctest_names(a.ctest_dir, a.ctest)
        if err:
            print("qanet_ledger: " + err, file=sys.stderr)
            return 2
    findings, rows = check(root, names)
    for f in findings:
        print(f)
    print(stats(rows) if rows else "ledger: no rows")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
