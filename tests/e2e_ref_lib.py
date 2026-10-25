#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Shared parts of the reference-player tests of the end-to-end battery (OS-E2E-9, docs/specs/OS-E2E.md "E2E-9"): a journey is run in the simulator (`o3sim --scenario ... --wav DIR`), the WAV
files of its renders are read back, and a reference player's own render of the same bytes is what they are compared with.

  run_journey(o3sim, scenario, cards, outdir)   runs one scenario of sim/scenarios/e2e/ and returns {render name: Wav}
  Wav                                            a render: .rate, .left, .right (numpy float64, int16 units), .mono()
  tick_track(x, rate, ticks, ...)                [(pitch Hz, rms)] per tick of a song: the pitch by autocorrelation (the Amiga voice's staircase has images that fool a zero-crossing counter)
  Reference                                      libopenmpt, loaded with ctypes from the system (nothing vendored), with its render parameters and its ctls
  Report                                         the checks of a test: counts them, keeps the failures, prints a table
  parse_perturb(text)                            `--perturb kind` or `kind=value`: how far a break proof moves its reference (tests/e2e_*_ref.py)

Without numpy or libopenmpt a test exits 77 (a skip), and with O3_CI=1 in the environment (tools/ci.sh sets it) that is a failure, as for the oracles of the formats. The same goes for a
libopenmpt outside TESTED_VERSIONS: the figures of the tests were measured on 0.7.3, and a newer mixer may move them, so it is an "untested version" (a skip, a failure under O3_CI=1)
until someone re-measures and widens the range. `python3 -I tests/e2e_ref_lib.py --self-test` checks the helpers (ctest e2e_ref_lib_selftest).
"""
import array
import ctypes
import glob
import math
import os
import re
import subprocess
import sys
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "tools"))

SKIP = 77
STRICT = os.environ.get("O3_CI") == "1"
# The libopenmpt versions the tolerances were measured on: [first, end). 0.7.3 is what Ubuntu 24.04 ships; the 0.7 series are bug-fix releases of one mixer, 0.8 is a new minor (the mixer, the
# Amiga resampler and the format importers change between minors), so it is not assumed to agree: a test says "untested version" until the figures are measured again.
TESTED_VERSIONS = ((0, 7, 0), (0, 8, 0))


def skip(who, why):
    """Exit code for a missing reference: a skip, except under O3_CI=1 (a failure)."""
    if STRICT:
        print("%s: %s, and O3_CI=1 says this test may not be skipped" % (who, why))
        return 1
    print("%s: %s, skipped" % (who, why))
    return SKIP


try:
    import numpy as np
except ImportError:  # pragma: no cover - the caller reports it through need_numpy()
    np = None


def need_numpy(who):
    """None when numpy is there, else the exit code to leave with."""
    return None if np is not None else skip(who, "numpy is not installed here (apt: python3-numpy)")


# ---- the simulator --------------------------------------------------------------------------------------------------------


class Wav:
    def __init__(self, rate, left, right):
        self.rate = rate
        self.left = left
        self.right = right

    def mono(self):
        return (self.left + self.right) / 2.0

    def __len__(self):
        return len(self.left)


def engine_rate(live):
    """The engine's true rate of a live capture: 44,100 Hz when the capture says 44,100 (the default clock, 1800000/441 cycles a sample), else the
    pre-1.0 clock's 750,000 / 17 = 44,117.647 Hz (4080 cycles a sample; its header says 44,118)."""
    return 44100.0 if round(live.rate) == 44100 else 750000.0 / 17.0


def read_wav(path):
    with wave.open(path) as w:
        if w.getsampwidth() != 2 or w.getnchannels() != 2:
            raise RuntimeError("%s: not a 16-bit stereo WAV" % path)
        rate = w.getframerate()
        a = array.array("h")
        a.frombytes(w.readframes(w.getnframes()))
    v = np.asarray(a, dtype=np.float64)
    return Wav(rate, v[0::2], v[1::2])


def run_journey(o3sim, scenario, cards, outdir, extra=()):
    """Runs the scenario once (the ctest scenario_<name> already runs it twice) and returns ({render name: Wav}, o3sim's stdout). Raises RuntimeError when it fails."""
    os.makedirs(outdir, exist_ok=True)
    stem = os.path.splitext(os.path.basename(scenario))[0]
    for old in glob.glob(os.path.join(outdir, stem + "_*.wav")):
        os.remove(old)
    r = subprocess.run([o3sim, "--scenario", scenario, "--cards", cards, "--wav", outdir] + list(extra), capture_output=True, text=True, timeout=300)
    if r.returncode != 0:
        raise RuntimeError("o3sim %s failed (exit %d): %s" % (os.path.basename(scenario), r.returncode, (r.stdout + r.stderr)[-600:]))
    out = {}
    for path in sorted(glob.glob(os.path.join(outdir, stem + "_*.wav"))):
        out[os.path.basename(path)[len(stem) + 1:-4]] = read_wav(path)
    return out, r.stdout


# ---- measuring --------------------------------------------------------------------------------------------------------------


def onset(x, fraction=0.05):
    """The first sample at which |x| passes `fraction` of its peak (None for silence)."""
    peak = float(np.max(np.abs(x))) if len(x) else 0.0
    if peak <= 0.0:
        return None
    return int(np.argmax(np.abs(x) > fraction * peak))


def pitch_ac(x, rate, lo=100.0, hi=1500.0):
    """The fundamental of a window by normalised autocorrelation with a parabolic peak (0 when there is none). A staircase (Paula's sample and hold) has images at every multiple of its rate
    that make a zero-crossing counter see extra crossings; the autocorrelation sees the period of the whole waveform."""
    n = len(x)
    min_lag = int(rate / hi)
    max_lag = min(int(rate / lo), n - 2 * 8)
    if max_lag <= min_lag + 2:
        return 0.0
    x = x - np.mean(x)
    size = 1 << (2 * n - 1).bit_length()
    f = np.fft.rfft(x, size)
    ac = np.fft.irfft(f * np.conj(f), size)[:max_lag + 2]
    # the normalisation of the overlap: the energy of the two parts that meet at each lag
    sq = np.concatenate(([0.0], np.cumsum(x * x)))
    lags = np.arange(len(ac))
    energy = np.sqrt(np.maximum((sq[n] - sq[lags]) * (sq[n - lags] - sq[0]), 1e-30))
    nac = ac / energy
    window = nac[min_lag:max_lag + 1]
    if len(window) < 3 or float(np.max(window)) < 0.5:
        return 0.0
    # the first peak that is within 5 % of the highest: the fundamental, not a multiple of its period
    top = float(np.max(window))
    k = None
    for i in range(1, len(window) - 1):
        if window[i] >= window[i - 1] and window[i] > window[i + 1] and window[i] >= 0.95 * top:
            k = i
            break
    if k is None:
        k = int(np.argmax(window))
        if k == 0 or k == len(window) - 1:
            return 0.0
    a, b, c = window[k - 1], window[k], window[k + 1]
    den = a - 2.0 * b + c
    off = 0.5 * (a - c) / den if den != 0.0 else 0.0
    return rate / (min_lag + k + off)


def tick_track(x, rate, ticks, tick_s=0.02, lo=100.0, hi=1500.0, head=0.12, tail=0.04):
    """[(pitch Hz, rms)] for each tick of a song that starts at x[0]: the middle of the tick (the first `head` and last `tail` of it are left out: the edges of a tick are where a level or a pitch
    changes, and a window that straddles two is measured as neither), the pitch by autocorrelation and the rms of the same window."""
    out = []
    for t in range(ticks):
        a = int(round((t + head) * tick_s * rate))
        b = int(round((t + 1 - tail) * tick_s * rate))
        w = x[a:b]
        if len(w) < 50:
            out.append((0.0, 0.0))
            continue
        out.append((pitch_ac(w, rate, lo, hi), float(math.sqrt(float(np.mean(w * w))))))
    return out


def cents(a, b):
    """1200 log2(a / b); NaN when either is not a pitch (0 is "none found"), which no tolerance accepts: a check reports it as a disagreement instead of the test raising."""
    if not (a > 0.0 and b > 0.0):
        return float("nan")
    return 1200.0 * math.log2(a / b)


def parse_perturb(text):
    """(kind, value text or None) of a --perturb argument: `pitch` or `pitch=3`. A break proof without a value moves its reference a long way (10 to 50 times the tolerance: it proves that
    the comparison can fail); `kind=value` moves it by a stated amount, which the CMake tests `_break_near_*` use for about twice the tolerance (it proves how close to the tolerance it fails)."""
    kind, _, value = text.partition("=")
    return kind, (value if value else None)


def number(text, default):
    """A float from a --perturb value, `default` when there is none."""
    return default if text is None else float(text)


# ---- libopenmpt --------------------------------------------------------------------------------------------------------------


def version_verdict(version):
    """'' when libopenmpt `version` (major, minor, patch) is one the tolerances were measured on, else why not (it starts with "untested version")."""
    first, end = TESTED_VERSIONS
    if first <= tuple(version[:3]) < end:
        return ""
    return "untested version: libopenmpt %d.%d.%d (the figures were measured on 0.7.3 and the range tested is %d.%d.%d up to %d.%d.%d; measure again, then widen TESTED_VERSIONS in tests/e2e_ref_lib.py)" % (
        tuple(version[:3]) + first + end)


class Reference:
    """libopenmpt through ctypes. `Reference.open()` is None when the library is not there (or too old: `why` says)."""

    def __init__(self, lib):
        import med_openmpt_oracle as mo
        import modimp_openmpt_oracle as mi
        self.lib = lib
        self.omp = mi.Omp(lib)
        c = ctypes
        lib.openmpt_module_read_interleaved_float_stereo.restype = c.c_size_t
        lib.openmpt_module_read_interleaved_float_stereo.argtypes = [c.c_void_p, c.c_int32, c.c_size_t, c.POINTER(c.c_float)]
        lib.openmpt_module_ctl_set_text.restype = c.c_int
        lib.openmpt_module_ctl_set_text.argtypes = [c.c_void_p, c.c_char_p, c.c_char_p]
        lib.openmpt_module_ctl_set_boolean.restype = c.c_int
        lib.openmpt_module_ctl_set_boolean.argtypes = [c.c_void_p, c.c_char_p, c.c_int]
        lib.openmpt_module_ctl_set_floatingpoint.restype = c.c_int
        lib.openmpt_module_ctl_set_floatingpoint.argtypes = [c.c_void_p, c.c_char_p, c.c_double]
        lib.openmpt_module_ctl_get_text.restype = c.c_void_p
        lib.openmpt_module_ctl_get_text.argtypes = [c.c_void_p, c.c_char_p]
        lib.openmpt_module_get_ctls.restype = c.c_void_p
        lib.openmpt_module_get_ctls.argtypes = [c.c_void_p]
        lib.openmpt_free_string.argtypes = [c.c_void_p]
        lib.openmpt_module_set_render_param.restype = c.c_int
        lib.openmpt_module_set_render_param.argtypes = [c.c_void_p, c.c_int, c.c_int32]
        self.version = self.omp.version()

    @staticmethod
    def open():
        """(Reference or None, why)"""
        import med_openmpt_oracle as mo
        lib = mo.load_library()
        if lib is None:
            return None, "libopenmpt is not installed here (apt: libopenmpt0t64)"
        ref = Reference(lib)
        why = version_verdict(ref.version)
        if why:
            return None, why
        return ref, ""

    def render(self, data, rate, frames, ctls=None, gain_millibel=None, stereo_separation=None):
        """Stereo float render of `data` (a module's bytes) at `rate`: (left, right) as numpy arrays in libopenmpt's float scale (1.0 = full scale). `ctls` is {ctl: text} set before the render
        (render.resampler.emulate_amiga: a bool, set with the boolean setter; ...)."""
        mod, buf = self.omp.load(data)
        try:
            for k, v in (ctls or {}).items():
                # (a boolean ctl set as text is accepted and does nothing: the type of the ctl decides which setter works)
                if isinstance(v, bool):
                    ok = self.lib.openmpt_module_ctl_set_boolean(mod, k.encode(), 1 if v else 0)
                elif isinstance(v, float):
                    ok = self.lib.openmpt_module_ctl_set_floatingpoint(mod, k.encode(), v)
                else:
                    ok = self.lib.openmpt_module_ctl_set_text(mod, k.encode(), str(v).encode())
                if ok == 0:
                    raise RuntimeError("libopenmpt has no ctl '%s' (or refused '%s')" % (k, v))
            if gain_millibel is not None:
                self.lib.openmpt_module_set_render_param(mod, 1, gain_millibel)  # OPENMPT_MODULE_RENDER_MASTERGAIN_MILLIBEL
            if stereo_separation is not None:
                self.lib.openmpt_module_set_render_param(mod, 2, stereo_separation)  # OPENMPT_MODULE_RENDER_STEREOSEPARATION_PERCENT
            out = (ctypes.c_float * (frames * 2))()
            n = self.lib.openmpt_module_read_interleaved_float_stereo(mod, rate, frames, out)
            v = np.frombuffer(out, dtype=np.float32, count=2 * n).astype(np.float64)
            return v[0::2], v[1::2]
        finally:
            self.omp.free(mod)

    def ctls(self, data):
        mod, buf = self.omp.load(data)
        try:
            ptr = self.lib.openmpt_module_get_ctls(mod)
            text = ctypes.string_at(ptr).decode()
            self.lib.openmpt_free_string(ptr)
            return text.split(";")
        finally:
            self.omp.free(mod)


# ---- reporting ----------------------------------------------------------------------------------------------------------------


class Report:
    def __init__(self, who):
        self.who = who
        self.count = 0
        self.failures = []

    def check(self, ok, what):
        self.count += 1
        if not ok:
            self.failures.append(what)
        return ok

    def finish(self, minimum):
        """The exit code: 0 when nothing failed and at least `minimum` comparisons ran (a test that lost its checks must not pass)."""
        for f in self.failures[:40]:
            print("DISAGREE: " + f)
        if len(self.failures) > 40:
            print("... and %d more" % (len(self.failures) - 40))
        print("%s: %d comparisons, %d disagree" % (self.who, self.count, len(self.failures)))
        if self.count < minimum:
            print("%s: only %d comparisons ran, at least %d are expected" % (self.who, self.count, minimum))
            return 1
        return 1 if self.failures else 0


# ---- self-test ----------------------------------------------------------------------------------------------------------------


def self_test():
    """The helpers that no journey exercises: the version range, the perturb argument, a pitch of 0. Runs without numpy and libopenmpt."""
    bad = []

    def expect(ok, what):
        if not ok:
            bad.append(what)

    for v, ok in (((0, 7, 3), True), ((0, 7, 0), True), ((0, 7, 99), True), ((0, 6, 9), False), ((0, 8, 0), False), ((0, 8, 1), False), ((1, 0, 0), False)):
        why = version_verdict(v)
        expect((why == "") == ok, "version %s: verdict '%s'" % (v, why))
        expect(ok or why.startswith("untested version"), "version %s: the reason should say 'untested version': '%s'" % (v, why))
    expect(parse_perturb("pitch") == ("pitch", None), "parse_perturb('pitch')")
    expect(parse_perturb("pitch=3") == ("pitch", "3"), "parse_perturb('pitch=3')")
    expect(parse_perturb("ratio=2922/2923") == ("ratio", "2922/2923"), "parse_perturb('ratio=2922/2923')")
    expect(number(None, 2.5) == 2.5 and number("3", 2.5) == 3.0, "number()")
    expect(math.isnan(cents(0.0, 440.0)) and math.isnan(cents(440.0, 0.0)) and abs(cents(880.0, 440.0) - 1200.0) < 1e-9, "cents() with a pitch of 0")
    rep = Report("selftest")
    rep.check(not (math.nan <= 1.5), "a NaN is never inside a tolerance")
    expect(rep.failures == [] and rep.count == 1, "Report counts")
    for b in bad:
        print("e2e_ref_lib self-test: " + b)
    print("e2e_ref_lib self-test: %s" % ("FAILED" if bad else "ok"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(self_test() if sys.argv[1:] == ["--self-test"] else 2)
