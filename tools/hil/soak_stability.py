#!/usr/bin/env python3
"""RTSP Mic soak: ping + HTTP + /status + serial Guru/LoadProhibited.

Usage:
  RTSPMIC_HIL_PASS='secret' python3 tools/hil/soak_stability.py \\
    --base http://192.168.8.106 \\
    --serial /dev/ttyACM0 \\
    --minutes 30 \\
    --user admin

Expect VLC (rtsp://IP:554/...) + WebUI open during soak.
Exit 0 if no Guru and network stayed healthy; 1 otherwise.
"""
from __future__ import annotations

import argparse
import base64
import json
import os
import re
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path


GURU_RE = re.compile(
    r"Guru Meditation|LoadProhibited|Stack canary|Abort\(\)|"
    r"Task watchdog got triggered|Interrupt wdt|Guru Meditation Error",
    re.I,
)
HEAP_SKIP_RE = re.compile(r"skip telemetry.*low heap=(\d+)", re.I)
PCB_RE = re.compile(r"pcb is NULL", re.I)
LED_RE = re.compile(r"\[LED\].*", re.I)
DIAG_RE = re.compile(r"\[DIAG\].*", re.I)

# Keep with this script (not world-writable /tmp) to avoid TOCTOU/race on spawn.
_LOGGER_SCRIPT = Path(__file__).resolve().parent / "_rtspmic_serial_logger.py"
_LOGGER_SRC = """\
import sys, time
try:
    import serial
except ImportError:
    sys.stderr.write("pyserial required\\n")
    sys.exit(2)
p, o = sys.argv[1], sys.argv[2]
s = serial.Serial()
s.port = p
s.baudrate = 115200
s.timeout = 0.5
s.dsrdtr = False
s.rtscts = False
s.open()
try:
    s.setDTR(False)
    s.setRTS(False)
except Exception:
    pass
with open(o, "ab", buffering=0) as f:
    while True:
        d = s.read(4096)
        if d:
            f.write(d)
        else:
            time.sleep(0.05)
"""


@dataclass
class Stats:
    ticks: int = 0
    ping_fail: int = 0
    http_fail: int = 0
    status_fail: int = 0
    guru: int = 0
    pcb: int = 0
    heap_skip: int = 0
    heap_min: int | None = None
    block_min: int | None = None
    rtsp_max: int = 0
    samples: list[dict] = field(default_factory=list)


def ping_ok(host: str, timeout_s: float = 2.0) -> bool:
    try:
        r = subprocess.run(
            ["ping", "-c", "1", "-W", str(int(timeout_s)), host],
            capture_output=True,
            timeout=timeout_s + 1,
        )
        return r.returncode == 0
    except Exception:
        return False


def http_get(url: str, auth: str | None, timeout: float = 5.0) -> tuple[int, bytes]:
    req = urllib.request.Request(url)
    if auth:
        req.add_header("Authorization", "Basic " + auth)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read() if e.fp else b""
    except Exception:
        return 0, b""


def ensure_logger_script() -> Path:
    _LOGGER_SCRIPT.write_text(_LOGGER_SRC, encoding="utf-8")
    return _LOGGER_SCRIPT


def scan_serial_text(text: str, stats: Stats, sample: dict) -> None:
    for line in text.splitlines():
        if GURU_RE.search(line):
            stats.guru += 1
            sample.setdefault("alerts", []).append(line[:200])
            print(f"ALERT guru: {line}", flush=True)
        if PCB_RE.search(line):
            stats.pcb += 1
        if HEAP_SKIP_RE.search(line):
            stats.heap_skip += 1
        if LED_RE.search(line) or DIAG_RE.search(line):
            print(line, flush=True)


def main() -> int:
    ap = argparse.ArgumentParser(description="RTSP Mic stability soak")
    ap.add_argument("--base", default=os.environ.get("RTSPMIC_HIL_BASE", "http://192.168.8.106"))
    ap.add_argument("--serial", default=os.environ.get("RTSPMIC_HIL_SERIAL", "/dev/ttyACM0"))
    ap.add_argument("--minutes", type=float, default=15.0)
    ap.add_argument("--interval", type=float, default=15.0, help="health tick seconds")
    ap.add_argument("--user", default=os.environ.get("RTSPMIC_HIL_USER", "admin"))
    ap.add_argument(
        "--password",
        default=os.environ.get("RTSPMIC_HIL_PASS"),
        help="Required (or set RTSPMIC_HIL_PASS). No hardcoded default.",
    )
    ap.add_argument("--log", default="/tmp/rtspmic_soak_stability.jsonl")
    ap.add_argument("--serial-log", default="/tmp/rtspmic_soak_serial_live.log")
    args = ap.parse_args()

    if not args.password:
        print("[soak] ERROR: --password or RTSPMIC_HIL_PASS required", file=sys.stderr)
        return 2

    host = args.base.split("//", 1)[-1].split("/", 1)[0].split(":")[0]
    auth = base64.b64encode(f"{args.user}:{args.password}".encode()).decode()
    stats = Stats()
    stop = {"v": False}

    def _sig(_s, _f):
        stop["v"] = True

    signal.signal(signal.SIGINT, _sig)
    signal.signal(signal.SIGTERM, _sig)

    ser_proc = None
    serial_path = Path(args.serial)
    if serial_path.exists():
        logger = ensure_logger_script()
        ser_proc = subprocess.Popen(
            [sys.executable, str(logger), str(serial_path), args.serial_log],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        time.sleep(0.3)
        if ser_proc.poll() is not None:
            err = (ser_proc.stderr.read() if ser_proc.stderr else b"").decode(
                "utf-8", "replace"
            )
            print(f"[soak] WARN: serial logger exited early: {err.strip() or ser_proc.returncode}")
            ser_proc = None
        else:
            print(f"[soak] serial logger pid={ser_proc.pid} → {args.serial_log}")
    else:
        print(f"[soak] WARN: no serial {serial_path}")

    deadline = time.time() + args.minutes * 60.0
    ser_pos = 0
    ser_carry = ""
    slog = Path(args.serial_log)
    if slog.exists():
        ser_pos = slog.stat().st_size

    print(
        f"[soak] base={args.base} host={host} minutes={args.minutes} "
        f"interval={args.interval}s — open VLC+Web now"
    )
    out = open(args.log, "a", encoding="utf-8")

    try:
        while time.time() < deadline and not stop["v"]:
            stats.ticks += 1
            t0 = time.time()
            sample: dict = {"t": t0, "tick": stats.ticks}

            ok_ping = ping_ok(host)
            if not ok_ping:
                stats.ping_fail += 1
            sample["ping"] = ok_ping

            code, _body = http_get(args.base.rstrip("/") + "/", None, timeout=5)
            sample["http"] = code
            if code != 200:
                stats.http_fail += 1

            # Firmware has /status (Basic), not /api/diag.
            scode, sbody = http_get(args.base.rstrip("/") + "/status", auth, timeout=5)
            sample["status_http"] = scode
            if scode == 200:
                try:
                    d = json.loads(sbody.decode("utf-8", "replace"))
                    sample["status"] = {
                        k: d.get(k)
                        for k in (
                            "free_heap",
                            "free_heap_block",
                            "min_free_heap",
                            "rtsp_clients",
                            "uptime_s",
                            "schema",
                        )
                        if k in d or True
                    }
                    # Prefer nested system fields when present (telemetry.v2).
                    sysobj = d.get("system") if isinstance(d.get("system"), dict) else {}
                    fh = int(sysobj.get("free_heap") or d.get("free_heap") or 0)
                    fb = int(
                        sysobj.get("free_heap_block")
                        or d.get("free_heap_block")
                        or 0
                    )
                    if fh and (stats.heap_min is None or fh < stats.heap_min):
                        stats.heap_min = fh
                    if fb and (stats.block_min is None or fb < stats.block_min):
                        stats.block_min = fb
                    rtsp = int(
                        d.get("rtsp_clients")
                        or (d.get("rtsp") or {}).get("clients")
                        or 0
                    )
                    stats.rtsp_max = max(stats.rtsp_max, rtsp)
                except Exception as e:
                    sample["status_err"] = str(e)
                    stats.status_fail += 1
            else:
                stats.status_fail += 1
                sample["status_body"] = sbody[:200].decode("utf-8", "replace")

            # serial tail — carry incomplete last line across chunks
            if slog.exists():
                size = slog.stat().st_size
                if size < ser_pos:
                    ser_pos = 0
                    ser_carry = ""
                if size > ser_pos:
                    with open(slog, "rb") as f:
                        f.seek(ser_pos)
                        chunk = f.read().decode("utf-8", "replace")
                        ser_pos = f.tell()
                    text = ser_carry + chunk
                    if text.endswith("\n"):
                        complete, ser_carry = text, ""
                    else:
                        idx = text.rfind("\n")
                        if idx < 0:
                            ser_carry = text
                            complete = ""
                        else:
                            complete, ser_carry = text[: idx + 1], text[idx + 1 :]
                    if complete:
                        scan_serial_text(complete, stats, sample)

            stats.samples.append(sample)
            out.write(json.dumps(sample, ensure_ascii=False) + "\n")
            out.flush()

            print(
                f"[tick {stats.ticks}] ping={ok_ping} http={code} status={scode} "
                f"heap_min={stats.heap_min} block_min={stats.block_min} "
                f"rtsp_max={stats.rtsp_max} "
                f"guru={stats.guru} pcb={stats.pcb} ping_fail={stats.ping_fail}",
                flush=True,
            )

            elapsed = time.time() - t0
            time.sleep(max(0.5, args.interval - elapsed))
    finally:
        out.close()
        if ser_proc and ser_proc.poll() is None:
            ser_proc.terminate()
            try:
                ser_proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                ser_proc.kill()

    summary = {
        "ticks": stats.ticks,
        "ping_fail": stats.ping_fail,
        "http_fail": stats.http_fail,
        "status_fail": stats.status_fail,
        "guru": stats.guru,
        "pcb": stats.pcb,
        "heap_skip": stats.heap_skip,
        "heap_min": stats.heap_min,
        "block_min": stats.block_min,
        "rtsp_max": stats.rtsp_max,
    }
    print("[soak] SUMMARY", json.dumps(summary), flush=True)
    Path("/tmp/rtspmic_soak_summary.json").write_text(
        json.dumps(summary, indent=2), encoding="utf-8"
    )

    fail = stats.guru > 0
    if stats.ticks > 0:
        if stats.ping_fail / stats.ticks > 0.1:
            fail = True
        if stats.http_fail / stats.ticks > 0.1:
            fail = True
        if stats.status_fail / stats.ticks > 0.1:
            fail = True
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
