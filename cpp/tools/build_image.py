#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build the EXPERIMENTAL OpenCFS image for the K2 CFS board (GD32F303VE).

Compiles the core, the board port and board/k2cfs/image/ with arm-none-eabi-g++,
links them at 0x08003000 (board/k2cfs/image/gd32f303xe_app.ld) and checks the
result. It writes ELF, map and an UNSEALED raw binary: the Creality header
(version, CRC-16, length at +0x200) is left erased for the packager. Nothing here
talks to a printer or flashes anything.

usage: build_image.py [--toolchain DIR] [--out DIR]
"""
import argparse
import concurrent.futures
import os
import pathlib
import shutil
import struct
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent  # cpp/ (or the private src/)
BOARD = ROOT / "board" / "k2cfs"
LDSCRIPT = BOARD / "image" / "gd32f303xe_app.ld"
APP_BASE, PARAM_PAGE, RAM_END = 0x08003000, 0x0807F800, 0x20010000
HEADER = slice(0x200, 0x212)

CPU = ["-mcpu=cortex-m4", "-mthumb", "-mfpu=fpv4-sp-d16", "-mfloat-abi=hard"]
CXXFLAGS = CPU + [
    "-std=c++17", "-Os", "-g", "-ffunction-sections", "-fdata-sections",
    "-fno-exceptions", "-fno-rtti", "-fno-threadsafe-statics", "-fno-use-cxa-atexit",
    "-ffreestanding", "-Wall", "-Wextra", "-Werror", "-Wpedantic",
    "-I" + str(ROOT / "include"), "-I" + str(BOARD),
]
# Allowed from the C library: memory and string helpers only. Anything that needs
# a heap, stdio, exit handling or system calls means something pulled in a
# hosted feature by mistake.
FORBIDDEN = {"malloc", "free", "calloc", "realloc", "_sbrk", "_sbrk_r", "printf", "puts",
             "abort", "exit", "_exit", "atexit", "__cxa_atexit", "__cxa_guard_acquire",
             "_write", "_read", "_close", "_lseek", "_fstat", "_isatty", "__assert_func"}


def sources():
    files = sorted((ROOT / "lib").glob("*.cpp")) + sorted((ROOT / "lib" / "tags").glob("*.cpp"))
    files += sorted(BOARD.glob("*.cpp")) + sorted((BOARD / "image").glob("*.cpp"))
    return files


def tool(bindir, name):
    exe = name + (".exe" if os.name == "nt" else "")
    if bindir:
        path = pathlib.Path(bindir) / exe
        if not path.is_file():
            sys.exit("missing %s" % path)
        return str(path)
    found = shutil.which(name)
    if not found:
        sys.exit("%s not found: pass --toolchain or set ARM_TOOLCHAIN" % name)
    return found


def check(ok, message, problems):
    if not ok:
        problems.append(message)


def verify(raw, nm_out):
    problems = []
    sp, reset = struct.unpack_from("<II", raw, 0)
    check(0x20000000 < sp <= RAM_END and sp % 8 == 0, "initial MSP 0x%08x outside SRAM" % sp, problems)
    check(reset & 1 and APP_BASE <= (reset & ~1) < APP_BASE + len(raw),
          "reset vector 0x%08x not a Thumb address inside the image" % reset, problems)
    check(all(b == 0xFF for b in raw[HEADER]), "+0x200..+0x211 is not reserved for the header", problems)
    check(len(raw) <= PARAM_PAGE - APP_BASE, "image reaches the parameter page", problems)
    symbols = {line.split()[-1] for line in nm_out.splitlines() if line.strip()}
    for name in sorted(FORBIDDEN & symbols):
        problems.append("hosted C library symbol linked in: " + name)
    for name in ("Reset_Handler", "Fault_Handler", "SysTick_Handler", "USART1_IRQHandler",
                 "EXTI10_15_IRQHandler"):
        check(name in symbols, "missing " + name, problems)
    return problems, sp, reset


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--toolchain", default=os.environ.get("ARM_TOOLCHAIN"),
                    help="directory holding arm-none-eabi-g++")
    ap.add_argument("--out", default="build-arm")
    a = ap.parse_args()
    out = pathlib.Path(a.out).resolve()
    (out / "obj").mkdir(parents=True, exist_ok=True)
    gxx, objcopy, nm, size = (tool(a.toolchain, "arm-none-eabi-" + t) for t in ("g++", "objcopy", "nm", "size"))

    keys = sorted((BOARD / "local").glob("*.hpp")) if (BOARD / "local").is_dir() else []
    if keys:
        print("WARNING: local vendor keys compiled in (%s): never publish this image"
              % ", ".join(k.name for k in keys))

    def compile_one(src):
        obj = out / "obj" / (src.relative_to(ROOT).as_posix().replace("/", "_") + ".o")
        r = subprocess.run([gxx, *CXXFLAGS, "-c", str(src), "-o", str(obj)], capture_output=True, text=True)
        return src, obj, r

    objs, failed = [], False
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        for src, obj, r in pool.map(compile_one, sources()):
            if r.returncode:
                failed = True
                print("FAILED", src.relative_to(ROOT), "\n" + r.stdout + r.stderr)
            objs.append(str(obj))
    if failed:
        return 1

    elf = out / "opencfs.elf"
    link = [gxx, *CPU, "-T", str(LDSCRIPT), "-nostartfiles", "--specs=nano.specs",
            "-Wl,--gc-sections", "-Wl,--no-warn-rwx-segments", "-Wl,-Map=" + str(out / "opencfs.map"),
            "-Wl,--print-memory-usage", *objs, "-lgcc", "-o", str(elf)]
    r = subprocess.run(link, capture_output=True, text=True)
    print(r.stdout.strip())
    if r.returncode:
        print("LINK FAILED\n" + r.stderr)
        return 1

    raw_path = out / "opencfs-unsealed.bin"
    subprocess.run([objcopy, "-O", "binary", str(elf), str(raw_path)], check=True)
    raw = raw_path.read_bytes()
    nm_out = subprocess.run([nm, str(elf)], capture_output=True, text=True, check=True).stdout
    problems, sp, reset = verify(raw, nm_out)
    print(subprocess.run([size, str(elf)], capture_output=True, text=True).stdout.strip())
    print("image 0x%x bytes, MSP 0x%08x, reset 0x%08x, %d sources" % (len(raw), sp, reset, len(objs)))
    if problems:
        print("IMAGE CHECK FAILED:\n  " + "\n  ".join(problems))
        return 1
    print("ELF:", elf)
    print("UNSEALED BIN (no Creality header yet, EXPERIMENTAL, NOT QUALIFIED):", raw_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
