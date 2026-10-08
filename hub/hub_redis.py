#!/usr/bin/env python3
"""Optional Redis for Nevod Hub (sessions / health). SQLite остаётся SoT данных."""
from __future__ import annotations

import os
import threading
import time
from typing import Any
from urllib.parse import urlparse

_PREFIX = "nevod:hub:"
_lock = threading.Lock()
_client: Any = None
_configured = False
_last_ok = False
_last_err = ""
_last_ping_at = 0.0


def _truthy(name: str, default: bool = False) -> bool:
    v = os.environ.get(name, "")
    if not v:
        return default
    return v.strip().lower() in ("1", "true", "yes", "on")


def redis_url() -> str:
    """HUB_REDIS_URL or redis://HOST:PORT/DB (+ optional password)."""
    url = (os.environ.get("HUB_REDIS_URL") or "").strip()
    if url:
        return url
    host = (os.environ.get("HUB_REDIS_HOST") or os.environ.get("REDIS_HOST") or "").strip()
    if not host:
        return ""
    port = int(os.environ.get("HUB_REDIS_PORT") or os.environ.get("REDIS_PORT") or "6379")
    db = int(os.environ.get("HUB_REDIS_DB") or os.environ.get("REDIS_DB") or "0")
    pw = (os.environ.get("HUB_REDIS_PASSWORD") or os.environ.get("REDIS_PASSWORD") or "").strip()
    auth = f":{pw}@" if pw else ""
    return f"redis://{auth}{host}:{port}/{db}"


def configure_from_env() -> bool:
    """Connect if URL set. Returns True when client ready."""
    global _client, _configured, _last_ok, _last_err
    url = redis_url()
    if not url:
        with _lock:
            _client = None
            _configured = False
            _last_ok = False
            _last_err = ""
        return False
    try:
        import redis as redis_mod  # type: ignore
    except ImportError:
        with _lock:
            _client = None
            _configured = True
            _last_ok = False
            _last_err = "redis_package_missing"
        return False
    try:
        # from_url handles redis:// and rediss://
        cli = redis_mod.Redis.from_url(
            url,
            decode_responses=True,
            socket_connect_timeout=1.5,
            socket_timeout=1.5,
            health_check_interval=30,
        )
        cli.ping()
        with _lock:
            _client = cli
            _configured = True
            _last_ok = True
            _last_err = ""
        return True
    except Exception as e:  # noqa: BLE001
        with _lock:
            _client = None
            _configured = True
            _last_ok = False
            _last_err = str(e)[:120]
        return False


def enabled() -> bool:
    return _configured and _client is not None and _last_ok


def ping(force: bool = False) -> bool:
    """Cheap health; throttled unless force."""
    global _last_ok, _last_err, _last_ping_at
    if not _configured or _client is None:
        return False
    now = time.time()
    if not force and (now - _last_ping_at) < 5.0:
        return _last_ok
    try:
        _client.ping()
        with _lock:
            _last_ok = True
            _last_err = ""
            _last_ping_at = now
        return True
    except Exception as e:  # noqa: BLE001
        with _lock:
            _last_ok = False
            _last_err = str(e)[:120]
            _last_ping_at = now
        return False


def public_dict() -> dict[str, Any]:
    url = redis_url()
    configured = bool(url) or _configured
    ok = ping() if (_client is not None) else False
    host = ""
    db = 0
    if url:
        try:
            u = urlparse(url)
            host = u.hostname or ""
            path = (u.path or "/0").lstrip("/")
            db = int(path or "0")
        except Exception:  # noqa: BLE001
            host = "?"
    return {
        "configured": configured,
        "ok": ok,
        "required": _truthy("HUB_REDIS_REQUIRED", False),
        "host": host,
        "db": db,
        "error": _last_err if configured and not ok else "",
    }


def _key(kind: str, token: str) -> str:
    return f"{_PREFIX}sess:{kind}:{token}"


def session_set(kind: str, token: str, ttl_s: float) -> bool:
    """Store session token. Returns False if Redis unavailable (caller uses RAM)."""
    if not ping():
        return False
    try:
        _client.setex(_key(kind, token), int(max(1, ttl_s)), "1")
        return True
    except Exception as e:  # noqa: BLE001
        global _last_ok, _last_err
        with _lock:
            _last_ok = False
            _last_err = str(e)[:120]
        return False


def session_ok(kind: str, token: str) -> bool | None:
    """
    True/False if Redis answered; None = Redis down → caller checks RAM.
    """
    if not token or _client is None or not _configured:
        return None
    if not ping():
        return None
    try:
        return bool(_client.exists(_key(kind, token)))
    except Exception as e:  # noqa: BLE001
        global _last_ok, _last_err
        with _lock:
            _last_ok = False
            _last_err = str(e)[:120]
        return None


def session_clear(kind: str, token: str) -> None:
    if not token or _client is None:
        return
    try:
        _client.delete(_key(kind, token))
    except Exception:  # noqa: BLE001
        pass
