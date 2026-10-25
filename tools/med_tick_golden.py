#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Makes tests/data/med_tick_golden.txt: libxmp's per-frame period, volume and restarts for one-track MED modules (OS-008c, docs/formats/med.md slice M3).

libxmp (MIT, https://github.com/libxmp/libxmp, commit in THIRD_PARTY.md) is NOT part of the repository or the build: this script writes each scenario as an MMD1 module
with tools/mmd_build.py (a made-up sample, no real module), plays it with a small program linked against a libxmp built by hand on a developer machine, and keeps what that
program printed. The committed text file is the oracle of tests/med_tick_test.cpp. Run it only to regenerate:

    med_tick_golden.py DUMP_PROGRAM OUT_FILE

DUMP_PROGRAM FILE FRAMES NCHANNELS prints one line a frame: position row frame speed, then per channel period (units of 1/4096 of a unit of Amiga period), volume (0..64),
instrument, note, sample position. The restart marker of a frame is "the sample position went back, or this is the first frame".
"""
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mmd_build as mb  # noqa: E402

FLAG_STSLIDE = 0x20
FLAG_VOLHEX = 0x10

# scenario: (name, flags, speed, rows) where a row is (note, inst, cmd, data); 0 note = none. MED note 13 is C-2 (period 428). 0C is the volume (hex: the song has FLAG_VOLHEX).
N = lambda note=0, inst=0, cmd=0, data=0: (note, inst, cmd, data)
SCENARIOS = [
    ("note", 0, 6, [N(13, 1), N(), N()]),
    ("note_vol", FLAG_VOLHEX, 6, [N(13, 1, 0x0C, 0x20), N(), N(25, 1, 0x0C, 0x10)]),
    ("arpeggio", 0, 6, [N(13, 1, 0x00, 0x37), N(0, 0, 0x00, 0x37), N(0, 0, 0x00, 0x37)]),
    ("arpeggio_speed5", 0, 5, [N(13, 1, 0x00, 0x47), N(0, 0, 0x00, 0x47), N(0, 0, 0x00, 0x47)]),
    ("slide_up", 0, 6, [N(13, 1, 0x01, 0x04), N(0, 0, 0x01, 0x04), N(0, 0, 0x01, 0x04), N()]),
    ("slide_down", 0, 6, [N(13, 1, 0x02, 0x06), N(0, 0, 0x02, 0x06), N(0, 0, 0x02, 0x06), N()]),
    ("slide_up_stslide", FLAG_STSLIDE, 6, [N(13, 1, 0x01, 0x04), N(0, 0, 0x01, 0x04), N()]),
    ("tone_porta", 0, 6, [N(13, 1), N(25, 1, 0x03, 0x08), N(0, 0, 0x03, 0x00), N(0, 0, 0x03, 0x00), N(13, 1, 0x03, 0x20), N(0, 0, 0x03, 0x00)]),
    ("tone_porta_stslide", FLAG_STSLIDE, 6, [N(13, 1), N(25, 1, 0x03, 0x08), N(0, 0, 0x03, 0x00), N(0, 0, 0x03, 0x00)]),
    ("vibrato_deep", 0, 6, [N(13, 1, 0x04, 0x46), N(0, 0, 0x04, 0x00), N(0, 0, 0x04, 0x00), N(0, 0, 0x04, 0xA8), N(0, 0, 0x04, 0x00)]),
    ("vibrato_pt", 0, 6, [N(13, 1, 0x14, 0x46), N(0, 0, 0x14, 0x00), N(0, 0, 0x14, 0x00), N(0, 0, 0x14, 0xF8)]),
    ("vol_slide", FLAG_VOLHEX, 6, [N(13, 1, 0x0C, 0x30), N(0, 0, 0x0D, 0x04), N(0, 0, 0x0D, 0x04), N(0, 0, 0x0D, 0x20), N(0, 0, 0x0D, 0x00), N(0, 0, 0x0A, 0x03)]),
    ("tremolo", FLAG_VOLHEX, 6, [N(13, 1, 0x0C, 0x30), N(0, 0, 0x07, 0x44), N(0, 0, 0x07, 0x00), N(0, 0, 0x07, 0x00), N(0, 0, 0x07, 0xF8)]),
    ("porta_vol", FLAG_VOLHEX, 6, [N(13, 1, 0x0C, 0x30), N(25, 1, 0x03, 0x06), N(0, 0, 0x05, 0x02), N(0, 0, 0x05, 0x02)]),
    ("vib_vol", FLAG_VOLHEX, 6, [N(13, 1, 0x0C, 0x30), N(0, 0, 0x04, 0x36), N(0, 0, 0x06, 0x02), N(0, 0, 0x06, 0x02)]),
    ("hold_decay", 0, 6, [N(13, 1, 0x08, 0x23), N(), N(), N()]),
    ("hold_decay_instant", 0, 6, [N(13, 1, 0x08, 0x02), N(), N()]),
    ("fine_slide", 0, 6, [N(13, 1, 0x11, 0x03), N(0, 0, 0x12, 0x05), N(0, 0, 0x11, 0x03), N()]),
    ("finetune", 0, 6, [N(13, 1, 0x15, 0x03), N(13, 1, 0x15, 0xFD), N(13, 1, 0x15, 0x00), N(13, 1)]),
    ("fine_vol", FLAG_VOLHEX, 6, [N(13, 1, 0x0C, 0x30), N(0, 0, 0x1A, 0x04), N(0, 0, 0x1B, 0x09), N()]),
    ("cut", 0, 6, [N(13, 1, 0x18, 0x02), N(13, 1, 0x18, 0x00), N(13, 1, 0x18, 0x05), N()]),
    ("retrig_ff1", 0, 6, [N(13, 1, 0x0F, 0xF1), N(), N(13, 1, 0x0F, 0xF1)]),
    ("retrig_ff3", 0, 6, [N(13, 1, 0x0F, 0xF3), N(), N(13, 1, 0x0F, 0xF3)]),
    ("retrig_1f", 0, 6, [N(13, 1, 0x1F, 0x02), N(), N(13, 1, 0x1F, 0x12), N(13, 1, 0x1F, 0x30), N()]),
    ("delay_ff2", 0, 6, [N(13, 1), N(25, 1, 0x0F, 0xF2), N(0, 0, 0x01, 0x04), N()]),
    ("two_notes", FLAG_VOLHEX, 6, [N(13, 1, 0x0C, 0x20), N(20, 1), N(0, 0, 0x01, 0x03), N(25, 1, 0x0C, 0x40)]),
]


def build_module(flags, speed, rows):
    m = mb.Module(1)
    m.deftempo, m.tempo2, m.flags = 33, speed, flags
    b = mb.Block(4, max(len(rows), 4))
    for i, (note, inst, cmd, data) in enumerate(rows):
        if note or inst or cmd or data:
            b.cell(i, 0, note, inst, cmd, data)
    m.blocks = [b]
    m.playseq = [0]
    ins = mb.Instrument(0, bytes((i * 3) & 0xFF for i in range(16384)), "")
    ins.svol = 64
    m.instruments = [ins]
    return mb.build(m)


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    dump, out_path = argv[1], argv[2]
    out = ["# med_tick_golden.txt: libxmp frames of one-track MED scenarios (tools/med_tick_golden.py; libxmp is not in the repository). Do not edit by hand.",
           "# scenario NAME stslide S speed K rows R | cell ROW NOTE INST CMD DATA | frame F PERIOD_X4096 VOLUME RESTART | end"]
    with tempfile.TemporaryDirectory() as tmp:
        for name, flags, speed, rows in SCENARIOS:
            path = os.path.join(tmp, name + ".mmd")
            with open(path, "wb") as f:
                f.write(build_module(flags, speed, rows))
            nframes = len(rows) * speed
            text = subprocess.run([dump, path, str(nframes), "1"], check=True, capture_output=True, text=True).stdout
            out.append("scenario %s stslide %d speed %d rows %d" % (name, 1 if flags & FLAG_STSLIDE else 0, speed, len(rows)))
            for i, (note, inst, cmd, data) in enumerate(rows):
                if note or inst or cmd or data:
                    out.append("cell %d %d %d %d %d" % (i, note, inst, cmd, data))
            prev = None
            for k, line in enumerate(text.strip().split("\n")):
                v = line.split()
                period, vol, pos = int(v[4]), int(v[5]), int(v[8])
                restart = 1 if (prev is None or pos < prev) else 0
                out.append("frame %d %d %d %d" % (k, period, vol, restart))
                prev = pos
            out.append("end")
    with open(out_path, "w") as f:
        f.write("\n".join(out) + "\n")
    print("wrote", out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
