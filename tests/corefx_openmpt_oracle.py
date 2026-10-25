#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The tracker FX of the Modern engine against libopenmpt, in the audio (OS-COREFX CF1, docs/specs/OS-COREFX.md, ADR 0004 amendment 2).

  corefx_openmpt_oracle.py TOOL WORKDIR

TOOL is `o3corefx_tool`. Each case is a synthetic module (tools/mod_build.py: nothing here is a real module and nothing is committed) with a looped sine and one note on a channel, followed by rows with
one effect each. The module is rendered twice: by libopenmpt (loaded with ctypes from the system), and by the OS: the importer, the sequencer, the Modern engine's tracker FX processor, the sampler
voices and the graph (`o3corefx_tool render`). For every TICK of the song (a tick is 20 ms at the speed 6 and 125 BPM of both formats) the script measures the pitch (the zero crossings of the sine)
and the level (the RMS), relative to the note's own tick, and compares the two:

  arpeggio             S3M Jxy, MOD 0xy: the note, +x, +y on the ticks of a row, the note again on the row after
  volume slide         S3M Dxy, MOD Axy: x up else y down from tick 1, held at 0 and 64, kept on the rows after
  fine volume slide    S3M DxF DFx, MOD EAx EBx: once, on tick 0
  fine portamento      S3M FFx EFx (x 4 quarter units), FEx EEx (x quarter units), MOD E1x E2x (4 x): once, on tick 0, kept on the rows after, over from a new note
  combined             a slide chain, then a new note over the slid pitch and level
  parameter memory     (CF2) S3M J D K L E F Q I P with a zero parameter, which repeat the channel's last non-zero one: J37 J00, D04 D00, K30 D00, E04 F00 (the memory of E and F is one), EF4 E00, Q23 Q00, I21 I00, P04 P00
  retrigger            (CF2) S3M Qxy on a decaying one-shot: every y ticks, the count running across the rows of a run, the volume code x at each restart (the level of the restarts is the comparison)
  tremor               (CF2) S3M Ixy: x + 1 ticks on, y + 1 off, the position kept across the rows
  pan slide            (CF2) S3M Pxy after S88: y right else x left 4/256 a tick, the fine forms, the ends; the pan is measured in the two channels of the output (R / (L + R) in libopenmpt, the graph's law here)
Not compared (nothing in libopenmpt to compare with): the waveforms (our vibrato and tremolo are the LFOs on the transport, not the tracker's phase) and the glissando (our tone portamento is the Glide FX).

The pitch tolerance is 0.6 cents (the smallest fine slide is 1.0), except where an arpeggio's semitones are compared: libopenmpt's S3M and ProTracker note tables are not equal tempered (a fifth is 4 cents wide) and the sampler's
are, so those are compared to 6 cents. The level tolerance is 4 % of the note's, which is the step of the velocity (1/127) plus the ramp at a tick's edge. Without libopenmpt the script exits 77 (a
skip), and with O3_CI=1 in the environment (tools/ci.sh) a missing or too old library, or fewer than MIN_COMPARISONS comparisons, is a failure.
"""
import array
import math
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import med_openmpt_oracle as mo  # noqa: E402  (its libopenmpt binding)
import mod_build as mb  # noqa: E402

SKIP = 77
MIN_VERSION = (0, 7)
MIN_COMPARISONS = 4000
STRICT = os.environ.get("O3_CI") == "1"
TICK = 0.02
SPEED = 6


def sine_u8(n):
    return bytes((128 + v) & 0xFF for v in mb.sine(n, 80))


def s3m_module(cells, vol=64, rows=8):
    m = mb.S3m("corefx")
    m.instruments = [mb.S3mInst(sine_u8(16), "sine", 8363, 64, (0, 16))]
    m.orders = [0, 255]
    m.channels = [0, 8, 1, 9] + [255] * 28
    pat = {(0, 0): (0x40, 1, vol, 0, 0)}
    for row, (cmd, info) in cells.items():
        pat[(row, 0)] = (255, 0, 255, cmd, info)
    m.patterns = [pat]
    return m


def mod_module(cells, vol=0x40, rows=8):
    m = mb.Mod("M.K.", 4, "corefx")
    m.samples = [mb.ModSample(mb.sine(16, 80), "sine", 64, 0, 0, 8)] + [mb.ModSample() for _ in range(30)]
    m.orders = [0]
    pat = {(0, 0): (12, 1, 0xC, vol)}
    for row, (eff, par) in cells.items():
        pat[(row, 0)] = (None, 0, eff, par)
    m.patterns = [pat]
    return m


def freq_rms(s, rate):
    ups = []
    for i in range(1, len(s)):
        if s[i - 1] < 0 <= s[i]:
            ups.append(i - 1 + (-s[i - 1]) / (s[i] - s[i - 1]))
    f = (len(ups) - 1) * rate / (ups[-1] - ups[0]) if len(ups) >= 3 else 0.0
    return f


def per_tick(samples, rate, ticks):
    """[(frequency, rms)] for each tick of the song."""
    out = []
    for t in range(ticks):
        a = int(round(t * TICK * rate))
        b = int(round((t + 1) * TICK * rate))
        w = samples[a + 60:b - 60]
        w2 = samples[a + 250:b - 20]
        rms = math.sqrt(sum(x * x for x in w2) / len(w2)) if w2 else 0.0
        out.append((freq_rms(w, rate), rms))
    return out


def theirs(omp, data, ticks, rate=44100):
    mod, buf = omp.load(data)
    samples = omp.render(mod, rate, int(round(ticks * TICK * rate)) + 10)
    omp.free(mod)
    return per_tick(samples, rate, ticks)


def per_tick_lr(l, r, rate, ticks):
    """[(left rms, right rms)] for each tick."""
    out = []
    for t in range(ticks):
        a = int(round(t * TICK * rate))
        b = int(round((t + 1) * TICK * rate))
        wl, wr = l[a + 250:b - 20], r[a + 250:b - 20]
        out.append((math.sqrt(sum(x * x for x in wl) / len(wl)) if wl else 0.0, math.sqrt(sum(x * x for x in wr) / len(wr)) if wr else 0.0))
    return out


def theirs_lr(omp, data, ticks, rate=44100):
    import ctypes
    mod, buf = omp.load(data)
    fn = omp.lib.openmpt_module_read_interleaved_float_stereo
    fn.restype = ctypes.c_size_t
    fn.argtypes = [ctypes.c_void_p, ctypes.c_int32, ctypes.c_size_t, ctypes.POINTER(ctypes.c_float)]
    frames = int(round(ticks * TICK * rate)) + 10
    out = (ctypes.c_float * (frames * 2))()
    n = fn(mod, rate, frames, out)
    omp.free(mod)
    v = list(out[:2 * n])
    return per_tick_lr(v[0::2], v[1::2], rate, ticks)


def ours_lr(tool, path, wd, ticks):
    raw = os.path.join(wd, os.path.basename(path) + ".lr.raw")
    r = subprocess.run([tool, "render", path, raw, str(ticks)], capture_output=True, text=True, timeout=120)
    if r.returncode != 0 or "render ok" not in r.stdout:
        return None, (r.stdout + r.stderr)[:300]
    rate = int(r.stdout.split("rate=")[1].split()[0])
    a = array.array("h")
    with open(raw, "rb") as f:
        a.frombytes(f.read())
    return per_tick_lr([float(x) for x in a[0::2]], [float(x) for x in a[1::2]], rate, ticks), ""


def pos_omp(l, r):
    """The pan position 0..100 libopenmpt's linear law gives: R / (L + R)."""
    return 100.0 * r / (l + r) if l + r > 0 else None


def pos_ours(l, r):
    """The same from the graph's law (smoothing.h: left 1 up to the centre then (100 - p) / 50, right p / 50 then 1): p = 50 R / L below the centre, 100 - 50 L / R above."""
    if l <= 0 and r <= 0:
        return None
    if r <= l:
        return 50.0 * r / l if l > 0 else 0.0
    return 100.0 - 50.0 * l / r


def ours(tool, path, wd, ticks, noengine):
    raw = os.path.join(wd, os.path.basename(path) + ".raw")
    args = [tool, "render", path, raw, str(ticks)] + (["noengine"] if noengine else [])
    r = subprocess.run(args, capture_output=True, text=True, timeout=120)
    if r.returncode != 0 or "render ok" not in r.stdout:
        return None, (r.stdout + r.stderr)[:300]
    rate = int(r.stdout.split("rate=")[1].split()[0])
    a = array.array("h")
    with open(raw, "rb") as f:
        a.frombytes(f.read())
    mono = [(a[2 * i] + a[2 * i + 1]) / 2.0 for i in range(len(a) // 2)]
    return per_tick(mono, rate, ticks), ""


def cents(a, b):
    return 1200.0 * math.log2(a / b)


class Checks(mo.Checks):
    pass


def run_case(c, omp, tool, wd, name, module, rows, pitch_tol=0.6, noengine=False, level_tol=0.04):
    ticks = rows * SPEED
    path = os.path.join(wd, name + (".mod" if isinstance(module, mb.Mod) else ".s3m"))
    data = mb.build(module)
    with open(path, "wb") as f:
        f.write(data)
    want = theirs(omp, data, ticks)
    have, why = ours(tool, path, wd, ticks, noengine)
    c.check(have is not None, "%s: the tool failed: %s" % (name, why))
    if have is None:
        return
    # the reference: the note's own ticks 1..5 of row 0
    wf = sum(f for f, _ in want[1:SPEED]) / (SPEED - 1)
    wr = sum(r for _, r in want[1:SPEED]) / (SPEED - 1)
    hf = sum(f for f, _ in have[1:SPEED]) / (SPEED - 1)
    hr = sum(r for _, r in have[1:SPEED]) / (SPEED - 1)
    c.check(wf > 0 and hf > 0 and wr > 0 and hr > 0, "%s: no sound in the reference ticks (libopenmpt %.1f Hz %.3f, ours %.1f Hz %.3f)" % (name, wf, wr, hf, hr))
    if not (wf > 0 and hf > 0 and wr > 0 and hr > 0):
        return
    for t in range(SPEED, ticks):
        (f1, r1), (f2, r2) = want[t], have[t]
        if r1 / wr > 0.02 or r2 / hr > 0.02:  # a silent tick has no pitch
            c.check(f1 > 0 and f2 > 0, "%s: tick %d has no pitch (libopenmpt %.1f, ours %.1f)" % (name, t, f1, f2))
            if f1 > 0 and f2 > 0:
                d = cents(f2 / hf, f1 / wf)
                c.check(abs(d) <= pitch_tol, "%s: row %d tick %d: the pitch is %.2f cents off the note in libopenmpt (%.2f) and %.2f here: %.2f cents apart"
                        % (name, t // SPEED, t % SPEED, cents(f1 / wf, 1.0), cents(f1 / wf, 1.0), cents(f2 / hf, 1.0), d))
        a, b = r1 / wr, r2 / hr
        # libopenmpt's mixer ramps a level change across the tick it begins in (measured with a ramp of 0 samples set too), so a tick that changes the level is compared with the step as slack;
        # a tick that does not (the rows after a slide, the held ends) is compared to the level tolerance, which is where the total of a slide is held to the tracker's
        prev = want[t - 1][1] / wr
        slack = 0.6 * abs(a - prev)
        c.check(abs(a - b) <= level_tol + 0.02 * a + slack, "%s: row %d tick %d: the level is %.3f of the note's in libopenmpt and %.3f here" % (name, t // SPEED, t % SPEED, a, b))


def decay_u8(n=60000):
    """A one shot that decays by half every 15000 frames, 16 frames a period (a restart shows as the level jumping back). It is played at 44,100 Hz (c2spd below): the chain plays a sample at the
    output rate, as libopenmpt does at c2spd = the output rate, so that the two decay together."""
    return bytes((128 + int(round(100 * (0.5 ** (i / 15000.0)) * math.sin(2 * math.pi * i / 16)))) & 0xFF for i in range(n))


def s3m_decay(cells, vol=64):
    m = mb.S3m("corefx")
    m.instruments = [mb.S3mInst(decay_u8(), "decay", 44100, 64, None)]
    m.orders = [0, 255]
    m.channels = [0, 8, 1, 9] + [255] * 28
    pat = {(0, 0): (0x40, 1, vol, 0, 0)}
    for row, (cmd, info) in cells.items():
        pat[(row, 0)] = (255, 0, 255, cmd, info)
    m.patterns = [pat]
    return m


def run_pan_case(c, omp, tool, wd, name, module, rows, tol=3.0, next_slack=0.0):
    ticks = rows * SPEED
    path = os.path.join(wd, name + ".s3m")
    data = mb.build(module)
    with open(path, "wb") as f:
        f.write(data)
    want = theirs_lr(omp, data, ticks)
    have, why = ours_lr(tool, path, wd, ticks)
    c.check(have is not None, "%s: the tool failed: %s" % (name, why))
    if have is None:
        return
    for t in range(SPEED, ticks):
        a = pos_omp(*want[t])
        b = pos_ours(*have[t])
        c.check(a is not None and b is not None, "%s: tick %d is silent (libopenmpt %s, ours %s)" % (name, t, want[t], have[t]))
        if a is None or b is None:
            continue
        # a tick that moves the pan is ramped across the tick by both mixers: the step is slack
        pa = pos_omp(*want[t - 1]) or a
        slack = 0.6 * abs(a - pa)
        if next_slack and t + 1 < ticks:
            # the graph recomputes the pan once per 64-sample block, so a step that comes at the next tick is already under way in the last (up to 43) samples of the window this tick is measured in
            pn = pos_omp(*want[t + 1])
            slack += next_slack * (abs(pn - a) if pn is not None else 0.0)
        c.check(abs(a - b) <= tol + slack, "%s: row %d tick %d: the pan is %.1f in libopenmpt and %.1f here" % (name, t // SPEED, t % SPEED, a, b))


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    tool, wd = sys.argv[1], sys.argv[2]
    lib = mo.load_library()
    if lib is None:
        if STRICT:
            print("corefx_openmpt_oracle: libopenmpt is not installed, and O3_CI=1 says this test may not be skipped (apt: libopenmpt0t64)")
            return 1
        print("corefx_openmpt_oracle: libopenmpt is not installed here, skipped")
        return SKIP
    os.makedirs(wd, exist_ok=True)
    import modimp_openmpt_oracle as mi  # noqa: E402
    omp = mi.Omp(lib)
    v = omp.version()
    print("libopenmpt %d.%d.%d" % v)
    if v[:2] < MIN_VERSION:
        msg = "libopenmpt %d.%d.%d is older than %d.%d" % (v + MIN_VERSION)
        if STRICT:
            print("corefx_openmpt_oracle: " + msg + ", and O3_CI=1 says this test may not be skipped")
            return 1
        print("corefx_openmpt_oracle: " + msg + ", skipped")
        return SKIP
    c = Checks()
    n = 0

    def case(name, module, rows, **kw):
        nonlocal n
        before, bad = c.count, len(c.failures)
        run_case(c, omp, tool, wd, name, module, rows, **kw)
        print("%s: %d comparisons%s" % (name, c.count - before, "" if len(c.failures) == bad else ", %d DISAGREE" % (len(c.failures) - bad)))

    # arpeggio (S3M Jxy, MOD 0xy): 6 cents for the tables
    for p in (0x37, 0x0C, 0x4F, 0x05):
        case("arp_s3m_%02x" % p, s3m_module({1: (10, p), 3: (10, p)}), 5, pitch_tol=6.0)
    case("arp_mod_37", mod_module({1: (0, 0x37)}), 4, pitch_tol=6.0, noengine=True)
    case("arp_mod_47_rows", mod_module({1: (0, 0x47), 2: (0, 0x47), 3: (0, 0x4F)}), 5, pitch_tol=6.0, noengine=True)
    # volume slides: down, up, both nibbles (x wins in MOD; S3M leaves it undefined: not compared), held at the ends, kept after the row
    for p in (0x04, 0x01, 0x0E):  # (D0F is a fine form in Scream Tracker's table and libopenmpt runs it on tick 0: not compared)
        case("vslide_s3m_down_%02x" % p, s3m_module({1: (4, p), 3: (4, p)}), 6)
    for p in (0x30, 0x10, 0x50):
        case("vslide_s3m_up_%02x" % p, s3m_module({1: (4, p)}, vol=20), 5)
    case("vslide_mod_down", mod_module({1: (0xA, 0x03), 3: (0xA, 0x02)}), 5, noengine=True)
    case("vslide_mod_up", mod_module({1: (0xA, 0x40), 2: (0xA, 0x20)}, vol=0x10), 5, noengine=True)
    case("vslide_mod_x_wins", mod_module({1: (0xA, 0x32)}), 4, noengine=True)
    case("vslide_mod_to_zero", mod_module({1: (0xA, 0x0F), 2: (0xA, 0x0F)}), 4, noengine=True)
    # fine volume slides
    for p in (0xF4, 0xF1, 0xFF):
        case("fvol_s3m_down_%02x" % p, s3m_module({1: (4, p), 2: (4, p)}, vol=30), 4)
    for p in (0x4F, 0x1F, 0x9F):
        case("fvol_s3m_up_%02x" % p, s3m_module({1: (4, p), 2: (4, p)}, vol=20), 4)
    case("fvol_mod", mod_module({1: (0xE, 0xB8), 2: (0xE, 0xA3), 3: (0xE, 0xBF)}, vol=0x20), 5, noengine=True)
    # fine portamento: S3M FFx EFx (x 4 quarter units), FEx EEx (x quarter units); MOD E1x E2x (4 x)
    for p in (0xF1, 0xF4, 0xFF, 0xE1, 0xE4, 0xEF):
        case("fporta_s3m_up_%02x" % p, s3m_module({1: (6, p), 2: (6, p)}), 4)
        case("fporta_s3m_down_%02x" % p, s3m_module({1: (5, p), 2: (5, p)}), 4)
    for x in (1, 4, 0xF):
        case("fporta_mod_up_%x" % x, mod_module({1: (0xE, 0x10 | x), 2: (0xE, 0x10 | x)}), 4, noengine=True)
        case("fporta_mod_down_%x" % x, mod_module({1: (0xE, 0x20 | x), 2: (0xE, 0x20 | x)}), 4, noengine=True)
    # combined: a slid pitch stays until a new note, and the slid volume too (the arpeggio after a fine slide is not compared: Scream Tracker's arpeggio plays from the note, not from the slid period,
    # which libopenmpt follows; ours plays from the slid period, as ProTracker's)
    m = s3m_module({1: (6, 0xFF), 3: (4, 0x04), 4: (5, 0xF8)}, vol=60)
    m.patterns[0][(2, 0)] = (0x40, 1, 64, 0, 0)  # a new note on row 2: the slid pitch and the slid volume start over
    case("renote_s3m", m, 6)
    # ---- CF2 -------------------------------------------------------------------------------------------------------------------------------------------------------------------
    # parameter memory: the importer resolves a zero parameter to the channel's last one of its class (D K L share one, E F share one, the others have their own), as libopenmpt plays it
    case("mem_j", s3m_module({1: (10, 0x37), 2: (10, 0x00), 3: (10, 0x00)}), 5, pitch_tol=6.0)
    case("mem_d_down", s3m_module({1: (4, 0x04), 2: (4, 0x00), 3: (4, 0x00)}), 6)
    case("mem_d_up", s3m_module({1: (4, 0x30), 2: (4, 0x00)}, vol=10), 5)
    # (K and L carry a volume slide of their own that the Modern engine does not run, only the memory they share with D is resolved: tested in modimp_import_test.cpp)
    case("mem_d_fine", s3m_module({1: (4, 0xF4), 2: (4, 0x00)}, vol=40), 4)
    # (the plain E and F are the Slide FX, whose unit is not confirmed against the OG: only the fine forms are compared)
    case("mem_fine_porta", s3m_module({1: (5, 0xF4), 2: (5, 0x00), 3: (6, 0x00)}), 5)
    case("mem_fine_porta_f_e", s3m_module({1: (6, 0xF3), 2: (5, 0x00), 3: (5, 0x00)}), 5)
    case("mem_fine_porta_e_e", s3m_module({1: (5, 0xF5), 2: (5, 0x00), 3: (5, 0x00)}), 5)
    case("mem_extra_fine", s3m_module({1: (6, 0xE3), 2: (6, 0x00)}), 4)
    # retrigger Qxy: the restarts and the volume codes; the count runs across the rows of a run and the first tick of a run only starts it
    for name, cells, rows, vol in (("q03", {1: (17, 0x03)}, 4, 64), ("q03_rows", {1: (17, 0x03), 2: (17, 0x03), 3: (17, 0x03)}, 5, 64), ("q04_rows", {1: (17, 0x04), 2: (17, 0x04), 3: (17, 0x04)}, 5, 64),
                                   ("q05_rows", {1: (17, 0x05), 2: (17, 0x05)}, 4, 64), ("q01", {1: (17, 0x01)}, 3, 64), ("q02_rows", {1: (17, 0x02), 2: (17, 0x02)}, 4, 64),
                                   ("q23", {1: (17, 0x23)}, 4, 64), ("q43", {1: (17, 0x43)}, 4, 64), ("q53", {1: (17, 0x53)}, 4, 64), ("q61", {1: (17, 0x61)}, 3, 64), ("q71", {1: (17, 0x71)}, 3, 64),
                                   ("q93", {1: (17, 0x93)}, 4, 30), ("qa3", {1: (17, 0xA3)}, 4, 30), ("qb3", {1: (17, 0xB3)}, 4, 30), ("qc3", {1: (17, 0xC3)}, 4, 20), ("qd3", {1: (17, 0xD3)}, 4, 20),
                                   ("qe2", {1: (17, 0xE2)}, 4, 20), ("qf2", {1: (17, 0xF2)}, 4, 10), ("q80", {1: (17, 0x83)}, 4, 40), ("q03_mem", {1: (17, 0x23), 2: (17, 0x00)}, 4, 64),
                                   ("q03_gap", {1: (17, 0x03), 3: (17, 0x03)}, 5, 64)):
        case("retrig_" + name, s3m_decay(cells, vol), rows)
    # a new note in the middle of a run: the count starts over with the note, and the volume is the new note's
    m = s3m_decay({1: (17, 0x03), 2: (17, 0x03)})
    m.patterns[0][(3, 0)] = (0x40, 1, 64, 17, 0x03)
    case("retrig_renote", m, 6)
    m = s3m_decay({1: (17, 0x52), 2: (17, 0x52)}, vol=64)
    m.patterns[0][(4, 0)] = (0x40, 1, 40, 17, 0x52)
    case("retrig_renote_vol", m, 6)
    # tremor Ixy: x + 1 on, y + 1 off, the position counted on the ticks of the rows that carry it and kept across the rows that do not
    for name, cells, rows in (("i11_rows", {1: (9, 0x11), 2: (9, 0x11), 3: (9, 0x11)}, 5), ("i21_rows", {1: (9, 0x21), 2: (9, 0x21), 3: (9, 0x21)}, 5), ("i12_rows", {1: (9, 0x12), 2: (9, 0x12)}, 4),
                              ("i03", {1: (9, 0x03), 2: (9, 0x03)}, 4), ("i30", {1: (9, 0x30), 2: (9, 0x30)}, 4), ("i00", {1: (9, 0x00), 2: (9, 0x00)}, 4), ("i11_gap", {1: (9, 0x11), 3: (9, 0x11)}, 5),
                              ("i23_mem", {1: (9, 0x23), 2: (9, 0x00)}, 5), ("i11_one", {1: (9, 0x11)}, 3)):
        case("tremor_" + name, s3m_module(cells), rows)
    # pan slide Pxy from S88 (pan 136 of 255): y right else x left, the fine forms once, the ends
    def pan(name, cells, rows, **kw):
        before, bad = c.count, len(c.failures)
        base = {1: (19, 0x88)}
        base.update(cells)
        run_pan_case(c, omp, tool, wd, "pan_" + name, s3m_module(base), rows, **kw)
        print("pan_%s: %d comparisons%s" % (name, c.count - before, "" if len(c.failures) == bad else ", %d DISAGREE" % (len(c.failures) - bad)))
    pan("p04", {2: (16, 0x04), 3: (16, 0x04)}, 5)
    pan("p40", {2: (16, 0x40), 3: (16, 0x40)}, 5)
    pan("p44", {2: (16, 0x44)}, 4)
    pan("p01", {2: (16, 0x01), 3: (16, 0x01)}, 5)
    pan("pf4", {2: (16, 0xF4), 3: (16, 0xF4)}, 5)
    pan("p4f", {2: (16, 0x4F), 3: (16, 0x4F)}, 5)
    pan("p0f", {2: (16, 0x0F)}, 4)
    pan("pf0", {2: (16, 0xF0)}, 4)
    pan("pff", {2: (16, 0xFF), 3: (16, 0xFF)}, 5)
    pan("p04_end", {2: (16, 0x04), 3: (16, 0x04), 4: (16, 0x04), 5: (16, 0x04), 6: (16, 0x04), 7: (16, 0x04), 8: (16, 0x04)}, 10)
    # S3M Xxx is 0..0x80 (0x40 the centre): the stereo position of the output (XA4, the surround, is not compared: libopenmpt plays it with the right channel inverted)
    for x in (0x00, 0x20, 0x40, 0x60, 0x80):
        pan("x%02x" % x, {1: (24, x)}, 3)
    pan("p04_mem", {2: (16, 0x04), 3: (16, 0x00), 4: (16, 0x00)}, 6)
    pan("p40_mem", {2: (16, 0x20), 3: (16, 0x00)}, 5)
    pan("p_only_mem", {2: (16, 0x00), 3: (16, 0x04), 4: (16, 0x00)}, 6)
    pan("p04_gap", {2: (16, 0x04), 4: (16, 0x04)}, 6)
    # a pan that was slid outlives the notes after it (S3M has no pan in the instrument): the next note plays where the slide left it
    m = s3m_module({1: (19, 0x88), 2: (16, 0x04), 3: (16, 0x04)})
    m.patterns[0][(5, 0)] = (0x40, 1, 64, 0, 0)
    before, bad = c.count, len(c.failures)
    run_pan_case(c, omp, tool, wd, "pan_renote", m, 7)
    print("pan_renote: %d comparisons%s" % (c.count - before, "" if len(c.failures) == bad else ", %d DISAGREE" % (len(c.failures) - bad)))
    # a second pan set (S84, not the same value: the Panning FX is a held FX, so an S88 repeated after a slide is not a change and is not sent again; an open limit, docs/specs/OS-COREFX.md section 5)
    pan("p04_then_s8", {2: (16, 0x04), 3: (19, 0x84), 4: (16, 0x00)}, 6)
    # ---- CF3 ---------------------------------------------------------------------------------------------------------------------------------------------------------------
    # channel volume Mxx (0..64, held over notes), the slide Nxy (libopenmpt's: y down first, xF fine up, Fy fine down, once on tick 0), global volume Vxx and the slide Wxy (x up first)
    for name, cells, rows in (("m20", {1: (13, 0x20)}, 4), ("m40", {1: (13, 0x40)}, 4), ("m10", {1: (13, 0x10), 3: (13, 0x30)}, 5), ("m00", {1: (13, 0x00), 3: (13, 0x20)}, 5),
                              ("m41_ignored", {1: (13, 0x20), 2: (13, 0x41)}, 4)):
        case("chvol_" + name, s3m_module(cells), rows)
    m = s3m_module({1: (13, 0x20)})
    m.patterns[0][(3, 0)] = (0x40, 1, 64, 0, 0)  # a new note: the channel volume outlives it
    case("chvol_renote", m, 6)
    m = s3m_module({1: (13, 0x30), 2: (4, 0x04)}, vol=64)  # the note's own volume slide and the channel volume multiply
    case("chvol_x_vslide", m, 5)
    for name, cells, rows in (("n04", {1: (14, 0x04), 2: (14, 0x04)}, 5), ("n01", {1: (14, 0x01), 3: (14, 0x01)}, 6), ("n40_up", {1: (13, 0x10), 2: (14, 0x40), 3: (14, 0x40)}, 5),
                              ("n10_up", {1: (13, 0x08), 2: (14, 0x10)}, 4), ("n32_y_first", {1: (13, 0x20), 2: (14, 0x32)}, 4), ("n23_y_first", {1: (13, 0x30), 2: (14, 0x23)}, 4),
                              ("n3f_fine_up", {1: (13, 0x20), 2: (14, 0x3F), 3: (14, 0x3F)}, 5), ("nf2_fine_down", {1: (13, 0x30), 2: (14, 0xF2), 3: (14, 0xF2)}, 5),
                              ("n0f", {1: (14, 0x0F)}, 3), ("nf0", {1: (13, 0x08), 2: (14, 0xF0)}, 3), ("nff", {1: (13, 0x10), 2: (14, 0xFF)}, 3),
                              ("n04_mem", {1: (14, 0x04), 2: (14, 0x00), 3: (14, 0x00)}, 6), ("n30_mem", {1: (13, 0x08), 2: (14, 0x30), 3: (14, 0x00)}, 5),
                              ("n04_gap", {1: (14, 0x04), 3: (14, 0x04)}, 6), ("n_to_zero", {1: (14, 0x0F), 2: (14, 0x0F)}, 4)):
        case("chvol_slide_" + name, s3m_module(cells), rows)
    for name, cells, rows in (("v20", {1: (22, 0x20)}, 4), ("v40", {1: (22, 0x40)}, 4), ("v00", {1: (22, 0x00), 3: (22, 0x30)}, 5), ("v10_v40", {1: (22, 0x10), 2: (22, 0x40)}, 4),
                              ("v50_over", {1: (22, 0x20), 2: (22, 0x50)}, 4)):
        case("gvol_" + name, s3m_module(cells), rows)
    m = s3m_module({1: (22, 0x20)})
    m.patterns[0][(3, 0)] = (0x40, 1, 64, 0, 0)
    case("gvol_renote", m, 6)
    m = s3m_module({1: (22, 0x30), 2: (13, 0x20)})  # the product of the two
    case("gvol_x_chvol", m, 4)
    for name, cells, rows in (("w04", {1: (23, 0x04), 2: (23, 0x04)}, 5), ("w01", {1: (23, 0x01), 3: (23, 0x01)}, 6), ("w40_up", {1: (22, 0x10), 2: (23, 0x40), 3: (23, 0x40)}, 5),
                              ("w10_up", {1: (22, 0x08), 2: (23, 0x10)}, 4), ("w32_x_first", {1: (22, 0x20), 2: (23, 0x32)}, 4), ("w23_x_first", {1: (22, 0x30), 2: (23, 0x23)}, 4),
                              ("w3f_fine_up", {1: (22, 0x20), 2: (23, 0x3F), 3: (23, 0x3F)}, 5), ("wf2_fine_down", {1: (22, 0x30), 2: (23, 0xF2), 3: (23, 0xF2)}, 5),
                              ("w0f", {1: (23, 0x0F)}, 3), ("wf0", {1: (22, 0x08), 2: (23, 0xF0)}, 3), ("wff", {1: (22, 0x10), 2: (23, 0xFF)}, 3),
                              ("w04_mem", {1: (23, 0x04), 2: (23, 0x00), 3: (23, 0x00)}, 6), ("w30_mem", {1: (22, 0x08), 2: (23, 0x30), 3: (23, 0x00)}, 5),
                              ("w04_gap", {1: (23, 0x04), 3: (23, 0x04)}, 6), ("w_to_zero", {1: (23, 0x0F), 2: (23, 0x0F)}, 4)):
        case("gvol_slide_" + name, s3m_module(cells), rows)
    # a slide of the channel and of the global volume at once, and N's memory is not W's
    case("n_and_w", s3m_module({1: (14, 0x03), 2: (23, 0x02), 3: (14, 0x00), 4: (23, 0x00)}), 6)
    # panbrello Yxy from S88: speed x, depth y, the position kept across rows, a zero nibble keeps the last, the pan back when the row has none
    for name, cells, rows in (("y3a", {2: (25, 0x3A)}, 4), ("y3a_rows", {2: (25, 0x3A), 3: (25, 0x3A), 4: (25, 0x3A)}, 6), ("y11", {2: (25, 0x11)}, 4), ("y4f", {2: (25, 0x4F), 3: (25, 0x4F)}, 5),
                              ("y81", {2: (25, 0x81), 3: (25, 0x81)}, 5), ("yf4", {2: (25, 0xF4), 3: (25, 0xF4), 4: (25, 0xF4)}, 6), ("y30_depth0", {2: (25, 0x30)}, 4),
                              ("y3a_mem", {2: (25, 0x3A), 3: (25, 0x00)}, 5), ("y3a_depth", {2: (25, 0x3A), 3: (25, 0x02)}, 5), ("y3a_speed", {2: (25, 0x3A), 3: (25, 0x50)}, 5),
                              ("y3a_gap", {2: (25, 0x3A), 4: (25, 0x3A)}, 6), ("y_mem_only", {2: (25, 0x00)}, 4)):
        pan("pb_" + name, cells, rows, next_slack=0.7)
    # the position is kept over a note (libopenmpt's S3M does not restart it)
    m = s3m_module({1: (19, 0x88), 2: (25, 0x3A), 3: (25, 0x3A)})
    m.patterns[0][(4, 0)] = (0x40, 1, 64, 25, 0x3A)
    before, bad = c.count, len(c.failures)
    run_pan_case(c, omp, tool, wd, "pb_renote", m, 6, next_slack=0.7)
    print("pb_renote: %d comparisons%s" % (c.count - before, "" if len(c.failures) == bad else ", %d DISAGREE" % (len(c.failures) - bad)))
    print("%d comparisons, %d disagreements" % (c.count, len(c.failures)))
    if c.count < MIN_COMPARISONS:
        print("FAIL: only %d comparisons were made, at least %d are expected: a check has gone quiet" % (c.count, MIN_COMPARISONS))
        return 1
    return 1 if c.failures else 0


if __name__ == "__main__":
    sys.exit(main())
