#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""OS-E2E-13: the card of the pro-session journey is what its generator writes, and nothing else.

The card of sim/scenarios/e2e/e2e_pro_session.txt (`card e2e_pro`) is GENERATED, never committed: sim/CMakeLists.txt, target e2e_pro_fixture, runs `o3pj_tool gen kit`
(tests/tools/o3pj_tool.cpp) into <build>/sim/cards/e2e_pro/Projects/Kit/ on every build: the project file and the four WAV files it names in samples/. This test runs the generator again
into a scratch folder and compares the whole tree: the same files with the same bytes, no other file or folder, no link.

Usage: e2e_pro_fixture_test.py <the generated card folder> <o3pj_tool>
"""
import os
import subprocess
import sys
import tempfile

PROJECT = os.path.join("Projects", "Kit")


def tree(root):
    """-> {relative path: bytes, or None for a folder} of everything under root; a link is an error."""
    found = {}
    for base, dirs, files in os.walk(root, followlinks=False):
        for name in dirs + files:
            path = os.path.join(base, name)
            if os.path.islink(path):
                raise SystemExit("e2e_pro_fixture: %s is a link" % os.path.relpath(path, root))
            rel = os.path.relpath(path, root)
            if os.path.isdir(path):
                found[rel] = None
            else:
                with open(path, "rb") as f:
                    found[rel] = f.read()
    return found


def main(argv):
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    card, tool = argv[1], argv[2]
    if not os.path.isdir(card):
        print("e2e_pro_fixture: %s is not there: build the target e2e_pro_fixture (cmake --build <dir>)" % card, file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory() as tmp:
        os.makedirs(os.path.join(tmp, PROJECT, "samples"))
        subprocess.run([tool, "gen", "kit", os.path.join(tmp, PROJECT, "project.o3pj")], check=True)
        want = tree(tmp)
    have = tree(card)
    if sorted(have) != sorted(want):
        print("e2e_pro_fixture: the card holds %s, wanted %s" % (sorted(have), sorted(want)), file=sys.stderr)
        return 1
    for rel in sorted(want):
        if have[rel] != want[rel]:
            print("e2e_pro_fixture: %s differs from `o3pj_tool gen kit`: build the target e2e_pro_fixture again" % rel, file=sys.stderr)
            return 1
    files = [r for r in want if want[r] is not None]
    if len(files) != 5:
        print("e2e_pro_fixture: the generator wrote %d files, expected the project and 4 WAVs" % len(files), file=sys.stderr)
        return 1
    print("e2e_pro_fixture: ok (%d files, %d bytes)" % (len(files), sum(len(want[r]) for r in files)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
