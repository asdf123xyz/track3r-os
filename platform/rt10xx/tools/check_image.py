#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Static checks of an rt10xx firmware image in Intel HEX form, a `.hex` or a `.ptf` (OS-PORT-RT10XX; stdlib only): the static part of
the image gate for the Tracker+ (the fw-gate-check skill).

    check_image.py check IMAGE [--ramfunc BASE:SIZE] [--expect-ptf] [--delete-on-fail] [--json OUT]
    check_image.py --self-test

`check` runs every check below and exits 1 if any fails (nothing is flashed, copied or handed over by this tool):

  parse          every record is well formed (length, checksum); types 00, 01, 04 and 05 only
  image_base     the lowest populated address is 0x60030000: nothing below it (the bootloader's flash) is in the file
  address_range  every populated address is below 0x60800000, the 8 MiB an installed image is known to reach
  contiguous     one run from the base to the last byte, no hole (an updater that fails on gaps is a known risk of this family)
  head_present   the vector table (704 bytes: 16 system vectors + 160 device interrupts) is fully populated
  initial_sp     the word at the base is an 8-aligned address inside the DTCM (0x20000000 < SP <= 0x20058000)
  reset_vector   the word at base + 4 is a Thumb address inside the image, past the vector table
  vectors        every other entry is 0 or a Thumb address inside the image or (--ramfunc BASE:SIZE) inside the ITCM code region;
                 the reserved slots (7-10 and 13) are exactly 0
  start_record   a type 05 record, when present, carries the reset vector; with --expect-ptf it must be present
  line_endings   with --expect-ptf every line ends with CRLF (the official files' layout)
  end_rule       the image's last byte is not in the first 256-byte page of a new 4 KiB sector (boot_image.h padded_end)

With --delete-on-fail a failing file is removed, so no image that failed its gate is left lying around.
"""
import argparse
import json
import os
import struct
import sys
import tempfile
from pathlib import Path

IMAGE_BASE = 0x60030000
IMAGE_MAX_END = 0x60800000
DTCM_BASE = 0x20000000
DTCM_END = 0x20058000
VECTORS = 16 + 160
VECTOR_BYTES = VECTORS * 4
RESERVED_SLOTS = (7, 8, 9, 10, 13)
SECTOR = 4096
PAGE = 256


class ImageError(Exception):
    pass


def parse_hex(text):
    """Returns (memory {address: byte}, start address or None, crlf count, line count)."""
    memory = {}
    upper = 0
    start = None
    seen_eof = False
    crlf = 0
    lines = 0
    for raw_line in text.split("\n"):
        line = raw_line
        if line.endswith("\r"):
            crlf += 1
            line = line[:-1]
        if not line:
            continue
        lines += 1
        if seen_eof:
            raise ImageError("records after the end-of-file record")
        if line[0] != ":" or len(line) < 11 or len(line) % 2 == 0:
            raise ImageError("malformed record: %r" % line[:20])
        try:
            raw = bytes.fromhex(line[1:])
        except ValueError:
            raise ImageError("non-hex record: %r" % line[:20])
        if sum(raw) & 0xFF:
            raise ImageError("record checksum: %r" % line[:20])
        if raw[0] != len(raw) - 5:
            raise ImageError("record length: %r" % line[:20])
        kind, address16, payload = raw[3], (raw[1] << 8) | raw[2], raw[4:-1]
        if kind == 0:
            for i, b in enumerate(payload):
                address = upper + address16 + i
                if address in memory:
                    raise ImageError("address 0x%08X populated twice" % address)
                memory[address] = b
        elif kind == 4:
            if len(payload) != 2:
                raise ImageError("type 04 record length")
            upper = struct.unpack(">H", payload)[0] << 16
        elif kind == 5:
            if len(payload) != 4 or start is not None:
                raise ImageError("type 05 record length, or a second one")
            start = struct.unpack(">I", payload)[0]
        elif kind == 1:
            seen_eof = True
        else:
            raise ImageError("record type %d is not allowed (00, 01, 04, 05 only)" % kind)
    if not seen_eof:
        raise ImageError("no end-of-file record")
    if not memory:
        raise ImageError("no data")
    return memory, start, crlf, lines


def word(memory, address):
    return struct.unpack("<I", bytes(memory.get(address + i, 0) for i in range(4)))[0]


def verdicts(text, ramfunc=None, expect_ptf=False):
    results = []

    def add(name, ok, detail):
        results.append({"check": name, "ok": bool(ok), "detail": detail})

    try:
        memory, start, crlf, lines = parse_hex(text)
    except ImageError as exc:
        add("parse", False, str(exc))
        return results
    add("parse", True, "%d records" % lines)
    low, high = min(memory), max(memory) + 1
    add("image_base", low == IMAGE_BASE, "lowest address 0x%08X (expected 0x%08X; nothing below the image base may be written)" % (low, IMAGE_BASE))
    add("address_range", high <= IMAGE_MAX_END, "highest address 0x%08X, limit 0x%08X" % (high - 1, IMAGE_MAX_END - 1))
    add("contiguous", len(memory) == high - low, "%d bytes populated over a span of %d" % (len(memory), high - low))
    head = all(IMAGE_BASE + i in memory for i in range(VECTOR_BYTES))
    add("head_present", head, "vector table 0x%08X..0x%08X %s" % (IMAGE_BASE, IMAGE_BASE + VECTOR_BYTES - 1, "populated" if head else "NOT fully populated"))
    sp = word(memory, IMAGE_BASE)
    add("initial_sp", DTCM_BASE < sp <= DTCM_END and sp % 8 == 0, "initial SP 0x%08X" % sp)
    reset = word(memory, IMAGE_BASE + 4)
    reset_ok = (reset & 1) == 1 and IMAGE_BASE + VECTOR_BYTES <= (reset & ~1) < high and (reset & ~1) in memory
    add("reset_vector", reset_ok, "reset vector 0x%08X" % reset)
    bad = []
    for slot in range(2, VECTORS):
        v = word(memory, IMAGE_BASE + 4 * slot)
        if slot in RESERVED_SLOTS:
            if v != 0:
                bad.append("slot %d (reserved) = 0x%08X" % (slot, v))
            continue
        if v == 0:
            continue
        target = v & ~1
        in_flash = IMAGE_BASE + VECTOR_BYTES <= target < high and target in memory
        in_ramfunc = ramfunc is not None and ramfunc[0] <= target < ramfunc[0] + ramfunc[1]
        if (v & 1) == 0 or not (in_flash or in_ramfunc):
            bad.append("slot %d = 0x%08X" % (slot, v))
    add("vectors", not bad, "all %d entries in the image%s" % (VECTORS, " or the ITCM code region" if ramfunc else "") if not bad else "; ".join(bad[:8]))
    if start is None:
        add("start_record", not expect_ptf, "no type 05 record" + (" (a .ptf carries one)" if expect_ptf else ""))
    else:
        add("start_record", start == reset, "start address 0x%08X %s the reset vector" % (start, "is" if start == reset else "differs from"))
    if expect_ptf:
        add("line_endings", crlf == lines, "%d of %d lines end with CRLF" % (crlf, lines))
    in_sector = high % SECTOR
    page0 = in_sector != 0 and in_sector <= PAGE
    add("end_rule", not page0, "image ends at 0x%08X, %s" % (high, "in the first page of a sector: the bootloader would program it without an erase" if page0 else "fine"))
    return results


def load_text(path):
    data = Path(path).read_bytes()
    try:
        return data.decode("ascii")
    except UnicodeDecodeError:
        raise ImageError("not an ASCII file")


def run_check(path, ramfunc, expect_ptf, delete_on_fail, json_out):
    try:
        results = verdicts(load_text(path), ramfunc, expect_ptf)
    except ImageError as exc:
        results = [{"check": "parse", "ok": False, "detail": str(exc)}]
    for r in results:
        print("%s %s: %s" % ("PASS" if r["ok"] else "FAIL", r["check"], r["detail"]))
    failed = [r["check"] for r in results if not r["ok"]]
    if json_out:
        Path(json_out).write_text(json.dumps({"image": str(path), "results": results, "ok": not failed}, indent=2) + "\n", encoding="utf-8")
    if failed:
        print("check_image: FAILED (%s)" % ", ".join(failed))
        if delete_on_fail and os.path.exists(path):
            os.remove(path)
            print("check_image: removed %s" % path)
        return 1
    print("check_image: all %d checks passed (nothing was flashed or handed over)" % len(results))
    return 0


def parse_ramfunc(text):
    if not text:
        return None
    base, size = text.split(":")
    return int(base, 0), int(size, 0)


# ---- self-test --------------------------------------------------------------------------------------------------------------------
def encode(memory_bytes, base, start=None, crlf=True, data_len=16):
    def rec(kind, a16, payload):
        raw = bytes([len(payload), (a16 >> 8) & 0xFF, a16 & 0xFF, kind]) + payload
        return ":" + (raw + bytes([(-sum(raw)) & 0xFF])).hex().upper()
    lines = []
    upper = None
    for off in range(0, len(memory_bytes), data_len):
        a = base + off
        if (a >> 16) != upper:
            upper = a >> 16
            lines.append(rec(4, 0, struct.pack(">H", upper)))
        lines.append(rec(0, a & 0xFFFF, memory_bytes[off:off + data_len]))
    if start is not None:
        lines.append(rec(5, 0, struct.pack(">I", start)))
    lines.append(rec(1, 0, b""))
    return ("\r\n" if crlf else "\n").join(lines) + ("\r\n" if crlf else "\n")


def good_image(size=0x1120, ramfunc_slot=None):
    body = bytearray(b"\x11" * size)
    struct.pack_into("<II", body, 0, DTCM_END, IMAGE_BASE + VECTOR_BYTES + 1)
    for slot in range(2, VECTORS):
        struct.pack_into("<I", body, 4 * slot, 0 if slot in RESERVED_SLOTS else IMAGE_BASE + VECTOR_BYTES + 0x11)
    if ramfunc_slot is not None:
        struct.pack_into("<I", body, 4 * ramfunc_slot, 0x00000101)
    return bytes(body)


def self_test():
    failures = []

    def expect(label, ok):
        if not ok:
            failures.append(label)

    def failed_checks(text, **kw):
        return [r["check"] for r in verdicts(text, **kw) if not r["ok"]]

    def expect_fail(label, text, name, **kw):
        expect(label + " fails " + name, name in failed_checks(text, **kw))

    img = good_image()
    expect("good .ptf passes", failed_checks(encode(img, IMAGE_BASE, start=IMAGE_BASE + VECTOR_BYTES + 1), expect_ptf=True) == [])
    expect("good .hex (LF, no start) passes", failed_checks(encode(img, IMAGE_BASE, crlf=False)) == [])
    expect_fail("LF .ptf", encode(img, IMAGE_BASE, start=IMAGE_BASE + VECTOR_BYTES + 1, crlf=False), "line_endings", expect_ptf=True)
    expect_fail("no start record in a .ptf", encode(img, IMAGE_BASE), "start_record", expect_ptf=True)
    expect_fail("wrong start record", encode(img, IMAGE_BASE, start=IMAGE_BASE + 0x301), "start_record")
    expect_fail("below the base", encode(img, IMAGE_BASE - 0x1000), "image_base")
    expect_fail("too high", encode(img, IMAGE_MAX_END - 0x1000), "address_range")
    expect_fail("hole", encode(img[:0x800], IMAGE_BASE) .replace(":00000001FF", "") + encode(img[0x900:], IMAGE_BASE + 0x900), "contiguous")
    expect_fail("short head", encode(img[:0x200], IMAGE_BASE), "head_present")
    bad = bytearray(img)
    struct.pack_into("<I", bad, 0, 0x20060000)
    expect_fail("sp past the DTCM", encode(bytes(bad), IMAGE_BASE), "initial_sp")
    bad = bytearray(img)
    struct.pack_into("<I", bad, 4, IMAGE_BASE + VECTOR_BYTES)
    expect_fail("reset not Thumb", encode(bytes(bad), IMAGE_BASE), "reset_vector")
    bad = bytearray(img)
    struct.pack_into("<I", bad, 4 * 7, 0x60031001)
    expect_fail("reserved slot populated", encode(bytes(bad), IMAGE_BASE), "vectors")
    bad = bytearray(img)
    struct.pack_into("<I", bad, 4 * 20, 0x20001001)
    expect_fail("vector into RAM", encode(bytes(bad), IMAGE_BASE), "vectors")
    ram = good_image(ramfunc_slot=20)
    expect("vector into the ITCM region passes with --ramfunc", failed_checks(encode(ram, IMAGE_BASE), ramfunc=(0, 0x18000)) == [])
    expect_fail("vector into the ITCM without --ramfunc", encode(ram, IMAGE_BASE), "vectors")
    expect_fail("end in page 0", encode(good_image(0x10D5), IMAGE_BASE), "end_rule")
    expect("end in page 1 passes", failed_checks(encode(good_image(0x110D), IMAGE_BASE)) == [])
    expect("end on a sector boundary passes", failed_checks(encode(good_image(0x1000), IMAGE_BASE)) == [])
    expect_fail("bad checksum", encode(img, IMAGE_BASE).replace(":10", ":11", 1), "parse")
    expect_fail("type 02 record", ":020000021000EC\r\n" + encode(img, IMAGE_BASE), "parse")
    with tempfile.TemporaryDirectory() as tmp:
        p = Path(tmp) / "TrackerPlus_t.ptf"
        p.write_text(encode(img, IMAGE_BASE, start=IMAGE_BASE + VECTOR_BYTES + 1))
        j = Path(tmp) / "r.json"
        expect("CLI passes", run_check(str(p), None, True, False, str(j)) == 0 and json.loads(j.read_text())["ok"] is True)
        p.write_text(encode(img, IMAGE_BASE - 0x1000))
        expect("CLI fails and deletes", run_check(str(p), None, False, True, None) == 1 and not p.exists())
    if failures:
        print("check_image self-test FAILED:\n  " + "\n  ".join(failures))
        return 1
    print("check_image self-test: all checks passed")
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true")
    sub = parser.add_subparsers(dest="command")
    p = sub.add_parser("check")
    p.add_argument("image")
    p.add_argument("--ramfunc", help="BASE:SIZE of the ITCM code region a vector may point into")
    p.add_argument("--expect-ptf", action="store_true", help="the file is a .ptf: CRLF line endings and a type 05 record are required")
    p.add_argument("--delete-on-fail", action="store_true")
    p.add_argument("--json")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.command != "check":
        parser.print_help()
        return 2
    return run_check(args.image, parse_ramfunc(args.ramfunc), args.expect_ptf, args.delete_on_fail, args.json)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
