#!/usr/bin/env python3
"""Nevod Hub store-and-forward → upstream Cloud PB.

@file hub_forward.py
@brief LoRa MQTT→Cloud uplink: queue, Bearer probe, Mel/zone policy.

@details
  - DIY/Light PB already carry a device Bearer — do not inject Hub Cloud token there.
  - Hub Cloud token (UI/env `forward_token`) is the cabinet key for LoRa.
    Empty key uses CLOUD_INGEST_TOKEN_DEFAULT — the same key as a fresh IoT.
    A saved key is not replaced on 401/403. PB keeps the device Bearer.
  - `has_token` is true only when the operator saved a key. `cabinet_key` is true
    when LoRa has either that key or the fresh-firmware default.
  - `ensure_probed()` / `probe_uplink()` POST ReportHeartbeat to verify Cloud accepts Bearer
    (HTTP 400 = auth OK / bad protobuf; 401/403 = rejected).

@see docs/superpowers/specs/2026-09-20-hub-maintainability-refactor-design.md
"""
from __future__ import annotations

import os
import re
import ssl
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any
from urllib.parse import urlparse

from hub_webhooks import webhook_url_allowed

# Product Cloud ingest (HTTPS PB). Token value lives in Config.h, not here.
DEFAULT_CLOUD_ORIGIN = "https://nevod.endorphine.agency"
# Сетевой сбой не выбрасывает пакет. 4xx (кроме 408/429) — да, повтор не поможет.
FORWARD_RETRY_MAX_AGE_S = 6 * 3600
_FACTORY_TOKEN_RE = re.compile(
    r'#define\s+CLOUD_INGEST_TOKEN_DEFAULT\s*\\\s*"([^"]+)"'
)
_factory_token_cache: str | None = None


def _forward_allow_private() -> bool:
    return os.environ.get("HUB_FORWARD_ALLOW_PRIVATE", "0").strip().lower() in (
        "1",
        "true",
        "yes",
        "on",
    )


def _forward_allow_loopback() -> bool:
    return os.environ.get("HUB_FORWARD_ALLOW_LOOPBACK", "0").strip().lower() in (
        "1",
        "true",
        "yes",
        "on",
    )


def factory_cloud_token() -> str:
    """Заводской Bearer из Config.h. Пусто, если файла нет (старый образ)."""
    global _factory_token_cache
    env = (os.environ.get("CLOUD_INGEST_TOKEN_DEFAULT") or "").strip()
    if env:
        return env
    if _factory_token_cache is not None:
        return _factory_token_cache
    here = Path(__file__).resolve()
    candidates = (
        here.parents[1] / "src" / "Config.h",
        here.parent / "Config.h",
        Path("/app/Config.h"),
    )
    found = ""
    for path in candidates:
        try:
            text = path.read_text(encoding="utf-8")
        except OSError:
            continue
        m = _FACTORY_TOKEN_RE.search(text)
        if m:
            found = m.group(1).strip()
            break
    _factory_token_cache = found
    return found


def looks_pro(*raw: object) -> bool:
    """Pro на Hub не принимаем. sku/device_type == pro, в том числе внутри extensions."""
    for item in raw:
        if isinstance(item, dict):
            if looks_pro(
                item.get("device_type"),
                item.get("sku"),
                item.get("extensions"),
            ):
                return True
            continue
        if str(item or "").strip().lower() == "pro":
            return True
    return False


def stamp_cloud_extensions(ext: dict | None) -> dict[str, Any]:
    """Флаг доставки ставит Hub, не плата. path=lora только если пакет уже был LoRa."""
    out = dict(ext or {})
    marker = str(out.get("via") or "").strip().lower()
    path = str(out.get("path") or "").strip().lower()
    if marker in ("lora_gw", "lora", "lora_mqtt") or path in (
        "lora_gw",
        "lora",
        "lora_mqtt",
    ):
        out["path"] = "lora"
    out["via"] = "hub"
    return out


def forward_headers(token: str, *, path_lora: bool = False) -> dict[str, str]:
    headers = {
        "Authorization": f"Bearer {token}",
        "Content-Type": "application/x-protobuf",
        "X-Nevod-Via": "hub",
    }
    if path_lora:
        headers["X-Nevod-Path"] = "lora"
    return headers


def forward_should_abandon(exc: BaseException, *, age_s: float) -> bool:
    """Постоянная ошибка — сразу. Таймаут и DNS — пока пакет не старше 6 часов."""
    if isinstance(exc, urllib.error.HTTPError):
        code = int(exc.code or 0)
        if code in (408, 429) or code >= 500 or code <= 0:
            return age_s >= FORWARD_RETRY_MAX_AGE_S
        if 400 <= code < 500:
            return True
    if isinstance(exc, ValueError) and str(exc) == "cabinet_key_missing":
        return True
    return age_s >= FORWARD_RETRY_MAX_AGE_S


def assert_forward_url_allowed(url: str) -> None:
    u = (url or "").strip().rstrip("/")
    if not u:
        return
    host = (urlparse(u).hostname or "").lower()
    if host in ("127.0.0.1", "localhost", "::1") and _forward_allow_loopback():
        return
    ok, reason = webhook_url_allowed(u, allow_private=_forward_allow_private())
    if not ok:
        raise ValueError(f"forward_url_blocked:{reason}")


class HubForwarder:
    """Queue Cloud PB posts; expose token/probe status for Hub UI.

    @brief Store-and-forward worker + public_dict for dashboard.
    """

    def __init__(self, store: Any) -> None:
        self.store = store
        self.base = ""
        self.token = ""
        self.ingest_token = ""
        self.ingest_explicit = False
        self.ca_file = ""
        self.forward_mel = False
        self.mel_lan_only = os.environ.get("HUB_MEL_LAN_ONLY", "0").lower() in (
            "1",
            "true",
            "yes",
        )
        self.queue_max = max(8, int(os.environ.get("HUB_FORWARD_QUEUE_MAX", "256") or 256))
        self.max_attempts = max(1, int(os.environ.get("HUB_FORWARD_MAX_ATTEMPTS", "8") or 8))
        self.adsb: Any | None = None  # AdsbService — optional #116 filter
        self.zones: Any | None = None  # ZoneRegistry — Mel/zone policy
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self.last_ok_at: float | None = None
        self.probe_ok: bool | None = None
        self.probe_at: float | None = None
        self.probe_error: str = ""
        self.probe_http: int = 0
        self._origin_block = ""
        self._cabinet_error = ""
        self._cabinet_missing_logged = False
        base_stats: dict[str, Any] = {}
        if store is not None:
            try:
                raw = store.forward_stats()
                if isinstance(raw, dict):
                    base_stats = raw
            except Exception:  # noqa: BLE001
                base_stats = {}
        self._drop_base = int(base_stats.get("dropped_total") or 0)
        self._abandon_base = int(base_stats.get("abandoned_total") or 0)
        self._last_err_log = 0.0
        self._lock = threading.Lock()
        self._start_lock = threading.Lock()
        self._load_bootstrap()

    def _load_bootstrap(self) -> None:
        """Env first, then SQLite meta overrides (UI-saved)."""
        self.base = (
            os.environ.get("HUB_FORWARD_URL") or DEFAULT_CLOUD_ORIGIN
        ).rstrip("/")
        self.token = (os.environ.get("HUB_FORWARD_TOKEN") or "").strip()
        self.ca_file = (os.environ.get("HUB_FORWARD_CA") or "").strip()
        self.forward_mel = os.environ.get("HUB_FORWARD_MEL", "1").lower() in (
            "1",
            "true",
            "yes",
        )
        if self.store is None:
            return
        meta_url = self.store.get_meta("forward_url")
        meta_tok = self.store.get_meta("forward_token")
        meta_ca = self.store.get_meta("forward_ca")
        meta_mel = self.store.get_meta("forward_mel")
        if isinstance(meta_url, str) and meta_url.strip():
            self.base = meta_url.strip().strip("\"'").rstrip("/")
        self._note_origin_check()
        if isinstance(meta_tok, str) and meta_tok.strip():
            self.token = meta_tok.strip()
        if isinstance(meta_ca, str):
            self.ca_file = meta_ca.strip()
        if meta_mel is not None:
            self.forward_mel = bool(meta_mel)

    def _mel_lan_only(self) -> bool:
        if self.zones is not None:
            return bool(self.zones.mel_lan_only_global)
        return bool(self.mel_lan_only)

    def _should_forward_mel(self, node_id: str | None) -> bool:
        if self.zones is not None:
            zone = self.zones.zone_for_node(node_id)
            if zone is not None and zone.get("forward_mel") is True:
                return True
            if not self.forward_mel:
                return False
            return bool(self.zones.effective_forward_mel(node_id))
        return bool(self.forward_mel) and not self.mel_lan_only

    def bind_ingest_token(self, token: str, *, explicit: bool) -> None:
        """INGEST_TOKEN клиента. explicit=False — пустая установка, заводской подставится сам."""
        self.ingest_token = (token or "").strip()
        self.ingest_explicit = bool(explicit and self.ingest_token)

    def _uplink_token(self) -> str:
        """Ключ LoRa в облако: сохранённый на Hub, иначе ключ свежей прошивки.

        DIY/Light PB несёт Bearer устройства. INGEST_TOKEN панели сюда не берём.
        Сохранённый ключ на 401/403 не подменяется.
        """
        saved = (self.token or "").strip()
        if saved:
            return saved
        return factory_cloud_token()

    def token_source(self) -> str:
        """user = ключ задан на Hub; factory = ключ свежей прошивки; none = пусто."""
        if (self.token or "").strip():
            return "user"
        if factory_cloud_token():
            return "factory"
        return "none"

    def ensure_probed(self) -> None:
        """One-shot Cloud auth check for the LoRa key (saved or fresh-firmware).

        Skips only a Hub with no key at all. At most one network round-trip.
        """
        if self.token_source() == "none":
            return
        if self.probe_at is not None:
            return
        if not (self.base or "").strip():
            return
        try:
            self.probe_uplink()
        except Exception:  # noqa: BLE001
            pass

    @property
    def enabled(self) -> bool:
        return bool(self.base)

    def public_dict(self, *, include_origin: bool = False) -> dict[str, Any]:
        st = (
            self.store.forward_stats()
            if self.store
            else {
                "queue_depth": 0,
                "last_error": "",
                "dropped_total": 0,
                "abandoned_total": 0,
            }
        )
        mel_lan = self._mel_lan_only()
        src = self.token_source()
        last_error = str(st.get("last_error", "") or "")
        if self._cabinet_error and src == "none":
            last_error = self._cabinet_error
        dropped = int(st.get("dropped_total") or 0)
        abandoned = int(st.get("abandoned_total") or 0)
        out: dict[str, Any] = {
            "enabled": self.enabled,
            "has_token": src == "user",
            "cabinet_key": src in ("user", "factory"),
            "token_source": src,
            "has_origin": bool(self.base),
            "forward_mel": (
                self._should_forward_mel(None)
                if self.zones is not None
                else (self.forward_mel and not mel_lan)
            ),
            "mel_lan_only": mel_lan,
            "queue_depth": st.get("queue_depth", 0),
            "queue_max": self.queue_max,
            "dropped_total": dropped,
            "abandoned_total": abandoned,
            "dropped_boot": max(0, dropped - int(getattr(self, "_drop_base", 0) or 0)),
            "abandoned_boot": max(0, abandoned - int(getattr(self, "_abandon_base", 0) or 0)),
            "nodes": st.get("nodes") or {},
            "last_error": last_error,
            "last_ok_at": self.last_ok_at,
            "probe_ok": self.probe_ok,
            "probe_at": self.probe_at,
            "probe_error": self.probe_error or "",
            "probe_http": int(self.probe_http or 0),
            "origin_block": self._origin_block,
        }
        if include_origin:
            out["base"] = self.base
            out["ca_file"] = self.ca_file
        else:
            # Не светим PB origin без инженерного unlock (как скрытое меню IoT).
            out["base"] = ""
            out["ca_file"] = ""
        return out

    def set_token(self, token: str, *, probe: bool = False) -> dict[str, Any]:
        tok = (token or "").strip()
        with self._lock:
            self.token = tok
            if self.store is not None:
                self.store.set_meta("forward_token", tok)
            if not tok:
                self.probe_ok = None
                self.probe_at = None
                self.probe_error = ""
                self.probe_http = 0
            else:
                self._cabinet_error = ""
                self._cabinet_missing_logged = False
        self._ensure_worker()
        if probe and tok and self.base:
            self.probe_uplink()
        return self.public_dict(include_origin=False)

    def probe_uplink(self) -> dict[str, Any]:
        """POST Cloud /api/v1/pb/ReportHeartbeat with saved Bearer — auth check, no enqueue.

        Valid token → 200 or 400 (protobuf rejected, auth OK).
        Bad token → 401/403.
        """
        with self._lock:
            tok = self._uplink_token()
            base = (self.base or "").rstrip("/")
            ca = self.ca_file
        self.probe_at = time.time()
        self.probe_http = 0
        self.probe_error = ""
        self.probe_ok = None
        if not tok:
            self.probe_ok = False
            self.probe_error = "no_token"
            return self.public_dict(include_origin=False)
        if not base:
            self.probe_ok = False
            self.probe_error = "no_url"
            return self.public_dict(include_origin=False)
        url = f"{base}/api/v1/pb/ReportHeartbeat"
        headers = {
            "Authorization": f"Bearer {tok}",
            "Accept": "application/json",
            "Content-Type": "application/octet-stream",
            "X-Nevod-Via": "hub",
            "User-Agent": "nevod-hub-probe/1",
        }
        try:
            # Minimal body: Cloud rejects protobuf (400) but accepts Bearer.
            req = urllib.request.Request(
                url, data=b"\x00", headers=headers, method="POST"
            )
            ctx = ssl.create_default_context()
            if ca and Path(ca).is_file():
                ctx.load_verify_locations(ca)
            with urllib.request.urlopen(req, timeout=12, context=ctx) as resp:
                self.probe_http = int(getattr(resp, "status", 200) or 200)
                self.probe_ok = 200 <= self.probe_http < 300
                if not self.probe_ok:
                    self.probe_error = f"http_{self.probe_http}"
        except urllib.error.HTTPError as e:
            self.probe_http = int(e.code or 0)
            if self.probe_http in (401, 403):
                self.probe_ok = False
                self.probe_error = "unauthorized"
            elif self.probe_http == 400:
                # Auth passed; body intentionally invalid.
                self.probe_ok = True
                self.probe_error = ""
            else:
                self.probe_ok = False
                self.probe_error = f"http_{self.probe_http}"
        except Exception as e:  # noqa: BLE001
            self.probe_ok = False
            self.probe_error = (type(e).__name__ or "error")[:48]
        return self.public_dict(include_origin=False)

    def set_origin(
        self,
        *,
        base: str,
        ca_file: str | None = None,
        forward_mel: bool | None = None,
        mel_lan_only: bool | None = None,
    ) -> dict[str, Any]:
        url = (base or "").strip().rstrip("/")
        assert_forward_url_allowed(url)
        with self._lock:
            self.base = url
            if ca_file is not None:
                self.ca_file = (ca_file or "").strip()
            if forward_mel is not None:
                self.forward_mel = bool(forward_mel)
                # Cloud uplink checkbox ↔ global LAN-only (zone.forward_mel overrides per T4).
                if self.zones is not None:
                    self.zones.mel_lan_only_global = not bool(forward_mel)
                    self.zones.persist()
            if mel_lan_only is not None:
                self.mel_lan_only = bool(mel_lan_only)
                if self.zones is not None:
                    self.zones.mel_lan_only_global = bool(mel_lan_only)
                    self.zones.persist()
            if self.store is not None:
                self.store.set_meta("forward_url", self.base)
                self.store.set_meta("forward_ca", self.ca_file)
                self.store.set_meta("forward_mel", self.forward_mel)
        self._ensure_worker()
        return self.public_dict(include_origin=True)

    def maybe_enqueue(
        self,
        path: str,
        body: bytes,
        *,
        node_id: str | None = None,
        lat: float | None = None,
        lon: float | None = None,
        timestamp_ms: int | None = None,
        path_lora: bool = False,
        bearer: str = "",
    ) -> None:
        if not self.enabled or self.store is None:
            return
        if path_lora and not self._uplink_token():
            self._cabinet_error = "cabinet_key_missing"
            if not self._cabinet_missing_logged:
                self._cabinet_missing_logged = True
                print("[hub-fwd] lora cabinet key missing", flush=True)
            return
        if self.zones is not None and not self.zones.node_allowed(node_id):
            return
        # Спектрограммы решает микрофон: если пакет пришёл, Hub его пробрасывает.
        kind = ""
        if "ReportDetection" in path:
            kind = "det"
        elif "ReportHeartbeat" in path:
            kind = "hb"
        elif "UploadMel" in path:
            kind = "mel"
        if kind and self.adsb is not None:
            try:
                if self.adsb.should_suppress_forward(
                    kind,
                    node_id=node_id,
                    lat=lat,
                    lon=lon,
                    timestamp_ms=timestamp_ms,
                ):
                    return
            except Exception as e:  # noqa: BLE001
                # Fail-open, но не молчим — иначе фильтр «ломается тихо».
                print(f"[hub-fwd] adsb filter error ({kind}): {e}", flush=True)
        store_path = f"{path}#lora" if path_lora else path
        self.store.enqueue_forward(
            store_path,
            body,
            max_depth=self.queue_max,
            bearer=bearer,
            node_id=node_id or "",
        )
        self._ensure_worker()

    def _ensure_worker(self) -> None:
        with self._start_lock:
            self._start_unlocked()

    def start(self) -> None:
        with self._start_lock:
            self._start_unlocked()

    def _note_origin_check(self) -> None:
        """Адрес облака не стираем: проверка могла сбойнуть на секунду."""
        if not self.base:
            self._origin_block = ""
            return
        try:
            assert_forward_url_allowed(self.base)
            self._origin_block = ""
        except ValueError as e:
            self._origin_block = str(e)
            print(f"[hub-fwd] origin kept, send will retry: {e}", flush=True)

    def _log_fwd(self, msg: str) -> None:
        now = time.time()
        if now - self._last_err_log < 60.0:
            return
        self._last_err_log = now
        print(msg, flush=True)

    def _start_unlocked(self) -> None:
        if not self.enabled:
            return
        if self._thread is not None and self._thread.is_alive():
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._loop, daemon=True, name="hub-fwd")
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        t = self._thread
        if t is not None and t.is_alive():
            t.join(timeout=5.0)
        self._thread = None

    def _ssl_ctx(self) -> ssl.SSLContext | None:
        if not self.ca_file:
            return ssl.create_default_context()
        ctx = ssl.create_default_context()
        ctx.load_verify_locations(self.ca_file)
        return ctx

    def _post(self, path: str, body: bytes, bearer: str = "") -> None:
        with self._lock:
            base = self.base
            ca = self.ca_file
            uplink = self._uplink_token()
        wire_path, path_lora = (
            (path[:-5], True) if path.endswith("#lora") else (path, False)
        )
        # LoRa: сохранённый ключ или ключ свежей прошивки. DIY/Light: Bearer прибора,
        # пустой — тот же ключ (самотест). На 401/403 другой ключ не пробуем.
        token = uplink if path_lora else ((bearer or "").strip() or uplink)
        if path_lora and not token:
            raise ValueError("cabinet_key_missing")
        try:
            assert_forward_url_allowed(base)
        except ValueError as e:
            self._origin_block = str(e)
            raise
        self._origin_block = ""
        url = f"{base}{wire_path}"
        if ca:
            ctx = ssl.create_default_context()
            ctx.load_verify_locations(ca)
        else:
            ctx = ssl.create_default_context()

        def _once(tok: str) -> None:
            req = urllib.request.Request(
                url,
                data=body,
                method="POST",
                headers=forward_headers(tok, path_lora=path_lora),
            )
            with urllib.request.urlopen(req, timeout=20, context=ctx) as r:
                if r.status >= 300:
                    raise RuntimeError(f"http_{r.status}")

        _once(token)

    def _loop(self) -> None:
        while not self._stop.is_set():
            try:
                items = self.store.dequeue_due(limit=10)
                if not items:
                    time.sleep(2.0)
                    continue
                for it in items:
                    try:
                        self._post(it["path"], it["body"], str(it.get("bearer") or ""))
                        self.store.forward_ok(it["id"])
                        self.last_ok_at = time.time()
                    except Exception as e:  # noqa: BLE001
                        created = float(it.get("created_at") or time.time())
                        if forward_should_abandon(e, age_s=time.time() - created):
                            self.store.abandon_forward(it["id"], str(e))
                            self._log_fwd(f"[hub-fwd] abandoned id={it['id']}: {e}")
                            continue
                        backoff = min(300.0, 15.0 * (2 ** min(int(it.get("attempts") or 0), 4)))
                        self.store.forward_fail(it["id"], str(e), backoff_s=backoff)
                        self._log_fwd(f"[hub-fwd] retry id={it['id']}: {e}")
            except Exception:  # noqa: BLE001
                time.sleep(5.0)
