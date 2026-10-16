#!/usr/bin/env python3
"""Rebuild the initial SRAM image of a CFS application (Keil scatter-load).

The CFS app is linked at 0x08003000. Its Keil scatter table lists one
compressed RW region and one zero-init region. This tool walks the table,
expands the RW data with the Keil LZ-style token format and writes a
flat SRAM image starting at 0x20000000, ready to load into Ghidra.

Offline only: reads the image, writes one output file.
"""
from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path

LINK_BASE = 0x08003000
SRAM_BASE = 0x20000000
SRAM_SIZE = 0x10000  # GD32F303VE: 64 KiB


def u32(img: bytes, addr: int) -> int:
    return struct.unpack_from("<I", img, addr - LINK_BASE)[0]


def find_scatter_table(img: bytes) -> int:
    """Return the address of the first 16-byte record whose destination is SRAM_BASE."""
    for off in range(0, len(img) - 32, 4):
        src, dst, size, handler = struct.unpack_from("<IIII", img, off)
        if dst != SRAM_BASE or not (0 < size < SRAM_SIZE):
            continue
        if not (LINK_BASE <= src < LINK_BASE + len(img)):
            continue
        if not (LINK_BASE <= (handler & ~1) < LINK_BASE + len(img)):
            continue
        nsrc, ndst, nsize, _ = struct.unpack_from("<IIII", img, off + 16)
        if ndst == dst + size and nsize < SRAM_SIZE:
            return LINK_BASE + off
    raise SystemExit("scatter table not found")


def decompress(img: bytes, src: int, limit: int, out_len: int) -> bytes:
    p, end = src - LINK_BASE, limit - LINK_BASE
    out = bytearray()

    def take() -> int:
        nonlocal p
        if p >= end:
            raise ValueError("compressed stream overruns its region")
        b = img[p]
        p += 1
        return b

    while len(out) < out_len:
        tag = take()
        lit = tag & 7 or take()
        rep = tag >> 4 or take()
        out += bytes(take() for _ in range(lit - 1))
        if tag & 8:
            dist = take()
            if not 0 < dist <= len(out):
                raise ValueError(f"bad back-reference distance {dist}")
            for _ in range(rep + 2):
                out.append(out[-dist])
        else:
            out += bytes(rep)
    if len(out) != out_len:
        raise ValueError(f"decoded {len(out)} bytes, expected {out_len}")
    return bytes(out)


def main() -> None:
    global LINK_BASE
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("image", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--base", type=lambda v: int(v, 0), default=LINK_BASE,
                    help="link base of the image (default 0x08003000; loader 0x08000000)")
    args = ap.parse_args()
    LINK_BASE = args.base

    img = args.image.read_bytes()
    table = find_scatter_table(img)
    (src1, dst1, len1, _), (src2, dst2, len2, _) = (
        struct.unpack_from("<IIII", img, table - LINK_BASE + i * 16) for i in (0, 1)
    )
    rw = decompress(img, src1, src2, len1)
    ram = bytearray(SRAM_SIZE)
    ram[dst1 - SRAM_BASE : dst1 - SRAM_BASE + len1] = rw
    args.out.write_bytes(ram)

    print(f"scatter table      {table:#010x}")
    print(f"RW   {src1:#010x} -> {dst1:#010x}  {len1} bytes  sha256 {hashlib.sha256(rw).hexdigest()}")
    print(f"ZI   {dst2:#010x}  {len2} bytes (end {dst2 + len2:#010x})")
    print(f"wrote {args.out} ({len(ram)} bytes at {SRAM_BASE:#010x})")


if __name__ == "__main__":
    main()
