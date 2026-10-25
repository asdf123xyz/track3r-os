#!/usr/bin/env python3
"""SessionStart hook: inject a short state brief so no session starts cold.

Prints (stdout becomes session context) the branch, uncommitted files, live
worktrees and the "## Next" section of docs/STATUS.md. Kept under ~40 lines
on purpose: token spend is kept low. Fails open (prints what it can).
"""
import json
import os
import subprocess
import sys

MAX_NEXT_LINES = 12
MAX_LINE_CHARS = 200
STATUS = os.path.join("docs", "STATUS.md")


def git(root, *args):
    try:
        return subprocess.run(["git", "-C", root, *args], capture_output=True,
                              text=True, timeout=10).stdout.rstrip("\n")
    except (OSError, subprocess.SubprocessError):
        return ""


def status_next(root):
    path = os.path.join(root, STATUS)
    try:
        with open(path, encoding="utf-8") as fh:
            lines = fh.read().splitlines()
    except OSError:
        return []
    out, inside = [], False
    for line in lines:
        if line.startswith("## "):
            if inside:
                break
            inside = line.strip() == "## Next"
            continue
        if inside and line.strip():
            if len(line) > MAX_LINE_CHARS:
                line = line[:MAX_LINE_CHARS - 1] + "..."
            out.append(line)
    if len(out) > MAX_NEXT_LINES:
        out = out[:MAX_NEXT_LINES] + ["  ... (rest in docs/STATUS.md)"]
    return out


def main():
    try:
        payload = json.load(sys.stdin)
    except json.JSONDecodeError:
        payload = {}
    root = os.environ.get("CLAUDE_PROJECT_DIR") or payload.get("cwd") or "."
    branch = git(root, "branch", "--show-current") or "(detached)"
    head = git(root, "log", "-1", "--format=%h %s")[:90]
    dirty = [l for l in git(root, "status", "--porcelain").splitlines() if l]
    here = os.path.realpath(root)
    trees = [l.split()[0] for l in git(root, "worktree", "list").splitlines()
             if l and os.path.realpath(l.split()[0]) != here]

    print("## Session brief (auto, SessionStart hook)")
    print(f"- Branch `{branch}` at {head}; {len(dirty)} uncommitted path(s)"
          + (": " + ", ".join(l[3:] for l in dirty[:5]) if dirty else "."))
    if trees:
        print(f"- {len(trees)} extra worktree(s): "
              + ", ".join(os.path.basename(t) for t in trees[:6])
              + ". Another session or agent may be using them; run ListAgents "
              "before touching shared files.")
    nxt = status_next(root)
    if nxt:
        print("- docs/STATUS.md -> Next:")
        print("\n".join(nxt))
    else:
        print("- No \"## Next\" section found in docs/STATUS.md; read it and "
              "docs/roadmap/BACKLOG.md.")
    print("- Rules: CLAUDE.md. Never flash; any SD card may be written, "
          "never a fixed or system disk; verify before claiming done; builds under nice -n 10, "
          "at most -j4; keep token spend low.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
