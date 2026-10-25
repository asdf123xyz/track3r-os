# SPDX-License-Identifier: GPL-3.0-or-later
"""The starter wavetables: five morphs of 32 windows of 2,048 samples (16-bit mono WAV, CC0, band-limited additive synthesis).

The file is what the OS reads as a wavetable (docs/manual/wave.md; formats/wav_sample.h): a WAV of equal windows one after another,
carrying Serum's `clm ` marker with the window size, so that "WT AUTO" on the Sample load screen takes it as a wavetable.
"""
from wavio import RATE, sin_turns, to_pcm16, wav_bytes

WINDOW = 2048
FRAMES = 32
HARMONICS = 32          # the highest harmonic of any window: well below the Nyquist limit of 1,024


def _sine_table():
    return [sin_turns(i / WINDOW) for i in range(WINDOW)]


def _frame(amps, sin_t, phases=None):
    """One window from the amplitude of each harmonic (amps[h-1]); `phases` gives each harmonic's offset in table steps (0 = sine)."""
    out = [0.0] * WINDOW
    for h, a in enumerate(amps, start=1):
        if a == 0.0:
            continue
        off = phases[h - 1] if phases else 0
        for n in range(WINDOW):
            out[n] += a * sin_t[(h * n + off) % WINDOW]
    return out


def _sine_to_saw(m):
    return [(1.0 - m if h == 1 else 0.0) + m / h for h in range(1, HARMONICS + 1)], None


def _sine_to_square(m):
    return [((1.0 - m) if h == 1 else 0.0) + (m / h if h & 1 else 0.0) for h in range(1, HARMONICS + 1)], None


def _pwm(m):
    # a pulse of duty d (0.5 down to 0.06): harmonic h is 2/(pi h) sin(pi h d), shifted so that the pulse is centred
    d = 0.5 - 0.44 * m
    amps = []
    phases = []
    for h in range(1, HARMONICS + 1):
        c = sin_turns(h * d / 2.0) * (2.0 / 3.141592653589793) / h     # sin(pi h d) = sin(2 pi (h d / 2))
        amps.append(c)
        # cos(2 pi h (x - d / 2)): a quarter turn (512 table steps) turns the sine table into a cosine; the shift is h d / 2 turns
        phases.append(int(512 - h * d * (WINDOW / 2.0) + 0.5 + WINDOW * 64) % WINDOW)
    return amps, phases


def _formant(m):
    # 1/h harmonics under a resonance peak that moves from the 2nd to the 15th harmonic
    centre = 2.0 + 13.0 * m
    width = 1.5 + 2.0 * m
    amps = []
    for h in range(1, HARMONICS + 1):
        x = (h - centre) / width
        amps.append((1.0 / h) * (0.15 + 1.0 / (1.0 + x * x)))
    return amps, None


def _harmonics(m):
    count = 1 + int(m * (HARMONICS - 1) + 0.5)
    return [1.0 / h if h <= count else 0.0 for h in range(1, HARMONICS + 1)], None


TABLES = [
    ("sine_to_saw", _sine_to_saw),
    ("sine_to_square", _sine_to_square),
    ("pwm_sweep", _pwm),
    ("formant_sweep", _formant),
    ("harmonic_series", _harmonics),
]


def _marker():
    return (b"clm ", b"<!>%d 20000000 wavetable (track3r-os)" % WINDOW)


def files():
    """{'name.wav': bytes}: each table as WAV, FRAMES windows of WINDOW samples, every window scaled to the same peak."""
    out = {}
    for name, shape in TABLES:
        sin_t = _sine_table()
        pcm = []
        for k in range(FRAMES):
            amps, phases = shape(k / (FRAMES - 1))
            pcm.extend(to_pcm16(_frame(amps, sin_t, phases), 0.9))
        out["wt_" + name + ".wav"] = wav_bytes(pcm, RATE, [_marker()])
    return out
