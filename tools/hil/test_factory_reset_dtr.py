#!/usr/bin/env python3
"""Prove USB DTR vs factory-reset: open ACM with DTR on/off, watch 12s for wipe."""
from __future__ import annotations

import argparse
import sys
import time

import serial


def read_for(ser: serial.Serial, seconds: float) -> str:
    deadline = time.time() + seconds
    buf = b""
    while time.time() < deadline:
        chunk = ser.read(256)
        if chunk:
            buf += chunk
            sys.stdout.buffer.write(chunk)
            sys.stdout.flush()
    return buf.decode("utf-8", errors="replace")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--dtr", choices=("on", "off"), required=True)
    ap.add_argument("--seconds", type=float, default=12.0)
    args = ap.parse_args()

    dtr = args.dtr == "on"
    ser = serial.Serial(args.port, 115200, timeout=0.2, dsrdtr=False, rtscts=False)
    ser.dtr = dtr
    ser.rts = False
    # pulse EN via RTS briefly only when dtr-off path (clean boot without download)
    if not dtr:
        ser.rts = True
        time.sleep(0.05)
        ser.rts = False
    time.sleep(0.3)
    print(f"\n=== TEST dtr={dtr} rts={ser.rts} for {args.seconds}s ===\n", flush=True)
    text = read_for(ser, args.seconds)
    ser.close()

    if "Factory reset" in text:
        print(f"\nRESULT FAIL: factory reset seen with dtr={dtr}", flush=True)
        return 1
    if "Captive portal" in text or "Found credentials" in text or "Initializing" in text:
        print(f"\nRESULT OK: no factory reset with dtr={dtr}", flush=True)
        return 0
    print("\nRESULT UNKNOWN: no boot markers", flush=True)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
