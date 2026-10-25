#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The report of `tools/ci.sh e2e` (OS-E2E, docs/specs/OS-E2E.md section 6): ctest's JUnit file as build-e2e/<date>/report.md.

    python3 -I tools/e2e_report.py --junit <ctest --output-junit file> --out report.md [--exit-code N] [--commit SHA]
                                   [--sheet screens.png --sheet-ok yes|no]
    python3 -I tools/e2e_report.py --self-test        the ctest `e2e_report_selftest`

One line of verdict, the pass/fail table of the tests with their times, and for each failing test the lines of its output that matter (a
legibility FINDING, a failed check) or else the tail of it. Stdlib only. A missing or unreadable JUnit file still writes a report that says so,
because the run that failed to produce one is the run someone must look at.
"""

import argparse
import datetime
import sys
import tempfile
import os
import xml.etree.ElementTree as ET

KEEP = ("FINDING", "allowed", "legibility:", "FAILED", "error", "Error", "mismatch", "expected", "CHECK", "REQUIRE")
TAIL = 20


def cases(path):
    """-> [(name, 'pass'|'fail'|'skip', seconds, output)] in the file's order."""
    out = []
    for tc in ET.parse(path).getroot().iter("testcase"):
        state = "fail" if (tc.find("failure") is not None or tc.find("error") is not None or tc.get("status") == "fail") else (
            "skip" if tc.find("skipped") is not None or tc.get("status") == "notrun" else "pass")
        text = "".join((e.text or "") for e in tc.iter() if e.tag in ("system-out", "system-err", "failure", "error"))
        out.append((tc.get("name", "?"), state, float(tc.get("time") or 0), text))
    return out


def interesting(text):
    """The lines of a failing test's output worth reading: the marked ones, up to 30, else the last TAIL lines."""
    lines = [ln.rstrip() for ln in text.splitlines() if ln.strip()]
    marked = [ln for ln in lines if any(k in ln for k in KEEP)]
    return (marked[:30] + (["... %d more" % (len(marked) - 30)] if len(marked) > 30 else [])) if marked else lines[-TAIL:]


def render(junit, exit_code, commit, sheet, sheet_ok, today):
    try:
        rows = cases(junit)
        problem = None
    except (OSError, ET.ParseError) as e:
        rows, problem = [], "ctest's JUnit file %s could not be read (%s)" % (junit, e)
    failed = [r for r in rows if r[1] == "fail"]
    verdict = "PASS" if (exit_code == 0 and rows and not failed and not problem) else "FAIL"
    lines = ["# OS-E2E report, %s" % today, "",
             "- Verdict: **%s** (ctest exit code %d)" % (verdict, exit_code),
             "- Commit: %s" % (commit or "unknown"),
             "- Tests: %d, passed %d, failed %d, skipped %d; the sum of their times %.0f s" %
             (len(rows), sum(r[1] == "pass" for r in rows), len(failed), sum(r[1] == "skip" for r in rows), sum(r[2] for r in rows))]
    if sheet:
        lines.append("- Contact sheet: %s" % (sheet if sheet_ok else "NOT WRITTEN (%s)" % sheet))
    if problem:
        lines += ["", "**%s.**" % problem]
    lines += ["", "| Test | Result | Time (s) |", "|---|---|---|"]
    lines += ["| %s | %s | %.1f |" % (n, s, t) for n, s, t, _ in rows]
    for name, state, _, text in rows:
        if state == "fail":
            lines += ["", "## %s" % name, "", "```"] + interesting(text) + ["```"]
    return "\n".join(lines) + "\n"


JUNIT = """<?xml version="1.0" encoding="UTF-8"?>
<testsuite name="Linux" tests="3" failures="1" disabled="0" skipped="0" hostname="h" time="12.5">
  <testcase name="legibility_1of6" classname="legibility_1of6" time="4.5" status="run"><system-out>legibility: 7 pictures, 0 findings, 0 allowed</system-out></testcase>
  <testcase name="legibility_2of6" classname="legibility_2of6" time="8" status="run">
    <failure message="Failed"/>
    <system-out>FINDING gallery/x_amiga: clipped: 10x20 'A' at (795, 12): 5 of 40 ink pixels cut by the screen edge
noise
FINDING gallery/y_amiga: overlap: a and b share 3 pixels
legibility: 7 pictures, 2 findings, 0 allowed</system-out>
  </testcase>
  <testcase name="legibility_3of6" classname="legibility_3of6" time="0" status="notrun"><skipped/></testcase>
</testsuite>
"""


def self_test():
    ok = True

    def check(name, good):
        nonlocal ok
        print("%-34s %s" % (name, "ok" if good else "FAIL"))
        ok = ok and good

    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "j.xml")
        with open(path, "w", encoding="utf-8") as f:
            f.write(JUNIT)
        rows = cases(path)
        check("the cases of a JUnit file", [(n, s) for n, s, _, _ in rows] == [("legibility_1of6", "pass"), ("legibility_2of6", "fail"), ("legibility_3of6", "skip")])
        text = render(path, 1, "abc1234", "2026-10-18/screens.png", True, "2026-10-18")
        check("a failing run says FAIL", "Verdict: **FAIL** (ctest exit code 1)" in text and "failed 1, skipped 1" in text)
        check("the table", "| legibility_1of6 | pass | 4.5 |" in text and "| legibility_2of6 | fail | 8.0 |" in text)
        check("the findings of the failure", "FINDING gallery/x_amiga: clipped" in text and "noise" not in text and "## legibility_2of6" in text)
        check("a passing test has no section", "## legibility_1of6" not in text)
        clean = JUNIT.replace('<failure message="Failed"/>', "")
        with open(path, "w", encoding="utf-8") as f:
            f.write(clean)
        check("a clean run says PASS", "Verdict: **PASS** (ctest exit code 0)" in render(path, 0, "abc", "", True, "2026-10-18"))
        check("a nonzero exit is FAIL", "Verdict: **FAIL**" in render(path, 8, "abc", "", True, "2026-10-18"))
        check("no sheet is said", "NOT WRITTEN" in render(path, 0, "abc", "s.png", False, "2026-10-18"))
        text = render(os.path.join(d, "missing.xml"), 1, "", "", True, "2026-10-18")
        check("a missing file is said", "could not be read" in text and "Verdict: **FAIL**" in text)
        with open(path, "w", encoding="utf-8") as f:
            f.write("<testsuite><testcase name='t' time='1'><failure/><system-out>a\nb\nc</system-out></testcase></testsuite>")
        check("a failure without marks: the tail", "```\na\nb\nc\n```" in render(path, 1, "", "", True, "2026-10-18"))
    print("e2e_report self-test: %s" % ("ok" if ok else "FAILED"))
    return 0 if ok else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--junit")
    ap.add_argument("--out")
    ap.add_argument("--exit-code", type=int, default=0)
    ap.add_argument("--commit", default="")
    ap.add_argument("--sheet", default="")
    ap.add_argument("--sheet-ok", default="yes")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.junit or not args.out:
        ap.error("--junit and --out are required")
    text = render(args.junit, args.exit_code, args.commit, args.sheet, args.sheet_ok == "yes", datetime.date.today().isoformat())
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
