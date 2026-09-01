#!/usr/bin/env python3
"""RTSP OPTIONS burst/soak for QA (TcpBudget / accept path).

  python3 tools/hil/test_rtsp_options.py --host 192.168.8.106
"""
from __future__ import annotations

import argparse
import socket
import sys
import time
import urllib.request


def options_once(host: str, port: int, cseq: int, timeout: float = 2.0) -> bool:
    s = socket.create_connection((host, port), timeout)
    try:
        s.settimeout(timeout)
        s.send(
            f"OPTIONS rtsp://{host}:{port}/ RTSP/1.0\r\n"
            f"CSeq: {cseq}\r\n\r\n".encode()
        )
        data = s.recv(256)
        return data.startswith(b"RTSP/1.0 200")
    finally:
        s.close()


def burst(host: str, port: int, n: int, delay: float) -> tuple[int, int]:
    ok = err = 0
    for i in range(n):
        try:
            if options_once(host, port, i):
                ok += 1
            else:
                err += 1
        except OSError:
            err += 1
        if delay:
            time.sleep(delay)
    return ok, err


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.8.106")
    ap.add_argument("--port", type=int, default=554)
    ap.add_argument("--spa", action="store_true", help="wait for WebUI / first")
    args = ap.parse_args()

    if args.spa:
        for _ in range(30):
            try:
                r = urllib.request.urlopen(f"http://{args.host}/", timeout=3)
                if r.status == 200:
                    print(f"[PASS] spa len={len(r.read())}")
                    break
            except Exception as e:
                print(f"[wait] spa {e}")
                time.sleep(1)
        else:
            print("[FAIL] spa not ready")
            return 1

    cases = [
        ("burst40@50ms", 40, 0.05),
        ("burst20@200ms", 20, 0.2),
        ("burst10@0 (expect some fail, max 4 slots)", 10, 0.0),
    ]
    failed = 0
    for name, n, delay in cases:
        ok, err = burst(args.host, args.port, n, delay)
        # Zero-delay: allow failures (only 4 session slots).
        need = n if delay > 0 else max(1, n // 2)
        passed = ok >= need
        print(f"[{'PASS' if passed else 'FAIL'}] {name} ok={ok} err={err}")
        if not passed:
            failed += 1

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
