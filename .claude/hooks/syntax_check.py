#!/usr/bin/env python3
"""PostToolUse(Edit|Write) check: compile the edited translation unit.

Runs the edited .c/.cc/.cpp file through its exact compile_commands.json
invocation with -fsyntax-only (no objects, no dependency files). Errors block
(exit 2, shown to the model); warnings in the edited file are fed back as
context. A header is checked through one unit that includes it: the source of
the same name first, else the first includer in the database.
`syntax_check.py FILE...` checks files by hand (after an O3_BULK_EDIT=1 shell
edit, see code_nav_guard.py).

The compile databases come from the host build directories of the tree
(build-os-host, build-os-ci-host), then any other build*/ directory (newest
first), so files that only the cross builds compile (platform/mk66, ...) are
found too. A file that is in no database is skipped silently.
"""
import json
import os
import shlex
import subprocess
import sys
from pathlib import Path

PROJECT = Path(os.environ.get("CLAUDE_PROJECT_DIR") or Path(__file__).resolve().parents[2])
SOURCE_EXTS = {".c", ".cpp", ".cc"}
HEADER_EXTS = {".h", ".hh", ".hpp"}
DROP_WITH_ARG = {"-o", "-MF", "-MT", "-MQ"}
DROP_FLAGS = {"-MD", "-MMD", "-c"}
PREFERRED_DIRS = ("build-os-host", "build-os-ci-host")
MAX_LINES = 40


def checkout_root(source: Path) -> Path:
    """The checkout (main tree or a git worktree) that contains the file."""
    for parent in source.parents:
        if (parent / ".git").exists():
            return parent
    return PROJECT


def compile_databases(root: Path):
    first = [root / d / "compile_commands.json" for d in PREFERRED_DIRS]
    first = [p for p in first if p.is_file()]
    others = sorted((p for p in root.glob("build*/compile_commands.json") if p not in first),
                    key=lambda p: p.stat().st_mtime, reverse=True)
    return first + others


def find_entry(root: Path, source: Path):
    for db in compile_databases(root):
        try:
            entries = json.loads(db.read_text())
        except (OSError, json.JSONDecodeError):
            continue
        for entry in entries:
            entry_file = Path(entry["file"])
            if not entry_file.is_absolute():
                entry_file = Path(entry["directory"]) / entry_file
            if entry_file.resolve() == source:
                return entry
    return None


def syntax_only_argv(entry):
    argv = entry.get("arguments") or shlex.split(entry["command"])
    out, skip = [], False
    for arg in argv:
        if skip:
            skip = False
        elif arg in DROP_WITH_ARG:
            skip = True
        elif arg not in DROP_FLAGS:
            out.append(arg)
    return out + ["-fsyntax-only"]


def including_unit(root: Path, header: Path):
    """A compile entry that includes the header: the source of the same stem first, then any includer."""
    needles = (f'/{header.name}"', f'"{header.name}"', f'/{header.name}>', f'<{header.name}>')
    for db in compile_databases(root):
        try:
            entries = json.loads(db.read_text())
        except (OSError, json.JSONDecodeError):
            continue
        paths = []
        for entry in entries:
            entry_file = Path(entry["file"])
            if not entry_file.is_absolute():
                entry_file = Path(entry["directory"]) / entry_file
            paths.append((entry_file.resolve(), entry))
        for entry_file, entry in paths:
            if entry_file.stem == header.stem and entry_file.parent.parent == header.parent.parent:
                return entry_file, entry
        for entry_file, entry in paths:
            try:
                text = entry_file.read_text(errors="replace")
            except OSError:
                continue
            if any(n in text for n in needles):
                return entry_file, entry
    return None, None


def check_file(raw: str):
    """(exit code, stderr text, context text) for one edited file."""
    source = Path(raw).resolve()
    if source.suffix not in SOURCE_EXTS | HEADER_EXTS:
        return 0, "", ""
    root = checkout_root(source)  # worktree edits use that worktree's build
    if not source.is_relative_to(root):
        return 0, "", ""
    unit = source
    if source.suffix in HEADER_EXTS:
        unit, entry = including_unit(root, source)
    else:
        entry = find_entry(root, source)
    if entry is None:
        return 0, "", ""  # not in the build (excluded file), no includer, or no compile database yet

    try:
        result = subprocess.run(["nice", "-n", "10"] + syntax_only_argv(entry),
                                cwd=entry["directory"], capture_output=True,
                                text=True, timeout=90)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return 0, f"syntax check skipped: {exc}", ""

    rel = source.relative_to(root)
    via = "" if unit == source else f" (checked through {unit.relative_to(root)})"
    lines = result.stderr.splitlines()
    if result.returncode != 0:
        return 2, (f"Compile errors in {rel}{via} (-fsyntax-only, same flags as the build):\n"
                   + "\n".join(lines[:MAX_LINES])), ""

    own_warnings = [l for l in lines if "warning:" in l and str(source) in l]
    if own_warnings:
        return 0, "", f"Compiler warnings in {rel}{via}:\n" + "\n".join(own_warnings[:15])
    return 0, "", ""


def main() -> int:
    if len(sys.argv) > 1:  # syntax_check.py FILE...: after an O3_BULK_EDIT=1 shell edit
        worst = 0
        for raw in sys.argv[1:]:
            rc, err, ctx = check_file(raw)
            worst = max(worst, rc)
            print(err or ctx or f"ok {raw}")
        return worst
    try:
        payload = json.load(sys.stdin)
    except json.JSONDecodeError:
        return 0
    tool_input = payload.get("tool_input") or {}
    raw = tool_input.get("file_path") or (payload.get("tool_response") or {}).get("filePath")
    if not raw:
        return 0
    rc, err, ctx = check_file(raw)
    if err:
        print(err, file=sys.stderr)
    if ctx:
        print(json.dumps({"hookSpecificOutput": {"hookEventName": "PostToolUse", "additionalContext": ctx}}))
    return rc


if __name__ == "__main__":
    sys.exit(main())
