#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/dsp_ref.py (scipy) as the reference of three journeys of the simulator (OS-E2E-9, docs/specs/OS-E2E.md "E2E-9").

  e2e_dsp_ref.py filter|limiter|export  O3SIM CARDS SCENARIOS WORKDIR [--perturb <kind>[=<amount>]]

  filter    e2e_ref_dsp_filter.txt: white noise through the instrument filter (low-pass at two cutoffs, with a resonance, high-pass); the cross spectrum of the filtered take with the open take
            over the open take's power spectrum is the filter's magnitude response, held to the analogue second-order state variable filter at the cutoff the engine's table gives
            (dsp_ref.svf_response_db) in eight octave bands (100 Hz to 22 kHz): 0.75 dB up to 6.4 kHz, 1.5 dB to 12.8 kHz, 4 dB above (a Chamberlin filter run at twice the rate is a little lower
            than its prototype there: the mean of its two half steps), and only where the reference is above -65 dB. --perturb cutoff moves the reference's cutoff up 10 % of the range (a
            table step is 1/477: 50 steps an octave, so 0.1 is 48 steps, almost an octave), --perturb q adds 2 to its resonance; `cutoff=0.008` (4 steps, 1/13 octave: the resonant band is 1.6 dB
            off, twice the tolerance) and `q=0.15` (about twice the tolerance too) are the near breaks.
  limiter   e2e_ref_dsp_limiter.txt: a sine at stepped levels through the master limiter; each take's peak against dsp_ref.limiter_static_db: the output follows the input to the ceiling
            within 0.1 dB and never passes it. THE INPUT IS CLEAN ONLY UP TO 0 dBFS: the sampler voice clips at int16 full scale before the master (audio/voices.cpp, "the voices saturate"), so the
            takes above volume 62 reach the limiter as a square wave of full scale (distortion -30 dB at volume 64, -7.5 dB at 100: printed) and prove only that the output is at the ceiling,
            which a hard clipper at the ceiling would give too. What a limiter does and a clipper does not is held by the last take, `poly`: two voices (pads 12 and 24, an octave apart, each
            under full scale at volume 60) sum to +4.6 dBFS, a clean overload; the limiter scales it by a gain: (1) the output peak is the ceiling, (2) the gain is ceiling / peak of the sum,
            which the unlimited takes (v60, v60b) and the take's own phases give (0.3 dB; measured 0.15), (3) it is the same at both tones (0.06 dB; measured 0.03), (4) what is left after a
            fit of the two tones is under -30 dB of them (measured -36.5 dB: the 64 sample window of the look-ahead against a period of 200 samples ripples the gain by 1.7 %; a hard clipper at
            the ceiling leaves -14.8 dB and a gain of -2.25 dB instead of -4.88). The relative phase of the two pads is fixed by the journey (deterministic), and moves the figures by a few dB.
            --perturb ceiling puts the reference's ceiling 1 dB up (`ceiling=0.1`: 0.1 dB, twice the tolerance at the ceiling); --perturb clipper replaces the
            `poly` take by what a hard clipper at the ceiling makes of the same sum (the other checks pass it: that is what the distortion check is for).
  export    e2e_ref_dsp_export.txt: five pure tones (689 Hz to 9.3 kHz), played live and exported through File > EXPORT to a 44,100 Hz WAV. The reference resamples the LIVE capture with an exact
            2499 / 2500 polyphase filter (dsp_ref.export_reference, a Kaiser window of 140 dB): per tone, over its steady part, the frequency (0.05 cent: the export keeps the pitch), the gain
            (0.02 dB to 2.7 kHz, 0.1 dB at 5.5 kHz, 0.4 dB at 9.3 kHz: the droop of the export's cubic interpolator), the delay against the live render (0.01 sample) and the SNR of the difference
            (55 dB, 36 dB at 5.5 kHz, 20 dB at 9.3 kHz) are held. --perturb ratio makes the reference's ratio 1000 / 1001 (about 1 cent); `ratio=2922/2923` is 0.1 cent off the exact one, twice
            the frequency tolerance.
Without numpy or scipy the script exits 77 (a skip), and with O3_CI=1 in the environment (tools/ci.sh sets it) that is a failure.
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import e2e_ref_lib as L  # noqa: E402

np = L.np
EDGES = [100.0 * 2 ** k for k in range(8)] + [22000.0]
BAND_TOL_LOW = 0.75    # dB, the bands below 6.4 kHz
BAND_TOL_HIGH = 1.5    # dB, 6.4 kHz to 12.8 kHz
BAND_TOL_TOP = 4.0     # dB, 12.8 kHz to 22 kHz: the engine's Chamberlin filter runs at twice the rate and gives the mean of its two half steps (and takes the mean of two inputs):
                       # cos(pi f / 88,235) twice, 3.4 dB at 17 kHz, which the analogue prototype does not have
HIGH_FROM = 6400.0
FLOOR_DB = -65.0       # a band whose reference is under this is not compared


def need_scipy(who):
    try:
        import scipy.signal  # noqa: F401
        import dsp_ref  # noqa: F401
    except ImportError:
        return L.skip(who, "scipy is not installed here (apt: python3-scipy)")
    return None


# ---- filter ----------------------------------------------------------------------------------------------------------------------


def transfer_bands(open_wav, take_wav):
    """The magnitude response of a filtered take over the open take, in dB, as the mean power in each octave band. The two takes play the SAME noise (the sample is deterministic), only a few
    samples apart (a pad is taken at the next block start, and the block grid is not at the same place in two takes), so the transfer function is the cross spectrum over the power spectrum of the
    open take (Welch, 1,024 point Hann segments) of the two takes cut at their onsets: noise in the estimate cancels, which a ratio of two power spectra does not do."""
    import scipy.signal as ss
    rate = open_wav.rate
    x, y = open_wav.mono(), take_wav.mono()
    ox, oy = L.onset(x), L.onset(y)
    n = int(0.2 * rate)
    xs, ys = x[ox + 300:ox + 300 + n], y[oy + 300:oy + 300 + n]
    f, pxx = ss.welch(xs, rate, nperseg=1024, window="hann")
    _, pxy = ss.csd(xs, ys, rate, nperseg=1024, window="hann")
    h2 = np.abs(pxy / pxx) ** 2
    out = []
    for a, b in zip(EDGES[:-1], EDGES[1:]):
        sel = (f >= a) & (f < b)
        out.append(10.0 * math.log10(float(np.mean(h2[sel])) + 1e-30))
    return np.array(out)


FILTER_TAKES = [   # take, cutoff c, resonance r, type (0 low-pass, 1 high-pass): the order of the journey
    ("lp45", 0.45, 0.0, 0),
    ("lp65", 0.65, 0.0, 0),
    ("lp65q", 0.65, 3.01, 0),
    ("hp65", 0.65, 0.0, 1),
]


def band_tolerance(lo):
    if lo >= 12800.0:
        return BAND_TOL_TOP
    return BAND_TOL_HIGH if lo >= HIGH_FROM else BAND_TOL_LOW


def run_filter(rep, who, renders, perturb, amount=None):
    import dsp_ref
    fgrid = np.linspace(20.0, 22000.0, 4000)
    print("%s: filter take | band edges %s Hz | response over the open take (dB): measured, reference" % (who, " ".join("%g" % e for e in EDGES)))
    worst = [0.0, 0.0, 0.0]
    for name, c, r, kind in FILTER_TAKES:
        measured = transfer_bands(renders["open"], renders[name])
        cc, rr = c, r
        if perturb == "cutoff":
            cc = min(1.0, c + L.number(amount, 0.1))   # up by a tenth of the range, 48 of the table's 477 steps (50 an octave): almost an octave
        if perturb == "q":
            rr = r + L.number(amount, 2.0)
        ref = np.array(dsp_ref.band_levels_db(dsp_ref.svf_response_db(fgrid, cc, rr, kind), fgrid, EDGES))
        print("  %-6s fc %.1f Hz Q %.1f: measured %s" % (name, dsp_ref.svf_cutoff_hz(c), dsp_ref.svf_q(r), " ".join("%7.2f" % v for v in measured)))
        print("  %-6s %-23s reference %s" % ("", "", " ".join("%7.2f" % v for v in ref)))
        for k in range(len(EDGES) - 1):
            if ref[k] < FLOOR_DB:
                continue
            tol = band_tolerance(EDGES[k])
            d = measured[k] - ref[k]
            region = 0 if EDGES[k] < HIGH_FROM else (1 if EDGES[k] < 12800.0 else 2)
            worst[region] = max(worst[region], abs(d))
            rep.check(abs(d) <= tol, "%s %s: the band %g-%g Hz is %.2f dB under the open noise, the analogue filter gives %.2f dB (%+.2f dB, tolerance %.2f)" %
                      (who, name, EDGES[k], EDGES[k + 1], measured[k], ref[k], d, tol))
    print("%s: worst difference from the analogue filter: %.2f dB below 6.4 kHz, %.2f dB to 12.8 kHz, %.2f dB above" % (who, worst[0], worst[1], worst[2]))


# ---- limiter ---------------------------------------------------------------------------------------------------------------------


LIMITER_TAKES = [("v30", 30), ("v50", 50), ("v60", 60), ("v62", 62), ("v64", 64), ("v85", 85), ("v100", 100)]
LIMITER_FIT = ("v30", "v50", "v60")   # the takes under the ceiling: the input level is read off these
LIMITER_OUT_TOL = 0.1                 # dB, below the ceiling (the sine's peak over 130 ms: a few hundredths of a dB of sampling)
LIMITER_CEIL_TOL = 0.05               # dB, at the ceiling
LIMITER_SLOPE = 0.48                  # dB a step of the volume (the 1.2+ law: 24 dB over 50 steps): the fit has to find it, as a check of the fit and not of the limiter
LIMITER_SLOPE_TOL = 0.01
# The two-voice take `poly` (pads 12 and 24 at volume 60, f and f / 2 at 441.18 and 220.59 Hz; v60 and v60b are the voices alone): the clean overload that a limiter scales and a clipper clips.
POLY_F = 441.18
POLY_OVER_MIN = 3.0                   # dB the sum of the two voices is over the ceiling (measured 4.88): the take has to make the limiter work
POLY_GAIN_TOL = 0.3                   # dB, the gain against ceiling / peak of the sum (measured 0.15: the ripple of the look-ahead's gain; a hard clipper is 2.6 dB off)
POLY_TONES_TOL = 0.06                 # dB, the gain at the two tones against each other (measured 0.03: a gain, not a reshaping)
POLY_RESIDUE_MAX = -30.0              # dB re the two tones: what the fit of them leaves (measured -36.5; a hard clipper -14.8)


def take_peak_db(wav):
    r = wav.rate
    seg = wav.left[int(0.06 * r):int(0.19 * r)]
    return 20.0 * math.log10(float(np.max(np.abs(seg))) / 32768.0)


def take_segment(wav):
    """The steady part of a limiter take (60 to 190 ms), left channel, int16 units."""
    r = wav.rate
    return wav.left[int(0.06 * r):int(0.19 * r)]


def fit_tones(x, freqs, fs):
    """Least squares fit of sines at `freqs` (and a constant) to x -> ([amplitude], [phase], residue in dB of the tones' rms): the residue is what is not those tones, the distortion plus noise."""
    n = np.arange(len(x))
    cols = []
    for f in freqs:
        cols += [np.cos(2 * np.pi * f * n / fs), np.sin(2 * np.pi * f * n / fs)]
    cols.append(np.ones(len(x)))
    a = np.stack(cols, 1)
    coef, *_ = np.linalg.lstsq(a, x, rcond=None)
    amps = [math.hypot(coef[2 * i], coef[2 * i + 1]) for i in range(len(freqs))]
    phases = [math.atan2(-coef[2 * i + 1], coef[2 * i]) for i in range(len(freqs))]
    tones = math.sqrt(sum(v * v for v in amps) / 2.0)
    resid = math.sqrt(float(np.mean((x - a @ coef) ** 2)))
    return amps, phases, 20.0 * math.log10(max(resid, 1e-9) / tones)


def run_limiter(rep, who, renders, perturb, amount=None):
    import dsp_ref
    ceiling = dsp_ref.LIMITER_CEILING_DB + (L.number(amount, 1.0) if perturb == "ceiling" else 0.0)
    peaks = {name: take_peak_db(renders[name]) for name, _ in LIMITER_TAKES}
    volumes = {name: v for name, v in LIMITER_TAKES}
    xs = np.array([volumes[n] for n in LIMITER_FIT], dtype=np.float64)
    ys = np.array([peaks[n] for n in LIMITER_FIT])
    slope, offset = np.polyfit(xs, ys, 1)
    rep.check(abs(slope - LIMITER_SLOPE) <= LIMITER_SLOPE_TOL, "%s: the quiet takes lie on %.4f dB a step of the volume, not %.2f: the input level cannot be read off them" % (who, slope, LIMITER_SLOPE))
    print("%s: input level = %.3f + %.4f x volume dBFS (fitted on %s); ceiling %.3f dBFS" % (who, offset, slope, ", ".join(LIMITER_FIT), ceiling))
    print("%s: take volume | volume's level dBFS (the line extended: the voice clips at 0 dBFS, so the limiter sees less) | output dBFS | reference output dBFS | distortion of the take (dB, not held)" % who)
    for name, v in LIMITER_TAKES:
        level_in = offset + slope * v
        want = dsp_ref.limiter_static_db(level_in, ceiling)
        got = peaks[name]
        limited = level_in > ceiling
        tol = LIMITER_CEIL_TOL if limited else LIMITER_OUT_TOL
        seg = take_segment(renders[name])
        fs = renders[name].rate
        thd = fit_tones(seg, [tone_frequency(seg, POLY_F, fs)], fs)[2]
        print("  %-5s %3d %9.3f %9.3f %9.3f %7.1f%s" % (name, v, level_in, got, want, thd, "  (limited)" if limited else ""))
        rep.check(abs(got - want) <= tol, "%s %s: input %.2f dBFS, output %.3f dBFS, the static curve gives %.3f (tolerance %.2f)" % (who, name, level_in, got, want, tol))
        rep.check(got <= dsp_ref.LIMITER_CEILING_DB + 1e-3, "%s %s: the output peak %.4f dBFS passes the ceiling %.4f" % (who, name, got, dsp_ref.LIMITER_CEILING_DB))
    run_poly(rep, who, renders, ceiling, perturb)


def run_poly(rep, who, renders, ceiling, perturb):
    """The two-voice take: a clean sum over the ceiling, scaled by a gain (and not clipped). `ceiling` is the reference's (moved by --perturb ceiling), the true one is dsp_ref's."""
    import dsp_ref
    fs = renders["poly"].rate
    sa, sb, sc = (take_segment(renders[k]) for k in ("v60", "v60b", "poly"))
    f_hi, f_lo = tone_frequency(sa, POLY_F, fs), tone_frequency(sb, POLY_F / 2.0, fs)
    rep.check(abs(f_hi / f_lo - 2.0) < 1e-4, "%s poly: the two pads are %.4f and %.4f Hz, not an octave apart: the take is not a sum of f and f / 2" % (who, f_hi, f_lo))
    (a_hi,), _, _ = fit_tones(sa, [f_hi], fs)
    (a_lo,), _, _ = fit_tones(sb, [f_lo], fs)
    (c_lo, c_hi), (p_lo, p_hi), _ = fit_tones(sc, [f_lo, f_hi], fs)
    # the sum before the limiter: the two voices alone (unlimited takes, so their amplitudes are the voices') with the take's own phases (a delay moves the waveform, not its peak)
    n = np.arange(len(sc))
    pre = a_lo * np.cos(2 * np.pi * f_lo * n / fs + p_lo) + a_hi * np.cos(2 * np.pi * f_hi * n / fs + p_hi)
    true_ceiling_lin = 32768.0 * 10.0 ** (dsp_ref.LIMITER_CEILING_DB / 20.0)
    if perturb == "clipper":   # what a hard clipper at the ceiling would have made of the same sum
        sc = np.clip(pre, -true_ceiling_lin, true_ceiling_lin)
    (c_lo, c_hi), _, residue = fit_tones(sc, [f_lo, f_hi], fs)
    level_in = 20.0 * math.log10(float(np.max(np.abs(pre))) / 32768.0)
    got = 20.0 * math.log10(float(np.max(np.abs(sc))) / 32768.0)
    want_gain = 20.0 * math.log10(10.0 ** (ceiling / 20.0) * 32768.0 / float(np.max(np.abs(pre))))   # ceiling / peak of the sum, dB
    g_hi, g_lo = 20.0 * math.log10(c_hi / a_hi), 20.0 * math.log10(c_lo / a_lo)
    print("%s: poly (pads 12 + 24 at volume 60: %.2f and %.2f Hz, %.0f and %.0f of 32768 alone): the sum peaks at %+.2f dBFS, %.2f dB over the ceiling; the output peaks at %.3f dBFS (reference %.3f)" %
          (who, f_hi, f_lo, a_hi, a_lo, level_in, level_in - ceiling, got, dsp_ref.limiter_static_db(level_in, ceiling)))
    print("%s: poly: gain %+.3f dB at %.2f Hz, %+.3f dB at %.2f Hz, ceiling / peak of the sum %+.3f dB; what a fit of the two tones leaves: %.1f dB of them%s" %
          (who, g_hi, f_hi, g_lo, f_lo, want_gain, residue, "  (PERTURBED: a hard clipper at the ceiling)" if perturb == "clipper" else ""))
    rep.check(level_in - dsp_ref.LIMITER_CEILING_DB >= POLY_OVER_MIN, "%s poly: the sum of the voices is only %.2f dB over the ceiling (at least %.1f): the limiter has little to do" % (who, level_in - dsp_ref.LIMITER_CEILING_DB, POLY_OVER_MIN))
    rep.check(abs(got - dsp_ref.limiter_static_db(level_in, ceiling)) <= LIMITER_CEIL_TOL, "%s poly: input %.2f dBFS, output %.3f dBFS, the static curve gives %.3f (tolerance %.2f)" %
              (who, level_in, got, dsp_ref.limiter_static_db(level_in, ceiling), LIMITER_CEIL_TOL))
    rep.check(got <= dsp_ref.LIMITER_CEILING_DB + 1e-3, "%s poly: the output peak %.4f dBFS passes the ceiling %.4f" % (who, got, dsp_ref.LIMITER_CEILING_DB))
    gain = 0.5 * (g_hi + g_lo)
    rep.check(abs(gain - want_gain) <= POLY_GAIN_TOL, "%s poly: the limiter's gain is %+.3f dB, the ceiling over the peak of the sum gives %+.3f dB (tolerance %.1f)" % (who, gain, want_gain, POLY_GAIN_TOL))
    rep.check(abs(g_hi - g_lo) <= POLY_TONES_TOL, "%s poly: the gain is %+.3f dB at %.1f Hz and %+.3f dB at %.1f Hz: a gain scales both alike, this reshapes the wave (tolerance %.2f)" %
              (who, g_hi, f_hi, g_lo, f_lo, POLY_TONES_TOL))
    rep.check(residue <= POLY_RESIDUE_MAX, "%s poly: the distortion left after a fit of the two tones is %.1f dB of them, a limiter leaves under %.0f" % (who, residue, POLY_RESIDUE_MAX))


# ---- export ----------------------------------------------------------------------------------------------------------------------


EXPORT_TONES = [   # nominal frequency (Hz, only to find the tone), gain tolerance (dB), the least SNR of the difference (dB): the cubic interpolator's droop and error grow with the frequency
    (689.0, 0.02, 55.0),
    (1378.0, 0.02, 55.0),
    (2756.0, 0.03, 55.0),
    (5512.0, 0.10, 36.0),
    (9267.0, 0.40, 20.0),
]
EXPORT_FREQ_TOL_CENTS = 0.05
EXPORT_DELAY_TOL = 0.01   # samples at 44.1 kHz: the export lies where the live render puts it


def tone_fit(x, f, fs):
    """Least squares amplitude and phase of a sine of frequency f in x, under a Hann window."""
    n = np.arange(len(x))
    w = np.hanning(len(x))
    a = np.stack([np.cos(2 * np.pi * f * n / fs), np.sin(2 * np.pi * f * n / fs)], 1) * w[:, None]
    coef, *_ = np.linalg.lstsq(a, x * w, rcond=None)
    return math.hypot(coef[0], coef[1]), math.atan2(-coef[1], coef[0])


def tone_frequency(x, f0, fs):
    """The frequency near f0 at which the fit is best (a search over +-1 % in 0.005 % steps)."""
    best_a, best_f = -1.0, f0
    for f in np.linspace(f0 * 0.99, f0 * 1.01, 401):
        a, _ = tone_fit(x, f, fs)
        if a > best_a:
            best_a, best_f = a, f
    return best_f


def run_export(rep, who, renders, perturb, amount=None):
    import dsp_ref
    live, exported = renders["live"], renders["export"]
    rep.check(exported.rate == 44100, "%s: the file says %d Hz, not 44,100" % (who, exported.rate))
    fs = 44100.0
    up, down = (1, 1) if round(live.rate) == 44100 else (2499, 2500)   # the engine at 44,100 Hz is the file's rate: the export is the identity
    if perturb == "ratio":   # a ratio that is wrong on purpose: 1000 / 1001 by default, `ratio=<up>/<down>` otherwise (2922 / 2923 is 0.1 cent from the exact one)
        up, down = (int(v) for v in amount.split("/")) if amount else (1000, 1001)
    ref = dsp_ref.export_reference(live.left, up, down)
    ol, oe = L.onset(ref), L.onset(exported.left)
    print("%s: tone | frequency here and in the reference | gain of the file against the reference (dB) | delay (samples) | SNR of the difference (dB)" % who)
    for i, (f0, gain_tol, snr_min) in enumerate(EXPORT_TONES):
        a, b = int((i * 0.240 + 0.050) * fs), int((i * 0.240 + 0.210) * fs)
        seg_r = ref[ol + a:ol + b]
        seg_e = exported.left[oe + a:oe + b]
        f_ref, f_exp = tone_frequency(seg_r, f0, fs), tone_frequency(seg_e, f0, fs)
        a_ref, p_ref = tone_fit(seg_r, f_ref, fs)
        a_exp, p_exp = tone_fit(seg_e, f_ref, fs)
        dphi = (p_exp - p_ref + math.pi) % (2.0 * math.pi) - math.pi
        delay = -dphi / (2.0 * math.pi * f_ref / fs)
        gain = 20.0 * math.log10(a_exp / a_ref)
        cents = 1200.0 * math.log2(f_exp / f_ref)
        err = seg_e - seg_r
        snr = 20.0 * math.log10(math.sqrt(float(np.mean(seg_r ** 2))) / max(1e-9, math.sqrt(float(np.mean(err ** 2)))))
        print("  %7.0f Hz: %9.3f / %9.3f Hz (%+.3f cents)  gain %+.3f dB  delay %+.4f  SNR %.1f dB" % (f0, f_exp, f_ref, cents, gain, delay, snr))
        rep.check(abs(cents) <= EXPORT_FREQ_TOL_CENTS, "%s %g Hz: the file's tone is %.3f cents off the reference's (%.3f against %.3f Hz; tolerance %.2f)" % (who, f0, cents, f_exp, f_ref, EXPORT_FREQ_TOL_CENTS))
        rep.check(abs(gain) <= gain_tol, "%s %g Hz: the file's tone is %+.3f dB against the reference (tolerance %.2f)" % (who, f0, gain, gain_tol))
        rep.check(abs(delay) <= EXPORT_DELAY_TOL, "%s %g Hz: the file lies %+.4f samples from where the live render puts it (tolerance %.2f)" % (who, f0, delay, EXPORT_DELAY_TOL))
        rep.check(snr >= snr_min, "%s %g Hz: the difference from the reference is only %.1f dB under the tone (at least %.0f)" % (who, f0, snr, snr_min))


# ---- main -----------------------------------------------------------------------------------------------------------------------


KINDS = {
    "filter": ("e2e_ref_dsp_filter", run_filter, ("cutoff", "q"), 20),
    "limiter": ("e2e_ref_dsp_limiter", run_limiter, ("ceiling", "clipper"), 20),
    "export": ("e2e_ref_dsp_export", run_export, ("ratio",), 15),
}


def main(argv):
    perturb = amount = None
    args = list(argv[1:])
    if "--perturb" in args:
        i = args.index("--perturb")
        perturb, amount = L.parse_perturb(args[i + 1])
        del args[i:i + 2]
    if len(args) != 5 or args[0] not in KINDS:
        print(__doc__)
        return 2
    kind, o3sim, cards, scenarios, workdir = args
    scenario, run, perturbs, minimum = KINDS[kind]
    if perturb is not None and perturb not in perturbs:
        print("e2e_dsp_%s: --perturb %s does not apply (%s)" % (kind, perturb, ", ".join(perturbs)))
        return 2
    who = "e2e_dsp_" + kind
    code = L.need_numpy(who)
    if code is not None:
        return code
    code = need_scipy(who)
    if code is not None:
        return code
    try:
        renders, _ = L.run_journey(o3sim, os.path.join(scenarios, scenario + ".txt"), cards, workdir)
    except RuntimeError as e:
        print("%s: %s" % (who, e))
        return 1
    rep = L.Report(who)
    run(rep, who, renders, perturb, amount)
    return rep.finish(minimum=minimum)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
