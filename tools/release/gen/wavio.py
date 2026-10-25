# SPDX-License-Identifier: GPL-3.0-or-later
"""Shared helpers of the starter-content generators (stdlib only; run through `python3 -I`).

Everything here is deterministic on any machine: no libm call (sin is a polynomial of + - * only, which IEEE 754 rounds the same
everywhere), no random module (a fixed integer generator), no clock. The code is GPL-3.0-or-later; the audio and the projects it generates are CC0 (own synthetic data).
"""
import math
import struct

RATE = 44100
TWO_PI = 6.283185307179586


def sin_turns(x):
    """sin(2 pi x) for x in turns, from + - * only (error below 1e-9)."""
    x = x - math.floor(x)          # 0 <= x < 1
    if x >= 0.5:
        x -= 1.0                   # -0.5 <= x < 0.5
    if x > 0.25:
        x = 0.5 - x
    elif x < -0.25:
        x = -0.5 - x
    t = x * TWO_PI
    t2 = t * t
    # t - t^3/3! + t^5/5! - ... - t^13/13!, Horner in t^2
    return t * (1.0 - t2 / 6.0 * (1.0 - t2 / 20.0 * (1.0 - t2 / 42.0 * (1.0 - t2 / 72.0 * (1.0 - t2 / 110.0 * (1.0 - t2 / 156.0))))))


class Noise:
    """White noise in [-1, 1) from a 32-bit LCG (the numerical-recipes constants); the seed makes each drum its own."""

    def __init__(self, seed):
        self.x = seed & 0xFFFFFFFF

    def next(self):
        self.x = (self.x * 1664525 + 1013904223) & 0xFFFFFFFF
        return (self.x >> 8) / 8388608.0 - 1.0


def to_pcm16(samples, peak=0.92, fade=0):
    """Scales the floats so that the largest magnitude is `peak`, fades the last `fade` samples to zero, returns int16 values."""
    top = max((abs(v) for v in samples), default=0.0)
    gain = peak / top if top > 0.0 else 0.0
    out = []
    n = len(samples)
    for i, v in enumerate(samples):
        v *= gain
        if fade and i >= n - fade:
            v *= (n - 1 - i) / fade
        q = int(math.floor(v * 32767.0 + 0.5))
        out.append(max(-32768, min(32767, q)))
    return out


def wav_bytes(pcm, rate=RATE, extra_chunks=()):
    """A canonical 16-bit mono PCM WAV. `extra_chunks` is a list of (4-byte id, payload) placed before the data chunk."""
    data = struct.pack("<%dh" % len(pcm), *pcm)
    body = b"WAVE" + b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
    for cid, payload in extra_chunks:
        body += cid + struct.pack("<I", len(payload)) + payload + (b"\0" if len(payload) & 1 else b"")
    body += b"data" + struct.pack("<I", len(data)) + data
    return b"RIFF" + struct.pack("<I", len(body)) + body
