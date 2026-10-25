#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic ProTracker MOD and Scream Tracker 3 S3M module writer (stdlib only; run it with `python3 -I`).

The layouts are the ones of docs/formats/mod.md (the public ProTracker and Scream Tracker 3 file descriptions); the C++ reader is formats/mod.
This is a second, independent implementation of the same bytes, written from the layout and not from the reader: ctest `mod_python` writes a set
of modules here together with the text dump it expects of each, and compares that with what `o3mod_tool dump` reads out of the files. ctest
`modimp_openmpt` builds on the same writer (tests/modimp_openmpt_oracle.py). Nothing here is a real module: every note, name and sample is made
up, so the files can be generated freely and never need to be committed (OS-MODIMP: no copyrighted modules in the tree).

  mod_build.py --out DIR            write the reference modules (NAME.mod / NAME.s3m) and their expected dumps (NAME.txt) into DIR
  mod_build.py --check TOOL DIR     write them into DIR and compare TOOL's `dump` of each with the expected text
  mod_build.py --self-test          build every module and check the structure of the bytes (sizes, tables)

As a library: Mod / S3m describe a song, build(module) -> bytes, dump(module, size) -> str.
"""
import os
import struct
import subprocess
import sys

PERIODS = [856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453,
           428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226,
           214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113]


def sine(n, amp=100):
    import math
    return bytes(int(round(amp * math.sin(2 * math.pi * i / n))) & 0xFF for i in range(n))


def sine16(n, amp=20000):
    import math
    out = bytearray()
    for i in range(n):
        out += struct.pack("<h", int(round(amp * math.sin(2 * math.pi * i / n))))
    return bytes(out)


def _name(s, width):
    b = s.encode("ascii")[:width]
    return b + b"\0" * (width - len(b))


# ---- MOD -----------------------------------------------------------------------------------------------------------------


class ModSample:
    def __init__(self, data=b"", name="", volume=64, finetune=0, loop_start=0, loop_len=0):
        self.data = bytes(data)   # signed 8-bit; an odd length is padded to whole words in the file
        self.name = name
        self.volume = volume
        self.finetune = finetune  # -8..7
        self.loop_start = loop_start  # words
        self.loop_len = loop_len      # words (<= 1: no loop)


class Mod:
    def __init__(self, tag="M.K.", channels=4, name="synthetic"):
        self.tag = tag
        self.channels = channels
        self.name = name
        self.restart = 127
        self.orders = [0]          # the song: pattern numbers (FLT8: the stored, even numbers)
        self.junk_orders = {}      # position -> pattern number behind the song length (the file's 128-entry table)
        self.patterns = [{}]       # each: (row, channel) -> (note index 0..35 or None, sample, effect, param), or a raw period via ("p", period)
        self.samples = [ModSample() for _ in range(31)]
        self.truncate_by = 0       # cut this many bytes off the end of the file


def mod_cell_bytes(cell):
    note, sample, effect, param = cell
    period = 0
    if note is not None:
        period = PERIODS[note] if not isinstance(note, tuple) else note[1]
    b0 = (sample & 0xF0) | (period >> 8)
    b1 = period & 0xFF
    b2 = ((sample & 0x0F) << 4) | (effect & 0x0F)
    return bytes((b0, b1, b2, param & 0xFF))


def _flt8(m):
    return m.tag == "FLT8"


def _pattern_count(m):
    songlen = len(m.orders)
    table = list(m.orders) + [0] * (128 - songlen)
    for pos, p in m.junk_orders.items():
        table[pos] = p
    top = max(table)
    return table, (top >> 1) + 1 if _flt8(m) else top + 1


def build_mod(m):
    table, count = _pattern_count(m)
    out = bytearray()
    out += _name(m.name, 20)
    for s in m.samples:
        data = s.data + (b"\0" if len(s.data) % 2 else b"")
        out += _name(s.name, 22)
        out += struct.pack(">H", len(data) // 2)
        out.append(s.finetune & 0x0F)
        out.append(s.volume)
        out += struct.pack(">HH", s.loop_start, s.loop_len)
    out.append(len(m.orders))
    out.append(m.restart)
    out += bytes(table)
    out += m.tag.encode("ascii")
    ch = m.channels
    for p in range(count):
        pat = m.patterns[p] if p < len(m.patterns) else {}
        if _flt8(m):
            # an 8-channel pattern is two blocks of 4 channels: channels 0-3 first, 4-7 second
            for half in range(2):
                for row in range(64):
                    for c in range(4):
                        cell = pat.get((row, c + 4 * half), (None, 0, 0, 0))
                        out += mod_cell_bytes(cell)
        else:
            for row in range(64):
                for c in range(ch):
                    out += mod_cell_bytes(pat.get((row, c), (None, 0, 0, 0)))
    for s in m.samples:
        out += s.data + (b"\0" if len(s.data) % 2 else b"")
    if m.truncate_by:
        out = out[:len(out) - m.truncate_by]
    return bytes(out)


def _clean(name):
    s = "".join(ch if 0x20 <= ord(ch) <= 0x7E else "?" for ch in name).rstrip(" ")
    return s


def dump_mod(m, size):
    table, count = _pattern_count(m)
    lines = []
    lines.append("mod tag=%s channels=%d orders=%d patterns=%d restart=%d size=%d" % (m.tag, m.channels, len(m.orders), count, m.restart, size))
    lines.append("name %s" % _clean(m.name))
    for i, p in enumerate(m.orders):
        lines.append("order %d %d" % (i, p >> 1 if _flt8(m) else p))
    # sample data begins after the patterns; each sample is its length in whole words, trimmed to the file
    base = 1084 + count * 64 * m.channels * 4
    off = base
    for i, s in enumerate(m.samples):
        n = len(s.data) + (len(s.data) % 2)
        frames = n
        notes = 0
        avail = max(0, size - off)
        if frames > avail:
            frames = avail
            notes |= 1
        loop = (0, 0)
        if frames > 0 and s.loop_len > 1:
            start = 2 * s.loop_start
            end = start + 2 * s.loop_len
            if end > frames:
                end = frames
                notes |= 2
            if start < end and end - start >= 2:
                loop = (start, end - start)
            else:
                notes |= 2
        if frames == 0:
            loop = (0, 0)
        ft = s.finetune & 0x0F
        ft = ft - 16 if ft >= 8 else ft
        lines.append("sample %d frames=%d vol=%d fine=%d loop=%d+%d notes=%d name=%s"
                     % (i, frames, min(s.volume, 64), ft, loop[0], loop[1], notes, _clean(s.name)))
        off += n
    for p in range(count if not _flt8(m) else (count)):
        pat = m.patterns[p] if p < len(m.patterns) else {}
        for row in range(64):
            for c in range(m.channels):
                note, sample, effect, param = pat.get((row, c), (None, 0, 0, 0))
                if note is None and sample == 0 and effect == 0 and param == 0:
                    continue
                period = 0 if note is None else (PERIODS[note] if not isinstance(note, tuple) else note[1])
                if period:
                    best = min(range(36), key=lambda i: (abs(PERIODS[i] - period), i))
                    n = best + 1
                    off_table = 0 if PERIODS[best] == period else 1
                else:
                    n, off_table = 0, 0
                lines.append("cell %d %d %d period=%d note=%d inst=%d fx=%d param=%d off=%d" % (p, row, c, period, n, sample, effect, param, off_table))
    return "\n".join(lines) + "\n"


# ---- S3M -----------------------------------------------------------------------------------------------------------------


class S3mInst:
    def __init__(self, data=b"", name="", c2spd=8363, volume=64, loop=None, sixteen=False, stereo=False, itype=1, packed=False):
        self.data = bytes(data)      # as stored in the file (8-bit unsigned or signed per the module's format, or 16-bit little-endian)
        self.name = name
        self.c2spd = c2spd
        self.volume = volume
        self.loop = loop             # (start, end) in frames or None
        self.sixteen = sixteen
        self.stereo = stereo
        self.itype = itype           # 1 PCM, 0 empty, 2.. AdLib
        self.packed = packed


class S3m:
    def __init__(self, name="synthetic"):
        self.name = name
        self.orders = [0]
        self.instruments = []
        self.patterns = [{}]     # each: (row, channel) -> (note byte or 255, instrument, volume or 255, command, info)
        self.channels = [0, 8, 1, 9] + [255] * 28   # channel settings
        self.speed = 6
        self.tempo = 125
        self.global_volume = 64
        self.master_volume = 0xB0       # bit 7: stereo
        self.signed = False             # ffi: 1 signed, 2 unsigned
        self.flags = 0
        self.pan_table = None           # 32 bytes (bit 5: valid, low nibble: pan) or None
        self.empty_patterns = set()     # pattern numbers whose parapointer is 0
        self.cwtv = 0x1320


def _pack_s3m_pattern(pat):
    out = bytearray()
    for row in range(64):
        for ch in range(32):
            cell = pat.get((row, ch))
            if cell is None:
                continue
            note, inst, vol, cmd, info = cell
            b = ch
            body = bytearray()
            if note != 255 or inst != 0:
                b |= 0x20
                body += bytes((note, inst))
            if vol != 255:
                b |= 0x40
                body.append(vol)
            if cmd != 0 or info != 0:
                b |= 0x80
                body += bytes((cmd, info))
            if b == ch:
                continue
            out.append(b)
            out += body
        out.append(0)
    return bytes(out)


def build_s3m(m):
    ord_num = len(m.orders)
    ins_num = len(m.instruments)
    pat_num = len(m.patterns)
    header_len = 96 + ord_num + 2 * ins_num + 2 * pat_num + (32 if m.pan_table is not None else 0)
    # parapointers are in 16-byte units: lay the instruments, then the patterns, then the sample data out on 16-byte boundaries
    pos = (header_len + 15) // 16 * 16
    ins_pos = []
    for _ in m.instruments:
        ins_pos.append(pos // 16)
        pos += 80
    pat_pos = []
    packed = []
    for i, p in enumerate(m.patterns):
        if i in m.empty_patterns:
            pat_pos.append(0)
            packed.append(b"")
            continue
        body = _pack_s3m_pattern(p)
        pat_pos.append(pos // 16)
        packed.append(struct.pack("<H", len(body) + 2) + body)
        pos += (len(packed[-1]) + 15) // 16 * 16
    data_pos = []
    for ins in m.instruments:
        data_pos.append(pos // 16)
        pos += (len(ins.data) * (2 if ins.stereo else 1) + 15) // 16 * 16
    out = bytearray(pos)
    out[0:28] = _name(m.name, 28)
    out[28] = 0x1A
    out[29] = 16
    struct.pack_into("<HHHHHH", out, 32, ord_num, ins_num, pat_num, m.flags, m.cwtv, 2 if not m.signed else 1)
    out[44:48] = b"SCRM"
    out[48] = m.global_volume
    out[49] = m.speed
    out[50] = m.tempo
    out[51] = m.master_volume
    out[52] = 0
    out[53] = 252 if m.pan_table is not None else 0
    out[64:96] = bytes(m.channels)
    at = 96
    out[at:at + ord_num] = bytes(m.orders)
    at += ord_num
    for p in ins_pos:
        struct.pack_into("<H", out, at, p)
        at += 2
    for p in pat_pos:
        struct.pack_into("<H", out, at, p)
        at += 2
    if m.pan_table is not None:
        out[at:at + 32] = bytes(m.pan_table)
    for ins, ip, dp in zip(m.instruments, ins_pos, data_pos):
        b = bytearray(80)
        b[0] = ins.itype
        b[1:13] = b"SYNTH.PCM\0\0\0"
        if ins.itype == 1:
            b[13] = (dp >> 16) & 0xFF
            struct.pack_into("<H", b, 14, dp & 0xFFFF)
            frames = len(ins.data) // (2 if ins.sixteen else 1)
            struct.pack_into("<I", b, 16, frames)
            if ins.loop is not None:
                struct.pack_into("<II", b, 20, ins.loop[0], ins.loop[1])
            b[28] = ins.volume
            b[30] = 1 if ins.packed else 0
            b[31] = (1 if ins.loop is not None else 0) | (2 if ins.stereo else 0) | (4 if ins.sixteen else 0)
            struct.pack_into("<I", b, 32, ins.c2spd)
            b[76:80] = b"SCRS"
        else:
            b[76:80] = b"SCRI"
        b[48:76] = _name(ins.name, 28)
        out[ip * 16:ip * 16 + 80] = b
    for p, pp in zip(packed, pat_pos):
        if pp:
            out[pp * 16:pp * 16 + len(p)] = p
    for ins, dp in zip(m.instruments, data_pos):
        if ins.itype == 1:
            out[dp * 16:dp * 16 + len(ins.data)] = ins.data
    return bytes(out)


def dump_s3m(m, size):
    ord_num = len(m.orders)
    pat_num = len(m.patterns)
    top = 0
    for c in range(32):
        if m.channels[c] != 255:
            top = c + 1
    played = []
    skipped = 0
    bad = 0
    for o in m.orders:
        if o == 255:
            break
        if o == 254:
            skipped += 1
            continue
        if o >= pat_num:
            bad += 1
            continue
        played.append(o)
    stereo = (m.master_volume & 0x80) != 0
    tempo = m.tempo if m.tempo >= 32 else 32
    speed = m.speed if m.speed != 0 else 6
    fast = 1 if ((m.flags & 0x40) or m.cwtv == 0x1300) else 0
    lines = ["s3m size=%d channels=%d orders=%d patterns=%d instruments=%d speed=%d tempo=%d gv=%d mv=%d stereo=%d signed=%d fast=%d skipped=%d bad=%d"
             % (size, top, len(played), pat_num, len(m.instruments), speed, tempo, m.global_volume, m.master_volume, 1 if stereo else 0,
                1 if m.signed else 0, fast, skipped, bad)]
    lines.append("name %s" % _clean(m.name))
    for c in range(top):
        s = m.channels[c]
        if s == 255:
            continue
        typ = s & 0x7F
        pct = 50
        if stereo and typ < 16:
            pct = ((3 if typ < 8 else 12) * 100 + 7) // 15
        if m.pan_table is not None and (m.pan_table[c] & 0x20):
            pct = ((m.pan_table[c] & 0x0F) * 100 + 7) // 15
        lines.append("chan %d pan=%d muted=%d adlib=%d" % (c, pct, 1 if s & 0x80 else 0, 1 if typ >= 16 else 0))
    for i, o in enumerate(played):
        lines.append("order %d %d" % (i, o))
    for i, ins in enumerate(m.instruments):
        if ins.itype == 0:
            lines.append("inst %d kind=empty frames=0 bits=8 vol=0 c2spd=0 loop=0+0 notes=0 name=%s" % (i, _clean(ins.name)))
            continue
        if ins.itype >= 2:
            lines.append("inst %d kind=adlib frames=0 bits=8 vol=0 c2spd=0 loop=0+0 notes=0 name=%s" % (i, _clean(ins.name)))
            continue
        if ins.packed:
            lines.append("inst %d kind=packed frames=0 bits=8 vol=%d c2spd=%d loop=0+0 notes=0 name=%s" % (i, min(ins.volume, 64), ins.c2spd, _clean(ins.name)))
            continue
        frames = len(ins.data) // (2 if ins.sixteen else 1)
        notes = 4 if ins.stereo else 0
        loop = (0, 0)
        if ins.loop is not None:
            start, end = ins.loop
            if end > frames:
                end = frames
                notes |= 2
            if start < end and end - start >= 2:
                loop = (start, end - start)
            else:
                notes |= 2
        kind = "pcm" if frames else "empty"
        lines.append("inst %d kind=%s frames=%d bits=%d vol=%d c2spd=%d loop=%d+%d notes=%d name=%s"
                     % (i, kind, frames, 16 if ins.sixteen else 8, min(ins.volume, 64), ins.c2spd, loop[0], loop[1], notes, _clean(ins.name)))
    for p, pat in enumerate(m.patterns):
        if p in m.empty_patterns:
            continue
        for row in range(64):
            for c in range(top):
                cell = pat.get((row, c))
                if cell is None:
                    continue
                note, inst, vol, cmd, info = cell
                if note == 255 and inst == 0 and vol == 255 and cmd == 0 and info == 0:
                    continue
                cut = 0
                n = 0
                bad_note = 0
                if note == 254:
                    cut = 1
                elif note != 255:
                    n = (note >> 4) * 12 + (note & 15) + 1
                    bad_note = 1 if (note & 15) > 11 else 0
                lines.append("cell %d %d %d note=%d cut=%d inst=%d vol=%d fx=%d param=%d bad=%d" % (p, row, c, n, cut, inst, vol, cmd, info, bad_note))
    return "\n".join(lines) + "\n"


# ---- the reference modules -------------------------------------------------------------------------------------------------


def _samples(n_used=3):
    s = [ModSample() for _ in range(31)]
    s[0] = ModSample(sine(32), "sine 32", 64, 0, 0, 16)
    s[1] = ModSample(sine(64, 90), "sine 64 ft+3", 48, 3, 8, 24)
    s[2] = ModSample(sine(16), "one shot", 32, -2)
    if n_used > 3:
        s[30] = ModSample(sine(8), "last", 10, 7, 0, 1)
    return s


def mod_mk():
    m = Mod("M.K.", 4, "reference m.k.")
    m.samples = _samples(4)
    m.orders = [0, 1, 0]
    p0 = {}
    for row in range(0, 64, 4):
        p0[(row, 0)] = (row // 4 % 36, 1, 0, 0)
    p0[(0, 1)] = (12, 2, 0x0C, 0x20)
    p0[(2, 1)] = (None, 0, 0x0E, 0xA3)
    p0[(4, 2)] = (24, 3, 0x0F, 0x20)
    p0[(8, 3)] = (35, 31, 0x01, 0xFF)
    p0[(63, 3)] = (None, 0, 0x0D, 0x00)
    m.patterns = [p0, {(0, 0): (0, 1, 0x0A, 0x0F), (1, 2): (11, 2, 0x04, 0x35)}]
    return m


def mod_mk2():
    m = Mod("M!K!", 4, "m!k! with junk")
    m.samples = _samples()
    m.orders = [0, 1]
    m.patterns = [{(0, 0): (12, 1, 0, 0)}, {(1, 1): (13, 2, 0x03, 0x10)}]
    m.junk_orders = {5: 1}
    return m


def mod_6chn():
    m = Mod("6CHN", 6, "six channels")
    m.samples = _samples()
    m.orders = [0]
    m.patterns = [{(r, c): ((r + c) % 36, 1 + c % 3, 0, 0) for r in range(0, 64, 8) for c in range(6)}]
    return m


def mod_8chn():
    m = Mod("8CHN", 8, "eight channels")
    m.samples = _samples()
    m.orders = [0, 0]
    m.patterns = [{(r, c): ((r // 2 + c) % 36, 1 + c % 3, 0x0C, 0x30) for r in range(0, 16, 4) for c in range(8)}]
    return m


def mod_flt4():
    m = Mod("FLT4", 4, "flt4")
    m.samples = _samples()
    m.orders = [0]
    m.patterns = [{(0, 0): (12, 1, 0, 0), (3, 3): (20, 2, 0x0F, 0x06)}]
    return m


def mod_flt8():
    m = Mod("FLT8", 8, "flt8 pairs")
    m.samples = _samples()
    m.orders = [0, 2]   # stored pattern numbers are even: pairs of 4-channel blocks
    m.patterns = [{(0, c): (12 + c, 1, 0, 0) for c in range(8)}, {(5, 4): (20, 2, 0x0F, 0x07), (5, 1): (21, 3, 0, 0)}]
    # block pairs: pattern 0 = blocks 0 and 1, pattern 1 = blocks 2 and 3 (order value 2)
    return m


def mod_16ch():
    m = Mod("16CH", 16, "sixteen channels")
    m.samples = _samples()
    m.orders = [0]
    m.patterns = [{(r, c): ((r + c) % 36, 1 + c % 3, 0, 0) for r in range(0, 8) for c in range(16)}]
    return m


def mod_32ch():
    m = Mod("32CH", 32, "thirty-two channels")
    m.samples = _samples()
    m.orders = [0]
    m.patterns = [{(r, c): ((r + c) % 36, 1 + c % 3, 0, 0) for r in range(0, 4) for c in range(32)}]
    return m


def mod_truncated():
    m = Mod("M.K.", 4, "samples cut short")
    m.samples = _samples()
    m.samples[1].data = sine(64, 90)
    m.orders = [0]
    m.patterns = [{(0, 0): (12, 1, 0, 0)}]
    m.truncate_by = 40
    return m


def mod_off_table():
    m = Mod("M.K.", 4, "off-table periods")
    m.samples = _samples()
    m.orders = [0]
    m.patterns = [{(0, 0): (("p", 430), 1, 0, 0), (1, 0): (("p", 100), 1, 0, 0), (2, 0): (("p", 900), 1, 0, 0), (3, 0): (("p", 0x0FFF), 1, 0, 0)}]
    return m


def s3m_basic():
    m = S3m("reference s3m")
    m.instruments = [
        S3mInst(bytes((128 + v) & 0xFF for v in sine(32)), "unsigned 8", 8363, 64, (0, 32)),
        S3mInst(bytes((128 + v) & 0xFF for v in sine(64)), "22 kHz", 22050, 40),
        S3mInst(sine16(32), "16 bit", 11025, 30, (8, 24), sixteen=True),
    ]
    m.orders = [0, 1, 255]
    m.patterns = [
        {(0, 0): (0x40, 1, 64, 0, 0), (4, 0): (0x43, 2, 255, 4, 0x20), (8, 1): (254, 0, 255, 0, 0), (0, 2): (255, 3, 20, 19, 0x34),
         (16, 3): (0x5B, 1, 255, 3, 0x14), (17, 3): (0x5C, 1, 255, 20, 0x90)},
        {(1, 0): (0x30, 1, 0, 1, 0x05), (63, 0): (0x61, 3, 63, 2, 0x31)},
    ]
    return m


def s3m_signed():
    m = S3m("signed 16 bit")
    m.signed = True
    m.instruments = [S3mInst(sine(32), "signed", 8363, 64, None), S3mInst(sine16(32), "signed 16", 8363, 64, None, sixteen=True)]
    m.orders = [0]
    m.patterns = [{(0, 0): (0x40, 1, 255, 0, 0), (0, 1): (0x40, 2, 255, 0, 0)}]
    return m


def s3m_special():
    m = S3m("markers adlib muted")
    m.instruments = [S3mInst(bytes(range(0, 64)), "pcm", 0, 70, (10, 9999)), S3mInst(b"", "adlib", itype=2), S3mInst(b"", "empty", itype=0),
                     S3mInst(bytes(64), "packed", 8363, 64, packed=True), S3mInst(bytes(64), "stereo", 8363, 64, stereo=True)]
    m.orders = [1, 254, 0, 9, 255, 0]
    m.patterns = [{(0, 0): (0x40, 1, 255, 0, 0), (0, 1): (0x40, 2, 255, 0, 0)}, {(2, 0): (0x4F, 5, 255, 1, 6)}]
    m.channels = [0, 8, 0x80 | 3, 17, 255, 5] + [255] * 26
    m.speed = 0
    m.tempo = 20
    m.master_volume = 0x30  # mono: every channel in the centre
    return m


def s3m_pans():
    m = S3m("pan table")
    m.instruments = [S3mInst(bytes(range(64)), "one")]
    m.orders = [0]
    m.patterns = [{(0, 0): (0x40, 1, 255, 0, 0)}, {}]
    m.channels = [0, 8, 1, 9, 2, 10, 3, 11] + [255] * 24
    m.pan_table = [0x20 | 0, 0x20 | 15, 0x20 | 7, 0x00] + [0] * 28
    m.empty_patterns = {1}
    m.master_volume = 0x80 | 0x30
    m.global_volume = 40
    return m


def s3m_wide():
    m = S3m("twenty-four channels")
    m.instruments = [S3mInst(bytes(range(64)), "one", 16726)]
    m.orders = [0]
    m.channels = list(range(8)) + list(range(8, 16)) + list(range(0, 8)) + [255] * 8
    m.patterns = [{(r, c): (0x40 + (c % 12), 1, 255, 0, 0) for r in range(0, 8) for c in range(24)}]
    m.flags = 0x40
    return m


MODULES = [
    ("mod_mk", ".mod", mod_mk, "mod"), ("mod_mk2", ".mod", mod_mk2, "mod"), ("mod_6chn", ".mod", mod_6chn, "mod"),
    ("mod_8chn", ".mod", mod_8chn, "mod"), ("mod_flt4", ".mod", mod_flt4, "mod"), ("mod_flt8", ".mod", mod_flt8, "mod"),
    ("mod_16ch", ".mod", mod_16ch, "mod"), ("mod_32ch", ".mod", mod_32ch, "mod"), ("mod_truncated", ".mod", mod_truncated, "mod"),
    ("mod_off_table", ".mod", mod_off_table, "mod"),
    ("s3m_basic", ".s3m", s3m_basic, "s3m"), ("s3m_signed", ".s3m", s3m_signed, "s3m"), ("s3m_special", ".s3m", s3m_special, "s3m"),
    ("s3m_pans", ".s3m", s3m_pans, "s3m"), ("s3m_wide", ".s3m", s3m_wide, "s3m"),
]


def build(m):
    return build_mod(m) if isinstance(m, Mod) else build_s3m(m)


def dump(m, size):
    return dump_mod(m, size) if isinstance(m, Mod) else dump_s3m(m, size)


def write_all(directory):
    os.makedirs(directory, exist_ok=True)
    out = []
    for name, ext, make, _ in MODULES:
        m = make()
        data = build(m)
        with open(os.path.join(directory, name + ext), "wb") as f:
            f.write(data)
        with open(os.path.join(directory, name + ".txt"), "w") as f:
            f.write(dump(m, len(data)))
        out.append((name, ext))
    return out


def check(tool, directory):
    failures = 0
    for name, ext in write_all(directory):
        want = open(os.path.join(directory, name + ".txt")).read()
        r = subprocess.run([tool, "dump", os.path.join(directory, name + ext)], capture_output=True, text=True, timeout=60)
        if r.returncode != 0:
            print("FAIL %s: the tool refused it: %s" % (name, r.stderr.strip()))
            failures += 1
        elif r.stdout != want:
            failures += 1
            a, b = want.splitlines(), r.stdout.splitlines()
            for i in range(max(len(a), len(b))):
                x = a[i] if i < len(a) else "<none>"
                y = b[i] if i < len(b) else "<none>"
                if x != y:
                    print("FAIL %s: line %d\n  expected: %s\n  tool:     %s" % (name, i + 1, x, y))
                    break
        else:
            print("ok   %s (%d lines)" % (name, want.count("\n")))
    print("%d modules, %d failures" % (len(MODULES), failures))
    return failures


def self_test():
    bad = 0
    for name, ext, make, kind in MODULES:
        m = make()
        data = build(m)
        if kind == "mod":
            table, count = _pattern_count(m)
            want = 1084 + count * 64 * m.channels * 4 + sum(len(s.data) + len(s.data) % 2 for s in m.samples) - m.truncate_by
            if len(data) != want:
                print("FAIL %s: size %d, expected %d" % (name, len(data), want))
                bad += 1
            if data[1080:1084] != m.tag.encode():
                print("FAIL %s: tag" % name)
                bad += 1
        else:
            if data[44:48] != b"SCRM" or data[28] != 0x1A or data[29] != 16:
                print("FAIL %s: header" % name)
                bad += 1
    print("self-test: %d failures" % bad)
    return bad


def main(argv):
    if len(argv) == 2 and argv[1] in ("-h", "--help"):
        sys.stdout.write(__doc__)
        return 0
    if len(argv) == 3 and argv[1] == "--out":
        write_all(argv[2])
        return 0
    if len(argv) == 4 and argv[1] == "--check":
        return 1 if check(argv[2], argv[3]) else 0
    if len(argv) == 2 and argv[1] == "--self-test":
        return 1 if self_test() else 0
    sys.stderr.write(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
