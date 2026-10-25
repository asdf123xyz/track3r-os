#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The large card of the random user (OS-QANET B1, docs/specs/OS-QANET.md part B): a made-up SD card of about 2000 files, generated, never committed.

  qanet_card.py --out DIR --o3pj-tool PATH [--seed N]

writes, under DIR (the build's sim/cards/qanet_card), 52 folders, up to four levels deep, and 2000 files in the mix of a used card: WAV, AIFF, IFF 8SVX, `.pti`, ProTracker /
Scream Tracker modules, native projects (`o3pj_tool gen small`, valid) and broken ones, and junk (hidden macOS files, text, names without an extension, a folder called like a file). Names are
long, have spaces and non-ASCII letters, and differ in case. One folder holds 600 files, more than the listing's 256 entries (app/dir_index.h). Every file is tiny (the audio is a few hundred
frames), so a run can copy the whole card in milliseconds; nothing in it is a real recording or a real module: the bytes are made from a seeded pseudo-random generator and integer arithmetic.

The same seed writes the same bytes, in the same order (os.walk-independent: the list is built first and sorted). The folder is removed first, so nothing stale stays (DIR must be a folder called qanet_card* and not a symbolic link: anything else is refused, and nothing is removed). Standard library only;
run with `python3 -I`. tests/qanet/ reads the folder; `--check` prints the counts and exits 1 when a number the spec names (files >= 2000, folders >= 40) is not met.
"""
import argparse
import os
import random
import shutil
import struct
import subprocess
import sys

FILES = 2000
FOLDERS = 40

WORDS = ["kick", "snare", "hat", "clap", "bass", "pad", "lead", "pluck", "vox", "loop", "break", "amen", "808", "909", "tom", "rim", "fx", "riser", "noise", "sine", "saw", "chord", "stab", "perc"]
ACCENTS = ["café", "niño", "über", "søng", "naïve", "ångström"]


def tone_s16(rng, frames, channels):
    """A decaying square-ish blip in 16-bit: integer arithmetic only, so the bytes are the same everywhere."""
    out = bytearray()
    period = rng.randrange(20, 120)
    for i in range(frames):
        v = (12000 if (i // (period // 2 + 1)) % 2 == 0 else -12000) * (frames - i) // frames
        for _ in range(channels):
            out += struct.pack("<h", v)
    return bytes(out)


def wav_bytes(rng):
    channels = rng.choice((1, 1, 1, 2))
    rate = rng.choice((22050, 44100, 44100, 48000, 11025))
    frames = rng.randrange(40, 500)
    data = tone_s16(rng, frames, channels)
    fmt = struct.pack("<HHIIHH", 1, channels, rate, rate * channels * 2, channels * 2, 16)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(data)) + data
    return b"RIFF" + struct.pack("<I", len(body)) + body


def aiff_bytes(rng):
    channels = rng.choice((1, 2))
    frames = rng.randrange(40, 400)
    data = bytearray()
    raw = tone_s16(rng, frames, channels)
    for i in range(0, len(raw), 2):  # big endian
        data += raw[i + 1:i + 2] + raw[i:i + 1]
    rate80 = struct.pack(">HQ", 0x400E, 0xAC44 << 48)  # 44100 as the 80-bit extended of the format
    comm = struct.pack(">hIh", channels, frames, 16) + rate80
    ssnd = struct.pack(">II", 0, 0) + bytes(data)
    body = b"AIFF" + b"COMM" + struct.pack(">I", len(comm)) + comm + b"SSND" + struct.pack(">I", len(ssnd)) + ssnd
    return b"FORM" + struct.pack(">I", len(body)) + body


def svx_bytes(rng):
    frames = rng.randrange(40, 400) & ~1
    period = rng.randrange(16, 100)
    data = bytes(((100 if (i // (period // 2 + 1)) % 2 == 0 else -100) * (frames - i) // frames) & 0xFF for i in range(frames))
    vhdr = struct.pack(">IIIHBBI", frames, 0, frames, 8363, 1, 0, 0x10000)
    body = b"8SVX" + b"VHDR" + struct.pack(">I", len(vhdr)) + vhdr + b"BODY" + struct.pack(">I", len(data)) + data
    if len(data) & 1:
        body += b"\0"
    return b"FORM" + struct.pack(">I", len(body)) + body


def noise(rng, n):
    return bytes(rng.randrange(256) for _ in range(n))


def junk_for(rng, kind):
    """Bytes that are not the file their name says: the loaders and the listing meet them on every real card."""
    return noise(rng, {"pti": 2000, "mod": 1084 + 64, "s3m": 200, "txt": 300, "none": 50, "bak": 20}[kind])


def name_for(rng, i):
    parts = [rng.choice(WORDS)]
    if rng.random() < 0.5:
        parts.append(rng.choice(WORDS))
    if rng.random() < 0.15:
        parts.append(rng.choice(ACCENTS))
    parts.append("%03d" % (i % 1000))
    if rng.random() < 0.08:  # long: up to the 100 bytes a name may have here
        parts.append("a very long descriptive name of a sample that somebody typed on a computer".replace(" ", "_" if rng.random() < 0.5 else " "))
    joined = (" " if rng.random() < 0.5 else "_").join(parts)
    return joined.encode("utf-8")[:90].decode("utf-8", "ignore").rstrip(" ")


def folder_plan(rng):
    """38 folders under the root, depth 1..5; the 14 of the projects come later: 52 in all, more than the 40 of the spec."""
    folders = ["Projects", "Samples", "Instruments", "Modules", "Recordings"]
    for top in ("Samples", "Instruments", "Modules"):
        for w in ("Drums", "Bass", "Vocals", "Loops", "Fx"):
            folders.append("%s/%s" % (top, w))
    for w in ("Drums", "Loops"):
        folders.append("Samples/Drums/%s" % w)
        folders.append("Samples/Drums/%s/Old" % w)
    folders += ["Samples/Big", "Samples/Drums/Loops/Old/Deep", "Samples/Empty", "Samples/Only junk", "Samples/kick.wav", "Recordings/Take 1", "Recordings/Take 2", "Backup", "Backup/2023", "Backup/2024",
                "Samples/café über", "Samples/UPPER", "Samples/lower", "Samples/a folder with a really long name that goes on and on and on and on"]
    return folders


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--o3pj-tool", required=True)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    rng = random.Random(args.seed)

    out = args.out
    if not os.path.basename(os.path.normpath(out)).startswith("qanet_card") or os.path.islink(out):
        print("qanet_card: --out must be a folder called qanet_card* (and not a symbolic link): %s" % out, file=sys.stderr)
        return 2
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    folders = folder_plan(rng)
    for f in folders:
        os.makedirs(os.path.join(out, f))
    sample_dirs = [f for f in folders if f.split("/")[0] in ("Samples", "Recordings", "Backup", "Instruments", "Modules") and f not in ("Samples/Empty", "Samples/Only junk", "Samples/kick.wav")]

    # projects: valid native ones (the generator's own `small` fixture), broken ones the loader must refuse, a folder that is not one
    small = os.path.join(out, "Projects", ".small.tmp")
    subprocess.run([args.o3pj_tool, "gen", "small", small], check=True, stdout=subprocess.DEVNULL)
    small_bytes = open(small, "rb").read()
    os.remove(small)
    count = 0
    for n in range(1, 9):
        d = os.path.join(out, "Projects", "Song %02d" % n)
        os.makedirs(d)
        open(os.path.join(d, "project.o3pj"), "wb").write(small_bytes)
        count += 1
    for n, cut in ((9, 20), (10, len(small_bytes) // 2), (11, len(small_bytes) - 1)):  # truncated: never a valid project
        d = os.path.join(out, "Projects", "Broken %02d" % n)
        os.makedirs(d)
        open(os.path.join(d, "project.o3pj"), "wb").write(small_bytes[:cut])
        count += 1
    d = os.path.join(out, "Projects", "Junk OG")
    os.makedirs(d)
    open(os.path.join(d, "project.mt"), "wb").write(noise(rng, 600))
    d = os.path.join(out, "Projects", "Recover Me")
    os.makedirs(d)
    open(os.path.join(d, "project.o3pj.bak"), "wb").write(small_bytes)
    os.makedirs(os.path.join(out, "Projects", "Not a project"))
    count += 3
    folders += ["Projects/Song %02d" % n for n in range(1, 9)]

    # the files: a plan first (name, kind, folder), then written in sorted order
    kinds = (["wav"] * 62 + ["aif"] * 8 + ["svx"] * 5 + ["pti"] * 4 + ["mod"] * 2 + ["s3m"] * 1 + ["txt"] * 5 + ["none"] * 3 + ["hidden"] * 4 + ["bak"] * 1 + ["upper"] * 5)
    plan = []
    big = 600
    i = 0
    while len(plan) + count < FILES:
        if len(plan) < big:
            folder = "Samples/Big"
            kind = "wav" if rng.random() < 0.9 else rng.choice(("aif", "svx", "txt"))
        else:
            folder = rng.choice(sample_dirs + [""] * 6)  # some at the root
            kind = rng.choice(kinds)
        plan.append((folder, "%s" % name_for(rng, i), kind))
        i += 1
    for folder, base, kind in plan:
        ext = {"wav": ".wav", "aif": rng.choice((".aif", ".aiff", ".aifc")), "svx": rng.choice((".8svx", ".svx", ".iff")), "pti": ".pti", "mod": ".mod", "s3m": ".s3m", "txt": ".txt", "none": "", "hidden": "", "bak": ".bak", "upper": ".WAV"}[kind]
        name = ("._" + base if kind == "hidden" else base) + ext
        path = os.path.join(out, folder, name)
        if os.path.exists(path):
            path = os.path.join(out, folder, base + "_dup" + ext)
            if os.path.exists(path):
                continue
        if kind in ("wav", "upper"):
            data = wav_bytes(rng)
        elif kind == "aif":
            data = aiff_bytes(rng)
        elif kind == "svx":
            data = svx_bytes(rng)
        else:
            data = junk_for(rng, kind if kind in ("pti", "mod", "s3m", "txt", "none", "bak") else "txt")
        with open(path, "wb") as fh:
            fh.write(data)
        count += 1
    # junk beside the listed files, and a folder that looks like a file with a real file inside it
    open(os.path.join(out, "Samples", "Only junk", ".DS_Store"), "wb").write(noise(rng, 100))
    open(os.path.join(out, "Samples", "Only junk", "readme.txt"), "wb").write(noise(rng, 100))
    open(os.path.join(out, "Samples", "kick.wav", "inside.wav"), "wb").write(wav_bytes(rng))
    count += 3
    # the root also gets a few WAVs so that the Sample load screen has files at its first folder
    for n in range(12):
        open(os.path.join(out, "root %02d.wav" % n), "wb").write(wav_bytes(rng))
        count += 1

    total_files = sum(len(fs) for _, _, fs in os.walk(out))
    total_dirs = sum(len(ds) for _, ds, _ in os.walk(out))
    print("qanet_card: %d files, %d folders in %s" % (total_files, total_dirs, out))
    if args.check and (total_files < FILES or total_dirs < FOLDERS):
        print("qanet_card: fewer than %d files or %d folders" % (FILES, FOLDERS), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
