#!/usr/bin/env python3
"""Linear Thumb disassembly of a CFS app range (linked at 0x08003000).

usage: thumbdis.py IMAGE START END
Literal-pool loads are annotated with the loaded word.
"""
import struct
import sys

from capstone import CS_ARCH_ARM, CS_MODE_MCLASS, CS_MODE_THUMB, Cs

BASE = 0x08003000


def main() -> None:
    img = open(sys.argv[1], "rb").read()
    start, end = (int(a, 16) for a in sys.argv[2:4])
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_MCLASS)
    md.skipdata = True
    for ins in md.disasm(img[start - BASE:end - BASE], start):
        note = ""
        if ins.mnemonic.startswith("ldr") and "[pc," in ins.op_str:
            try:
                imm = int(ins.op_str.rsplit("#", 1)[1].rstrip("]"), 0)
                lit = ((ins.address + 4) & ~3) + imm
                note = f"   ; [{lit:#010x}] = {struct.unpack_from('<I', img, lit - BASE)[0]:#010x}"
            except (ValueError, IndexError, struct.error):
                pass
        print(f"{ins.address:08x}  {ins.bytes.hex():<10} {ins.mnemonic:<8} {ins.op_str}{note}")


if __name__ == "__main__":
    main()
