#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""OS-FINALQA FQA-7, OS-DENSE: the songs of the unit's CPU runs are what docs/architecture/cpu-budget.md section 4 describes.

`o3pj_tool gen <kind>` (tests/tools/o3pj_tool.cpp) writes Projects/<folder>/project.o3pj and the WAVs it names in samples/. Three kinds: `heavy` (Heavy12, the stress song: 6 Modern + 4 Amiga sampler voices,
TB-303, FM = 12 tracks), `dense10` (6 Modern + 2 Amiga + TB-303 + FM = 10 tracks) and `dense8` (4 Modern + 2 Amiga + TB-303 + FM = 8 tracks), the candidates of the 1.0 CPU reference. For each this test
runs the tool twice (the same bytes), loads the project through the C++ reader (`resave`: byte for byte the same file, `dump`: the model as text) and holds the dump to the mix: the sampler voices (looped, filter on,
both sends, a cutoff source) split into Modern (engine 0) and Amiga (engine 2) as asserted, one TB-303 and one FM instrument from the built-in modules, one note stream on each track in use and none on the
others, the limiter on, the send effects set, and every sample the project names is a file. The Modern voices of Dense10 are the voices of Heavy12, line for line.

Usage: heavy_song_test.py <o3pj_tool>
"""
import os
import re
import subprocess
import sys
import tempfile

# kind: (folder, project name, Modern voices, Amiga voices)
KINDS = {
    "heavy": ("Heavy", "Heavy Song", 6, 4),
    "dense10": ("Dense10", "Dense10 Song", 6, 2),
    "dense8": ("Dense8", "Dense8 Song", 4, 2),
}


def gen(tool, root, kind):
    folder = KINDS[kind][0]
    os.makedirs(os.path.join(root, folder, "samples"))
    path = os.path.join(root, folder, "project.o3pj")
    subprocess.run([tool, "gen", kind, path], check=True)
    return path


def tree(root):
    found = {}
    for base, _dirs, files in os.walk(root):
        for name in files:
            p = os.path.join(base, name)
            with open(p, "rb") as f:
                found[os.path.relpath(p, root)] = f.read()
    return found


def hexstr(h):
    return bytes.fromhex(h).decode("ascii")


def check(tool, kind, need, instrument_lines):
    """One kind: returns the length of its project file. `instrument_lines[kind]` is filled with the dump lines of its Modern sampler voices."""
    folder, name, modern, amiga = KINDS[kind]
    voices = modern + amiga
    tracks = voices + 2
    tag = kind + ": "
    with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
        path = gen(tool, a, kind)
        gen(tool, b, kind)
        files = tree(a)
        need(files == tree(b), tag + "two runs of `gen %s` differ" % kind)
        need(sorted(files) == sorted([os.path.join(folder, "project.o3pj")] + [os.path.join(folder, "samples", n) for n in ("sine.wav", "saw.wav", "pluck.wav", "hat.wav")]),
             tag + "the files written are %s" % sorted(files))
        resaved = os.path.join(a, "resaved.o3pj")
        subprocess.run([tool, "resave", path, resaved], check=True)
        with open(path, "rb") as f1, open(resaved, "rb") as f2:
            need(f1.read() == f2.read(), tag + "the project does not resave byte for byte")
        dump = subprocess.run([tool, "dump", path], check=True, capture_output=True, text=True).stdout.splitlines()

        head = re.match(r"project name=(\w+) tempo=(\d+) swing=\d+ deflen=(\d+) active=(\d+) master=(\d+),(\d+),(\d+),(\d+),(\d+)", dump[0])
        need(head is not None, tag + "no project line")
        if head:
            need(hexstr(head.group(1)) == name, tag + "the name")
            need(int(head.group(4)) == tracks, tag + "active tracks %s, wanted %d (%d Modern + %d Amiga sampler, TB-303, FM)" % (head.group(4), tracks, modern, amiga))
            need(int(head.group(9)) & 8, tag + "the master limiter is not on")
        modules = [hexstr(m.group(1)) for m in (re.match(r"module \d+ abi=\S+ name=(\w+)", l) for l in dump) if m]
        need(modules == ["tb303", "fm"], tag + "modules %s" % modules)

        sample, module, engines, lines = [], [], [], []
        for i, l in enumerate(dump):
            m = re.match(r"instrument (\d+) kind=(\d) midi=\d+ module=(\d+) name=\w+ path=(\w*) rate=(\d+)", l)
            if not m:
                continue
            slot, kind_id = int(m.group(1)), int(m.group(2))
            if kind_id == 1:
                s = dump[i + 1].split()
                # sampler type play_mode filter_type filter_enable tune fine vol pan delay reverb overdrive bit_depth ...
                need(s[0] == "sampler" and s[2] == "1" and s[4] == "1", tag + "instrument %d: not a looped sample with the filter on" % (slot + 1))
                need(int(s[9]) > 0 and int(s[10]) > 0, tag + "instrument %d: a send is 0" % (slot + 1))
                envs = dump[i + 1].split(" | env ")[1:7]  # env: amount delay attack hold decay sustain release loop enable
                need(len(envs) == 6 and envs[2].split()[-1] == "1", tag + "instrument %d: the cutoff has no source" % (slot + 1))
                rel = hexstr(m.group(4))
                need(os.path.isfile(os.path.join(a, folder, rel)), tag + "instrument %d names %s, which is not written" % (slot + 1, rel))
                sample.append(slot)
                eng = re.search(r" engine=(\d+)", l)
                engines.append(int(eng.group(1)) if eng else -1)
                lines.append(l + "\n" + dump[i + 1])
            elif kind_id == 3:
                module.append((slot, int(m.group(3))))
        need(sample == list(range(voices)), tag + "sample instruments in slots %s" % sample)
        need(engines == [0] * modern + [2] * amiga, tag + "sampler engines %s, wanted %d Modern (0) then %d Amiga (2)" % (engines, modern, amiga))
        need(module == [(voices, 0), (voices + 1, 1)], tag + "module instruments %s" % module)
        instrument_lines[kind] = lines[:modern]

        rows = None
        for l in dump:
            m = re.match(r"pattern 1 lens=\S+ name=\w* rows=(\S+)", l)
            if m:
                rows = m.group(1).split(",")
        need(rows is not None and len(rows) == 16, tag + "no pattern 1")
        if rows:
            empty = rows[15]
            need(all(rows[t] == empty for t in range(tracks, 15)), tag + "tracks %d to 16 differ (all are empty)" % (tracks + 1))
            for t in range(tracks):
                need(rows[t] != empty, tag + "track %d has no steps" % (t + 1))
        sendfx = [l for l in dump if l.startswith("sendfx ")]
        need(len(sendfx) == 1 and sendfx[0].split("=")[1].split(",")[0] == "1", tag + "the send effects are not set (ping-pong on)")
    return len(files[os.path.join(folder, "project.o3pj")])


def main(argv):
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    tool = argv[1]
    problems = []

    def need(ok, what):
        if not ok:
            problems.append(what)

    lines, sizes = {}, {}
    for kind in KINDS:
        sizes[kind] = check(tool, kind, need, lines)
    # a Dense10 Modern voice is the Heavy12 voice with the same number (the recipe is one routine)
    need(lines.get("dense10") == lines.get("heavy")[:6] if lines.get("heavy") else False, "the Modern voices of dense10 are not those of heavy")
    need(lines.get("dense8") == lines.get("heavy")[:4] if lines.get("heavy") else False, "the Modern voices of dense8 are not the first four of heavy")

    for p in problems:
        print("heavy_song: " + p, file=sys.stderr)
    if problems:
        return 1
    print("heavy_song: ok (%s)" % ", ".join("%s: %d sampler + tb303 + fm, %d bytes" % (k, KINDS[k][2] + KINDS[k][3], sizes[k]) for k in KINDS))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
