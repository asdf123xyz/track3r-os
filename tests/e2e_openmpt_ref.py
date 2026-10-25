#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""libopenmpt as the reference player of three journeys of the simulator (OS-E2E-9, docs/specs/OS-E2E.md "E2E-9").

  e2e_openmpt_ref.py mod|s3m|med|paula  O3SIM CARDS SCENARIOS WORKDIR [--perturb pitch[=cents]|tempo|level[=factor]|amiga|tilt[=dB]]

O3SIM is the simulator, CARDS the folder with the generated cards (tools/e2e_ref_cards.py: `<build>/sim/cards`), SCENARIOS `sim/scenarios/e2e`, WORKDIR where the renders go. The journey
`e2e_ref_<format>.txt` is a deep user's session in the simulator: a card with one synthetic module goes in, File > IMPORT, F1, Play, and the master of the engine is captured (`render song`).
This script reads that capture back, renders the SAME bytes (read from the card folder) with libopenmpt at the capture's own rate, and compares the two TICK by TICK (a tick is 20 ms: speed 6 at
125 BPM in the three formats): the pitch of the tick (autocorrelation, tests/e2e_ref_lib.py) and its level (rms) against the level of the song's own steady notes.

TOLERANCES (what a disagreement means is in each line of the report):
  pitch   1.5 cents between the two players on a tick (the engines differ by 0.05 to 0.7 cent in the tuning: a sampler at the engine's rate (44,100 Hz by default; 44,117.647 Hz with O3OS_RATE_44100 off) that plays R Hz samples at R / 44,100,
          a Paula period that is an integer; 6 cents for the arpeggio of an S3M, whose note table libopenmpt does not make equal tempered: the same 6 cents as tests/corefx_openmpt_oracle.py)
  level   0.03 of the song's steady level + 2 % of the reference's, + 0.6 of the step the reference makes at that tick (libopenmpt's mixer ramps a level change across the tick it begins in);
          the first tick of a note is left out of the level (the sampler's note-on ramp, NOTE_ON_TICKS: finding F6, which this test therefore does not gate); a tick that is silent in one
          player must be silent in the other. The levels are normalised to each player's OWN steady level, so they cannot see a gain error that moves the whole song:
  gain    the absolute level of the song's steady level against libopenmpt's at its default gain, PINNED per format (ABSOLUTE_DB, within 0.25 dB): the two players' own gain structures
          differ (+12.03 dB for the MOD, +14.53 dB for the S3M, +18.05 dB for the OctaMED song: libopenmpt scales each format its own way, the engine does not), so this is a calibration
          and not a parity claim: a change of the engine's level (or of libopenmpt's default gain) fails it, and says to measure again
  models  where the two differ ON PURPOSE or by a gap the importer and the docs name, the difference is PINNED, not skipped: each side is held to the closed form of its own rule (the
          ProTracker slide of libopenmpt, the OctaMED slide of the Amiga engine) within the same 1.5 cents / 0.03, so a fix of the engine (or a change of libopenmpt) fails the test
          and says what to remove
  --perturb  breaks the REFERENCE on purpose, to prove the comparison can fail (ctest e2e_ref_openmpt_*_break and _break_near_* require the report to name disagreements): pitch (the reference
          rendered at a rate 1.2 % low and read at the capture's: 21 cents up and 1.2 % fast; `pitch=3` is 3 cents, twice the tolerance), tempo (the reference's rows come one tick early),
          level (the reference's ticks from the fifth on are 25 % quieter; `level=0.9` is 10 % quieter, about twice the tolerance)
`paula` is the fourth journey: the Amiga voice's SOUND against libopenmpt's Amiga resampler emulation (ctl render.resampler.emulate_amiga, type a1200 as the Amiga voices' default filter is the
1200's), not tick by tick and not sample by sample: a bright saw is played at three periods and, for the steady part of each note,
  band energy      the energy of each octave band (100 Hz to 22 kHz, eight bands) as dB of the note's total: within 1.5 dB where the band holds more than -45 dB of the total
  aliasing         the energy that is NOT a harmonic of the note (outside +-12 Hz of every multiple of the Paula-rate fundamental 3,546,895 / (period x 64)), as dB of the total: within 3 dB of
                   the reference's, and under -40 dB (a clean Paula). Its distribution over the bands is printed, not held: the OS puts all of it above 12.8 kHz (under -89 dB below), libopenmpt's
                   emulation spreads it over every octave (-57 to -76 dB): a finding of this comparison (docs/specs/OS-E2E.md "E2E-9")
  --perturb amiga  breaks the reference: the emulation off (a band-limited sinc reconstruction of the same bytes: no image spectrum above the sample's Nyquist)
  --perturb tilt   breaks the reference by a shelf: everything of it above 3.2 kHz raised by 3 dB (`tilt=<dB>`), twice the tolerance of the band energies
Without libopenmpt or numpy the script exits 77 (a skip), and with O3_CI=1 in the environment (tools/ci.sh sets it) that is a failure.
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import e2e_ref_lib as L  # noqa: E402

SPEED = 6
TICK_S = 0.02
PITCH_TOL = 1.5
ONSET_PITCH_TOL = 3.0  # the first tick of a note holds its attack (the autocorrelation window sees the amplitude still rising: up to 1.5 cents high on the Modern engine)
BASE_TICK = 3          # the first case of the three songs is an arpeggio (root, +x, +y, root, ...): tick 3 is the plain note, past the attack
ARP_TOL_S3M = 6.0
LEVEL_TOL = 0.03
SILENT = 0.02          # a tick quieter than this (of the song's level) is silent
NOTE_ON_TICKS = "the sampler's note-on ramp: the first tick of a note is 12 % under its steady level on the Modern engine (S3M: 0.88 of it, libopenmpt 0.99 to 1.01; the Amiga engine and the MOD and MED songs have no such step)"
# The song's steady level of the engine (int16 units) against libopenmpt's (full scale 1.0), in dB, measured on libopenmpt 0.7.3 at its default gain and 100 % separation, mono mix: the mean rms of the first case's ticks 1..5 and 7..11.
ABSOLUTE_DB = {"mod": 12.03, "s3m": 14.53, "med": 18.05}
ABSOLUTE_TOL_DB = 0.25


class Case:
    """first: the tick the case starts at (row * 6); rows: its length; kind: strict | model | known; extra: the parameters of a model."""

    def __init__(self, name, first_row, rows, kind="strict", **extra):
        self.name = name
        self.first = first_row * SPEED
        self.ticks = rows * SPEED
        self.kind = kind
        self.extra = extra

    def tick_range(self):
        return range(self.first, self.first + self.ticks)


# ---- the songs (tools/e2e_ref_cards.py describes each row) ------------------------------------------------------------------------

MOD_CASES = [
    Case("arpeggio 037", 0, 2),
    Case("slide up 104", 2, 2, "model", model="slide", p0=428, step=4, dir=+1, ours_tick0=True, effect_from=0),
    Case("fine slide up E13", 4, 2),
    Case("volume slide down A03", 6, 2, "model", model="vslide", step=3, ours_tick0=True, effect_from=0),
    Case("note at volume 32 (C20 beside it)", 8, 1),
    Case("C10 alone: the volume of the sounding note", 9, 1, "known", known="vol_alone", before=8, ratio=0.5),
    Case("tone portamento 305 to G-2", 10, 3, "model", model="porta", p0=428, target=285, step=5, ours_tick0=True, effect_from=1),
    Case("note cut EC3", 13, 2),
]
MOD_TICKS = 15 * SPEED

S3M_CASES = [
    Case("arpeggio J37", 0, 2, pitch_tol=ARP_TOL_S3M),
    Case("volume slide down D04", 2, 2),
    Case("fine volume slide up D4F", 4, 2),
    Case("fine portamento up FF4 (C2SPD 11025)", 6, 2),   # OS-BUG-MODPLAY F4: the period is the sample's (21.5 cents), it was 16.2 at the fixed C-4 = 1712
    Case("tremor I11", 8, 2),
    Case("the note", 10, 1),
    Case("volume column 16 alone", 11, 1, "known", known="vol_alone", before=10, ratio=0.25),
]
S3M_TICKS = 12 * SPEED

MED_CASES = [
    Case("arpeggio 00 37", 0, 2),
    Case("slide up 01 04", 2, 2),
    Case("slide down 02 04", 4, 2),
    Case("volume slide down 0A 03", 6, 2),
    Case("note at 0C 20", 8, 1),
    Case("0C 10 alone: the volume of the sounding note", 9, 1, "known", known="vol_alone", before=8, ratio=0.5),
    Case("tone portamento 03 05", 10, 3),
    Case("note cut 18 03", 13, 2),
]
MED_TICKS = 15 * SPEED

PAULA_PERIODS = [428, 214, 160]   # the three notes of tools/e2e_ref_cards.py paula_mod, four rows (480 ms) each
PAULA_BANDS = [100.0 * 2 ** k for k in range(8)] + [22000.0]
PAULA_BAND_TOL = 1.5     # dB
PAULA_FLOOR = -45.0      # a band under this (of the note's total) is not compared
PAULA_ALIAS_TOL = 3.0    # dB
PAULA_ALIAS_MAX = -40.0  # dB re the total
PAULA_HARM_HZ = 12.0

FORMATS = {
    "mod": ("ref.mod", "e2e_ref_mod", MOD_CASES, MOD_TICKS),
    "s3m": ("ref.s3m", "e2e_ref_s3m", S3M_CASES, S3M_TICKS),
    "med": ("ref.med", "e2e_ref_med", MED_CASES, MED_TICKS),
    "paula": ("paula.mod", "e2e_ref_paula", None, 0),
}


# ---- the models ---------------------------------------------------------------------------------------------------------------------


def slide_count(case, side, j, k):
    """Ticks of sliding done by tick k of row j of the case (the rows of the effect start at rows[0]). ProTracker (libopenmpt, 'omp'): ticks 1..5 of a row, tick 0 none. The Amiga engine
    ('ours', OctaMED's rule, audio/med_tick.h): every tick, tick 0 included."""
    r0 = case.extra["effect_from"]
    if j < r0:
        return 0
    jj = j - r0
    return (SPEED * jj + k + 1) if side == "ours" and case.extra["ours_tick0"] else (SPEED - 1) * jj + k


def model_pitch_cents(case, side, j, k):
    """The pitch in cents above the case's first note, by the closed form of the side's own rule."""
    n = slide_count(case, side, j, k)
    x = case.extra
    if x["model"] == "slide":
        return 1200.0 * math.log2(x["p0"] / (x["p0"] - x["dir"] * x["step"] * n))
    if x["model"] == "porta":
        period = max(x["target"], x["p0"] - x["step"] * n)
        return 1200.0 * math.log2(x["p0"] / period)
    return 0.0


def model_level(case, side, j, k):
    """The level of a volume slide down, of the note's 64."""
    return max(0.0, 64.0 - case.extra["step"] * slide_count(case, side, j, k)) / 64.0


# ---- the comparison ---------------------------------------------------------------------------------------------------------------


def measure(wav, data, ref, ticks, perturb, amount=None):
    """-> (ours, theirs): [(pitch, rms)] per tick, ours from the first sound of the capture (the Play command is taken at the next block start), libopenmpt's from its first frame."""
    mono = wav.mono()
    on = L.onset(mono)
    ours = L.tick_track(mono[on:], wav.rate, ticks)
    # (the pitch break renders the reference at another rate and reads it as the capture's: 21 cents up, and 1.2 % faster; `pitch=<cents>` renders it that many cents up and as much faster)
    rate = wav.rate
    if perturb == "pitch":
        rate = int(round(wav.rate * (0.988 if amount is None else 2.0 ** (-L.number(amount, 0.0) / 1200.0))))
    left, right = ref.render(data, rate, int((ticks + 4) * TICK_S * rate))
    theirs = L.tick_track((left + right) / 2.0, wav.rate, ticks)
    if perturb == "tempo":
        theirs = theirs[1:] + theirs[-1:]  # the rows come one tick (20 ms) early
    if perturb == "level":
        factor = L.number(amount, 0.75)
        theirs = [(f, r if t < 5 else r * factor) for t, (f, r) in enumerate(theirs)]
    return ours, theirs


def compare(rep, who, fmt, cases, ours, theirs):
    """The report of a journey: strict cases tick by tick, models and known differences as the header says."""
    # the song's own steady level of each player: the first case's ticks 1..11 (a steady note at volume 64, an arpeggio or a plain note)
    c0 = cases[0]
    ticks0 = [t for t in c0.tick_range() if t % SPEED != 0]
    unit_o = sum(ours[t][1] for t in ticks0) / len(ticks0)
    unit_t = sum(theirs[t][1] for t in ticks0) / len(ticks0)
    rep.check(unit_o > 0 and unit_t > 0, "%s: no sound in the first case (ours %.1f, libopenmpt %.4f)" % (who, unit_o, unit_t))
    if not (unit_o > 0 and unit_t > 0):
        return
    # (the levels below are each player's own steady level = 1: the absolute level is the one thing they cannot see, so it is pinned here. ours is in int16 units, libopenmpt's in full scale 1.0)
    offset = 20.0 * math.log10(unit_o / (unit_t * 32768.0))
    print("%s: steady level %.0f here (int16 rms), %.0f in libopenmpt at its default gain: %+.2f dB (pinned at %+.2f dB, tolerance %.2f)" % (who, unit_o, unit_t * 32768.0, offset, ABSOLUTE_DB[fmt], ABSOLUTE_TOL_DB))
    rep.check(abs(offset - ABSOLUTE_DB[fmt]) <= ABSOLUTE_TOL_DB, "%s: the song's steady level is %+.2f dB against libopenmpt's at its default gain, pinned at %+.2f dB (tolerance %.2f): the engine's level moved, or libopenmpt's gain did; measure "
              "again and update ABSOLUTE_DB" % (who, offset, ABSOLUTE_DB[fmt], ABSOLUTE_TOL_DB))
    lo = [r / unit_o for _, r in ours]
    lt = [r / unit_t for _, r in theirs]
    table = []
    for c in cases:
        if c.kind == "known":
            table.append(check_known(rep, who, c, ours, theirs, lo, lt))
        else:
            table.append(check_case(rep, who, c, ours, theirs, lo, lt))
    print_table(who, table)


def check_case(rep, who, c, ours, theirs, lo, lt):
    tol_p = c.extra.get("pitch_tol", PITCH_TOL)
    po0, pt0 = ours[BASE_TICK][0], theirs[BASE_TICK][0]   # the note's own pitch, from the plain tick of the first case
    worst_diff = worst_rule = worst_l = 0.0
    if not (po0 > 0.0 and pt0 > 0.0):
        rep.check(False, "%s %s: no pitch at the plain tick %d of the first case (%.2f Hz here, %.2f Hz in libopenmpt): the pitches of the song cannot be told from a note" % (who, c.name, BASE_TICK, po0, pt0))
        return (c.name, c.kind, float("nan"), float("nan"), float("nan"))
    model = c.extra.get("model")
    for t in c.tick_range():
        j, k = (t - c.first) // SPEED, (t - c.first) % SPEED
        where = "%s %s: row %d tick %d" % (who, c.name, t // SPEED, k)
        silent_o, silent_t = lo[t] < SILENT, lt[t] < SILENT
        rep.check(silent_o == silent_t, "%s is %s in libopenmpt (level %.3f) and %s here (%.3f)" % (where, "silent" if silent_t else "sounding", lt[t], "silent" if silent_o else "sounding", lo[t]))
        if silent_o or silent_t:
            continue
        po, pt = ours[t][0], theirs[t][0]
        if not (po > 0.0 and pt > 0.0):   # sounding, and no pitch found in one of them (a pitch of 0): a disagreement, not an exception
            rep.check(False, "%s: a sounding tick with no pitch (%.2f Hz here, %.2f Hz in libopenmpt)" % (where, po, pt))
            continue
        d = L.cents(po, pt)
        worst_diff = max(worst_diff, abs(d))
        tol = max(tol_p, ONSET_PITCH_TOL) if t == c.first else tol_p
        if model in ("slide", "porta"):
            dm_o = L.cents(po, po0) - model_pitch_cents(c, "ours", j, k)
            dm_t = L.cents(pt, pt0) - model_pitch_cents(c, "omp", j, k)
            worst_rule = max(worst_rule, abs(dm_o), abs(dm_t))
            rep.check(abs(dm_o) <= max(PITCH_TOL, tol - tol_p + PITCH_TOL if t == c.first else 0), "%s: the Amiga engine is %.2f cents off OctaMED's rule (%.2f Hz)" % (where, dm_o, po))
            rep.check(abs(dm_t) <= PITCH_TOL, "%s: libopenmpt is %.2f cents off ProTracker's rule (%.2f Hz)" % (where, dm_t, pt))
        else:
            rep.check(abs(d) <= tol, "%s: the pitch is %.2f cents off libopenmpt's (%.2f Hz here, %.2f Hz there; tolerance %.1f)" % (where, d, po, pt, tol))
        if t == c.first:
            continue  # the first tick of a note: the level is compared from the next on (NOTE_ON_TICKS)
        if model == "vslide":
            mo, mt = model_level(c, "ours", j, k), model_level(c, "omp", j, k)
            pj, pk = prev_jk(j, k)
            slack_o = 0.6 * abs(mo - model_level(c, "ours", pj, pk))
            slack_t = 0.6 * abs(mt - model_level(c, "omp", pj, pk))
            worst_l = max(worst_l, abs(lo[t] - mo), abs(lt[t] - mt))
            rep.check(abs(lo[t] - mo) <= LEVEL_TOL + 0.02 * mo + slack_o, "%s: the Amiga engine's level is %.3f of the song's, OctaMED's rule gives %.3f" % (where, lo[t], mo))
            rep.check(abs(lt[t] - mt) <= LEVEL_TOL + 0.02 * mt + slack_t, "%s: libopenmpt's level is %.3f of the song's, ProTracker's rule gives %.3f" % (where, lt[t], mt))
        else:
            slack = 0.6 * abs(lt[t] - lt[t - 1])
            worst_l = max(worst_l, abs(lo[t] - lt[t]))
            rep.check(abs(lo[t] - lt[t]) <= LEVEL_TOL + 0.02 * lt[t] + slack, "%s: the level is %.3f of the song's here and %.3f in libopenmpt" % (where, lo[t], lt[t]))
    return (c.name, c.kind, worst_diff, worst_rule, worst_l)


def row_level(lv, row):
    """Mean level over ticks 1..5 of a row."""
    ticks = range(row * SPEED + 1, (row + 1) * SPEED)
    return sum(lv[t] for t in ticks) / len(ticks)


def check_known(rep, who, c, ours, theirs, lo, lt):
    x = c.extra
    if x["known"] == "vol_alone":
        # A volume with no note on its row. libopenmpt sets the sounding note's volume at once; the engine reads the volume of a step only where a note starts, and the importer writes no
        # Volume FX for it, so the level stays. Both are pinned: the reference's change (x["ratio"] of the note before) and the engine's none.
        before = x["before"]
        base_o, base_t = row_level(lo, before), row_level(lt, before)
        got_o, got_t = row_level(lo, c.first // SPEED), row_level(lt, c.first // SPEED)
        rep.check(abs(got_t / base_t - x["ratio"]) <= 0.08, "%s %s: libopenmpt's level went to %.3f of the note before, the new volume is %.2f of it" % (who, c.name, got_t / base_t, x["ratio"]))
        rep.check(abs(got_o / base_o - 1.0) <= 0.06, "%s %s: the level changed here (%.3f of the note before): the engine reads a volume without a note now, update the journey and the spec" % (who, c.name, got_o / base_o))
        print("%s %s: libopenmpt %.3f of the note's level, here %.3f (the engine keeps the level)" % (who, c.name, got_t / base_t, got_o / base_o))
        return (c.name, c.kind, 0.0, 0.0, abs(got_o - got_t))
    raise AssertionError(x)


def prev_jk(j, k):
    return (j, k - 1) if k > 0 else ((j - 1, SPEED - 1) if j > 0 else (0, 0))


def print_table(who, table):
    print("%s: %-46s %-7s %s" % (who, "case", "kind", "players differ (cents) | worst from a rule (cents) | worst level difference (of the song's level)"))
    for name, kind, wd, wr, wl in table:
        print("  %-52s %-7s %7.2f %7.2f %8.3f" % (name, kind, wd, wr, wl))


# ---- the Paula journey ------------------------------------------------------------------------------------------------------------


def blackman_harris(n):
    k = np.arange(n) * (2.0 * math.pi / (n - 1))
    return 0.35875 - 0.48829 * np.cos(k) + 0.14128 * np.cos(2 * k) - 0.01168 * np.cos(3 * k)


np = L.np


def paula_profile(x, rate, period):
    """-> (octave band energies in dB of the total, the inharmonic energy in dB of the total, the inharmonic energy of each band in dB of the total) of a steady stretch of a note."""
    f0 = 3546895.0 / (period * 64.0)   # the saw is 64 frames long: the Paula-rate fundamental
    size = 1 << 17
    sp = np.abs(np.fft.rfft(x * blackman_harris(len(x)), size)) ** 2
    f = np.fft.rfftfreq(size, 1.0 / rate)
    harmonic = np.abs(f - np.round(f / f0) * f0) < PAULA_HARM_HZ
    total = float(np.sum(sp[(f >= 50.0) & (f < 22000.0)]))
    bands, inharmonic = [], []
    for a, b in zip(PAULA_BANDS[:-1], PAULA_BANDS[1:]):
        sel = (f >= a) & (f < b)
        bands.append(10.0 * math.log10(float(np.sum(sp[sel])) / total + 1e-30))
        inharmonic.append(10.0 * math.log10(float(np.sum(sp[sel & ~harmonic])) / total + 1e-30))
    alias = 10.0 * math.log10(float(np.sum(sp[(f >= 50.0) & (f < 22000.0) & ~harmonic])) / total + 1e-30)
    return np.array(bands), alias, np.array(inharmonic)


def paula_compare(rep, who, wav, data, ref, perturb, amount=None):
    rate = wav.rate
    mono = wav.mono()
    ours_x = mono[L.onset(mono):]
    ctls = {"render.resampler.emulate_amiga": perturb != "amiga", "render.resampler.emulate_amiga_type": "a1200"}
    left, right = ref.render(data, rate, int(1.62 * rate), ctls=ctls)
    theirs_x = (left + right) / 2.0
    if perturb == "tilt":   # a shelf of `amount` dB (3 by default) from 3.2 kHz up, on the reference
        spec = np.fft.rfft(theirs_x)
        spec[np.fft.rfftfreq(len(theirs_x), 1.0 / rate) >= 3200.0] *= 10.0 ** (L.number(amount, 3.0) / 20.0)
        theirs_x = np.fft.irfft(spec, len(theirs_x))
    print("%s: octave bands (dB of the note's total) and the aliasing (the energy that is not a harmonic), steady part of each note" % who)
    print("  band edges (Hz): " + " ".join("%g" % e for e in PAULA_BANDS))
    for i, period in enumerate(PAULA_PERIODS):
        a, b = int((i * 0.48 + 0.06) * rate), int((i * 0.48 + 0.44) * rate)
        bo, ao, io = paula_profile(ours_x[a:b], rate, period)
        bt, at, it = paula_profile(theirs_x[a:b], rate, period)
        print("  note %d (period %d): bands here %s" % (i, period, " ".join("%7.2f" % v for v in bo)))
        print("  %-22s libopenmpt %s" % ("", " ".join("%7.2f" % v for v in bt)))
        print("  %-22s aliasing here %.2f dB, libopenmpt %.2f dB; by band here %s" % ("", ao, at, " ".join("%6.1f" % v for v in io)))
        print("  %-22s by band libopenmpt                     %s" % ("", " ".join("%6.1f" % v for v in it)))
        for k, (x, y) in enumerate(zip(bo, bt)):
            if max(x, y) > PAULA_FLOOR:
                rep.check(abs(x - y) <= PAULA_BAND_TOL, "%s note %d: the band %g-%g Hz holds %.2f dB of the total here and %.2f dB in libopenmpt (tolerance %.1f)" %
                          (who, i, PAULA_BANDS[k], PAULA_BANDS[k + 1], x, y, PAULA_BAND_TOL))
        rep.check(abs(ao - at) <= PAULA_ALIAS_TOL, "%s note %d: the aliasing is %.2f dB of the total here and %.2f dB in libopenmpt (tolerance %.1f)" % (who, i, ao, at, PAULA_ALIAS_TOL))
        rep.check(ao <= PAULA_ALIAS_MAX, "%s note %d: the aliasing is %.2f dB of the total, a clean Paula is under %.0f" % (who, i, ao, PAULA_ALIAS_MAX))


# ---- main -----------------------------------------------------------------------------------------------------------------------


def main(argv):
    perturb = amount = None
    args = [a for a in argv[1:]]
    if "--perturb" in args:
        i = args.index("--perturb")
        perturb, amount = L.parse_perturb(args[i + 1])
        del args[i:i + 2]
    if len(args) != 5 or args[0] not in FORMATS or perturb not in (None, "pitch", "tempo", "level", "amiga", "tilt") or (amount is not None and perturb in ("tempo", "amiga")):
        print(__doc__)
        return 2
    fmt, o3sim, cards, scenarios, workdir = args
    who = "e2e_openmpt_" + fmt
    code = L.need_numpy(who)
    if code is not None:
        return code
    ref, why = L.Reference.open()
    if ref is None:
        return L.skip(who, why)
    print("libopenmpt %d.%d.%d" % ref.version)
    if perturb is not None and (perturb in ("amiga", "tilt")) != (fmt == "paula"):
        print("%s: --perturb %s does not apply to %s" % (who, perturb, fmt))
        return 2
    fname, card, cases, ticks = FORMATS[fmt]
    with open(os.path.join(cards, card, fname), "rb") as f:
        data = f.read()
    try:
        renders, _ = L.run_journey(o3sim, os.path.join(scenarios, card + ".txt"), cards, workdir)
    except RuntimeError as e:
        print("%s: %s" % (who, e))
        return 1
    if "song" not in renders:
        print("%s: the journey made no render called 'song'" % who)
        return 1
    rep = L.Report(who)
    if fmt == "paula":
        paula_compare(rep, who, renders["song"], data, ref, perturb, amount)
        return rep.finish(minimum=len(PAULA_PERIODS) * 8)
    ours, theirs = measure(renders["song"], data, ref, ticks, perturb, amount)
    compare(rep, who, fmt, cases, ours, theirs)
    return rep.finish(minimum=len(cases) * 6)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
