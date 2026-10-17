#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build the EXPERIMENTAL OpenCFS images for the K2 CFS board (GD32F303VE).

Three variants (docs/spec/13 section 5), each in its own directory:

  release   OpenCFS-RELEASE  the normal firmware
  inert     OpenCFS-INERT    boot and RS-485 only, no output can be driven
  bench     OpenCFS-BENCH    INERT plus the 0x7B sensor and actuator test commands
  busrel    OpenCFS-BUSREL   diagnostic: PA2 held high, clock up, then nothing (no USART)

Each one is compiled with arm-none-eabi-g++, linked at 0x08003000
(board/k2cfs/image/gd32f303xe_app.ld) and checked. The output is an ELF, a map
and an unsealed raw binary. With --seal it is also a sealed, named binary
  <boot>-<app>-OpenCFS-<VARIANT>-<revision>.bin  (+ .sha256)
carrying the Creality header (version, CRC-16, length). Nothing here talks to a
printer or flashes anything.

usage: build_image.py [--variant release|inert|bench|busrel|all] [--toolchain DIR] [--out DIR]
                      [--seal] [--boot cfs0_050_G32] [--app cfs0_000_153]
"""
import argparse
import concurrent.futures
import hashlib
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent  # cpp/ (or the private src/)
BOARD = ROOT / "board" / "k2cfs"
LDSCRIPT = BOARD / "image" / "gd32f303xe_app.ld"
APP_BASE, PARAM_PAGE, RAM_END = 0x08003000, 0x0807F800, 0x20010000
HEADER = slice(0x200, 0x212)
INFO_OFF = 0x220
VARIANTS = {"release": (1, "OpenCFS-RELEASE"), "inert": (2, "OpenCFS-INERT"), "bench": (3, "OpenCFS-BENCH"),
            "busrel": (4, "OpenCFS-BUSREL")}
BOOT_RE, APP_RE = re.compile(r"^cfs\d+_\d+_G\d+$"), re.compile(r"^cfs\d+_\d+_\d+$")

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


def crc16(image):
    """Creality container CRC-16: poly 0x8005, init 0, the six bytes at +0x20C zero."""
    crc = 0
    for off, value in enumerate(image):
        if 0x20C <= off < 0x212:
            value = 0
        crc ^= value << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x8005) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def seal(raw, app):
    image = bytearray(raw)
    image.extend(b"\xff" * (-len(image) % 4))
    image[0x200:0x20C] = app.encode("ascii")
    image[0x20C:0x212] = bytes(6)
    struct.pack_into("<I", image, 0x20E, len(image))
    struct.pack_into("<H", image, 0x20C, crc16(image))
    return bytes(image)


def revision():
    git = ["git", "-C", str(ROOT)]
    r = subprocess.run(git + ["rev-parse", "--short=9", "HEAD"], capture_output=True, text=True)
    if r.returncode:
        return "local"
    dirty = subprocess.run(git + ["status", "--porcelain", "--untracked-files=no"],
                           capture_output=True, text=True).stdout.strip()
    return r.stdout.strip() + (".d" if dirty else "")


def sources(variant):
    files = sorted((ROOT / "lib").glob("*.cpp")) + sorted((ROOT / "lib" / "tags").glob("*.cpp"))
    files += sorted(BOARD.glob("*.cpp")) + sorted((BOARD / "image").glob("*.cpp"))
    if variant != "bench":  # the test commands exist only in the BENCH image
        files = [f for f in files if f.name != "bench.cpp"]
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


def verify(raw, nm_out, variant):
    problems = []

    def check(ok, message):
        if not ok:
            problems.append(message)

    number, name = VARIANTS[variant]
    sp, reset = struct.unpack_from("<II", raw, 0)
    check(0x20000000 < sp <= RAM_END and sp % 8 == 0, "initial MSP 0x%08x outside SRAM" % sp)
    check(reset & 1 and APP_BASE <= (reset & ~1) < APP_BASE + len(raw),
          "reset vector 0x%08x not a Thumb address inside the image" % reset)
    check(all(b == 0xFF for b in raw[HEADER]), "+0x200..+0x211 is not reserved for the header")
    check(len(raw) <= PARAM_PAGE - APP_BASE, "image reaches the parameter page")
    info = raw[INFO_OFF:INFO_OFF + 40]
    check(info[:8] == b"OCFSINFO" and info[9] == number and info[24:40].rstrip(b"\0") == name.encode(),
          "image info at +0x220 does not say %s" % name)
    symbols = {line.split()[-1] for line in nm_out.splitlines() if line.strip()}
    for sym in sorted(FORBIDDEN & symbols):
        problems.append("hosted C library symbol linked in: " + sym)
    for sym in ("Reset_Handler", "Fault_Handler", "SysTick_Handler", "USART1_IRQHandler",
                "EXTI10_15_IRQHandler"):
        check(sym in symbols, "missing " + sym)
    bench = any("BenchCommands" in s for s in symbols)
    check(bench == (variant == "bench"),
          "bench test code %s in %s" % ("present" if bench else "missing", name))
    return problems, sp, reset


def build(variant, a, tools, rev):
    gxx, objcopy, nm, size = tools
    number, name = VARIANTS[variant]
    out = pathlib.Path(a.out).resolve() / variant
    (out / "obj").mkdir(parents=True, exist_ok=True)
    flags = CXXFLAGS + ["-DCFS_IMAGE_VARIANT=%d" % number, '-DCFS_BUILD_REVISION="%s"' % rev]

    def compile_one(src):
        obj = out / "obj" / (src.relative_to(ROOT).as_posix().replace("/", "_") + ".o")
        r = subprocess.run([gxx, *flags, "-c", str(src), "-o", str(obj)], capture_output=True, text=True)
        return src, obj, r

    objs, failed = [], False
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        for src, obj, r in pool.map(compile_one, sources(variant)):
            if r.returncode:
                failed = True
                print("FAILED", src.relative_to(ROOT), "\n" + r.stdout + r.stderr)
            objs.append(str(obj))
    if failed:
        return None

    elf = out / "opencfs.elf"
    link = [gxx, *CPU, "-T", str(LDSCRIPT), "-nostartfiles", "--specs=nano.specs",
            "-Wl,--gc-sections", "-Wl,--no-warn-rwx-segments", "-Wl,-Map=" + str(out / "opencfs.map"),
            *objs, "-lgcc", "-o", str(elf)]
    r = subprocess.run(link, capture_output=True, text=True)
    if r.returncode:
        print("LINK FAILED (%s)\n%s" % (name, r.stderr))
        return None
    raw_path = out / "opencfs-unsealed.bin"
    subprocess.run([objcopy, "-O", "binary", str(elf), str(raw_path)], check=True)
    raw = raw_path.read_bytes()
    nm_out = subprocess.run([nm, "-C", str(elf)], capture_output=True, text=True, check=True).stdout
    problems, sp, reset = verify(raw, nm_out, variant)
    sizes = subprocess.run([size, str(elf)], capture_output=True, text=True).stdout.splitlines()[-1].split()
    print("%-16s %6d bytes flash, %5s bytes RAM, MSP 0x%08x, reset 0x%08x, %d sources"
          % (name, len(raw), sizes[2], sp, reset, len(objs)))
    if problems:
        print("IMAGE CHECK FAILED (%s):\n  %s" % (name, "\n  ".join(problems)))
        return None
    result = {"elf": elf, "unsealed": raw_path}
    if a.seal:
        sealed = seal(raw, a.app)
        path = out / ("%s-%s-%s-%s.bin" % (a.boot, a.app, name, rev))
        path.write_bytes(sealed)
        digest = hashlib.sha256(sealed).hexdigest()
        (out / (path.name + ".sha256")).write_text("%s  %s\n" % (digest, path.name))
        result["sealed"] = path
        print("  sealed: %s\n  sha256: %s" % (path.name, digest))
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--variant", choices=[*VARIANTS, "all"], default="release")
    ap.add_argument("--toolchain", default=os.environ.get("ARM_TOOLCHAIN"),
                    help="directory holding arm-none-eabi-g++")
    ap.add_argument("--out", default="build-arm")
    ap.add_argument("--seal", action="store_true", help="also write the sealed, named binary")
    ap.add_argument("--boot", default="cfs0_050_G32", help="boot version for the file name")
    ap.add_argument("--app", default="cfs0_000_153", help="Creality version written at +0x200")
    a = ap.parse_args()
    if not BOOT_RE.match(a.boot) or not APP_RE.match(a.app) or len(a.app) != 12:
        sys.exit("--boot must look like cfs0_050_G32 and --app like cfs0_000_153 (12 characters)")
    tools = tuple(tool(a.toolchain, "arm-none-eabi-" + t) for t in ("g++", "objcopy", "nm", "size"))
    rev = revision()
    keys = sorted((BOARD / "local").glob("*.hpp")) if (BOARD / "local").is_dir() else []
    if keys:
        print("WARNING: local vendor keys compiled in (%s): never publish these images"
              % ", ".join(k.name for k in keys))
    print("revision %s; EXPERIMENTAL, NOT QUALIFIED ON HARDWARE" % rev)
    ok = True
    for variant in (VARIANTS if a.variant == "all" else [a.variant]):
        ok = build(variant, a, tools, rev) is not None and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
