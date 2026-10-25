#!/usr/bin/env python3
"""PreToolUse(Bash) guard: refuse anything that could flash hardware.

Flashing is a physical, user-visible action gated by the rules in CLAUDE.md
(SD cards and firmware delivery) and the skill fw-gate-check. No agent may run
an uploader, a debug probe or write a raw block device. Agents may write files,
including candidate images, on any SD card mounted from removable media under
/media or /run/media (maintainer, 2026-10-17: no test-card marker or volume
check); other mounts (/mnt) stay read-only. The update itself goes only through
the documented OS update path. Exit code 2 blocks the tool call and shows the
reason.
"""
import json
import os
import re
import shlex
import sys

# A tool only counts when it is in command position, so `grep teensy_loader_cli`
# or reading docs that mention these names is not blocked.
CMD_START = r"(?:^|[;&|`(]|\$\(|\bsudo\s+|\bexec\s+|\benv\s+(?:\S+=\S+\s+)*)\s*(?:\S*/)?"
UPLOADERS = (
    r"teensy_loader_cli|teensy_post_compile|teensy_reboot|teensy_restart"
    r"|openocd|pyocd|JLinkExe|JLink|JLinkGDBServer\w*|dfu-util|st-flash"
    r"|bossac|nrfjprog|blhost|kinetis-flash"
)

RULES = [
    (re.compile(CMD_START + r"(?:" + UPLOADERS + r")\b"),
     "hardware uploader/debug-probe tool"),
    (re.compile(CMD_START + r"tycmd\s+(?:upload|reset|bootloader)\b"),
     "tycmd upload/reset"),
    (re.compile(r"\bdd\b[^|;&]*\bof=/dev/(?:sd|mmcblk|nvme|disk|hd)"),
     "dd to a block device"),
    (re.compile(r"\b(?:cp|mv|rsync|install|scp)\b[^|;&]*\.(?:ptf|hex)\b[^|;&]*"
                r"\s(?:/media/|/run/media/|/mnt/)"),
     "copying a firmware image to removable media"),
    (re.compile(r">\s*(?:/media/|/run/media/|/mnt/)\S*\.(?:ptf|hex)\b"),
     "writing a firmware image to removable media"),
]


# Removable media: an SD card is mounted under /media or /run/media and may be
# written (any card, maintainer 2026-10-17); a /mnt mount may be a fixed disk
# and stays read-only.
MEDIA = ("/media/", "/run/media/", "/mnt/")
WRITABLE_MEDIA = ("/media/", "/run/media/")
MEDIA_PATH = re.compile(r"(?:/run/media/|/media/|/mnt/)[^\s'\";|&<>]*")


def _media_root(path):
    """Mount root: /media/<user>/<vol>, /run/media/<user>/<vol> or /mnt/<vol>."""
    for prefix, depth in (("/run/media/", 2), ("/media/", 2), ("/mnt/", 1)):
        if path.startswith(prefix):
            parts = [p for p in path[len(prefix):].split("/") if p]
            if len(parts) < depth:
                return None
            return prefix + "/".join(parts[:depth])
    return None


def _is_writable_card(path):
    """A mounted volume under /media/<user>/<vol> or /run/media/<user>/<vol>."""
    root = _media_root(path)
    return bool(root) and root.startswith(WRITABLE_MEDIA) and os.path.ismount(root)


def _only_writable_cards(command):
    paths = MEDIA_PATH.findall(command)
    return bool(paths) and all(_is_writable_card(p) for p in paths)
WRITES_LAST_ARG = {"cp", "rsync", "install", "scp"}
WRITES_ANY_ARG = {"mv", "rm", "rmdir", "touch", "mkdir", "tee", "truncate",
                  "ln", "chmod", "chown", "unzip", "shred"}
OPERATORS = {";", "&", "|", "&&", "||", "(", ")", "\n"}
REDIRECTS = {">", ">>", ">|", "&>", "1>", "2>"}


def _segments(command):
    """Split a shell command into simple commands; quotes are respected."""
    lexer = shlex.shlex(command, posix=True, punctuation_chars=";&|()<>")
    lexer.whitespace_split = True
    segment = []
    try:
        for tok in lexer:
            if tok in OPERATORS:
                yield segment
                segment = []
            else:
                segment.append(tok)
    except ValueError:  # unbalanced quotes: fall back to a plain split
        segment = command.split()
    yield segment


def _write_target(cmd, toks, args):
    if cmd in WRITES_LAST_ARG and args and args[-1].startswith(MEDIA):
        return f"{cmd} onto removable media"
    if cmd in WRITES_ANY_ARG and any(a.startswith(MEDIA) for a in args):
        return f"{cmd} on removable media"
    if cmd == "sed" and any(t.startswith("-i") for t in toks) \
            and any(a.startswith(MEDIA) for a in args):
        return "sed -i on removable media"
    if cmd == "dd" and any(a.startswith("of=" + m) for a in args
                           for m in MEDIA):
        return "dd onto removable media"
    if cmd == "git" and args[:1] == ["add"] and any(
            a.startswith(("fixtures", "build-sdtest")) or "/fixtures/" in a
            or "/build-sdtest/" in a for a in args[1:]):
        return "staging fixtures/ or build-sdtest/ (test material of unknown licence)"
    return None


def media_write(command):
    for toks in _segments(command):
        for i, tok in enumerate(toks[:-1]):
            if tok in REDIRECTS and toks[i + 1].startswith(MEDIA):
                return "redirecting output onto removable media"
        while toks and (toks[0] in ("sudo", "env", "exec", "nice")
                        or re.match(r"^\w+=", toks[0])):
            toks = toks[1:]
        if not toks:
            continue
        args = [t for t in toks[1:] if not t.startswith("-")]
        what = _write_target(os.path.basename(toks[0]), toks, args)
        if what:
            return what
    return None


def block(what, why):
    print(f"BLOCKED by project guard: {what}. {why}", file=sys.stderr)
    return 2


def main() -> int:
    try:
        payload = json.load(sys.stdin)
    except json.JSONDecodeError:
        return 0
    tool_input = payload.get("tool_input") or {}
    path = tool_input.get("file_path") or tool_input.get("notebook_path") or ""
    if path.startswith(MEDIA) and not _is_writable_card(path):
        return block("editing a file outside a mounted SD card",
                     "Only a card mounted under /media or /run/media may be "
                     "written by agents; copy files into the scratchpad or "
                     "the repository and work there.")
    command = tool_input.get("command") or ""
    card_only = _only_writable_cards(command)
    for pattern, what in RULES:
        if pattern.search(command):
            if card_only and "removable media" in what:
                continue
            return block(what,
                         "Updating hardware is a physical action that needs a "
                         "verified target, a passed gate and the maintainer's "
                         "go. Produce the candidate image and its SHA-256, run "
                         "/fw-gate-check, and use the documented OS update "
                         "path or a mounted SD card instead.")
    what = None if card_only else media_write(command)
    if what:
        return block(what, "Only a card mounted under /media or /run/media is "
                     "writable for agents (not /mnt, not an unmounted path), "
                     "and fixtures/ and build-sdtest/ are never committed. "
                     "Write only to the scratchpad, the repo or a mounted SD "
                     "card.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
