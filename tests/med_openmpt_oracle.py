#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A second opinion on the OctaMED importer (OS-008b, docs/formats/med.md section 3), from libopenmpt.

  med_openmpt_oracle.py TOOL WORKDIR

TOOL is `o3med_tool`; `TOOL import FILE [pal|ntsc]` prints the project the importer makes of FILE. This script writes synthetic
modules (tools/mmd_build.py: nothing here is a real module and nothing is committed), imports each with TOOL, loads the same
bytes into libopenmpt and compares what the two make of them:

  notes      our note byte is OpenMPT's note number (MED note + 48, 24 less in MMD3), our instrument number its instrument
  volume     0C: decimal-coded, or 7 bit under VOLHEX, the same 0..64 once converted back
  pitch      the frequency libopenmpt renders for a note equals the one the importer's rate, note, tune and fine tune give, for
             every instrument type (plain, 16-bit, stereo, 1..6 octaves, type 7), strans, playtransp and InstrExt finetune
  loops      a sample sustains in libopenmpt exactly when the importer says it loops (the InstrExt loop flag rule of O11)
  tempo      the length of a song, from OpenMPT's duration, against 15 / BPM per line from the importer's PHDR tempo
  flow       the rows OpenMPT visits against the rows of the importer's pieces in the importer's order (breaks, jumps, loops)
  dropped    a tempo or speed command on a track that gets no slot (more than 16 tracks) still moves time: the length of such a song
  volume     0C persists until the next instrument number: the loudness of each row of OpenMPT's render against the importer's steps

libopenmpt is not vendored: it is loaded with ctypes from the system (the shared library alone is enough, no headers). Without it
the script exits 77, which ctest reports as skipped, except under CI: with O3_CI=1 in the environment (tools/ci.sh sets it, the
workflow installs the library) a missing or too old library, or fewer than MIN_COMPARISONS comparisons, is a failure, so that the
test cannot go quiet where it matters. Where the two disagree on purpose the
reason is in the comments below (OpenMPT folds a note the Amiga cannot play by octaves, the importer does not; libopenmpt times
speed mode as 0.66 / tempo seconds, the CIA constants are 1.3 % slower).
"""
import ctypes
import ctypes.util
import math
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import mmd_build as mb  # noqa: E402

SKIP = 77
MIN_VERSION = (0, 7)        # the behaviour below was measured on libopenmpt 0.7.3; Ubuntu 24.04 ships it
MIN_COMPARISONS = 2400      # a run that compares less than this has lost a check (the full run makes about 2,600)
STRICT = os.environ.get("O3_CI") == "1"


# ---- libopenmpt through ctypes -----------------------------------------------------------------------------------------


def load_library():
    names = [ctypes.util.find_library("openmpt"), "libopenmpt.so.0", "libopenmpt.dylib"]
    for n in names:
        if not n:
            continue
        try:
            return ctypes.CDLL(n)
        except OSError:
            continue
    return None


class Omp:
    def __init__(self, lib):
        self.lib = lib
        c = ctypes
        lib.openmpt_module_create_from_memory2.restype = c.c_void_p
        lib.openmpt_module_create_from_memory2.argtypes = [c.c_void_p, c.c_size_t, c.c_void_p, c.c_void_p, c.c_void_p, c.c_void_p,
                                                          c.POINTER(c.c_int), c.POINTER(c.c_char_p), c.c_void_p]
        lib.openmpt_module_read_float_mono.restype = c.c_size_t
        lib.openmpt_module_read_float_mono.argtypes = [c.c_void_p, c.c_int32, c.c_size_t, c.POINTER(c.c_float)]
        lib.openmpt_module_destroy.argtypes = [c.c_void_p]
        lib.openmpt_module_set_repeat_count.argtypes = [c.c_void_p, c.c_int32]
        lib.openmpt_module_get_pattern_row_channel_command.argtypes = [c.c_void_p, c.c_int32, c.c_int32, c.c_int32, c.c_int]
        lib.openmpt_module_get_pattern_row_channel_command.restype = c.c_uint8
        lib.openmpt_module_get_duration_seconds.argtypes = [c.c_void_p]
        lib.openmpt_module_get_duration_seconds.restype = c.c_double
        for f in ("current_order", "current_pattern", "current_row"):
            fn = getattr(lib, "openmpt_module_get_" + f)
            fn.argtypes = [c.c_void_p]
            fn.restype = c.c_int32

    def version(self):
        """(major, minor, patch) of the library, from openmpt_get_library_version (major << 24 | minor << 16 | patch)."""
        fn = self.lib.openmpt_get_library_version
        fn.argtypes = []
        fn.restype = ctypes.c_uint32
        v = fn()
        return (v >> 24) & 0xFF, (v >> 16) & 0xFF, v & 0xFFFF

    def load(self, data):
        buf = ctypes.create_string_buffer(bytes(data), len(data))
        err = ctypes.c_int(0)
        msg = ctypes.c_char_p()
        mod = self.lib.openmpt_module_create_from_memory2(ctypes.cast(buf, ctypes.c_void_p), len(data), None, None, None, None,
                                                         ctypes.byref(err), ctypes.byref(msg), None)
        if not mod:
            raise RuntimeError("libopenmpt refused the module (%d)" % err.value)
        self.lib.openmpt_module_set_repeat_count(mod, 0)
        return mod, buf

    def free(self, mod):
        self.lib.openmpt_module_destroy(mod)

    def render(self, mod, rate, frames):
        out = (ctypes.c_float * frames)()
        n = self.lib.openmpt_module_read_float_mono(mod, rate, frames, out)
        return list(out[:n])

    def cell(self, mod, pattern, row, channel):
        """(note, instrument, volume effect, effect, volume, parameter)"""
        return tuple(self.lib.openmpt_module_get_pattern_row_channel_command(mod, pattern, row, channel, k) for k in range(6))

    def duration(self, mod):
        return self.lib.openmpt_module_get_duration_seconds(mod)

    def trace(self, mod, want, rate=8000, step=40, limit_seconds=120):
        """The (pattern, row) pairs the player visits, consecutive repeats collapsed, until `want` are known or the song ends."""
        seen = []
        frames = 0
        while len(seen) < want and frames < limit_seconds * rate:
            got = self.render(mod, rate, step)
            if not got:
                break
            frames += len(got)
            pos = (self.lib.openmpt_module_get_current_pattern(mod), self.lib.openmpt_module_get_current_row(mod))
            if not seen or seen[-1] != pos:
                seen.append(pos)
        return seen


# ---- the tool ----------------------------------------------------------------------------------------------------------


class Imported:
    def __init__(self, text):
        self.ok = text.startswith("import ok")
        self.text = text
        self.steps = {}       # (pattern, track, line) -> dict
        self.instr = {}       # slot -> dict
        self.order = []
        self.pieces = {}      # id -> (block, first, last, replay)
        self.project = {}
        self.report = {}
        for line in text.splitlines():
            m = re.match(r"project name=(.*) tempo_x100=(\d+) active=(\d+) patterns=(\d+) order=(\d+)$", line)
            if m:
                self.project = {"name": m.group(1), "tempo": int(m.group(2)), "active": int(m.group(3)), "patterns": int(m.group(4)),
                                "order": int(m.group(5))}
                continue
            f = line.split()
            if not f:
                continue
            if f[0] == "step":
                d = dict(x.split("=", 1) for x in f[4:])
                self.steps[(int(f[1]), int(f[2]), int(f[3]))] = {
                    "note": int(d["n"]), "inst": int(d["i"]), "vol": int(d["v"]),
                    "fx": [tuple(int(y) for y in d["f1"].split(":")), tuple(int(y) for y in d["f2"].split(":"))]}
            elif f[0] == "instr":
                d = dict(x.split("=", 1) for x in f[2:] if "=" in x)
                d["result"] = d.get("result")
                self.instr[int(f[1])] = d
            elif f[0] == "order":
                self.order.append(int(f[2]))
            elif f[0] == "piece":
                d = dict(x.split("=", 1) for x in f[2:])
                self.pieces[int(f[1])] = (int(d["block"]), int(d["first"]), int(d["last"]), int(d["replay"]))
            elif f[0] == "report":
                for x in f[1:]:
                    if "=" in x:
                        k, v = x.split("=", 1)
                        self.report[k] = v


def run_tool(tool, path, opts=()):
    r = subprocess.run([tool, "import", path] + list(opts), capture_output=True, text=True, timeout=120)
    return Imported(r.stdout if r.returncode == 0 else "import failed: " + r.stdout + r.stderr)


# ---- the harness -------------------------------------------------------------------------------------------------------


class Checks:
    def __init__(self):
        self.count = 0
        self.failures = []

    def check(self, ok, what):
        self.count += 1
        if not ok:
            self.failures.append(what)
            if len(self.failures) <= 40:
                print("FAIL: " + what)


def sine(n):
    return bytes(int(round(100 * math.sin(2 * math.pi * i / n))) & 0xFF for i in range(n))


def sine16(n):
    out = bytearray()
    for i in range(n):
        out += int(round(20000 * math.sin(2 * math.pi * i / n))).to_bytes(2, "big", signed=True)
    return bytes(out)


def one_note_module(version, note, itype=0, strans=0, playtransp=0, ext=None, ext_stride=0, replen=None, cycle=64,
                    flags=0, flags2=0, deftempo=125, spd=6, lines=64, data=None):
    m = mb.Module(version)
    m.deftempo = deftempo
    m.tempo2 = spd
    m.flags = flags
    m.flags2 = flags2
    m.playtransp = playtransp
    b = mb.Block(4, lines)
    b.cell(0, 0, note, 1, 0, 0)
    m.blocks = [b]
    m.playseq = [0]
    ins = mb.Instrument(type=itype, data=data if data is not None else sine(cycle))
    ins.strans = strans
    low = itype & 7 if itype >= 0 else 0
    ins.replen = replen if replen is not None else ((len(ins.data) // ((2 if itype & 0x10 else 1) * (2 if itype & 0x20 else 1)) // 2)
                                                    if low == 0 or low == 7 else 0)
    if ext is not None:
        ins.ext = bytes(ext)
        m.s_ext_entrsz = ext_stride
        m.exp = True
    m.instruments = [ins]
    if version >= 2:
        m.pseqs = [mb.PlaySeq([0])]
        m.sections = [0]
    return m


def octaves_data(n_oct, cycle=16):
    out = b""
    for j in range(n_oct):
        out += sine(cycle * (2 ** j))
    return out


def cents(a, b):
    return 1200.0 * math.log2(a / b)


def measure_freq(omp, mod, rate=48000, seconds=1.2, skip=0.15):
    s = omp.render(mod, rate, int(rate * seconds))
    s = s[int(rate * skip):]
    ups = []
    for i in range(1, len(s)):
        if s[i - 1] < 0 <= s[i]:
            ups.append(i - 1 + (-s[i - 1]) / (s[i] - s[i - 1]))
    if len(ups) < 3:
        return 0.0
    return (len(ups) - 1) * rate / (ups[-1] - ups[0])


def sustained(omp, mod, rate=22050):
    s = omp.render(mod, rate, int(rate * 1.0))
    tail = s[int(rate * 0.6):]
    return bool(tail) and sum(abs(x) for x in tail) / len(tail) > 0.01


# ---- the comparisons -----------------------------------------------------------------------------------------------------


def write(workdir, name, module):
    path = os.path.join(workdir, name + ".mmd")
    with open(path, "wb") as f:
        f.write(mb.build(module))
    return path


def check_notes_and_volumes(c, omp, tool, workdir):
    for ver in range(4):
        for vol_flag in (0, 0x10):
            m = mb.Module(ver)
            m.flags = vol_flag
            top = 63 if ver == 0 else 120
            notes = list(range(1, top + 1))
            b = mb.Block(4, (len(notes) + 3) // 4)
            for i, n in enumerate(notes):
                b.cell(i // 4, i % 4, n, 1 + n % 5, 0, 0)
            vols = mb.Block(4, 8)
            data = [0x00, 0x01, 0x20, 0x32, 0x40, 0x63, 0x64, 0x65, 0x7F, 0x80, 0xFF, 0x1F, 0x09, 0x10, 0x55, 0x33]
            for i, d in enumerate(data):
                vols.cell(i // 4 % 8, i % 4, 13, 1, 0x0C, d)
            vols.lines = 4
            m.blocks = [b, vols]
            m.playseq = [0, 1]
            m.instruments = [mb.Instrument(type=0, data=sine(8)) for _ in range(5)]
            for ins in m.instruments:
                ins.replen = 4
            if ver >= 2:
                m.pseqs = [mb.PlaySeq([0, 1])]
                m.sections = [0]
            name = "notes_v%d_h%d" % (ver, vol_flag)
            path = write(workdir, name, m)
            imp = run_tool(tool, path)
            c.check(imp.ok, "%s: the import failed: %s" % (name, imp.text[:200]))
            if not imp.ok:
                continue
            mod, buf = omp.load(open(path, "rb").read())
            # block 0 is OpenMPT pattern 0; ours are patterns 1 (and 2 after a cut): find a step by (block line)
            pieces = {pid: p for pid, p in imp.pieces.items()}
            for pid, (block, first, last, replay) in pieces.items():
                for line in range(first, last + 1):
                    for track in range(4):
                        note, inst, volfx, eff, vol, param = omp.cell(mod, block, line, track)
                        st = imp.steps.get((pid, track, line - first))
                        if block == 0:
                            want_note = note
                            have = st["note"] if st else 0
                            ok = have == want_note or (want_note == 0 and (have == 0 or 121 <= have <= 128))
                            c.check(ok, "%s: note at block 0 line %d track %d: ours %d, OpenMPT %d" % (name, line, track, have, want_note))
                            if note != 0 or inst != 0:
                                c.check((st["inst"] if st else 0) == inst,
                                        "%s: instrument at line %d track %d: ours %d, OpenMPT %d" % (name, line, track, st["inst"] if st else 0, inst))
                        elif eff == 13:  # CMD_VOLUME: the 0C of block 1
                            if st is None:
                                c.check(False, "%s: no step for the volume at line %d track %d" % (name, line, track))
                                continue
                            ours = (st["vol"] + 1) >> 1
                            c.check(ours == min(param, 64),
                                    "%s: volume %d at line %d track %d: ours %d (column %d), OpenMPT %d" % (name, param, line, track, ours, st["vol"], param))
            omp.free(mod)


def pitch_case(c, omp, tool, workdir, name, module, opts=(), tolerance=8.0):
    path = write(workdir, name, module)
    imp = run_tool(tool, path, opts)
    c.check(imp.ok, "%s: the import failed: %s" % (name, imp.text[:200]))
    if not imp.ok:
        return
    st = imp.steps.get((1, 0, 0))
    ins = imp.instr.get(0)
    if st is None or ins is None or ins.get("result") != "sample":
        c.check(False, "%s: no sample step to compare" % name)
        return
    mod, buf = omp.load(open(path, "rb").read())
    f = measure_freq(omp, mod)
    omp.free(mod)
    rate = int(ins["rate"])
    frames = int(ins["frames"])
    tune = int(ins["tune"])
    fine = int(ins["fine"])
    note = st["note"]
    if note == 0:
        c.check(False, "%s: the importer dropped the note" % name)
        return
    want = rate * 2.0 ** ((note - 61 + tune + fine / 100.0) / 12.0) / frames
    c.check(f > 0, "%s: libopenmpt rendered nothing" % name)
    if f > 0:
        err = cents(f, want)
        c.check(abs(err) <= tolerance, "%s: pitch off by %.1f cents (OpenMPT %.2f Hz, importer %.2f Hz: note %d tune %d fine %d rate %d frames %d)"
                % (name, err, f, want, note, tune, fine, rate, frames))


def check_pitch(c, omp, tool, workdir):
    # plain samples: 8-bit, 16-bit, stereo, in every version, over the notes a type-0 sample can play (C-1 to B-3)
    notes0 = [1, 4, 8, 13, 17, 24, 30, 36]
    for ver in range(4):
        for itype, data, label in ((0, None, "8"), (0x10, sine16(64), "16"), (0x20, sine(64) + sine(64), "st")):
            for n in notes0:
                if ver == 3 and n > 24:
                    continue  # the importer and OpenMPT both fold nothing, but MMD3's Amiga range ends two octaves higher up
                nn = n + (24 if ver == 3 else 0)
                m = one_note_module(ver, nn if ver == 3 else n, itype=itype, data=data)
                if ver == 0 and (nn if ver == 3 else n) > 63:
                    continue
                pitch_case(c, omp, tool, workdir, "pitch_v%d_t%s_n%d" % (ver, label, n), m)
    # strans and playtransp
    for strans in (-7, 5, 12):
        for ver in (1, 3):
            pitch_case(c, omp, tool, workdir, "pitch_strans%d_v%d" % (strans, ver),
                       one_note_module(ver, 13 if ver < 3 else 37, strans=strans))
    for tr in (-12, 3, 12):
        pitch_case(c, omp, tool, workdir, "pitch_playtransp%d" % tr, one_note_module(1, 13, playtransp=tr))
    # finetune of InstrExt (stride 10 reaches the flag, so the sample loops by its flag)
    for ft in (-8, -3, 1, 7):
        m = one_note_module(1, 13, ext=[0, 0, 0, ft & 0xFF, 0, 1], ext_stride=10)
        pitch_case(c, omp, tool, workdir, "pitch_finetune%d" % ft, m)
    # the multi-octave types over the range each can play (libopenmpt folds a note beyond it by octaves, the importer does not)
    octaves = {1: 5, 2: 3, 3: 2, 4: 4, 5: 6, 6: 7}
    top = {1: 72, 2: 60, 3: 48, 4: 72, 5: 72, 6: 72}
    for t in range(1, 7):
        for n in range(1, top[t] + 1, 5):
            m = one_note_module(1, n, itype=t, data=octaves_data(octaves[t]), replen=8)
            pitch_case(c, omp, tool, workdir, "pitch_octaves_t%d_n%d" % (t, n), m)
        m = one_note_module(1, 13, itype=t, data=octaves_data(octaves[t]), replen=8, strans=3)
        pitch_case(c, omp, tool, workdir, "pitch_octaves_t%d_strans" % t, m)
    # type 7: two extra low octaves
    for n in (1, 13, 25, 37, 49, 61):
        pitch_case(c, omp, tool, workdir, "pitch_ext_n%d" % n, one_note_module(1, n, itype=7))
    # the NTSC clock: the importer's rate is 8,363 Hz, libopenmpt's is PAL's 8,287: 15.8 cents apart, said so, not hidden
    path = write(workdir, "pitch_ntsc", one_note_module(1, 13))
    imp = run_tool(tool, path, ("ntsc",))
    mod, buf = omp.load(open(path, "rb").read())
    f = measure_freq(omp, mod)
    omp.free(mod)
    rate = int(imp.instr[0]["rate"])
    c.check(rate == 8363, "the NTSC rate is 8,363 Hz, got %d" % rate)
    err = cents(rate / 64.0, f)
    c.check(abs(err - 15.8) < 3.0, "NTSC against libopenmpt's PAL: %.1f cents, expected 15.8" % err)


def check_loops(c, omp, tool, workdir):
    for ver in range(4):
        for ext, stride, label in ((None, 0, "noext"), ([0, 0, 0, 0, 0, 0], 6, "flag0"), ([0, 0, 0, 0, 0, 1], 6, "flag1"),
                                    ([0, 0, 0, 0, 0, 1], 10, "flag1s10"), ([0, 0, 0, 0, 0, 0], 10, "flag0s10")):
            for replen in (0, 1, 32):
                m = one_note_module(ver, 13 if ver < 3 else 37, ext=ext, ext_stride=stride, replen=replen)
                name = "loop_v%d_%s_r%d" % (ver, label, replen)
                path = write(workdir, name, m)
                imp = run_tool(tool, path)
                if not imp.ok:
                    c.check(False, "%s: import failed" % name)
                    continue
                mod, buf = omp.load(open(path, "rb").read())
                sus = sustained(omp, mod)
                omp.free(mod)
                mode = int(imp.instr[0]["mode"])
                c.check((mode != 0) == sus, "%s: the importer says play mode %d, libopenmpt %s the sample" % (name, mode, "sustains" if sus else "stops"))


def check_tempo(c, omp, tool, workdir):
    rows = []
    for tempo in (11, 20, 33, 60, 100, 200):
        for spd in (3, 6):
            rows.append(("speed_t%d_s%d" % (tempo, spd), 0, 0, tempo, spd, 0.02))
    for tempo in (1, 5, 10):  # speed mode with the SoundTracker-compatible tempos
        rows.append(("speed_low_t%d" % tempo, 0, 0, tempo, 6, 0.005))
    for tempo in (100, 125, 180):
        for lpb_bits in (3, 7):
            for spd in (3, 6):
                rows.append(("bpm_t%d_l%d_s%d" % (tempo, lpb_bits + 1, spd), 0, 0x20 | lpb_bits, tempo, spd, 0.005))
    for tempo in (1, 4, 10, 11):
        rows.append(("eight_t%d" % tempo, 0x40, 0, tempo, 6, 0.005))
    for name, flags, flags2, tempo, spd, tol in rows:
        m = one_note_module(1, 13, flags=flags, flags2=flags2, deftempo=tempo, spd=spd, lines=16, replen=0)
        LINES = 16
        path = write(workdir, "tempo_" + name, m)
        imp = run_tool(tool, path)
        if not imp.ok:
            c.check(False, "tempo_%s: import failed" % name)
            continue
        mod, buf = omp.load(open(path, "rb").read())
        theirs = omp.duration(mod)
        omp.free(mod)
        ours = LINES * 15.0 / (imp.project["tempo"] / 100.0)
        bpm_theirs = LINES * 15.0 / theirs
        if bpm_theirs > 800.0 or bpm_theirs < 40.0:  # outside the model's tempo range: the importer clamps, and says so
            want = 80000 if bpm_theirs > 800.0 else 4000
            c.check(imp.project["tempo"] == want, "tempo_%s: %.1f BPM is outside the model's range: expected the clamp %d, got %d"
                    % (name, bpm_theirs, want, imp.project["tempo"]))
            continue
        rel = ours / theirs - 1.0
        c.check(abs(rel) <= tol, "tempo_%s: %d lines take %.4f s here (%.2f BPM), %.4f s in libopenmpt: %.2f %% apart (limit %.1f %%)"
                % (name, LINES, ours, imp.project["tempo"] / 100.0, theirs, rel * 100, tol * 100))


def flow_module(version=1):
    """Blocks of lines marked by their number in the note column, so any pattern is recognisable; returns the module."""
    m = mb.Module(version)
    m.deftempo = 125
    m.flags2 = 0x20 | 3
    m.tempo2 = 3
    m.instruments = [mb.Instrument(type=0, data=sine(8))]
    m.instruments[0].replen = 4
    return m


def check_flow(c, omp, tool, workdir):
    scenarios = []

    def blocks(*sizes):
        out = []
        for s in sizes:
            out.append(mb.Block(2, s))
        return out

    # a break: 0F 00 on line 2 of A (and 1D 02 on line 1 of B jumping to line 2 of the next)
    m = flow_module()
    a, b, cc = blocks(4, 4, 6)
    a.cell(2, 0, 13, 1, 0x0F, 0x00)
    b.cell(1, 1, 14, 1, 0x1D, 0x02)
    m.blocks = [a, b, cc]
    m.playseq = [0, 1, 2, 0]
    scenarios.append(("breaks", m))
    # a jump: 0B 02 on line 1 of A to position 2
    m = flow_module()
    a, b, cc = blocks(4, 3, 5)
    a.cell(1, 0, 13, 1, 0x0B, 0x02)
    m.blocks = [a, b, cc]
    m.playseq = [0, 1, 2]
    scenarios.append(("jump", m))
    # a loop: 16 00 on line 1, 16 03 on line 3 of a block of 6 (lines 1..3 three times over)
    m = flow_module()
    a, b = blocks(6, 3)
    a.cell(1, 0, 13, 1, 0x16, 0x00)
    a.cell(3, 0, 14, 1, 0x16, 0x02)
    m.blocks = [a, b]
    m.playseq = [0, 1]
    scenarios.append(("loop", m))
    # a repeated line: 1E 02 on line 2: the row shows once, the importer repeats it as a pattern
    m = flow_module()
    a, b = blocks(5, 3)
    a.cell(2, 0, 13, 1, 0x1E, 0x02)
    m.blocks = [a, b]
    m.playseq = [0, 1]
    scenarios.append(("repeat", m))
    # 1D takes its line as hex (V5 manual, OpenMPT) or BCD (libxmp): 1D 10 is line 16 or line 10 of a block of 20
    m = flow_module()
    a, b = blocks(4, 20)
    a.cell(1, 0, 13, 1, 0x1D, 0x10)
    m.blocks = [a, b]
    m.playseq = [0, 1]
    scenarios.append(("break_hex", m))
    # a break to a line past the next block's end restarts it at line 0 (both players), and a jump out of range
    m = flow_module()
    a, b = blocks(4, 3)
    a.cell(1, 0, 13, 1, 0x1D, 0x08)
    m.blocks = [a, b]
    m.playseq = [0, 1]
    scenarios.append(("break_past_end", m))
    # the same in MMD2 with one section and one play sequence
    m = flow_module(2)
    a, b, cc = blocks(4, 3, 5)
    a.cell(1, 0, 13, 1, 0x0B, 0x02)
    m.blocks = [a, b, cc]
    m.pseqs = [mb.PlaySeq([0, 1, 2])]
    m.sections = [0]
    scenarios.append(("jump_mmd2", m))
    for name, m in scenarios:
        path = write(workdir, "flow_" + name, m)
        imp = run_tool(tool, path)
        if not imp.ok:
            c.check(False, "flow_%s: import failed: %s" % (name, imp.text[:200]))
            continue
        ours = []
        for pid in imp.order:
            block, first, last, replay = imp.pieces[pid]
            for line in range(first, last + 1):
                if not ours or ours[-1] != (block, line):
                    ours.append((block, line))
        mod, buf = omp.load(open(path, "rb").read())
        theirs = omp.trace(mod, len(ours))
        omp.free(mod)
        c.check(theirs[:len(ours)] == ours, "flow_%s: rows differ\n  importer: %s\n  OpenMPT:  %s" % (name, ours, theirs[:len(ours) + 2]))


def our_duration(imp):
    """Seconds the importer's song takes: each line lasts 15 / BPM, the BPM starting at the project's and following the Tempo FX
    (15: value x 2; 44, in the same step: + (v - 128) / 4) wherever they are; Speed (43) changes no line's length."""
    bpm = imp.project["tempo"] / 100.0
    total = 0.0
    for pid in imp.order:
        _, first, last, _ = imp.pieces[pid]
        for line in range(last - first + 1):
            for key in sorted(k for k in imp.steps if k[0] == pid and k[2] == line):
                tempo_seen = False
                for t, v in imp.steps[key]["fx"]:
                    if t == 15:
                        bpm = v * 2.0
                        tempo_seen = True
                    elif t == 44 and tempo_seen:
                        bpm += (v - 128) * 0.25
            total += 15.0 / bpm
    return total


def check_dropped_track_timing(c, omp, tool, workdir):
    """More than 16 tracks: the 20th carries only a tempo and a speed command, gets no slot, and the song must not run at the wrong
    speed for it (the importer carries those two onto a track that has a slot)."""
    cases = (("tempo", [(4, 0x0F, 0x50)]), ("speed", [(4, 0x09, 0x03)]), ("both", [(3, 0x0F, 0x28), (9, 0x09, 0x04)]))
    for name, cmds in cases:
        m = mb.Module(1)
        m.deftempo, m.tempo2 = 33, 6
        b = mb.Block(20, 16)
        for t in range(16):
            for line in range(16):
                b.cell(line, t, 13, 1, 0, 0)
        for line, cmd, data in cmds:
            b.cell(line, 19, 0, 0, cmd, data)
        m.blocks = [b]
        m.playseq = [0]
        ins = mb.Instrument(type=0, data=sine(64))
        ins.replen = 32
        m.instruments = [ins]
        path = write(workdir, "dropped_" + name, m)
        imp = run_tool(tool, path)
        if not imp.ok:
            c.check(False, "dropped_%s: import failed: %s" % (name, imp.text[:200]))
            continue
        c.check(int(imp.report.get("timing_carried", "0")) >= len(cmds), "dropped_%s: nothing was carried: %s" % (name, imp.report))
        c.check(int(imp.report.get("timing_lost", "1")) == 0, "dropped_%s: time was lost" % name)
        mod, buf = omp.load(open(path, "rb").read())
        theirs = omp.duration(mod)
        omp.free(mod)
        ours = our_duration(imp)
        rel = ours / theirs - 1.0
        # the CIA constants of speed mode are 1.3 % slower than libopenmpt's, the Tempo FX is exact to 0.125 BPM
        c.check(abs(rel) <= 0.025, "dropped_%s: the song takes %.3f s here, %.3f s in libopenmpt: %.2f %% apart" % (name, ours, theirs, rel * 100))


def check_volume_persistence(c, omp, tool, workdir):
    """0C persists until the next instrument number (libopenmpt: 0C 20 then a bare note plays at 20 of 64, not full). The loudness of
    every row of the render, against the volume the importer's steps give it: the column when it has one, else the instrument's own."""
    m = mb.Module(1)
    m.deftempo, m.tempo2 = 33, 6
    b = mb.Block(4, 10)
    rows = [(13, 1, None), (13, 1, 0x20), (15, 0, None), (13, 1, None), (15, 0, None), (13, 2, None), (15, 0, None), (13, 1, 0x00),
            (15, 0, None), (13, 2, 0x50)]
    for line, (note, inst, vol) in enumerate(rows):
        b.cell(line, 0, note, inst, 0x0C if vol is not None else 0, vol if vol is not None else 0)
    m.blocks = [b]
    m.playseq = [0]
    full = mb.Instrument(type=0, data=sine(64))
    full.replen = 32
    half = mb.Instrument(type=0, data=sine(64))
    half.replen = 32
    half.svol = 32
    m.instruments = [full, half]
    svol = {1: 64, 2: 32}
    path = write(workdir, "volume_persist", m)
    imp = run_tool(tool, path)
    if not imp.ok:
        c.check(False, "volume_persist: import failed: %s" % imp.text[:200])
        return
    c.check(int(imp.report.get("volume_baked", "0")) >= 2, "volume_persist: the importer baked %s volumes" % imp.report.get("volume_baked"))
    rate = 22050
    mod, buf = omp.load(open(path, "rb").read())
    samples = omp.render(mod, rate, int(rate * 0.12 * len(rows)))
    omp.free(mod)
    per_row = int(rate * 0.12)

    def rms(row):
        w = samples[row * per_row + int(per_row * 0.3):row * per_row + int(per_row * 0.9)]
        return math.sqrt(sum(x * x for x in w) / len(w)) if w else 0.0

    ref = rms(0)
    c.check(ref > 0.01, "volume_persist: libopenmpt rendered nothing at row 0")
    inst = 0
    for line, (note, i, vol) in enumerate(rows):
        if i:
            inst = i
        st = imp.steps.get((1, 0, line))
        col = st["vol"] if st else 255
        expected = ((col + 1) >> 1) if col != 255 else svol[inst]
        have = rms(line) / ref * 64.0 if ref > 0 else 0.0
        c.check(abs(have - expected) <= 4.0, "volume_persist: row %d plays at %.1f of 64 in libopenmpt, the importer's step says %d "
                "(column %d, instrument %d)" % (line, have, expected, col, inst))


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    tool, workdir = sys.argv[1], sys.argv[2]
    lib = load_library()
    if lib is None:
        if STRICT:
            print("med_openmpt_oracle: libopenmpt is not installed, and O3_CI=1 says this test may not be skipped (apt: libopenmpt0t64)")
            return 1
        print("med_openmpt_oracle: libopenmpt is not installed here, skipped")
        return SKIP
    os.makedirs(workdir, exist_ok=True)
    omp = Omp(lib)
    version = omp.version()
    print("libopenmpt %d.%d.%d" % version)
    if version[:2] < MIN_VERSION:
        msg = "libopenmpt %d.%d.%d is older than %d.%d, the version the comparisons were measured on" % (version + MIN_VERSION)
        if STRICT:
            print("med_openmpt_oracle: " + msg + ", and O3_CI=1 says this test may not be skipped")
            return 1
        print("med_openmpt_oracle: " + msg + ", skipped")
        return SKIP
    c = Checks()
    for fn in (check_notes_and_volumes, check_pitch, check_loops, check_tempo, check_flow, check_dropped_track_timing,
               check_volume_persistence):
        before = c.count
        fn(c, omp, tool, workdir)
        print("%s: %d comparisons" % (fn.__name__, c.count - before))
    print("%d comparisons, %d disagreements" % (c.count, len(c.failures)))
    if c.count < MIN_COMPARISONS:
        print("FAIL: only %d comparisons were made, at least %d are expected: a check has gone quiet" % (c.count, MIN_COMPARISONS))
        return 1
    return 1 if c.failures else 0


if __name__ == "__main__":
    sys.exit(main())
