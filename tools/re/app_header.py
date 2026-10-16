#!/usr/bin/env python3
"""Check the loader header of CFS application images (docs/01, image format).

usage: app_header.py IMAGE [IMAGE...]
Header at file offset 0x20C: CRC-16 (u16 LE) + image length (u32 LE).
CRC-16: poly 0x8005, init 0, MSB first, over the file with those 6 bytes zeroed.
Exit status 1 if any image does not match.
"""
import struct
import sys

HDR = 0x20C


def crc16(data: bytes) -> int:
    c = 0
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x8005) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def main() -> int:
    bad = 0
    for path in sys.argv[1:]:
        img = bytearray(open(path, "rb").read())
        stored, length = struct.unpack_from("<HI", img, HDR)
        img[HDR:HDR + 6] = bytes(6)
        calc = crc16(img)
        ok = calc == stored and length == len(img)
        bad += not ok
        print(f"{'ok ' if ok else 'BAD'} crc {stored:#06x}/{calc:#06x} len {length}/{len(img)}  {path}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
