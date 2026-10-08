"""In-process Hub ADS-B service (#114): multi-feeder poll → MQTT + per-zone Cloud filter."""

from __future__ import annotations

import threading
import time
from typing import Any, Callable

from adsb.ingest import (
    LAB_ADSB_URL,
    MqttPublisher,
    assert_adsb_url_allowed,
    fetch_aircraft_json,
    haversine_m,
    process_poll,
)
from adsb.lol_fallback import (
    DEFAULT_DIST_NM,
    build_lol_url,
    fetch_adsb_lol_multi,
    is_blind_aircraft_json,
    lol_fallback_enabled,
)
from adsb.schema import (
    POLL_INTERVAL_S_DEFAULT,
    QOS_AC,
    QOS_HEALTH,
    QOS_SNAPSHOT,
    RETAIN_AC,
    RETAIN_HEALTH,
    RETAIN_SNAPSHOT,
    build_health,
    topic_ac,
    topic_health,
    topic_snapshot,
)

# Fail-open for Cloud filter if health older than this.
STALE_S_DEFAULT = 30.0

FILTER_TYPES = ("det", "hb", "mel")


class AdsbService:
    def __init__(
        self,
        store: Any,
        mqtt_creds: Callable[[], dict[str, Any]],
        zones: Any | None = None,
    ) -> None:
        self.store = store
        self._mqtt_creds = mqtt_creds
        self.zones = zones  # ZoneRegistry | None
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self.enabled = False
        self.url = LAB_ADSB_URL
        self.site_id = "lab"
        self.radius_m = 50000.0
        self.site_lat: float | None = None
        self.site_lon: float | None = None
        self.interval_s = POLL_INTERVAL_S_DEFAULT
        self.stale_s = STALE_S_DEFAULT
        self.snapshot = False
        # Cloud filter checkboxes — default all OFF = transparent (#116)
        self.filter_det = False
        self.filter_hb = False
        self.filter_mel = False
        self._last_health: dict[str, Any] | None = None
        self._last_meta: dict[str, Any] = {
            "civilian_in_radius": False,
            "adsb_radius_m": self.radius_m,
            "adsb_count_in_radius": 0,
            "adsb_nearest_dist_m": None,
        }
        self._last_aircraft: list[dict[str, Any]] = []
        self._last_ok_at: float | None = None
        self._last_error = ""
        self._suppressed = {"det": 0, "hb": 0, "mel": 0}
        # feeder_id → {ok_at, stale_s, positions:[(lat,lon),...], error, url}
        self._feeder_cache: dict[str, dict[str, Any]] = {}
        self.fallback_lol = True  # product default; env HUB_ADSB_LOL_FALLBACK can force off
        self.fallback_lol_dist_nm = DEFAULT_DIST_NM
        self._fallback_info: dict[str, Any] = {
            "active": False,
            "reason": "",
            "url": "",
            "error": "",
        }
        self._extra_centers_fn: Callable[[], list[tuple[float, float]]] | None = None
        self._load()

    def set_extra_centers_fn(
        self, fn: Callable[[], list[tuple[float, float]]] | None
    ) -> None:
        """Опционально: lat/lon узлов (HB) для multi-point adsb.lol."""
        self._extra_centers_fn = fn

    def bind_zones(self, zones: Any) -> None:
        self.zones = zones
        if zones is not None:
            try:
                view = zones.legacy_adsb_view()
                self.apply(view, persist=False, sync_zones=False)
            except Exception as e:  # noqa: BLE001
                self._last_error = f"zones_bind:{e}"
                print(f"[hub-adsb] zones bind: {e}", flush=True)

    def _load(self) -> None:
        if self.zones is not None:
            try:
                self.apply(self.zones.legacy_adsb_view(), persist=False, sync_zones=False)
            except Exception as e:  # noqa: BLE001
                self.enabled = False
                self._last_error = f"cfg_load:{e}"
                print(f"[hub-adsb] zones cfg ignored: {e}", flush=True)
            return
        if self.store is None:
            return
        cfg = self.store.get_meta("adsb_cfg")
        if isinstance(cfg, dict):
            try:
                self.apply(cfg, persist=False, sync_zones=False)
            except Exception as e:  # noqa: BLE001
                self.enabled = False
                self._last_error = f"cfg_load:{e}"
                print(f"[hub-adsb] store cfg ignored: {e}", flush=True)

    def apply(
        self,
        cfg: dict[str, Any],
        *,
        persist: bool = True,
        sync_zones: bool = True,
    ) -> dict[str, Any]:
        # Сначала собрать next-state и провалидировать — без partial mutate.
        with self._lock:
            enabled = bool(cfg["enabled"]) if "enabled" in cfg else self.enabled
            url = self.url
            if isinstance(cfg.get("url"), str) and cfg["url"].strip():
                url = cfg["url"].strip()
                assert_adsb_url_allowed(url)
            site_id = (
                cfg["site_id"].strip()
                if isinstance(cfg.get("site_id"), str) and cfg["site_id"].strip()
                else self.site_id
            )
            radius_m = float(cfg["radius_m"]) if cfg.get("radius_m") is not None else self.radius_m
            if "site_lat" in cfg:
                site_lat = float(cfg["site_lat"]) if cfg["site_lat"] is not None else None
            else:
                site_lat = self.site_lat
            if "site_lon" in cfg:
                site_lon = float(cfg["site_lon"]) if cfg["site_lon"] is not None else None
            else:
                site_lon = self.site_lon
            interval_s = (
                max(1.0, float(cfg["interval_s"]))
                if cfg.get("interval_s") is not None
                else self.interval_s
            )
            stale_s = (
                max(5.0, float(cfg["stale_s"]))
                if cfg.get("stale_s") is not None
                else self.stale_s
            )
            snapshot = bool(cfg["snapshot"]) if "snapshot" in cfg else self.snapshot
            filter_det = bool(cfg["filter_det"]) if "filter_det" in cfg else self.filter_det
            filter_hb = bool(cfg["filter_hb"]) if "filter_hb" in cfg else self.filter_hb
            filter_mel = bool(cfg["filter_mel"]) if "filter_mel" in cfg else self.filter_mel
            if (filter_det or filter_hb or filter_mel) and (
                site_lat is None or site_lon is None
            ):
                raise ValueError("adsb_filter_requires_site_lat_lon")
            fallback_lol = (
                bool(cfg["fallback_lol"]) if "fallback_lol" in cfg else self.fallback_lol
            )
            fallback_dist = (
                max(1.0, min(float(cfg["fallback_lol_dist_nm"]), 250.0))
                if cfg.get("fallback_lol_dist_nm") is not None
                else self.fallback_lol_dist_nm
            )
            self.enabled = enabled
            self.url = url
            self.site_id = site_id
            self.radius_m = radius_m
            self.site_lat = site_lat
            self.site_lon = site_lon
            self.interval_s = interval_s
            self.stale_s = stale_s
            self.snapshot = snapshot
            self.filter_det = filter_det
            self.filter_hb = filter_hb
            self.filter_mel = filter_mel
            self.fallback_lol = fallback_lol
            self.fallback_lol_dist_nm = fallback_dist
            if persist and self.store is not None:
                self.store.set_meta("adsb_cfg", self.config_dict())
        if sync_zones and self.zones is not None:
            try:
                self.zones.sync_from_legacy_adsb_apply(self.config_dict())
            except Exception as e:  # noqa: BLE001
                print(f"[hub-adsb] zones sync: {e}", flush=True)
        if self._want_running():
            self.start()
        else:
            self.stop()
        return self.public_dict()

    def config_dict(self) -> dict[str, Any]:
        return {
            "enabled": self.enabled,
            "url": self.url,
            "site_id": self.site_id,
            "radius_m": self.radius_m,
            "site_lat": self.site_lat,
            "site_lon": self.site_lon,
            "interval_s": self.interval_s,
            "stale_s": self.stale_s,
            "snapshot": self.snapshot,
            "filter_det": self.filter_det,
            "filter_hb": self.filter_hb,
            "filter_mel": self.filter_mel,
            "fallback_lol": self.fallback_lol,
            "fallback_lol_dist_nm": self.fallback_lol_dist_nm,
        }

    def public_dict(self) -> dict[str, Any]:
        with self._lock:
            fresh = self._is_fresh_unlocked()
            meta = dict(self._last_meta)
            health = dict(self._last_health) if self._last_health else None
            total = int((health or {}).get("total_aircraft") or 0)
            with_pos = int((health or {}).get("aircraft_with_positions") or 0)
            in_r = int(meta.get("adsb_count_in_radius") or 0)
            any_feeder = bool(self._feeder_cache) or self.enabled
            if not any_feeder and not (
                self.zones and self.zones.enabled_feeders_unique()
            ):
                if self._fallback_info.get("active"):
                    signal = "live" if fresh else "waiting"
                else:
                    signal = "off"
            elif self._last_error and not fresh:
                signal = "error"
            elif fresh and (health or {}).get("ok"):
                signal = "live"
            elif self._last_ok_at:
                signal = "stale"
            else:
                signal = "waiting"
            feeders_pub = {
                fid: {
                    "ok_at": c.get("ok_at"),
                    "error": c.get("error") or "",
                    "positions": len(c.get("positions") or []),
                    "fresh": self._feeder_fresh_unlocked(fid),
                    "source": c.get("source") or "local",
                }
                for fid, c in self._feeder_cache.items()
            }
            return {
                **self.config_dict(),
                "running": self._thread is not None and self._thread.is_alive(),
                "fresh": fresh,
                "stale": bool(self.enabled and self._last_ok_at and not fresh),
                "signal": signal,
                "last_ok_at": self._last_ok_at,
                "last_error": self._last_error,
                "last_health": health,
                "meta": meta,
                "aircraft": list(self._last_aircraft),
                "suppressed": dict(self._suppressed),
                "civilian_in_radius": bool(meta.get("civilian_in_radius")) and fresh,
                "filter_fail_open": not fresh,
                "positions_missing": bool(fresh and total > 0 and with_pos == 0),
                "in_circle": bool(fresh and in_r > 0),
                "feeders": feeders_pub,
                "fallback": dict(self._fallback_info),
                "zones": self.zones.public_dict() if self.zones else None,
            }

    def _is_fresh_unlocked(self) -> bool:
        if self._last_ok_at is None:
            return False
        return (time.time() - self._last_ok_at) <= self.stale_s

    def _feeder_fresh_unlocked(self, feeder_id: str) -> bool:
        c = self._feeder_cache.get(feeder_id)
        if not c or c.get("ok_at") is None:
            return False
        stale = float(c.get("stale_s") or self.stale_s)
        return (time.time() - float(c["ok_at"])) <= stale

    def is_fresh(self) -> bool:
        with self._lock:
            return self._is_fresh_unlocked()

    def civilian_in_radius(self) -> bool:
        """True only when ADS-B fresh and ≥1 aircraft in radius. Fail-open → False."""
        with self._lock:
            if not self._is_fresh_unlocked():
                return False
            return bool(self._last_meta.get("civilian_in_radius"))

    def _count_in_radius(
        self, positions: list[tuple[float, float]], lat: float, lon: float, radius_m: float
    ) -> tuple[int, float | None]:
        n = 0
        nearest: float | None = None
        for plat, plon in positions:
            d = haversine_m(lat, lon, plat, plon)
            if d <= radius_m:
                n += 1
            if nearest is None or d < nearest:
                nearest = d
        return n, nearest

    def civilian_for_zone(
        self,
        zone: dict[str, Any],
        *,
        lat: float | None = None,
        lon: float | None = None,
        timestamp_ms: int | None = None,
    ) -> dict[str, Any]:
        """Evaluate civilian_in_radius for a zone (+ optional GPS override)."""
        if self.zones is None:
            return {
                "civilian_in_radius": self.civilian_in_radius(),
                "center_source": "legacy",
                "zone_id": None,
                "fail_open": not self.is_fresh(),
            }
        fid = zone.get("feeder_id")
        if not fid:
            return {
                "civilian_in_radius": False,
                "center_source": "none",
                "zone_id": zone.get("id"),
                "fail_open": True,
                "filter_active": False,
            }
        clat, clon, src = self.zones.resolve_center(
            zone, lat=lat, lon=lon, timestamp_ms=timestamp_ms
        )
        with self._lock:
            fresh = self._feeder_fresh_unlocked(str(fid))
            cache = self._feeder_cache.get(str(fid)) or {}
            positions = list(cache.get("positions") or [])
        if not fresh or clat is None or clon is None:
            return {
                "civilian_in_radius": False,
                "center_source": src,
                "zone_id": zone.get("id"),
                "fail_open": True,
                "filter_active": True,
            }
        radius = float(zone.get("radius_m") or self.radius_m)
        in_r, nearest = self._count_in_radius(positions, clat, clon, radius)
        return {
            "civilian_in_radius": in_r > 0,
            "adsb_count_in_radius": in_r,
            "adsb_nearest_dist_m": nearest,
            "adsb_radius_m": radius,
            "center_lat": clat,
            "center_lon": clon,
            "center_source": src,
            "zone_id": zone.get("id"),
            "fail_open": False,
            "filter_active": True,
        }

    def should_suppress_forward(
        self,
        kind: str,
        *,
        node_id: str | None = None,
        lat: float | None = None,
        lon: float | None = None,
        timestamp_ms: int | None = None,
    ) -> bool:
        """#116 + zones: suppress Cloud enqueue when civilian and filter ON."""
        k = (kind or "").lower()
        if self.zones is not None:
            zone = self.zones.zone_for_node(node_id)
            if zone is None or not zone.get("feeder_id"):
                return False
            ev = self.civilian_for_zone(
                zone, lat=lat, lon=lon, timestamp_ms=timestamp_ms
            )
            if ev.get("fail_open") or not ev.get("civilian_in_radius"):
                return False
            flag = {
                "det": bool(zone.get("filter_det")),
                "hb": bool(zone.get("filter_hb")),
                "mel": bool(zone.get("filter_mel")),
            }.get(k, False)
            if flag:
                with self._lock:
                    self._suppressed[k] = self._suppressed.get(k, 0) + 1
            return bool(flag)
        with self._lock:
            if not self._is_fresh_unlocked():
                return False  # fail-open
            if not self._last_meta.get("civilian_in_radius"):
                return False
            flag = {
                "det": self.filter_det,
                "hb": self.filter_hb,
                "mel": self.filter_mel,
            }.get(k, False)
            if flag:
                self._suppressed[k] = self._suppressed.get(k, 0) + 1
            return bool(flag)

    def start(self) -> None:
        self.stop()
        if not self._want_running():
            return
        with self._lock:
            self._stop.clear()
            self._thread = threading.Thread(
                target=self._loop, daemon=True, name="hub-adsb"
            )
            self._thread.start()

    def _want_running(self) -> bool:
        with self._lock:
            if self._want_running_unlocked():
                return True
            lol = self.fallback_lol and lol_fallback_enabled()
            fn = self._extra_centers_fn
        # extra_centers_fn может брать STATE.lock — нельзя под ADSB._lock.
        if lol and fn is not None:
            try:
                return bool(fn())
            except Exception as e:  # noqa: BLE001
                print(f"[hub-adsb] extra centers: {e}", flush=True)
        return False

    def _want_running_unlocked(self) -> bool:
        if self.enabled:
            return True
        if self.zones is not None and self.zones.enabled_feeders_unique():
            return True
        if self.fallback_lol and lol_fallback_enabled() and self._fallback_centers_static_unlocked():
            return True
        return False

    def _fallback_center(self) -> tuple[float, float] | None:
        centers = self._collect_fallback_centers()
        return centers[0] if centers else None

    def _fallback_center_unlocked(self) -> tuple[float, float] | None:
        centers = self._fallback_centers_static_unlocked()
        return centers[0] if centers else None

    @staticmethod
    def _merge_centers(
        base: list[tuple[float, float]],
        extra: list[tuple[float, float]] | None,
        *,
        max_n: int = 3,
    ) -> list[tuple[float, float]]:
        out = list(base)
        seen = {(round(a, 2), round(b, 2)) for a, b in out}
        for lat, lon in extra or []:
            key = (round(float(lat), 2), round(float(lon), 2))
            if key in seen:
                continue
            seen.add(key)
            out.append((float(lat), float(lon)))
            if len(out) >= max_n:
                break
        return out

    def _fallback_centers_static_unlocked(self) -> list[tuple[float, float]]:
        """site + зоны (без extra_centers_fn — безопасно под ADSB._lock)."""
        out: list[tuple[float, float]] = []
        seen: set[tuple[float, float]] = set()

        def add(lat: Any, lon: Any) -> None:
            if not isinstance(lat, (int, float)) or not isinstance(lon, (int, float)):
                return
            key = (round(float(lat), 2), round(float(lon), 2))
            if key in seen:
                return
            seen.add(key)
            out.append((float(lat), float(lon)))

        add(self.site_lat, self.site_lon)
        if self.zones is not None:
            zones = getattr(self.zones, "zones", {}) or {}
            for z in zones.values():
                if isinstance(z, dict):
                    add(z.get("lat"), z.get("lon"))
        return out

    def _collect_fallback_centers(self) -> list[tuple[float, float]]:
        with self._lock:
            static = self._fallback_centers_static_unlocked()
            fn = self._extra_centers_fn
        extra: list[tuple[float, float]] | None = None
        if fn is not None:
            try:
                extra = list(fn() or [])
            except Exception as e:  # noqa: BLE001
                print(f"[hub-adsb] extra centers: {e}", flush=True)
        return self._merge_centers(static, extra)

    def stop(self) -> None:
        self._stop.set()
        t: threading.Thread | None
        with self._lock:
            t = self._thread
        if t is not None and t.is_alive() and t is not threading.current_thread():
            t.join(timeout=8.0)
        with self._lock:
            if self._thread is t:
                self._thread = None

    def _poll_targets(self) -> list[dict[str, Any]]:
        if self.zones is not None:
            feeders = self.zones.enabled_feeders_unique()
            if feeders:
                return feeders
        with self._lock:
            if not self.enabled:
                return []
            return [
                {
                    "id": "default",
                    "name": self.site_id,
                    "url": self.url,
                    "enabled": True,
                    "poll_interval_s": self.interval_s,
                    "stale_s": self.stale_s,
                }
            ]

    def _summary_from_poll(
        self, acs: list[dict[str, Any]], *, feeder_id: str, source: str
    ) -> list[dict[str, Any]]:
        summary = []
        for ac in acs[:48]:
            raw_ac = (
                ac.get("aircraft") if isinstance(ac.get("aircraft"), dict) else {}
            )
            summary.append(
                {
                    "hex": ac.get("hex"),
                    "flight": (
                        (raw_ac.get("flight") or ac.get("flight") or "").strip() or None
                    ),
                    "alt_baro": raw_ac.get("alt_baro", ac.get("alt_baro")),
                    "gs": raw_ac.get("gs", ac.get("gs")),
                    "track": raw_ac.get(
                        "track", raw_ac.get("true_heading", ac.get("track"))
                    ),
                    "lat": raw_ac.get("lat", ac.get("lat")),
                    "lon": raw_ac.get("lon", ac.get("lon")),
                    "dist_m": ac.get("dist_m"),
                    "in_radius": ac.get("in_radius"),
                    "feeder_id": feeder_id,
                    "source": source,
                }
            )
        summary.sort(
            key=lambda x: (
                0 if x.get("in_radius") else 1,
                0 if x.get("dist_m") is not None else 1,
                x.get("dist_m")
                if isinstance(x.get("dist_m"), (int, float))
                else 1e12,
            )
        )
        return summary

    def _positions_from_raw(self, raw: dict[str, Any]) -> list[tuple[float, float]]:
        positions: list[tuple[float, float]] = []
        for item in raw.get("aircraft") or []:
            if not isinstance(item, dict):
                continue
            plat, plon = item.get("lat"), item.get("lon")
            if isinstance(plat, (int, float)) and isinstance(plon, (int, float)):
                positions.append((float(plat), float(plon)))
        return positions

    def _loop(self) -> None:
        pub: MqttPublisher | None = None
        pub_fp: tuple[str, int, str, str] | None = None
        while not self._stop.is_set():
            targets = self._poll_targets()
            with self._lock:
                slat, slon = self.site_lat, self.site_lon
                radius = self.radius_m
                want_snap = self.snapshot
                default_interval = self.interval_s
                dist_nm = self.fallback_lol_dist_nm
                allow_lol = self.fallback_lol and lol_fallback_enabled()
            centers = self._collect_fallback_centers()
            center = centers[0] if centers else None
            if not targets and not (allow_lol and center):
                break
            if targets:
                interval = min(
                    float(t.get("poll_interval_s") or default_interval) for t in targets
                )
            else:
                interval = max(10.0, float(default_interval))
            # adsb.lol — не долбить чаще 10 с
            if allow_lol:
                interval = max(interval, 10.0)

            creds = self._mqtt_creds() or {}
            host = (creds.get("host") or "").strip()
            port = int(creds.get("port") or 1883)
            user = str(creds.get("username") or "")
            password = str(creds.get("password") or "")
            fp = (host, port, user, password) if host else None
            if pub is not None and fp != pub_fp:
                try:
                    pub.close()
                except Exception:  # noqa: BLE001
                    pass
                pub = None
                pub_fp = None
            if host and pub is None:
                try:
                    pub = MqttPublisher(host, port, user, password)
                    pub_fp = fp
                except Exception as e:  # noqa: BLE001
                    with self._lock:
                        self._last_error = f"mqtt:{e}"
                    pub = None
                    pub_fp = None

            any_ok = False
            local_blind = False
            local_error = False
            last_summary: list[dict[str, Any]] = []
            last_health: dict[str, Any] | None = None
            last_meta: dict[str, Any] | None = None
            last_err = ""
            need_fallback_fids: list[str] = []

            for feeder in targets:
                url = (feeder.get("url") or "").strip()
                fid = str(feeder.get("id") or "default")
                site = fid
                stale_s = float(feeder.get("stale_s") or STALE_S_DEFAULT)
                t0 = time.time()
                try:
                    raw = fetch_aircraft_json(url, timeout_s=5.0)
                    age = time.time() - t0
                    if is_blind_aircraft_json(raw):
                        local_blind = True
                        need_fallback_fids.append(fid)
                        with self._lock:
                            prev = self._feeder_cache.get(fid) or {}
                            self._feeder_cache[fid] = {
                                **prev,
                                "error": "blind_no_positions",
                                "stale_s": stale_s,
                                "url": url,
                                "source": "local",
                            }
                        continue
                    health, acs, meta = process_poll(
                        raw,
                        site_id=site,
                        source_url=url,
                        poll_age_s=age,
                        site_lat=slat,
                        site_lon=slon,
                        radius_m=radius,
                    )
                    positions = self._positions_from_raw(raw)
                    summary = self._summary_from_poll(acs, feeder_id=fid, source="local")
                    with self._lock:
                        self._feeder_cache[fid] = {
                            "ok_at": time.time(),
                            "stale_s": stale_s,
                            "positions": positions,
                            "error": "",
                            "url": url,
                            "source": "local",
                        }
                    any_ok = True
                    last_summary = summary
                    last_health = health
                    last_meta = meta
                    if pub is not None:
                        pub.publish(
                            topic_health(site),
                            health,
                            qos=QOS_HEALTH,
                            retain=RETAIN_HEALTH,
                        )
                        for ac in acs:
                            pub.publish(
                                topic_ac(site, ac["hex"]),
                                ac,
                                qos=QOS_AC,
                                retain=RETAIN_AC,
                            )
                        if want_snap and acs:
                            snap = {
                                "schema": "nevod.adsb.snapshot.v1",
                                "site_id": site,
                                "ts": health.get("ts"),
                                "count": len(acs),
                                "aircraft": acs,
                            }
                            pub.publish(
                                topic_snapshot(site),
                                snap,
                                qos=QOS_SNAPSHOT,
                                retain=RETAIN_SNAPSHOT,
                            )
                except Exception as e:  # noqa: BLE001
                    local_error = True
                    need_fallback_fids.append(fid)
                    age = time.time() - t0
                    health = build_health(
                        site_id=site,
                        ts=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                        total_aircraft=0,
                        aircraft_with_positions=0,
                        in_radius_count=0,
                        adsb_radius_m=radius,
                        source_url=url,
                        poll_age_s=age,
                        ok=False,
                        problem=type(e).__name__,
                    )
                    last_err = str(e)
                    with self._lock:
                        prev = self._feeder_cache.get(fid) or {}
                        self._feeder_cache[fid] = {
                            **prev,
                            "error": str(e),
                            "stale_s": stale_s,
                            "url": url,
                            "source": "local",
                        }
                    last_health = health
                    if pub is not None:
                        try:
                            pub.publish(
                                topic_health(site),
                                health,
                                qos=QOS_HEALTH,
                                retain=RETAIN_HEALTH,
                            )
                        except Exception:
                            pass

            # Reserve: adsb.lol if нет локального / ошибка / ослеп
            reason = ""
            if not targets:
                reason = "no_local_feeder"
            elif need_fallback_fids and not any_ok:
                if local_blind and not local_error:
                    reason = "local_blind"
                else:
                    reason = "local_error"
            elif need_fallback_fids and any_ok:
                reason = "local_partial_blind"
            use_lol = bool(allow_lol and center and reason)

            if use_lol and center is not None:
                clat, clon = center
                lol_url = build_lol_url(clat, clon, dist_nm)
                t0 = time.time()
                try:
                    raw = fetch_adsb_lol_multi(
                        centers or [center], dist_nm=dist_nm, timeout_s=8.0
                    )
                    lol_url = str(raw.get("url") or lol_url)
                    age = time.time() - t0
                    fids = need_fallback_fids or (["adsb_lol"] if not targets else [])
                    if not fids:
                        fids = ["adsb_lol"]
                    health, acs, meta = process_poll(
                        raw,
                        site_id=fids[0],
                        source_url=lol_url,
                        poll_age_s=age,
                        site_lat=slat if slat is not None else clat,
                        site_lon=slon if slon is not None else clon,
                        radius_m=radius,
                    )
                    positions = self._positions_from_raw(raw)
                    summary = self._summary_from_poll(
                        acs, feeder_id=fids[0], source="adsb.lol"
                    )
                    with self._lock:
                        for fid in fids:
                            self._feeder_cache[fid] = {
                                "ok_at": time.time(),
                                "stale_s": max(30.0, float(default_interval) * 3),
                                "positions": positions,
                                "error": "",
                                "url": lol_url,
                                "source": "adsb.lol",
                            }
                        self._fallback_info = {
                            "active": True,
                            "reason": reason,
                            "url": lol_url,
                            "error": "",
                            "aircraft": len(raw.get("aircraft") or []),
                            "centers": int(raw.get("centers") or len(centers or [center])),
                        }
                        if not any_ok:
                            self._last_ok_at = time.time()
                            self._last_error = ""
                            self._last_health = health
                            self._last_meta = meta
                            self._last_aircraft = summary
                        else:
                            # локальный жив — карту не затираем, только кэш слепых фидеров
                            self._last_ok_at = time.time()
                            self._last_error = ""
                            if last_health is not None:
                                self._last_health = last_health
                            if last_meta is not None:
                                self._last_meta = last_meta
                            self._last_aircraft = last_summary
                    any_ok = True
                    if pub is not None and not last_summary:
                        pub.publish(
                            topic_health(fids[0]),
                            health,
                            qos=QOS_HEALTH,
                            retain=RETAIN_HEALTH,
                        )
                        for ac in acs:
                            pub.publish(
                                topic_ac(fids[0], ac["hex"]),
                                ac,
                                qos=QOS_AC,
                                retain=RETAIN_AC,
                            )
                except Exception as e:  # noqa: BLE001
                    with self._lock:
                        self._fallback_info = {
                            "active": False,
                            "reason": reason,
                            "url": lol_url,
                            "error": str(e),
                            "aircraft": 0,
                        }
                        if not any_ok:
                            self._last_error = f"adsb_lol:{e}"
                        elif last_health is not None:
                            self._last_health = last_health
                            if last_meta is not None:
                                self._last_meta = last_meta
                            self._last_aircraft = last_summary
                            self._last_ok_at = time.time()
                            self._last_error = ""
            else:
                with self._lock:
                    if any_ok:
                        self._fallback_info = {
                            "active": False,
                            "reason": "",
                            "url": "",
                            "error": "",
                            "aircraft": 0,
                        }
                        self._last_ok_at = time.time()
                        self._last_error = ""
                        if last_health is not None:
                            self._last_health = last_health
                        if last_meta is not None:
                            self._last_meta = last_meta
                        self._last_aircraft = last_summary
                    else:
                        self._last_error = last_err or self._last_error
                        if last_health is not None:
                            self._last_health = last_health

            self._stop.wait(interval)

        if pub is not None:
            pub.close()
        with self._lock:
            self._thread = None
