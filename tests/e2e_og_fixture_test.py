#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""OS-E2E J2: the card of the OG migration journey is what its generator writes, and nothing else.

The card of sim/scenarios/e2e/e2e_og_migration.txt (`card e2e_og`) is GENERATED, never committed: sim/CMakeLists.txt, target e2e_og_fixture, runs `o3og_fixture`
(tests/tools/o3og_fixture.cpp, from the synthetic byte maps of tests/support/og_fixture.h) into <build>/sim/cards/e2e_og/ on every build: Projects/OG Demo/ with project.mt,
patterns/pattern_01.mtp and instruments/01 Sine.pti. This test runs the generator again into a scratch folder and compares the whole tree: the same files with the same bytes, no
other file or folder, no link, and the three files of the sizes the format gives (2,320 and 9,260 bytes, and a 392-byte head with 200 frames of 16-bit PCM).

Usage: e2e_og_fixture_test.py <the generated card folder> <o3og_fixture>
"""
import os
import subprocess
import sys
import tempfile

SIZES = {
    os.path.join("Projects", "OG Demo", "project.mt"): 2320,
    os.path.join("Projects", "OG Demo", "patterns", "pattern_01.mtp"): 9260,
    os.path.join("Projects", "OG Demo", "instruments", "01 Sine.pti"): 392 + 2 * 200,
}


def tree(root):
    """-> {relative path: bytes, or None for a folder} of everything under root; a link is an error."""
    found = {}
    for base, dirs, files in os.walk(root, followlinks=False):
        for name in dirs + files:
            path = os.path.join(base, name)
            if os.path.islink(path):
                raise SystemExit("e2e_fixture: %s is a link" % os.path.relpath(path, root))
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
        print("e2e_fixture: %s is not there: build the target e2e_og_fixture (cmake --build <dir>)" % card, file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run([tool, tmp], check=True)
        want = tree(tmp)
    have = tree(card)
    if sorted(have) != sorted(want):
        print("e2e_fixture: the card holds %s, wanted %s" % (sorted(have), sorted(want)), file=sys.stderr)
        return 1
    for rel in sorted(want):
        if have[rel] != want[rel]:
            print("e2e_fixture: %s differs from `o3og_fixture`: build the target e2e_og_fixture again" % rel, file=sys.stderr)
            return 1
    for rel, size in SIZES.items():
        if rel not in want or want[rel] is None or len(want[rel]) != size:
            print("e2e_fixture: %s is not %d bytes" % (rel, size), file=sys.stderr)
            return 1
    files = [r for r in want if want[r] is not None]
    if len(files) != len(SIZES):
        print("e2e_fixture: the generator wrote %d files, expected %d" % (len(files), len(SIZES)), file=sys.stderr)
        return 1
    print("e2e_fixture: ok (%d files, %d bytes)" % (len(files), sum(len(want[r]) for r in files)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
