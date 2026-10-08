#!/usr/bin/env python3
"""Nevod Hub webhooks: Telegram / IFTTT / email only (no Discord)."""
from __future__ import annotations

import ipaddress
import json
import os
import smtplib
import socket
import ssl
import threading
import time
import urllib.error
import urllib.request
from email.message import EmailMessage
from typing import Any
from urllib.parse import urlparse


def _env_bool(name: str, default: bool = False) -> bool:
    v = os.environ.get(name, "")
    if not v:
        return default
    return v.lower() in ("1", "true", "yes", "on")


def webhook_url_allowed(url: str, *, allow_private: bool = False) -> tuple[bool, str]:
    """SSRF guard for IFTTT/maker HTTPS URLs (fog forwarder pattern)."""
    try:
        u = urlparse(url)
    except Exception as e:  # noqa: BLE001
        return False, f"bad_url:{e}"
    if u.scheme not in ("http", "https"):
        return False, f"bad_scheme:{u.scheme!r}"
    if not u.hostname:
        return False, "no_host"
    host = u.hostname.lower()
    if host in ("localhost", "metadata.google.internal"):
        return False, "blocked_host"
    try:
        infos = socket.getaddrinfo(host, u.port or (443 if u.scheme == "https" else 80), type=socket.SOCK_STREAM)
    except OSError as e:
        return False, f"dns:{e}"
    for info in infos:
        sockaddr = info[4]
        try:
            ip = ipaddress.ip_address(sockaddr[0])
        except ValueError:
            return False, f"bad_resolved_addr:{sockaddr[0]!r}"
        # Loopback / link-local / metadata всегда запрещены (даже allow_private).
        if ip.is_loopback or ip.is_link_local or ip.is_reserved or ip.is_multicast:
            return False, f"blocked_target_ip:{ip}"
        if ip.is_private and not allow_private:
            return False, f"blocked_target_ip:{ip}"
    return True, "ok"


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    """Block HTTP redirects — иначе SSRF-check обходится 302→LAN/metadata."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):  # noqa: ANN001, ARG002
        return None


def urlopen_no_redirect(req: urllib.request.Request, timeout: float = 8.0):
    opener = urllib.request.build_opener(_NoRedirect())
    return opener.open(req, timeout=timeout)


class WebhookHub:
    """Cooldown + deliver alerts on joined DET."""

    def __init__(self, store: Any | None = None) -> None:
        self.store = store
        self._lock = threading.Lock()
        self._last_fire: dict[str, float] = {}  # node_id -> ts
        self.reload_from_env()
        self._load_store_overrides()

    def reload_from_env(self) -> None:
        self.min_threat = float(os.environ.get("HUB_ALERT_MIN_THREAT", "0.5"))
        self.cooldown_s = float(os.environ.get("HUB_ALERT_COOLDOWN_S", "60"))
        self.allow_private = _env_bool("HUB_WEBHOOK_ALLOW_PRIVATE", False)

        self.tg_enabled = _env_bool("HUB_TG_ENABLED", False)
        self.tg_token = os.environ.get("HUB_TG_BOT_TOKEN", "").strip()
        self.tg_chat = os.environ.get("HUB_TG_CHAT_ID", "").strip()

        self.ifttt_enabled = _env_bool("HUB_IFTTT_ENABLED", False)
        self.ifttt_url = os.environ.get("HUB_IFTTT_URL", "").strip()

        self.email_enabled = _env_bool("HUB_EMAIL_ENABLED", False)
        self.smtp_host = os.environ.get("HUB_SMTP_HOST", "").strip()
        self.smtp_port = int(os.environ.get("HUB_SMTP_PORT", "587"))
        self.smtp_user = os.environ.get("HUB_SMTP_USER", "").strip()
        self.smtp_pass = os.environ.get("HUB_SMTP_PASS", "").strip()
        self.smtp_from = os.environ.get("HUB_SMTP_FROM", "").strip() or self.smtp_user
        self.smtp_to = os.environ.get("HUB_SMTP_TO", "").strip()
        self.smtp_tls = _env_bool("HUB_SMTP_TLS", True)

    def _load_store_overrides(self) -> None:
        """UI-saved meta overrides env (same pattern as HubForwarder)."""
        if self.store is None:
            return
        cfg = self.store.get_meta("webhook_cfg")
        if not isinstance(cfg, dict):
            return
        try:
            self.apply_config(cfg, persist=False)
        except Exception as e:  # noqa: BLE001
            # Legacy SQLite may hold LAN IFTTT URL while env allow_private=0.
            print(f"[hub-webhooks] store override ignored: {e}", flush=True)
            self.ifttt_enabled = False
            self.ifttt_url = ""

    def apply_config(self, payload: dict[str, Any], *, persist: bool = True) -> dict[str, Any]:
        """Update channel config. Empty secret fields keep previous values."""
        with self._lock:
            if "min_threat" in payload and payload["min_threat"] is not None:
                self.min_threat = float(payload["min_threat"])
            if "cooldown_s" in payload and payload["cooldown_s"] is not None:
                self.cooldown_s = float(payload["cooldown_s"])
            # allow_private — только env HUB_WEBHOOK_ALLOW_PRIVATE (не из UI/JSON).

            tg = payload.get("telegram") if isinstance(payload.get("telegram"), dict) else {}
            if "enabled" in tg:
                self.tg_enabled = bool(tg["enabled"])
            if "chat_id" in tg and tg["chat_id"] is not None:
                self.tg_chat = str(tg["chat_id"]).strip()
            tok = tg.get("bot_token")
            if isinstance(tok, str) and tok.strip() and tok.strip() != "********":
                self.tg_token = tok.strip()

            ift = payload.get("ifttt") if isinstance(payload.get("ifttt"), dict) else {}
            if "enabled" in ift:
                self.ifttt_enabled = bool(ift["enabled"])
            url = ift.get("url")
            if isinstance(url, str) and url.strip() and url.strip() != "********":
                ok, reason = webhook_url_allowed(url.strip(), allow_private=self.allow_private)
                if not ok:
                    raise ValueError(f"ifttt_ssrf:{reason}")
                self.ifttt_url = url.strip()

            em = payload.get("email") if isinstance(payload.get("email"), dict) else {}
            if "enabled" in em:
                self.email_enabled = bool(em["enabled"])
            if "host" in em and em["host"] is not None:
                self.smtp_host = str(em["host"]).strip()
            if "port" in em and em["port"] is not None:
                self.smtp_port = int(em["port"])
            if "user" in em and em["user"] is not None:
                self.smtp_user = str(em["user"]).strip()
            if "from_addr" in em and em["from_addr"] is not None:
                self.smtp_from = str(em["from_addr"]).strip()
            elif "from" in em and em["from"] is not None:
                self.smtp_from = str(em["from"]).strip()
            if "to" in em and em["to"] is not None:
                self.smtp_to = str(em["to"]).strip()
            if "tls" in em:
                self.smtp_tls = bool(em["tls"])
            pw = em.get("password")
            if isinstance(pw, str) and pw.strip() and pw.strip() != "********":
                self.smtp_pass = pw.strip()
            if self.smtp_from == "" and self.smtp_user:
                self.smtp_from = self.smtp_user

            if persist and self.store is not None:
                self.store.set_meta(
                    "webhook_cfg",
                    {
                        "min_threat": self.min_threat,
                        "cooldown_s": self.cooldown_s,
                        "telegram": {
                            "enabled": self.tg_enabled,
                            "chat_id": self.tg_chat,
                            "bot_token": self.tg_token,
                        },
                        "ifttt": {
                            "enabled": self.ifttt_enabled,
                            "url": self.ifttt_url,
                        },
                        "email": {
                            "enabled": self.email_enabled,
                            "host": self.smtp_host,
                            "port": self.smtp_port,
                            "user": self.smtp_user,
                            "password": self.smtp_pass,
                            "from_addr": self.smtp_from,
                            "to": self.smtp_to,
                            "tls": self.smtp_tls,
                        },
                    },
                )
        return self.public_config()

    def public_config(self) -> dict[str, Any]:
        """No secrets."""
        return {
            "min_threat": self.min_threat,
            "cooldown_s": self.cooldown_s,
            "allow_private": self.allow_private,
            "telegram": {
                "enabled": self.tg_enabled,
                "configured": bool(self.tg_token and self.tg_chat),
                "has_token": bool(self.tg_token),
                "chat_id": self.tg_chat if self.tg_chat else "",
            },
            "ifttt": {
                "enabled": self.ifttt_enabled,
                "configured": bool(self.ifttt_url),
                "has_url": bool(self.ifttt_url),
            },
            "email": {
                "enabled": self.email_enabled,
                "configured": bool(self.smtp_host and self.smtp_to),
                "host": self.smtp_host,
                "port": self.smtp_port,
                "user": self.smtp_user,
                "from_addr": self.smtp_from,
                "to": self.smtp_to,
                "tls": self.smtp_tls,
                "has_password": bool(self.smtp_pass),
            },
            "channels": ["telegram", "ifttt", "email"],
            "out_of_scope": ["discord"],
        }

    def _should_fire(self, node_id: str, threat: float, joined: bool) -> bool:
        if not joined:
            return False
        if threat < self.min_threat:
            return False
        now = time.time()
        with self._lock:
            last = self._last_fire.get(node_id, 0.0)
            if now - last < self.cooldown_s:
                return False
            self._last_fire[node_id] = now
        return True

    def schedule_joined_detection(self, det: dict[str, Any]) -> None:
        """Fire webhooks off STATE.lock / ingest thread (sync I/O inside worker)."""
        payload = dict(det)

        def _run() -> None:
            try:
                self.on_joined_detection(payload)
            except Exception as e:  # noqa: BLE001
                print(f"[hub] webhook async: {e}", flush=True)

        threading.Thread(target=_run, daemon=True, name="hub-webhook").start()

    def on_joined_detection(self, det: dict[str, Any]) -> list[dict[str, Any]]:
        node_id = str(det.get("node_id") or "?")
        try:
            threat = float(det.get("threat") or 0.0)
        except (TypeError, ValueError):
            threat = 0.0
        joined = bool(det.get("joined"))
        if not self._should_fire(node_id, threat, joined):
            return []
        alert = {
            "schema": "nevod.hub.alert.v1",
            "ts": int(time.time() * 1000),
            "node_id": node_id,
            "class": det.get("class_name") or det.get("class") or "?",
            "threat": threat,
            "azimuth_deg": det.get("azimuth") if det.get("azimuth") is not None else det.get("azimuth_deg"),
            "timestamp_ms": det.get("timestamp_ms"),
            "confidence": det.get("confidence") or det.get("p"),
        }
        results: list[dict[str, Any]] = []
        if self.tg_enabled and self.tg_token and self.tg_chat:
            results.append(self._send_telegram(alert))
        if self.ifttt_enabled and self.ifttt_url:
            results.append(self._send_ifttt(alert))
        if self.email_enabled and self.smtp_host and self.smtp_to:
            results.append(self._send_email(alert))
        return results

    def _log(self, channel: str, node_id: str, ok: bool, detail: str) -> dict[str, Any]:
        if self.store is not None:
            try:
                self.store.log_webhook(channel, node_id, ok, detail)
            except Exception:  # noqa: BLE001
                pass
        return {"channel": channel, "node_id": node_id, "ok": ok, "detail": detail}

    def _send_telegram(self, alert: dict[str, Any]) -> dict[str, Any]:
        text = (
            f"NEVOD Hub DET\n"
            f"node={alert['node_id']} class={alert['class']} "
            f"threat={alert['threat']:.2f} az={alert.get('azimuth_deg')} "
            f"ts={alert.get('timestamp_ms')}"
        )
        url = f"https://api.telegram.org/bot{self.tg_token}/sendMessage"
        payload = json.dumps({"chat_id": self.tg_chat, "text": text}).encode()
        req = urllib.request.Request(
            url, data=payload, method="POST", headers={"Content-Type": "application/json"}
        )
        try:
            with urllib.request.urlopen(req, timeout=8) as r:
                ok = 200 <= r.status < 300
                return self._log("telegram", alert["node_id"], ok, f"http_{r.status}")
        except Exception as e:  # noqa: BLE001
            return self._log("telegram", alert["node_id"], False, str(e))

    def _urlopen(self, req: urllib.request.Request, timeout: float = 8.0):
        """Outbound HTTP (no redirects). Overridable in self-tests."""
        return urlopen_no_redirect(req, timeout=timeout)

    def _send_ifttt(self, alert: dict[str, Any]) -> dict[str, Any]:
        ok_url, reason = webhook_url_allowed(self.ifttt_url, allow_private=self.allow_private)
        if not ok_url:
            return self._log("ifttt", alert["node_id"], False, f"ssrf:{reason}")
        body = {
            "value1": alert["node_id"],
            "value2": f"{alert['class']}:{alert['threat']:.2f}",
            "value3": json.dumps(alert, ensure_ascii=False),
        }
        req = urllib.request.Request(
            self.ifttt_url,
            data=json.dumps(body).encode(),
            method="POST",
            headers={"Content-Type": "application/json"},
        )
        try:
            with self._urlopen(req, timeout=8) as r:
                return self._log("ifttt", alert["node_id"], 200 <= r.status < 300, f"http_{r.status}")
        except urllib.error.HTTPError as e:
            # 3xx without Location follow → redirect blocked by _NoRedirect
            if 300 <= int(e.code) < 400:
                return self._log("ifttt", alert["node_id"], False, f"redirect_blocked:{e.code}")
            return self._log("ifttt", alert["node_id"], False, f"http_{e.code}")
        except Exception as e:  # noqa: BLE001
            return self._log("ifttt", alert["node_id"], False, str(e))

    def _send_email(self, alert: dict[str, Any]) -> dict[str, Any]:
        msg = EmailMessage()
        msg["Subject"] = f"[NEVOD Hub] {alert['class']} threat={alert['threat']:.2f} {alert['node_id']}"
        msg["From"] = self.smtp_from
        msg["To"] = self.smtp_to
        msg.set_content(json.dumps(alert, ensure_ascii=False, indent=2))
        try:
            if self.smtp_tls:
                ctx = ssl.create_default_context()
                with smtplib.SMTP(self.smtp_host, self.smtp_port, timeout=15) as s:
                    s.starttls(context=ctx)
                    if self.smtp_user:
                        s.login(self.smtp_user, self.smtp_pass)
                    s.send_message(msg)
            else:
                with smtplib.SMTP(self.smtp_host, self.smtp_port, timeout=15) as s:
                    if self.smtp_user:
                        s.login(self.smtp_user, self.smtp_pass)
                    s.send_message(msg)
            return self._log("email", alert["node_id"], True, "sent")
        except Exception as e:  # noqa: BLE001
            return self._log("email", alert["node_id"], False, str(e))
