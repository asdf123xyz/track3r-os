# SPDX-License-Identifier: GPL-3.0-or-later
"""The two starter demo projects, written in the native format (docs/formats/o3-project.md) with tools/o3project.py (CC0 content).

  Starter-Samples   a drum groove on the generated kit and a wavetable pad; the WAV files sit in the project's samples/ folder
  Starter-Modules   a bass line, a lead and chords on the built-in instrument modules TB-303, chip and FM; no sample files

Both are plain projects: open them from the File screen. The files come from `files()` as {path under the card: bytes}.
"""
import json
import os
import struct
import sys

import drumkit
import wavetables

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, TOOLS)
import o3project  # noqa: E402

NOTE_OFF = 255
EMPTY = o3project.EMPTY_STEP
LENGTH = 32


def f32(x):
    return struct.unpack(">I", struct.pack(">f", x))[0]


def sampler(**over):
    """The sampler block of a fresh instrument (model::init_instrument), as the o3project head / env / lfo / slices lists."""
    head = [0] * 27
    head[6], head[7], head[11] = 50, 50, 16        # volume, panning, bit depth
    head[22] = head[23] = 65535                    # loop end, end
    head[25] = f32(1.0)                            # cutoff: the filter is off
    env = [[f32(1.0), 0, 3000, 0, 0, f32(1.0), 1000, 0, 0] for _ in range(6)]
    env[0][2], env[0][8] = 10, 1                   # the volume envelope: on, 10 ms
    lfo = [[2, 0, f32(0.5)] for _ in range(6)]
    for key, value in over.items():
        head[{"type": 0, "mode": 1, "volume": 6, "pan": 7, "delay": 8, "reverb": 9, "wt_size": 17}[key]] = value
    return head, env, lfo


def instrument(kind, name, head, env, lfo, path=b"", module=0xFF, blob=b"", rate=0):
    return {"kind": kind, "midi": 0, "module": module, "name": name, "path": path, "rate": rate, "engine": 0,
            "m13": list(o3project.INST13_DEFAULT), "head": head, "env": env, "lfo": lfo, "slices": [0] * 48, "blob": blob}


class Pattern:
    def __init__(self):
        self.steps = {}

    def put(self, track, step, note, instr, vol=255):
        self.steps[(track, step)] = bytes([note + 1, instr + 1, vol, 0, 0, 0, 0])      # note byte = MIDI note + 1; instrument byte = slot + 1
        return self

    def off(self, track, step):
        self.steps[(track, step)] = bytes([NOTE_OFF, 0, 255, 0, 0, 0, 0])
        return self

    def row(self, track):
        out = bytearray(EMPTY * o3project.L["max_pattern_length"])
        for (t, s), cell in self.steps.items():
            if t == track:
                out[s * 7:s * 7 + 7] = cell
        return bytes(out)

    def build(self, name):
        return {"lengths": [LENGTH] * o3project.L["tracks"], "name": name,
                "rows": [self.row(t) for t in range(o3project.L["tracks"])]}


def _project(name, tempo, track_names):
    p = o3project.new_project()
    p["name"] = name
    p["tempo"] = tempo
    for i, tn in enumerate(track_names):
        p["tracks"][i]["name"] = tn
    return p


# ---- Starter-Samples -----------------------------------------------------------------------------------------------------

SAMPLE_SLOTS = [  # (instrument name, file, volume, delay send, reverb send)
    (b"KICK", "kick.wav", 90, 0, 0),
    (b"SNARE", "snare.wav", 80, 0, 15),
    (b"HAT", "hat_closed.wav", 55, 0, 0),
    (b"OPEN HAT", "hat_open.wav", 50, 10, 0),
    (b"CLAP", "clap.wav", 70, 0, 25),
    (b"TOM", "tom_mid.wav", 75, 0, 10),
    (b"COWBELL", "cowbell.wav", 40, 20, 0),
]
WT_FILE = "wt_sine_to_saw.wav"       # a file of wavetables.files()


def _samples_patterns():
    a = Pattern()
    for s in range(0, 32, 4):
        a.put(0, s, 60, 0)                                  # kick on every beat
    a.put(0, 10, 60, 0, 80).put(0, 26, 60, 0, 80)           # ghost kicks
    for s in (4, 12, 20, 28):
        a.put(1, s, 60, 1)                                  # snare on 2 and 4
    for s in range(0, 32, 2):
        a.put(2, s, 60, 2, 100 if s & 2 else 60)            # hats, accents on the off-beats
    a.put(3, 14, 60, 3).put(3, 30, 60, 3)                   # open hat before each downbeat
    a.put(4, 12, 60, 4).put(4, 28, 60, 4)                   # clap with the snare
    a.put(6, 6, 64, 6, 60).put(6, 22, 60, 6, 60)            # cowbell
    for s, n in ((0, 48), (16, 51)):                        # the wavetable pad: one note a bar
        a.put(7, s, n, 7, 90).off(7, s + 14)
    b = Pattern()                                           # a variation with a tom fill and a higher pad
    for s in range(0, 32, 4):
        b.put(0, s, 60, 0)
    b.put(0, 11, 60, 0, 80)
    for s in (4, 12, 20):
        b.put(1, s, 60, 1)
    for s in (24, 26, 28, 30):
        b.put(1, s, 60, 1, 60 + (s - 24) * 8)               # a snare roll that gets louder
    for s in range(0, 24, 2):
        b.put(2, s, 60, 2, 100 if s & 2 else 60)
    b.put(3, 14, 60, 3)
    for s, n in ((20, 62), (24, 60), (26, 58), (28, 55)):
        b.put(5, s, n, 5, 100)                              # toms stepping down
    for s, n in ((0, 55), (16, 58)):
        b.put(7, s, n, 7, 90).off(7, s + 14)
    return a, b


def samples_project():
    p = _project(b"Starter Samples", 12800, [b"KICK", b"SNARE", b"HAT", b"OPEN HAT", b"CLAP", b"TOM", b"COWBELL", b"WT PAD"])
    for slot, (name, fname, vol, delay, reverb) in enumerate(SAMPLE_SLOTS):
        head, env, lfo = sampler(volume=vol, delay=delay, reverb=reverb)
        p["instruments"][slot] = instrument(1, name, head, env, lfo, b"samples/" + fname.encode(), rate=drumkit.RATE)
    head, env, lfo = sampler(type=1, mode=6, volume=45, wt_size=wavetables.WINDOW)    # sample type 1, play mode 6: a wavetable
    env[0][2], env[0][6] = 120, 600                          # a soft attack and a longer release for the pad
    p["instruments"][7] = instrument(1, b"WT SAW", head, env, lfo, b"samples/" + WT_FILE.encode(), rate=drumkit.RATE)
    a, b = _samples_patterns()
    p["patterns"] = {1: a.build(b"GROOVE"), 2: b.build(b"FILL")}
    p["song"] = [1, 1, 2, 1, 1, 2]
    return p


# ---- Starter-Modules -----------------------------------------------------------------------------------------------------

MODULES = [  # (stem, instrument name, {parameter name: value})
    ("tb303", b"ACID BASS", {"Waveform": 0, "Cutoff": 700, "Resonance": 70, "Env Mod": 60, "Decay": 400, "Accent": 70}),
    ("chip", b"CHIP LEAD", {"Wave": 0, "Duty": 1, "Arp 1": 4, "Arp 2": 7, "Arp Speed": 40, "Decay": 300}),
    ("fm", b"FM KEYS", {}),
]


def _manifest(stem):
    with open(os.path.join(TOOLS, "..", "modules", stem, "module.json"), "r", encoding="utf-8") as f:
        return json.load(f)


def _blob(manifest, values):
    """The parameter blob of a module instrument: one little-endian float per parameter, defaults except the given ones."""
    names = [q["name"] for q in manifest["params"]]
    for key in values:
        assert key in names, "unknown parameter " + key
    out = b""
    for q in manifest["params"]:
        v = float(values.get(q["name"], q["default"]))
        assert q["min"] <= v <= q["max"], q["name"]
        out += struct.pack("<f", v)
    return out


def _modules_pattern(second):
    a = Pattern()
    bass = [(0, 36), (3, 36), (6, 39), (8, 34), (11, 34), (14, 41)]
    if second:
        bass = [(0, 41), (3, 41), (6, 44), (8, 39), (11, 39), (14, 36)]
    for bar in (0, 16):
        for s, n in bass:
            a.put(0, bar + s, n, 0, 110 if s in (0, 8) else 80).off(0, bar + s + 2)
    lead = [(0, 72), (8, 75), (16, 79), (24, 77)] if not second else [(0, 74), (8, 77), (16, 81), (24, 79)]
    for s, n in lead:
        a.put(1, s, n, 1, 100).off(1, s + 6)
    chords = ([(0, (55, 58, 62)), (16, (53, 56, 60))] if not second else [(0, (58, 61, 65)), (16, (56, 60, 63))])
    for s, notes in chords:
        for k, n in enumerate(notes):
            a.put(2 + k, s, n, 2, 80).off(2 + k, s + 14)
    return a


def modules_project():
    p = _project(b"Starter Modules", 12400, [b"BASS", b"LEAD", b"KEYS 1", b"KEYS 2", b"KEYS 3"])
    for index, (stem, name, values) in enumerate(MODULES):
        m = _manifest(stem)
        major, minor = (int(x) for x in m["abi"].split("."))
        p["modules"].append((stem.encode(), major, minor))
        head, env, lfo = sampler()
        p["instruments"][index] = instrument(3, name, head, env, lfo, module=index, blob=_blob(m, values))
    p["patterns"] = {1: _modules_pattern(False).build(b"A"), 2: _modules_pattern(True).build(b"B")}
    p["song"] = [1, 1, 2, 1, 2, 2]
    return p


def files(kit_files=None, table_files=None):
    """{'Projects/<name>/...': bytes}: the project files and the samples next to the sampled one.

    `kit_files` and `table_files` are the outputs of drumkit.files() and wavetables.files() when the caller has them already."""
    kit = kit_files if kit_files is not None else drumkit.files()
    tables = table_files if table_files is not None else wavetables.files()
    out = {}
    for name, project in (("Starter-Samples", samples_project()), ("Starter-Modules", modules_project())):
        data = o3project.build(project)
        o3project.parse(data)                              # a project that does not read back is a bug in this file
        out["Projects/%s/project.o3pj" % name] = data
    for _, fname, _, _, _ in SAMPLE_SLOTS:
        out["Projects/Starter-Samples/samples/" + fname] = kit[fname]
    out["Projects/Starter-Samples/samples/" + WT_FILE] = tables[WT_FILE]
    return out
