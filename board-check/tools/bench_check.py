#!/usr/bin/env python3
"""Drive the board check image over a serial port the way the web page does,
and print every line it sends back. For bench runs on boards nobody presses.

  bench_check.py PORT [--psram none|2MB|8MB] [--flash-mb N] [--usb native|bridge]
"""
import argparse
import json
import sys
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("--psram", default="")
    ap.add_argument("--flash-mb", type=int, default=0)
    ap.add_argument("--usb", default="")
    ap.add_argument("--watch", type=float, default=5.0, help="seconds to watch after the pictures")
    ap.add_argument("--dumps", help="save the screen's pictures (monochrome screens) to this JSON file")
    a = ap.parse_args()
    s = serial.Serial(a.port, 115200, timeout=0.1)
    s.dtr = False
    s.rts = False

    def send(obj):
        s.write((json.dumps(obj) + "\n").encode())

    def read_for(seconds, until=None):
        end = time.time() + seconds
        buf = b""
        while time.time() < end:
            buf += s.read(4096)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", "replace").rstrip("\r")
                print("%.2f %s" % (time.time() % 1000, text[:3000]), flush=True)
                try:
                    ev = json.loads(text)
                except ValueError:
                    continue
                if until and ev.get("event") == until:
                    return ev
        return None

    send({"cmd": "hello"})
    read_for(2, "hello")
    t0 = time.time()
    send({"cmd": "probe", "psram": a.psram, "flash_mb": a.flash_mb, "usb": a.usb})
    found = read_for(30, "found")
    print("probe took %.1f s" % (time.time() - t0))
    send({"cmd": "wireless"})
    read_for(15, "wireless")
    dumps = []
    send({"cmd": "show", "what": "x"})
    read_for(1.0)
    send({"cmd": "dump"})
    dumps.append(read_for(2, "dump"))
    send({"cmd": "show", "what": "arrow"})
    read_for(1.0)
    for i in range(8):
        send({"cmd": "dump"})
        dumps.append(read_for(2, "dump"))
        if i < 7:
            send({"cmd": "rotate"})
            read_for(0.8)
    if a.dumps:
        with open(a.dumps, "w") as f:
            json.dump([d for d in dumps if d], f)
    send({"cmd": "show", "what": "status"})
    read_for(a.watch)
    return 0 if found else 1


if __name__ == "__main__":
    sys.exit(main())
