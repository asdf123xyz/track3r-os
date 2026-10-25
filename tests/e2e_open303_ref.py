#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Open303 as the reference player of the TB-303 journey (OS-E2E-9, docs/specs/OS-E2E.md "E2E-9").

  e2e_open303_ref.py O3SIM CARDS SCENARIOS WORKDIR O3REF303 [--perturb cutoff[=Hz]|accent]

The journey `e2e_ref_303.txt` (`document tb303`, `audio on`, Play) plays an acid line through the REAL path: the sequencer, the engine's module voices, the module (modules/tb303, the host shared
object of the same source as the .tmod, bound as the unit's binder binds a project's module instrument), the graph and the master. `render acid` captures it. The ORIGINAL Open303 (double precision,
third_party/open303, MIT) renders the same notes at the engine's rate with the same parameters: O3REF303 is tests/tools/o3ref303_tool, the oracle of tests/tb303_oracle_test.cpp as a command. The
notes and the parameters are read from sim/include/o3os/sim/demo_project.h (kTb303Line, kTb303Params, the tempo of fill_tb303_project), so the document and the reference cannot drift apart.

The comparison is the one tests/tb303_oracle_test.cpp makes of the whole voice, with the same measures (the port differs from the original on purpose: PolyBLEP for the wavetables, no 4x
oversampling; that test's bounds, level mean 1.0 dB and max 8 dB, spectrum mean 1.5 dB and p95 8 dB, cover four settings of the voice; this journey is one, a saw at 600 Hz with resonance 70 %, and measures
level mean 0.06 dB, max 2.8 dB in a 10 ms window on a transient, spectrum mean 0.13 dB, p95 0.5 dB, pitch within 0.2 cent, take rms +0.46 dB. The simulator is deterministic (o3sim --twice: the same render
bit for bit), so the bounds here are about twice the measurement and not a margin for noise):
  level      the rms of 10 ms windows, in dB, where the reference is above -60 dB of its peak: mean difference under 0.12 dB, largest under 5 dB (a transient: one window)
  spectrum   the energy of third-octave bands (100 Hz to 9 kHz) in 2,048 sample Hann windows where the reference holds more than -30 dB of the window: mean under 0.25 dB, 95th percentile under 1 dB
  pitch      the fundamental of each note that lasts 90 ms or more, by autocorrelation: within 0.4 cent of the reference's (the same note, the same tuning)
  gain       the whole take's rms against the reference's: within 1 dB (measured +0.46: the module's volume parameter is dB at the output, the graph is unity at the centre)
  --perturb  breaks the REFERENCE on purpose (ctests e2e_ref_open303_break* have PASS_REGULAR_EXPRESSION): cutoff (the reference's cutoff 600 Hz -> 900 Hz; `cutoff=620` is 3 % up, and moves the
             spectrum to 0.46 dB mean and 2.2 dB at the 95th percentile, twice the bounds), accent (the reference plays its accented notes without the accent)
Without numpy the script exits 77 (a skip), and with O3_CI=1 in the environment (tools/ci.sh sets it) that is a failure.
"""
import math
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import e2e_ref_lib as L  # noqa: E402

np = L.np
ENV_WINDOW_MS = 10.0
ENV_FLOOR = 1e-3          # a window under this of the reference's loudest is not compared
SPEC_WINDOW = 2048
SPEC_FLOOR = 1e-3         # of the window's total power
ENV_MEAN_MAX = 0.12       # measured 0.06
ENV_PEAK_MAX = 5.0        # measured 2.8
SPEC_MEAN_MAX = 0.25      # measured 0.13
SPEC_P95_MAX = 1.0        # measured 0.49
PITCH_TOL_CENTS = 0.4     # measured 0.18
GAIN_TOL_DB = 1.0         # measured 0.46


def read_document(header_path):
    """(line, params, tempo_x100) from sim/include/o3os/sim/demo_project.h and sim/src/demo_project.cpp."""
    with open(header_path) as f:
        text = f.read()
    body = re.search(r"kTb303Line\[\] = \{(.*?)\};", text, re.S).group(1)
    line = [tuple(int(x) for x in m) for m in re.findall(r"\{(\d+), (\d+), (\d+)\}", body)]
    params = [float(x.rstrip("f")) for x in re.search(r"kTb303Params\[9\] = \{([^}]*)\}", text).group(1).split(",")]
    src = os.path.join(os.path.dirname(header_path), "..", "..", "..", "src", "demo_project.cpp")
    with open(os.path.normpath(src)) as f:
        cpp = f.read()
    tempo = int(re.search(r"fill_tb303_project[\s\S]*?set_tempo_x100\((\d+)\)", cpp).group(1))
    if len(line) < 5 or len(params) != 9:
        raise RuntimeError("the TB-303 document could not be read from %s" % header_path)
    return line, params, tempo


def reference_events(line, tempo_x100, rate, perturb=None):
    step = 60.0 / (tempo_x100 / 100.0) / 4.0 * rate            # samples in a 16th
    # (the accent break plays every note of the reference without its accent)
    return ["%d,%d,%d" % (int(round(r * step)), midi, (min(vel, 96) if perturb == "accent" else vel) if midi else 0) for r, midi, vel in line]


def render_reference(tool, workdir, params, events, rate, samples, perturb, amount=None):
    out = os.path.join(workdir, "open303.f32")
    p = list(params)
    if perturb == "cutoff":
        p[1] = L.number(amount, 900.0)
    cmd = [tool, "render", repr(rate), str(samples), out] + ["%g" % v for v in p[:8]] + events
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    if r.returncode != 0 or "render ok" not in r.stdout:
        raise RuntimeError("o3ref303_tool failed: %s" % (r.stdout + r.stderr)[:300])
    return np.fromfile(out, dtype=np.float32).astype(np.float64)


def third_octave_bands(rate):
    fc, edges = 100.0, []
    while fc < 9000.0:
        edges.append((fc / 1.1224620483093730, fc * 1.1224620483093730))
        fc *= 1.2599210498948732
    return edges


def distance(ref, ours, rate):
    """(level mean dB, level max dB, windows, spectrum mean dB, spectrum p95 dB, bands) between the reference and the capture, both aligned and equally long."""
    n = min(len(ref), len(ours))
    ref, ours = ref[:n], ours[:n]
    w = int(round(ENV_WINDOW_MS * 0.001 * rate))
    count = n // w
    rr = np.sqrt(np.mean(ref[:count * w].reshape(count, w) ** 2, axis=1))
    ro = np.sqrt(np.mean(ours[:count * w].reshape(count, w) ** 2, axis=1))
    sel = rr > ENV_FLOOR * np.max(rr)
    db = np.abs(20.0 * np.log10((ro[sel] + 1e-12) / rr[sel]))
    hann = np.hanning(SPEC_WINDOW + 1)[:-1]
    f = np.fft.rfftfreq(SPEC_WINDOW, 1.0 / rate)
    bands = third_octave_bands(rate)
    diffs, peak_power = [], 0.0
    spectra = []
    for i in range(0, n - SPEC_WINDOW + 1, SPEC_WINDOW):
        a = np.abs(np.fft.rfft(ref[i:i + SPEC_WINDOW] * hann)) ** 2
        b = np.abs(np.fft.rfft(ours[i:i + SPEC_WINDOW] * hann)) ** 2
        spectra.append((a, b))
        peak_power = max(peak_power, float(np.sum(a)))
    for a, b in spectra:
        total = float(np.sum(a))
        if total < 1e-4 * peak_power:
            continue
        for lo, hi in bands:
            m = (f >= lo) & (f <= hi)
            pa, pb = float(np.sum(a[m])), float(np.sum(b[m]))
            if pa < SPEC_FLOOR * total:
                continue
            diffs.append(abs(10.0 * math.log10((pb + 1e-30) / pa)))
    diffs.sort()
    return (float(np.mean(db)), float(np.max(db)), int(np.sum(sel)), float(np.mean(diffs)) if diffs else 0.0, diffs[int(len(diffs) * 95 / 100)] if diffs else 0.0, len(diffs))


def main(argv):
    perturb = amount = None
    args = list(argv[1:])
    if "--perturb" in args:
        i = args.index("--perturb")
        perturb, amount = L.parse_perturb(args[i + 1])
        del args[i:i + 2]
    if len(args) != 5 or perturb not in (None, "cutoff", "accent") or (amount is not None and perturb == "accent"):
        print(__doc__)
        return 2
    o3sim, cards, scenarios, workdir, tool = args
    who = "e2e_open303"
    code = L.need_numpy(who)
    if code is not None:
        return code
    header = os.path.normpath(os.path.join(scenarios, "..", "..", "include", "o3os", "sim", "demo_project.h"))
    line, params, tempo = read_document(header)
    try:
        renders, _ = L.run_journey(o3sim, os.path.join(scenarios, "e2e_ref_303.txt"), cards, workdir)
    except RuntimeError as e:
        print("%s: %s" % (who, e))
        return 1
    wav = renders["acid"]
    rate = L.engine_rate(wav)
    ours_all = wav.mono() / 32768.0   # full scale 1.0, as the reference
    on = L.onset(ours_all)
    samples = len(ours_all) - on
    events = reference_events(line, tempo, rate, perturb)
    ref = render_reference(tool, workdir, params, events, rate, samples + 4000, perturb, amount)
    ro = L.onset(ref)
    ours = ours_all[on:]
    ref = ref[ro:ro + len(ours)]
    ours = ours[:len(ref)]
    rep = L.Report(who)
    lm, lx, nw, sm, sp, nb = distance(ref, ours, rate)
    gain = 20.0 * math.log10(float(np.sqrt(np.mean(ours ** 2))) / float(np.sqrt(np.mean(ref ** 2))))
    print("%s: the reference notes %s; cutoff %g Hz, resonance %g %%" % (who, " ".join(events), params[1], params[2]))
    print("%s: level mean %.2f dB, max %.2f dB (%d windows of %g ms); spectrum mean %.2f dB, p95 %.2f dB (%d bands); take rms %+.2f dB against the reference" % (who, lm, lx, nw, ENV_WINDOW_MS, sm, sp, nb, gain))
    rep.check(nw > 100, "%s: only %d windows of the level were compared" % (who, nw))
    rep.check(nb > 200, "%s: only %d bands of the spectrum were compared" % (who, nb))
    rep.check(lm < ENV_MEAN_MAX, "%s: the level is %.2f dB off on average (at most %g)" % (who, lm, ENV_MEAN_MAX))
    rep.check(lx < ENV_PEAK_MAX, "%s: the level is %.2f dB off in its worst window (at most %g)" % (who, lx, ENV_PEAK_MAX))
    rep.check(sm < SPEC_MEAN_MAX, "%s: the spectrum is %.2f dB off on average (at most %g)" % (who, sm, SPEC_MEAN_MAX))
    rep.check(sp < SPEC_P95_MAX, "%s: the spectrum is %.2f dB off at the 95th percentile (at most %g)" % (who, sp, SPEC_P95_MAX))
    rep.check(abs(gain) <= GAIN_TOL_DB, "%s: the take is %+.2f dB against the reference's level (at most %g)" % (who, gain, GAIN_TOL_DB))
    # the pitch of each note that lasts long enough, from 20 ms after its start to 20 ms before its end
    step = 60.0 / (tempo / 100.0) / 4.0 * rate
    notes = [(r, m, v) for r, m, v in line]
    for k, (r, m, v) in enumerate(notes):
        if m == 0:
            continue
        end_row = notes[k + 1][0] if k + 1 < len(notes) else 16
        length_ms = (end_row - r) * step / rate * 1000.0
        if length_ms < 90.0:
            continue
        a, b = int((r * step) + 0.020 * rate), int((end_row * step) - 0.020 * rate)
        lo = 27.5 * 2 ** ((m - 21) / 12.0) * 0.8
        p_ours = L.pitch_ac(ours[a:b], rate, lo=lo, hi=lo * 3.0)
        p_ref = L.pitch_ac(ref[a:b], rate, lo=lo, hi=lo * 3.0)
        cents = L.cents(p_ours, p_ref) if p_ours > 0 and p_ref > 0 else float("nan")
        print("  note %d at step %d (%.1f Hz nominal): here %.3f Hz, reference %.3f Hz (%+.2f cents)" % (m, r, 440.0 * 2 ** ((m - 69) / 12.0), p_ours, p_ref, cents))
        rep.check(p_ours > 0 and p_ref > 0 and abs(cents) <= PITCH_TOL_CENTS, "%s: note %d at step %d: %.3f Hz here, %.3f Hz in Open303 (%+.2f cents, tolerance %g)" % (who, m, r, p_ours, p_ref, cents, PITCH_TOL_CENTS))
    return rep.finish(minimum=8)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
