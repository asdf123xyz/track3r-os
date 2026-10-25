#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""usb_return_test: the Tracker's USB AUDIO return path (PC -> unit) from the PC side (stdlib + ctypes on libasound; Linux). Helper of MANUAL.md, not a scenario.

    usb_return_test.py gen DIR                         DIR/left_1k.wav, right_1k.wav (3 s, 1 kHz, -6 dBFS on one channel only), click.wav (stereo, one click at 0.5 s)
    usb_return_test.py latency --card hw:2,0 [--trials 5] [--channel 9] [--channels 10] [--rate 44100]
                                                       play a click to the unit and record its 10-channel capture at the same time, with both PCMs LINKED (snd_pcm_link: one
                                                       start trigger), and say how many ms later the click shows in the master channel (--channel 9 = master L of the 10 channel
                                                       profile). Needs `mon 70` on the unit first: the USB return reaches MAIN only through the input strip's level (OS-006e).
    usb_return_test.py --self-test                     the click finder on synthetic data, and the ALSA calls against the `null` PCM

The measured delay is what a PC-side program sees from the sample it writes to the sample it reads back: host buffering that does not move the click (the ring in front of the
start) is NOT in it, the USB packets, the unit's return FIFO, the engine block (64 frames) and the capture path are. Linked streams start within a USB frame of each other
(+-1 ms); repeat and take the median.
"""
import argparse
import array
import ctypes
import ctypes.util
import math
import os
import statistics
import sys
import tempfile
import wave

PLAYBACK, CAPTURE = 0, 1
FORMAT_S16_LE = 2
ACCESS_RW_INTERLEAVED = 3


def find_click(samples, rate, threshold=0.25):
    """Index of the first sample whose magnitude is above `threshold` of the largest one, or None for silence."""
    peak = max((abs(s) for s in samples), default=0)
    if peak < 200:
        return None
    limit = peak * threshold
    for i, s in enumerate(samples):
        if abs(s) >= limit:
            return i
    return None


def write_wav(path, channels, rate, data):
    with wave.open(path, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(data.tobytes())


def gen(directory, rate=44100):
    os.makedirs(directory, exist_ok=True)
    n = 3 * rate
    tone = [int(0.5 * 32767 * math.sin(2 * math.pi * 1000 * i / rate)) for i in range(n)]
    for name, left in (("left_1k.wav", True), ("right_1k.wav", False)):
        d = array.array("h", [0]) * (2 * n)
        d[(0 if left else 1)::2] = array.array("h", tone)
        write_wav(os.path.join(directory, name), 2, rate, d)
    d = array.array("h", [0]) * (2 * rate)
    for i in range(rate // 2, rate // 2 + 8):  # a click: eight samples at 0.8 full scale
        d[2 * i] = d[2 * i + 1] = 26000
    write_wav(os.path.join(directory, "click.wav"), 2, rate, d)
    for name in ("left_1k.wav", "right_1k.wav", "click.wav"):
        print(os.path.join(directory, name))


class Alsa:
    def __init__(self):
        name = ctypes.util.find_library("asound") or "libasound.so.2"
        self.lib = ctypes.CDLL(name)
        self.lib.snd_strerror.restype = ctypes.c_char_p
        self.lib.snd_pcm_writei.restype = ctypes.c_long
        self.lib.snd_pcm_readi.restype = ctypes.c_long
        self.lib.snd_pcm_writei.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong]
        self.lib.snd_pcm_readi.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong]

    def check(self, code, what):
        if code < 0:
            raise RuntimeError("%s: %s (%d)" % (what, self.lib.snd_strerror(int(code)).decode(), code))
        return code

    def open(self, name, stream, channels, rate, latency_us):
        pcm = ctypes.c_void_p()
        self.check(self.lib.snd_pcm_open(ctypes.byref(pcm), name.encode(), stream, 0), "open %s" % name)
        self.check(self.lib.snd_pcm_set_params(pcm, FORMAT_S16_LE, ACCESS_RW_INTERLEAVED, channels, rate, 0, latency_us), "set_params %s" % name)
        return pcm

    def sizes(self, pcm):
        buffer_size = ctypes.c_ulong()
        period_size = ctypes.c_ulong()
        self.check(self.lib.snd_pcm_get_params(pcm, ctypes.byref(buffer_size), ctypes.byref(period_size)), "get_params")
        return buffer_size.value, period_size.value


def one_trial(alsa, card, play_card, channels, rate, channel, seconds=2.5):
    pb = alsa.open(play_card, PLAYBACK, 2, rate, 60000)
    cp = alsa.open(card, CAPTURE, channels, rate, 60000)
    try:
        buf_p, per_p = alsa.sizes(pb)
        _, per_c = alsa.sizes(cp)
        step = per_p
        total = int(seconds * rate) // step * step
        click_at = rate  # frame 44100 of the playback stream
        stream = array.array("h", [0]) * (2 * total)
        for i in range(click_at, click_at + 8):
            stream[2 * i] = stream[2 * i + 1] = 26000
        alsa.check(alsa.lib.snd_pcm_link(cp, pb), "link capture and playback")
        alsa.check(alsa.lib.snd_pcm_prepare(pb), "prepare")
        pre = max(step, (buf_p // step - 1) * step)  # a prefill below the start threshold: nothing runs until snd_pcm_start
        written = 0

        def put(frames):
            nonlocal written
            chunk = (ctypes.c_int16 * (2 * frames)).from_buffer(stream, 2 * 2 * written)
            n = alsa.lib.snd_pcm_writei(pb, chunk, frames)
            if n < 0:
                alsa.check(alsa.lib.snd_pcm_recover(pb, int(n), 1), "playback recover")
                return
            written += n

        while written < pre and written < total:
            put(min(step, pre - written))
        alsa.check(alsa.lib.snd_pcm_start(pb), "start (both: they are linked)")
        got = array.array("h")
        want = total
        chunk_frames = per_c
        capture_buf = (ctypes.c_int16 * (chunk_frames * channels))()
        while len(got) < want * channels:
            if written < total:
                put(min(step, total - written))
            n = alsa.lib.snd_pcm_readi(cp, capture_buf, chunk_frames)
            if n < 0:
                alsa.check(alsa.lib.snd_pcm_recover(cp, int(n), 1), "capture recover")
                continue
            got.extend(capture_buf[: n * channels])
        mono = got[(channel - 1)::channels]
        at = find_click(mono, rate)
        return None if at is None else (at - click_at) * 1000.0 / rate
    finally:
        alsa.lib.snd_pcm_close(pb)
        alsa.lib.snd_pcm_close(cp)


def latency(card, play_card, trials, channels, rate, channel):
    alsa = Alsa()
    alsa.lib.snd_pcm_get_params.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong)]
    results = []
    for t in range(trials):
        ms = one_trial(alsa, card, play_card, channels, rate, channel)
        print("trial %d: %s" % (t + 1, "no click in channel %d (is `mon` above 0? is the profile %d channels?)" % (channel, channels) if ms is None else "%.2f ms" % ms))
        if ms is not None:
            results.append(ms)
    if not results:
        return 1
    print("click delay: min %.2f  median %.2f  max %.2f ms over %d trials" % (min(results), statistics.median(results), max(results), len(results)))
    return 0


def self_test():
    rate = 44100
    quiet = [0] * 1000 + [20000] + [0] * 100
    if find_click(quiet, rate) != 1000 or find_click([0] * 100, rate) is not None or find_click([10, -12, 5], rate) is not None:
        print("usb_return_test self-test FAILED: find_click")
        return 1
    with tempfile.TemporaryDirectory() as tmp:
        gen(tmp)
        with wave.open(os.path.join(tmp, "click.wav")) as w:
            if w.getnchannels() != 2 or w.getnframes() != rate:
                print("usb_return_test self-test FAILED: click.wav")
                return 1
    alsa = Alsa()
    alsa.lib.snd_pcm_get_params.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong)]
    try:
        pb = alsa.open("null", PLAYBACK, 2, rate, 60000)
        cp = alsa.open("null", CAPTURE, 10, rate, 60000)
        b, p = alsa.sizes(pb)
        alsa.lib.snd_pcm_close(pb)
        alsa.lib.snd_pcm_close(cp)
        if b == 0 or p == 0:
            print("usb_return_test self-test FAILED: null PCM sizes")
            return 1
    except RuntimeError as error:
        print("usb_return_test self-test FAILED: %s" % error)
        return 1
    print("usb_return_test self-test passed (click finder, WAV generator, ALSA open/set_params/get_params on the null PCM; the linked run needs the unit)")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true")
    sub = parser.add_subparsers(dest="command")
    g = sub.add_parser("gen")
    g.add_argument("dir")
    lt = sub.add_parser("latency")
    lt.add_argument("--card", required=True, help="the unit's ALSA device for capture, like hw:2,0")
    lt.add_argument("--play-card", help="its playback device (default: the same)")
    lt.add_argument("--trials", type=int, default=5)
    lt.add_argument("--channels", type=int, default=10)
    lt.add_argument("--channel", type=int, default=9, help="1-based channel of the capture that carries the master L (9 in the 10 channel profile)")
    lt.add_argument("--rate", type=int, default=44100)
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.command == "gen":
        gen(args.dir)
        return 0
    if args.command == "latency":
        return latency(args.card, args.play_card or args.card, args.trials, args.channels, args.rate, args.channel)
    parser.print_usage(sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
