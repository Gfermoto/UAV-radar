"""Thin Overpass proxy for Hub map acoustic layers (browser CORS bypass)."""
from __future__ import annotations

import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from typing import Any

_LOCK = threading.Lock()
_SEM = threading.Semaphore(2)
_LAST_TS = 0.0
_MIN_INTERVAL_S = 1.0
_MAX_QL = 12_000
_TIMEOUT_S = 12.0
_CACHE_TTL_S = 30 * 60.0
_CACHE: dict[str, tuple[float, dict[str, Any]]] = {}
_UPSTREAMS = (
    "https://overpass.openstreetmap.fr/api/interpreter",
    "https://overpass-api.de/api/interpreter",
)
_UA = "NEVOD-Hub/0.1.42 (lab; acoustic-map; +https://github.com/Gfermoto/nevod)"


def proxy_overpass(ql: str) -> tuple[int, dict[str, Any] | bytes]:
    """Returns (http_code, json_dict_or_raw_bytes_for_passthrough).

    On success returns (200, parsed_json). On rate limit (429, {error}).
    """
    global _LAST_TS
    q = (ql or "").strip()
    if not q or len(q) > _MAX_QL:
        return 400, {"error": "bad_query", "max_len": _MAX_QL}
    if "[out:" not in q:
        return 400, {"error": "query_must_be_overpass_ql"}
    cached = _CACHE.get(q)
    if cached and time.time() - cached[0] < _CACHE_TTL_S:
        return 200, cached[1]
    with _LOCK:
        now = time.monotonic()
        if now - _LAST_TS < _MIN_INTERVAL_S:
            return 429, {"error": "rate_limited", "retry_s": _MIN_INTERVAL_S}
        _LAST_TS = now

    if not _SEM.acquire(blocking=False):
        return 429, {"error": "too_many_concurrent", "retry_s": 5}

    try:
        body = urllib.parse.urlencode({"data": q}).encode("utf-8")
        last_err = "upstream_fail"
        for url in _UPSTREAMS:
            req = urllib.request.Request(
                url,
                data=body,
                method="POST",
                headers={
                    "Content-Type": "application/x-www-form-urlencoded",
                    "Accept": "application/json",
                    "User-Agent": _UA,
                },
            )
            try:
                with urllib.request.urlopen(req, timeout=_TIMEOUT_S) as resp:
                    raw = resp.read()
                import json

                try:
                    parsed = json.loads(raw.decode("utf-8"))
                except json.JSONDecodeError:
                    last_err = "bad_upstream_json"
                    continue
                if isinstance(parsed, dict) and (
                    isinstance(parsed.get("elements"), list) or not parsed.get("remark")
                ):
                    if isinstance(parsed.get("elements"), list):
                        _CACHE[q] = (time.time(), parsed)
                    if parsed.get("remark") and not parsed.get("elements"):
                        last_err = "upstream_remark"
                        continue
                    return 200, parsed
                last_err = "bad_upstream_json"
            except urllib.error.HTTPError as e:
                last_err = f"http_{e.code}"
                continue
            except Exception as e:  # noqa: BLE001
                last_err = type(e).__name__
                continue
        if cached:
            return 200, cached[1]
        return 502, {"error": last_err}
    finally:
        _SEM.release()
