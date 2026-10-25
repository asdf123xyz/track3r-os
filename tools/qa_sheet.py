#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Picture sheets for the QA catalogue (OS-FINALQA FQA-4): one Markdown page per screen, each picture with the rows that cite it.

    python3 -I tools/qa_sheet.py <png-dir> --out <sheet-dir>   the pages: one per catalogue file, index.md, other.md
    python3 -I tools/qa_sheet.py --self-test                    the ctest `qa_sheet_python_selftest`

The pictures are those of `o3sim --gallery <dir>` (`<screen>_<state>_<theme>.png`, cited as `gallery/<name>`) or of
`o3sim --scenario ... --out <dir>` (`<snap>.png`, cited as `qa/qa_x.txt` › `qa_xxx`). A row cites a picture when its Sim cell
gives the snap name after `›` (a comma-separated list after one `›` counts, too). `contact_sheet*.png` are not pictures.

Each page (a screen is one catalogue file) has:
  - every picture it owns: its link, then the rows that cite it. A picture is owned by the screen of the first row that cites it,
    else by the catalogue file whose name is the longest prefix of it (`pattern_typical_amiga` is the Pattern page), else other.md;
  - the pictures that no row cites;
  - the rows whose snap has no picture in <png-dir>.
The pages link the pictures by their path from the sheet directory, so keep the PNGs where they are and do not commit either.

Stdlib only; reads the catalogue and the names of the PNGs (never their pixels). Exit 2 on a usage error, else 0.
"""

import argparse
import collections
import os
import re
import sys
import tempfile
import urllib.parse

CAT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs", "qa", "catalogue"))
ROW_RE = re.compile(r"^\|\s*(QA-[A-Z0-9]+-\d+)\s*\|")
CELL_RE = re.compile(r"(?<!\\)\|")
TOKEN_RE = re.compile(r"`([^`]*)`|;|›")
PAREN_RE = re.compile(r"\([^()]*\)")
HOSTISH_RE = re.compile(r"_test$|\.(cpp|py|json)$|^host", re.I)  # a host test or a remark after a `›` list, not a snap
COLS = 8  # ID, Function, How, Expected, Sim, Unit, Spec, Status
SHEET = "contact_sheet"
OTHER = "other"

Row = collections.namedtuple("Row", "stem no id function status snaps")


def sim_snaps(cell):
    """-> the picture names a Sim cell cites: `gallery/<name>`, and the snap labels after a `›` (`qa/x.txt` › `a`, `b` cites a and b)."""
    out, listing, arrow = [], False, False
    for m in TOKEN_RE.finditer(PAREN_RE.sub("", cell)):
        s = m.group(0)
        if s == ";":
            listing = arrow = False
        elif s == "›":
            arrow = True
        elif not s.startswith("`"):
            continue
        else:
            tok = m.group(1).strip()
            if tok.startswith("gallery/"):
                out.append(tok[len("gallery/"):])
                listing = False
            elif "›" in tok:  # the arrow inside the backticks: `qa/x.txt › label`
                out.extend(x.strip() for x in tok.split("›")[1:] if x.strip())
                listing = True
            elif tok.endswith(".txt"):  # the scenario file: its labels follow a `›`, which is not in this token
                listing = False
            elif (arrow or listing) and not HOSTISH_RE.search(tok):
                out.append(tok)
                listing = True
            else:
                listing = False
            arrow = False
    return out


def load_rows(cat, warn):
    """-> the catalogue rows, in file order (README.md is the index and has none to read)."""
    rows = []
    for name in sorted(os.listdir(cat)):
        if not name.endswith(".md") or name == "README.md":
            continue
        with open(os.path.join(cat, name), encoding="utf-8") as f:
            for no, line in enumerate(f, 1):
                m = ROW_RE.match(line)
                if not m:
                    continue
                cells = [c.strip() for c in CELL_RE.split(line.strip().strip("|"))]
                if len(cells) != COLS:
                    warn("%s:%d: %d cells, not %d: row left out (qa_cover names it)" % (name, no, len(cells), COLS))
                    continue
                rows.append(Row(name[:-3], no, m.group(1), cells[1], cells[7], sim_snaps(cells[4])))
    return rows


def build(png_dir, cat, out, warn=lambda msg: print(msg, file=sys.stderr)):
    """Writes the pages into `out`; returns the summary numbers."""
    names = sorted(n for n in os.listdir(png_dir) if n.endswith(".png") and os.path.isfile(os.path.join(png_dir, n)))
    sheets = [n for n in names if n.startswith(SHEET)]
    pics = {n[:-4]: n for n in names if not n.startswith(SHEET)}
    rows = load_rows(cat, warn)
    stems = sorted(n[:-3] for n in os.listdir(cat) if n.endswith(".md") and n != "README.md")

    citing = collections.defaultdict(list)  # picture -> the rows that cite it
    owner = {}  # picture -> the page it is on
    for r in rows:
        for s in r.snaps:
            if s in pics:
                if r not in citing[s]:
                    citing[s].append(r)
                owner.setdefault(s, r.stem)
    for p in pics:
        if p not in owner:
            owner[p] = max((s for s in stems if p.startswith(s + "_")), key=len, default=OTHER)

    pages = stems + ([OTHER] if OTHER in owner.values() else [])
    os.makedirs(out, exist_ok=True)
    out_abs, png_abs = os.path.abspath(out), os.path.abspath(png_dir)
    stats = {}
    for stem in pages:
        here = sorted(p for p in pics if owner[p] == stem)
        cited = [p for p in here if citing[p]]
        uncited = [p for p in here if not citing[p]]
        rows_here = [r for r in rows if r.stem == stem and r.snaps]
        missing = [(r, [s for s in r.snaps if s not in pics]) for r in rows_here if any(s not in pics for s in r.snaps)]
        stats[stem] = dict(pics=len(here), cited=len(cited), uncited=len(uncited), rows=len(rows_here), missing=len(missing))

        def link(name):
            rel = os.path.relpath(os.path.join(png_abs, pics[name]), out_abs).replace(os.sep, "/")
            return urllib.parse.quote(rel, safe="/._-")

        title = stem.replace("_", " ").capitalize()
        lines = ["# %s picture sheet" % title, "",
                 "Generated by `tools/qa_sheet.py` from `docs/qa/catalogue/%s.md`. The pictures are not for git." % stem, "",
                 "Pictures %d, cited by a row %d, no row cites %d; rows citing a snap %d, rows whose snap has no picture %d."
                 % (len(here), len(cited), len(uncited), len(rows_here), len(missing)), ""]
        for p in cited:
            lines += ["## %s" % p, "", "![%s](%s)" % (p, link(p)), ""]
            lines += ["- %s %s (%s)" % (r.id, r.function, r.status) for r in citing[p]]
            lines.append("")
        lines += ["## Pictures no row cites", ""]
        lines += ["- [%s](%s)" % (p, link(p)) for p in uncited] or ["None."]
        lines += ["", "## Rows whose snap has no picture", ""]
        lines += ["- %s %s (no picture: %s)" % (r.id, r.function, ", ".join(m)) for r, m in missing] or ["None."]
        with open(os.path.join(out, stem + ".md"), "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")

    index = ["# QA picture sheets", "",
             "Generated by `tools/qa_sheet.py` from `docs/qa/catalogue/` and the pictures in one directory. Not for git.", "",
             "| Page | Pictures | Cited | No row cites | Rows citing a snap | Rows with no picture |",
             "|---|---|---|---|---|---|"]
    for stem in pages:
        s = stats[stem]
        index.append("| [%s](%s.md) | %d | %d | %d | %d | %d |" % (stem, stem, s["pics"], s["cited"], s["uncited"], s["rows"], s["missing"]))
    with open(os.path.join(out, "index.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(index) + "\n")

    cited_total = sum(1 for p in pics if citing[p])
    return dict(pictures=len(pics), sheets=len(sheets), rows=len(rows),
                rows_snap=sum(1 for r in rows if r.snaps),
                cited=cited_total, uncited=len(pics) - cited_total,
                missing=sum(s["missing"] for s in stats.values()), pages=len(pages), stats=stats)


def self_test():
    """Plants one of each case in a scratch catalogue and picture directory, and checks the pages and the counts."""
    fails = []

    def check(ok, what):
        if not ok:
            fails.append(what)

    with tempfile.TemporaryDirectory() as tmp:
        cat, png, out = (os.path.join(tmp, d) for d in ("cat", "png", "sheets"))
        os.makedirs(cat)
        os.makedirs(png)
        head = "| ID | Function | How | Expected | Sim | Unit | Spec | Status |\n|---|---|---|---|---|---|---|---|\n"
        with open(os.path.join(cat, "pattern.md"), "w", encoding="utf-8") as f:
            f.write("# Pattern\n\n" + head
                    + "| QA-PAT-001 | Enter STEP | h | e | `qa/qa_pattern.txt` › `qa_pat_001` | - | S | sim PASS |\n"
                    + "| QA-PAT-002 | Leave STEP | h | e | `qa/qa_pattern.txt` › `qa_pat_002`, `qa_pat_003` | - | S | sim PASS |\n"
                    + "| QA-PAT-003 | Gallery | h | e | `gallery/pattern_typical_amiga` (remark `x`) | - | S | sim PASS |\n"
                    + "| QA-PAT-004 | No picture | h | e | `qa/qa_pattern.txt` › `qa_pat_009` | - | S | sim PASS |\n"
                    + "| QA-PAT-005 | Host only | h | e | host: `x_test` | - | S | host PASS (`x_test`) |\n")
        with open(os.path.join(cat, "module_app.md"), "w", encoding="utf-8") as f:
            f.write("# Module app\n\n" + head
                    + "| QA-MOD-001 | Open | h | e | `qa/qa_mod.txt` › `qa_mod_01` | - | S | sim PASS |\n")
        for name in ("qa_pat_001", "qa_pat_002", "qa_pat_003", "pattern_typical_amiga", "pattern_worst_black",
                     "qa_orphan", "module_app_typical_amiga", "modules_typical_amiga", "contact_sheet", "contact_sheet_black"):
            open(os.path.join(png, name + ".png"), "wb").close()
        open(os.path.join(png, "notes.txt"), "w").close()

        def quiet(msg):
            fails.append("unexpected warning: " + msg)

        res = build(png, cat, out, warn=quiet)
        check(res["pictures"] == 8, "pictures %d, not 8" % res["pictures"])
        check(res["sheets"] == 2, "contact sheets %d, not 2" % res["sheets"])
        check(res["rows"] == 6 and res["rows_snap"] == 5, "rows %d / snap rows %d, not 6 / 5" % (res["rows"], res["rows_snap"]))
        check(res["cited"] == 4 and res["uncited"] == 4, "cited %d / uncited %d, not 4 / 4" % (res["cited"], res["uncited"]))
        check(res["missing"] == 2, "rows with no picture %d, not 2 (QA-PAT-004 and QA-MOD-001)" % res["missing"])
        check(res["pages"] == 3, "pages %d, not 3 (pattern, module_app, other)" % res["pages"])

        def read(name):
            with open(os.path.join(out, name), encoding="utf-8") as f:
                return f.read()

        pattern = read("pattern.md")
        check("![qa_pat_003](" in pattern and "- QA-PAT-002 Leave STEP (sim PASS)" in pattern, "pattern.md lacks a cited picture and its row")
        check("- QA-PAT-004 No picture (no picture: qa_pat_009)" in pattern, "pattern.md lacks the row with no picture")
        check("- [pattern_worst_black](" in pattern, "pattern.md lacks the uncited picture")
        check("module_app_typical_amiga" in read("module_app.md"), "module_app_typical_amiga is not on the module app page")
        other = read("other.md")
        check("qa_orphan" in other and "modules_typical_amiga" in other, "unowned pictures are not on other.md")
        check("| [pattern](pattern.md) | 5 | 4 | 1 | 4 | 1 |" in read("index.md"), "index.md row for pattern is wrong")

        # A list after a `›` runs on to a `;`; a host test is not a snap; a label without a `›` is not cited.
        check(sim_snaps("`qa/a.txt` › `s1`, `s2`; `gallery/g1`; host: `y_test`") == ["s1", "s2", "g1"], "sim_snaps lists")
        check(sim_snaps("`qa/a.txt` › `s1` (`s9` remark)") == ["s1"], "sim_snaps remark")
        check(sim_snaps("`qa/a.txt`, `s1`") == [], "sim_snaps bare label without arrow")
        check(sim_snaps("`qa/x.txt` › `snap_a`, host: `z_test`") == ["snap_a"], "sim_snaps host after list")

    if fails:
        for f in fails:
            print("self-test FAIL: " + f, file=sys.stderr)
        return 1
    print("self-test: ok")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="One Markdown picture sheet per QA catalogue screen (see the module docstring).")
    ap.add_argument("png_dir", nargs="?", help="directory of PNGs: o3sim --gallery <dir>, or --scenario ... --out <dir>")
    ap.add_argument("--out", help="directory for the pages (created)")
    ap.add_argument("--catalogue", default=CAT, help="catalogue directory (default: docs/qa/catalogue of this checkout)")
    ap.add_argument("--self-test", action="store_true", help="plant the cases in a scratch directory and check the pages")
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    if not a.png_dir or not a.out:
        ap.error("png_dir and --out are needed")
    if not os.path.isdir(a.png_dir):
        print("qa_sheet: no such directory: %s" % a.png_dir, file=sys.stderr)
        return 2
    if not os.path.isdir(a.catalogue):
        print("qa_sheet: no such catalogue: %s" % a.catalogue, file=sys.stderr)
        return 2
    res = build(a.png_dir, a.catalogue, a.out)
    print("qa_sheet: %d pictures (%d contact sheets left out), %d catalogue rows, %d cite a snap"
          % (res["pictures"], res["sheets"], res["rows"], res["rows_snap"]))
    print("qa_sheet: %d pictures cited by a row, %d cited by no row, %d rows whose snap has no picture"
          % (res["cited"], res["uncited"], res["missing"]))
    print("qa_sheet: %d pages and index.md in %s" % (res["pages"], os.path.abspath(a.out)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
