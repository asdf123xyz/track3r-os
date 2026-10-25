#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic OctaMED / MED module writer, MMD0 to MMD3 (stdlib only; run it with `python3 -I`).

The format is specified in docs/formats/med.md section 2 (Kinnunen, "MED/OctaMED MMD0/MMD1/MMD2/MMD3 file formats",
Rev 6); the C++ reader is formats/med. This is a second, independent implementation of the same bytes, written from
the layout document and not from the reader: ctest `med_python` writes a set of modules here together with the text
dump it expects of each, and compares that with what `o3med_tool dump` reads out of the files. Nothing here is a real
module: every note, name and sample is made up, so the files can be generated freely and never need to be committed.

  mmd_build.py --out DIR            write the reference modules (NAME.mmd) and their expected dumps (NAME.txt) into DIR
  mmd_build.py --check TOOL DIR     write them into DIR and compare TOOL's `dump` of each with the expected text
  mmd_build.py --self-test          build every module and check the structure of the bytes (pointers, alignment, sizes)
  mmd_build.py --import-check TOOL DIR
                                    write them into DIR and check that `TOOL import` (the OS-008b importer) takes each one: the song
                                    name, and per instrument the result, the frames of PCM and the PCM itself (a CRC-32 of it),
                                    all worked out here from the description (notes, volumes and commands are checked against
                                    libopenmpt by tests/med_openmpt_oracle.py and by the C++ goldens)

As a library: Module / Block / Instrument describe a song, build(module) -> bytes, dump(module, size) -> str. A cell is
(note, instrument, command, data); MMD0 cells are 3 bytes (note 0..63, command 0..15), MMD1 and later 4 bytes.
"""
import os
import struct
import subprocess
import sys
import zlib

# ---- the description ---------------------------------------------------------------------------------------------------


class Block:
    def __init__(self, tracks=4, lines=8, name="", empty=False):
        self.tracks = tracks
        self.lines = lines
        self.name = name
        self.empty = empty
        self.cells = {}   # (line, track) -> (note, instrument, command, data)
        self.pages = []   # extra command pages: dicts (line, track) -> (command, data)
        self.ext = None   # command-extension bytes: tracks * lines * (1 + pages), or None

    def cell(self, line, track, note=0, inst=0, cmd=0, data=0):
        self.cells[(line, track)] = (note, inst, cmd, data)
        return self

    def page(self, index, line, track, cmd, data):
        while len(self.pages) <= index:
            self.pages.append({})
        self.pages[index][(line, track)] = (cmd, data)
        return self


class Instrument:
    def __init__(self, type=0, data=b"", name="", present=True):
        self.present = present
        self.type = type          # InstrHdr.type as stored: -2 hybrid, -1 synth, 0..7 sample types, | 0x10 16-bit, | 0x20 stereo
        self.data = bytes(data)   # sample data (stereo: left channel then right)
        self.name = name
        self.rep = 0              # song record: units of two frames
        self.replen = 0
        self.midich = 0
        self.midipreset = 0
        self.svol = 64
        self.strans = 0
        self.ext = b""            # InstrExt entry (padded or cut to the module's stride)
        self.waves = []           # synth: waveform bytes; hybrid: waves[0] is the sample


class PlaySeq:
    def __init__(self, entries, name="", cmds=()):
        self.entries = list(entries)
        self.name = name
        self.cmds = list(cmds)    # (position, command 1 stop | 2 jump, extra bytes)


class Module:
    def __init__(self, version=1):
        self.version = version
        self.deftempo = 125
        self.playtransp = 0
        self.flags = 0
        self.flags2 = 0
        self.tempo2 = 6
        self.mastervol = 64
        self.trkvol = [64] * 16
        self.blocks = []
        self.instruments = []
        self.playseq = []         # MMD0/1: block numbers
        self.pseqs = []           # MMD2+
        self.sections = []
        self.trackvols = []
        self.trackpans = []
        self.flags3 = 0
        self.voladj = 0
        self.channels = 0
        self.echotype = 0
        self.echodepth = 0
        self.echolen = 0
        self.stereosep = 0
        self.s_ext_entrsz = 0     # InstrExt stride, 0 = no array
        self.i_ext_entrsz = 0     # instrument name stride (>= 40 for names), 0 = no array
        self.songname = ""
        self.annotation = ""
        self.extra_songs = 0
        self.exp = False

    def num_tracks(self):
        if self.version <= 1:
            return 0
        if self.trackvols:
            return len(self.trackvols)
        return max([b.tracks for b in self.blocks] or [0])


# ---- the writer --------------------------------------------------------------------------------------------------------


def _check_cell(version, note, inst, cmd, data):
    if version == 0:
        assert 0 <= note < 64 and 0 <= cmd < 16, "MMD0 cell out of range"
    else:
        assert 0 <= note < 256 and 0 <= inst < 64 and 0 <= cmd < 256, "MMD1 cell out of range"
    assert 0 <= inst < 64 and 0 <= data < 256


def build(m):
    out = bytearray(52)
    out[0:4] = b"MMD" + bytes([0x30 + m.version])
    ver = m.version

    def align():
        if len(out) & 1:
            out.append(0)

    def patch32(at, v):
        out[at:at + 4] = struct.pack(">I", v)

    def patch16(at, v):
        out[at:at + 2] = struct.pack(">H", v)

    song = len(out)
    out.extend(bytes(788))

    playseqtable = sectiontable = trackvols = trackpans = 0
    pseq_offsets = []
    if ver >= 2:
        for ps in m.pseqs:
            align()
            at = len(out)
            pseq_offsets.append(at)
            out.extend(ps.name.encode("ascii").ljust(32, b"\0")[:32])
            out.extend(bytes(8))
            out.extend(struct.pack(">H", len(ps.entries)))
            for e in ps.entries:
                out.extend(struct.pack(">H", e))
            if ps.cmds:
                align()
                patch32(at + 32, len(out))
                for pos, cmd, extra in ps.cmds:
                    out.extend(struct.pack(">HBB", pos, cmd, len(extra)))
                    out.extend(extra)
                out.extend(struct.pack(">HBB", 0xFFFF, 0, 0))
        align()
        playseqtable = len(out)
        for o in pseq_offsets:
            out.extend(struct.pack(">I", o))
        sectiontable = len(out)
        for s in m.sections:
            out.extend(struct.pack(">H", s))
        align()
        if m.trackvols:
            trackvols = len(out)
            out.extend(bytes(m.trackvols))
        align()
        if m.trackpans:
            trackpans = len(out)
            out.extend(bytes(v & 0xFF for v in m.trackpans))
        align()

    blockarr = len(out)
    out.extend(bytes(4 * len(m.blocks)))
    smplarr = len(out)
    out.extend(bytes(4 * len(m.instruments)))

    for i, b in enumerate(m.blocks):
        if b.empty:
            continue
        align()
        at = len(out)
        if ver == 0:
            out.extend(struct.pack(">BB", b.tracks, b.lines - 1))
        else:
            out.extend(struct.pack(">HHI", b.tracks, b.lines - 1, 0))
        for line in range(b.lines):
            for track in range(b.tracks):
                note, inst, cmd, data = b.cells.get((line, track), (0, 0, 0, 0))
                _check_cell(ver, note, inst, cmd, data)
                if ver == 0:
                    out.extend(bytes([note | (0x80 if inst & 0x10 else 0) | (0x40 if inst & 0x20 else 0),
                                      ((inst & 0x0F) << 4) | cmd, data]))
                else:
                    out.extend(bytes([note, inst, cmd, data]))
        if ver >= 1 and (b.name or b.pages or b.ext):
            align()
            info = len(out)
            patch32(at + 4, info)
            out.extend(bytes(36))
            if b.name:
                name = len(out)
                out.extend(b.name.encode("ascii") + b"\0")
                patch32(info + 4, name)
                patch32(info + 8, len(b.name) + 1)
            if b.pages:
                align()
                pt = len(out)
                patch32(info + 12, pt)
                out.extend(struct.pack(">HH", len(b.pages), 0))
                out.extend(bytes(4 * len(b.pages)))
                for p, page in enumerate(b.pages):
                    align()
                    patch32(pt + 4 + 4 * p, len(out))
                    for line in range(b.lines):
                        for track in range(b.tracks):
                            out.extend(bytes(page.get((line, track), (0, 0))))
            if b.ext:
                assert len(b.ext) == b.tracks * b.lines * (1 + len(b.pages)), "extension table size"
                align()
                ptr = len(out)
                out.extend(bytes(4))
                patch32(info + 16, ptr)
                patch32(ptr, len(out))
                out.extend(b.ext)
        patch32(blockarr + 4 * i, at)

    for i, ins in enumerate(m.instruments):
        if not ins.present:
            continue
        align()
        at = len(out)
        patch32(smplarr + 4 * i, at)
        if ins.waves and ins.type in (-1, -2):
            # SynthInstr: length, type, defaultdecay, reserved[3], rep, replen, voltbllen, wftbllen, volspeed, wfspeed,
            # wforms, voltbl[128], wftbl[128], wf[64]; waveform pointers are relative to the instrument
            out.extend(struct.pack(">IhB3xHHHHBBH", 534, ins.type, 0, 0, 0, 4, 4, 1, 1, len(ins.waves)))
            out.extend(bytes(256))
            wf = len(out)
            out.extend(bytes(256))
            for k, w in enumerate(ins.waves):
                align()
                patch32(wf + 4 * k, len(out) - at)
                if ins.type == -2 and k == 0:
                    out.extend(struct.pack(">Ih", len(w), 0))
                else:
                    out.extend(struct.pack(">H", len(w) // 2))
                out.extend(w)
        else:
            channels = 2 if ins.type >= 0 and (ins.type & 0x20) else 1
            out.extend(struct.pack(">Ih", len(ins.data) // channels, ins.type))
            out.extend(ins.data)

    exp = 0
    want_exp = m.exp or m.s_ext_entrsz or m.i_ext_entrsz or m.songname or m.annotation or m.extra_songs
    if want_exp:
        align()
        exp = len(out)
        out.extend(bytes(80))
        n = len(m.instruments)
        if m.s_ext_entrsz:
            align()
            at = len(out)
            for ins in m.instruments:
                out.extend(ins.ext[:m.s_ext_entrsz].ljust(m.s_ext_entrsz, b"\0"))
            patch32(exp + 4, at)
            patch16(exp + 8, n)
            patch16(exp + 10, m.s_ext_entrsz)
        if m.i_ext_entrsz:
            align()
            at = len(out)
            for ins in m.instruments:
                out.extend(ins.name.encode("ascii").ljust(m.i_ext_entrsz, b"\0")[:m.i_ext_entrsz])
            patch32(exp + 20, at)
            patch16(exp + 24, n)
            patch16(exp + 26, m.i_ext_entrsz)
        if m.songname:
            at = len(out)
            out.extend(m.songname.encode("ascii") + b"\0")
            patch32(exp + 44, at)
            patch32(exp + 48, len(m.songname) + 1)
        if m.annotation:
            at = len(out)
            out.extend(m.annotation.encode("ascii") + b"\0")
            patch32(exp + 12, at)
            patch32(exp + 16, len(m.annotation) + 1)
        patch32(32, exp)
        prev = exp
        for _ in range(m.extra_songs):
            align()
            h = len(out)
            out.extend(b"MCN1" + bytes(48))
            patch32(prev, h)
            align()
            xe = len(out)
            out.extend(bytes(80))
            patch32(h + 32, xe)
            prev = xe

    # the song record
    for i, ins in enumerate(m.instruments[:63]):
        out[song + 8 * i:song + 8 * i + 8] = struct.pack(">HHBBBb", ins.rep, ins.replen, ins.midich, ins.midipreset, ins.svol,
                                                        ins.strans)
    patch16(song + 504, len(m.blocks))
    if ver <= 1:
        patch16(song + 506, len(m.playseq))
        out[song + 508:song + 508 + len(m.playseq)] = bytes(m.playseq)
        out[song + 770:song + 786] = bytes(m.trkvol)
    else:
        patch16(song + 506, len(m.sections))
        patch32(song + 508, playseqtable)
        patch32(song + 512, sectiontable)
        patch32(song + 516, trackvols)
        patch16(song + 520, m.num_tracks())
        patch16(song + 522, len(m.pseqs))
        patch32(song + 524, trackpans)
        patch32(song + 528, m.flags3)
        patch16(song + 532, m.voladj)
        patch16(song + 534, m.channels)
        out[song + 536] = m.echotype
        out[song + 537] = m.echodepth
        patch16(song + 538, m.echolen)
        out[song + 540] = m.stereosep & 0xFF
    patch16(song + 764, m.deftempo)
    out[song + 766] = m.playtransp & 0xFF
    out[song + 767] = m.flags
    out[song + 768] = m.flags2
    out[song + 769] = m.tempo2
    out[song + 786] = m.mastervol
    out[song + 787] = len(m.instruments)

    patch32(4, len(out))
    patch32(8, song)
    patch32(16, blockarr if m.blocks else 0)
    patch32(24, smplarr if m.instruments else 0)
    out[51] = m.extra_songs
    patch16(48, 0xFFFF)
    return bytes(out)


# ---- the expected dump -------------------------------------------------------------------------------------------------

OCTAVES = {1: 5, 2: 3, 3: 2, 4: 4, 5: 6, 6: 7}


def _i8(v):
    return v - 256 if v >= 128 else v


def _play_list(m):
    """[(kind, number)] of the flattened play list, as `o3med_tool dump` prints it."""
    nblocks = len(m.blocks)
    out = []
    if m.version <= 1:
        for b in m.playseq:
            out.append(("block", b) if b < nblocks else ("bad", b))
        return out
    for s in m.sections:
        if s >= len(m.pseqs):
            continue
        ps = m.pseqs[s]
        for idx, e in enumerate(ps.entries):
            if e >= 0x8000:
                kind, num = "skip", 0
            else:
                kind, num = ("block", e) if e < nblocks else ("bad", e)
            for pos, cmd, extra in ps.cmds:   # the last matching command wins
                if pos != idx:
                    continue
                if cmd == 1:
                    kind = "stop"
                elif cmd == 2 and len(extra) >= 2:
                    kind, num = "jump", struct.unpack(">H", bytes(extra[:2]))[0]
            out.append((kind, num))
    return out


def dump(m, size):
    ver = m.version
    plist = _play_list(m)
    lines = ["mmd v=%d size=%d" % (ver, size)]
    songlen = len(m.playseq) if ver <= 1 else len(m.sections)
    lines.append("song blocks=%d len=%d samples=%d tempo=%d spd=%d flags=%d flags2=%d transpose=%d master=%d pseqs=%d tracks=%d entries=%d"
                 % (len(m.blocks), songlen, len(m.instruments), m.deftempo, m.tempo2, m.flags, m.flags2, m.playtransp,
                    m.mastervol, len(m.pseqs) if ver >= 2 else 0, m.num_tracks(), len(plist)))
    lines.append("name %s" % m.songname[:32])
    for i, (kind, num) in enumerate(plist):
        lines.append("play %d %s %d" % (i, kind, num))
    for i, b in enumerate(m.blocks):
        if b.empty:
            lines.append("block %d empty" % i)
            continue
        pages = len(b.pages) if ver >= 1 else 0
        ext = b.ext if (ver >= 1 and b.ext) else None
        name = b.name if ver >= 1 else ""
        lines.append("block %d tracks=%d lines=%d pages=%d ext=%d name=%s" % (i, b.tracks, b.lines, pages, 1 if ext else 0, name[:30]))
        for page in range(pages + 1):
            for line in range(b.lines):
                for track in range(b.tracks):
                    if page == 0:
                        note, inst, cmd, data = b.cells.get((line, track), (0, 0, 0, 0))
                    else:
                        cmd, data = b.pages[page - 1].get((line, track), (0, 0))
                        note = inst = 0
                    x = ext[(page * b.lines + line) * b.tracks + track] if ext else 0
                    hi = 1 if (ver >= 1 and note & 0x80) else 0   # MMD1+: bit 7 of the note byte is passed on, not masked into the note
                    note &= 0x7F if ver >= 1 else 0xFF
                    if note | inst | cmd | data | x | hi:
                        lines.append("cell %d %d %d %d n=%d i=%d c=%d d=%d x=%d%s" % (i, page, line, track, note, inst, cmd, data, x,
                                                                                    " h=1" if hi else ""))
    for slot, ins in enumerate(m.instruments):
        if not ins.present:
            lines.append("instr %d empty" % slot)
            continue
        t = ins.type
        synth = t in (-1, -2) and bool(ins.waves)
        if synth:
            kind = "synth" if t == -1 else "hybrid"
            bits, stereo, nbytes, octaves = 8, 0, (len(ins.waves[0]) if t == -2 else 0), 1
        elif t < 0 or (t != 0x18 and (t & 0xC0 or (t & 0x0F) > 7)):
            kind, bits, stereo, nbytes, octaves = "unsupported", 8, 0, 0, 1
        else:
            kind = "sample"
            bits = 16 if (t == 0x18 or t & 0x10) else 8
            stereo = 1 if (t != 0x18 and t & 0x20) else 0
            nbytes = len(ins.data) // (2 if stereo else 1)
            octaves = OCTAVES.get(t & 0x0F, 1) if t != 0x18 else 1
        stride = m.s_ext_entrsz
        ent = ins.ext[:stride].ljust(stride, b"\0")[:18] if stride else b""
        n = len(ent)
        hold = ent[0] if n >= 1 else 0
        decay = ent[1] if n >= 2 else 0
        fine = _i8(ent[3]) if n >= 4 else 0
        pitch = ent[4] if n >= 5 else 0
        eflags = ent[5] if n >= 6 else 0
        name = ins.name[:32] if m.i_ext_entrsz >= 40 else ""
        lines.append("instr %d kind=%s type=%d bits=%d stereo=%d bytes=%d octaves=%d vol=%d trans=%d midich=%d midipre=%d name=%s "
                     "hold=%d decay=%d fine=%d pitch=%d eflags=%d waves=%d"
                     % (slot, kind, t, bits, stereo, nbytes, octaves, ins.svol, ins.strans, ins.midich, ins.midipreset, name,
                        hold, decay, fine, pitch, eflags, len(ins.waves) if synth else 0))
        if synth:
            for w, wave in enumerate(ins.waves):
                lines.append("wave %d %d %d" % (slot, w, len(wave)))
    ntracks = 16 if ver <= 1 else m.num_tracks()
    for t in range(ntracks):
        if ver <= 1:
            v = m.trkvol[t]
        else:
            v = m.trackvols[t] if m.trackvols else 64
        lines.append("trackvol %d %d" % (t, min(v, 64)))
    if ver >= 2:
        for t in range(ntracks):
            p = m.trackpans[t] if m.trackpans else 0
            lines.append("trackpan %d %d" % (t, max(-16, min(16, p))))
    return "\n".join(lines) + "\n"


# ---- the reference modules ---------------------------------------------------------------------------------------------


def ramp(n, step=3):
    return bytes((i * step) & 0xFF for i in range(n))


def mmd0_basic():
    m = Module(0)
    m.deftempo, m.tempo2 = 33, 6
    a = Block(4, 16)
    a.cell(0, 0, 25, 1, 0x0C, 0x40).cell(0, 1, 30, 2).cell(3, 3, 63, 63, 0x0F, 0x00).cell(15, 2, 1, 33, 0x09, 0x03)
    a.cell(8, 0, 0, 17, 0x04, 0x35).cell(9, 1, 0, 48, 0x0B, 0x00)
    b = Block(4, 8)
    b.cell(1, 0, 13, 5, 0x1 & 0x0F, 0x10).cell(7, 3, 40, 0, 0x0F, 0xF3)
    m.blocks = [a, b]
    m.playseq = [0, 1, 0, 1]
    s1 = Instrument(0, ramp(64), "")
    s1.rep, s1.replen, s1.svol, s1.strans = 8, 16, 48, -3
    s2 = Instrument(2, ramp(35, 7))
    s2.svol = 20
    m.instruments = [s1, s2]
    m.trkvol = [64, 50, 32, 16] + [64] * 12
    return m


def mmd1_full():
    m = Module(1)
    m.deftempo, m.tempo2, m.flags, m.flags2 = 120, 4, 0x10, 0x20 | 7
    m.exp = True
    m.s_ext_entrsz, m.i_ext_entrsz = 18, 40
    m.songname, m.annotation = "Reference Song", "made up for the tests"
    a = Block(6, 12, "intro")
    a.cell(0, 0, 100, 1, 0x1F, 0x23).cell(0, 5, 127, 63, 0x0F, 0xFF).cell(11, 2, 1, 2, 0x0C, 0x7F)
    a.cell(5, 3, 0x80).cell(6, 3, 0x81, 2)    # a note byte of exactly 0x80 (a cut to OpenMPT), and note 1 with the reserved bit
    a.page(0, 1, 0, 0x1D, 0x02).page(0, 3, 4, 0x16, 0x00).page(1, 3, 4, 0x1E, 0x02)
    a.ext = bytes((i * 5) & 0x7F if i % 11 == 0 else 0 for i in range(6 * 12 * 3))
    b = Block(4, 5, "second")
    b.cell(2, 1, 60, 7, 0x09, 0x03).cell(4, 3, 0, 0, 0x2E, 0xF0)
    c = Block(2, 3)
    c.cell(1, 1, 33, 3, 0xFF, 0xFF)
    e = Block(2, 2, empty=True)
    m.blocks = [a, b, c, e]
    m.playseq = [0, 1, 2, 0, 9, 3]
    s1 = Instrument(0, ramp(64), "square")
    s1.rep, s1.replen, s1.svol, s1.strans, s1.midich, s1.midipreset = 8, 16, 48, -3, 5, 17
    s1.ext = bytes([2, 3, 0, 0xFF, 25, 1, 0, 0, 0, 0, 0, 0, 0, 16, 0, 0, 0, 32])
    s2 = Instrument(0x30, ramp(64, 5), "stereo16")
    s3 = Instrument(2, ramp(35), "iff3")
    s3.rep, s3.replen = 1, 2
    s4 = Instrument(-1, b"", "synth")
    s4.waves = [bytes(8), bytes([1] * 16)]
    s5 = Instrument(-2, b"", "hybrid")
    s5.waves = [ramp(12), bytes(8)]
    s6 = Instrument(0, b"", "gap", present=False)
    s7 = Instrument(-3, ramp(8), "highlife")
    s8 = Instrument(0x18, ramp(40), "aura")
    s9 = Instrument(7, ramp(20), "ext")
    m.instruments = [s1, s2, s3, s4, s5, s6, s7, s8, s9]
    return m


def mmd2_sections():
    m = Module(2)
    m.deftempo, m.tempo2, m.flags = 33, 6, 0x10
    m.exp = True
    m.s_ext_entrsz, m.i_ext_entrsz = 10, 44
    m.songname = "Sections"
    blocks = []
    for i, (tracks, lines) in enumerate([(4, 8), (4, 4), (8, 6), (4, 2)]):
        b = Block(tracks, lines, "blk%d" % i)
        b.cell(0, 0, 25 + i, i + 1, 0x0C, 0x40)
        if i == 2:
            b.cell(1, 7, 40, 9, 0x0B, 0x01)
            b.page(0, 2, 6, 0x1D, 0x01)
        blocks.append(b)
    m.blocks = blocks
    m.pseqs = [PlaySeq([0, 1], "main"), PlaySeq([2, 0x8000, 0, 3], "coda", [(1, 2, bytes([0, 0])), (3, 1, b"")])]
    m.sections = [0, 1]
    m.trackvols = [64, 50, 32, 16, 64, 64, 8, 0]
    m.trackpans = [-16, 0, 0, 16, 5, -5, 100, -100]
    m.flags3, m.voladj, m.channels = 3, 150, 8
    m.echotype, m.echodepth, m.echolen, m.stereosep = 1, 2, 300, 2
    s1 = Instrument(0x10, ramp(40), "sixteen")
    s1.ext = bytes([1, 2, 0, 3, 0, 0, 0, 0, 0, 0])
    s2 = Instrument(0, ramp(10), "tiny")
    m.instruments = [s1, s2]
    return m


def mmd3_mix():
    m = Module(3)
    m.deftempo, m.tempo2, m.flags, m.flags2 = 6, 3, 0x40, 0x80
    m.exp = True
    m.s_ext_entrsz, m.i_ext_entrsz = 21, 40
    m.songname = "Mixing"
    a = Block(12, 4)
    a.cell(0, 11, 20, 1, 0x20, 0x00).cell(2, 0, 30, 2, 0x29, 0x08)
    a.ext = bytes((i % 7) for i in range(12 * 4))
    m.blocks = [a]
    m.pseqs = [PlaySeq([0, 0], "")]
    m.sections = [0, 0]
    s1 = Instrument(0x30, ramp(100), "stereo")
    s1.ext = bytes([0, 0, 0, 1, 25, 3, 0, 0, 0, 0, 0, 0, 0, 5, 0, 0, 0, 20, 100, 1, 2])
    m.instruments = [s1]
    return m


def mmd1_chain():
    m = Module(1)
    m.extra_songs = 2
    m.songname = "Chained"
    a = Block(4, 4)
    a.cell(0, 0, 10, 1)
    m.blocks = [a]
    m.playseq = [0]
    return m


def mmd1_strides():
    m = Module(1)
    m.s_ext_entrsz, m.i_ext_entrsz = 4, 8     # strides too short for the flags and for names
    m.exp = True
    a = Block(4, 2)
    a.cell(0, 0, 10, 1, 0x0C, 0x20)
    m.blocks = [a]
    m.playseq = [0]
    s1 = Instrument(0, ramp(16), "never read")
    s1.ext = bytes([9, 8, 7, 0xF8, 6, 5])
    m.instruments = [s1]
    return m


MODULES = [
    ("mmd0_basic", mmd0_basic),
    ("mmd1_full", mmd1_full),
    ("mmd2_sections", mmd2_sections),
    ("mmd3_mix", mmd3_mix),
    ("mmd1_chain", mmd1_chain),
    ("mmd1_strides", mmd1_strides),
]


def write_all(directory):
    os.makedirs(directory, exist_ok=True)
    out = []
    for name, make in MODULES:
        m = make()
        data = build(m)
        path = os.path.join(directory, name + ".mmd")
        with open(path, "wb") as f:
            f.write(data)
        expected = dump(m, len(data))
        with open(os.path.join(directory, name + ".txt"), "w") as f:
            f.write(expected)
        out.append((name, path, expected))
    return out


def check(tool, directory):
    failures = 0
    for name, path, expected in write_all(directory):
        done = subprocess.run([tool, "dump", path], capture_output=True)
        got = done.stdout.decode("utf-8", "replace")
        if done.returncode != 0:
            print("FAILED %s: %s exited %d: %s" % (name, tool, done.returncode, done.stderr.decode("utf-8", "replace")))
            failures += 1
            continue
        if got != expected:
            a, b = expected.splitlines(), got.splitlines()
            at = next((i for i in range(min(len(a), len(b))) if a[i] != b[i]), min(len(a), len(b)))
            print("FAILED %s: dumps differ at line %d\n  python: %s\n  c++:    %s" %
                  (name, at + 1, a[at] if at < len(a) else "<end>", b[at] if at < len(b) else "<end>"))
            failures += 1
        else:
            print("ok %s (%d bytes, %d lines)" % (name, os.path.getsize(path), len(expected.splitlines())))
    return failures


def expected_import_instrument(ins, num_octaves=OCTAVES):
    """(result, frames) the importer should report for an instrument, from its description alone."""
    if not ins.present:
        return "none", 0
    if ins.midich != 0:
        return "midi", 0
    t = ins.type
    if t in (-1, -2):
        if not ins.waves:
            return "no-wave", 0
        return "sample", len(ins.waves[0])        # the first waveform (hybrid: the sample of its first waveform)
    if t < 0 or ((t & 0xC0) != 0) or ((t & 0x0F) > 7 and t != 0x18):
        return "unsupported", 0
    sixteen = t == 0x18 or (t & 0x10) != 0
    stereo = t != 0x18 and (t & 0x20) != 0
    total = len(ins.data) // ((2 if sixteen else 1) * (2 if stereo else 1))
    n = num_octaves.get(t & 0x0F, 1) if t != 0x18 else 1
    if total == 0:
        return "empty", 0
    base = total // ((1 << n) - 1)
    return "sample", base << (n - 1)


def _trunc_half(v):
    return v // 2 if v >= 0 else -((-v) // 2)


def expected_import_pcm(ins, num_octaves=OCTAVES):
    """The 16-bit mono little-endian PCM the importer should make of an instrument, from its description alone: the longest octave
    of a sample, the first waveform of a synth instrument or hybrid; 8-bit data times 256, 16-bit data as it is, stereo mixed to
    (L + R) / 2 truncated toward zero."""
    t = ins.type
    if t in (-1, -2):
        data = ins.waves[0]
        return b"".join(struct.pack("<h", _i8(b) * 256) for b in data)
    sixteen = t == 0x18 or (t & 0x10) != 0
    stereo = t != 0x18 and (t & 0x20) != 0
    width = 2 if sixteen else 1
    total = len(ins.data) // ((2 if stereo else 1) * width)       # frames of one channel, every octave
    n = num_octaves.get(t & 0x0F, 1) if t != 0x18 else 1
    base = total // ((1 << n) - 1)
    skip, frames = base * ((1 << (n - 1)) - 1), base << (n - 1)

    def channel(first_byte):
        out = []
        for i in range(skip, skip + frames):
            at = first_byte + i * width
            if sixteen:
                out.append(struct.unpack(">h", ins.data[at:at + 2])[0])
            else:
                out.append(_i8(ins.data[at]) * 256)
        return out

    left = channel(0)
    if stereo:
        right = channel(total * width)
        left = [_trunc_half(l + r) for l, r in zip(left, right)]
    return b"".join(struct.pack("<h", v) for v in left)


def import_check(tool, directory):
    """Every reference module imports, with the song name and what each instrument needs as the description says."""
    failures = 0
    for name, path, _ in write_all(directory):
        m = dict(MODULES)[name]()
        done = subprocess.run([tool, "import", path], capture_output=True)
        text = done.stdout.decode("utf-8", "replace")
        if done.returncode != 0 or not text.startswith("import ok version=%d " % m.version):
            print("FAILED %s: `%s import` exited %d: %s%s" % (name, tool, done.returncode, text[:200], done.stderr.decode("utf-8", "replace")))
            failures += 1
            continue
        problems = []
        project = [ln for ln in text.splitlines() if ln.startswith("project name=")]
        if not project or not project[0].startswith("project name=%s tempo_x100=" % m.songname[:32]):
            problems.append("song name: %s" % (project[0] if project else "no project line"))
        got = {}
        for ln in text.splitlines():
            f = ln.split()
            if f and f[0] == "instr":
                got[int(f[1])] = dict(x.split("=", 1) for x in f[2:] if "=" in x)
        for slot, ins in enumerate(m.instruments[:63]):
            want, frames = expected_import_instrument(ins)
            have = got.get(slot)
            if want == "none":
                if have is not None:
                    problems.append("instrument %d should make nothing, made %s" % (slot, have.get("result")))
                continue
            if have is None or have.get("result") != want:
                problems.append("instrument %d: expected %s, got %s" % (slot, want, have.get("result") if have else "nothing"))
            elif want == "sample" and int(have["frames"]) != frames:
                problems.append("instrument %d: expected %d frames of PCM, got %s" % (slot, frames, have["frames"]))
            elif want == "sample":
                crc = zlib.crc32(expected_import_pcm(ins)) & 0xFFFFFFFF
                if have.get("pcm_crc") != "%08x" % crc:
                    problems.append("instrument %d: the PCM differs from the description (CRC-32 %s, expected %08x)"
                                    % (slot, have.get("pcm_crc"), crc))
        if problems:
            print("FAILED %s: %s" % (name, "; ".join(problems)))
            failures += 1
        else:
            print("ok %s (imported, %d instruments checked)" % (name, len(m.instruments)))
    return failures


def self_test():
    """Structure of the bytes, read back here with a deliberately plain walk (no shared code with the dump)."""
    problems = 0
    for name, make in MODULES:
        m = make()
        data = build(m)

        def fail(msg):
            nonlocal problems
            problems += 1
            print("FAILED %s: %s" % (name, msg))

        if data[:3] != b"MMD" or data[3] != 0x30 + m.version:
            fail("id")
        size = struct.unpack(">I", data[4:8])[0]
        if size != len(data):
            fail("modlen %d != %d" % (size, len(data)))
        for field in (8, 16, 24, 32):
            p = struct.unpack(">I", data[field:field + 4])[0]
            if p != 0 and (p < 52 or p >= len(data)):
                fail("header pointer at %d: %d" % (field, p))
            if p & 1:
                fail("header pointer at %d is odd" % field)
        song = struct.unpack(">I", data[8:12])[0]
        if song != 52:
            fail("song record at %d" % song)
        if struct.unpack(">H", data[song + 504:song + 506])[0] != len(m.blocks):
            fail("numblocks")
        if data[song + 787] != len(m.instruments):
            fail("numsamples")
        blockarr = struct.unpack(">I", data[16:20])[0]
        for i, b in enumerate(m.blocks):
            p = struct.unpack(">I", data[blockarr + 4 * i:blockarr + 4 * i + 4])[0]
            if b.empty != (p == 0):
                fail("block %d pointer" % i)
            if p:
                hdr = 2 if m.version == 0 else 8
                tracks, lines = struct.unpack(">BB", data[p:p + 2]) if m.version == 0 else struct.unpack(">HH", data[p:p + 4])
                if tracks != b.tracks or lines + 1 != b.lines:
                    fail("block %d geometry" % i)
                if p + hdr + tracks * (lines + 1) * (3 if m.version == 0 else 4) > len(data):
                    fail("block %d cells run past the end" % i)
    print("self-test: %d modules, %d problems" % (len(MODULES), problems))
    return problems


def main(argv):
    if len(argv) == 2 and argv[1] in ("-h", "--help"):
        sys.stdout.write(__doc__)
        return 0
    if len(argv) == 3 and argv[1] == "--out":
        write_all(argv[2])
        return 0
    if len(argv) == 4 and argv[1] == "--check":
        failures = check(argv[2], argv[3])
        return 1 if failures else 0
    if len(argv) == 4 and argv[1] == "--import-check":
        failures = import_check(argv[2], argv[3])
        return 1 if failures else 0
    if len(argv) == 2 and argv[1] == "--self-test":
        return 1 if self_test() else 0
    sys.stderr.write(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
