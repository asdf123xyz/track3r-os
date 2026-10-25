#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regenerate the manual's screenshots from the host simulator (OS-MANUALTOOLS).

Reads the manifest docs/manual/images.txt, one image per line:

    <name>.png <scenario> <snap>            a `snap <snap>` line of sim/scenarios/qa/<scenario>.txt (or sim/scenarios/<scenario>.txt)
    <name>.png <folder>/<scenario> <snap>   a `snap <snap>` line of sim/scenarios/<folder>/<scenario>.txt: the walk-throughs of the chapters
                                            (OS-MANUAL2 M2-4) are manual/<screen>_walk, their pictures <screen>_walk_<step>.png
                                            (every scenario is a file under sim/scenarios/: an absolute word, a `..` and a link that leads out are errors)
    <name>.png gallery <screen>_<state>     the Amiga-theme picture <screen>_<state>_amiga.png of `o3sim --gallery`

`#` starts a comment. A line that ends in `# MISSING` names a source that does not exist (yet): it is skipped with a
note and is not an error; any other source that cannot be found is an error (exit 1).

The pictures are written to docs/manual/img/ (default) and ARE committed: GitHub shows the chapters with them (maintainer decision
2026-10-17, a narrow exception to "no binaries in git"). They stay generated outputs: never edit one by hand; regenerate with this
script and commit the PNGs together with the change that moved them. The renders are byte-deterministic (the simulator rasteriser is
integer only and the PNG writer is stb_image_write, not zlib), so ctest `o3sim_manual_images` (--check --strict) fails when a
committed PNG differs from a fresh render. The script never builds: it expects build-os-host/sim/o3sim
(cmake --build build-os-host --target o3sim).

  tools/manual_images.py             regenerate every image of the manifest
  tools/manual_images.py --check     regenerate into a temporary directory and report the images that changed
                                     against docs/manual/img/ (a changed screenshot means the chapter text needs a re-read)
                                     or that are missing there, and the PNGs there that the manifest does not list;
                                     with --strict the exit code is 1 for any of them (the ctest)
  tools/manual_images.py --list      print the manifest as it is understood, with a flag for the sources found by a text scan
                                     (scenario snaps only; gallery names need a run)

One o3sim process at a time, at nice 10.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def parse_manifest(path):
    """[(name, source, arg, missing, lineno)]; raises ValueError on a malformed line."""
    out = []
    with open(path, encoding="utf-8") as f:
        for no, raw in enumerate(f, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            missing = False
            if "#" in line:
                line, comment = line.split("#", 1)
                missing = comment.strip().startswith("MISSING")
                line = line.strip()
            f3 = line.split()
            if len(f3) != 3 or not f3[0].endswith(".png"):
                raise ValueError("%s:%d: expected `<name>.png <scenario> <snap>` or `<name>.png gallery <screen>_<state>`" % (path, no))
            out.append((f3[0], f3[1], f3[2], missing, no))
    return out


def scenario_file(root, scenario):
    """The scenario file of a manifest word, always a file under sim/scenarios/: `<folder>/<name>` is sim/scenarios/<folder>/<name>.txt (one level), a bare `<name>` is qa/
    and then sim/scenarios/, and a word that ends in .txt is a path from the root. An absolute word, a `..` or `.` or empty part, a path with more than one folder, and a file
    whose real path (a symbolic link followed) is outside sim/scenarios/ give None."""
    parts = scenario.split("/")
    if os.path.isabs(scenario) or any(p in ("", ".", "..") for p in parts):
        return None
    if scenario.endswith(".txt"):
        cands = [os.path.join(root, scenario)]
    elif len(parts) == 2:
        cands = [os.path.join(root, "sim", "scenarios", parts[0], parts[1] + ".txt")]
    elif len(parts) == 1:
        cands = [os.path.join(root, "sim", "scenarios", d, scenario + ".txt") for d in ("qa", "")]
    else:
        return None
    inside = os.path.realpath(os.path.join(root, "sim", "scenarios")) + os.sep
    for c in cands:
        if os.path.isfile(c) and os.path.realpath(c).startswith(inside):
            return c
    return None


def snap_in_scenario(path, snap):
    with open(path, encoding="utf-8") as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 2 and parts[0] == "snap" and parts[1] == snap:
                return True
    return False


def run_sim(cmd):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout[-2000:])
        raise RuntimeError("%s exited %d" % (" ".join(cmd[:3]), r.returncode))


def render(root, sim, entries, work):
    """Render every needed scenario and (once) the gallery; returns {name: path of the png} and [errors]."""
    found, errors = {}, []
    scen_done = {}
    gallery_dir = None
    for name, src, arg, missing, no in entries:
        if missing:
            continue
        if src == "gallery":
            if gallery_dir is None:
                gallery_dir = os.path.join(work, "gallery")
                os.makedirs(gallery_dir)
                run_sim([sim, "--gallery", gallery_dir, "--theme", "amiga"])
            p = os.path.join(gallery_dir, arg + "_amiga.png")
        else:
            sf = scenario_file(root, src)
            if sf is None:
                errors.append("%s: no scenario %s" % (name, src))
                continue
            if sf not in scen_done:
                d = os.path.join(work, "scn_%d" % len(scen_done))
                os.makedirs(d)
                cmd = [sim, "--scenario", sf, "--out", d]
                cards = os.path.join(os.path.dirname(sim), "cards")  # the card fixtures a `card` line names (built with the simulator: sim/CMakeLists.txt)
                if os.path.isdir(cards):
                    cmd += ["--cards", cards]
                run_sim(cmd)
                scen_done[sf] = d
            p = os.path.join(scen_done[sf], arg + ".png")
        if os.path.isfile(p):
            found[name] = p
        else:
            errors.append("%s: no snap or gallery picture %s %s (mark the line `# MISSING` if it is not there yet)" % (name, src, arg))
    return found, errors


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", default=ROOT)
    ap.add_argument("--manifest", help="default: docs/manual/images.txt")
    ap.add_argument("--sim", help="default: build-os-host/sim/o3sim")
    ap.add_argument("--out", help="default: docs/manual/img")
    ap.add_argument("--check", action="store_true", help="report images whose bytes changed; write nothing")
    ap.add_argument("--strict", action="store_true", help="with --check: exit 1 when an image changed, is missing, or is not in the manifest")
    ap.add_argument("--list", action="store_true", help="print the manifest")
    args = ap.parse_args(argv)
    root = args.root
    manifest = args.manifest or os.path.join(root, "docs", "manual", "images.txt")
    sim = args.sim or os.path.join(root, "build-os-host", "sim", "o3sim")
    out = args.out or os.path.join(root, "docs", "manual", "img")
    try:
        entries = parse_manifest(manifest)
    except (OSError, ValueError) as e:
        print("manual_images: %s" % e, file=sys.stderr)
        return 2

    if args.list:
        n_missing = 0
        for name, src, arg, missing, _ in entries:
            if missing:
                n_missing += 1
            note = "MISSING" if missing else ""
            if src != "gallery":
                sf = scenario_file(root, src)
                if sf is None or not snap_in_scenario(sf, arg):
                    note = note or "NOT FOUND"
            print("%-36s %-18s %-24s %s" % (name, src, arg, note))
        print("%d images, %d marked MISSING" % (len(entries), n_missing))
        return 0

    if not os.path.exists(sim):
        print("manual_images: %s not found (cmake --build build-os-host --target o3sim)" % sim, file=sys.stderr)
        return 2
    try:
        os.nice(10)
    except (AttributeError, OSError):
        pass

    strict_fail = False
    with tempfile.TemporaryDirectory(prefix="manual_images_") as work:
        try:
            found, errors = render(root, sim, entries, work)
        except RuntimeError as e:
            print("manual_images: %s" % e, file=sys.stderr)
            return 1
        skipped = [e[0] for e in entries if e[3]]
        if args.check:
            changed, same, absent, orphan = [], 0, [], []
            for name, p in sorted(found.items()):
                dest = os.path.join(out, name)
                if not os.path.exists(dest):
                    absent.append(name)
                else:
                    with open(p, "rb") as a, open(dest, "rb") as b:
                        if a.read() == b.read():
                            same += 1
                        else:
                            changed.append(name)
            if os.path.isdir(out):
                listed = {e[0] for e in entries if not e[3]}  # a `# MISSING` line has no picture yet: a PNG committed for it is not listed
                orphan = sorted(n for n in os.listdir(out) if n.endswith(".png") and n not in listed)
            print("manual_images --check: %d unchanged, %d changed, %d not generated yet, %d not in the manifest, %d marked MISSING, %d errors"
                  % (same, len(changed), len(absent), len(orphan), len(skipped), len(errors)))
            for n in changed:
                print("  changed: " + n)
            for n in absent:
                print("  not generated yet: " + n)
            for n in orphan:
                print("  not in the manifest: " + n)
            if args.strict and (changed or absent or orphan):
                print("manual_images: run tools/manual_images.py, commit docs/manual/img/ and re-read the chapters that show a changed picture",
                      file=sys.stderr)
                strict_fail = True
        else:
            os.makedirs(out, exist_ok=True)
            for name, p in sorted(found.items()):
                shutil.copyfile(p, os.path.join(out, name))
            print("manual_images: wrote %d images to %s, %d marked MISSING (skipped), %d errors"
                  % (len(found), os.path.relpath(out, root), len(skipped), len(errors)))
        for e in errors:
            print("  error: " + e, file=sys.stderr)
        for n in skipped:
            print("  missing source (skipped): " + n)
    return 1 if errors or strict_fail else 0


if __name__ == "__main__":
    sys.exit(main())
