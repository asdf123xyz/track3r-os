# SPDX-License-Identifier: GPL-3.0-or-later
"""The starter drum kit: kick, snare, hats, clap, toms, rim and cowbell as 16-bit mono WAV at 44,100 Hz (synthesised, CC0).

Each drum is a short loop of plain arithmetic (decaying sine sweeps, filtered noise), so the bytes depend on nothing but this file.
`kit()` returns {name: float samples}; `files()` returns {file name: WAV bytes}.
"""
from wavio import RATE, Noise, sin_turns, to_pcm16, wav_bytes


def _kick(length, f_end, f_sweep, sweep_decay, amp_decay, seed):
    noise = Noise(seed)
    out = []
    phase = 0.0
    sweep = 1.0
    amp = 1.0
    for n in range(length):
        phase += (f_end + f_sweep * sweep) / RATE
        sweep *= sweep_decay
        v = sin_turns(phase) * amp
        if n < 48:
            v += 0.25 * noise.next() * (1.0 - n / 48.0)   # the beater's click
        out.append(v)
        amp *= amp_decay
    return out


def _tom(length, f_end, f_sweep):
    out = []
    phase = 0.0
    sweep = 1.0
    amp = 1.0
    for _ in range(length):
        phase += (f_end + f_sweep * sweep) / RATE
        sweep *= 0.9994
        out.append(sin_turns(phase) * amp)
        amp *= 0.99965
    return out


def _snare():
    noise = Noise(0x5EED0002)
    out = []
    phase = 0.0
    amp_tone = 1.0
    amp_noise = 1.0
    prev = 0.0
    for _ in range(int(0.28 * RATE)):
        phase += (185.0 + 60.0 * amp_tone) / RATE
        x = noise.next()
        hp = x - prev                                   # a one-sample difference: a crude high-pass
        prev = x
        out.append(0.55 * sin_turns(phase) * amp_tone + 0.6 * hp * amp_noise)
        amp_tone *= 0.9990
        amp_noise *= 0.99950
    return out


def _hat(length, decay, seed):
    noise = Noise(seed)
    out = []
    p1 = p2 = 0.0
    amp = 1.0
    for _ in range(length):
        x = noise.next()
        h1 = x - p1
        p1 = x
        h2 = h1 - p2                                    # twice differenced: little but the top end
        p2 = h1
        out.append(h2 * amp)
        amp *= decay
    return out


def _clap():
    noise = Noise(0x5EED0006)
    out = []
    prev = 0.0
    low = 0.0
    env = 1.0
    for n in range(int(0.30 * RATE)):
        x = noise.next()
        hp = x - prev
        prev = x
        low += 0.45 * (hp - low)                        # one-pole low-pass after the high-pass: a band around the middle
        if n in (441, 882):
            env = 1.0                                   # three quick bursts ...
        elif n == 1323:
            env = 0.5                                   # ... then the tail
        out.append(low * env)
        env *= 0.9985 if n < 1323 else 0.99965
    return out


def _rim():
    noise = Noise(0x5EED0007)
    out = []
    amp = 1.0
    for n in range(int(0.07 * RATE)):
        v = 0.6 * sin_turns(n * 1700.0 / RATE) + 0.6 * sin_turns(n * 480.0 / RATE)
        if n < 24:
            v += 0.5 * noise.next()
        out.append(v * amp)
        amp *= 0.9975
    return out


def _cowbell():
    out = []
    amp = 1.0
    for n in range(int(0.35 * RATE)):
        a = 1.0 if sin_turns(n * 562.0 / RATE) >= 0.0 else -1.0
        b = 1.0 if sin_turns(n * 845.0 / RATE) >= 0.0 else -1.0
        out.append((0.5 * a + 0.5 * b) * amp)
        amp *= 0.99982
    return out


def kit():
    """{name: float samples}, in the order of the kit."""
    return {
        "kick": _kick(int(0.45 * RATE), 48.0, 150.0, 0.9992, 0.99987, 0x5EED0001),
        "kick_long": _kick(int(0.80 * RATE), 38.0, 110.0, 0.9996, 0.99993, 0x5EED0011),
        "snare": _snare(),
        "hat_closed": _hat(int(0.08 * RATE), 0.9993, 0x5EED0003),
        "hat_open": _hat(int(0.40 * RATE), 0.99985, 0x5EED0004),
        "clap": _clap(),
        "tom_low": _tom(int(0.45 * RATE), 80.0, 70.0),
        "tom_mid": _tom(int(0.40 * RATE), 120.0, 90.0),
        "tom_high": _tom(int(0.35 * RATE), 180.0, 110.0),
        "rim": _rim(),
        "cowbell": _cowbell(),
    }


def files():
    """{'kick.wav': bytes, ...}: the kit as 16-bit mono WAV files."""
    return {name + ".wav": wav_bytes(to_pcm16(s, 0.92, 220)) for name, s in kit().items()}
