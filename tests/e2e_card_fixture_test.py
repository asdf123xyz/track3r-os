#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""OS-E2E-1b: the simulator's card fixture is what its generator writes, and nothing else.

The card of sim/scenarios/e2e/e2e_card.txt (`card e2e_card`) is GENERATED, never committed (a binary file does not belong in git): sim/CMakeLists.txt, target
e2e_card_fixture, runs `o3pj_tool gen small` (tests/tools/o3pj_tool.cpp, the fixture the format tests use: the file is made by the project writer, not by hand) into
<build>/sim/cards/e2e_card/Projects/Small Song/project.o3pj on every build. This test runs the generator again and compares: the card must hold that one file with the
bytes the tool writes, no other file or folder, and no link. (A change of the format moves the file; then review the `sha=` and `size=` lines of the scenario.)

Usage: e2e_card_fixture_test.py <the generated card folder> <o3pj_tool>
"""
import os
import subprocess
import sys
import tempfile

PROJECT = os.path.join("Projects", "Small Song", "project.o3pj")


def tree(root):
    """-> sorted relative paths of everything under root (files, folders and links alike)."""
    found = []
    for base, dirs, files in os.walk(root, followlinks=False):
        for name in dirs + files:
            found.append(os.path.relpath(os.path.join(base, name), root))
    return sorted(found)


def main(argv):
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    card, tool = argv[1], argv[2]
    if not os.path.isdir(card):
        print("e2e_card_fixture: %s is not there: build the target e2e_card_fixture (cmake --build <dir>)" % card, file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "project.o3pj")
        subprocess.run([tool, "gen", "small", out], check=True)
        with open(out, "rb") as f:
            fresh = f.read()
    want = sorted([os.path.dirname(os.path.dirname(PROJECT)), os.path.dirname(PROJECT), PROJECT])  # Projects, Projects/Small Song, the file
    found = tree(card)
    if found != want:
        print("e2e_card_fixture: the card holds %s, wanted %s" % (found, want), file=sys.stderr)
        return 1
    path = os.path.join(card, PROJECT)
    if os.path.islink(path) or not os.path.isfile(path):
        print("e2e_card_fixture: %s is not a plain file" % PROJECT, file=sys.stderr)
        return 1
    with open(path, "rb") as f:
        have = f.read()
    if have != fresh:
        print("e2e_card_fixture: %s differs from `o3pj_tool gen small` (%d bytes on the card, %d generated): build the target e2e_card_fixture again" % (PROJECT, len(have), len(fresh)),
              file=sys.stderr)
        return 1
    print("e2e_card_fixture: ok (%d bytes)" % len(fresh))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
