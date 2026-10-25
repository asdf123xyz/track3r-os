#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reference DSP for the end-to-end battery (OS-E2E-9, docs/specs/OS-E2E.md "E2E-9"): what the engine's filters, its 44.1 kHz export and its master limiter SHOULD do, worked out with scipy
from the textbook definitions and the numbers the engine's own headers publish. Nothing here runs the engine and nothing here is derived from its code: a journey in the simulator renders
the sound (sim/scenarios/e2e/e2e_ref_dsp_*.txt), tests/e2e_dsp_ref.py measures it and holds it to the curves below.

Run it with `python3 -I` (it needs numpy and scipy: apt python3-numpy python3-scipy). As a script it prints the reference curves, for a look.

  svf_cutoff_hz(c)                  the cutoff of the instrument filter for a stored cutoff c in 0..1 (audio/svf.h: index = int(c x 477), 20 Hz x 2^(index / 50))
  svf_q(r)                          the quality of a stored resonance r in 0..4.3 (Q = r + 0.7, held to 0.7..5)
  svf_response_db(f, c, r, kind)    the magnitude, in dB, of the analogue second-order state variable filter at cutoff c and resonance r: low-pass (0), high-pass (1) or band-pass (2),
                                    at the frequencies f in Hz (scipy.signal.freqs of the continuous-time prototype: the engine is a Chamberlin filter run at twice the sample rate, which
                                    approaches this prototype well below the sample rate and is a little lower above a few kHz because its output is the mean of two half steps)
  band_levels_db(response_db, f, edges)
                                    the mean power of a response in each band between `edges` (Hz), in dB: how a measured and a reference response are both reduced to a few numbers
  export_reference(x, rate_in)      the exported file's samples for engine samples x at rate_in: an exact rational resampler (scipy.signal.resample_poly) with a Kaiser window of 140 dB, from
                                    the engine's 750,000 / 17 Hz to 44,100 Hz (the ratio is 2499 / 2500 exactly)
  limiter_static_db(level_db, ceiling_db, threshold_db)
                                    the output level of the master limiter for a steady sine of `level_db` dBFS: unity below the threshold (when the slow stage is on) or the ceiling, then
                                    held at it. The look-ahead brickwall of audio/master_limiter.h keeps |out| <= ceiling (-0.3 dBFS) exactly; the slow stage (threshold, attack, release) is optional.
"""
import math
import sys

import numpy as np
import scipy.signal as ss

ENGINE_RATE = 44100.0   # hal::audio::sample_rate_mhz() / 1000 at the default clock (O3OS_RATE_44100 ON); the pre-1.0 clock was 750000 / 17 = 44,117.647 Hz (tests/e2e_ref_lib.py engine_rate reads it from a capture)
EXPORT_RATE = 44100.0
LIMITER_CEILING_DB = 20.0 * math.log10(0.96605088)   # -0.3 dBFS, MasterLimiter::kCeiling


def svf_cutoff_hz(c):
    index = int(c * 477.0)
    return 20.0 * 2.0 ** (index / 50.0)


def svf_q(r):
    return min(5.0, max(0.7, r + 0.7))


def svf_response_db(f, c, r, kind):
    """Magnitude in dB of the analogue prototype at the frequencies f (Hz, an array, all above 0)."""
    w0 = 2.0 * math.pi * svf_cutoff_hz(c)
    q = svf_q(r)
    den = [1.0, w0 / q, w0 * w0]
    num = {0: [w0 * w0], 1: [1.0, 0.0, 0.0], 2: [w0 / q, 0.0]}[kind]
    _, h = ss.freqs(num, den, worN=2.0 * math.pi * np.asarray(f, dtype=np.float64))
    return 20.0 * np.log10(np.maximum(np.abs(h), 1e-12))


def band_levels_db(response_db, f, edges):
    """Mean power of the response in each band [edges[i], edges[i+1]) in dB (nan for a band with no point)."""
    f = np.asarray(f)
    p = 10.0 ** (np.asarray(response_db) / 10.0)
    out = []
    for a, b in zip(edges[:-1], edges[1:]):
        sel = (f >= a) & (f < b)
        out.append(10.0 * math.log10(float(np.mean(p[sel]))) if np.any(sel) else float("nan"))
    return out


def export_reference(x, up=2499, down=2500):
    """The file a perfect resampler would write for engine samples x (floats in int16 units): 44,117.647 Hz (750,000 / 17) to 44,100 Hz is exactly 2499 / 2500 (the defaults; a test
    that wants a reference that is wrong on purpose passes another ratio)."""
    return ss.resample_poly(np.asarray(x, dtype=np.float64), up, down, window=("kaiser", 14.0))


def limiter_static_db(level_db, ceiling_db=LIMITER_CEILING_DB, threshold_db=None):
    """Output level for a steady sine at level_db: min(level, threshold (when the slow stage is on), ceiling)."""
    top = ceiling_db if threshold_db is None else min(ceiling_db, threshold_db)
    return min(level_db, top)


def main():
    edges = [100.0 * 2 ** k for k in range(8)] + [22000.0]
    f = np.linspace(20.0, 22000.0, 4000)
    for c, r, kind in ((0.45, 0.0, 0), (0.65, 0.0, 0), (0.65, 3.0, 0), (0.65, 0.0, 1)):
        print("filter c=%.2f (%.1f Hz) r=%.1f (Q %.1f) type %d: bands" % (c, svf_cutoff_hz(c), r, svf_q(r), kind),
              " ".join("%7.2f" % v for v in band_levels_db(svf_response_db(f, c, r, kind), f, edges)))
    print("limiter static curve (ceiling %.2f dBFS):" % LIMITER_CEILING_DB, " ".join("%g->%.2f" % (x, limiter_static_db(x)) for x in (-20, -6, -1, 0, 6, 12)))
    print("export resampler ratio: %d / %d" % (2499, 2500))
    return 0


if __name__ == "__main__":
    sys.exit(main())
