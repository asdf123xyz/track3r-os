#!/usr/bin/env python3
"""Stop/SubagentStop hook: verification before completion.

If code in the working tree changed after the last recorded verification, the
agent may not stop yet: it is told once which command proves its work. The
evidence is the file timestamps that ctest and tools/ci.sh already write:
  build system (CMakeLists.txt, cmake/, tools/ci.sh)
      -> build-os-ci-*/Testing/Temporary/LastTest.log   (a tools/ci.sh run)
  sources, tests, tools (anything else the rule below lists)
      -> any build-os*/Testing/Temporary/LastTest.log   (ctest of any build dir)
`stop_hook_active` guarantees a single nudge, never a loop. Fails open.
"""
import glob
import json
import os
import subprocess
import sys

# Build-system files: only a full tools/ci.sh run proves them.
BUILD_PREFIXES = ("CMakeLists.txt", "cmake/", "tools/ci.sh")
BUILD_SUFFIXES = ("/CMakeLists.txt",)
# Module directories of the tree, plus tests/ and tools/.
CODE_PREFIXES = ("app/", "audio/", "core/", "eve/", "formats/", "hal/", "midi/",
                 "model/", "module/", "modules/", "platform/", "seq/", "sim/",
                 "ui/", "usb/", "tests/", "tools/", "third_party/")
CODE_EXT = (".c", ".cc", ".cpp", ".h", ".hpp", ".py", ".txt", ".cmake", ".ld",
            ".in", ".s", ".S", ".sh")


def newest(paths):
    times = [os.path.getmtime(p) for p in paths if os.path.exists(p)]
    return max(times) if times else 0.0


def is_build(path):
    return path.startswith(BUILD_PREFIXES) or path.endswith(BUILD_SUFFIXES)


def main():
    try:
        payload = json.load(sys.stdin)
    except json.JSONDecodeError:
        return 0
    if payload.get("stop_hook_active"):
        return 0
    root = payload.get("cwd") or os.getcwd()
    try:
        out = subprocess.run(
            ["git", "-C", root, "status", "--porcelain", "--untracked-files=all"],
            capture_output=True, text=True, timeout=10).stdout
    except (OSError, subprocess.SubprocessError):
        return 0
    changed = [l[3:].split(" -> ")[-1] for l in out.splitlines()
               if l and not l.startswith(" D") and l[3:].endswith(CODE_EXT)]
    if not changed:
        return 0

    log = os.path.join("Testing", "Temporary", "LastTest.log")
    ci_ev = newest(glob.glob(os.path.join(root, "build-os-ci-*", log)))
    any_ev = newest(glob.glob(os.path.join(root, "build-os*", log)))

    def stale(files, evidence):
        mtime = newest(os.path.join(root, f) for f in files)
        return files if files and mtime > evidence else []

    need_build = stale([f for f in changed if is_build(f)], ci_ev)
    need_tests = stale([f for f in changed
                        if f.startswith(CODE_PREFIXES) and not is_build(f)], any_ev)
    if not (need_build or need_tests):
        return 0

    steps = []
    if need_build:
        steps.append("nice -n 10 tools/ci.sh host   (build system changed: "
                     + ", ".join(need_build[:4]) + "; add qemu and mk66 when "
                     "the cross builds are affected)")
    if need_tests:
        steps.append("nice -n 10 cmake --build build-os-host -j4 && ctest "
                     "--test-dir build-os-host --output-on-failure -j4   "
                     "(or O3OS_CI_SCOPE=changed nice -n 10 tools/ci.sh host; "
                     "changed: " + ", ".join(need_tests[:4]) + ")")
    reason = ("Verification before completion: code changed after the last "
              "recorded build/test run. Run and quote the result (exit code, "
              "ctest pass count, warnings, flash/SRAM when relevant) before "
              "claiming done:\n  - " + "\n  - ".join(steps)
              + "\nIf the change is intentionally unverified (WIP, docs-only "
              "intent), say so explicitly in your final message instead.")
    print(json.dumps({"decision": "block", "reason": reason}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
