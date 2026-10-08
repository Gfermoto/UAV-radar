#!/usr/bin/env python3
"""
NEVOD Hub — on-prem site ingest для DIY/Light (бывший Mock Cloud).

Принимает и валидирует все пути DIY → Cloud:
  POST /api/v1/pb/ReportDetection   (protobuf DetectionFrame)
  POST /api/v1/pb/ReportHeartbeat   (protobuf Heartbeat)
  POST /api/v1/pb/UploadMel         (protobuf MelFrame uint8)
  GET  /api/ingest/health           (probe)
  POST /api/ingest                  (Bearer; FOG JSON track/det/hb/alert — lab Cloud)
  GET  /api/v1/detections           (Bearer; cloud_pull list from Hub nodes)
  GET  /api/hub/detections          (cookie или Bearer; sqlite facts ?node_id&since_ms&until_ms)
  POST /nodes/*                    → 410 Gone (legacy float/JSON removed)
  GET  /ca.crt                      (PEM серверного/self-signed CA для DIY tls_ca)
  GET  /                            (login HTML без cookie; панель с cookie)
  GET  /api/hub/ui/status           (public; {configured})
  POST /api/hub/ui/setup|login|logout|password
  (POST /api/hub/ui/reset — отключён; сброс только SSH на VPS)
  GET  /status.json | /api/dashboard (cookie или Bearer)
  GET  /api/nodes/{id}
  POST /api/lab/reset               (Bearer; чистит RAM + mel_*.bin)
  POST /api/mel/clear               (Bearer; только Mel на диске)
  GET  /api/mel/{stem}.wav          (cookie/Bearer; Fast Griffin-Lim, кэш рядом с .bin)
  GET  /api/mel/archive.zip         (cookie/Bearer; bin+PNG; ?from=&to= YYYY-MM-DD MSK)
  GET  /api/mel/archive-gt.zip      (cookie/Bearer; размеченные Mel+WAV; те же даты)
  GET  /api/mel/corrections.zip     (cookie/Bearer; только gt_label ≠ класс приёма)
  POST /api/mel/{file}/gt           (cookie/Bearer; {gt_label} operator GT)
  DELETE /api/mel/{file}            (Bearer; один mel_*.bin + .meta.json + .wav)
  POST /api/mel/delete              (Bearer; JSON {"file":"mel_….bin"})
  GET  /api/mqtt                   (cookie или Bearer; cfg без пароля)
  POST /api/mqtt                   (cookie или Bearer)
  POST /api/mqtt/disconnect        (cookie или Bearer)
  GET  /api/hub/cloud              (cookie или Bearer; origin только с X-Hub-Unlock)
  POST /api/hub/cloud/token        (cookie или Bearer)
  POST /api/hub/cloud/unlock/set   (viewer; задать engineer unlock)
  POST /api/hub/cloud/unlock       (viewer; открыть URL облака → session token)
  POST /api/hub/cloud/origin       (Bearer + X-Hub-Unlock; только URL Cloud)
  GET/POST /api/hub/webhooks       (cookie или Bearer)
  GET/POST /api/hub/adsb           (cookie или Bearer)
  GET/POST /api/hub/zones          (cookie или Bearer)
  POST /api/hub/nodes/{id}/label   (cookie или Bearer)
  GET  /api/hub/meteo              (viewer; Open-Meteo / met.no cache for map weather)
  GET  /api/hub/dem                (viewer; elevation ring Open-Meteo / OpenTopoData)
  GET/POST /api/hub/map_wx         (viewer; Open-Meteo key + Ecowitt share URL)
  POST /api/hub/overpass           (viewer; thin Overpass proxy for map acoustic layers)

Зависимости: stdlib (+ openssl для --https; optional paho-mqtt для зеркала MQTT).

Пример (LAN, HTTPS — как требует WebUI ingest_url):
  python3 iot/tools/nevod_hub.py --bind 0.0.0.0 --port 9443 --https \\
      --token "$(openssl rand -hex 32)" --print-diy-config

Self-test без платы:
  python3 iot/tools/nevod_hub.py --self-test
"""
from __future__ import annotations

from hub_store import HubStore, normalize_node_label
from hub_webhooks import WebhookHub, webhook_url_allowed
from hub_forward import HubForwarder, factory_cloud_token, looks_pro
from hub_zones import ZoneRegistry
from adsb.service import AdsbService
from hub_overpass import proxy_overpass
from hub_dem import attach_node_tips, proxy_dem
from hub_meteo import proxy_meteo, set_api_key_provider
from hub_ecowitt_share import (
    clear_cache as clear_ecowitt_cache,
    parse_share_url,
    set_radius_km_provider,
    set_share_url_provider,
)
from adsb.ingest import assert_adsb_url_allowed
from hub_context import HubContext

import hub_auth
import hub_mel_io
import hub_mel_invert
import hub_mel_gt
from hub_pb import (
    MIN_UNIX_TS_MS,
    NODE_ID_RE_MAX,
    _canon_node_id,
    _finite,
    _finite_prob,
    _last_bytes,
    _last_f32,
    _last_f64,
    _last_str,
    _last_varint,
    _require_wall_clock_ms,
    pb_decode,
    pb_encode,
    validate_detection_pb,
    validate_heartbeat_pb,
    validate_mel_pb,
)
from hub_mel_io import (
    MEL_BANDS_EXPECTED,
    MEL_DATA_MAX,
    MEL_FMAX,
    MEL_FMIN,
    MEL_FRAMES_MAX,
    MEL_MAX_FILES,
    MEL_SKIP_NODES,
    clear_saved_mel,
    decode_mel_uint8,
    delete_saved_mel,
    is_emulated_mel_source,
    list_saved_mel,
    mel_analytics_float32,
    mel_analytics_uint8,
    build_mel_archive_zip,
    save_mel_payload,
    parse_mel_date_range,
    MelDateError,
    mel_archive_download_name,
)
from hub_events import (
    VIA_LABELS,
    _event_meta,
    _event_transport_kind,
    build_event_summary,
    enrich_event,
)
import hub_episodes
from hub_episodes import EpisodeRegistry, apply_detection, apply_mel, episode_event_dict, find_event_index
from hub_fog_bridge import apply_fog_ingest, list_detections_for_pull, validate_fog_ingest
from hub_mqtt_parse import (
    JOIN_HB_MAX_AGE_MS,
    _apply_firmware_version,
    _apply_nn_model_version,
    _apply_xvf_firmware_version,
    _extract_fw_version,
    _json_obj,
    _mqtt_node_id,
    _mqtt_ts_ms,
    _parse_mqtt_detection,
    _parse_mqtt_heartbeat,
    join_detection_with_hb,
    mqtt_is_lora,
    lora_mqtt_pb,
)
from hub_auth import (
    HUB_SETTINGS_SESSIONS,
    HUB_UI_SESSIONS,
    HUB_UNLOCK_HARDCODED_PW,
    HUB_UNLOCK_SESSIONS,
    HUB_UNLOCK_TTL_S,
    _bearer_ok,
    _env_truthy,
    _hash_unlock_pw,
    _ui_clear_cookie_header,
    _ui_configured,
    _ui_issue,
    _ui_session_drop,
    _ui_session_ok,
    _ui_set,
    _ui_set_cookie_header,
    _ui_token_from_cookie,
    _ui_verify,
    _unlock_hash_stored,
    _unlock_issue,
    _unlock_session_ok,
    _unlock_set,
    _validate_ui_password,
    _validate_ui_user,
)
import hub_redis
from hub_tls import (
    _cert_covers_names,
    _parse_cn_list,
    _san_entry,
    ensure_self_signed,
    guess_lan_ip,
    print_diy_config,
)

import argparse
import hashlib
import hmac
import json
import math
import os
import re
import secrets
import smtplib
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
import uuid
import urllib.request
from dataclasses import dataclass, field
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.parse import parse_qs, urlparse
from urllib.request import Request, urlopen

# ── Константы контракта DIY (nevod_esp.proto / Config.h) ─────────────────────

# UI /api: кликабельная версия (5× → скрытое Cloud-меню), как IoT sysVer.
HUB_VERSION = "0.2.4"

# Mel paths/flags live in hub_mel_io (mutable; self-test rebinds via module).
MEL_DIR = hub_mel_io.MEL_DIR
MEL_SAVE = hub_mel_io.MEL_SAVE

INGEST_TIMELINE_WINDOW_S = 24 * 3600
INGEST_TIMELINE_BUCKET_S = 300
MQTT_TOPICS_DEFAULT = (
    "nevod/+/detection",
    "nevod/+/heartbeat",
    "msh/+/2/e/#",
)
# Human-readable checklist for lab UI (Cloud contract PB-only)
CHECK_LABELS: dict[str, str] = {
    "pb_detection": "Protobuf · детекция (событие)",
    "pb_heartbeat": "Protobuf · пульс узла (HB)",
    "pb_mel": "Protobuf · спектрограмма Mel",
    "mqtt_v1": "MQTT v1 · detection|heartbeat",
}
CHECK_HINTS: dict[str, str] = {
    "pb_detection": "На плате: Cloud ON + детекция. Event-only PB (без GPS/SPL).",
    "pb_heartbeat": "HB ~45с: status, SPL, noise, GPS, uptime (PB).",
    "pb_mel": "mel_upload≠0; ≥120с; mute при открытом WebUI.",
    "mqtt_v1": "На Mock: форма MQTT → host/port существующего брокера; на плате mqtt_enabled.",
}
MAX_BODY = 5 * 1024 * 1024


# ── Validation + analytics ───────────────────────────────────────────────────

from hub_labels import apply_node_label, attach_node_labels, parse_hub_node_label_path
from hub_runtime import (
    CheckItem,
    NodeRecord,
    State,
    _merge_node_into,
    _now_iso,
    bind_services,
    build_ingest_histograms,
    build_ingest_timeline,
    build_ingest_timelines_by_node,
)

# ── HTTP server ──────────────────────────────────────────────────────────────

STATE: State | None = None
MQTT: "MqttMirror | None" = None
CERT_PATH: Path | None = None
STORE: HubStore | None = None
WEBHOOKS: WebhookHub | None = None
FORWARDER: HubForwarder | None = None
ADSB: AdsbService | None = None
ZONES: ZoneRegistry | None = None
DEVICE_TOKENS: dict[str, str] = {}
REQUIRE_DEVICE_TOKEN = os.environ.get("HUB_REQUIRE_DEVICE_TOKEN", "0").lower() in (
    "1",
    "true",
    "yes",
)
# HUB_DASHBOARD_OPEN ignored for viewer/HTML (UI login). Env kept so old .env does not crash.
DASHBOARD_OPEN = os.environ.get("HUB_DASHBOARD_OPEN", "0").lower() in (
    "1",
    "true",
    "yes",
)
# Engineer unlock / settings sessions: hub_auth.HUB_*_SESSIONS (imported).
_UI_DIR = Path(__file__).resolve().parent
_HUB_UI_DIR = _UI_DIR / "hub_ui"


def _gps_ctx_for_node(node_id: str | None) -> tuple[float | None, float | None, int | None]:
    """Last HB lat/lon/timestamp_ms for ADS-B GPS override (DET/Mel inherit HB)."""
    if STATE is None or not node_id:
        return None, None, None
    nid = _canon_node_id(node_id) or str(node_id).strip()
    node = STATE.nodes.get(nid)
    if node is None:
        return None, None, None
    hb = node.last_heartbeat or {}
    lat = hb.get("lat")
    lon = hb.get("lon")
    ts = hb.get("timestamp_ms")
    try:
        lat_f = float(lat) if lat is not None else None
        lon_f = float(lon) if lon is not None else None
    except (TypeError, ValueError):
        lat_f, lon_f = None, None
    try:
        ts_i = int(ts) if ts is not None else None
    except (TypeError, ValueError):
        ts_i = None
    return lat_f, lon_f, ts_i


def _pb_is_pro(headers: Any, node_id: str | None) -> bool:
    blobs: list[Any] = []
    if headers is not None:
        blobs.append(headers.get("X-Nevod-Sku"))
        blobs.append(headers.get("X-Nevod-Device-Type"))
    if STATE is not None and node_id:
        node = STATE.nodes.get(str(node_id))
        if node is not None:
            blobs.append(node.last_heartbeat)
            blobs.append(node.last_detection)
    return looks_pro(*blobs)


def _presented_bearer(header: str | None) -> str:
    raw = (header or "").strip()
    if raw.lower().startswith("bearer "):
        return raw[7:].strip()
    return raw


def _forward_enqueue(path: str, body: bytes, meta: dict[str, Any] | None = None) -> None:
    if FORWARDER is None:
        return
    meta = meta or {}
    nid = meta.get("node_id")
    lat = meta.get("lat")
    lon = meta.get("lon")
    ts = meta.get("timestamp_ms")
    if lat is None or lon is None:
        glat, glon, gts = _gps_ctx_for_node(str(nid) if nid else None)
        if lat is None:
            lat = glat
        if lon is None:
            lon = glon
        if ts is None:
            ts = gts
    try:
        lat_f = float(lat) if lat is not None else None
    except (TypeError, ValueError):
        lat_f = None
    try:
        lon_f = float(lon) if lon is not None else None
    except (TypeError, ValueError):
        lon_f = None
    try:
        ts_i = int(ts) if ts is not None else None
    except (TypeError, ValueError):
        ts_i = None
    if meta.get("path_lora"):
        print(f"[hub-fwd] lora enqueue {path} node={nid}", flush=True)
    FORWARDER.maybe_enqueue(
        path,
        body,
        node_id=str(nid) if nid else None,
        lat=lat_f,
        lon=lon_f,
        timestamp_ms=ts_i,
        path_lora=bool(meta.get("path_lora")),
        bearer=str(meta.get("bearer") or ""),
    )
_FAVICON_SVG = _UI_DIR / "hub_favicon.svg"
_FAVICON_ICO = _UI_DIR / "hub_favicon.ico"
_HUB_UI_TYPES = {
    ".css": "text/css; charset=utf-8",
    ".js": "application/javascript; charset=utf-8",
    ".json": "application/json; charset=utf-8",
}


def _load_login_html() -> bytes:
    path = _UI_DIR / "hub_login.html"
    if not path.is_file():
        return (
            b"<!doctype html><meta charset=utf-8><title>Hub login</title>"
            b"<p>missing hub_login.html</p>"
        )
    raw = path.read_text(encoding="utf-8")
    v = HUB_VERSION
    raw = raw.replace('/hub_ui/hub.css"', f'/hub_ui/hub.css?v={v}"')
    configured = _ui_configured()
    raw = raw.replace(
        "window.__HUB_UI_CONFIGURED__=null",
        f"window.__HUB_UI_CONFIGURED__={'true' if configured else 'false'}",
    )
    mode_cls = "login-mode-login" if configured else "login-mode-setup"
    raw = raw.replace('class="login-page login-mode-login"', f'class="login-page {mode_cls}"')
    return raw.encode("utf-8")


def _load_dashboard_html() -> bytes:
    path = _UI_DIR / "hub_diy_ui.html"
    if not path.is_file():
        return (
            b"<!doctype html><meta charset=utf-8><title>hub</title>"
            b"<p>missing hub_diy_ui.html - see /status.json</p>"
        )
    raw = path.read_text(encoding="utf-8")
    v = HUB_VERSION
    # Cache-bust static assets after deploy.
    for name in (
        "hub.css",
        "hub_util.js",
        "hub_mel.js",
        "hub_render.js",
        "hub_zones_ui.js",
        "hub_settings.js",
        "hub_map.js",
        "hub_map_acoustic.js",
        "hub_app.js",
    ):
        raw = raw.replace(f'/hub_ui/{name}"', f'/hub_ui/{name}?v={v}"')
    return raw.encode("utf-8")


def _resolve_hub_ui_file(url_path: str) -> Path | None:
    """Map /hub_ui/<name> → file under hub_ui/; reject traversal."""
    if not url_path.startswith("/hub_ui/"):
        return None
    rel = url_path[len("/hub_ui/") :]
    if not rel or "/" in rel or "\\" in rel or rel in (".", "..") or ".." in rel:
        return None
    name = Path(rel).name
    if name != rel:
        return None
    suffix = Path(name).suffix.lower()
    if suffix not in _HUB_UI_TYPES:
        return None
    root = _HUB_UI_DIR.resolve()
    candidate = (root / name).resolve()
    try:
        candidate.relative_to(root)
    except ValueError:
        return None
    if not candidate.is_file():
        return None
    return candidate


from hub_http_auth_ui import HubAuthUiHttp
from hub_http_mel import HubMelHttp
from hub_http_admin import HubAdminHttp
from hub_http_mqtt import HubMqttHttp
from hub_http_pb import HubPbHttp


class Handler(
    HubAuthUiHttp,
    HubMelHttp,
    HubAdminHttp,
    HubMqttHttp,
    HubPbHttp,
    BaseHTTPRequestHandler,
):

    server_version = "NevodHub/1.0"

    @property
    def ctx(self) -> HubContext:
        """Service bundle from the HTTP server; falls back to module globals."""
        server = getattr(self, "server", None)
        server_ctx = getattr(server, "ctx", None) if server is not None else None
        if isinstance(server_ctx, HubContext):
            return server_ctx
        # Self-test / legacy: globals are source of truth until fully migrated.
        return HubContext.from_module(sys.modules[__name__])

    def log_message(self, fmt: str, *args: Any) -> None:
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def _json(
        self,
        code: int,
        obj: dict[str, Any],
        *,
        set_cookie: str | None = None,
        cors: bool = True,
    ) -> None:
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        if cors:
            self.send_header("Access-Control-Allow-Origin", "*")
        if set_cookie:
            self.send_header("Set-Cookie", set_cookie)
        self.end_headers()
        self.wfile.write(body)

    def _bytes(self, code: int, body: bytes, content_type: str) -> None:
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _read_body(self) -> bytes | None:
        raw_len = self.headers.get("Content-Length")
        if raw_len is None or raw_len == "":
            return b""
        try:
            length = int(raw_len)
        except ValueError:
            self._json(400, {"error": "bad_content_length", "accepted": False})
            return None
        if length < 0 or length > MAX_BODY:
            self._json(413, {"error": "payload_too_large", "accepted": False})
            return None
        return self.rfile.read(length)

    def _auth(self, *, node_id: str | None = None, allow_factory: bool = False) -> bool:
        assert STATE is not None
        hdr = self.headers.get("Authorization")
        if _bearer_ok(hdr, STATE.token):
            return True
        # Пакеты: свой ключ, заводской и ключ, уже заданный на Hub.
        # Настройки панели сюда не входят.
        if allow_factory:
            extra = [factory_cloud_token(), (os.environ.get("HUB_FORWARD_TOKEN") or "").strip()]
            if FORWARDER is not None:
                extra.append((FORWARDER.token or "").strip())
            for tok in extra:
                if tok and _bearer_ok(hdr, tok):
                    return True
        if node_id and node_id in DEVICE_TOKENS and _bearer_ok(hdr, DEVICE_TOKENS[node_id]):
            return True
        if REQUIRE_DEVICE_TOKEN and node_id:
            self._json(401, {"error": "unauthorized_device", "accepted": False})
            return False
        self._json(401, {"error": "unauthorized", "accepted": False})
        return False

    def _auth_viewer(self) -> bool:
        """GET dashboard / mel / zones: UI cookie or Bearer. Not DASHBOARD_OPEN."""
        tok = _ui_token_from_cookie(self.headers.get("Cookie"))
        if tok and _ui_session_ok(tok):
            return True
        return self._auth()

    def _auth_ui_cookie(self) -> bool:
        tok = _ui_token_from_cookie(self.headers.get("Cookie"))
        return bool(tok and _ui_session_ok(tok))

    def _auth_lan_or_bearer(self) -> bool:
        """Compat alias: cookie or Bearer (DASHBOARD_OPEN does not open APIs)."""
        return self._auth_viewer()

    def _auth_settings_write(self) -> bool:
        """Viewer (cookie or Bearer) + engineer unlock."""
        if not self._auth_viewer():
            return False
        if not _unlock_session_ok(self.headers.get("X-Hub-Unlock")):
            self._json(403, {"error": "cloud_locked"})
            return False
        return True

    def _ingest_token_matches(self, provided: str) -> bool:
        assert STATE is not None
        t = (provided or "").strip()
        if t.startswith("Bearer "):
            t = t[7:].strip()
        expected = STATE.token or ""
        if not expected or not t or len(t) != len(expected):
            return False
        return hmac.compare_digest(t, expected)

    def do_OPTIONS(self) -> None:  # noqa: N802
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header(
            "Access-Control-Allow-Methods", "GET, HEAD, POST, DELETE, OPTIONS"
        )
        self.send_header(
            "Access-Control-Allow-Headers",
            "Authorization, Content-Type, X-Nevod-Source, X-Node-Id, "
            "X-Mel-Bands, X-Mel-Frames, X-Mel-Fmin, X-Mel-Fmax, "
            "X-Hub-Unlock, X-Hub-Settings",
        )
        self.end_headers()

    def _mel_date_range(self) -> tuple[int | None, int | None, str | None, str | None] | None:
        """Parse from/to query. None = already sent 400."""
        qs = parse_qs(urlparse(self.path).query)
        from_s = (qs.get("from") or [""])[0].strip() or None
        to_s = (qs.get("to") or [""])[0].strip() or None
        try:
            start_ms, end_ms = parse_mel_date_range(from_s, to_s)
        except MelDateError as exc:
            self._json(400, {"error": str(exc)})
            return None
        return start_ms, end_ms, from_s, to_s

    def _send_mel_archive(self, head_only: bool = False, *, labeled: bool = False) -> None:
        parsed = self._mel_date_range()
        if parsed is None:
            return
        start_ms, end_ms, from_s, to_s = parsed
        try:
            if labeled:
                data = hub_mel_gt.build_mel_gt_archive_zip(start_ms=start_ms, end_ms=end_ms)
            else:
                data = build_mel_archive_zip(start_ms=start_ms, end_ms=end_ms)
        except hub_mel_gt.MelGtError as exc:
            code = 400 if str(exc) == "no_gt" else 500
            body: dict[str, Any] = {"error": str(exc)}
            fname = getattr(exc, "file", None)
            if fname:
                body["file"] = fname
            self._json(code, body)
            return
        except OSError as exc:
            self._json(500, {"error": "mel_zip_failed", "detail": str(exc)})
            return
        kind = "MEL-gt" if labeled else "MEL"
        dl = mel_archive_download_name(kind, from_s, to_s)
        self.send_response(200)
        self.send_header("Content-Type", "application/zip")
        self.send_header("Content-Length", str(len(data)))
        self.send_header(
            "Content-Disposition",
            f'attachment; filename="{dl}"',
        )
        self.send_header("Cache-Control", "no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        if not head_only:
            self.wfile.write(data)

    def _send_mel_corrections(self, head_only: bool = False) -> None:
        """On-demand zip. Не удаляет файлы, не пишет class_name, не зовёт тренер."""
        try:
            facts: dict[str, Any] = {}
            if STORE is not None and MEL_DIR.is_dir():
                names = [p.name for p in MEL_DIR.glob("mel_*.bin") if p.is_file()]
                facts = STORE.facts_by_mel_names(names)
            data = hub_mel_gt.build_mel_corrections_zip(facts_by_name=facts)
        except OSError as exc:
            self._json(500, {"error": "mel_zip_failed", "detail": str(exc)})
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/zip")
        self.send_header("Content-Length", str(len(data)))
        self.send_header(
            "Content-Disposition",
            'attachment; filename="MEL-corrections.zip"',
        )
        self.send_header("Cache-Control", "no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        if not head_only:
            self.wfile.write(data)

    def do_HEAD(self) -> None:  # noqa: N802
        """Browsers/прокси иногда делают HEAD перед download — раньше был 501."""
        assert STATE is not None
        path = self.path.split("?", 1)[0]
        if path in (
            "/api/mel/archive.zip",
            "/api/mel/archive",
            "/api/mel/archive-gt.zip",
            "/api/mel/archive-gt",
            "/api/mel/corrections.zip",
            "/api/mel/corrections",
        ):
            if not self._auth_viewer():
                return
            if "corrections" in path:
                self._send_mel_corrections(head_only=True)
                return
            labeled = "archive-gt" in path
            self._send_mel_archive(head_only=True, labeled=labeled)
            return
        if path in ("/ca.crt", "/api/ca.crt"):
            if CERT_PATH is None or not CERT_PATH.is_file():
                self.send_response(404)
                self.end_headers()
                return
            data = CERT_PATH.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "application/x-pem-file")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Content-Disposition", 'attachment; filename="ca.crt"')
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            return
        if path in (
            "/api/ingest/health",
            "/health",
        ):
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            return
        if path in (
            "/status.json",
            "/api/dashboard",
            "/api/v1/dashboard",
            "/api/mel",
            "/api/mel/",
            "/api/mqtt",
            "/api/mqtt/",
        ):
            if not self._auth_viewer():
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            return
        self.send_response(404)
        self.end_headers()

    def do_GET(self) -> None:  # noqa: N802
        assert STATE is not None
        path = self.path.split("?", 1)[0]
        if path.startswith("/hub_ui/"):
            fpath = _resolve_hub_ui_file(path)
            if fpath is None:
                self._json(404, {"error": "not_found"})
                return
            data = fpath.read_bytes()
            ctype = _HUB_UI_TYPES.get(fpath.suffix.lower(), "application/octet-stream")
            self._bytes(200, data, ctype)
            return
        if path in ("/api/ingest/health", "/health"):
            auth = self.headers.get("Authorization")
            if auth:
                if not _bearer_ok(auth, STATE.token):
                    self._json(401, {"error": "unauthorized", "accepted": False})
                    return
            body: dict[str, Any] = {"status": "ok", "service": "nevod_hub"}
            if auth and _bearer_ok(auth, STATE.token):
                body["version"] = HUB_VERSION
                body["redis"] = hub_redis.public_dict()
            self._json(200, body)
            return
        if path in ("/api/hub/ui/status", "/api/hub/ui/status/"):
            self._json(200, {"configured": _ui_configured()}, cors=False)
            return
        if path == "/api/v1/detections":
            if not self._auth():
                return
            qs = parse_qs(urlparse(self.path).query)
            try:
                since_ms = int((qs.get("since_ms") or ["0"])[0] or 0)
            except ValueError:
                since_ms = 0
            try:
                limit = int((qs.get("limit") or ["100"])[0] or 100)
            except ValueError:
                limit = 100
            with STATE.lock:
                dets = list_detections_for_pull(STATE.nodes, since_ms=since_ms, limit=limit)
            self._json(200, {"detections": dets, "count": len(dets)})
            return
        if path in ("/api/hub/detections", "/api/hub/detections/"):
            if not self._auth_viewer():
                return
            if STORE is None:
                self._json(503, {"error": "store_unavailable"})
                return
            qs = parse_qs(urlparse(self.path).query)
            raw_nid = (qs.get("node_id") or [""])[0]
            nid = _canon_node_id(raw_nid) if raw_nid else ""
            if raw_nid and not nid:
                self._json(400, {"error": "invalid_node_id"})
                return
            try:
                since_ms = int((qs.get("since_ms") or ["0"])[0] or 0)
            except ValueError:
                self._json(400, {"error": "bad_since_ms"})
                return
            until_raw = (qs.get("until_ms") or [""])[0]
            until_ms: int | None
            if until_raw == "":
                until_ms = None
            else:
                try:
                    until_ms = int(until_raw)
                except ValueError:
                    self._json(400, {"error": "bad_until_ms"})
                    return
            try:
                limit = int((qs.get("limit") or ["500"])[0] or 500)
            except ValueError:
                limit = 500
            rows = STORE.list_detections(
                node_id=nid, since_ms=since_ms, until_ms=until_ms, limit=limit
            )
            self._json(
                200,
                {
                    "detections": rows,
                    "count": len(rows),
                    "node_id": nid,
                    "since_ms": since_ms,
                    "until_ms": until_ms,
                },
            )
            return
        if path in ("/ca.crt", "/api/ca.crt"):
            if CERT_PATH is None or not CERT_PATH.is_file():
                self._json(404, {"error": "ca_unavailable"})
                return
            data = CERT_PATH.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "application/x-pem-file")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Content-Disposition", 'attachment; filename="ca.crt"')
            self.send_header("Cache-Control", "no-store")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(data)
            return
        if path in ("/api/hub/meteo", "/api/hub/meteo/"):
            if not self._auth_viewer():
                return
            qs = parse_qs(urlparse(self.path).query)
            try:
                lat = float((qs.get("lat") or [""])[0])
                lon = float((qs.get("lon") or [""])[0])
            except (TypeError, ValueError):
                self._json(400, {"error": "bad_coord"})
                return
            try:
                sample_m = float((qs.get("sample_m") or ["300"])[0] or 300)
            except (TypeError, ValueError):
                sample_m = 300.0
            code, payload = proxy_meteo(lat, lon, sample_m)
            self._json(code, payload)
            return
        if path in ("/api/hub/dem", "/api/hub/dem/"):
            if not self._auth_viewer():
                return
            qs = parse_qs(urlparse(self.path).query)
            try:
                lat = float((qs.get("lat") or [""])[0])
                lon = float((qs.get("lon") or [""])[0])
            except (TypeError, ValueError):
                self._json(400, {"error": "bad_coord"})
                return
            try:
                sample_m = float((qs.get("sample_m") or ["500"])[0] or 500)
            except (TypeError, ValueError):
                sample_m = 500.0
            code, payload = proxy_dem(lat, lon, sample_m)
            self._json(code, payload)
            return
        if path in ("/api/hub/map_wx", "/api/hub/map_wx/"):
            if not self._auth_viewer():
                return
            key = ""
            eco_url = ""
            eco_r = 8.0
            if STORE is not None:
                key = str(STORE.get_meta("open_meteo_api_key") or "").strip()
                eco_url = str(STORE.get_meta("ecowitt_share_url") or "").strip()
                try:
                    eco_r = float(STORE.get_meta("ecowitt_radius_km") or 8.0)
                except (TypeError, ValueError):
                    eco_r = 8.0
            if not key:
                key = str((__import__("os").environ.get("OPEN_METEO_API_KEY") or "")).strip()
            eco_ok = parse_share_url(eco_url) is not None
            self._json(
                200,
                {
                    "status": "ok",
                    "configured": bool(key),
                    "key_hint": (("…" + key[-4:]) if len(key) >= 4 else ("set" if key else "")),
                    "via": "settings" if (STORE and STORE.get_meta("open_meteo_api_key")) else ("env" if key else "none"),
                    "ecowitt_share_url": eco_url if eco_ok else "",
                    "ecowitt_configured": eco_ok,
                    "ecowitt_radius_km": eco_r,
                },
            )
            return
        if path in ("/status.json", "/api/dashboard", "/api/v1/dashboard"):
            if not self._auth_viewer():
                return
            self._json(200, STATE.snapshot())
            return
        if path in ("/api/mel", "/api/mel/"):
            if not self._auth_viewer():
                return
            parsed = self._mel_date_range()
            if parsed is None:
                return
            start_ms, end_ms, from_s, to_s = parsed
            qs = parse_qs(urlparse(self.path).query)
            node_s = (qs.get("node") or [""])[0].strip() or None
            sort_s = (qs.get("sort") or ["date"])[0].strip() or "date"
            ranged = from_s is not None or to_s is not None or bool(node_s)
            lim = MEL_MAX_FILES if ranged else 100
            self._json(
                200,
                {
                    "files": list_saved_mel(
                        limit=lim,
                        start_ms=start_ms,
                        end_ms=end_ms,
                        node_id=node_s,
                        sort=sort_s,
                    ),
                    "from": from_s,
                    "to": to_s,
                    "node": node_s,
                    "sort": sort_s,
                },
            )
            return
        if path in ("/api/mel/archive.zip", "/api/mel/archive"):
            if not self._auth_viewer():
                return
            self._send_mel_archive(head_only=False, labeled=False)
            return
        if path in ("/api/mel/archive-gt.zip", "/api/mel/archive-gt"):
            if not self._auth_viewer():
                return
            self._send_mel_archive(head_only=False, labeled=True)
            return
        if path in ("/api/mel/corrections.zip", "/api/mel/corrections"):
            if not self._auth_viewer():
                return
            self._send_mel_corrections(head_only=False)
            return
        if path.startswith("/api/mel/"):
            if not self._auth_viewer():
                return
            fname = path[len("/api/mel/") :]
            if "/" in fname or ".." in fname or not (
                fname.startswith("mel_") or fname.endswith(".meta.json")
            ):
                self._json(400, {"error": "bad_filename"})
                return
            if fname.endswith(".wav"):
                bin_name = fname[: -len(".wav")] + ".bin"
                if not bin_name.startswith("mel_") or not bin_name.endswith(".bin"):
                    self._json(400, {"error": "bad_filename"})
                    return
                try:
                    data = hub_mel_invert.ensure_mel_wav(MEL_DIR / bin_name)
                except FileNotFoundError:
                    self._json(404, {"error": "not_found"})
                    return
                except hub_mel_invert.MelInvertError as exc:
                    self._json(400, {"error": str(exc)})
                    return
                except Exception as exc:
                    print(f"[hub] mel wav invert {bin_name}: {exc}", flush=True)
                    self._json(500, {"error": "invert_failed"})
                    return
                self.send_response(200)
                self.send_header("Content-Type", "audio/wav")
                self.send_header("Content-Length", str(len(data)))
                self.send_header(
                    "Content-Disposition", f'inline; filename="{fname}"'
                )
                self.send_header("Cache-Control", "no-store")
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(data)
                return
            fpath = MEL_DIR / fname
            if not fpath.is_file():
                self._json(404, {"error": "not_found"})
                return
            data = fpath.read_bytes()
            if fname.endswith(".meta.json") or fname.endswith(".json"):
                self.send_response(200)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Cache-Control", "no-store")
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(data)
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Content-Disposition", f'attachment; filename="{fname}"')
            self.send_header("Cache-Control", "no-store")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(data)
            return
        if path in ("/api/hub", "/api/hub/"):
            if not self._auth_viewer():
                return
            unlocked = _unlock_session_ok(self.headers.get("X-Hub-Unlock"))
            self._json(200, {
                "service": "nevod_hub",
                "version": HUB_VERSION,
                "webhooks": WEBHOOKS.public_config() if WEBHOOKS else {},
                "forward": FORWARDER.public_dict(include_origin=unlocked) if FORWARDER else {},
                "engineer": {
                    "unlock_set": bool(_unlock_hash_stored()),
                    "unlocked": unlocked,
                },
                "webhook_deliveries": STORE.recent_webhooks(40) if STORE else [],
                "device_nodes": sorted(DEVICE_TOKENS.keys()),
            })
            return
        if path in ("/api/hub/cloud", "/api/hub/cloud/"):
            if not self._auth_viewer():
                return
            unlocked = _unlock_session_ok(self.headers.get("X-Hub-Unlock"))
            if FORWARDER is not None:
                FORWARDER.ensure_probed()
                fwd = FORWARDER.public_dict(include_origin=unlocked)
            else:
                fwd = {
                    "enabled": False,
                    "has_token": False,
                    "has_origin": False,
                    "token_source": "none",
                }
            self._json(200, {
                "status": "ok",
                "version": HUB_VERSION,
                "forward": fwd,
                "engineer": {
                    "unlock_set": bool(_unlock_hash_stored()),
                    "unlocked": unlocked,
                },
            })
            return
        if path in ("/api/hub/webhooks", "/api/hub/webhooks/"):
            if not self._auth_viewer():
                return
            unlocked = _unlock_session_ok(self.headers.get("X-Hub-Unlock"))
            self._json(
                200,
                {
                    "status": "ok",
                    "webhooks": WEBHOOKS.public_config() if WEBHOOKS else {},
                    "engineer": {
                        "unlock_set": bool(_unlock_hash_stored()),
                        "unlocked": unlocked,
                    },
                    "deliveries": STORE.recent_webhooks(40) if STORE else [],
                },
            )
            return
        if path in ("/api/hub/settings", "/api/hub/settings/"):
            self._json(410, {"error": "gone", "detail": "use_ui_login"})
            return
        if path.startswith("/api/nodes/"):
            if not self._auth_viewer():
                return
            nid = path[len("/api/nodes/") :]
            snap = STATE.snapshot()
            for n in snap["nodes"]:
                if n["node_id"] == nid:
                    self._json(200, n)
                    return
            self._json(404, {"error": "unknown_node"})
            return
        if path in ("/api/hub/adsb", "/api/hub/adsb/"):
            if not self._auth_viewer():
                return
            self._json(
                200,
                {
                    "status": "ok",
                    "adsb": ADSB.public_dict() if ADSB else {"enabled": False},
                },
            )
            return
        if path in ("/api/hub/zones", "/api/hub/zones/"):
            if not self._auth_viewer():
                return
            self._json(
                200,
                {
                    "status": "ok",
                    "zones": ZONES.public_dict() if ZONES else {},
                },
            )
            return
        if path in ("/api/mqtt", "/api/mqtt/"):
            if not self._auth_viewer():
                return
            assert MQTT is not None
            self._json(200, MQTT.public_dict())
            return
        if path == "/favicon.svg" and _FAVICON_SVG.is_file():
            data = _FAVICON_SVG.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "image/svg+xml")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "public, max-age=86400")
            self.end_headers()
            self.wfile.write(data)
            return
        if path in ("/favicon.ico", "/apple-touch-icon.png") and _FAVICON_ICO.is_file():
            data = _FAVICON_ICO.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "image/x-icon")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "public, max-age=86400")
            self.end_headers()
            self.wfile.write(data)
            return
        if path in ("/", "/index.html"):
            if self._auth_ui_cookie():
                self._bytes(200, _load_dashboard_html(), "text/html; charset=utf-8")
            else:
                self._bytes(200, _load_login_html(), "text/html; charset=utf-8")
            return
        self._json(404, {"error": "not_found"})

    def do_DELETE(self) -> None:  # noqa: N802
        assert STATE is not None
        path = self.path.split("?", 1)[0]
        if path.startswith("/api/mel/"):
            if not self._auth_viewer():
                return
            fname = path[len("/api/mel/") :]
            # Don't treat collection endpoints as filenames.
            if fname in (
                "",
                "clear",
                "delete",
                "archive.zip",
                "archive",
                "archive-gt.zip",
                "archive-gt",
                "corrections.zip",
                "corrections",
            ):
                self._json(405, {"error": "method_not_allowed"})
                return
            try:
                info = delete_saved_mel(fname)
                self._json(200, {"status": "ok", **info})
            except FileNotFoundError as e:
                self._json(404, {"error": "not_found", "file": str(e)})
            except ValueError as e:
                self._json(400, {"error": str(e)})
            except OSError as e:
                self._json(500, {"error": str(e)})
            return
        self._json(404, {"error": "not_found"})

    def do_POST(self) -> None:  # noqa: N802
        assert STATE is not None
        path = self.path.split("?", 1)[0]
        if path == "/api/lab/reset":
            if not self._auth_viewer():
                return
            info = STATE.reset(clear_mel=True)
            self._json(200, {"status": "ok", "reset": True, **info})
            return
        if self._handle_ui_login(path):
            return
        if path in ("/api/hub/overpass", "/api/hub/overpass/"):
            if not self._auth_viewer():
                return
            raw = self._read_body()
            if raw is None:
                return
            ql = ""
            ctype = (self.headers.get("Content-Type") or "").split(";")[0].strip().lower()
            if ctype == "application/json":
                try:
                    obj = json.loads(raw.decode("utf-8") or "{}")
                except json.JSONDecodeError:
                    self._json(400, {"error": "bad_json"})
                    return
                ql = str((obj or {}).get("data") or (obj or {}).get("q") or "")
            else:
                # form: data=<ql>
                from urllib.parse import parse_qs

                qs = parse_qs(raw.decode("utf-8", errors="replace"))
                ql = (qs.get("data") or [""])[0]
            code, payload = proxy_overpass(ql)
            self._json(code, payload if isinstance(payload, dict) else {"error": "bad_payload"})
            return
        if self._handle_mel_post(path):
            return
        if self._handle_hub_admin(path):
            return
        if self._handle_mqtt_post(path):
            return
        if path == "/api/ingest":
            if not self._auth():
                return
            raw = self._read_body()
            if raw is None:
                return
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError:
                self._json(400, {"error": "bad_json", "accepted": False})
                return
            schema, verr = validate_fog_ingest(payload)
            if verr or not schema:
                self._json(400, {"error": verr or "invalid", "accepted": False})
                return
            try:
                out = apply_fog_ingest(STATE.mark, payload, body_len=len(raw))
                self._json(200, out)
            except ValueError as e:
                self._json(400, {"error": str(e), "accepted": False})
            except Exception as e:  # noqa: BLE001
                sys.stderr.write(f"[hub] fog ingest: {e}\n")
                self._json(500, {"error": "internal_error", "accepted": False})
            return
        self._handle_pb_ingest(path)


def _rebind_http_mixin_globals() -> None:
    """Mixin methods were defined in hub_http_*; rebind their globals to this module."""
    import types

    for cls in (HubAuthUiHttp, HubMelHttp, HubAdminHttp, HubMqttHttp, HubPbHttp):
        for name, obj in list(cls.__dict__.items()):
            if isinstance(obj, types.FunctionType):
                setattr(
                    cls,
                    name,
                    types.FunctionType(
                        obj.__code__,
                        globals(),
                        name,
                        obj.__defaults__,
                        obj.__closure__,
                    ),
                )


_rebind_http_mixin_globals()

from hub_mqtt_mirror import MqttMirror

_HTTP_WORKER_MAX = max(8, int(os.environ.get("HUB_HTTP_MAX_WORKERS", "48")))
_HTTP_WORKER_SEM = threading.Semaphore(_HTTP_WORKER_MAX)


class _BoundedThreadingServer(ThreadingHTTPServer):
    """Cap concurrent HTTP workers (avoid thread explosion under load)."""

    def process_request(self, request, client_address) -> None:  # type: ignore[override]
        if not _HTTP_WORKER_SEM.acquire(blocking=False):
            try:
                request.close()
            except OSError:
                pass
            return

        def _run(req: Any = request, addr: Any = client_address) -> None:
            try:
                self._handle_client(req, addr)
            except Exception:
                self.handle_error(req, addr)
            finally:
                try:
                    self.shutdown_request(req)
                except Exception:
                    pass
                _HTTP_WORKER_SEM.release()

        threading.Thread(target=_run, daemon=True).start()

    def _handle_client(self, request: Any, client_address: Any) -> None:
        self.finish_request(request, client_address)


def run_self_test(port: int = 0) -> int:
    from hub_self_test import run_self_test as _impl
    return _impl(port)


def main() -> int:
    ap = argparse.ArgumentParser(description="NEVOD Hub — DIY/Light site ingest")
    ap.add_argument("--bind", default=os.environ.get("HUB_BIND", os.environ.get("MOCK_BIND", "0.0.0.0")))
    ap.add_argument("--port", type=int, default=int(os.environ.get("HUB_PORT", os.environ.get("MOCK_PORT", "9443"))))
    ap.add_argument("--token", default=os.environ.get("INGEST_TOKEN", ""))
    ap.add_argument("--https", action="store_true", help="TLS (нужен для WebUI ingest_url)")
    ap.add_argument("--http", action="store_true", help="явный cleartext (PB через grpc_port=80)")
    ap.add_argument("--cert-dir", default=os.environ.get("HUB_CERT_DIR", os.environ.get("MOCK_CERT_DIR", "")),
                    help="каталог для hub CA (hub.crt/key; compat mock_cloud.*)")
    ap.add_argument("--cn", default=os.environ.get("HUB_CN", os.environ.get("MOCK_CN", "")),
                    help="CN/SAN для самоподписанного сертификата (IP, как его видит DIY)")
    ap.add_argument("--mqtt-host", default=os.environ.get("MQTT_HOST", ""),
                    help="опциональный seed host внешнего MQTT (иначе — форма в UI)")
    ap.add_argument("--mqtt-port", type=int, default=int(os.environ.get("MQTT_PORT", "1883")))
    ap.add_argument("--mqtt-user", default=os.environ.get("MQTT_USER", ""))
    ap.add_argument("--mqtt-password", default=os.environ.get("MQTT_PASSWORD", ""))
    ap.add_argument("--print-diy-config", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        return run_self_test()

    if DASHBOARD_OPEN:
        print(
            "[hub] WARNING: HUB_DASHBOARD_OPEN=1 ignored — UI login required",
            file=sys.stderr,
        )

    env_http = os.environ.get("HTTP_CLEARTEXT", "").lower() in ("1", "true", "yes")
    use_https = bool(args.https) or not (args.http or env_http)
    if args.http and args.https:
        print("укажите либо --https, либо --http", file=sys.stderr)
        return 2
    if not args.https and not args.http and not env_http:
        use_https = True  # default HTTPS (Docker / lab)

    print_cfg = args.print_diy_config or os.environ.get(
        "PRINT_DIY_CONFIG", ""
    ).lower() in ("1", "true", "yes")

    token = args.token.strip()
    ingest_explicit = bool(token)
    if not token:
        if _env_truthy("HUB_ALLOW_EMPTY_TOKEN", False):
            print(
                "[hub] WARNING: пустой INGEST_TOKEN (HUB_ALLOW_EMPTY_TOKEN=1)",
                file=sys.stderr,
            )
        else:
            token = factory_cloud_token()
            if token:
                print("[hub] INGEST_TOKEN пуст — заводской", file=sys.stderr)
            else:
                token = "lab-" + uuid.uuid4().hex  # 36 chars
                print("[hub] сгенерирован token (заводской не найден)", file=sys.stderr)

    global STATE, MQTT, STORE, WEBHOOKS, FORWARDER, ADSB, ZONES, DEVICE_TOKENS
    data_dir = Path(os.environ.get("HUB_DATA_DIR") or (Path(tempfile.gettempdir()) / "nevod_hub_data"))
    data_dir.mkdir(parents=True, exist_ok=True)
    STORE = HubStore(data_dir / "hub.sqlite")
    set_api_key_provider(
        lambda: str(STORE.get_meta("open_meteo_api_key") or "") if STORE else ""
    )
    set_share_url_provider(
        lambda: str(STORE.get_meta("ecowitt_share_url") or "") if STORE else ""
    )

    def _eco_radius() -> float:
        if STORE is None:
            return 8.0
        try:
            return float(STORE.get_meta("ecowitt_radius_km") or 8.0)
        except (TypeError, ValueError):
            return 8.0

    set_radius_km_provider(_eco_radius)
    hub_auth.bind_store(STORE)
    if hub_redis.redis_url():
        ok = hub_redis.configure_from_env()
        print(
            f"[hub] redis: {'ok' if ok else 'FAIL'} ({hub_redis.public_dict()})",
            file=sys.stderr,
        )
        if hub_redis.public_dict().get("required") and not ok:
            print("[hub] HUB_REDIS_REQUIRED=1 but Redis unavailable — exit", file=sys.stderr)
            return 2
    else:
        print("[hub] redis: off (set HUB_REDIS_URL to enable sessions)", file=sys.stderr)
    DEVICE_TOKENS.clear()
    DEVICE_TOKENS.update(STORE.all_device_tokens())
    for pair in (os.environ.get("HUB_DEVICE_TOKENS") or "").split(","):
        pair = pair.strip()
        if ":" in pair:
            nid, tok = pair.split(":", 1)
            nid, tok = nid.strip(), tok.strip()
            if nid and tok:
                DEVICE_TOKENS[nid] = tok
                STORE.set_device_token(nid, tok)
    WEBHOOKS = WebhookHub(STORE)
    ZONES = ZoneRegistry(STORE)
    FORWARDER = HubForwarder(STORE)
    FORWARDER.bind_ingest_token(token, explicit=ingest_explicit)
    FORWARDER.zones = ZONES
    FORWARDER.start()
    STATE = State(token)
    try:
        STATE.ingest_log = STORE.load_ingest(time.time() - 86400)
    except Exception:
        pass
    MQTT = MqttMirror(STATE)

    def _adsb_mqtt_creds() -> dict[str, Any]:
        if MQTT is None:
            return {}
        d = MQTT.public_dict()
        return {
            "host": d.get("host") or "",
            "port": d.get("port") or 1883,
            "username": d.get("username") or "",
            "password": getattr(MQTT, "password", "") or "",
        }

    ADSB = AdsbService(STORE, mqtt_creds=_adsb_mqtt_creds, zones=ZONES)
    FORWARDER.adsb = ADSB

    def _adsb_node_centers() -> list[tuple[float, float]]:
        if STATE is None:
            return []
        out: list[tuple[float, float]] = []
        with STATE.lock:
            for node in STATE.nodes.values():
                hb = node.last_heartbeat or {}
                lat, lon = hb.get("lat"), hb.get("lon")
                try:
                    if lat is not None and lon is not None:
                        out.append((float(lat), float(lon)))
                except (TypeError, ValueError):
                    pass
        return out

    ADSB.set_extra_centers_fn(_adsb_node_centers)
    if ADSB.enabled or ZONES.enabled_feeders_unique():
        ADSB.start()

    mqtt_host = (args.mqtt_host or os.environ.get("MQTT_HOST", "")).strip()
    if mqtt_host:
        MQTT.seed_from_env(
            mqtt_host,
            int(args.mqtt_port),
            args.mqtt_user or os.environ.get("MQTT_USER", ""),
            args.mqtt_password or os.environ.get("MQTT_PASSWORD", ""),
        )
    elif MQTT.enabled and MQTT.host:
        MQTT.start()
    MQTT.start_mesh_from_env()

    host_for_cfg = args.cn.strip() or guess_lan_ip()
    cn_names = _parse_cn_list(host_for_cfg)
    primary_host = cn_names[0] if cn_names else guess_lan_ip()
    cert_path = key_path = None
    httpd = _BoundedThreadingServer((args.bind, args.port), Handler)

    if use_https:
        cert_dir = Path(args.cert_dir) if args.cert_dir else Path(tempfile.gettempdir()) / "nevod_hub"
        try:
            cert_path, key_path = ensure_self_signed(cert_dir, host_for_cfg)
        except (OSError, subprocess.CalledProcessError) as e:
            print(f"openssl failed: {e}", file=sys.stderr)
            return 1
        ssl_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ssl_ctx.load_cert_chain(str(cert_path), str(key_path))
        # TLS handshake в worker: иначе один зависший клиент блокирует accept (Recv-Q).
        _ssl_ctx = ssl_ctx

        class _TlsBoundedThreadingServer(_BoundedThreadingServer):
            def _handle_client(self, request: Any, client_address: Any) -> None:
                sock = request
                try:
                    request.settimeout(15)
                    sock = _ssl_ctx.wrap_socket(request, server_side=True)
                    self.finish_request(sock, client_address)
                except (ssl.SSLError, ConnectionResetError, BrokenPipeError, OSError):
                    pass

        httpd.server_close()
        httpd = _TlsBoundedThreadingServer((args.bind, args.port), Handler)
        print(f"[hub] HTTPS https://{args.bind}:{args.port}  CA/cert={cert_path}", flush=True)
        global CERT_PATH
        CERT_PATH = cert_path
    else:
        print(f"[hub] HTTP  http://{args.bind}:{args.port}", flush=True)

    hub_ctx = HubContext(
        state=STATE,
        mqtt=MQTT,
        store=STORE,
        webhooks=WEBHOOKS,
        forwarder=FORWARDER,
        adsb=ADSB,
        zones=ZONES,
        device_tokens=DEVICE_TOKENS,
        cert_path=CERT_PATH,
        require_device_token=REQUIRE_DEVICE_TOKEN,
        dashboard_open=DASHBOARD_OPEN,
    )
    bind_services(
        STATE=STATE,
        MQTT=MQTT,
        STORE=STORE,
        WEBHOOKS=WEBHOOKS,
        FORWARDER=FORWARDER,
        ADSB=ADSB,
        ZONES=ZONES,
        DEVICE_TOKENS=DEVICE_TOKENS,
        CERT_PATH=CERT_PATH,
        REQUIRE_DEVICE_TOKEN=REQUIRE_DEVICE_TOKEN,
        DASHBOARD_OPEN=DASHBOARD_OPEN,
    )
    httpd.ctx = hub_ctx  # type: ignore[attr-defined]

    if print_cfg:
        print_diy_config(
            host=primary_host,
            port=args.port,
            https=use_https,
            token=token,
            ca_path=cert_path,
            alt_hosts=cn_names[1:],
        )
        sys.stdout.flush()

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[hub] stop")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
