#!/usr/bin/env python3
"""Unlock / UI-login sessions + Bearer check for Nevod Hub."""
from __future__ import annotations

import hashlib
import hmac
import os
import re
import secrets
import time
from typing import Any

import hub_redis

# Bound by nevod_hub.main / run_self_test (same pattern as former globals).
STORE: Any = None
HUB_UNLOCK_SESSIONS: dict[str, float] = {}
HUB_SETTINGS_SESSIONS: dict[str, float] = {}
HUB_UI_SESSIONS: dict[str, float] = {}
HUB_UNLOCK_TTL_S = 3600.0
HUB_UI_TTL_S = 12 * 3600.0
HUB_UNLOCK_HARDCODED_PW = "111000111"
HUB_UI_COOKIE = "nevod_hub_ui"
_UI_USER_RE = re.compile(r"^[A-Za-z0-9._-]{1,32}$")
_PBKDF2_ITERS = 120_000


def _env_truthy(name: str, default: bool = False) -> bool:
    v = os.environ.get(name, "")
    if not v:
        return default
    return v.strip().lower() in ("1", "true", "yes", "on")


def _hash_unlock_pw(password: str) -> str:
    return hashlib.sha256(password.encode("utf-8")).hexdigest()


def _unlock_hash_stored() -> str:
    """Hash for verify: meta → HUB_ENGINEER_PASSWORD → lab hardcoded (opt-in)."""
    if STORE is not None:
        v = STORE.get_meta("engineer_unlock_hash")
        if isinstance(v, str) and v:
            return v
    env_pw = (os.environ.get("HUB_ENGINEER_PASSWORD") or "").strip()
    if env_pw:
        return _hash_unlock_pw(env_pw)
    if _env_truthy("HUB_LAB_DEFAULT_UNLOCK", False):
        return _hash_unlock_pw(HUB_UNLOCK_HARDCODED_PW)
    return ""


def _unlock_set(password: str) -> None:
    if len(password) < 8 or ":" in password:
        raise ValueError("unlock_password_invalid")
    if STORE is None:
        raise ValueError("store_required")
    STORE.set_meta("engineer_unlock_hash", _hash_unlock_pw(password))


def _session_ok_ram(table: dict[str, float], tok: str) -> bool:
    exp = table.get(tok)
    if exp is None:
        return False
    if time.time() > exp:
        table.pop(tok, None)
        return False
    return True


def _session_ok(table: dict[str, float], kind: str, header: str | None) -> bool:
    tok = (header or "").strip()
    if not tok:
        return False
    ram_ok = _session_ok_ram(table, tok)
    r = hub_redis.session_ok(kind, tok)
    if r is True:
        return True
    if r is False:
        # Redis miss: keep valid RAM session (write may have failed) and re-sync.
        if ram_ok:
            exp = table.get(tok)
            if exp is not None:
                hub_redis.session_set(kind, tok, max(1.0, exp - time.time()))
            return True
        table.pop(tok, None)
        return False
    return ram_ok


def _session_issue(table: dict[str, float], kind: str, ttl_s: float | None = None) -> str:
    ttl = HUB_UNLOCK_TTL_S if ttl_s is None else ttl_s
    tok = secrets.token_urlsafe(24)
    table[tok] = time.time() + ttl
    hub_redis.session_set(kind, tok, ttl)
    return tok


def _unlock_session_ok(header: str | None) -> bool:
    return _session_ok(HUB_UNLOCK_SESSIONS, "unlock", header)


def _unlock_issue() -> str:
    return _session_issue(HUB_UNLOCK_SESSIONS, "unlock")


def _settings_hash_stored() -> str | None:
    """None = пароль настроек ещё не задан (вкладка открыта без пароля)."""
    if STORE is None:
        return None
    v = STORE.get_meta("settings_unlock_hash")
    if isinstance(v, str) and v:
        return v
    return None


def _settings_password_set() -> bool:
    return _settings_hash_stored() is not None


def _settings_set(password: str) -> None:
    if len(password) < 4 or ":" in password:
        raise ValueError("settings_password_invalid")
    if STORE is None:
        raise ValueError("store_required")
    STORE.set_meta("settings_unlock_hash", _hash_unlock_pw(password))


def _settings_clear() -> None:
    if STORE is None:
        raise ValueError("store_required")
    STORE.set_meta("settings_unlock_hash", "")


def _settings_session_ok(header: str | None) -> bool:
    return _session_ok(HUB_SETTINGS_SESSIONS, "settings", header)


def _settings_issue() -> str:
    return _session_issue(HUB_SETTINGS_SESSIONS, "settings")


def _hash_ui_pw(password: str) -> str:
    salt = secrets.token_hex(16)
    dk = hashlib.pbkdf2_hmac(
        "sha256",
        password.encode("utf-8"),
        bytes.fromhex(salt),
        _PBKDF2_ITERS,
    )
    return f"pbkdf2${_PBKDF2_ITERS}${salt}${dk.hex()}"


def _verify_ui_pw(password: str, stored: str) -> bool:
    if not isinstance(stored, str) or not stored.startswith("pbkdf2$"):
        return False
    parts = stored.split("$")
    if len(parts) != 4:
        return False
    try:
        iters = int(parts[1])
        salt = parts[2]
        want = parts[3]
        dk = hashlib.pbkdf2_hmac(
            "sha256",
            password.encode("utf-8"),
            bytes.fromhex(salt),
            iters,
        )
    except (ValueError, TypeError):
        return False
    return hmac.compare_digest(dk.hex(), want)


def _validate_ui_user(user: str) -> str:
    u = (user or "").strip()
    if not _UI_USER_RE.fullmatch(u):
        raise ValueError("ui_user_invalid")
    return u


def _validate_ui_password(password: str) -> None:
    if len(password) < 8 or ":" in password:
        raise ValueError("ui_password_invalid")


def _ui_configured() -> bool:
    if STORE is None:
        return False
    u = STORE.get_meta("ui_user")
    h = STORE.get_meta("ui_password_hash")
    return bool(isinstance(u, str) and u) and bool(isinstance(h, str) and h)


def _ui_set(user: str, password: str) -> None:
    if STORE is None:
        raise ValueError("store_required")
    u = _validate_ui_user(user)
    _validate_ui_password(password)
    STORE.set_meta("ui_user", u)
    STORE.set_meta("ui_password_hash", _hash_ui_pw(password))


def _ui_verify(user: str, password: str) -> bool:
    if STORE is None:
        return False
    stored_u = STORE.get_meta("ui_user")
    stored_h = STORE.get_meta("ui_password_hash")
    if not isinstance(stored_u, str) or not stored_u:
        return False
    if not isinstance(stored_h, str) or not stored_h:
        return False
    u = (user or "").strip()
    if not hmac.compare_digest(u, stored_u):
        return False
    return _verify_ui_pw(password, stored_h)


def _ui_issue() -> str:
    return _session_issue(HUB_UI_SESSIONS, "ui", HUB_UI_TTL_S)


def _ui_session_ok(tok: str | None) -> bool:
    return _session_ok(HUB_UI_SESSIONS, "ui", tok)


def _ui_session_drop(tok: str) -> None:
    tok = (tok or "").strip()
    if not tok:
        return
    HUB_UI_SESSIONS.pop(tok, None)
    hub_redis.session_clear("ui", tok)


def _ui_token_from_cookie(header: str | None) -> str:
    if not header:
        return ""
    prefix = HUB_UI_COOKIE + "="
    for part in header.split(";"):
        p = part.strip()
        if p.startswith(prefix):
            return p[len(prefix):].strip()
    return ""


def _ui_set_cookie_header(tok: str) -> str:
    return (
        f"{HUB_UI_COOKIE}={tok}; HttpOnly; Secure; SameSite=Lax; Path=/; "
        f"Max-Age={int(HUB_UI_TTL_S)}"
    )


def _ui_clear_cookie_header() -> str:
    return (
        f"{HUB_UI_COOKIE}=; HttpOnly; Secure; SameSite=Lax; Path=/; Max-Age=0"
    )


def _bearer_ok(header: str | None, token: str) -> bool:
    if not token:
        return _env_truthy("HUB_ALLOW_EMPTY_TOKEN", False)
    expected = f"Bearer {token}"
    provided = header or ""
    if len(provided) != len(expected):
        return False
    return hmac.compare_digest(provided, expected)


def bind_store(store: Any) -> None:
    """Wire HubStore into auth helpers (call from main / self-test)."""
    global STORE
    STORE = store
