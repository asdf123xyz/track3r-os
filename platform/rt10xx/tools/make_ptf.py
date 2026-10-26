#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes a Tracker+ firmware file (.ptf) from a flat image of the rt10xx build (OS-PORT-RT10XX; stdlib only).

    make_ptf.py <image.bin> <TrackerPlus_<name>.ptf> [--base 0x60030000] [--allow-any-name]
    make_ptf.py --self-test

A `.ptf` is Intel HEX with CRLF line endings: type 04 (extended linear address) records, type 00 data records, one type 05 (start linear
address: the reset handler, read from the image's vector table) and one type 01. The image is one contiguous run from the base; nothing
below the base (the bootloader's part of the flash) is ever written. What the tool refuses or applies, with the facts behind it
(platform/rt10xx/include/o3os/platform/rt10xx/boot_image.h, docs/specs/OS-PORT-RT10XX.md section 2):

  base       the image must start at 0x60030000, where the bootloader programs and hands over;
  size       it must end within the 8 MiB of flash an installed image is known to reach (and must not be empty);
  vectors    the first word is a stack pointer inside the DTCM, the second the reset handler (Thumb) inside the image;
  end rule   an image whose last byte falls in the first 256-byte page of a new 4 KiB sector is padded with 0xFF to 16 bytes into the
             second page (the bootloader programs such a last page without erasing the sector);
  name       the output's file name is one the unit's update list shows: "TrackerPlus<...>.ptf" (--allow-any-name for a scratch file).

Nothing here flashes, copies to removable media or hands over an image: the file is a build artifact, not a candidate (fw-gate-check).
"""
import argparse
import struct
import sys
import tempfile
from pathlib import Path

IMAGE_BASE = 0x60030000
IMAGE_MAX_END = 0x60800000
DTCM_BASE = 0x20000000
DTCM_END = 0x20058000
SECTOR = 4096
PAGE = 256
PAGE_PAD = 16
RECORD_BYTES = 16
NAME_PREFIX = "TrackerPlus"
NAME_SUFFIX = ".ptf"


class PtfError(Exception):
    pass


def padded_end(end):
    """The end rule of boot_image.h padded_end(): the end to pad to with 0xFF (the same value when nothing is needed)."""
    in_sector = end % SECTOR
    if in_sector == 0 or in_sector > PAGE:
        return end
    return end - in_sector + PAGE + PAGE_PAD


def listed_name(name):
    return name.startswith(NAME_PREFIX) and name.endswith(NAME_SUFFIX) and len(name) > len(NAME_PREFIX) + len(NAME_SUFFIX)


def record(kind, address16, payload):
    raw = bytes([len(payload), (address16 >> 8) & 0xFF, address16 & 0xFF, kind]) + payload
    return ":" + (raw + bytes([(-sum(raw)) & 0xFF])).hex().upper()


def check_image(image, base):
    if base != IMAGE_BASE:
        raise PtfError("the image base is 0x%08X, not 0x%08X (the bootloader's hand-over address)" % (base, IMAGE_BASE))
    if len(image) < 8:
        raise PtfError("the image is too small to hold a vector table")
    end = base + len(image)
    if end > IMAGE_MAX_END:
        raise PtfError("the image ends at 0x%08X, past the 8 MiB of flash an installed image is known to reach (0x%08X)" % (end, IMAGE_MAX_END))
    sp, reset = struct.unpack_from("<II", image, 0)
    if not (DTCM_BASE < sp <= DTCM_END) or sp % 8 != 0:
        raise PtfError("the initial stack pointer 0x%08X is not an 8-aligned address inside the DTCM" % sp)
    if (reset & 1) == 0 or not (base <= (reset & ~1) < end):
        raise PtfError("the reset vector 0x%08X is not a Thumb address inside the image" % reset)
    return reset


def encode(image, base):
    """The .ptf text (CRLF) of a flat image at `base`, padded by the end rule."""
    reset = check_image(image, base)
    end = base + len(image)
    pad_to = padded_end(end)
    image = bytes(image) + b"\xff" * (pad_to - end)
    lines = []
    upper = None
    for offset in range(0, len(image), RECORD_BYTES):
        address = base + offset
        chunk = image[offset:offset + RECORD_BYTES]
        # A data record never crosses a 64 KiB boundary (RECORD_BYTES divides 65536 and the base is aligned), so one 04 record per 64 KiB.
        if (address >> 16) != upper:
            upper = address >> 16
            lines.append(record(4, 0, struct.pack(">H", upper)))
        lines.append(record(0, address & 0xFFFF, chunk))
    lines.append(record(5, 0, struct.pack(">I", reset)))
    lines.append(record(1, 0, b""))
    return "\r\n".join(lines) + "\r\n", pad_to - end


def decode(text):
    """A tiny reader for the self-test: {address: byte}, the start address, and the line endings seen."""
    memory = {}
    upper = 0
    start = None
    crlf = text.count("\r\n")
    for line in text.replace("\r\n", "\n").split("\n"):
        if not line:
            continue
        raw = bytes.fromhex(line[1:])
        if line[0] != ":" or sum(raw) & 0xFF or raw[0] != len(raw) - 5:
            raise PtfError("bad record: %s" % line)
        kind, address16, payload = raw[3], (raw[1] << 8) | raw[2], raw[4:-1]
        if kind == 0:
            for i, b in enumerate(payload):
                memory[upper + address16 + i] = b
        elif kind == 4:
            upper = struct.unpack(">H", payload)[0] << 16
        elif kind == 5:
            start = struct.unpack(">I", payload)[0]
        elif kind == 1:
            break
        else:
            raise PtfError("record type %d" % kind)
    return memory, start, crlf


def write(bin_path, ptf_path, base, allow_any_name):
    name = Path(ptf_path).name
    if not allow_any_name and not listed_name(name):
        raise PtfError("'%s' is not a name the unit's update list shows (TrackerPlus<...>.ptf)" % name)
    image = Path(bin_path).read_bytes()
    text, padding = encode(image, base)
    Path(ptf_path).write_bytes(text.encode("ascii"))
    print("make_ptf: wrote %s: %d bytes of image at 0x%08X%s, %d records" %
          (ptf_path, len(image), base, (", %d bytes of 0xFF padding (the sector end rule)" % padding) if padding else "", text.count("\r\n")))
    return 0


def self_test():
    failures = []

    def expect(label, ok):
        if not ok:
            failures.append(label)

    def fake_image(size, sp=DTCM_END, reset=IMAGE_BASE + 0x2C1):
        body = bytearray(range(256)) * (size // 256 + 1)
        body = body[:size]
        struct.pack_into("<II", body, 0, sp, reset)
        return bytes(body)

    # Round trip, record layout, CRLF, the start record.
    image = fake_image(0x1234)
    text, padding = encode(image, IMAGE_BASE)
    memory, start, crlf = decode(text)
    expect("round trip", all(memory.get(IMAGE_BASE + i) == image[i] for i in range(len(image))))
    expect("no byte below the base", min(memory) == IMAGE_BASE)
    expect("start record is the reset vector", start == IMAGE_BASE + 0x2C1)
    expect("every line ends CRLF", crlf == text.count("\n") and not text.replace("\r\n", "").count("\r"))
    expect("no padding for an end in page 1 or later (0x1234 % 4096 = 0x234)", padding == 0 and max(memory) == IMAGE_BASE + 0x1233)
    expect("first record is a 04 for 0x6003", text.startswith(":020000046003"))
    # A record never crosses a 64 KiB boundary; a new 04 record appears at it.
    big = fake_image(0x10120)  # ends 0x120 into a sector: page 1, no padding
    text, _ = encode(big, IMAGE_BASE)
    expect("04 record at 0x60040000", ":020000046004" in text and text.count(":02000004") == 2)
    memory, _, _ = decode(text)
    expect("big round trip", len(memory) == 0x10120 and memory[IMAGE_BASE + 0x1011F] == big[0x1011F])
    # The end rule (FINDINGS 26): an end 0xD5 into a sector is padded to 0x110.
    size = 0x2000 + 0xD5
    text, padding = encode(fake_image(size), IMAGE_BASE)
    memory, _, _ = decode(text)
    expect("page-0 end padded", padding == 0x110 - 0xD5 and max(memory) == IMAGE_BASE + 0x2000 + 0x110 - 1 and memory[max(memory)] == 0xFF)
    expect("padded_end cases", padded_end(0x601BB0D5) == 0x601BB110 and padded_end(0x601BB10D) == 0x601BB10D and padded_end(0x601BB000) == 0x601BB000
           and padded_end(0x601BB100) == 0x601BB110 and padded_end(0x601BB101) == 0x601BB101)
    # Refusals.
    for label, image, base in (
            ("wrong base", fake_image(0x100), 0x60000000),
            ("too large", fake_image(0x100), IMAGE_MAX_END - 0x80),
            ("tiny", b"\x00\x00\x00\x20", IMAGE_BASE),
            ("sp outside the DTCM", fake_image(0x100, sp=0x20060000), IMAGE_BASE),
            ("sp unaligned", fake_image(0x100, sp=DTCM_END - 4), IMAGE_BASE),
            ("reset not Thumb", fake_image(0x100, reset=IMAGE_BASE + 0x40), IMAGE_BASE),
            ("reset outside the image", fake_image(0x100, reset=IMAGE_BASE + 0x1001), IMAGE_BASE)):
        try:
            encode(image, base)
            expect("refused: " + label, False)
        except PtfError:
            pass
    expect("name rule", listed_name("TrackerPlus_o3os.ptf") and not listed_name("trackerplus_o3os.ptf") and not listed_name("TrackerPlus.ptf")
           and not listed_name("TrackerPlus_o3os.PTF") and not listed_name("track3r-os.ptf"))
    with tempfile.TemporaryDirectory() as tmp:
        bin_path = Path(tmp) / "o3os.bin"
        bin_path.write_bytes(fake_image(0x3000))
        try:
            write(str(bin_path), str(Path(tmp) / "o3os.ptf"), IMAGE_BASE, False)
            expect("refused: unlisted name", False)
        except PtfError:
            pass
        out = Path(tmp) / "TrackerPlus_test.ptf"
        expect("write", write(str(bin_path), str(out), IMAGE_BASE, False) == 0 and out.read_bytes().endswith(b":00000001FF\r\n"))
        expect("the file holds only ASCII and CRLF", all(b < 128 for b in out.read_bytes()))
    if failures:
        print("make_ptf self-test FAILED:\n  " + "\n  ".join(failures))
        return 1
    print("make_ptf self-test: all checks passed")
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("bin", nargs="?")
    parser.add_argument("ptf", nargs="?")
    parser.add_argument("--base", default="0x60030000", help="the flat image's load address (must be 0x60030000)")
    parser.add_argument("--allow-any-name", action="store_true", help="skip the TrackerPlus<...>.ptf name rule (a scratch file)")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.bin or not args.ptf:
        parser.print_help()
        return 2
    try:
        return write(args.bin, args.ptf, int(args.base, 0), args.allow_any_name)
    except PtfError as exc:
        print("make_ptf: %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
