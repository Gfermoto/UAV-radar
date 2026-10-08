#!/usr/bin/env python3
"""Hub Feeder + Zone registry (design 2026-08-17).

Persist: store meta key ``zones_cfg``.
Migrate: legacy ``adsb_cfg`` → feeder ``default`` + zone ``default``.
"""
from __future__ import annotations

import copy
import os
import time
from typing import Any

from adsb.ingest import assert_adsb_url_allowed

META_KEY = "zones_cfg"
LEGACY_ADSB_KEY = "adsb_cfg"
DEFAULT_FEEDER_ID = "default"
DEFAULT_ZONE_ID = "default"
# Explicit unbind: missing key still inherits default zone (single-site UX).
ZONE_NONE_ID = "_none"
_NONE_ALIASES = frozenset({"", "_none", "none", "__none__"})
GPS_OVERRIDE_MAX_AGE_S_DEFAULT = 60.0
RADIUS_M_DEFAULT = 15000.0


def _env_mel_lan_only() -> bool:
    return os.environ.get("HUB_MEL_LAN_ONLY", "0").lower() in ("1", "true", "yes")


def _finite(v: Any) -> float | None:
    try:
        f = float(v)
    except (TypeError, ValueError):
        return None
    if f != f or abs(f) == float("inf"):  # noqa: PLR0124
        return None
    return f


def _store_zone_bind(raw: Any) -> str:
    """Zone id to persist. Empty / none aliases → ZONE_NONE_ID (exclude)."""
    s = str(raw).strip() if raw is not None else ""
    if not s or s.lower() in _NONE_ALIASES:
        return ZONE_NONE_ID
    return s


def _canon_node(node_id: str | None) -> str:
    s = (node_id or "").strip()
    if s.lower().startswith("nevod-"):
        s = s[6:]
    return s.upper()[:32]


class ZoneRegistry:
    """In-memory + SQLite meta for Feeders / Zones / node bindings."""

    def __init__(self, store: Any = None) -> None:
        self.store = store
        self.feeders: dict[str, dict[str, Any]] = {}
        self.zones: dict[str, dict[str, Any]] = {}
        self.node_zone: dict[str, str] = {}  # node_id → zone_id
        self.mel_lan_only_global = _env_mel_lan_only()
        self.gps_override_max_age_s = GPS_OVERRIDE_MAX_AGE_S_DEFAULT
        self.node_allowlist_enabled = False
        self.allowed_nodes: set[str] = set()
        self._load()

    def _load(self) -> None:
        raw = self.store.get_meta(META_KEY) if self.store is not None else None
        if isinstance(raw, dict) and (raw.get("feeders") or raw.get("zones")):
            self._from_dict(raw)
            return
        legacy = self.store.get_meta(LEGACY_ADSB_KEY) if self.store is not None else None
        if isinstance(legacy, dict):
            self.mel_lan_only_global = _env_mel_lan_only()
            self._migrate_legacy_adsb(legacy)
            self.persist()
            return
        self.mel_lan_only_global = _env_mel_lan_only()
        self._ensure_defaults()
        self.persist()

    def _ensure_defaults(self) -> None:
        if DEFAULT_FEEDER_ID not in self.feeders:
            self.feeders[DEFAULT_FEEDER_ID] = {
                "id": DEFAULT_FEEDER_ID,
                "name": "Default",
                "url": "http://192.168.1.117/skyaware/data/aircraft.json",
                "enabled": False,
                "poll_interval_s": 5.0,
                "stale_s": 30.0,
            }
        if DEFAULT_ZONE_ID not in self.zones:
            self.zones[DEFAULT_ZONE_ID] = {
                "id": DEFAULT_ZONE_ID,
                "name": "Default",
                "lat": None,
                "lon": None,
                "radius_m": RADIUS_M_DEFAULT,
                "feeder_id": DEFAULT_FEEDER_ID,
                "filter_det": False,
                "filter_hb": False,
                "filter_mel": False,
                "forward_mel": None,  # inherit global
                "alert_mute": False,
            }

    def _migrate_legacy_adsb(self, cfg: dict[str, Any]) -> None:
        self._ensure_defaults()
        f = self.feeders[DEFAULT_FEEDER_ID]
        z = self.zones[DEFAULT_ZONE_ID]
        if isinstance(cfg.get("url"), str) and cfg["url"].strip():
            f["url"] = cfg["url"].strip()
        f["enabled"] = bool(cfg.get("enabled"))
        if cfg.get("interval_s") is not None:
            f["poll_interval_s"] = max(1.0, float(cfg["interval_s"]))
        if cfg.get("stale_s") is not None:
            f["stale_s"] = max(5.0, float(cfg["stale_s"]))
        if cfg.get("site_lat") is not None:
            z["lat"] = _finite(cfg["site_lat"])
        if cfg.get("site_lon") is not None:
            z["lon"] = _finite(cfg["site_lon"])
        if cfg.get("radius_m") is not None:
            z["radius_m"] = float(cfg["radius_m"])
        z["filter_det"] = bool(cfg.get("filter_det"))
        z["filter_hb"] = bool(cfg.get("filter_hb"))
        z["filter_mel"] = bool(cfg.get("filter_mel"))
        z["feeder_id"] = DEFAULT_FEEDER_ID
        if isinstance(cfg.get("site_id"), str) and cfg["site_id"].strip():
            z["name"] = cfg["site_id"].strip()
            f["name"] = cfg["site_id"].strip()

    def _from_dict(self, raw: dict[str, Any]) -> None:
        self.feeders = {}
        for item in raw.get("feeders") or []:
            if not isinstance(item, dict):
                continue
            fid = str(item.get("id") or "").strip() or DEFAULT_FEEDER_ID
            self.feeders[fid] = {
                "id": fid,
                "name": str(item.get("name") or fid),
                "url": str(item.get("url") or "").strip(),
                "enabled": bool(item.get("enabled")),
                "poll_interval_s": max(1.0, float(item.get("poll_interval_s") or 5)),
                "stale_s": max(5.0, float(item.get("stale_s") or 30)),
            }
        self.zones = {}
        for item in raw.get("zones") or []:
            if not isinstance(item, dict):
                continue
            zid = str(item.get("id") or "").strip() or DEFAULT_ZONE_ID
            fm = item.get("forward_mel")
            self.zones[zid] = {
                "id": zid,
                "name": str(item.get("name") or zid),
                "lat": _finite(item.get("lat")),
                "lon": _finite(item.get("lon")),
                "radius_m": float(item.get("radius_m") or RADIUS_M_DEFAULT),
                "feeder_id": (
                    str(item["feeder_id"]).strip()
                    if item.get("feeder_id") not in (None, "")
                    else None
                ),
                "filter_det": bool(item.get("filter_det")),
                "filter_hb": bool(item.get("filter_hb")),
                "filter_mel": bool(item.get("filter_mel")),
                "forward_mel": None if fm is None else bool(fm),
                "alert_mute": bool(item.get("alert_mute")),
            }
        self.node_zone = {}
        for nid, zid in (raw.get("node_zone") or {}).items():
            cn = _canon_node(str(nid))
            if cn:
                self.node_zone[cn] = _store_zone_bind(zid)
        if "mel_lan_only_global" in raw:
            self.mel_lan_only_global = bool(raw.get("mel_lan_only_global"))
        else:
            self.mel_lan_only_global = _env_mel_lan_only()
        self.gps_override_max_age_s = float(
            raw.get("gps_override_max_age_s") or GPS_OVERRIDE_MAX_AGE_S_DEFAULT
        )
        self.node_allowlist_enabled = bool(raw.get("node_allowlist_enabled", False))
        self.allowed_nodes = {
            _canon_node(x) for x in (raw.get("allowed_nodes") or []) if _canon_node(x)
        }
        self._ensure_defaults()

    def to_dict(self) -> dict[str, Any]:
        return {
            "feeders": list(self.feeders.values()),
            "zones": list(self.zones.values()),
            "node_zone": dict(self.node_zone),
            "mel_lan_only_global": self.mel_lan_only_global,
            "gps_override_max_age_s": self.gps_override_max_age_s,
            "node_allowlist_enabled": self.node_allowlist_enabled,
            "allowed_nodes": sorted(self.allowed_nodes),
        }

    def persist(self) -> None:
        if self.store is None:
            return
        self.store.set_meta(META_KEY, self.to_dict())

    def public_dict(self) -> dict[str, Any]:
        return copy.deepcopy(self.to_dict())

    def apply_patch(self, payload: dict[str, Any], *, persist: bool = True) -> dict[str, Any]:
        """Replace/merge feeders, zones, node_zone, globals from API body."""
        if "feeders" in payload and isinstance(payload["feeders"], list):
            nxt: dict[str, dict[str, Any]] = {}
            for item in payload["feeders"]:
                if not isinstance(item, dict):
                    continue
                fid = str(item.get("id") or "").strip()
                if not fid:
                    continue
                url = str(item.get("url") or "").strip()
                if url:
                    assert_adsb_url_allowed(url)
                nxt[fid] = {
                    "id": fid,
                    "name": str(item.get("name") or fid),
                    "url": url,
                    "enabled": bool(item.get("enabled")),
                    "poll_interval_s": max(1.0, float(item.get("poll_interval_s") or 5)),
                    "stale_s": max(5.0, float(item.get("stale_s") or 30)),
                }
            if nxt:
                self.feeders = nxt
        if "zones" in payload and isinstance(payload["zones"], list):
            nxt_z: dict[str, dict[str, Any]] = {}
            for item in payload["zones"]:
                if not isinstance(item, dict):
                    continue
                zid = str(item.get("id") or "").strip()
                if not zid:
                    continue
                fm = item.get("forward_mel")
                fid = item.get("feeder_id")
                filter_on = any(
                    bool(item.get(k))
                    for k in ("filter_det", "filter_hb", "filter_mel")
                )
                feeder_id = str(fid).strip() if fid not in (None, "") else None
                if filter_on and not feeder_id:
                    raise ValueError(f"zone_filter_requires_feeder:{zid}")
                nxt_z[zid] = {
                    "id": zid,
                    "name": str(item.get("name") or zid),
                    "lat": _finite(item.get("lat")),
                    "lon": _finite(item.get("lon")),
                    "radius_m": float(item.get("radius_m") or RADIUS_M_DEFAULT),
                    "feeder_id": feeder_id,
                    "filter_det": bool(item.get("filter_det")),
                    "filter_hb": bool(item.get("filter_hb")),
                    "filter_mel": bool(item.get("filter_mel")),
                    "forward_mel": None if fm is None else bool(fm),
                    "alert_mute": bool(item.get("alert_mute")),
                }
            if nxt_z:
                self.zones = nxt_z
        if "node_zone" in payload and isinstance(payload["node_zone"], dict):
            nxt_n: dict[str, str] = {}
            for k, v in payload["node_zone"].items():
                cn = _canon_node(k)
                if cn:
                    nxt_n[cn] = _store_zone_bind(v)
            self.node_zone = nxt_n
        if "mel_lan_only_global" in payload:
            self.mel_lan_only_global = bool(payload["mel_lan_only_global"])
        if payload.get("gps_override_max_age_s") is not None:
            self.gps_override_max_age_s = max(
                5.0, float(payload["gps_override_max_age_s"])
            )
        if "node_allowlist_enabled" in payload:
            self.node_allowlist_enabled = bool(payload["node_allowlist_enabled"])
        if "allowed_nodes" in payload and isinstance(payload["allowed_nodes"], list):
            self.allowed_nodes = {
                _canon_node(x) for x in payload["allowed_nodes"] if _canon_node(x)
            }
        self._ensure_defaults()
        if persist:
            self.persist()
        return self.public_dict()

    def sync_from_legacy_adsb_apply(self, cfg: dict[str, Any]) -> None:
        """Keep zones in sync when UI still POSTs /api/hub/adsb single form."""
        self._ensure_defaults()
        f = self.feeders[DEFAULT_FEEDER_ID]
        z = self.zones[DEFAULT_ZONE_ID]
        if "url" in cfg and isinstance(cfg["url"], str) and cfg["url"].strip():
            f["url"] = cfg["url"].strip()
        if "enabled" in cfg:
            f["enabled"] = bool(cfg["enabled"])
        if cfg.get("interval_s") is not None:
            f["poll_interval_s"] = max(1.0, float(cfg["interval_s"]))
        if cfg.get("stale_s") is not None:
            f["stale_s"] = max(5.0, float(cfg["stale_s"]))
        if "site_lat" in cfg:
            z["lat"] = _finite(cfg["site_lat"])
        if "site_lon" in cfg:
            z["lon"] = _finite(cfg["site_lon"])
        if cfg.get("radius_m") is not None:
            z["radius_m"] = float(cfg["radius_m"])
        if "filter_det" in cfg:
            z["filter_det"] = bool(cfg["filter_det"])
        if "filter_hb" in cfg:
            z["filter_hb"] = bool(cfg["filter_hb"])
        if "filter_mel" in cfg:
            z["filter_mel"] = bool(cfg["filter_mel"])
        if isinstance(cfg.get("site_id"), str) and cfg["site_id"].strip():
            z["name"] = cfg["site_id"].strip()
            f["name"] = cfg["site_id"].strip()
        z["feeder_id"] = DEFAULT_FEEDER_ID
        self.persist()

    def legacy_adsb_view(self) -> dict[str, Any]:
        """Shape expected by old UI / AdsbService.config_dict."""
        self._ensure_defaults()
        f = self.feeders[DEFAULT_FEEDER_ID]
        z = self.zones[DEFAULT_ZONE_ID]
        return {
            "enabled": bool(f.get("enabled")),
            "url": f.get("url") or "",
            "site_id": z.get("name") or "lab",
            "radius_m": float(z.get("radius_m") or RADIUS_M_DEFAULT),
            "site_lat": z.get("lat"),
            "site_lon": z.get("lon"),
            "interval_s": float(f.get("poll_interval_s") or 5),
            "stale_s": float(f.get("stale_s") or 30),
            "snapshot": False,
            "filter_det": bool(z.get("filter_det")),
            "filter_hb": bool(z.get("filter_hb")),
            "filter_mel": bool(z.get("filter_mel")),
        }

    def zone_for_node(self, node_id: str | None) -> dict[str, Any] | None:
        nid = _canon_node(node_id)
        if nid:
            zid = self.node_zone.get(nid)
            if zid == ZONE_NONE_ID:
                return None
            if zid:
                return self.zones.get(zid)
        # Never mapped: default zone (single-site UX).
        return self.zones.get(DEFAULT_ZONE_ID)

    def bind_node(self, node_id: str, zone_id: str | None, *, persist: bool = True) -> None:
        nid = _canon_node(node_id)
        if not nid:
            return
        self.node_zone[nid] = _store_zone_bind(zone_id)
        if persist:
            self.persist()

    def effective_forward_mel(self, node_id: str | None) -> bool:
        """T4: zone.forward_mel override, else not mel_lan_only_global."""
        zone = self.zone_for_node(node_id)
        if zone is not None and zone.get("forward_mel") is not None:
            return bool(zone["forward_mel"])
        return not self.mel_lan_only_global

    def resolve_center(
        self,
        zone: dict[str, Any],
        *,
        lat: float | None,
        lon: float | None,
        timestamp_ms: int | None,
        now_s: float | None = None,
    ) -> tuple[float | None, float | None, str]:
        """Return (lat, lon, source) — source is ``gps`` or ``zone``."""
        now = now_s if now_s is not None else time.time()
        glat, glon = _finite(lat), _finite(lon)
        age_ok = False
        if glat is not None and glon is not None and timestamp_ms is not None:
            try:
                age_s = abs(now - (int(timestamp_ms) / 1000.0))
                age_ok = age_s <= self.gps_override_max_age_s
            except (TypeError, ValueError):
                age_ok = False
        if age_ok and glat is not None and glon is not None:
            return glat, glon, "gps"
        zlat, zlon = _finite(zone.get("lat")), _finite(zone.get("lon"))
        return zlat, zlon, "zone"

    def node_allowed(self, node_id: str | None) -> bool:
        if not self.node_allowlist_enabled:
            return True
        nid = _canon_node(node_id)
        return bool(nid and nid in self.allowed_nodes)

    def enabled_feeders_unique(self) -> list[dict[str, Any]]:
        """Enabled feeders; dedupe by URL (first wins)."""
        seen_url: set[str] = set()
        out: list[dict[str, Any]] = []
        for f in self.feeders.values():
            if not f.get("enabled"):
                continue
            url = (f.get("url") or "").strip()
            if not url or url in seen_url:
                continue
            seen_url.add(url)
            out.append(f)
        return out
