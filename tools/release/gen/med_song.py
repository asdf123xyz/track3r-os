# SPDX-License-Identifier: GPL-3.0-or-later
"""The starter OctaMED song: a four-track MMD1 module (tools/mmd_build.py writes the bytes), CC0, made from the synthesised drum kit.

A short groove in two blocks (kick, snare, hat, a sawtooth bass), 8-bit samples taken from the kit, so the MED importer has one
module of the format that is ours. Nothing in it is taken from an existing song.
"""
import os
import sys

import drumkit
from wavio import to_pcm16

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))   # tools/: mmd_build.py
import mmd_build  # noqa: E402


def _eight_bit(samples, step=3, length=None):
    """Signed 8-bit data: the floats averaged `step` at a time (a box filter before the decimation), then scaled to the peak."""
    n = (len(samples) if length is None else min(length, len(samples))) // step
    avg = [sum(samples[i * step:(i + 1) * step]) / step for i in range(n)]
    return bytes(v & 0xFF for v in (q >> 8 for q in to_pcm16(avg, 0.92, 24)))


def _bass_cycle():
    return bytes(((i * 8) - 128) & 0xFF for i in range(32))      # one rising sawtooth of 32 frames


def module():
    kit = drumkit.kit()
    m = mmd_build.Module(1)
    m.deftempo, m.tempo2 = 33, 6
    m.songname = "Starter Groove"
    kick = mmd_build.Instrument(0, _eight_bit(kit["kick"], 3, 7000), "kick")
    snare = mmd_build.Instrument(0, _eight_bit(kit["snare"], 3), "snare")
    hat = mmd_build.Instrument(0, _eight_bit(kit["hat_closed"], 3), "hat")
    bass = mmd_build.Instrument(0, _bass_cycle(), "bass")
    bass.rep, bass.replen, bass.svol = 0, 16, 40               # the whole cycle loops (units of two frames)
    m.instruments = [kick, snare, hat, bass]

    def groove(name, bass_line):
        b = mmd_build.Block(4, 16, name)
        for line in (0, 4, 8, 12):
            b.cell(line, 0, 25, 1)
        for line in (4, 12):
            b.cell(line, 1, 25, 2)
        for line in range(0, 16, 2):
            b.cell(line, 2, 25, 3, 0x0C, 0x30 if line & 2 else 0x40)    # command C: volume
        for line, note in bass_line:
            b.cell(line, 3, note, 4)
        return b

    a = groove("verse", [(0, 13), (3, 13), (6, 16), (8, 11), (11, 11), (14, 18)])
    b = groove("lift", [(0, 18), (2, 18), (4, 21), (8, 16), (10, 16), (12, 13)])
    b.cell(15, 0, 25, 1).cell(14, 1, 25, 2)                              # a fill on the last two lines
    m.blocks = [a, b]
    m.playseq = [0, 0, 1, 0, 1, 1]
    return m


def files():
    """{'starter_groove.med': bytes}"""
    return {"starter_groove.med": mmd_build.build(module())}
