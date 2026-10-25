#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A second opinion on the MOD and S3M importers (OS-MODIMP, docs/formats/mod.md), from libopenmpt.

  modimp_openmpt_oracle.py TOOL WORKDIR

TOOL is `o3mod_tool`; `TOOL import FILE [options]` prints the project the importer makes of FILE. This script writes synthetic modules
(tools/mod_build.py: nothing here is a real module and nothing is committed), imports each with TOOL, loads the same bytes into libopenmpt
and compares what the two make of them, the way tests/med_openmpt_oracle.py does for OctaMED:

  decode     note, instrument, volume column and effect of every cell: our note byte is OpenMPT's note number, our instrument number its
             instrument, an S3M volume column v is our 2 v, and the FX the importer writes for an effect are the ones docs/formats/mod.md section 5
             lists for the effect OpenMPT reads in that cell (its text form: "C20", "EA3", "SB2"), for every MOD variant and for S3M
  pitch      the frequency libopenmpt renders for a note equals the one the importer's rate, note and fine tune give (MOD: the 36 notes of the
             period table, every finetune; S3M: C2SPD, octave, 8 and 16 bit)
  loops      a sample sustains in libopenmpt exactly when the importer says it loops
  flow       the rows OpenMPT visits against the rows of the importer's pieces in the importer's order (breaks, jumps, loops, in MOD and in S3M,
             including the cases where the two formats differ: a jump after a break on the row, which pattern delay wins)
  timing     the length of a song from OpenMPT's duration against 15 / BPM per row of the importer's patterns (speed and tempo commands,
             pattern delays, the start values, the S3M clamps)
  channels   more than 16 channels: the 20th carries only a tempo and a speed, and the song must not run at the wrong speed for it
  volume     a note without an instrument number keeps the channel's volume (MOD Cxx, S3M volume column): the loudness of each row of OpenMPT's
             render against the importer's steps
  core       OS-COREFX CF1, the importer on the Modern engine (an S3M, or a MOD with `noengine`): the tracker FX 46..50 (arpeggio, volume slides, fine volume,
             fine slides: the same decode of every cell, with the units of ADR 0004 amendment 2) and the pan of MOD 8xx / E8x; and the volume a bare note
             keeps after a volume slide (OpenMPT's loudness against the volume the importer bakes into the step)

libopenmpt is not vendored: it is loaded with ctypes from the system (the shared library alone is enough, no headers). Without it the script exits
77, which ctest reports as skipped, except under CI: with O3_CI=1 in the environment (tools/ci.sh sets it, the workflow installs the library) a
missing or too old library, or fewer than MIN_COMPARISONS comparisons, is a failure, so that the test cannot go quiet where it matters. Where the
two disagree on purpose the reason is in the comments below.
"""
import ctypes
import math
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import med_openmpt_oracle as mo  # noqa: E402  (its libopenmpt binding, the importer output parser, the duration and pitch helpers)
import mod_build as mb  # noqa: E402

SKIP = 77
MIN_VERSION = (0, 7)        # the behaviour below was measured on libopenmpt 0.7.3; Ubuntu 24.04 ships it
MIN_COMPARISONS = 10000     # a run that compares less than this has lost a check (the full run makes about 13,600)
STRICT = os.environ.get("O3_CI") == "1"


class Omp(mo.Omp):
    def __init__(self, lib):
        super().__init__(lib)
        lib.openmpt_module_format_pattern_row_channel.restype = ctypes.c_void_p
        lib.openmpt_module_format_pattern_row_channel.argtypes = [ctypes.c_void_p, ctypes.c_int32, ctypes.c_int32, ctypes.c_int32,
                                                                  ctypes.c_size_t, ctypes.c_int]
        lib.openmpt_free_string.argtypes = [ctypes.c_void_p]

    def text(self, mod, pattern, row, channel):
        """The cell as OpenMPT shows it: 'C-5 01v40 A0F' (note, instrument, volume column, effect)."""
        ptr = self.lib.openmpt_module_format_pattern_row_channel(mod, pattern, row, channel, 0, 1)
        s = ctypes.string_at(ptr).decode("latin-1")
        self.lib.openmpt_free_string(ptr)
        return s


def write(workdir, name, module):
    ext = ".mod" if isinstance(module, mb.Mod) else ".s3m"
    path = os.path.join(workdir, name + ext)
    with open(path, "wb") as f:
        f.write(mb.build(module))
    return path


def tracks_of(imp):
    return {int(m.group(1)): int(m.group(2)) for m in re.finditer(r"^track (\d+) slot=(\d+)", imp.text, re.M)}


def run(tool, path, opts=()):
    return mo.run_tool(tool, path, opts)


# ---- the translation docs/formats/mod.md section 5 lists, written out for the check ---------------------------------------------------


def pan_fx(p255):
    """Channel pan (FX 57, OS-COREFX): the pan 0..255 of libopenmpt (S3M Xxx is 2 x min(xx, 0x80)) as 0..100, rounded."""
    return (57, (max(0, min(255, p255)) * 100 + 127) // 255)


def expected_mod(letter, param):
    """(kind, value): 'fx' with (type, value) in the engine window or OG types, 'vol' with the volume byte, 'none', 'flow', 'timing'."""
    p, x = param, param & 15
    E = 128
    if letter == "0":
        return ("fx", (E + 0x00, p)) if p else ("none", None)
    table = {"1": 0x01, "2": 0x02, "3": 0x03, "4": 0x14, "5": 0x05, "6": 0x06, "7": 0x07, "9": 0x19, "A": 0x0A}
    if letter in table:
        return ("fx", (E + table[letter], p))
    if letter == "8":
        return ("fx", pan_fx(p))  # OS-COREFX: not a ProTracker effect, but libopenmpt (and every tracker since FastTracker) reads it as a pan: 00 left, 80 centre, FF right
    if letter == "C":
        return ("vol", min(127, 2 * min(p, 64)))
    if letter in "BD":
        return ("flow", None)
    if letter == "F":
        return ("timing", None) if p else ("none", None)
    if letter == "E":
        sub = p >> 4
        if sub == 0:
            return ("fx", (E + 0x0F, 0xF9 if x == 0 else 0xF8))
        if sub == 1:
            return ("fx", (E + 0x11, x))
        if sub == 2:
            return ("fx", (E + 0x12, x))
        if sub == 5:
            return ("fx", (E + 0x15, (0xF0 | x) if x >= 8 else x))
        if sub == 9:
            return ("fx", (E + 0x1F, x)) if x else ("none", None)
        if sub == 0xA:
            return ("fx", (E + 0x1A, x))
        if sub == 0xB:
            return ("fx", (E + 0x1B, x))
        if sub == 0xC:
            return ("fx", (E + 0x18, x))
        if sub == 0xD:
            return ("fx", (E + 0x1F, x << 4)) if x else ("none", None)
        if sub in (6, 0xE):
            return ("flow", None)
        if sub == 8:
            return ("fx", pan_fx(x * 17))
        return ("drop", None)  # E3x E4x E7x EFx
    return ("drop", None)


def expected_s3m(letter, param):
    p, hi, lo = param, param >> 4, param & 15
    E = 128
    if letter in "BC":
        return ("flow", None)
    if letter == "A":
        return ("timing", None) if p else ("none", None)
    if letter == "T":
        return ("timing", None) if p >= 0x20 else ("none", None)
    if letter == "D":
        if p == 0:
            return ("fx", (E + 0x0A, 0))
        if lo == 0:
            return ("fx", (E + 0x0A, hi << 4))
        if hi == 0:
            return ("fx", (E + 0x0A, lo))
        if lo == 0xF:
            return ("fx", (E + 0x1A, hi))
        if hi == 0xF:
            return ("fx", (E + 0x1B, lo))
        return ("fx", (E + 0x0A, p))
    if letter in "EF":
        slide, fine = (0x02, 0x12) if letter == "E" else (0x01, 0x11)
        if p >= 0xF0:
            return ("fx", (E + fine, lo)) if lo else ("none", None)
        if p >= 0xE0:
            return ("fx", (E + fine, lo >> 2)) if (lo >> 2) else ("drop", None)
        return ("fx", (E + slide, p))
    simple = {"G": 0x03, "H": 0x14, "K": 0x06, "L": 0x05, "O": 0x19, "R": 0x07}
    if letter in simple:
        return ("fx", (E + simple[letter], p))
    if letter == "J":
        return ("fx", (E + 0x00, p)) if p else ("none", None)
    if letter == "Q":
        return ("fx", (E + 0x1F, lo)) if lo else ("none", None)
    if letter == "U":
        depth = 0 if lo == 0 else max(1, lo >> 2)
        return ("fx", (E + 0x14, (p & 0xF0) | depth))
    if letter == "X":
        return ("fx", pan_fx(2 * min(p, 0x80)))
    if letter == "S":
        if hi == 2:
            return ("fx", (E + 0x15, (lo - 8) & 0xFF))
        if hi == 8:
            return ("fx", pan_fx(lo * 17))
        if hi == 0xC:
            return ("fx", (E + 0x18, lo))
        if hi == 0xD:
            return ("fx", (E + 0x1F, lo << 4)) if lo else ("none", None)
        if hi in (0xB, 0xE):
            return ("flow", None)
        return ("drop", None)
    return ("drop", None)  # I M N P V W Y Z


def expected_core(is_mod, letter, param):
    """The same for the Modern engine (OS-COREFX): ('fx', (type, value)), ('fine', value) for a fine volume slide (the FX 48, or the volume baked into a note's step), ('none', None) or ('skip', None)
    for what this pass does not look at (the OG / core FX of the other effects are unchanged and the engine pass above covers their decode)."""
    p, x, hi, lo = param, param & 15, param >> 4, param & 15
    if is_mod:
        if letter == "0":
            return ("fx", (46, p)) if p else ("none", None)
        if letter == "8":
            return ("fx", pan_fx(p))
        if letter == "A":
            return ("fx", (47, p)) if p else ("none", None)
        if letter == "E":
            if hi == 1:
                return ("fx", (49, 4 * x)) if x else ("none", None)
            if hi == 2:
                return ("fx", (50, 4 * x)) if x else ("none", None)
            if hi == 0xA:
                return ("fine", x << 4) if x else ("none", None)
            if hi == 0xB:
                return ("fine", x) if x else ("none", None)
            if hi == 8:
                return ("fx", pan_fx(x * 17))
        return ("skip", None)
    if letter == "D":
        if p == 0:
            return ("none", None)
        if lo == 0:
            return ("fx", (47, hi << 4))
        if hi == 0:
            return ("fx", (47, lo))
        if lo == 0xF:
            return ("fine", hi << 4)
        if hi == 0xF:
            return ("fine", lo)
        return ("fx", (47, p))
    if letter in "EF":
        up = letter == "F"
        if p >= 0xF0:
            return ("fx", (49 if up else 50, 4 * lo)) if lo else ("none", None)
        if p >= 0xE0:
            return ("fx", (49 if up else 50, lo)) if lo else ("none", None)  # extra fine: exact since OS-COREFX
        return ("skip", None)
    if letter == "J":
        return ("fx", (46, p)) if p else ("none", None)
    if letter == "X":
        return ("fx", pan_fx(2 * min(p, 0x80)))
    if letter == "S" and hi == 8:
        return ("fx", pan_fx(lo * 17))
    return ("skip", None)


def sanitize_text(letter_and_param):
    return letter_and_param


# ---- the harness -----------------------------------------------------------------------------------------------------------------------


class Checks(mo.Checks):
    pass


def sine_u8(n):
    return bytes((128 + v) & 0xFF for v in mb.sine(n))


# ---- decode ----------------------------------------------------------------------------------------------------------------------------

MOD_VARIANTS = (("M.K.", 4), ("M!K!", 4), ("FLT4", 4), ("4CHN", 4), ("6CHN", 6), ("8CHN", 8), ("FLT8", 8), ("10CH", 10), ("16CH", 16), ("28CH", 28))


def decode_module_mod(tag, channels):
    m = mb.Mod(tag, channels, "decode %s" % tag)
    m.samples = [mb.ModSample(mb.sine(16), "s%d" % i, 8 + 2 * i, 0, 0, 8) for i in range(31)]
    top = 4
    pats = []
    effects = [(0x0, 0x47), (0x1, 0x12), (0x2, 0x34), (0x3, 0x56), (0x4, 0x78), (0x5, 0x9A), (0x6, 0xBC), (0x7, 0xDE), (0x8, 0x20), (0x9, 0x01), (0xA, 0x0F),
               (0xC, 0x30), (0xC, 0x41), (0xE, 0x01), (0xE, 0x00), (0xE, 0x15), (0xE, 0x1F), (0xE, 0x2A), (0xE, 0x39), (0xE, 0x45), (0xE, 0x58), (0xE, 0x75),
               (0xE, 0x84), (0xE, 0x93), (0xE, 0x9F), (0xE, 0xA3), (0xE, 0xB5), (0xE, 0xC2), (0xE, 0xD4), (0xE, 0xF7), (0xF, 0x00),
               (0x8, 0x00), (0x8, 0xFF), (0x8, 0x80), (0xE, 0x80), (0xE, 0x8F), (0xA, 0xF0), (0xA, 0x00), (0xE, 0x10), (0xE, 0x20), (0xE, 0x1F), (0xE, 0xA0), (0xE, 0xB0), (0xE, 0xAF)]
    pat = {}
    row = 0
    for i, (eff, par) in enumerate(effects):
        ch = i % channels
        r = i // channels
        pat[(r, ch)] = ((i * 5 + ch) % 36, 1 + i % 31, eff, par)
    pats.append(pat)
    # a second pattern: notes of the whole table and every sample number
    pat2 = {}
    for i in range(36 * 1):
        pat2[(i % 64, (i // 64 + i) % channels)] = (i % 36, 1 + (i * 7) % 31, 0, 0)
    pats.append(pat2)
    m.orders = [0, 1] if tag != "FLT8" else [0, 2]
    m.patterns = pats
    return m


def decode_module_s3m():
    m = mb.S3m("decode s3m")
    m.instruments = [mb.S3mInst(sine_u8(16), "i%d" % i, 8363 + 100 * i, 10 + i, (0, 16)) for i in range(12)]
    m.channels = [0, 8, 1, 9, 2, 10, 3, 11] + [255] * 24
    pat = {}
    letters = "ADEFGHJKLOQRSTUX"
    n = 0
    cells = []
    for lt in letters:
        cmd = ord(lt) - 64
        for par in (0x00, 0x01, 0x10, 0x25, 0x4F, 0xF1, 0xE3, 0x7F, 0xFF, 0xE1, 0xEF, 0xF4):
            if lt == "S":
                continue
            cells.append((cmd, par))
    for sub in range(16):
        for x in (0, 1, 7, 15):
            if sub in (0xB, 0xE):
                continue
            cells.append((19, (sub << 4) | x))
    pats = []
    pat = {}
    for i, (cmd, par) in enumerate(cells):
        r, ch = divmod(i, 8)
        if r >= 64:
            break
        note = 0x40 + (i % 36) // 12 * 16 + (i % 12) if i % 3 == 0 else 255
        vol = (i * 3) % 65 if i % 4 == 1 else 255
        pat[(r, ch)] = (note, 1 + i % 12 if i % 3 == 0 else 0, vol, cmd, par)
    pats.append(pat)
    pat = {}
    for i in range(64):
        for ch in range(8):
            pat[(i, ch)] = (0x20 + (i + ch) % 96 if (i + ch) % 5 else 254 if (i + ch) % 7 == 0 else 255, 1 + (i + ch) % 12 if (i + ch) % 5 else 0,
                            (i * 5 + ch) % 65 if (i + ch) % 3 == 0 else 255, 0, 0)
    pats.append(pat)
    m.orders = [0, 1]
    m.patterns = pats
    return m


def check_decode(c, omp, tool, wd):
    mods = [("dec_mod_%s" % tag, decode_module_mod(tag, ch)) for tag, ch in MOD_VARIANTS] + [("dec_s3m", decode_module_s3m())]
    for name, m in mods:
        decode_pass(c, omp, tool, wd, name, m, "engine")


def check_core(c, omp, tool, wd):
    """OS-COREFX: the same cells imported for the Modern engine (an S3M by default, a MOD with `noengine`): the tracker FX in place of the dropped effects."""
    mods = [("core_mod_%s" % tag, decode_module_mod(tag, ch)) for tag, ch in MOD_VARIANTS] + [("core_s3m", decode_module_s3m())]
    for name, m in mods:
        decode_pass(c, omp, tool, wd, name, m, "core")


def decode_pass(c, omp, tool, wd, name, m, mode):
    is_mod = isinstance(m, mb.Mod)
    path = write(wd, name, m)
    opts = ("engine",) if mode == "engine" else (("noengine",) if is_mod else ())
    imp = run(tool, path, opts)
    c.check(imp.ok, "%s: the import failed: %s" % (name, imp.text[:200]))
    if not imp.ok:
        return
    c.check(("engine=1" in imp.text.split("\n")[0]) == (mode == "engine"), "%s: the engine window is %s" % (name, "off" if mode == "engine" else "on"))
    slots = tracks_of(imp)
    mod, buf = omp.load(open(path, "rb").read())
    memory = {}  # OS-COREFX CF2: S3M parameter memory, per channel and class: the importer writes the repeated value where the cell says 00
    mem_class = {"D": 0, "K": 0, "L": 0, "E": 1, "F": 1, "J": 2, "Q": 3, "R": 4, "S": 5, "P": 6, "I": 7}
    for pid, (block, first, last, replay) in imp.pieces.items():
        if replay:
            continue
        for line in range(first, last + 1):
            for ch in range(m.channels if is_mod else 8):
                text = omp.text(mod, block, line, ch)
                note, inst, volfx, eff, vol, param = omp.cell(mod, block, line, ch)
                slot = slots.get(ch)
                if slot is None:
                    continue  # a channel the importer did not give a track (more than 16: the busiest are kept)
                st = imp.steps.get((pid, slot, line - first))
                if mode == "engine":
                    have_note = st["note"] if st else 0
                    note_text = text[0:3]
                    if note_text == "^^^":
                        c.check(have_note == 254, "%s: pattern %d row %d channel %d: a note cut, ours %d" % (name, block, line, ch, have_note))
                    elif note != 0 and note <= 120:
                        c.check(have_note == note, "%s: note at pattern %d row %d channel %d: ours %d, OpenMPT %d (%s)" % (name, block, line, ch, have_note, note, text))
                    elif note == 0:
                        c.check(have_note == 0, "%s: pattern %d row %d channel %d: ours has note %d where OpenMPT has none (%s)" % (name, block, line, ch, have_note, text))
                    if note != 0 or inst != 0:
                        want_inst = inst if inst <= 31 or not is_mod else 0
                        if st is not None or want_inst:
                            c.check((st["inst"] if st else 0) == want_inst, "%s: instrument at pattern %d row %d channel %d: ours %d, OpenMPT %d" % (name, block, line, ch, st["inst"] if st else 0, want_inst))
                    # the volume column of S3M
                    if not is_mod and volfx == 1:
                        c.check(st is not None and st["vol"] == min(127, 2 * vol), "%s: volume at pattern %d row %d channel %d: ours %s, OpenMPT %d" % (name, block, line, ch, st["vol"] if st else None, vol))
                # the effect
                eff_text = text[-3:]
                if eff_text == "...":
                    continue
                letter, par = eff_text[0], int(eff_text[1:], 16)
                if not is_mod and letter in mem_class:
                    if par != 0:
                        memory[(ch, mem_class[letter])] = par
                    else:
                        par = memory.get((ch, mem_class[letter]), 0)
                if mode == "core":
                    kind, val = expected_core(is_mod, letter, par)
                elif is_mod:
                    kind, val = expected_mod(letter, par)
                else:
                    kind, val = expected_s3m(letter, par)
                if st is None:
                    st = {"fx": [(0, 0), (0, 0)], "vol": 255, "note": 0, "inst": 0}
                if kind == "fx" and st is not None:
                    c.check(val in [tuple(f) for f in st["fx"]], "%s: effect %s at pattern %d row %d channel %d: FX %s, expected %s" % (name, eff_text, block, line, ch, st["fx"], val))
                elif kind == "fine":
                    # a fine volume slide is the FX 48, or, on a row with a note whose volume the importer knows, baked into the step's volume
                    c.check((48, val) in [tuple(f) for f in st["fx"]] or st["vol"] != 255,
                            "%s: effect %s at pattern %d row %d channel %d: FX %s, volume %d, expected FX 48 %d or a baked volume" % (name, eff_text, block, line, ch, st["fx"], st["vol"], val))
                elif kind == "vol" and st is not None:
                    c.check(st["vol"] == val, "%s: effect %s at pattern %d row %d channel %d: volume %d, expected %d" % (name, eff_text, block, line, ch, st["vol"], val))
                elif kind == "drop":
                    # nothing to map it to: no FX in the step (the engine's terminator, E00 00, is type 128)
                    c.check(all(f[0] in (0, 128) for f in [tuple(x) for x in st["fx"]]), "%s: effect %s at pattern %d row %d channel %d has no counterpart, FX %s"
                            % (name, eff_text, block, line, ch, st["fx"]))
    omp.free(mod)


# ---- pitch -----------------------------------------------------------------------------------------------------------------------------


def pitch_case(c, omp, tool, wd, name, module, opts=(), tolerance=14.0, min_hz=0.0, max_hz=9000.0):
    path = write(wd, name, module)
    imp = run(tool, path, opts)
    c.check(imp.ok, "%s: the import failed: %s" % (name, imp.text[:200]))
    if not imp.ok:
        return
    slots = tracks_of(imp)
    st = imp.steps.get((1, slots.get(0), 0))
    ins = imp.instr.get(0)
    if st is None or ins is None or ins.get("result") != "sample":
        c.check(False, "%s: no sample step to compare" % name)
        return
    mod, buf = omp.load(open(path, "rb").read())
    f = mo.measure_freq(omp, mod, 48000, 1.2)
    omp.free(mod)
    rate, frames, tune, fine, note = int(ins["rate"]), int(ins["frames"]), int(ins["tune"]), int(ins["fine"]), st["note"]
    if note == 0:
        c.check(False, "%s: the importer dropped the note" % name)
        return
    want = rate * 2.0 ** ((note - 61 + tune + fine / 100.0) / 12.0) / frames
    if want > max_hz or want < 20:
        return
    c.check(f > 0, "%s: libopenmpt rendered nothing" % name)
    if f > 0:
        err = mo.cents(f, want)
        c.check(abs(err) <= tolerance, "%s: pitch off by %.1f cents (OpenMPT %.2f Hz, importer %.2f Hz: note %d tune %d fine %d rate %d frames %d)"
                % (name, err, f, want, note, tune, fine, rate, frames))


def one_note_mod(note, finetune=0, cycle=32, tag="M.K."):
    m = mb.Mod(tag, {"M.K.": 4, "FLT4": 4, "6CHN": 6}.get(tag, 8), "pitch")
    m.samples[0] = mb.ModSample(mb.sine(cycle), "sine", 64, finetune, 0, cycle // 2)
    m.orders = [0]
    m.patterns = [{(0, 0): (note, 1, 0, 0)}]
    return m


def one_note_s3m(note_byte, c2spd=8363, sixteen=False, signed=False, cycle=32):
    m = mb.S3m("pitch")
    m.signed = signed
    if sixteen:
        data = mb.sine16(cycle)
        if not signed:
            data = b"".join(((struct_u16(data, i) + 32768) & 0xFFFF).to_bytes(2, "little") for i in range(cycle))
    else:
        data = mb.sine(cycle) if signed else sine_u8(cycle)
    m.instruments = [mb.S3mInst(data, "sine", c2spd, 64, (0, cycle), sixteen=sixteen)]
    m.orders = [0]
    m.patterns = [{(0, 0): (note_byte, 1, 255, 0, 0)}]
    return m


def struct_u16(data, i):
    return int.from_bytes(data[2 * i:2 * i + 2], "little", signed=True)


def check_pitch(c, omp, tool, wd):
    for ft in (-8, -5, -1, 0, 1, 3, 7):
        for n in (0, 5, 12, 17, 24, 30, 35):
            if ft > 0 and n > 30:
                continue  # a period below 113 is past Paula's limit: libopenmpt and the Amiga engine clamp it, the formula here does not
            pitch_case(c, omp, tool, wd, "pitch_mod_f%d_n%d" % (ft, n), one_note_mod(n, ft))
    for tag in ("FLT8", "6CHN"):
        for n in (0, 12, 35):
            pitch_case(c, omp, tool, wd, "pitch_%s_n%d" % (tag, n), one_note_mod(n, 0, tag=tag))
    for c2 in (4181, 8363, 11025, 16726, 22050, 33452):
        for note in (0x20, 0x30, 0x40, 0x43, 0x4B, 0x50, 0x60):
            for sixteen, signed in ((False, False), (True, False), (False, True), (True, True)):
                if c2 not in (8363, 22050) and (sixteen or signed):
                    continue
                pitch_case(c, omp, tool, wd, "pitch_s3m_c%d_n%02x_%s%s" % (c2, note, "16" if sixteen else "8", "s" if signed else "u"),
                           one_note_s3m(note, c2, sixteen, signed), tolerance=8.0)
    # a C2SPD of 0 plays at 8,363 Hz (libopenmpt and the importer agree)
    pitch_case(c, omp, tool, wd, "pitch_s3m_c0", one_note_s3m(0x40, 0), tolerance=8.0)


# ---- loops -----------------------------------------------------------------------------------------------------------------------------


def check_loops(c, omp, tool, wd):
    # Left out: (40, 4) starts past the end of the sample, which no reader agrees on, and the loops of 2 to 4 words at the very start, which libopenmpt
    # ends (ProTracker clears the first word of a sample and uses such a loop as its silence; the Amiga loops them, so does the importer). The data is a
    # cosine of 8 bytes, loud wherever a loop of any length lies in it (a sine starts near zero).
    cos8 = bytes(int(round(100 * math.cos(2 * math.pi * i / 8))) & 0xFF for i in range(64))
    for start, length in ((0, 0), (0, 1), (0, 16), (4, 2), (4, 3), (8, 8), (8, 40), (30, 16), (31, 16)):
        m = mb.Mod("M.K.", 4, "loop")
        m.samples[0] = mb.ModSample(cos8, "cos", 64, 0, start, length)
        m.orders = [0]
        m.patterns = [{(0, 0): (12, 1, 0, 0)}]
        name = "loop_mod_%d_%d" % (start, length)
        path = write(wd, name, m)
        imp = run(tool, path)
        if not imp.ok:
            c.check(False, "%s: import failed" % name)
            continue
        mod, buf = omp.load(open(path, "rb").read())
        sus = mo.sustained(omp, mod)
        omp.free(mod)
        mode = int(imp.instr[0]["mode"])
        c.check(sus == (mode == 1), "%s: libopenmpt %s, the importer says mode %d" % (name, "sustains" if sus else "ends", mode))
    for flag in (False, True):
        for loop in ((0, 64), (0, 16), (8, 200), (20, 30), (30, 20), (60, 70), (0, 1)):
            m = mb.S3m("loop")
            m.instruments = [mb.S3mInst(sine_u8(64), "sine", 8363, 64, loop if flag else None)]
            m.orders = [0]
            m.patterns = [{(0, 0): (0x40, 1, 255, 0, 0)}]
            name = "loop_s3m_%d_%d_%d" % (1 if flag else 0, loop[0], loop[1])
            path = write(wd, name, m)
            imp = run(tool, path)
            if not imp.ok:
                c.check(False, "%s: import failed" % name)
                continue
            mod, buf = omp.load(open(path, "rb").read())
            sus = mo.sustained(omp, mod)
            omp.free(mod)
            mode = int(imp.instr[0]["mode"])
            c.check(sus == (mode == 1), "%s: libopenmpt %s, the importer says mode %d" % (name, "sustains" if sus else "ends", mode))


# ---- flow ------------------------------------------------------------------------------------------------------------------------------

S_B, S_C, S_S, S_A, S_T = 2, 3, 19, 1, 20


def fast_mod(pats, orders, channels=4, tag="M.K."):
    """A MOD whose first pattern starts with speed 1 and tempo 255, so that a row is 9.8 ms and the trace is quick."""
    m = mb.Mod(tag, channels, "flow")
    m.samples[0] = mb.ModSample(mb.sine(32), "s", 64, 0, 0, 16)
    m.orders = orders
    p0 = dict(pats[0])
    p0[(0, channels - 1)] = (None, 0, 0xF, 1)
    p0[(0, channels - 2)] = (None, 0, 0xF, 0xFF)
    m.patterns = [p0] + pats[1:]
    return m


def fast_s3m(pats, orders):
    m = mb.S3m("flow")
    m.instruments = [mb.S3mInst(bytes(range(64)), "i")]
    m.orders = orders
    m.speed = 1
    m.tempo = 255
    m.patterns = pats
    return m


def E(cmd, par):
    return (255, 0, 255, cmd, par)


def flow_scenarios():
    s = []
    def mod(name, pats, orders):
        s.append((name, fast_mod(pats, orders)))
    def s3m(name, pats, orders):
        s.append((name, fast_s3m(pats, orders)))
    n = None
    mod("mod_break", [{(2, 0): (n, 0, 0xD, 0x05)}, {}], [0, 1])
    mod("mod_break_bcd", [{(2, 0): (n, 0, 0xD, 0x12)}, {}], [0, 1])
    mod("mod_break_0a", [{(2, 0): (n, 0, 0xD, 0x0A)}, {}], [0, 1])
    mod("mod_break_past_end", [{(2, 0): (n, 0, 0xD, 0x65)}, {}], [0, 1])
    mod("mod_break_63", [{(2, 0): (n, 0, 0xD, 0x63)}, {}], [0, 1])
    mod("mod_jump", [{(2, 0): (n, 0, 0xB, 0x01)}, {}, {}], [0, 2, 1])
    mod("mod_jump_out_of_range", [{(2, 0): (n, 0, 0xB, 0x09)}, {}], [0, 1])
    mod("mod_jump_then_break", [{(2, 0): (n, 0, 0xB, 0x01), (2, 1): (n, 0, 0xD, 0x04)}, {}, {}], [0, 1, 2])
    mod("mod_break_then_jump", [{(2, 1): (n, 0, 0xB, 0x02), (2, 0): (n, 0, 0xD, 0x04)}, {}, {}], [0, 1, 2])
    mod("mod_jump_self", [{(4, 0): (n, 0, 0xB, 0x00)}], [0])
    mod("mod_break_loop_end", [{(2, 0): (n, 0, 0xD, 0x00)}], [0])
    mod("mod_loop", [{(2, 0): (n, 0, 0xE, 0x60), (5, 0): (n, 0, 0xE, 0x62)}], [0])
    mod("mod_loop_no_start", [{(3, 0): (n, 0, 0xE, 0x62)}], [0])
    mod("mod_loop_then_break", [{(1, 0): (n, 0, 0xE, 0x60), (4, 0): (n, 0, 0xE, 0x61), (4, 1): (n, 0, 0xD, 0x03)}, {}], [0, 1])
    mod("mod_loop_then_jump", [{(1, 0): (n, 0, 0xE, 0x60), (4, 0): (n, 0, 0xE, 0x61), (4, 1): (n, 0, 0xB, 0x01)}, {}, {}], [0, 2, 1])
    mod("mod_two_loops", [{(1, 0): (n, 0, 0xE, 0x60), (2, 1): (n, 0, 0xE, 0x60), (4, 0): (n, 0, 0xE, 0x61), (4, 1): (n, 0, 0xE, 0x61)}], [0])
    mod("mod_loop_big", [{(1, 0): (n, 0, 0xE, 0x60), (3, 0): (n, 0, 0xE, 0x6F)}], [0])
    mod("mod_loop_in_second", [{}, {(2, 0): (n, 0, 0xE, 0x60), (5, 0): (n, 0, 0xE, 0x63)}], [0, 1])
    mod("mod_three_orders", [{(63, 0): (n, 0, 0xD, 0x10)}, {(20, 0): (n, 0, 0xB, 0x02)}, {}], [0, 1, 2])
    s3m("s3m_break", [{(2, 0): E(S_C, 0x05)}, {}], [0, 1])
    s3m("s3m_break_12", [{(2, 0): E(S_C, 0x12)}, {}], [0, 1])
    s3m("s3m_break_0a", [{(2, 0): E(S_C, 0x0A)}, {}], [0, 1])
    s3m("s3m_break_past_end", [{(2, 0): E(S_C, 0x64)}, {}], [0, 1])
    s3m("s3m_jump", [{(2, 0): E(S_B, 1)}, {}, {}], [0, 2, 1])
    s3m("s3m_jump_then_break", [{(2, 0): E(S_B, 1), (2, 1): E(S_C, 0x04)}, {}, {}], [0, 1, 2])
    s3m("s3m_break_then_jump", [{(2, 1): E(S_B, 2), (2, 0): E(S_C, 0x04)}, {}, {}], [0, 1, 2])
    s3m("s3m_jump_out_of_range", [{(2, 0): E(S_B, 9)}, {}], [0, 1])
    s3m("s3m_loop", [{(2, 0): E(S_S, 0xB0), (5, 0): E(S_S, 0xB2)}], [0])
    s3m("s3m_loop_no_start", [{(3, 0): E(S_S, 0xB2)}], [0])
    s3m("s3m_loop_then_break", [{(1, 0): E(S_S, 0xB0), (4, 0): E(S_S, 0xB1), (4, 1): E(S_C, 0x03)}, {}], [0, 1])
    # (loops on two channels of an S3M are not compared: libopenmpt ends the song at the second pass, a quirk of its own)
    s3m("s3m_markers", [{(63, 0): E(S_C, 0x00)}, {}, {}], [0, 254, 1, 9, 255, 2])
    return s


def trace_rows(imp):
    ours = []
    for pid in imp.order:
        block, first, last, replay = imp.pieces[pid]
        for line in range(first, last + 1):
            if not ours or ours[-1] != (block, line):
                ours.append((block, line))
    return ours


def check_flow(c, omp, tool, wd):
    for name, m in flow_scenarios():
        path = write(wd, "flow_" + name, m)
        imp = run(tool, path)
        if not imp.ok:
            c.check(False, "flow_%s: import failed: %s" % (name, imp.text[:200]))
            continue
        ours = trace_rows(imp)
        mod, buf = omp.load(open(path, "rb").read())
        theirs = omp.trace(mod, len(ours))
        omp.free(mod)
        c.check(theirs[:len(ours)] == ours, "flow_%s: rows differ\n  importer: %s\n  OpenMPT:  %s" % (name, ours, theirs[:len(ours) + 2]))


# ---- timing ----------------------------------------------------------------------------------------------------------------------------


def plain_mod(cells, channels=4, tag="M.K.", orders=None):
    m = mb.Mod(tag, channels, "timing")
    m.samples[0] = mb.ModSample(mb.sine(32), "s", 64, 0, 0, 16)
    m.orders = orders or [0]
    m.patterns = [cells] if not isinstance(cells, list) else cells
    return m


def plain_s3m(cells, speed=6, tempo=125, orders=None):
    m = mb.S3m("timing")
    m.instruments = [mb.S3mInst(bytes(range(64)), "i")]
    m.orders = orders or [0]
    m.speed = speed
    m.tempo = tempo
    m.patterns = [cells] if not isinstance(cells, list) else cells
    return m


def timing_case(c, omp, tool, wd, name, module, tol=0.012):
    path = write(wd, "timing_" + name, module)
    imp = run(tool, path)
    if not imp.ok:
        c.check(False, "timing_%s: import failed: %s" % (name, imp.text[:200]))
        return
    mod, buf = omp.load(open(path, "rb").read())
    theirs = omp.duration(mod)
    omp.free(mod)
    ours = mo.our_duration(imp)
    rel = ours / theirs - 1.0
    c.check(abs(rel) <= tol, "timing_%s: the song takes %.3f s here, %.3f s in libopenmpt: %.2f %% apart" % (name, ours, theirs, rel * 100))


def check_timing(c, omp, tool, wd):
    n = None
    for sp in (3, 4, 6, 8, 12):
        timing_case(c, omp, tool, wd, "mod_speed%d" % sp, plain_mod({(0, 0): (n, 0, 0xF, sp)}))
        timing_case(c, omp, tool, wd, "mod_speed%d_mid" % sp, plain_mod({(20, 1): (n, 0, 0xF, sp)}))
    for bpm in (60, 100, 125, 140, 200, 255):
        timing_case(c, omp, tool, wd, "mod_tempo%d" % bpm, plain_mod({(0, 0): (n, 0, 0xF, bpm)}))
        timing_case(c, omp, tool, wd, "mod_tempo%d_then_speed" % bpm, plain_mod({(0, 0): (n, 0, 0xF, bpm), (10, 2): (n, 0, 0xF, 5)}))
    timing_case(c, omp, tool, wd, "mod_f00_ignored", plain_mod({(8, 0): (n, 0, 0xF, 0)}))
    for d in (1, 2, 3, 6):
        timing_case(c, omp, tool, wd, "mod_delay%d" % d, plain_mod({(4, 0): (n, 0, 0xE, 0xE0 | d)}))
    timing_case(c, omp, tool, wd, "mod_delay_last_wins", plain_mod({(4, 0): (n, 0, 0xE, 0xE5), (4, 1): (n, 0, 0xE, 0xE2)}))
    timing_case(c, omp, tool, wd, "mod_delay_last_wins2", plain_mod({(4, 0): (n, 0, 0xE, 0xE2), (4, 1): (n, 0, 0xE, 0xE5)}))
    timing_case(c, omp, tool, wd, "mod_delay_with_speed", plain_mod({(0, 0): (n, 0, 0xF, 3), (4, 0): (n, 0, 0xE, 0xE4)}))
    timing_case(c, omp, tool, wd, "mod_loop_time", plain_mod({(2, 0): (n, 0, 0xE, 0x60), (5, 0): (n, 0, 0xE, 0x63)}))
    timing_case(c, omp, tool, wd, "mod_two_patterns_tempo", plain_mod([{(0, 0): (n, 0, 0xF, 0x70)}, {(0, 0): (n, 0, 0xF, 6)}], orders=[0, 1]))
    for sp, tp in ((6, 125), (3, 125), (6, 100), (4, 150), (8, 200), (0, 125), (6, 255), (2, 80)):
        timing_case(c, omp, tool, wd, "s3m_start_%d_%d" % (sp, tp), plain_s3m({}, sp, tp))
    for tp in (20, 32):  # below 33 ST3 plays 32 (libopenmpt measured); 32 BPM at speed 6 is 32 BPM of ours: under the model's 40, so the importer clamps and says so
        path = write(wd, "timing_s3m_low_tempo_%d" % tp, plain_s3m({}, 6, tp))
        imp = run(tool, path)
        c.check(imp.ok and imp.project["tempo"] == 4000, "timing_s3m_low_tempo_%d: the importer's tempo is %s, the model's minimum 4000 expected" % (tp, imp.project.get("tempo")))
    for sp in (2, 3, 4, 5, 8, 12):
        timing_case(c, omp, tool, wd, "s3m_A%d" % sp, plain_s3m({(0, 0): E(S_A, sp)}))
        timing_case(c, omp, tool, wd, "s3m_A%d_mid" % sp, plain_s3m({(30, 1): E(S_A, sp)}))
    for bpm in (60, 100, 140, 200, 255):
        timing_case(c, omp, tool, wd, "s3m_T%d" % bpm, plain_s3m({(0, 0): E(S_T, bpm)}))
        timing_case(c, omp, tool, wd, "s3m_T%d_A" % bpm, plain_s3m({(0, 0): E(S_T, bpm), (10, 2): E(S_A, 5)}))
    timing_case(c, omp, tool, wd, "s3m_A0_ignored", plain_s3m({(5, 0): E(S_A, 0)}))
    timing_case(c, omp, tool, wd, "s3m_T10_ignored", plain_s3m({(5, 0): E(S_T, 0x10)}))
    for d in (1, 2, 3, 6):
        timing_case(c, omp, tool, wd, "s3m_delay%d" % d, plain_s3m({(4, 0): E(S_S, 0xE0 | d)}))
    timing_case(c, omp, tool, wd, "s3m_delay_first_wins", plain_s3m({(4, 0): E(S_S, 0xE5), (4, 1): E(S_S, 0xE2)}))
    timing_case(c, omp, tool, wd, "s3m_delay_first_wins2", plain_s3m({(4, 0): E(S_S, 0xE2), (4, 1): E(S_S, 0xE5)}))
    timing_case(c, omp, tool, wd, "s3m_loop_time", plain_s3m({(2, 0): E(S_S, 0xB0), (5, 0): E(S_S, 0xB3)}))


# ---- more than 16 channels ---------------------------------------------------------------------------------------------------------------


def check_channels(c, omp, tool, wd):
    for kind in ("mod", "s3m"):
        for name, cmds in (("tempo", [(4, 0x0F, 0x50)]), ("speed", [(4, 0x0F, 0x03)]), ("both", [(3, 0x0F, 0x28), (9, 0x0F, 0x04)])):
            if kind == "mod":
                m = mb.Mod("20CH", 20, "wide")
                m.samples[0] = mb.ModSample(mb.sine(32), "s", 64, 0, 0, 16)
                cells = {(line, t): (13 % 36, 1, 0, 0) for t in range(16) for line in range(16)}
                for line, cmd, data in cmds:
                    cells[(line, 19)] = (None, 0, cmd, data)
                m.orders = [0]
                m.patterns = [cells]
            else:
                m = mb.S3m("wide")
                m.instruments = [mb.S3mInst(bytes(range(64)), "i")]
                m.channels = list(range(8)) + list(range(8, 16)) + list(range(0, 8)) + [255] * 8
                cells = {(line, t): (0x40 + t % 12, 1, 255, 0, 0) for t in range(16) for line in range(16)}
                for line, cmd, data in cmds:
                    letter = S_T if cmd == 0x0F and data >= 0x20 else S_A
                    cells[(line, 19)] = (255, 0, 255, letter, data)
                m.orders = [0]
                m.patterns = [cells]
            path = write(wd, "wide_%s_%s" % (kind, name), m)
            imp = run(tool, path)
            if not imp.ok:
                c.check(False, "wide_%s_%s: import failed: %s" % (kind, name, imp.text[:200]))
                continue
            c.check(int(imp.report.get("timing_carried", "0")) >= len(cmds), "wide_%s_%s: nothing was carried: %s" % (kind, name, imp.report))
            c.check(int(imp.report.get("timing_lost", "1")) == 0, "wide_%s_%s: time was lost" % (kind, name))
            c.check(int(imp.report.get("dropped", "0")) == 1, "wide_%s_%s: expected 1 dropped channel, report %s" % (kind, name, imp.report))
            mod, buf = omp.load(open(path, "rb").read())
            theirs = omp.duration(mod)
            omp.free(mod)
            ours = mo.our_duration(imp)
            rel = ours / theirs - 1.0
            c.check(abs(rel) <= 0.02, "wide_%s_%s: the song takes %.3f s here, %.3f s in libopenmpt: %.2f %% apart" % (kind, name, ours, theirs, rel * 100))


# ---- volume ------------------------------------------------------------------------------------------------------------------------------


def check_volume(c, omp, tool, wd):
    for kind in ("mod", "s3m"):
        rows = [(13, 1, None), (13, 1, 0x20), (15, 0, None), (13, 1, None), (15, 0, None), (13, 2, None), (15, 0, None), (13, 1, 0x00), (15, 0, None), (13, 2, 0x30)]
        if kind == "mod":
            m = mb.Mod("M.K.", 4, "volume")
            m.samples[0] = mb.ModSample(mb.sine(64), "full", 64, 0, 0, 32)
            m.samples[1] = mb.ModSample(mb.sine(64), "half", 32, 0, 0, 32)
            cells = {}
            for line, (note, inst, vol) in enumerate(rows):
                cells[(line, 0)] = (note % 36, inst, 0xC if vol is not None else 0, vol if vol is not None else 0)
            m.orders = [0]
            m.patterns = [cells]
            m.restart = 127
        else:
            m = mb.S3m("volume")
            m.instruments = [mb.S3mInst(sine_u8(64), "full", 22050, 64, (0, 64)), mb.S3mInst(sine_u8(64), "half", 22050, 32, (0, 64))]
            cells = {}
            for line, (note, inst, vol) in enumerate(rows):
                cells[(line, 0)] = (0x40 + (note - 13) % 12 if note else 255, inst, vol if vol is not None else 255, 0, 0)
            m.orders = [0]
            m.patterns = [cells]
        svol = {1: 64, 2: 32}
        path = write(wd, "volume_%s" % kind, m)
        imp = run(tool, path)
        if not imp.ok:
            c.check(False, "volume_%s: import failed: %s" % (kind, imp.text[:200]))
            continue
        slots = tracks_of(imp)
        c.check(int(imp.report.get("volume_baked", "0")) >= 2, "volume_%s: the importer baked %s volumes" % (kind, imp.report.get("volume_baked")))
        rate = 22050
        mod, buf = omp.load(open(path, "rb").read())
        samples = omp.render(mod, rate, int(rate * 0.12 * len(rows)))
        omp.free(mod)
        per_row = int(rate * 0.12)

        def rms(row):
            w = samples[row * per_row + int(per_row * 0.3):row * per_row + int(per_row * 0.9)]
            return math.sqrt(sum(x * x for x in w) / len(w)) if w else 0.0

        ref = rms(0)
        c.check(ref > 0.01, "volume_%s: libopenmpt rendered nothing at row 0" % kind)
        inst = 0
        for line, (note, i, vol) in enumerate(rows):
            if i:
                inst = i
            st = imp.steps.get((1, slots.get(0), line))
            col = st["vol"] if st else 255
            expected = ((col + 1) >> 1) if col != 255 else svol[inst]
            have = rms(line) / ref * 64.0 if ref > 0 else 0.0
            c.check(abs(have - expected) <= 5.0, "volume_%s: row %d plays at %.1f of 64 in libopenmpt, the importer's step says %d (column %d, instrument %d)"
                    % (kind, line, have, expected, col, inst))


def check_volume_slides(c, omp, tool, wd):
    """OS-COREFX: a bare note keeps the volume the channel ended at, after a volume slide (5 ticks of x a tick at speed 6) or a fine one: OpenMPT's loudness on each note row against the volume
    the importer bakes into the step (the importer's follow of the volume through a slide is a count of ticks; OpenMPT runs them). The rows that carry the slide are not measured (a ramp)."""
    # (row kind, argument): 'n' a note with an instrument, 'b' a bare note, 's' a slide (MOD Axy / S3M Dxy), 'f' a fine slide (MOD EAx EBx / S3M DxF DFx)
    spec = [("n", 1), ("s", 0x04), ("b", 0), ("f", ("dn", 8)), ("b", 0), ("s", 0x50), ("b", 0), ("s", 0xF0), ("b", 0), ("n", 2), ("s", 0x03), ("b", 0), ("f", ("up", 4)), ("b", 0)]
    for kind in ("mod", "s3m"):
        if kind == "mod":
            m = mb.Mod("M.K.", 4, "slides")
            m.samples[0] = mb.ModSample(mb.sine(64), "full", 64, 0, 0, 32)
            m.samples[1] = mb.ModSample(mb.sine(64), "half", 32, 0, 0, 32)
        else:
            m = mb.S3m("slides")
            m.instruments = [mb.S3mInst(sine_u8(64), "full", 22050, 64, (0, 64)), mb.S3mInst(sine_u8(64), "half", 22050, 32, (0, 64))]
        cells = {}
        for line, (what, arg) in enumerate(spec):
            note = 13 + (line % 5)
            if what in ("n", "b"):
                inst = arg if what == "n" else 0
                cells[(line, 0)] = (note % 36, inst, 0, 0) if kind == "mod" else (0x40 + (note - 13) % 12, inst, 255, 0, 0)
            elif what == "s":
                cells[(line, 0)] = (None, 0, 0xA, arg) if kind == "mod" else (255, 0, 255, 4, arg)
            else:
                d, x = arg
                if kind == "mod":
                    cells[(line, 0)] = (None, 0, 0xE, (0xB0 if d == "dn" else 0xA0) | x)
                else:
                    cells[(line, 0)] = (255, 0, 255, 4, (0xF0 | x) if d == "dn" else ((x << 4) | 0xF))
        m.orders = [0]
        m.patterns = [cells]
        if kind == "mod":
            m.restart = 127
        path = write(wd, "slides_%s" % kind, m)
        imp = run(tool, path, ("noengine",) if kind == "mod" else ())
        if not imp.ok:
            c.check(False, "slides_%s: import failed: %s" % (kind, imp.text[:200]))
            continue
        slots = tracks_of(imp)
        rate = 22050
        mod, buf = omp.load(open(path, "rb").read())
        per_row = int(rate * 0.12)
        samples = omp.render(mod, rate, per_row * len(spec))
        omp.free(mod)

        def rms(row):
            w = samples[row * per_row + int(per_row * 0.3):row * per_row + int(per_row * 0.9)]
            return math.sqrt(sum(x * x for x in w) / len(w)) if w else 0.0

        ref = rms(0)
        c.check(ref > 0.01, "slides_%s: libopenmpt rendered nothing at row 0" % kind)
        svol = {1: 64, 2: 32}
        inst = 0
        vol = None
        for line, (what, arg) in enumerate(spec):
            if what == "n":
                inst = arg
            if what not in ("n", "b"):
                continue
            st = imp.steps.get((1, slots.get(0), line))
            col = st["vol"] if st else 255
            expected = ((col + 1) >> 1) if col != 255 else svol[inst]
            have = rms(line) / ref * 64.0 if ref > 0 else 0.0
            c.check(abs(have - expected) <= 3.0, "slides_%s: row %d (%s) plays at %.1f of 64 in libopenmpt, the importer's step says %d (column %d, instrument %d)" % (kind, line, what, have, expected, col, inst))
        # the effects themselves reached the steps (the slide rows carry the tracker FX; the fine rows carry FX 48)
        for line, (what, arg) in enumerate(spec):
            if what not in ("s", "f"):
                continue
            st = imp.steps.get((1, slots.get(0), line))
            types = [f[0] for f in st["fx"]] if st else []
            c.check((47 if what == "s" else 48) in types, "slides_%s: row %d (%s): FX %s" % (kind, line, what, types))


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    tool, workdir = sys.argv[1], sys.argv[2]
    lib = mo.load_library()
    if lib is None:
        if STRICT:
            print("modimp_openmpt_oracle: libopenmpt is not installed, and O3_CI=1 says this test may not be skipped (apt: libopenmpt0t64)")
            return 1
        print("modimp_openmpt_oracle: libopenmpt is not installed here, skipped")
        return SKIP
    os.makedirs(workdir, exist_ok=True)
    omp = Omp(lib)
    version = omp.version()
    print("libopenmpt %d.%d.%d" % version)
    if version[:2] < MIN_VERSION:
        msg = "libopenmpt %d.%d.%d is older than %d.%d, the version the comparisons were measured on" % (version + MIN_VERSION)
        if STRICT:
            print("modimp_openmpt_oracle: " + msg + ", and O3_CI=1 says this test may not be skipped")
            return 1
        print("modimp_openmpt_oracle: " + msg + ", skipped")
        return SKIP
    c = Checks()
    for fn in (check_decode, check_core, check_volume_slides, check_pitch, check_loops, check_flow, check_timing, check_channels, check_volume):
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
