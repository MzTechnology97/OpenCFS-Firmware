#!/usr/bin/env python3
"""Print words of a CFS image (flash 0x08003000) or SRAM init image (0x20000000).

usage: peek.py IMAGE SRAM_IMAGE ADDR [ADDR...]
Each ADDR prints u32, i32, float and, when the word points into flash/SRAM, the target word.
"""
import struct
import sys

FLASH, SRAM = 0x08003000, 0x20000000


def word(img: bytes, ram: bytes, a: int):
    if FLASH <= a < FLASH + len(img) - 3:
        return struct.unpack_from("<I", img, a - FLASH)[0]
    if SRAM <= a < SRAM + len(ram) - 3:
        return struct.unpack_from("<I", ram, a - SRAM)[0]
    return None


def main() -> None:
    img, ram = (open(p, "rb").read() for p in sys.argv[1:3])
    for arg in sys.argv[3:]:
        a = int(arg, 16)
        w = word(img, ram, a)
        if w is None:
            print(f"{a:#010x}  out of range")
            continue
        f = struct.unpack("<f", struct.pack("<I", w))[0]
        tgt = word(img, ram, w)
        extra = f"  -> [{w:#010x}] = {tgt:#010x}" if tgt is not None else ""
        print(f"{a:#010x}  u32 {w:#010x}  i32 {struct.unpack('<i', struct.pack('<I', w))[0]:>11}  f32 {f:<12.6g}{extra}")


if __name__ == "__main__":
    main()
