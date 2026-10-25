#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""update_goldens: rewrite sim/goldens.txt from the scenarios and the gallery, in parallel (OS-CIFAST2).

`cmake --build <dir> --target update_goldens` runs this. One `o3sim` process per scenario and one per gallery shard
(`--shard i/N`, the same split as ctest gallery_<i>of<N>), at most O3OS_GOLDEN_JOBS (default 4) at a time, each at a low
priority, each writing its own partial goldens file; the partial files are then merged into goldens.txt. The result is
the same bytes as the serial run (one process, all scenarios and the whole gallery in turn): keys sorted bytewise, the
header o3sim writes, a re-rendered scenario's old entries replaced, every old `gallery/` entry dropped.

  O3OS_GOLDEN_ONLY=<name>   refresh just one scenario (scenarios/<name>.txt or scenarios/<folder>/<name>.txt: its entries are
                            replaced) and/or the gallery screens whose registry name starts with <name> (`o3sim --gallery
                            --only <name>`: the entries of the rendered shots are replaced, the others stay). A name that is
                            neither is an error.
  O3OS_GOLDEN_JOBS=<n>      parallel processes (default 4)

PNGs go where the serial run put them: <sim-bin>/png/<scenario>/*.png and <sim-bin>/gallery/<screen>_<state>_<theme>.png.
The contact sheets of a sharded run are per shard (<sim-bin>/gallery/shard<i>of<N>/contact_sheet*.png: a sheet of the whole
gallery needs one process); the sheets of an earlier run in gallery/ itself are removed so that none is stale.

  update_goldens.py --o3sim <exe> --sim-src <os/sim> --sim-bin <build/sim> --goldens <goldens.txt> [--shards N] [--cards <build/sim/cards>]
  update_goldens.py --self-test
"""
import argparse
import concurrent.futures
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

GALLERY = "gallery/"


def parse_goldens(text):
    """-> (header lines, {key: hash}); the same reading as o3sim's read_goldens."""
    header, entries = [], {}
    for line in text.splitlines():
        if line.startswith("#"):
            header.append(line)
            continue
        words = line.split()
        if not words:
            continue
        if len(words) < 2 or len(words[1]) != 64:
            raise ValueError("malformed goldens line '%s'" % line)
        entries[words[0]] = words[1]
    return header, entries


def merge(old, partials, drop_prefixes, header):
    """The new goldens text. `old`: the existing {key: hash}; `partials`: the {key: hash} dicts of the jobs;
    `drop_prefixes`: old keys that start with one of these go (the re-rendered scenarios' "<stem>/" and, for the whole
    gallery, "gallery/"). Sorted bytewise, as o3sim's std::map writes it."""
    merged = {k: v for k, v in old.items() if not any(k.startswith(p) for p in drop_prefixes)}
    for part in partials:
        merged.update(part)
    return "\n".join(header + ["%s %s" % (k, merged[k]) for k in sorted(merged)]) + "\n"


def scenario_files(sim_src):
    """The scenarios by stem: scenarios/*.txt and every subfolder's (qa/, e2e/, manual/, ...). A stem is also the ctest name and the goldens prefix, so two files of one stem are an error."""
    root = Path(sim_src, "scenarios")
    out = {}
    for f in sorted(root.glob("*.txt")) + sorted(root.glob("*/*.txt")):
        if f.stem in out:
            raise SystemExit("update_goldens: two scenarios are called '%s': %s and %s (the file names carry their folder's prefix: qa_x.txt, e2e_x.txt, and manual/x_walk.txt)" % (f.stem, out[f.stem], f))
        out[f.stem] = f
    return out


def run_job(label, argv, nice):
    t0 = time.monotonic()
    proc = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                          preexec_fn=(lambda: os.nice(nice)) if nice else None)
    return label, proc.returncode, proc.stdout, time.monotonic() - t0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--o3sim")
    ap.add_argument("--sim-src")
    ap.add_argument("--sim-bin")
    ap.add_argument("--goldens")
    ap.add_argument("--shards", type=int, default=6)
    ap.add_argument("--cards", default="", help="the folder of the generated card fixtures (o3sim --cards); the journeys with a `card` need it")
    ap.add_argument("--jobs", type=int, default=int(os.environ.get("O3OS_GOLDEN_JOBS", "4")))
    ap.add_argument("--only", default=os.environ.get("O3OS_GOLDEN_ONLY", ""))
    ap.add_argument("--nice", type=int, default=10)
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if not (args.o3sim and args.sim_src and args.sim_bin and args.goldens):
        ap.error("--o3sim, --sim-src, --sim-bin and --goldens are required")
    jobs = max(1, min(args.jobs, 4)) if args.jobs > 0 else 4  # never more than 4: the PC is shared

    scenarios = scenario_files(args.sim_src)
    sim_bin = Path(args.sim_bin)
    parts = Path(tempfile.mkdtemp(prefix="golden_parts_", dir=sim_bin))
    gallery = sim_bin / "gallery"
    queue = []  # (label, argv), the long ones first
    gallery_only = ""
    run_gallery = True
    run_scenarios = dict(scenarios)
    if args.only:
        run_scenarios = {args.only: scenarios[args.only]} if args.only in scenarios else {}
        gallery_only = args.only
        # does the name start a screen? ask o3sim (--list draws nothing; no key listed: no such screen)
        probe = subprocess.run([args.o3sim, "--gallery", str(parts / "probe"), "--only", args.only, "--list"],
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        run_gallery = probe.returncode == 0 and bool(probe.stdout.strip())
        if not run_scenarios and not run_gallery:
            print("update_goldens: O3OS_GOLDEN_ONLY='%s' is neither a scenario (scenarios/, scenarios/<folder>/) nor the start "
                  "of a gallery screen name" % args.only, file=sys.stderr)
            shutil.rmtree(parts, ignore_errors=True)
            return 2

    gallery_dirs = []
    if run_gallery and gallery_only:
        gallery.mkdir(parents=True, exist_ok=True)
        queue.append(("gallery --only " + gallery_only,
                      [args.o3sim, "--gallery", str(gallery), "--goldens", str(parts / "gallery.txt"),
                       "--update-goldens", "--only", gallery_only]))
    elif run_gallery:
        n = args.shards
        for i in range(1, n + 1):
            d = gallery / ("shard%dof%d" % (i, n))
            shutil.rmtree(d, ignore_errors=True)
            d.mkdir(parents=True)
            gallery_dirs.append(d)
            queue.append(("gallery %d/%d" % (i, n),
                          [args.o3sim, "--gallery", str(d), "--goldens", str(parts / ("gallery%d.txt" % i)),
                           "--update-goldens", "--shard", "%d/%d" % (i, n)]))
    for stem, f in run_scenarios.items():
        out = sim_bin / "png" / stem
        queue.append(("scenario " + stem,
                      [args.o3sim, "--scenario", str(f), "--out", str(out), "--goldens", str(parts / (stem + ".txt")),
                       "--update-goldens"] + (["--cards", args.cards] if args.cards else [])))

    failed = []
    t0 = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = [pool.submit(run_job, label, argv, args.nice) for label, argv in queue]
        for fut in concurrent.futures.as_completed(futures):
            label, code, output, secs = fut.result()
            print("update_goldens: %-28s exit %d  %5.1f s" % (label, code, secs), flush=True)
            if code != 0:
                failed.append(label)
                sys.stdout.write(output)
    if failed:
        print("update_goldens: FAILED: %s; goldens.txt is unchanged (the partial files are in %s)" % (", ".join(failed), parts),
              file=sys.stderr)
        return 1

    header, partials = None, []
    for f in sorted(parts.glob("*.txt")):
        h, entries = parse_goldens(f.read_text())
        header = header if header is not None else h
        partials.append(entries)
    gpath = Path(args.goldens)
    old_header, old = parse_goldens(gpath.read_text()) if gpath.exists() else ([], {})
    drop = [stem + "/" for stem in run_scenarios] + ([GALLERY] if run_gallery and not gallery_only else [])
    gpath.write_text(merge(old, partials, drop, header if header else old_header))

    if run_gallery and not gallery_only:  # the shots of the shards into gallery/, as the serial run left them
        for stale in gallery.glob("contact_sheet*.png"):
            stale.unlink()
        for d in gallery_dirs:
            for png in d.glob("*.png"):
                if not png.name.startswith("contact_sheet"):
                    os.replace(png, gallery / png.name)
    shutil.rmtree(parts, ignore_errors=True)
    print("update_goldens: %d processes, %d at a time, %.1f s wall; %s written" % (len(queue), jobs, time.monotonic() - t0, gpath))
    return 0


def self_test():
    hexa = lambda c: c * 64
    hdr = ["# a", "# b"]
    old = {"a/x": hexa("1"), "a/y": hexa("2"), "b/x": hexa("3"), "gallery/s_a": hexa("4"), "gallery/old": hexa("5"), "z/gone": hexa("6")}
    # the whole run: scenario a re-rendered (a/y no longer exists), the whole gallery replaced, b and z kept (as the serial run does)
    text = merge(old, [{"a/x": hexa("7")}, {"gallery/s_a": hexa("8")}, {"gallery/s_b": hexa("9")}], ["a/", GALLERY], hdr)
    assert text == "# a\n# b\na/x %s\nb/x %s\ngallery/s_a %s\ngallery/s_b %s\nz/gone %s\n" % (hexa("7"), hexa("3"), hexa("8"), hexa("9"), hexa("6")), text
    # one screen: its entries replaced, the other gallery entries stay
    text = merge(old, [{"gallery/s_a": hexa("8")}], [], hdr)
    assert "gallery/old " + hexa("5") in text and "gallery/s_a " + hexa("8") in text and "a/y " + hexa("2") in text
    # parse round trip
    h, e = parse_goldens(text)
    assert h == hdr and merge(e, [], [], h) == text
    # bytewise order, not locale order
    assert merge({}, [{"B/x": hexa("1"), "a/x": hexa("2"), "_/x": hexa("3")}], [], []) == "B/x %s\n_/x %s\na/x %s\n" % (hexa("1"), hexa("3"), hexa("2"))
    try:
        parse_goldens("k tooshort\n")
        raise AssertionError("a malformed line must raise")
    except ValueError:
        pass
    print("update_goldens self-test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
