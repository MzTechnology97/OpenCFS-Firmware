#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Conformance test: drive OpenCFS (simulated, through cfs_host_bridge) with the
real host code from Kalico (box_protocol.py, serial_485.py, box_cfs_runtime.py).

The Kalico files are loaded from a directory you point to (they are GPLv3 and
are not copied into this repository). Every reply OpenCFS produces must pass
Kalico's own envelope, CRC, length and payload checks.

Usage: kalico_conformance.py --bridge build/cfs_host_bridge --extras path/to/klippy/extras
"""
import argparse
import importlib.util
import subprocess
import sys
import types
from contextlib import contextmanager


def load_extras(path):
    pkg = types.ModuleType("extras")
    pkg.__path__ = [path]
    sys.modules["extras"] = pkg
    mods = {}
    for name in ("serial_485", "box_protocol", "box_cfs_runtime"):
        spec = importlib.util.spec_from_file_location("extras." + name, f"{path}/{name}.py")
        mod = importlib.util.module_from_spec(spec)
        sys.modules["extras." + name] = mod
        spec.loader.exec_module(mod)
        setattr(pkg, name, mod)
        mods[name] = mod
    return mods


class BridgeSerial:
    """The subset of Kalico's serial485 object that the drivers use."""

    def __init__(self, exe, crc8):
        self.proc = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.crc8 = crc8
        self.frames = 0

    def command(self, line):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        if line.startswith("frame"):
            return self.proc.stdout.readline().strip()
        return None

    def cmd_send_data_with_response(self, body, timeout):
        body = bytes(body)
        frame = bytes([0xF7]) + body + bytes([self.crc8(body[1:])])
        reply = self.command("frame %s %d" % (frame.hex(), int(timeout * 1000)))
        self.frames += 1
        if not reply.startswith("reply ") or reply == "reply -":
            return None
        return bytes.fromhex(reply[6:])

    @contextmanager
    def request_session(self):
        yield self

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=10)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bridge", required=True)
    ap.add_argument("--extras", required=True, help="Kalico klippy/extras directory")
    a = ap.parse_args()
    m = load_extras(a.extras)
    bp, rt = m["box_protocol"], m["box_cfs_runtime"]
    ser = BridgeSerial(a.bridge, m["serial_485"].crc8)
    results = []

    def check(name, fn):
        try:
            value = fn()
            results.append((name, True, value))
        except Exception as exc:  # any host-side rejection is a failure
            results.append((name, False, "%s: %s" % (type(exc).__name__, exc)))
            return None
        return value

    # Auto-addressing from power-up, exactly as Kalico enumerates boxes.
    auto = bp.AutoAddressClient(ser)
    found = check("auto.discover", lambda: auto.discover())
    uniid = getattr(found, "uniid", None) if found else None
    if uniid is not None:
        check("auto.assign(1)", lambda: auto.assign(uniid, 1))
    check("auto.query(1)", lambda: auto.query(1))

    box = bp.BoxDriver(ser, 1)
    ser.command("insert 0")
    ser.command("advance 300")
    check("query_box_state", box.query_box_state)
    check("query_slot_mask", box.query_slot_mask)
    check("query_hub_mask", box.query_hub_mask)
    check("query_buffer", box.query_buffer)
    check("query_encoder", box.query_encoder)
    check("query_rfid_records", lambda: box.query_rfid_records(0x0F))
    check("query_rfid_remaining", lambda: box.query_rfid_remaining(0x0F))
    check("set_rfid_insert_reading(False)", lambda: box.set_rfid_insert_reading(False))
    check("set_rfid_insert_reading(True)", lambda: box.set_rfid_insert_reading(True))
    check("force_rfid_read(slot 0)", lambda: box.force_rfid_read(0x01))

    # Runtime Config API v2 with the host's own driver and catalogue.
    drv = rt.CfsRuntimeDriver(ser, 1)
    info = check("config.info_v2", drv.info_v2)
    for spec in rt.PARAMETERS:
        desc = check("config.describe_v2(%d)" % spec.param_id, lambda s=spec: drv.describe_v2(s.param_id))
        if desc is not None:
            check("config.validate(%d)" % spec.param_id,
                  lambda s=spec, d=desc: rt.BoxCfsRuntime._validate_descriptor(s, d))
        check("config.get_v2(%d)" % spec.param_id, lambda s=spec: drv.get_v2(s.param_id))
    ser.command("advance 600")  # motion interlock: at rest for 500 ms
    check("config.set_v2(1, 120)", lambda: drv.set_v2(1, 120))
    check("config.get_v2(1) == 120", lambda: (_ for _ in ()).throw(AssertionError("value"))
          if drv.get_v2(1) != 120 else 120)
    check("config.reset_v2(all)", lambda: drv.reset_v2(0xFF))
    ser.close()

    failed = [r for r in results if not r[1]]
    for name, ok, value in results:
        if not ok or name.startswith(("auto", "config.info", "query_box")):
            print("%-34s %s %s" % (name, "ok  " if ok else "FAIL", value if not ok else ""))
    print("%d host checks, %d failed, %d frames" % (len(results), len(failed), ser.frames))
    if info:
        print("INFO:", info)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
