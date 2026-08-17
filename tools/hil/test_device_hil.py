#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
RTSP Mic — Hardware-in-the-Loop (HIL) tests.

Требования:
  - Плата на USB (UART) и в той же LAN, что хост.
  - Windows Python с pyserial (COM*) ИЛИ Linux с /dev/ttyACM*.

Запуск (из Windows Python, т.к. COM6):
  py -3 iot/tools/hil/test_device_hil.py --port COM6 --base http://192.168.2.65

Env overrides:
  RTSPMIC_HIL_PORT, RTSPMIC_HIL_BASE, RTSPMIC_HIL_USER, RTSPMIC_HIL_PASS
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass, field
from typing import Callable, List, Optional, Tuple

try:
    import serial  # type: ignore
except ImportError:
    serial = None


DEFAULT_USER = "admin"
DEFAULT_PASS = "rtsp-mic-change-me"


@dataclass
class Result:
    name: str
    ok: bool
    detail: str = ""
    ms: float = 0.0


@dataclass
class Report:
    results: List[Result] = field(default_factory=list)

    def add(self, r: Result) -> None:
        self.results.append(r)
        mark = "PASS" if r.ok else "FAIL"
        print(f"[{mark}] {r.name} ({r.ms:.0f}ms) {r.detail}")

    @property
    def failed(self) -> int:
        return sum(1 for r in self.results if not r.ok)

    @property
    def passed(self) -> int:
        return sum(1 for r in self.results if r.ok)


def _b64_basic(user: str, password: str) -> str:
    raw = f"{user}:{password}".encode("utf-8")
    return "Basic " + base64.b64encode(raw).decode("ascii")


class HttpClient:
    def __init__(self, base: str, user: str, password: str, timeout: float = 20.0):
        self.base = base.rstrip("/")
        self.auth = _b64_basic(user, password)
        self.timeout = timeout
        self.csrf: Optional[str] = None
        self.spa_timeout = 60.0

    def request(
        self,
        path: str,
        method: str = "GET",
        data: Optional[bytes] = None,
        headers: Optional[dict] = None,
        auth: bool = True,
    ) -> Tuple[int, bytes, dict]:
        url = self.base + path
        hdrs = dict(headers or {})
        if auth:
            hdrs["Authorization"] = self.auth
        if self.csrf and method in ("POST", "PUT", "DELETE", "PATCH"):
            hdrs.setdefault("X-CSRF-Token", self.csrf)
        req = urllib.request.Request(url, data=data, headers=hdrs, method=method)
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                return resp.status, resp.read(), dict(resp.headers.items())
        except urllib.error.HTTPError as e:
            body = e.read() if e.fp else b""
            return e.code, body, dict(e.headers.items()) if e.headers else {}

    def json(self, path: str, **kw):
        code, body, hdrs = self.request(path, **kw)
        try:
            payload = json.loads(body.decode("utf-8", "replace")) if body else None
        except json.JSONDecodeError:
            payload = None
        return code, payload, body, hdrs


class UartMonitor:
    def __init__(self, port: str, baud: int = 115200):
        if serial is None:
            raise RuntimeError("pyserial not installed")
        self.port = port
        self.baud = baud
        self._ser = None
        self.buf = bytearray()

    def open(self) -> None:
        # XIAO ESP32-S3 USB-JTAG: авто-DTR при open = chip reset.
        self._ser = serial.Serial()
        self._ser.port = self.port
        self._ser.baudrate = self.baud
        self._ser.timeout = 0.2
        self._ser.dsrdtr = False
        self._ser.rtscts = False
        self._ser.open()
        try:
            self._ser.setDTR(False)
            self._ser.setRTS(False)
        except Exception:
            pass
        time.sleep(0.1)
        self._ser.reset_input_buffer()

    def close(self) -> None:
        if self._ser:
            self._ser.close()
            self._ser = None

    def reset_board(self) -> None:
        assert self._ser
        self._ser.setDTR(False)
        self._ser.setRTS(True)
        time.sleep(0.08)
        self._ser.setRTS(False)
        self._ser.setDTR(True)
        time.sleep(0.3)
        self._ser.reset_input_buffer()
        self.buf.clear()

    def read_for(self, seconds: float) -> str:
        assert self._ser
        t0 = time.time()
        while time.time() - t0 < seconds:
            chunk = self._ser.read(8192)
            if chunk:
                self.buf.extend(chunk)
        return self.buf.decode("utf-8", "replace")

    def text(self) -> str:
        return self.buf.decode("utf-8", "replace")


def run_case(name: str, fn: Callable[[], str], report: Report) -> None:
    t0 = time.time()
    try:
        detail = fn() or ""
        report.add(Result(name, True, detail, (time.time() - t0) * 1000))
    except Exception as e:
        report.add(Result(name, False, f"{type(e).__name__}: {e}", (time.time() - t0) * 1000))


def test_uart_boot(uart: UartMonitor, report: Report) -> str:
    def case() -> str:
        uart.reset_board()
        text = uart.read_for(28.0)
        need = [
            "Initialization complete",
            "[WEB] Server port=80",
        ]
        missing = [s for s in need if s not in text]
        if missing:
            raise AssertionError(f"missing markers: {missing}")
        bad = []
        for needle in ("stack overflow", "Stack canary", "Guru Meditation", "Rebooting..."):
            # one Rebooting from our RTS reset is ok; panic loop is not
            pass
        panics = text.count("Guru Meditation") + text.count("Stack canary") + text.count("stack overflow")
        if panics:
            raise AssertionError(f"panic markers={panics}")
        # After init, allow at most 1 reboot (our reset). Count post-init reboots.
        idx = text.find("Initialization complete")
        after = text[idx:] if idx >= 0 else text
        # If device reboot-loops, we'll see multiple init completes
        inits = text.count("Initialization complete")
        if inits > 2:
            raise AssertionError(f"reboot loop suspected: init_complete={inits}")
        wifi_ok = ("Connected to" in text) or ("Wi-Fi:" in text)
        if not wifi_ok:
            raise AssertionError("Wi-Fi connect marker not found")
        return f"inits={inits} panics=0 wifi=ok len={len(text)}"

    run_case("uart.boot_stable", case, report)
    return uart.text()


def test_http_suite(http: HttpClient, report: Report, uart: Optional["UartMonitor"] = None) -> None:
    def spa() -> str:
        # Первый / собирает HTML-кэш в PSRAM (~6–15s) — не резать дефолтным timeout.
        old = http.timeout
        http.timeout = max(old, http.spa_timeout)
        try:
            code, body, _ = http.request("/", auth=False)
        finally:
            http.timeout = old
        if code != 200:
            raise AssertionError(f"status={code}")
        if b"<!DOCTYPE html>" not in body:
            raise AssertionError("HTML missing")
        if b"<script" not in body.lower():
            raise AssertionError("HTML truncated: missing <script> (SPA JS)")
        if b"</html>" not in body.lower():
            raise AssertionError("HTML truncated: missing </html>")
        if b"loginUser" not in body:
            raise AssertionError("loginUser missing")
        return f"len={len(body)}"

    def status_unauth() -> str:
        code, _, _ = http.request("/status", auth=False)
        if code not in (401, 403):
            raise AssertionError(f"expected 401, got {code}")
        return f"status={code}"

    def status_auth() -> str:
        code, payload, body, _ = http.json("/status")
        if code != 200:
            raise AssertionError(f"status={code} body={body[:120]!r}")
        if not isinstance(payload, dict):
            raise AssertionError("not json object")
        if "must_change_password" not in payload:
            raise AssertionError("must_change_password missing")
        if "audio" not in payload and payload.get("schema") != "rtsp-mic.public.v1":
            if "schema" not in payload:
                raise AssertionError("schema missing")
        return f"schema={payload.get('schema')} mcp={payload.get('must_change_password')}"

    def status_live() -> str:
        code1, p1, _, _ = http.json("/status")
        time.sleep(1.2)
        code2, p2, _, _ = http.json("/status")
        if code1 != 200 or code2 != 200:
            raise AssertionError(f"codes {code1}/{code2}")
        t1 = (p1 or {}).get("timestamp_ms")
        t2 = (p2 or {}).get("timestamp_ms")
        if t1 is None or t2 is None:
            raise AssertionError("timestamp_ms missing")
        if int(t2) <= int(t1):
            raise AssertionError(f"timestamp not advancing {t1} -> {t2}")
        return f"ts {t1} -> {t2}"

    def csrf() -> str:
        code, payload, _, _ = http.json("/api/csrf")
        if code != 200 or not payload or not payload.get("token"):
            raise AssertionError(f"status={code} payload={payload}")
        http.csrf = payload["token"]
        return f"token_len={len(http.csrf)}"

    def manifest() -> str:
        code, body, hdrs = http.request("/manifest.json", auth=False)
        if code != 200:
            raise AssertionError(f"status={code}")
        data = json.loads(body.decode("utf-8"))
        if "name" not in data and "short_name" not in data:
            raise AssertionError("manifest incomplete")
        return f"name={data.get('name') or data.get('short_name')}"

    def svg() -> str:
        code, body, _ = http.request("/manifest.svg", auth=False)
        if code != 200 or b"<svg" not in body.lower():
            raise AssertionError(f"status={code}")
        return f"len={len(body)}"

    def mel_default_pw() -> str:
        code, body, _ = http.request("/api/mel")
        if code == 429:
            time.sleep(0.6)
            code, body, _ = http.request("/api/mel")
        if code not in (200, 403):
            raise AssertionError(f"unexpected {code} body={body[:80]!r}")
        return f"status={code}"

    def integrations_get() -> str:
        code, body, _ = http.request("/api/integrations")
        if code not in (200, 403):
            raise AssertionError(f"unexpected {code} body={body[:80]!r}")
        return f"status={code}"

    def ws_ticket_default() -> str:
        code, body, _ = http.request("/api/ws-ticket")
        if code not in (200, 403):
            raise AssertionError(f"unexpected {code} body={body[:80]!r}")
        return f"status={code}"

    def password_change_validation() -> str:
        if not http.csrf:
            raise AssertionError("csrf missing")
        time.sleep(1.1)
        data = urllib.parse.urlencode({"user": "admin", "password": "short"}).encode()
        code, body, _ = http.request(
            "/api/system/password",
            method="POST",
            data=data,
            headers={"Content-Type": "application/x-www-form-urlencoded"},
        )
        if code == 429:
            time.sleep(1.2)
            code, body, _ = http.request(
                "/api/system/password",
                method="POST",
                data=data,
                headers={"Content-Type": "application/x-www-form-urlencoded"},
            )
        if code not in (200, 400, 403):
            raise AssertionError(f"unexpected {code} body={body[:120]!r}")
        return f"status={code} body={body[:80]!r}"

    def weather_absent() -> str:
        """OWM/WeatherProvider removed from product — /status must not advertise weather."""
        code, payload, _, _ = http.json("/status")
        if code != 200 or not payload:
            raise AssertionError(f"status={code}")
        if "weather" in payload:
            raise AssertionError(f"unexpected weather block: {payload.get('weather')}")
        return "weather absent (product)"

    def rtsp_port() -> str:
        code, payload, _, _ = http.json("/status")
        if code != 200 or not payload:
            raise AssertionError(f"status={code}")
        port = payload.get("rtsp_port", 554)
        if int(port) != 554:
            raise AssertionError(f"unexpected rtsp_port={port}")
        return "rtsp_port=554"

    def stability_burst_no_ws_overflow() -> str:
        if uart:
            uart.buf.clear()
        errors = 0
        for _ in range(20):
            try:
                code, _, _ = http.request("/status")
                if code != 200:
                    errors += 1
            except Exception:
                errors += 1
            time.sleep(0.15)
        text = uart.text() if uart else ""
        if "Too many messages queued" in text:
            raise AssertionError("WS queue overflow in UART")
        if "Guru Meditation" in text or "abort()" in text:
            raise AssertionError("panic during burst")
        if errors > 5:
            raise AssertionError(f"http errors={errors}/20")
        return f"http_err={errors} uart_len={len(text)}"

    run_case("http.spa", spa, report)
    run_case("http.status_unauth", status_unauth, report)
    run_case("http.status_auth", status_auth, report)
    run_case("http.status_live", status_live, report)
    run_case("http.csrf", csrf, report)
    run_case("http.manifest", manifest, report)
    run_case("http.manifest_svg", svg, report)
    run_case("http.mel_gate", mel_default_pw, report)
    run_case("http.integrations_gate", integrations_get, report)
    run_case("http.ws_ticket_gate", ws_ticket_default, report)
    run_case("http.password_validation", password_change_validation, report)
    run_case("http.weather_absent", weather_absent, report)
    run_case("http.rtsp_port", rtsp_port, report)
    run_case("stability.http_burst_no_ws_overflow", stability_burst_no_ws_overflow, report)


def test_uart_no_panic_while_http(uart: UartMonitor, http: HttpClient, report: Report) -> None:
    def case() -> str:
        uart.buf.clear()
        errors = 0
        for _ in range(12):
            try:
                code, _, _ = http.request("/status")
                if code != 200:
                    errors += 1
            except Exception:
                errors += 1
            uart.read_for(0.5)
        text = uart.text()
        panic_needles = (
            "Guru Meditation Error",
            "Stack canary watchpoint triggered",
            "stack overflow",
            "Task watchdog got triggered",
            "Abort called",
        )
        panics = sum(text.count(n) for n in panic_needles)
        reboots = text.count("Rebooting...")
        if panics or reboots:
            raise AssertionError(f"panics={panics} reboots={reboots} http_err={errors}")
        if errors > 3:
            raise AssertionError(f"too many HTTP errors under load: {errors}")
        return f"uart_bytes={len(uart.buf)} panics=0 reboots=0 http_err={errors}"

    run_case("uart.stable_under_http", case, report)


def discover_base(hint: str) -> str:
    candidates = [hint]
    # common fallbacks
    for ip in ("http://192.168.2.65", "http://rtsp-mic.local", "http://192.168.4.1"):
        if ip not in candidates:
            candidates.append(ip)
    for base in candidates:
        try:
            req = urllib.request.Request(base + "/manifest.json")
            with urllib.request.urlopen(req, timeout=2.5) as r:
                if r.status == 200:
                    return base
        except Exception:
            continue
    return hint


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        try:
            sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass

    ap = argparse.ArgumentParser(description="RTSP Mic HIL tests")
    ap.add_argument("--port", default=os.environ.get("RTSPMIC_HIL_PORT", "COM6"))
    ap.add_argument("--base", default=os.environ.get("RTSPMIC_HIL_BASE", "http://192.168.2.65"))
    ap.add_argument("--user", default=os.environ.get("RTSPMIC_HIL_USER", DEFAULT_USER))
    ap.add_argument("--password", default=os.environ.get("RTSPMIC_HIL_PASS", DEFAULT_PASS))
    ap.add_argument("--skip-uart", action="store_true")
    ap.add_argument("--skip-http", action="store_true")
    args = ap.parse_args()

    report = Report()
    print("=== RTSP MIC HIL ===")
    print(f"port={args.port} base={args.base} user={args.user}")

    uart = None
    if not args.skip_uart:
        if serial is None:
            report.add(Result("uart.import", False, "pyserial missing"))
        else:
            try:
                uart = UartMonitor(args.port)
                uart.open()
                report.add(Result("uart.open", True, args.port))
                test_uart_boot(uart, report)
            except Exception as e:
                report.add(Result("uart.open", False, str(e)))
                uart = None

    base = discover_base(args.base)
    if base != args.base:
        print(f"[info] discovered base={base}")
    http = HttpClient(base, args.user, args.password)

    if not args.skip_http:
        test_http_suite(http, report, uart)
        if uart:
            test_uart_no_panic_while_http(uart, http, report)

    if uart:
        uart.close()

    print("=== SUMMARY ===")
    print(f"passed={report.passed} failed={report.failed} total={len(report.results)}")
    # Note about browser extension noise
    print(
        "NOTE: contentscript.js MaxListenersExceededWarning / ObjectMultiplex "
        "— это расширение браузера, не прошивка RTSP Mic."
    )
    return 1 if report.failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
