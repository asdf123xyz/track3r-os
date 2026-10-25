#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""code_map.py: a map of the tree that places an agent in seconds, written to build-map/ (untracked, regenerated).

Run from the repository root (or a worktree): `python3 -I tools/code_map.py`. It writes
  build-map/README.md    what each file holds and how to search it
  build-map/keys.md      every built-in binding: context, gesture, action, handler file:line, the handler's comment
  build-map/screens.md   every screen: its context, source files, QA catalogue, simulator scenario, HIL steps, manual chapter
  build-map/features.md  every work-item ID of the commit subjects (`[ID]`): its spec and the files its commits touched
Every line of keys.md is one binding, so `grep -i 'sampleload.*F1' build-map/keys.md` answers "what does F1 do on Sample
load, and where is it". Symbols (definitions, callers) are the LSP tool's job, or `rg -n`.
`--check` also fails (exit 1) when a binding names an action with no registry row or a registry row names a handler with
no definition; ctest `code_map` runs it. Standard library only.
"""
import argparse
import collections
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), os.pardir))
APP = os.path.join(ROOT, "app")


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


def line_of(text, pos):
    return text.count("\n", 0, pos) + 1


def calls(text, name):
    """Yield (pos, [top-level arguments]) for every call `name(...)` in text."""
    for m in re.finditer(r"\b%s\(" % re.escape(name), text):
        depth, args, cur, i = 1, [], [], m.end()
        while i < len(text) and depth:
            c = text[i]
            if c in "([{":
                depth += 1
            elif c in ")]}":
                depth -= 1
                if not depth:
                    break
            if c == "," and depth == 1:
                args.append("".join(cur).strip())
                cur = []
            elif c == '"':
                j = text.index('"', i + 1)
                cur.append(text[i:j + 1])
                i = j
            else:
                cur.append(c)
            i += 1
        args.append("".join(cur).strip())
        yield m.start(), [re.sub(r"\s+", " ", a) for a in args]


def git(*args):
    """The output of a git command in ROOT, or "" when git or the repository is missing (a source tarball)."""
    try:
        return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return ""


def tracked():
    out = git("ls-files").split()
    if out:
        return out
    skip = ("build", ".git", ".claude", ".toolchains")
    return sorted(os.path.relpath(os.path.join(d, f), ROOT) for d, dirs, fs in os.walk(ROOT)
                  if not os.path.relpath(d, ROOT).startswith(skip) for f in fs)


def strip_comments(text):
    # Keep the length (positions stay valid): comments become spaces.
    return re.sub(r"//[^\n]*|/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.S)


def gesture_text(g):
    g = re.sub(r"button_ref\(Button::(\w+)\)", r"\1", g)
    g = re.sub(r"pad_ref\((?:core::)?kAnyId\)", "pad", g)
    g = re.sub(r"pad_ref\((\w+)\)", r"pad \1", g)
    g = re.sub(r"encoder_ref\(0\)", "encoder", g)
    g = re.sub(r"encoder_ref\((\w+)\)", r"encoder \1", g)
    g = re.sub(r"\bk(Shift|Alt|Ctrl|Fn)\b", r"+\1", g)
    g = re.sub(r"^(\w+)\((.*)\)$", r"\1 \2", g)
    return g.replace(",", "")


def handlers():
    """handler name -> (file:line, comment) from app/src/*.cpp definitions and action_handlers.h comments."""
    comments = {}
    for m in re.finditer(r"^bool (\w+)\(ActionContext&[^;]*;\s*//\s*(.*)$", read("app/src/action_handlers.h"), re.M):
        comments[m.group(1)] = m.group(2).strip()
    defs = {}
    src = os.path.join(APP, "src")
    for dirpath, _, files in os.walk(src):
        for f in sorted(files):
            if not f.endswith(".cpp"):
                continue
            rel = os.path.relpath(os.path.join(dirpath, f), ROOT)
            text = read(rel)
            for m in re.finditer(r"^bool (\w+)\(ActionContext& \w*,", text, re.M):
                defs.setdefault(m.group(1), "%s:%d" % (rel, line_of(text, m.start())))
    return defs, comments


def actions():
    """action name -> handler name, from the registry in app/src/actions.cpp."""
    text = strip_comments(read("app/src/actions.cpp"))
    out = {}
    for _, a in calls(text, "make_action"):
        if len(a) >= 2 and a[0].startswith('"'):
            out[a[0].strip('"')] = a[1]
    return out


def bindings():
    raw = read("app/src/bindings.cpp")
    text = strip_comments(raw)
    ids = dict(re.findall(r"constexpr core::ActionId (\w+) = core::action_id\(\"([^\"]+)\"\)", text))
    ctxs = dict(re.findall(r"constexpr core::ContextId (\w+) = context_id\(Context::(\w+)\)", text))
    rows = []
    for pos, a in calls(text, "bind"):
        if len(a) < 3 or not a[0].startswith("k"):
            continue
        ctx = ctxs.get(a[0], a[0])
        action = ids.get(a[2], a[2])
        arg = ""
        if len(a) >= 5 and a[3] == "ArgFrom::Fixed":
            arg = a[4]
        elif len(a) >= 4:
            arg = a[3].replace("ArgFrom::", "")
        rows.append((ctx, gesture_text(a[1]), action, arg, "app/src/bindings.cpp:%d" % line_of(raw, pos)))
    return rows


def write_keys(out):
    defs, comments = handlers()
    reg = actions()
    problems = []
    lines = ["# Built-in bindings (generated by tools/code_map.py; do not edit)", "",
             "One line per binding: `context | gesture | action (arg) | handler file:line | what the handler does | binding line`.",
             "Search: `grep -i '<context>.*<key>' build-map/keys.md`. Add an action or a binding: app/CLAUDE.md.", ""]
    for ctx, gesture, action, arg, where in bindings():
        h = reg.get(action)
        if h is None and not re.match(r"^[a-z][\w]*\.[\w.]+$", action):
            loc, note = "-", "computed in code (see the binding line)"
        elif h is None:
            problems.append("%s: action %s has no registry row in app/src/actions.cpp" % (where, action))
            loc, note = "-", ""
        else:
            loc = defs.get(h)
            if loc is None:
                problems.append("%s: handler %s of %s has no definition in app/src" % (where, h, action))
                loc = "-"
            note = comments.get(h, "")
        lines.append("%s | %s | %s%s | %s %s | %s | %s" % (ctx, gesture, action, " (%s)" % arg if arg else "",
                                                       h or "-", loc, note, where))
    with open(os.path.join(out, "keys.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    return problems


def snake(name):
    return re.sub(r"(?<=[a-z0-9])(?=[A-Z])", "_", name).lower()


def write_screens(out):
    reg = read("app/src/screens/registry.cpp")
    lines = ["# Screens (generated by tools/code_map.py; do not edit)", ""]
    files = tracked()
    for m in re.finditer(r"\{ScreenId::(\w+), \"([^\"]+)\", &(\w+)", reg):
        sid, title, obj = m.groups()
        key = snake(sid)
        flat = key.replace("_", "")
        src = [f for f in files if f.startswith(("app/src/screens/", "app/include/o3os/app/screens/"))
               and os.path.basename(f).split(".")[0].replace("_", "").startswith(flat)]
        defn = [f for f in files if f.startswith("app/src/") and f.endswith(".cpp")
                and re.search(r"\b%s\b\s*[{;=]" % obj, read(f))]
        cat = [f for f in ("docs/qa/catalogue/%s.md" % key, "docs/qa/catalogue/%s.md" % flat) if f in files]
        rows = len(re.findall(r"^\| QA-", read(cat[0]), re.M)) if cat else 0
        specs = sorted(set(re.findall(r"docs/specs/[\w.-]+\.md", read(cat[0])))) if cat else []
        extra = [f for f in files if re.search(r"(^|/)qa_%s\.(txt|json)$" % key, f)
                 or f in ("docs/manual/%s.md" % key, "docs/manual/%s.md" % flat)]
        lines += ["## %s (%s)" % (title, sid), "",
                  "- context: Context::%s (bindings in build-map/keys.md)" % sid,
                  "- object: %s, defined in %s" % (obj, ", ".join(defn) or "-"),
                  "- sources: %s" % (", ".join(sorted(set(src))) or "-"),
                  "- QA catalogue: %s (%d rows)" % (", ".join(cat) or "-", rows),
                  "- simulator / HIL / manual: %s" % (", ".join(sorted(extra)) or "-"),
                  "- specs named by its catalogue rows: %s" % (", ".join(specs) or "-"), ""]
    with open(os.path.join(out, "screens.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


def write_features(out):
    log = git("log", "--no-merges", "--format=%x00%s", "--name-only")
    touched = collections.defaultdict(collections.Counter)
    files = tracked()
    mentioned = collections.defaultdict(set)
    for f in files:
        if f.startswith(("docs/", "third_party/")) or not f.endswith((".cpp", ".h", ".c", ".txt", ".json", ".py", ".cmake")):
            continue
        try:
            text = read(f)
        except (UnicodeDecodeError, OSError):
            continue
        for fid in set(re.findall(r"\b((?:OS|PAT|ODD|SONG|MOD|E2E|FQA|PRF|ADR)-[A-Z0-9]+(?:-[A-Z0-9]+)*)\b", text)):
            mentioned[fid].add(f)
    for chunk in log.split("\x00")[1:]:
        head, _, names = chunk.partition("\n")
        m = re.search(r"\[([A-Z][A-Za-z0-9._-]*(?:[ ,/+]+[A-Z][A-Za-z0-9._-]*)*)\]\s*$", head)
        if not m:
            continue
        for fid in re.split(r"[ ,/+]+", m.group(1)):
            for n in names.split():
                touched[fid][n] += 1
    lines = ["# Work items and the files their commits touched (generated by tools/code_map.py; do not edit)", "",
             "From `git log --no-merges` (the `[ID]` at the end of each commit subject; most-touched files first) and from the IDs named in",
             "code, tests, scenarios and scripts (`named in`; this repository's history starts at its export, so older IDs have only that).",
             "Search: `grep -A3 '^## OS-RATEPITCH' build-map/features.md`.", ""]
    for fid in sorted(set(touched) | set(mentioned)):
        c = touched.get(fid, collections.Counter())
        code = [f for f, _ in c.most_common() if not f.startswith("docs/")]
        docs = [f for f, _ in c.most_common() if f.startswith("docs/")]
        spec = "docs/specs/%s.md" % fid
        lines += ["## %s" % fid,
                  "- spec: %s" % (spec if os.path.exists(os.path.join(ROOT, spec)) else "-"),
                  "- code and tests: %s" % (", ".join(code[:20]) + (" (+%d)" % (len(code) - 20) if len(code) > 20 else "")
                                            if code else "-"),
                  "- docs: %s" % (", ".join(docs[:8]) + (" (+%d)" % (len(docs) - 8) if len(docs) > 8 else "")
                                  if docs else "-"),
                  "- named in: %s" % (", ".join(sorted(mentioned[fid])[:20]) + (" (+%d)" % (len(mentioned[fid]) - 20)
                                      if len(mentioned[fid]) > 20 else "") if fid in mentioned else "-"), ""]
    with open(os.path.join(out, "features.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    return len(set(touched) | set(mentioned))


README = """# build-map/ (generated by tools/code_map.py; untracked; run it again after pulling)

Read these before exploring the tree:
- keys.md: one line per built-in binding. `grep -i 'sampleload.*F1' build-map/keys.md` gives the action, the handler
  file:line and what it does. Contexts are the `Context::` names (Global, Pattern, SampleLoad, Wave, ...).
- screens.md: per screen, its source files, QA catalogue (and row count), simulator scenario, HIL steps, manual and specs.
- features.md: per work-item ID, its spec and the files its commits touched: start from the ID of the backlog row.
Then: the LSP tool (go to definition, find references, workspace symbols) or `rg -n`, and the area's CLAUDE.md.
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default=os.path.join(ROOT, "build-map"))
    ap.add_argument("--check", action="store_true", help="exit 1 on an unresolved binding or handler")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    problems = write_keys(a.out)
    write_screens(a.out)
    n = write_features(a.out)
    with open(os.path.join(a.out, "README.md"), "w", encoding="utf-8") as f:
        f.write(README)
    print("code_map: %d bindings, %d work items -> %s" % (len(bindings()), n, os.path.relpath(a.out, ROOT)))
    for p in problems:
        print("code_map: " + p, file=sys.stderr)
    return 1 if a.check and problems else 0


if __name__ == "__main__":
    sys.exit(main())
