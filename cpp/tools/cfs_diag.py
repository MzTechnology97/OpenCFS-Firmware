#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""OpenCFS diagnostic client: talk to one CFS over the K2's RS-485 link.

Run it on the host (CM5) with Klipper STOPPED, so that this tool is the only
owner of the bus. It speaks the box protocol directly (docs/spec/03):

  enumerate              A1 discovery, then A0 assigns --addr (default 1)
  identity               0x7A build identity (OpenCFS only)
  version                0x14 version text and serial number
  state                  0x0A box state (temperature, humidity, state, hub mask)
  runtime                0x57 runtime-config INFO v2
  tag SLOT               0x60 last tag readout of a slot (OpenCFS only)
  bench-status           0x7B 00   BENCH level, last result, seconds armed
  bench-tag SLOT         0x7B 20   read the tag in place (no motion), then 0x60
  bench-arm              0x7B 01   ARM with the box's unique ID (ACTUATOR_TEST)
  bench-disarm           0x7B 02
  feeder-jog SLOT DIR MS 0x7B 10   one feeder, DIR 0 fwd / 1 rev, MS 1..500
  hub-jog DIR SPEED MS   0x7B 11   hub, SPEED 1..64, MS 1..1000
  fault-test SLOT        0x7B 30   that feeder runs 50 ms, then a real CPU fault

Commands that can move a motor (feeder-jog, hub-jog, fault-test) run only on a
box whose 0x7A identity says BENCH, and only after typing the command name to
confirm. They then ARM the box themselves and send the command at once: the box
falls back to SENSORS_ONLY after 5 s without a frame, so a separate bench-arm
would expire while the confirmation is typed. Nothing here writes to a tag or
to the CFS flash.

usage: cfs_diag.py [--port /dev/k2-rs485] [--addr 1] COMMAND [ARGS]
"""
import argparse
import os
import select
import subprocess
import sys
import time

BAUD = 230400
VARIANTS = {1: "RELEASE", 2: "INERT", 3: "BENCH", 4: "BUS-RELEASE", 5: "BUS-LED"}
LEVELS = {0: "not a bench build", 1: "SENSORS_ONLY", 2: "ACTUATOR_TEST"}
STATES = {0: "IDLE", 1: "PRELOAD", 2: "PRINT", 3: "RELOAD", 4: "ERROR", 5: "TEST"}
JOG_RESULTS = {0: "done", 1: "stalled", 2: "overload", 3: "travel limit"}
MOTION = ("feeder-jog", "hub-jog", "fault-test")


def crc8(data):
    c = 0
    for b in data:
        c ^= b
        for _ in range(8):
            c = ((c << 1) ^ 0x07) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c


def frame(addr, header, cmd, payload=b""):
    body = bytes([len(payload) + 3, header, cmd]) + bytes(payload)
    return bytes([0xF7, addr]) + body + bytes([crc8(body)])


def split_frames(buf):
    """Valid frames in a byte stream (CRC-checked), in order."""
    out, i = [], 0
    while i + 6 <= len(buf):
        if buf[i] != 0xF7:
            i += 1
            continue
        n = buf[i + 2]
        f = buf[i:i + 3 + n]
        if n < 3 or len(f) < 3 + n:
            i += 1
            continue
        if crc8(f[2:-1]) == f[-1]:
            out.append(bytes(f))
            i += 3 + n
        else:
            i += 1
    return out


class SerialLink:
    """Raw 8N1 serial port on Linux (termios), no extra Python package needed."""

    def __init__(self, port, baud=BAUD):
        import termios
        import tty
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        tty.setraw(self.fd)
        attrs = termios.tcgetattr(self.fd)
        speed = getattr(termios, "B%d" % baud)
        attrs[4] = attrs[5] = speed
        attrs[2] &= ~(termios.PARENB | termios.CSTOPB | termios.CSIZE)
        attrs[2] |= termios.CS8 | termios.CLOCAL | termios.CREAD
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        termios.tcflush(self.fd, termios.TCIOFLUSH)

    def write(self, data):
        os.write(self.fd, data)

    def read(self, seconds):
        end, buf = time.monotonic() + seconds, b""
        while True:
            left = end - time.monotonic()
            if left <= 0:
                return buf
            r, _, _ = select.select([self.fd], [], [], left)
            if r:
                try:
                    buf += os.read(self.fd, 512)
                except BlockingIOError:
                    pass


class Box:
    def __init__(self, link, addr):
        self.link, self.addr = link, addr

    def exchange(self, addr, header, cmd, payload=b"", wait=0.5, many=False):
        req = frame(addr, header, cmd, payload)
        self.link.write(req)
        got = []
        for f in split_frames(self.link.read(wait)):
            if f == req:
                continue  # our own transmission heard back on the bus
            if f[4] == cmd:
                got.append(f)
        if many:
            return got
        return got[-1] if got else None

    def request(self, cmd, payload=b"", wait=0.5):
        f = self.exchange(self.addr, 0xFF, cmd, payload, wait)
        if f is None:
            raise SystemExit("no reply to 0x%02X from address %d" % (cmd, self.addr))
        return f[3], f[5:-1]

    def discover(self):
        replies = self.exchange(0xFE, 0x00, 0xA1, b"\xFE\xFE", wait=1.5, many=True)
        return [f[7:19] for f in replies if len(f) >= 20]

    def assign(self, uid):
        replies = self.exchange(0xFE, 0x00, 0xA0, bytes([self.addr]) + uid, wait=0.6, many=True)
        return [f for f in replies if f[1] == self.addr]

    def identity(self):
        st, p = self.request(0x7A)
        if st != 0 or len(p) != 32 or p[:4] != b"OCFS":
            return None
        return {"variant": p[5], "level": p[6], "flags": p[7],
                "revision": p[8:20].rstrip(b"\0").decode("ascii", "replace"),
                "app": p[20:32].decode("ascii", "replace")}


def klipper_active():
    try:
        r = subprocess.run(["systemctl", "is-active", "klipper"], capture_output=True, text=True, timeout=5)
        return r.stdout.strip() == "active"
    except (OSError, subprocess.SubprocessError):
        return False


def show_tag(p):
    if len(p) < 49:
        print("  readout too short: %s" % p.hex())
        return
    uid = p[4:4 + p[3]].hex(" ")
    material = p[17:33].split(b"\0")[0].decode("ascii", "replace")
    u16 = lambda o: p[o] | p[o + 1] << 8
    print("  slot %d decoder %d flags 0x%02x uid %s atqa %02x%02x sak %02x" % (p[0], p[1], p[2], uid, p[14], p[15], p[16]))
    print("  material %r colour #%02x%02x%02x alpha %02x" % (material, p[33], p[34], p[35], p[36]))
    print("  weight %d g, length %d m, diameter %.2f mm, nozzle %d..%d C, vendor hint %d, raw blocks %d"
          % (u16(37), u16(39), u16(41) / 100.0, u16(43), u16(45), p[47], p[48]))


def confirm(word):
    print("This command can move a motor. The operator must be at the CFS, ready to cut power.")
    if input("Type %s to continue: " % word).strip() != word:
        raise SystemExit("cancelled")


def load_uid(a):
    text = a.uid
    if text is None and os.path.exists(a.uid_file):
        text = open(a.uid_file).read()
    if not text:
        raise SystemExit("unique ID unknown: run 'enumerate' first or pass --uid")
    uid = bytes.fromhex(text.replace(" ", "").strip())
    if len(uid) != 12:
        raise SystemExit("the unique ID must be 12 bytes")
    return uid


def arm(box, a):
    st, p = box.request(0x7B, b"\x01ARM" + load_uid(a)[8:12])
    return st, (p[0] if p else None)


def main(argv=None, link=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", default="/dev/k2-rs485")
    ap.add_argument("--addr", type=int, default=1)
    ap.add_argument("--no-klipper-check", action="store_true", help="skip the Klipper-stopped check")
    ap.add_argument("--uid", help="the box's 12-byte unique ID in hex, for bench-arm "
                    "(default: the one saved by the last 'enumerate')")
    ap.add_argument("--uid-file", default=os.path.expanduser("~/.cfs_diag_uid"))
    ap.add_argument("command")
    ap.add_argument("args", nargs="*", type=int)
    a = ap.parse_args(argv)
    if link is None:
        if not a.no_klipper_check and klipper_active():
            raise SystemExit("Klipper is running: stop it first (sudo systemctl stop klipper); "
                             "this tool must be the only owner of the bus")
        link = SerialLink(a.port)
    box = Box(link, a.addr)
    c, args = a.command, a.args

    if c == "enumerate":
        uids = box.discover()
        if not uids:
            raise SystemExit("no unassigned CFS answered A1 (already addressed? try 'version')")
        for u in uids:
            print("found uid %s" % u.hex(" "))
        with open(a.uid_file, "w") as f:
            f.write(uids[0].hex() + "\n")
        replies = box.assign(uids[0])
        print("A0 to address %d: %d replies (3 expected)" % (a.addr, len(replies)))
    elif c == "identity":
        ident = box.identity()
        if ident is None:
            print("no OpenCFS identity (stock or K2-OpenHost firmware answers 0x7A as unknown)")
        else:
            print("variant %s, bench level %s, flags 0x%02x, revision %s, version %s"
                  % (VARIANTS.get(ident["variant"], ident["variant"]), LEVELS.get(ident["level"], ident["level"]),
                     ident["flags"], ident["revision"], ident["app"]))
    elif c == "version":
        st, p = box.request(0x14)
        print("status %d: %s" % (st, p.decode("ascii", "replace")))
    elif c == "state":
        st, p = box.request(0x0A)
        if len(p) == 6:
            print("status 0x%02x, %d C, %d %%, state %s, event 0x%02x, hub mask 0x%x, rfid mask 0x%x"
                  % (st, p[0], p[1], STATES.get(p[3], p[3]), p[2], p[4], p[5]))
        else:
            print("status 0x%02x, payload %s (slot events)" % (st, p.hex(" ")))
    elif c == "runtime":
        st, p = box.request(0x57, b"\x10")
        if st == 0 and len(p) >= 4:
            print("runtime config API v%d, %d parameters, descriptor rev %d, features 0x%02X" % (p[0], p[1], p[2], p[3]))
        else:
            print("status %d: runtime config not supported" % st)
    elif c == "tag":
        st, p = box.request(0x60, bytes([args[0]]))
        print("status %d" % st) if st else show_tag(p)
    elif c.startswith("bench") or c in MOTION:
        ident = box.identity()
        if ident is None or ident["variant"] != 3:
            raise SystemExit("not an OpenCFS BENCH build: refusing bench commands")
        if c == "bench-status":
            st, p = box.request(0x7B, b"\x00")
            print("level %s, last result %s, %d s left armed"
                  % (LEVELS.get(p[0], p[0]), JOG_RESULTS.get(p[1], p[1]), p[2]))
        elif c == "bench-tag":
            st, p = box.request(0x7B, bytes([0x20, args[0]]), wait=1.5)
            print("tag read: status %d, found 0x%02x, uid %s" % (st, p[0], p[2:2 + p[1]].hex(" ")))
            if st == 0 and p[0] & 2:
                st, p = box.request(0x60, bytes([args[0]]))
                print("status %d" % st) if st else show_tag(p)
        elif c == "bench-disarm":
            st, p = box.request(0x7B, b"\x02")
            print("status %d, level %s" % (st, LEVELS.get(p[0], p[0]) if p else "?"))
        elif c == "bench-arm":
            st, level = arm(box, a)
            print("status %d, level %s" % (st, LEVELS.get(level, level)))
        else:
            load_uid(a)  # fail before the confirmation, not after
            confirm(c.upper())
            st, level = arm(box, a)
            if st != 0 or level != 2:
                raise SystemExit("ARM refused (status %d, level %s): nothing sent" % (st, LEVELS.get(level, level)))
            if c == "feeder-jog":
                slot, d, ms = args
                st, p = box.request(0x7B, bytes([0x10, slot, d, ms & 0xFF, ms >> 8]), wait=ms / 1000 + 1.0)
                if st == 0:
                    print("result %s, %d ms, feeder %d mV, odometer %+.1f mm"
                          % (JOG_RESULTS.get(p[0], p[0]), p[1] | p[2] << 8, p[3] | p[4] << 8,
                             int.from_bytes(p[5:7], "little", signed=True) / 10))
                else:
                    print("refused, status %d" % st)
            elif c == "hub-jog":
                d, speed, ms = args
                st, p = box.request(0x7B, bytes([0x11, d, speed, ms & 0xFF, ms >> 8]), wait=ms / 1000 + 1.0)
                if st == 0:
                    print("result %s, %d ms, %d FG pulses, %.1f rps, odometer %+.1f mm"
                          % (JOG_RESULTS.get(p[0], p[0]), p[1] | p[2] << 8, p[3] | p[4] << 8,
                             (p[5] | p[6] << 8) / 10, int.from_bytes(p[7:9], "little", signed=True) / 10))
                else:
                    print("refused, status %d" % st)
            else:
                st, p = box.request(0x7B, bytes([0x30, args[0]]))
                print("status %d: the box should now be stopped with every output braked" % st)
    else:
        raise SystemExit("unknown command %r" % c)
    return 0


if __name__ == "__main__":
    sys.exit(main())
