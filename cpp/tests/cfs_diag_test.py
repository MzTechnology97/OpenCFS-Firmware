#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Drive tools/cfs_diag.py against linked OpenCFS images in the emulator.

The emulator (image_boot_test.Board) plays the CFS; the host hears its own
transmission back, as on the K2 bus. Needs `pip install unicorn`.

usage: cfs_diag_test.py BENCH.bin INERT.bin
"""
import builtins
import contextlib
import io
import os
import pathlib
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "tools"))
import cfs_diag  # noqa: E402
import image_boot_test as emu  # noqa: E402


class EmuLink:
    def __init__(self, image):
        self.b = emu.Board(open(image, "rb").read(), echo=True)
        self.b.start()
        self.b.run_ms(300)
        self.pending = b""

    def write(self, data):
        self.b.rx += data
        self.pending += data  # the host hears itself on the bus

    def read(self, seconds):
        self.b.run_ms(int(seconds * 1000))
        out = self.pending + bytes(self.b.tx)
        self.pending = b""
        self.b.tx.clear()
        return out


def run(link, *argv, answer=""):
    out = io.StringIO()
    old_input = builtins.input
    builtins.input = lambda prompt="": answer
    try:
        with contextlib.redirect_stdout(out):
            try:
                cfs_diag.main(list(argv), link=link)
                code = 0
            except SystemExit as e:
                code = e.code
                print(e.code if isinstance(e.code, str) else "")
    finally:
        builtins.input = old_input
    return code, out.getvalue()


def main():
    bench, inert = sys.argv[1], sys.argv[2]
    results = []

    def check(name, ok, detail=""):
        results.append((name, bool(ok), detail))

    uid_file = os.path.join(tempfile.mkdtemp(), "uid")
    common = ["--uid-file", uid_file]

    link = EmuLink(bench)
    code, out = run(link, *common, "enumerate")
    check("enumerate: uid found, 3 A0 replies", code == 0 and "found uid" in out and "3 replies" in out, out)
    code, out = run(link, *common, "identity")
    check("identity: BENCH, SENSORS_ONLY", "variant BENCH" in out and "SENSORS_ONLY" in out, out)
    code, out = run(link, *common, "version")
    check("version answered", code == 0 and "status 0" in out, out)
    code, out = run(link, *common, "state")
    check("state decoded", code == 0 and "state IDLE" in out, out)
    code, out = run(link, *common, "runtime")
    check("runtime: API v2, 28 parameters, features 0x2B", "v2, 28 parameters" in out and "0x2B" in out, out)
    code, out = run(link, "--uid-file", uid_file + ".missing", "feeder-jog", "0", "0", "60", answer="FEEDER-JOG")
    check("feeder-jog without a known uid refused before confirming", code != 0 and "unique ID unknown" in out, out)
    code, out = run(link, *common, "bench-tag", "0")
    check("bench-tag answered (no tag in the model)", code == 0 and "tag read: status 0" in out, out)
    code, out = run(link, *common, "bench-arm")
    check("bench-arm with the saved uid -> ACTUATOR_TEST", "level ACTUATOR_TEST" in out, out)
    link.b.run_ms(6000)  # an operator typing: more than the 5 s link-loss limit
    code, out = run(link, *common, "bench-status")
    check("6 s without a frame -> back to SENSORS_ONLY", "level SENSORS_ONLY" in out, out)
    code, out = run(link, *common, "feeder-jog", "0", "0", "60", answer="no")
    check("feeder-jog cancelled without the typed confirmation", code != 0 and "cancelled" in out, out)
    code, out = run(link, *common, "bench-status")
    check("a cancelled jog leaves the box unarmed", "level SENSORS_ONLY" in out, out)
    code, out = run(link, *common, "feeder-jog", "0", "0", "60", answer="FEEDER-JOG")
    check("feeder-jog 60 ms with confirmation: done", "result done" in out, out)
    code, out = run(link, *common, "hub-jog", "0", "32", "200", answer="HUB-JOG")
    check("hub-jog 200 ms with confirmation: done", "result done" in out, out)
    code, out = run(link, *common, "bench-disarm")
    check("bench-disarm -> SENSORS_ONLY", "level SENSORS_ONLY" in out, out)
    check("no fault in the box during the session", not link.b.faults and not link.b.problems,
          "; ".join(link.b.problems))

    link = EmuLink(inert)
    run(link, *common, "enumerate")
    code, out = run(link, *common, "identity")
    check("INERT identity", "variant INERT" in out, out)
    code, out = run(link, *common, "bench-status")
    check("bench commands refused on INERT", code != 0 and "not an OpenCFS BENCH build" in out, out)

    for name, ok, detail in results:
        print("%-58s %s %s" % (name, "ok  " if ok else "FAIL", "" if ok else detail.strip()[:200]))
    failed = sum(not ok for _, ok, _ in results)
    print("%d cfs_diag checks, %d failed" % (len(results), failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
