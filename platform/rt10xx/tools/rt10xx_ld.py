#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates and checks the rt10xx linker script from mem_map.h (OS-PORT-RT10XX; stdlib only).

    rt10xx_ld.py generate --header mem_map.h --template rt10xx.ld.in --out rt10xx.ld [--gen-ld <path to gen_ld.py>]
    rt10xx_ld.py check    --header mem_map.h --template rt10xx.ld.in [--ld rt10xx.ld] [--gen-ld ...]
    rt10xx_ld.py --self-test

The header parser and the placeholder renderer are platform/mk66/tools/gen_ld.py's (imported: the same whitelist arithmetic, the
same refusal of a leftover placeholder); the verification is this chip's: the MEMORY regions against the header, the regions inside
the RAM that exists and the image region of the flash, the SDRAM table's sum, no overlaps. `generate` refuses to write a script that
fails the checks. ctest runs `check` as `rt10xx_ld` (template and header) and `rt10xx_ld_file` (the generated file).
"""
import argparse
import importlib.util
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_GEN_LD = HERE.parent.parent / "mk66" / "tools" / "gen_ld.py"

# The SDRAM map (mem_map.h), region constants in order: base name, size name.
SDRAM_REGION_NAMES = [
    ("kSampleArenaBase", "kSampleArenaSize"), ("kDelayBase", "kDelaySize"), ("kReverbBase", "kReverbSize"),
    ("kModuleRegionBase", "kModuleRegionSize"), ("kProjectBase", "kProjectSize"), ("kUndoBase", "kUndoSize"),
    ("kFileJobBase", "kFileJobSize"), ("kLutBase", "kLutSize"), ("kUiCacheBase", "kUiCacheSize"),
    ("kDiagLogBase", "kDiagLogSize"), ("kSdramSlackBase", "kSdramSlackSize"),
]


class LdError(Exception):
    pass


def load_gen_ld(path):
    path = Path(path)
    if not path.is_file():
        raise LdError("gen_ld.py not found at %s" % path)
    spec = importlib.util.spec_from_file_location("gen_ld", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def verify(constants, script, memory_regions):
    """Independent checks of a generated script against the header. Returns a list of problems."""
    problems = []
    regions = memory_regions(script)
    expect = {
        "FLASH": (constants["kImageBase"], constants["kImageMaxSize"]),
        "ITCM": (constants["kItcmBase"], constants["kRamfuncSize"]),
        "DTCM": (constants["kDtcmBase"], constants["kDtcmSize"]),
        "OCRAM": (constants["kOcramBase"], constants["kOcramSize"]),
        "SDRAM_STATIC": (constants["kSdramSlackBase"], constants["kSdramSlackSize"]),
    }
    for name, want in expect.items():
        got = regions.get(name)
        if got is None:
            problems.append("MEMORY has no region %s" % name)
        elif got != want:
            problems.append("MEMORY %s is origin 0x%08X length 0x%X, mem_map.h says origin 0x%08X length 0x%X" % ((name,) + got + want))
    extra = sorted(set(regions) - set(expect))
    if extra:
        problems.append("MEMORY has regions the header does not describe: %s" % ", ".join(extra))
    spans = sorted((o, o + l, n) for n, (o, l) in regions.items())
    for (a0, a1, an), (b0, b1, bn) in zip(spans, spans[1:]):
        if b0 < a1:
            problems.append("regions %s and %s overlap" % (an, bn))
    # Each region inside the memory it belongs to.
    if not (constants["kFlashBase"] <= constants["kImageBase"] and constants["kImageBase"] + constants["kImageMaxSize"] <= constants["kFlashBase"] + constants["kFlashSize"]):
        problems.append("the image region is outside the flash")
    if constants["kImageBase"] != 0x60030000:
        problems.append("the image base is not 0x60030000 (the bootloader's hand-over address)")
    if constants["kItcmBase"] + constants["kRamfuncSize"] + constants["kModuleSlabSize"] != constants["kItcmBase"] + constants["kItcmSize"]:
        problems.append("ITCM: hot code plus the module slab must be the whole ITCM")
    if constants["kItcmHotBudget"] + constants["kItcmDataBudget"] > constants["kRamfuncSize"]:
        problems.append("ITCM: the hot-code budget plus the tap-data budget exceed the region below the module slab")
    if constants["kDtcmBase"] + constants["kDtcmSize"] != constants["kStackTop"] or constants["kStackTop"] != 0x20058000:
        problems.append("DTCM: the stack top must be the end of the DTCM, 0x20058000")
    if constants["kOcramBase"] + constants["kOcramSize"] > constants["kOcramWindowEnd"]:
        problems.append("OCRAM: the region exceeds the RAM that exists")
    if constants["kItcmSize"] + constants["kDtcmSize"] + constants["kOcramSize"] != 512 << 10:
        problems.append("FlexRAM: the three TCM sizes must sum to 512 KiB")
    cursor = constants["kSdramBase"]
    for base_name, size_name in SDRAM_REGION_NAMES:
        if constants[base_name] != cursor:
            problems.append("%s = 0x%08X, expected 0x%08X (regions must follow one another)" % (base_name, constants[base_name], cursor))
        cursor += constants[size_name]
    if cursor != constants["kSdramBase"] + constants["kSdramSize"]:
        problems.append("the SDRAM regions end at 0x%08X, not at the end of SDRAM 0x%08X" % (cursor, constants["kSdramBase"] + constants["kSdramSize"]))
    if constants["kSdramSize"] != 64 << 20:
        problems.append("the SDRAM is 64 MiB on this unit")
    return problems


def build(args):
    gen_ld = load_gen_ld(args.gen_ld)
    try:
        constants = gen_ld.parse_header(Path(args.header).read_text(encoding="utf-8"))
        script = gen_ld.render(Path(args.template).read_text(encoding="utf-8"), constants)
    except gen_ld.GenError as exc:
        raise LdError(str(exc))
    return gen_ld, constants, script


def cmd_generate(args):
    gen_ld, constants, script = build(args)
    problems = verify(constants, script, gen_ld.memory_regions)
    if problems:
        raise LdError("generated script fails its checks:\n  " + "\n  ".join(problems))
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(script, encoding="utf-8")
    print("rt10xx_ld: wrote %s (%d constants from %s)" % (out, len(constants), args.header))
    return 0


def cmd_check(args):
    gen_ld, constants, script = build(args)
    if args.ld:
        on_disk = Path(args.ld).read_text(encoding="utf-8")
        if on_disk != script:
            raise LdError("%s is not what the header and template generate: rebuild it" % args.ld)
    problems = verify(constants, script, gen_ld.memory_regions)
    if problems:
        raise LdError("\n  ".join(["rt10xx linker script check failed:"] + problems))
    print("rt10xx_ld: check ok (%d constants, %d regions%s)" % (len(constants), len(gen_ld.memory_regions(script)), ", file matches" if args.ld else ""))
    return 0


def self_test():
    failures = []

    def expect(label, ok):
        if not ok:
            failures.append(label)

    gen_ld = load_gen_ld(DEFAULT_GEN_LD)
    header = HERE.parent / "include" / "o3os" / "platform" / "rt10xx" / "mem_map.h"
    template = HERE.parent / "rt10xx.ld.in"
    constants = gen_ld.parse_header(header.read_text(encoding="utf-8"))
    script = gen_ld.render(template.read_text(encoding="utf-8"), constants)
    expect("real header and template pass", verify(constants, script, gen_ld.memory_regions) == [])
    pins = {  # independent copies of the facts (docs/specs/OS-PORT-RT10XX.md section 2)
        "kImageBase": 0x60030000, "kImageMaxEnd": 0x60800000, "kItcmSize": 128 << 10, "kDtcmSize": 352 << 10, "kOcramSize": 32 << 10,
        "kStackTop": 0x20058000, "kSdramBase": 0x80000000, "kSdramSize": 64 << 20, "kVectorTableBytes": 704, "kFlashSize": 16 << 20,
        "kFlashSectorSize": 4096, "kFlashPageSize": 256, "kPagePadBytes": 16, "kDmaRingAlign": 1024,
    }
    for name, value in pins.items():
        expect("%s pinned" % name, constants.get(name) == value)
    regions = gen_ld.memory_regions(script)
    expect("FLASH region", regions.get("FLASH") == (0x60030000, 0x60800000 - 0x60030000))
    expect("ITCM region", regions.get("ITCM") == (0, (128 - 32) << 10))
    expect("DTCM region", regions.get("DTCM") == (0x20000000, 352 << 10))
    expect("no leftover placeholder", "@k" not in script)
    broken = dict(constants)
    broken["kDelayBase"] += 32
    expect("moved SDRAM region is caught", any("kDelayBase" in p for p in verify(broken, script, gen_ld.memory_regions)))
    expect("extra region is caught", any("describe" in p for p in verify(constants, script.replace("SDRAM_STATIC (rw)", "OTHER (rw) : ORIGIN = 0x80000000, LENGTH = 0x10\n  SDRAM_STATIC (rw)"), gen_ld.memory_regions)))
    expect("wrong region is caught", any("MEMORY DTCM" in p for p in verify(constants, script.replace("0x20000000", "0x20000020", 1), gen_ld.memory_regions)))
    broken = dict(constants)
    broken["kStackTop"] = 0x20030000
    expect("wrong stack top is caught", any("0x20058000" in p for p in verify(broken, script, gen_ld.memory_regions)))
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "rt10xx.ld"
        ns = argparse.Namespace(header=str(header), template=str(template), gen_ld=str(DEFAULT_GEN_LD), out=str(out), ld=None)
        expect("generate writes", cmd_generate(ns) == 0 and out.read_text(encoding="utf-8") == script)
        ns.ld = str(out)
        expect("check accepts the file", cmd_check(ns) == 0)
        out.write_text(script + "\n/* edited */\n", encoding="utf-8")
        try:
            cmd_check(ns)
            expect("check refuses an edited file", False)
        except LdError:
            pass
    if failures:
        print("rt10xx_ld self-test FAILED:\n  " + "\n  ".join(failures))
        return 1
    print("rt10xx_ld self-test: all checks passed")
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true")
    sub = parser.add_subparsers(dest="command")
    for name in ("generate", "check"):
        p = sub.add_parser(name)
        p.add_argument("--header", required=True)
        p.add_argument("--template", required=True)
        p.add_argument("--gen-ld", default=str(DEFAULT_GEN_LD), help="platform/mk66/tools/gen_ld.py (its parser and renderer are reused)")
        if name == "generate":
            p.add_argument("--out", required=True)
        else:
            p.add_argument("--ld", help="a generated script to compare with what the header and template generate")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.command:
        parser.print_help()
        return 2
    try:
        return {"generate": cmd_generate, "check": cmd_check}[args.command](args)
    except LdError as exc:
        print("rt10xx_ld: %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
