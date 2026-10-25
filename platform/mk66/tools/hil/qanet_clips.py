#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The four clips of qanet_latency.json (OS-QANET D4), the same as tests/qanet/budgets.cpp's preview card: python3 -I qanet_clips.py <folder>.

  qanet_1_short.wav   1 s mono, 441 Hz        qanet_3_tone_a.wav   6 s mono, 210 Hz (about)
  qanet_2_long.wav    6 s stereo, 441 Hz      qanet_4_tone_b.wav   6 s mono, 882 Hz

Copy the four files to the ROOT of the card in the unit (they sort one after the other, which the scenario relies on). 16-bit PCM, 44.1 kHz. Writes only into <folder>. Stdlib only."""
import math
import os
import struct
import sys

RATE = 44100


def wav(seconds, period_frames, channels):
    frames = RATE * seconds
    data = bytearray()
    for i in range(frames):
        v = int(round(0.5 * 32767.0 * math.sin(2.0 * math.pi * (i % period_frames) / period_frames)))
        data += struct.pack("<h", v) * channels
    head = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, channels, RATE, RATE * channels * 2, channels * 2, 16) + b"data" + struct.pack("<I", len(data))
    return head + bytes(data)


def main(argv):
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[0], file=sys.stderr)
        return 2
    os.makedirs(argv[1], exist_ok=True)
    for name, args in (("qanet_1_short.wav", (1, 100, 1)), ("qanet_2_long.wav", (6, 100, 2)), ("qanet_3_tone_a.wav", (6, 210, 1)), ("qanet_4_tone_b.wav", (6, 50, 1))):
        with open(os.path.join(argv[1], name), "wb") as f:
            f.write(wav(*args))
        print("wrote", os.path.join(argv[1], name))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
