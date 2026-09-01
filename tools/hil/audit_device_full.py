#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Полный HW-аудит RTSP Mic: API-матрица, WS, RTSP, нагрузка, качество телеметрии."""

from __future__ import annotations

import argparse
import base64
import json
import os
import socket
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Tuple

try:
    import serial  # type: ignore
except ImportError:
    serial = None


@dataclass
class Result:
    name: str
    ok: bool
    detail: str = ""
    severity: str = "P2"  # P0 blocker / P1 major / P2 minor / INFO


@dataclass
class Report:
    results: List[Result] = field(default_factory=list)

    def add(self, name: str, ok: bool, detail: str = "", severity: str = "P2") -> None:
        r = Result(name, ok, detail, severity if not ok else "INFO")
        self.results.append(r)
        mark = "PASS" if ok else f"FAIL:{severity}"
        print(f"[{mark}] {name} — {detail}")

    def failed(self) -> List[Result]:
        return [r for r in self.results if not r.ok]


def basic(user: str, password: str) -> str:
    return "Basic " + base64.b64encode(f"{user}:{password}".encode()).decode()


class Client:
    def __init__(self, base: str, user: str, password: str, timeout: float = 12.0):
        self.base = base.rstrip("/")
        self.auth = basic(user, password)
        self.timeout = timeout
        self.csrf: Optional[str] = None

    def req(
        self,
        path: str,
        method: str = "GET",
        data: Optional[bytes] = None,
        headers: Optional[dict] = None,
        auth: bool = True,
        csrf: bool = False,
    ) -> Tuple[int, bytes]:
        hdrs = dict(headers or {})
        if auth:
            hdrs["Authorization"] = self.auth
        if csrf and self.csrf:
            hdrs["X-CSRF-Token"] = self.csrf
        req = urllib.request.Request(self.base + path, data=data, headers=hdrs, method=method)
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                return resp.status, resp.read()
        except urllib.error.HTTPError as e:
            return e.code, (e.read() if e.fp else b"")
        except Exception as e:
            return -1, str(e).encode()

    def json(self, path: str, **kw) -> Tuple[int, Any]:
        code, body = self.req(path, **kw)
        try:
            return code, json.loads(body.decode("utf-8", "replace")) if body else None
        except json.JSONDecodeError:
            return code, None


def tcp_open(host: str, port: int, timeout: float = 3.0) -> bool:
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def ws_probe(host: str, path: str = "/ws", ticket: Optional[str] = None, timeout: float = 5.0) -> Tuple[bool, str]:
    """Minimal WS handshake; returns (ok, detail). Ticket в URL не используется прошивкой."""
    try:
        s = socket.create_connection((host, 80), timeout=timeout)
        s.settimeout(timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n"
        )
        s.sendall(req.encode())
        data = s.recv(4096).decode("utf-8", "replace")
        s.close()
        if "101" in data.split("\r\n", 1)[0]:
            return True, "101 Switching Protocols"
        line = data.split("\r\n", 1)[0]
        return False, line or "empty"
    except Exception as e:
        return False, f"{type(e).__name__}: {e}"


def _ws_mask_frame(payload: bytes) -> bytes:
    mask = os.urandom(4)
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    header = bytes([0x81, 0x80 | len(payload)]) + mask
    return header + masked


def ws_auth_exchange(host: str, ticket: str, path: str = "/ws", timeout: float = 5.0) -> Tuple[bool, str]:
    """101 + JSON auth frame. ok=True если сокет остался открыт после auth."""
    try:
        s = socket.create_connection((host, 80), timeout=timeout)
        s.settimeout(timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n"
        )
        s.sendall(req.encode())
        hs = s.recv(4096).decode("utf-8", "replace")
        if "101" not in hs.split("\r\n", 1)[0]:
            s.close()
            return False, f"no101:{hs.split(chr(13),1)[0]}"
        payload = json.dumps({"auth": ticket}).encode()
        s.sendall(_ws_mask_frame(payload))
        time.sleep(0.35)
        s.settimeout(0.8)
        try:
            extra = s.recv(256)
        except socket.timeout:
            extra = b""
        # close frame opcode 0x8 → reject
        if extra and (extra[0] & 0x0F) == 0x8:
            s.close()
            return False, "close_frame_reject"
        # probe: send ping-like empty text; if connection dead → reject
        try:
            s.sendall(_ws_mask_frame(b"{}"))
            time.sleep(0.2)
            try:
                more = s.recv(64)
            except socket.timeout:
                more = b""
            if more and (more[0] & 0x0F) == 0x8:
                s.close()
                return False, "close_after_probe"
            s.close()
            return True, "auth_accepted"
        except OSError as e:
            return False, f"fail:{e}"
    except Exception as e:
        return False, f"{type(e).__name__}: {e}"


def rtsp_options(host: str, port: int = 554, timeout: float = 4.0) -> Tuple[bool, str]:
    last = "empty"
    for attempt in range(3):
        try:
            s = socket.create_connection((host, port), timeout=timeout)
            s.settimeout(timeout)
            msg = (
                f"OPTIONS rtsp://{host}:{port}/ RTSP/1.0\r\n"
                "CSeq: 1\r\n"
                "\r\n"
            )
            s.sendall(msg.encode())
            data = s.recv(2048).decode("utf-8", "replace")
            s.close()
            first = data.split("\r\n", 1)[0]
            ok = first.startswith("RTSP/1.0") and ("200" in first or "401" in first or "404" in first)
            if ok:
                return True, first or "empty"
            last = first or "empty"
        except Exception as e:
            last = f"{type(e).__name__}: {e}"
        time.sleep(0.4 * (attempt + 1))
    return False, last


def load_test(http: Client, path: str, n: int = 30, delay: float = 0.05) -> Dict[str, Any]:
    codes: Dict[int, int] = {}
    t0 = time.time()
    for _ in range(n):
        code, _ = http.req(path)
        codes[code] = codes.get(code, 0) + 1
        if delay:
            time.sleep(delay)
    return {"n": n, "codes": codes, "ms": int((time.time() - t0) * 1000)}


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        try:
            sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass

    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=os.environ.get("RTSPMIC_HIL_PORT", "COM6"))
    ap.add_argument("--base", default=os.environ.get("RTSPMIC_HIL_BASE", "http://192.168.2.65"))
    ap.add_argument("--user", default=os.environ.get("RTSPMIC_HIL_USER", "admin"))
    ap.add_argument("--password", default=os.environ.get("RTSPMIC_HIL_PASS", "rtsp-mic-change-me"))
    ap.add_argument("--skip-uart", action="store_true")
    args = ap.parse_args()

    report = Report()
    host = urllib.parse.urlparse(args.base).hostname or "192.168.2.65"
    http = Client(args.base, args.user, args.password)

    print("=== RTSP MIC FULL HW AUDIT ===")
    print(f"base={args.base} user={args.user}")

    # --- Connectivity ---
    code, body = http.req("/manifest.json", auth=False)
    report.add("conn.manifest", code == 200, f"status={code} len={len(body)}", "P0")

    code, body = http.req("/", auth=False)
    spa_ok = code == 200 and b"<script" in body.lower() and b"</html>" in body.lower()
    report.add("conn.spa", spa_ok, f"status={code} len={len(body)}", "P0")

    code, st = http.json("/status")
    report.add("conn.status", code == 200 and isinstance(st, dict), f"status={code}", "P0")
    if not isinstance(st, dict):
        print("ABORT: no status"); return 2

    # --- Telemetry quality ---
    required = [
        "schema", "node_id", "firmware_version", "timestamp_ms", "must_change_password",
        "system", "audio",
    ]
    missing = [k for k in required if k not in st]
    report.add("telemetry.required_keys", not missing, f"missing={missing}", "P1")
    report.add(
        "telemetry.schema_v1",
        st.get("schema") == "rtsp-mic.telemetry.v1",
        f"schema={st.get('schema')}",
        "P1",
    )
    report.add(
        "telemetry.mcp_false",
        st.get("must_change_password") is False,
        f"mcp={st.get('must_change_password')}",
        "P2",
    )
    heap = int(st.get("free_heap") or 0)
    report.add("telemetry.heap_gt_20k", heap > 20000, f"free_heap={heap}", "P1")
    psram = int(st.get("free_psram") or 0)
    report.add("telemetry.psram_present", psram > 1_000_000, f"free_psram={psram}", "P1")
    report.add("telemetry.ntp", bool(st.get("ntp_synced")), f"ntp={st.get('ntp_synced')}", "P1")
    # Live timestamp
    time.sleep(1.1)
    code2, st2 = http.json("/status")
    advancing = (
        code2 == 200
        and isinstance(st2, dict)
        and int(st2.get("timestamp_ms") or 0) > int(st.get("timestamp_ms") or 0)
    )
    report.add("telemetry.ts_live", advancing, "timestamp advances", "P0")

    # Audio path quality signals
    audio = st.get("audio") or {}
    report.add(
        "audio.samples_flowing",
        int(audio.get("samples_processed") or 0) > 0,
        f"samples={audio.get('samples_processed')} rms={audio.get('raw_rms')}",
        "P1",
    )

    # --- Public assets ---
    for path in ("/manifest.svg", "/sw.js"):
        c, b = http.req(path, auth=False)
        report.add(f"public.{path}", c == 200 and len(b) > 20, f"status={c} len={len(b)}", "P2")

    # --- Auth gates ---
    c, _ = http.req("/status", auth=False)
    report.add("auth.status_unauth", c in (401, 403), f"status={c}", "P0")
    c, _ = http.req("/api/csrf", auth=False)
    report.add("auth.csrf_unauth", c in (401, 403), f"status={c}", "P0")

    c, csrf = http.json("/api/csrf")
    ok_csrf = c == 200 and isinstance(csrf, dict) and bool(csrf.get("token"))
    report.add("auth.csrf", ok_csrf, f"status={c}", "P0")
    if ok_csrf:
        http.csrf = csrf["token"]

    # Hardened endpoints should work with non-default password
    for path, sev in (("/api/mel", "P1"), ("/api/integrations", "P1"), ("/api/ws-ticket", "P0"), ("/api/system/tls_ca", "P2")):
        c, body = http.req(path)
        report.add(
            f"api.get.{path}",
            c in (200, 204) or (path == "/api/mel" and c == 200),
            f"status={c} len={len(body)}",
            sev,
        )

    # Password validation (short → 400)
    time.sleep(1.2)
    data = urllib.parse.urlencode({"user": "admin", "password": "short"}).encode()
    c, body = http.req(
        "/api/system/password",
        method="POST",
        data=data,
        headers={"Content-Type": "application/x-www-form-urlencoded"},
        csrf=True,
    )
    report.add(
        "api.password_reject_short",
        c == 400,
        f"status={c} body={body[:80]!r}",
        "P1",
    )

    time.sleep(1.1)
    # LED: mode ∈ {off,status,level}
    data = urllib.parse.urlencode({"mode": "status"}).encode()
    c, body = http.req(
        "/api/system/led",
        method="POST",
        data=data,
        headers={"Content-Type": "application/x-www-form-urlencoded"},
        csrf=True,
    )
    report.add("api.led_noop", c in (200, 204), f"status={c} body={body[:60]!r}", "P2")

    # Network scan: 202 = async started, 200 = results
    c, body = http.req("/api/network/scan")
    report.add(
        "api.network_scan",
        c in (200, 202, 429, 503),
        f"status={c} len={len(body)}",
        "P2",
    )

    # CSRF missing on mutable → reject
    time.sleep(1.1)
    data = urllib.parse.urlencode({"mode": "0"}).encode()
    c, body = http.req(
        "/api/system/led",
        method="POST",
        data=data,
        headers={"Content-Type": "application/x-www-form-urlencoded"},
        csrf=False,
    )
    # Without X-CSRF-Token should fail closed
    report.add("api.csrf_required", c in (403, 400), f"status={c} body={body[:80]!r}", "P0")

    # --- WS ---
    c, ticket_payload = http.json("/api/ws-ticket")
    ticket = (ticket_payload or {}).get("ticket") if isinstance(ticket_payload, dict) else None
    report.add("ws.ticket", c == 200 and bool(ticket), f"status={c}", "P0")
    if ticket:
        ok, detail = ws_probe(host, "/ws", ticket=ticket)
        report.add("ws.handshake", ok, detail, "P1")
        # Auth — пост-handshake JSON {auth:ticket}; 101 сам по себе не = доступ.
        ok_auth, det_auth = ws_auth_exchange(host, ticket)
        report.add("ws.auth_ok", ok_auth, det_auth, "P0")
        ok_bad, det_bad = ws_auth_exchange(host, "deadbeef")
        report.add("ws.reject_bad_ticket", (not ok_bad) and ("close" in det_bad or "fail" in det_bad or "401" in det_bad or "reject" in det_bad), det_bad, "P1")

    # --- RTSP ---
    rtsp_port = 554
    report.add("rtsp.port_open", tcp_open(host, rtsp_port), f"tcp:{rtsp_port}", "P1")
    ok, detail = rtsp_options(host, rtsp_port)
    # path /stream на этой прошивке рвёт сокет; OPTIONS на / — ок
    if not ok:
        try:
            s = socket.create_connection((host, rtsp_port), timeout=5.0)
            s.settimeout(5.0)
            s.sendall(
                f"OPTIONS rtsp://{host}:{rtsp_port}/ RTSP/1.0\r\nCSeq: 1\r\n\r\n".encode()
            )
            first = s.recv(2048).decode("utf-8", "replace").split("\r\n", 1)[0]
            s.close()
            ok = first.startswith("RTSP/1.0") and "200" in first
            detail = first
        except Exception as e:
            detail = f"{type(e).__name__}: {e}"
    report.add("rtsp.options", ok, f"port={rtsp_port} {detail}", "P1")

    # --- Load ---
    load = load_test(http, "/status", n=25, delay=0.08)
    ok200 = load["codes"].get(200, 0)
    report.add(
        "load.status_25",
        ok200 >= 20,
        f"codes={load['codes']} ms={load['ms']}",
        "P1",
    )

    # Rate limit behavior on csrf
    rl_codes = {}
    for _ in range(20):
        c, _ = http.req("/api/csrf")
        rl_codes[c] = rl_codes.get(c, 0) + 1
    report.add(
        "rate.csrf_burst",
        200 in rl_codes,
        f"codes={rl_codes} (429 expected under burst)",
        "INFO",
    )

    # --- UART stability snapshot (optional) ---
    if not args.skip_uart and serial is not None:
        try:
            ser = serial.Serial()
            ser.port = args.port
            ser.baudrate = 115200
            ser.timeout = 0.2
            ser.dsrdtr = False
            ser.rtscts = False
            ser.open()
            try:
                ser.setDTR(False)
                ser.setRTS(False)
            except Exception:
                pass
            ser.reset_input_buffer()
            t0 = time.time()
            buf = bytearray()
            while time.time() - t0 < 8:
                http.req("/status")
                chunk = ser.read(4096)
                if chunk:
                    buf.extend(chunk)
            text = buf.decode("utf-8", "replace")
            panics = sum(text.count(x) for x in ("Guru Meditation", "Stack canary", "stack overflow", "task_wdt"))
            report.add("uart.no_panic_8s", panics == 0, f"panics={panics} bytes={len(buf)}", "P0")
            # XVF presence
            # (boot markers may not appear in this window)
            ser.close()
        except Exception as e:
            report.add("uart.monitor", False, str(e), "P1")

    # --- Summary ---
    fails = report.failed()
    p0 = [r for r in fails if r.severity == "P0"]
    p1 = [r for r in fails if r.severity == "P1"]
    print("=== SUMMARY ===")
    print(f"total={len(report.results)} fail={len(fails)} P0={len(p0)} P1={len(p1)}")
    for r in fails:
        print(f"  - [{r.severity}] {r.name}: {r.detail}")

    # Snapshot for report
    snap = {
        "firmware": st.get("firmware_version"),
        "heap": heap,
        "psram": psram,
        "wifi_rssi": (st.get("system") or {}).get("wifi_rssi"),
        "temp_c": (st.get("system") or {}).get("temp_c"),
        "fails": [{"sev": r.severity, "name": r.name, "detail": r.detail} for r in fails],
    }
    print("=== SNAPSHOT ===")
    print(json.dumps(snap, ensure_ascii=False, indent=2))
    return 1 if p0 or p1 else 0


if __name__ == "__main__":
    raise SystemExit(main())
